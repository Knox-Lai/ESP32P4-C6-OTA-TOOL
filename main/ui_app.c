/**
 * @file  ui_app.c
 * @brief iPadOS 风格界面：桌面（widget + 图标网格 + 底部浮条）+ 四个应用页
 *
 * 视觉规则（照 iPadOS 的观感提炼，不追求逐像素复刻）：
 *   - 桌面是渐变壁纸，内容浮在上面：两张白色半透明 widget 卡 + 圆角图标 + 底部浮条
 *   - 应用页是浅灰底（#F2F2F7）+ 白色圆角卡，深色文字，和桌面的彩色形成层次
 *   - 一切圆角：卡片 18~24，图标 24，胶囊按钮全圆
 *   - 进入应用用"自下而上滑入 260 ms"，返回时反向滑出 —— 这是 iPadOS 最标志性的动效
 *
 * 四条硬约束（踩过就知道）：
 *   1. 文字走 i18n_t()，字体走 UI_F_SMALL/UI_F_BASE/UI_F_LARGE。
 *      英文用 LVGL 自带 Montserrat，中文用 tools/fontgen 生成的 simhei 子集；
 *      直接写字面量 + 固定字体会让另一种语言显示成方块。
 *   2. 所有 lv_* 调用必须在 LVGL 上下文中：build 时持 bsp_display_lock，
 *      定时器回调本身就在 LVGL 任务里（端口已持锁），**不要**在里面再加锁，否则自死锁。
 *   3. 图标用 iconfont 字形（ICO_*），不用 LV_SYMBOL_*：内置 Montserrat 里
 *      只有 HOME/DOWNLOAD/TINT/SHUFFLE 四个符号，其余都会画成方块。
 *   4. 在事件回调里**不能**直接重建界面（会删掉正在派发事件的对象）。
 *      切换语言走 lv_async_call(rebuild_async) 推到下一轮再重建。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_flash.h"      /* 本机 Flash 容量（信息页显示） */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"            /* 亮度偏好持久化（命名空间 "ui"，与语言同一个） */
#include "soc/soc.h"        /* SOC_DRAM_LOW/HIGH：信息页内存条的 768 KB 全长 */

#include "lvgl.h"

#include "app_config.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "dev_table.h"
#include "i18n.h"
#include "net_dhcps.h"
#include "net_eth.h"
#include "net_wifi.h"
#include "ota_client.h"
#include "ota_image.h"
#include "ota_proto.h"
#include "ui_app.h"

static const char *TAG = "ui";

/* 工具名/版本：桌面浮条和设置页关于都用这两个宏，改版本只改这里。
 * 名字本身是产品名不翻译；tagline 走 i18n（内容是"以太网 + WiFi"两条通道）。 */
#define APP_NAME     "Automatic Firmware Update Tool"
#define APP_VERSION  "v1.0.0"

/* ---------------- 配色（取自 iOS 系统色） ---------------- */
/* 壁纸：淡蓝紫渐变（原版是 0x5B8DEF → 0x9B6FE0，偏浓；这里整体提亮降饱和，
 * 走"淡"的路线）。底色偏浅，所以桌面上的文字/图标标签一律用深色 C_TXT，
 * 白字在浅底上直接糊掉。 */
#define C_WALL_TOP   0x9FC0F2      /* 壁纸渐变上：淡天蓝 */
#define C_WALL_BOT   0xC6B4EC      /* 壁纸渐变下：淡丁香紫 */
#define C_CARD       0xFFFFFF
#define C_PAGE_BG    0xF2F2F7      /* 应用页浅灰底 */
#define C_TXT        0x1C1C1E
#define C_SUB        0x8E8E93
#define C_LINE       0xE3E3E8
#define C_BLUE       0x0A84FF
#define C_GREEN      0x30D158
#define C_RED        0xFF453A
#define C_ORANGE     0xFF9F0A

#define CONTENT_Y    92            /* 应用页内容起始 y */

/* 信息页内存条的全长。取硬件 SRAM 窗（P4 是 0x4ff00000~0x4ffc0000 = 768 KB），
 * 而不是 heap_caps 统计到的堆总量（约 462 KB）：两者差的那 300 KB 是
 * 静态段 + 启动预留，它确实占着内存、也确实不能给应用用 —— 按 768 画全长
 * 才能把这一段单独涂成灰色显示出来，而不是让它凭空消失。 */
#define UI_RAM_HW_KB  ((unsigned)((SOC_DRAM_HIGH - SOC_DRAM_LOW) / 1024))
#define UI_MEM_BAR_W  436         /* 卡片内宽 484 - 左右各 24 */

/* ---------------- 状态 ---------------- */
typedef enum {
    APP_HOME = 0,
    APP_DEVICES,
    APP_DETAIL,
    APP_UPGRADE,
    APP_INFO,
    APP_SETTINGS,
    APP_LANG,          /* 设置 → 语言（子页，不是桌面 App） */
    APP_BRIGHT,        /* 设置 → 亮度（子页，滑块） */
    APP_WIFI,          /* 设置 → WiFi（子页，扫描 + 密码键盘） */
    APP_COUNT,
} app_id_t;

static app_id_t  s_cur = APP_HOME;
static lv_obj_t *s_page[APP_COUNT];

/* 页面栈：栈底固定是桌面，返回时弹一页而不是直接跳桌面。
 * 例：HOME -> Devices -> Device 详情 -> Upgrade，返回依次回退。 */
#define NAV_MAX  8
static app_id_t s_stack[NAV_MAX];
static int      s_depth = 0;

/* 顶部状态栏（全局，浮在所有页之上）
 *
 * 现在只剩右侧两个网络接口图标（WiFi + 网线）。
 * 左上角的设备数已去掉 —— 那个数字放在右上角太远、又只是"表里有几台"，
 * 挪到桌面"设备"卡片里做成主信息更合适；右上角的帧率同理去掉，
 * 它本来就是开发期看"到底是谁在乱重绘"的诊断量，不该占用户界面的位置。 */
static lv_obj_t *s_sb = NULL;

/* 桌面上那张"以太网"卡里的 IP 是否已经写过（重建后需重置） */
static lv_obj_t *s_home_ip   = NULL;
static lv_obj_t *s_home_link = NULL;
static lv_obj_t *s_home_lease = NULL;
static lv_obj_t *s_home_heap = NULL;

/* 桌面右侧卡片：在线设备台数 + 内存条 + TF 卡
 *
 * 这里原来是 CPU 折线（60 点，每秒推一个点）。折线撤掉的原因有两个：
 *   1) 每秒钟必然产生一次整块卡片的重绘，是"主页一直闪"的直接来源；
 *   2) 对一个"刷固件"的工具来说，用户在主页想看的是"有几台设备能升"，
 *      CPU 占用率属于开发期数据，要看可以进信息页（那里仍有百分比）。
 */
static lv_obj_t *s_home_online      = NULL;   /* 在线台数（大号数字） */
static lv_obj_t *s_home_dev_off     = NULL;   /* 右上角"N 台离线"，0 台时隐藏 */
static int       s_home_online_shown = -1;    /* 上次写入的在线台数 */
static int       s_home_devoff_shown = -1;    /* 上次写入的离线台数 */
static lv_obj_t *s_home_mem_pct = NULL;
static lv_obj_t *s_home_mem_bar = NULL;
static lv_obj_t *s_home_tf      = NULL;

/* 设备列表 */
static lv_obj_t *s_dev_list  = NULL;
static lv_obj_t *s_dev_count = NULL;
static uint32_t  s_dev_sig   = 0;
static bool      s_dev_have_sig  = false;
static bool      s_dev_last_link = false;   /* 链路断/通时空态文案不同，需要重建 */

/* 设备详情 */
static int       s_detail_idx = -1;
static lv_obj_t *s_dt_model = NULL;
static lv_obj_t *s_dt_ip    = NULL;
static lv_obj_t *s_dt_fw    = NULL;
static lv_obj_t *s_dt_hw    = NULL;
static lv_obj_t *s_dt_flash = NULL;
static lv_obj_t *s_dt_caps  = NULL;
static lv_obj_t *s_dt_uid   = NULL;
static lv_obj_t *s_dt_host  = NULL;
static lv_obj_t *s_dt_mac   = NULL;

/* 升级页 */
static int       s_upg_sel = -1;
static lv_obj_t *s_upg_hdr   = NULL;   /* 左卡片标题 "SELECT DEVICE"，无设备时隐藏 */
static lv_obj_t *s_upg_list  = NULL;
static lv_obj_t *s_upg_arc   = NULL;
static lv_obj_t *s_upg_pct   = NULL;
static lv_obj_t *s_upg_state = NULL;
static lv_obj_t *s_upg_sub   = NULL;
static lv_obj_t *s_upg_btn   = NULL;
static uint32_t  s_upg_sig   = 0;
static bool      s_upg_have_sig = false;

/* 信息页 */
static lv_obj_t *s_in_ip, *s_in_mask, *s_in_gw, *s_in_link, *s_in_lease;
static lv_obj_t *s_in_mem_pct, *s_in_mem_sub, *s_in_psram;
static lv_obj_t *s_in_mem_res, *s_in_mem_used;   /* 768 KB 条里的"系统预留"/"堆已用"两段 */
static lv_obj_t *s_in_flash, *s_in_min, *s_in_up;

/* ---- 背光亮度 ----
 * 0~100 (%)，写在 NVS（命名空间 "ui"，key "bright"，和语言同一个命名空间）。
 * 滑块下限不是 0 而是 BRIGHT_MIN：拖到 0 就是全黑，用户既看不见界面也
 * 找不到滑块，只能串口救回来 —— 留一点底线光更安全。 */
#define BRIGHT_MIN   5
#define BRIGHT_NS    "ui"
#define BRIGHT_KEY   "bright"

static int       s_bright     = 100;    /* 当前亮度，掉电从上一次的值恢复 */
static lv_obj_t *s_br_slider  = NULL;   /* 亮度子页的滑块（手势判命中带要用） */
static lv_obj_t *s_br_pct     = NULL;   /* 亮度子页的大号百分比 */
static lv_obj_t *s_set_br_val = NULL;   /* 设置页"亮度"行右侧的数值 */

/* ---- 设置 → WiFi ----
 * 列表行只带索引，详情从这份快照里取：把 SSID 字符串挂在 user_data 上
 * 会牵扯到何时 free（行被 lv_obj_clean 删掉时没有回调），索引最省事，
 * 前提是快照和行永远同一次刷新生成 —— 所以两者都在 wifi_fill_list() 里一起动。 */
static lv_obj_t *s_w_sw         = NULL;  /* 总开关 */
/* 用户刚拨过开关的时刻（ms）。开关是异步生效的：拨到"开"之后要等一次性任务
 * 跑完 net_wifi_set_enabled()，s_enabled 才真的变 true。这中间的几百毫秒里，
 * 下面的"反向同步"会把开关又拨回去 —— 表现为"手指一松开关自己弹回原位"。
 * 所以用户操作后 1.5 s 内不做反向同步。 */
static int64_t   s_sw_touch_ms  = 0;
static lv_obj_t *s_w_state      = NULL;  /* 状态行（已连接 SSID / IP / 信号） */
static lv_obj_t *s_w_scan_lbl   = NULL;  /* "扫描"按钮上的字，扫描中改成"扫描中…" */
static lv_obj_t *s_w_disc       = NULL;  /* "断开"按钮：已连/连接中/失败时显示 */
static lv_obj_t *s_w_list       = NULL;  /* 网络列表容器 */
static lv_obj_t *s_set_wifi_val = NULL;  /* 设置页"WiFi"行右侧的状态 */

static net_wifi_ap_t s_w_aps[NET_WIFI_SCAN_MAX];
static int       s_w_apn        = 0;
static int       s_w_drawn_apn  = -1;    /* 列表是按多少条画的 */
static int       s_w_drawn_st   = -1;    /* 画列表时的 WiFi 状态 */
static bool      s_w_drawn_scan = false; /* 画列表时是否在扫描 */
static int       s_w_drawn_sv   = -1;    /* 画列表时已知网络有几条 */

/* 长按已知网络弹"忘记"框之后，松开时 LVGL 仍会给行发一次 CLICKED
 * （长按不清 last_obj），那会顺手触发连接。这个标志把紧随其后的
 * 那一次点击吞掉 —— 定义放在这里是因为 wifi_row_cb 比确认框先出现。 */
static bool      s_row_long     = false;

/* 密码浮层（模态：遮罩 + 卡片 + 软键盘），NULL = 没打开。
 * 浮层挂在 lv_layer_top() 上，不是挂在页面上 —— 页面有入场动画、隐藏标志和
 * 裁剪，浮层跟着页面走会出现"卡片在、键盘不在"这类 z 序问题。 */
static lv_obj_t *s_pw_modal = NULL;
static lv_obj_t *s_pw_ta    = NULL;
static lv_obj_t *s_pw_kb    = NULL;
static char      s_pw_ssid[NET_WIFI_SSID_MAX];

/* ------------------------------------------------------------------
 * 刷新判重缓存。
 * 原来是 refresh_cb 里的函数内 static，切换语言重建界面后这些值还留着，
 * 会出现"重建完文字不刷新"的假象，因此统一提到文件作用域，由
 * ui_app_rebuild() 一并复位。
 * ------------------------------------------------------------------ */
static int      s_heap_shown     = -1;          /* 桌面空闲内存（8 KB 死区） */
static int      s_in_mem_shown   = -1;          /* 信息页内部 RAM 占用率 % */
static int      s_flash_shown    = -1;          /* 信息页 Flash 容量（-1 未读，只读一次） */
static int      s_home_mem_shown = -1;          /* 桌面内存条百分比 */
static int      s_tf_shown       = -2;          /* 桌面 TF 行：-2 未画，-1 无卡，1 有卡 */
static int      s_arc_shown      = -1;          /* 升级页进度环 */
static int      s_busy_shown     = -1;          /* 升级按钮显隐 */
static int      s_lan_key        = -1;          /* 网线图标 (up|dark) */
static int      s_lan_drawn      = -1;          /* 图标实际画出的状态（-1 未画） */
static uint32_t s_lan_fg_drawn   = 0;           /* 图标实际画出的颜色 */
static uint32_t s_upg_col_shown  = 0xFFFFFFFFu; /* 升级页状态文字色 */

/* ---------------- 基础控件 ---------------- */

/* LVGL 9 的基类构造函数给**每个**对象默认加了 LV_OBJ_FLAG_CLICKABLE
 * （core/lv_obj.c: obj->flags = LV_OBJ_FLAG_CLICKABLE）。
 * 结果就是：图标方块、卡片里的装饰容器、甚至文字标签都会"吞掉"点击——
 * lv_indev_search_obj 命中到最里层的可点对象就返回，事件不会冒泡到父级，
 * 于是父容器上挂的回调永远收不到，表现为"点了没反应"。
 *
 * 所以约定：装饰性对象（容器 / 标签 / 折线 / 进度条）一律去掉 CLICKABLE，
 * 只有真正要响应点击的对象（图标、列表行、按钮、返回胶囊）显式加回来。 */
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* 标签一律不可点（否则压在按钮/行上的文字会把点击吃掉） */
static lv_obj_t *mk_label(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, int r)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, r, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), LV_PART_MAIN);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f,
                      uint32_t color, int x, int y)
{
    lv_obj_t *l = mk_label(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 圆角胶囊底（标签放进去自己居中） */
static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, lv_opa_t opa)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
    return o;
}

/* ---------------- 只在内容真的变了才写 label ---------------- */
/*
 * 为什么必须判重：lv_label_set_text* 每次调用都会 free 旧文本缓冲、重新分配、
 * 重新排版并 invalidate 所在区域 —— **内容一样也照样重绘**。
 * refresh_cb 每 400 ms 把整页字段无条件刷一遍，桌面两张卡片又是半透明的
 * 半透明（重绘要连带下面的渐变壁纸一起重新合成），
 * 在 1024x600 + PSRAM 单 framebuffer 上肉眼看到的就是"主页一直在闪"。
 * 加了判重之后稳态下一个字都不写，屏幕完全静止，只有数值真变时才重绘。
 */
static void set_txt(lv_obj_t *l, const char *s)
{
    if (l == NULL || s == NULL) {
        return;
    }
    const char *old = lv_label_get_text(l);
    if (old != NULL && strcmp(old, s) == 0) {
        return;
    }
    lv_label_set_text(l, s);
}

static void set_txt_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_txt(l, buf);
}

/* ---------------- 图标（iconfont 字形） ---------------- */

/* 之前三个 App 图标是矩形 + 折线手绘（因为 LVGL 自带字体没有这些符号）。
 * 现在 font/ 里有一套 iconfont，转成了 ui_icon_40 / ui_icon_24 两个字号的子集，
 * 直接用字形：笔画统一、缩放一致、加图标不用再写绘图代码。
 * 码位宏（ICO_*）在 i18n.h，改图标只改那里的转义串。
 * 这里不居中：调用方一般要用 align / set_pos 定位，居中会让后续 align 失效。 */
static lv_obj_t *icon_glyph(lv_obj_t *parent, const char *utf8, uint32_t color,
                            const lv_font_t *f)
{
    lv_obj_t *l = mk_label(parent);
    lv_label_set_text(l, utf8);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}

/* ---------------- 状态栏 ---------------- */

/* 状态栏右侧的网络接口图标（e631）。
 * 断线时不再换字形，而是**在同一个图标上划一道斜杠**：
 * 换成 iconfont 自带的 disconnect 字形时，断线/连线切换会让字形宽度变化，
 * 图标位置跟着横跳一下；叠斜杠则底图始终是 e631，位置完全不动。 */
static lv_obj_t *s_lan   = NULL;
static lv_obj_t *s_slash = NULL;    /* 断开时压上去的斜杠（旋转 45° 的细条） */

/* 斜杠本体：3 x 26 的圆角竖条，绕自身中心顺时针转 45° 就是 "/"。
 * 转 45° 后对角跨度 ≈ (26 + 3) × 0.707 ≈ 20.5 px，比 18 px 的图标 ink
 * 长出一点，两端才都露出纸面，看起来是"划掉"而不是"图标上贴了个点"。
 * 26 而不是 24：24 时左下那端刚好卡在 ink 边缘上，视觉上像被切掉一截。
 *
 * pivot 必须显式设成对象中心：LVGL 的 transform 默认绕**左上角**转，
 * 3×26 的条绕 (0,0) 转 45° 后几何中心会跑到 (-8.1, 10.3)，
 * 也就是左偏 9 px、上移 2.5 px —— 这就是"斜杠没经过图标正中间"的原因，
 * 跟布局和字体都无关。 */
static lv_obj_t *lan_slash(lv_obj_t *parent)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, 3, 26);
    lv_obj_set_style_radius(o, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_SUB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(o, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(o, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(o, 450, LV_PART_MAIN);   /* 单位 0.1° */
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void lan_icon_create(lv_obj_t *parent)
{
    s_lan = icon_glyph(parent, ICO_LINK, C_TXT, &ui_icon_24);
    lv_obj_align(s_lan, LV_ALIGN_TOP_RIGHT, -18, 2);

    /* 斜杠压在图标中心：底图 24 px 字形，ink 约 18 px，
     * 斜杠转 45° 后跨度约 20.5 px，横穿整个图标。
     * 中心故意下移 1 px：条长 26、两端已经对称多出 1 px，再整体下移 1 px
     * 就变成"右上端维持原位、左下端多伸 2 px"，斜杠左下那截才划得出去。 */
    s_slash = lan_slash(parent);
    lv_obj_align_to(s_slash, s_lan, LV_ALIGN_CENTER, 0, 1);
}

static void lan_icon_update(bool up, uint32_t fg)
{
    /* 状态没变就不动：一次多余的 set_style 就是一次无效区的重绘 */
    if ((int)up == s_lan_drawn && fg == s_lan_fg_drawn) {
        return;
    }
    s_lan_drawn    = (int)up;
    s_lan_fg_drawn = fg;

    /* 底图永远是 e631：连线用正文色，断线转灰（斜杠同色，只是把图标划掉） */
    lv_obj_set_style_text_color(s_lan, lv_color_hex(up ? fg : C_SUB), LV_PART_MAIN);

    if (up) {
        lv_obj_add_flag(s_slash, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_slash, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 状态栏右上角的 WiFi 图标，排在网线图标左侧。
 *
 * 语义和网线图标一致（没连上就被划掉，而不是整个消失）：
 *   已连接（拿到 IP） → 按信号档位点亮弧线，无斜杠；
 *   关闭 / 未连 / 连接中 / 失败 / 硬件不可用 → 全灰 + 斜杠。
 *
 * 信号强度用**自绘的同心弧**表达：档位 1 只有底部圆点，2/3/4 依次多点亮
 * 一段弧（内 → 外），一共 4 个可亮单元，正好对应 4 档。
 *
 * 画法演变（前三版都否决过，记下来免得再绕回去）：
 *   ① 图标左侧加四段竖条 —— 否（"这样不好"）；
 *   ② lv_arc 控件叠三层 —— 否（"太丑了"）：控件自带背景/指示器/knob，
 *      每层都要单独关，三层半径还得互相对齐，参数一多就调不动；
 *   ③ 换成设计稿导出的四档字形（e6a1-e6a4）—— 效果可以，但每加一档就要
 *      重新生成字库，且强弱只能离散到 4 挡。
 *   现在：直接在 layer 上画 lv_draw_arc。只有"圆心 / 半径 / 线宽"三个量，
 *   圆头和抗锯齿是现成的，改档位只需改一个整数。 */

static lv_obj_t *s_wifi       = NULL;
static lv_obj_t *s_wifi_slash = NULL;
static int       s_wifi_lvl   = -1;   /* 档位：0 = 未连接，1..4 = 亮几个单元，-1 = 没画过 */

/* 尺寸与网线图标同占 24×24；圆心在底部中点，三段弧同心、最外一段撑满宽度。 */
#define WIFI_ICO_W      24
#define WIFI_ICO_H      24
#define WIFI_ICO_CX     12     /* 圆心 x（对象内坐标） */
#define WIFI_ICO_CY     19     /* 圆心 y：圆点在底部，弧往上长 */
#define WIFI_ICO_LW     2      /* 线宽：设计稿 1024 网格里是 70.3，换算到 24 px = 1.65，
                                * 只能用整数，取 2 —— 1 px 在屏上会被抗锯齿吃掉。 */
/* 圆点相对圆心（三段弧的同心点）的纵向偏移。
 * 设计稿里圆点是和弧同心的，但 24 px 等比缩放后，圆点底边离图标下沿只剩
 * 4 px、离内弧内缘（cy - 5）却有 3 px，看起来"点掉在下面、和弧脱开"。
 * 上移 1 px 让间隙变 2 px，四档整体的视觉重心更紧凑。
 * 不要再往上（2 px 起）：圆点 3 px 高、弧 2 px 宽，间隙缩到 1 px 后抗锯齿
 * 会把两者糊成一根，档位 1（只有圆点）就和档位 2（圆点 + 内弧）分不开了。 */
#define WIFI_ICO_DOT_DY (-1)

/* 三段弧半径（lv_draw_arc 的 radius 是**外**半径，线往里长）。
 * 设计稿 a-wifixinhaoqiangdu*.svg 以 (512, 815.9) 为心，三段内缘半径
 * 267.8 / 468.4 / 609.2，按 24/1024 缩放并加一个线宽，得到 7 / 11 / 15，
 * 内缘正好 5 / 9 / 13，弧间距均匀。不要随手改成等距的小半径：
 * 半径越小，弧顶越平，四档的层次就越分不出来。 */
static const uint16_t s_wifi_arc_r[3] = { 7, 11, 15 };

/* 自绘回调：三段同心弧 + 底部圆点。
 *
 * 不用 lv_draw_line 折线：24 px 的图标上，三个点连成的"弧"在转折处会露出
 * 明显棱角，半径越小越像尖顶房子；把采样点加密又变成几十次绘制调用。
 * lv_draw_arc 一次调用就是一段真弧，边缘自带抗锯齿。 */
static void wifi_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   c;
    lv_obj_get_coords(lv_event_get_target(e), &c);

    const int32_t cx = c.x1 + WIFI_ICO_CX;
    const int32_t cy = c.y1 + WIFI_ICO_CY;

    const bool linked = (s_wifi_lvl > 0);
    /* 亮段用正文色、暗段浅灰；未连接时整体转灰，由斜杠表达"断开"。
     * 已连接却画成灰的会被当成没连，所以这两者不能共用一套颜色。 */
    const uint32_t on  = linked ? C_TXT : C_SUB;
    const uint32_t off = linked ? 0xC7C7CE : C_SUB;

    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.width   = WIFI_ICO_LW;
    ad.center  = (lv_point_t){ .x = cx, .y = cy };
    /* 端头用平切（rounded = 0）：设计稿的弧端就是沿径向 45° 切平的，
     * 圆头会在端点鼓出一个和线宽同直径的球，24 px 上像"糖葫芦"。 */
    ad.rounded = 0;
    /* 顶部 90° 弧：0° 在 3 点钟、90° 在 6 点钟（顺时针为正，
     * 见 lv_draw_arc.h 里 start_angle 的注释），所以 12 点钟是 270°，
     * 左右各张 45° 就是 225° → 315°。
     *
     * 千万别写成 -135 / -45：lv_draw_sw_mask_angle_init() 会把负角度
     * 直接钳成 0（`if(start_angle < 0) start_angle = 0;`），两个都变 0 之后
     * start == end，角度掩码退化成全放行，画出来是一整圈同心圆环。 */
    ad.start_angle = 225;
    ad.end_angle   = 315;

    for (int i = 0; i < 3; i++) {          /* i=0 内弧，i=2 外弧 */
        ad.radius = s_wifi_arc_r[i];
        /* 档位 1 只亮圆点；2/3/4 依次多点一段弧 */
        ad.color = lv_color_hex(s_wifi_lvl >= i + 2 ? on : off);
        lv_draw_arc(layer, &ad);
    }

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(linked ? on : off);
    rd.bg_opa   = LV_OPA_COVER;
    rd.radius   = LV_RADIUS_CIRCLE;
    /* 圆点 3×3：设计稿半径 70.3/1024 → 1.65 px，直径 3.3，取 3 最接近
     * （原来 4×4 的实心方块面积是它的两倍，看着像颗痣） */
    const int32_t dy = cy + WIFI_ICO_DOT_DY;
    const lv_area_t dot = { cx - 1, dy - 1, cx + 1, dy + 1 };
    lv_draw_rect(layer, &rd, &dot);
}

/* 信号档位定义在 WiFi 列表那段（同一套阈值两处共用），
 * 状态栏的档数必须和列表里的格数一致，否则同一个信号两块地方说不同的话。 */
static int wifi_sig_level(int8_t rssi);

static void wifi_icon_create(lv_obj_t *parent)
{
    /* 自绘对象：本体只当画布用，背景/边框/内边距全部清零，尺寸就是纯坐标系。
     * 不用 lv_obj_remove_style_all()：那样对象连"主部件样式"都没有了，
     * invalidate 时会不会被当成完全透明、跳过 DRAW_MAIN 取决于 LVGL 版本；
     * 显式设成透明背景更稳，代价只有三条样式设置。 */
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_size(o, WIFI_ICO_W, WIFI_ICO_H);
    lv_obj_align(o, LV_ALIGN_TOP_RIGHT, -46, 2);
    lv_obj_add_event_cb(o, wifi_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_wifi = o;

    /* 斜杠：开机那几秒本来就没连上，先按"未连接"画，不会闪出一个已连接的样子。
     * 尺寸/圆角/颜色/角度全部沿用 lan_slash() 的默认值，一个都不覆盖：
     * 网口和 WiFi 两个图标的斜杠必须长得完全一样，否则并排放在状态栏里，
     * 一眼就能看出"两根斜杠不是一套"。 */
    s_wifi_slash = lan_slash(parent);
    lv_obj_align_to(s_wifi_slash, s_wifi, LV_ALIGN_CENTER, 0, 1);   /* 与网口同一偏移 */
    /* lan_slash() 建出来默认隐藏（给网线用），这里一开始就要划掉 */
    lv_obj_remove_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);

    s_wifi_lvl = 0;
}

static void wifi_icon_update(void)
{
    if (s_wifi == NULL) {
        return;
    }

    bool   linked = (net_wifi_state() == NET_WIFI_CONNECTED);
    int8_t rssi   = net_wifi_rssi();
    int    lvl;

    if (!linked) {
        lvl = 0;
    } else if (rssi > -120) {
        lvl = wifi_sig_level(rssi);
    } else {
        /* 刚拿到 IP 的那几秒还没有 RSSI（5 s 才问一次）：保持上一次的档位，
         * 一次都没取到就按满格 —— 已连接却只画一个点，看着像"信号为零"。 */
        lvl = (s_wifi_lvl > 0) ? s_wifi_lvl : 4;
    }

    if (lvl == s_wifi_lvl) {
        return;                     /* 档位没变就不碰对象，避免每 400 ms 触发重绘 */
    }
    s_wifi_lvl = lvl;

    /* 颜色由回调按 s_wifi_lvl 现算，这里只让这一小块区域重画。
     * invalidate 之后 LVGL 会调一次 DRAW_MAIN，等于把整只图标重画一遍。 */
    lv_obj_invalidate(s_wifi);

    if (linked) {
        lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 状态栏：左边设备数，右边帧率 + WiFi + 网线图标。
 * 它是屏幕直属子对象、最后创建，所以永远压在页面之上；
 * 透明且不可点，不会挡住下面的图标和列表。
 * 时间/日期先不做：本机没有 RTC 也没有 NTP，显示一个需要手动 time set
 * 才准的时钟意义不大，先留空位，等有对时来源再加回来。 */
static void build_status_bar(lv_obj_t *scr)
{
    s_sb = plain(scr);
    lv_obj_set_pos(s_sb, 0, 0);
    lv_obj_set_size(s_sb, BSP_LCD_H_RES, 30);

    /* 左右两侧都不放文字了：右边依次是 WiFi 和网线两个 24 px 图标
     * （-18 网线、-46 WiFi，各占 24 px），左边空着。 */
    lan_icon_create(s_sb);
    wifi_icon_create(s_sb);
    lv_obj_move_to_index(s_sb, -1);
}

/* ---- 刷新统计（诊断"到底是谁在闪"） ----
 * 每 10 s 往串口打一行：这段时间 flush 了多少次、最近一次刷的是哪块区域。
 * 判据很直接：
 *   静止时一行应该是 "0 次"        → LVGL 没在画，闪就不是软件重绘引起的；
 *   静止时持续非 0，区域又是整屏   → 有对象在周期性 invalidate，看区域就能定位到是谁；
 *   区域只有一小块（状态栏那块）    → 只是数字更新，配合 VSYNC 对齐后不该看得见。
 *
 * 这个统计仍然留着（只写串口、不上屏）：CPU 折线撤掉之后，静止的主页
 * 刷新次数应该接近 0，一旦又开始周期性刷新，日志能第一时间指出来。 */
static uint32_t s_flush_cnt = 0;
static int32_t  s_flush_x1, s_flush_y1, s_flush_x2, s_flush_y2;

static void disp_flush_stat_cb(lv_event_t *e)
{
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);

    s_flush_cnt++;
    if (a != NULL) {
        s_flush_x1 = a->x1;
        s_flush_y1 = a->y1;
        s_flush_x2 = a->x2;
        s_flush_y2 = a->y2;
    }
}

/* ---------------- 导航 ---------------- */

static void anim_set_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_hide_done(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

/* 把状态栏重新压到最上层（页面前后移动会改变 z 序） */
static void sb_to_front(void)
{
    if (s_sb != NULL) {
        lv_obj_move_to_index(s_sb, -1);
    }
}

/* 切页时立刻重刷状态栏配色（桌面深底白字 / 应用页浅底深字）。
 * 不走 refresh_cb：那个会重建设备列表，在点击回调里删对象不安全。 */
static void sb_restyle(void)
{
    if (s_sb == NULL) {
        return;
    }

    /* 状态栏只剩两个图标：换页时按当前链路状态把网线图标重刷一次即可
     * （WiFi 图标不走这里，它自己有状态判重）。 */
    lan_icon_update(net_eth_link_up(), C_TXT);
}

static void page_show(app_id_t id, bool animate)
{
    if (s_page[id] == NULL) {
        return;
    }

    /* 入栈：只有和栈顶不同才压入，避免重复打开同一页把栈撑爆 */
    if (s_depth + 1 < NAV_MAX && (s_depth == 0 || s_stack[s_depth] != id)) {
        s_stack[++s_depth] = id;
    }

    /* 先把其它页藏掉，避免两页叠在一起闪一帧 */
    for (int i = 0; i < APP_COUNT; i++) {
        if (i != (int)id && s_page[i] != NULL) {
            lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_remove_flag(s_page[id], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_page[id], 0);
    lv_obj_move_to_index(s_page[id], -1);

    if (animate) {
        lv_obj_set_y(s_page[id], BSP_LCD_V_RES);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_page[id]);
        lv_anim_set_values(&a, BSP_LCD_V_RES, 0);
        lv_anim_set_duration(&a, 260);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, anim_set_y);
        lv_anim_start(&a);
    }

    s_cur = id;
    sb_to_front();
    sb_restyle();           /* 状态栏配色随页面立刻切换（桌面白字 / 应用页深字） */
}

static void page_back(void)
{
    if (s_cur == APP_HOME || s_depth <= 0) {
        return;
    }

    app_id_t prev = s_stack[s_depth - 1];
    s_depth--;

    lv_obj_t *cur = s_page[s_cur];

    /* 目标页先显示出来，再把当前页从它上面滑下去 */
    lv_obj_remove_flag(s_page[prev], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_page[prev], 0);
    lv_obj_move_to_index(cur, -1);      /* 当前页必须在最上层，才有"盖着滑走"的效果 */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cur);
    lv_anim_set_values(&a, lv_obj_get_y(cur), BSP_LCD_V_RES);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_set_y);
    lv_anim_set_completed_cb(&a, anim_hide_done);   /* 动画结束才把当前页藏起来 */
    lv_anim_start(&a);

    s_cur = prev;
    sb_to_front();
    sb_restyle();
}

/* ---------------- 滑动返回 ---------------- */

/* 水平滑动超过这个像素就返回上一页（左右方向都算，方便单手） */
#define SWIPE_BACK_PX   110

/* 只有从屏幕左侧 1/3 起手才算返回手势。
 * 整屏都能滑的话，在列表/卡片上横向拖一下就误触发返回；
 * 限定左边缘后，中间和右侧区域随便滑都不会退出当前页。 */
#define SWIPE_EDGE_W    (BSP_LCD_H_RES / 3)

static bool       s_sw_tracking = false;
static bool       s_sw_fired    = false;
static bool       s_sw_edge     = false;    /* 本次按下是否落在左 1/3 */
static lv_point_t s_sw_start;

/* 亮度页：按下点落在滑块（含余量）的命中带里时，本次按下不判返回手势。
 *
 * 为什么用几何判定而不是 lv_indev_get_active_obj()：
 * 滑块对象只有 28 px 高，LVGL 内建的扩展点击区也只有 LV_DPX(8)。
 * 手指压在轨道上方/下方几像素时，视觉上就是在"抓滑块"，但 active_obj
 * 可能还是页面或卡片；30 ms 轮询正好读到那一刻，就误判成横滑返回。
 * 直接按矩形加余量判断：按下瞬间就能定，不依赖 LVGL 的输入状态机。 */
#define BR_HIT_PAD_X   24
#define BR_HIT_PAD_Y   26

static bool point_on_bright_slider(lv_point_t p)
{
    if (s_cur != APP_BRIGHT || s_br_slider == NULL || !lv_obj_is_valid(s_br_slider)) {
        return false;
    }

    lv_area_t a;
    lv_obj_get_coords(s_br_slider, &a);   /* 绝对坐标，页面全屏无偏移 */
    return (p.x >= a.x1 - BR_HIT_PAD_X) && (p.x <= a.x2 + BR_HIT_PAD_X) &&
           (p.y >= a.y1 - BR_HIT_PAD_Y) && (p.y <= a.y2 + BR_HIT_PAD_Y);
}

/* 30 ms 轮询一次指针：LVGL 的手势事件默认没开（LV_USE_GESTURE_RECOGNITION=0），
 * 自己看按下起点的位移，判断是不是一次横滑，两个方向都支持。 */

/* 密码浮层是否打开（实现在 WiFi 子页那一节）。打开时整页禁手势：
 * 软键盘上换行/切符号很容易带上横向位移，那时返回上一层是最坏的结果。 */
static bool wifi_modal_open(void);

static void swipe_poll_cb(lv_timer_t *t)
{
    (void)t;

    /* lv_indev_active() 只在 LVGL 内部处理输入时有效，定时器里必须自己找 */
    static lv_indev_t *indev = NULL;
    if (indev == NULL) {
        for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
            if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
                indev = i;
                break;
            }
        }
        if (indev == NULL) {
            return;
        }
    }

    if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
        s_sw_tracking = false;
        s_sw_fired    = false;
        return;
    }

    /* 当前页不接受手势返回：本次按下直接作废（fired 置位，松手才复位）。
     * 亮度页不整页禁，只有起手点在滑块命中带里时才不算返回（见下）。 */
    if (s_cur == APP_HOME) {
        s_sw_tracking = false;
        s_sw_fired    = true;
        return;
    }

    /* 密码浮层开着时一律不返回（键盘上的横滑太多了） */
    if (wifi_modal_open()) {
        s_sw_tracking = false;
        s_sw_fired    = true;
        return;
    }

    lv_point_t p = {0};
    lv_indev_get_point(indev, &p);

    if (!s_sw_tracking) {
        s_sw_tracking = true;
        s_sw_start    = p;
        /* 起手位置同时决定两件事：在不在左 1/3，以及是不是在拖亮度滑块。
         * 在滑块带上就彻底不判返回 —— 手指从轨道上横拖是在调亮度。 */
        s_sw_edge     = (p.x < SWIPE_EDGE_W) && !point_on_bright_slider(p);
        return;
    }

    int dx = (int)p.x - (int)s_sw_start.x;
    int dy = (int)p.y - (int)s_sw_start.y;
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;

    /* 左 1/3 起手 + 横滑足够远 + 明显比竖向大 → 认为是一次滑动返回
     * （起手点是否算数已在上面定好：APP_HOME 不判，亮度滑块带上不判） */
    if (!s_sw_fired && s_sw_edge && adx >= SWIPE_BACK_PX && adx > ady * 2) {
        s_sw_fired = true;
        ESP_LOGI(TAG, "滑动返回：dx=%d dy=%d 起手 x=%d", dx, dy, (int)s_sw_start.x);
        page_back();
    }
}

/* ---------------- 桌面 ---------------- */

static void home_app_cb(lv_event_t *e)
{
    app_id_t id = (app_id_t)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "点击图标：app=%d", (int)id);
    page_show(id, true);
}

/* App 图标块：淡色渐变底 + 同色系深图标。
 * 早先是高饱和渐变配白色图标，和淡蓝紫壁纸放一起显得"重"；
 * 改成浅底深字后，四个块在壁纸上是一层薄薄的色片，观感更接近 iOS 浅色。 */
static lv_obj_t *app_tile(lv_obj_t *parent, int x, int y, i18n_str_t label,
                          uint32_t c1, uint32_t c2, uint32_t c_ico,
                          const char *glyph, app_id_t id)
{
    lv_obj_t *holder = plain(parent);
    lv_obj_set_pos(holder, x, y);
    lv_obj_set_size(holder, 96, 124);
    lv_obj_add_flag(holder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(holder, home_app_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *inner = plain(holder);
    lv_obj_set_size(inner, 96, 96);
    lv_obj_set_pos(inner, 0, 0);
    /* 图标方块本身也接一次点击：手指落在实测覆盖面积最大的这块上，
     * 双重保险，避免命中判定落在非可点的子对象上导致"点了没反应"。 */
    lv_obj_add_flag(inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(inner, home_app_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_set_style_radius(inner, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(inner, lv_color_hex(c1), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(inner, lv_color_hex(c2), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(inner, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, LV_PART_MAIN);
    /* 按下时压暗，模拟 iPadOS 的图标按压反馈 */
    lv_obj_set_style_bg_opa(inner, LV_OPA_70, LV_STATE_PRESSED);

    lv_obj_t *ico = icon_glyph(inner, glyph, c_ico, &ui_icon_40);
    lv_obj_center(ico);

    lv_obj_t *l = mk_label(holder);
    lv_label_set_text(l, i18n_t(label));
    lv_obj_set_style_text_font(l, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), LV_PART_MAIN);
    lv_obj_set_width(l, 96);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(l, 0, 102);

    return holder;
}

/* ---------------- 桌面系统状态：内存 / TF 卡 ---------------- */

/* 内部 RAM 占用率。口径与信息页一致：默认堆里并了 32 MB PSRAM，
 * 按默认堆算永远是两位数以下，看不出运行时压力。 */
static int mem_used_pct(void)
{
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t fre   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (total == 0) {
        return -1;
    }
    return (int)(((total - fre) * 100u) / total);
}

/* 容量格式化：大卡用 GB 一位小数，小卡用 MB 整数 */
static void fmt_cap(char *buf, size_t n, uint64_t used_kb, uint64_t total_kb)
{
    if (total_kb >= 1024u * 1024u) {
        const unsigned u10 = (unsigned)(used_kb * 10u / (1024u * 1024u));
        const unsigned t10 = (unsigned)(total_kb * 10u / (1024u * 1024u));
        snprintf(buf, n, "%u.%u / %u.%u GB", u10 / 10u, u10 % 10u, t10 / 10u, t10 % 10u);
    } else {
        snprintf(buf, n, "%u / %u MB",
                 (unsigned)(used_kb / 1024u), (unsigned)(total_kb / 1024u));
    }
}

/* 每 400 ms 调一次的刷新，文字都带死区。 */
static void home_stat_update(int64_t now_us)
{
    (void)now_us;

    if (s_cur != APP_HOME) {
        return;                     /* 看不见就不写文字，省掉重排 */
    }

    /* ---- 设备台数：整数变了才写（稳态完全不重绘） ----
     * 大字 = 已识别且在线的 OTA 设备；右上角只在真有设备掉线时才出现，
     * 平时（离线 0 台）整条隐藏，避免又出现一个"看不懂的第二数字"。 */
    const int online = dev_table_online_count();
    if (online != s_home_online_shown) {
        s_home_online_shown = online;
        set_txt_fmt(s_home_online, "%d", online);
    }

    const int offl = dev_table_offline_count();
    if (offl != s_home_devoff_shown) {
        s_home_devoff_shown = offl;
        if (offl <= 0) {
            lv_obj_add_flag(s_home_dev_off, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_txt_fmt(s_home_dev_off, i18n_t(H_DEV_OFFLINE_FMT), (unsigned)offl);
            lv_obj_remove_flag(s_home_dev_off, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ---- 内存：2% 死区（内部 RAM 的 1% ≈ 4 KB，稳态下进度条不重绘） ---- */
    const int mp = mem_used_pct();
    if (mp >= 0 && (s_home_mem_shown < 0 || abs(mp - s_home_mem_shown) >= 2)) {
        s_home_mem_shown = mp;
        set_txt_fmt(s_home_mem_pct, "%d%%", mp);
        lv_bar_set_value(s_home_mem_bar, mp, LV_ANIM_OFF);
    }

    /* ---- TF 卡：只读 bsp_sdcard 的缓存，不碰文件系统 ---- */
    uint64_t tk = 0;
    uint64_t fk = 0;
    if (bsp_sdcard_usage(&tk, &fk)) {
        char cap[40];
        fmt_cap(cap, sizeof(cap), tk > fk ? tk - fk : 0, tk);
        set_txt(s_home_tf, cap);              /* set_txt 自带判重 */
        if (s_tf_shown != 1) {
            s_tf_shown = 1;
            lv_obj_set_style_text_color(s_home_tf, lv_color_hex(C_TXT), LV_PART_MAIN);
        }
    } else if (s_tf_shown != -1) {
        s_tf_shown = -1;
        set_txt(s_home_tf, i18n_t(H_TF_NONE));
        lv_obj_set_style_text_color(s_home_tf, lv_color_hex(C_SUB), LV_PART_MAIN);
    }
}

static void build_home(lv_obj_t *page)
{
    /* 状态栏不在这里建：它是全局的（s_sb），浮在所有页之上，见 build_status_bar() */

    /* ---- 左：网络 widget ----
     * 底和右卡一样是实底白（原来是 80% 半透明，两张卡摆一起色差明显）。
     * 下面三行的行距从 32 放到 41：原来最后一行停在 y=172，
     * 卡高 236、小字行高 19，底下空出 45 px，比左右 24 px 内边距宽出一截，
     * 看着像"内容没填满"。三行等距摊到 108 / 149 / 190 之后，
     * 底边留白 236-190-19 = 27，和右卡 TF 行（同样 y=190）对齐。
     *
     * 标签 20→16、IP 42→55：原来标签底 38 紧贴 IP 顶 42（只隔 4 px），
     * 而 IP 底 74 到 LINK 行 108 却空着 34 px，上下严重不对称，看着就是
     * "标题黏在 IP 上、IP 下面挂了块空"。按行盒算，改成上 21 / 下 21
     * （55-16-18 = 21，108-55-32 = 21），数字落在整块的视觉中心。
     * 标签同时与右卡 DEVICES 的 y=16 对齐，两卡首行同一基线。 */
    lv_obj_t *c1 = card(page, 32, 56, 466, 236, 24);
    lv_obj_set_style_border_width(c1, 0, LV_PART_MAIN);

    text(c1, i18n_t(H_NETWORK), UI_F_SMALL, C_SUB, 24, 16);
    s_home_ip = text(c1, "0.0.0.0", UI_F_LARGE, C_TXT, 24, 55);

    text(c1, i18n_t(H_LINK), UI_F_SMALL, C_SUB, 24, 108);
    s_home_link = text(c1, "--", UI_F_SMALL, C_TXT, 0, 108);
    lv_obj_align(s_home_link, LV_ALIGN_TOP_RIGHT, -24, 108);

    text(c1, i18n_t(H_DHCP_LEASE), UI_F_SMALL, C_SUB, 24, 149);
    s_home_lease = text(c1, "0", UI_F_SMALL, C_TXT, 0, 149);
    lv_obj_align(s_home_lease, LV_ALIGN_TOP_RIGHT, -24, 149);

    text(c1, i18n_t(H_FREE_HEAP), UI_F_SMALL, C_SUB, 24, 190);
    s_home_heap = text(c1, "0 KB", UI_F_SMALL, C_TXT, 0, 190);
    lv_obj_align(s_home_heap, LV_ALIGN_TOP_RIGHT, -24, 190);

    /* ---- 右：设备 / 运行状态（在线台数 + 内存 + TF 卡） ----
     * 这里原来是 CPU 折线（60 点、每秒推一个点）。换成"在线设备台数"的理由：
     * 主页是用户一进来就会看的位置，而"现在有几台设备能升、有几台掉线了"
     * 才是这个工具的主信息；CPU 占用率属于开发期数据，信息页里仍有百分比。
     * 顺带也去掉了每秒一次的整卡重绘（"主页一直闪"的老来源）。
     *
     * 卡片是实底白，不做半透明：半透明意味着每次重绘都要把下面的渐变壁纸
     * 重新合成一遍，而且两张卡摆一起能有肉眼可见的色差。 */
    lv_obj_t *c2 = card(page, 526, 56, 466, 236, 24);
    lv_obj_set_style_border_width(c2, 0, LV_PART_MAIN);

    text(c2, i18n_t(H_DEVICES), UI_F_SMALL, C_SUB, 24, 16);

    /* 大号数字 = 已识别且在线的 OTA 设备台数。
     * 之前这里还有一个右上角"共 N 台"，取自设备表的 used 槽位数 ——
     * 而表里凡是拿到 DHCP 租约的主机都会占一槽（PC、手机、打印机都算），
     * 于是"共 5 台 / 在线 1 台"这种数字对用户毫无意义。
     * 现在右上角只在真有已识别设备掉线时显示"N 台离线"，平时隐藏。 */
    s_home_online = text(c2, "-", UI_F_LARGE, C_TXT, 24, 40);
    text(c2, i18n_t(H_DEV_ONLINE), UI_F_SMALL, C_SUB, 24, 88);

    s_home_dev_off = text(c2, "", UI_F_SMALL, C_SUB, 0, 56);
    lv_obj_align(s_home_dev_off, LV_ALIGN_TOP_RIGHT, -24, 56);
    lv_obj_add_flag(s_home_dev_off, LV_OBJ_FLAG_HIDDEN);   /* 0 台离线 → 不占位 */

    /* 细分隔线：设备区 / 运行状态区各占一半，视觉上分得开 */
    lv_obj_t *dsep = plain(c2);
    lv_obj_set_pos(dsep, 24, 118);
    lv_obj_set_size(dsep, 466 - 48, 1);
    lv_obj_set_style_bg_color(dsep, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dsep, LV_OPA_COVER, LV_PART_MAIN);

    /* 内存行：标签 + 右侧百分比（同一行），进度条单独占下一行。
     *
     * 原来三者挤在一行：标签 x=24、进度条 x=78。中文"内存"只有 32 px 还看不出来，
     * 英文 "MEMORY" 在 16 px 下约 66 px，直接压到进度条头上。
     * 这一行的高度是随语言浮动的（"TF CARD" 也比 "TF 卡" 宽），
     * 横向顺排就得先量文字宽度，量早了 coords 还没算出来；
     * 改成上下两行最稳：进度条固定 x=24、占满卡片内宽，任何语言都不会撞。 */
    text(c2, i18n_t(H_MEM), UI_F_SMALL, C_SUB, 24, 138);

    s_home_mem_pct = text(c2, "--", UI_F_SMALL, C_TXT, 0, 138);
    lv_obj_align(s_home_mem_pct, LV_ALIGN_TOP_RIGHT, -24, 138);

    s_home_mem_bar = lv_bar_create(c2);
    lv_obj_remove_flag(s_home_mem_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_home_mem_bar, 24, 166);
    lv_obj_set_size(s_home_mem_bar, 466 - 48, 8);   /* 卡片内宽，左右各留 24 px */
    lv_bar_set_range(s_home_mem_bar, 0, 100);
    lv_bar_set_value(s_home_mem_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_home_mem_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_home_mem_bar, lv_color_hex(0xE9E9EE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_home_mem_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_home_mem_bar, lv_color_hex(C_BLUE), LV_PART_INDICATOR);

    /* TF 卡行：只读缓存，不碰文件系统（f_getfree 要遍历 FAT，会卡 LVGL 任务） */
    text(c2, i18n_t(H_TF), UI_F_SMALL, C_SUB, 24, 190);
    s_home_tf = text(c2, i18n_t(H_TF_NONE), UI_F_SMALL, C_SUB, 0, 190);
    lv_obj_align(s_home_tf, LV_ALIGN_TOP_RIGHT, -24, 190);

    /* ---- 图标网格：4 个居中排布，4*96 + 3*32 = 480，(1024-480)/2 = 272 ---- */
    app_tile(page, 272, 322, A_DEVICES,  0xEAF3FF, 0xD4E6FF, 0x1A63C8, ICO_DEVICES,  APP_DEVICES);
    app_tile(page, 400, 322, A_UPGRADE,  0xE8F9EF, 0xD0F1DD, 0x127A3E, ICO_UPGRADE, APP_UPGRADE);
    app_tile(page, 528, 322, A_INFO,     0xF3ECFF, 0xE3D4FF, 0x6B34C0, ICO_INFO,     APP_INFO);
    app_tile(page, 656, 322, A_SETTINGS, 0xF2F2F7, 0xE4E4EC, 0x4A4C5A, ICO_SETTINGS, APP_SETTINGS);

    /* ---- 底部浮条：工具名 + 副标题 + 版本 ---- */
    lv_obj_t *dock = pill(page, 32, 516, 960, 56, C_CARD, LV_OPA_70);

    lv_obj_t *dic = icon_glyph(dock, ICO_UPLOAD, C_BLUE, &ui_icon_24);
    lv_obj_align(dic, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *dn = mk_label(dock);
    lv_label_set_text(dn, APP_NAME);
    lv_obj_set_style_text_font(dn, UI_F_BASE, LV_PART_MAIN);
    lv_obj_set_style_text_color(dn, lv_color_hex(C_TXT), LV_PART_MAIN);
    lv_obj_align(dn, LV_ALIGN_LEFT_MID, 60, 0);

    /* 原来的副标题 "Ethernet + Wi-Fi" 已按需求去掉，浮条只留工具名 + 版本 */

    lv_obj_t *dv = mk_label(dock);
    lv_label_set_text(dv, APP_VERSION);
    lv_obj_set_style_text_font(dv, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(dv, lv_color_hex(C_SUB), LV_PART_MAIN);
    lv_obj_align(dv, LV_ALIGN_RIGHT_MID, -28, 0);
}

/* ---------------- 设备列表 ---------------- */

static int ip_a(uint32_t ip) { return (int)(ip & 0xFF); }
static int ip_b(uint32_t ip) { return (int)((ip >> 8) & 0xFF); }
static int ip_c(uint32_t ip) { return (int)((ip >> 16) & 0xFF); }
static int ip_d(uint32_t ip) { return (int)((ip >> 24) & 0xFF); }

static void open_detail(int idx)
{
    s_detail_idx = idx;
    page_show(APP_DETAIL, true);
}

static void dev_row_cb(lv_event_t *e)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static uint32_t dev_sig(const dev_entry_t *s, int n)
{
    uint32_t h = 2166136261u ^ (uint32_t)n;

    for (int i = 0; i < n; i++) {
        h = h * 16777619u ^ s[i].ip;
        h = h * 16777619u ^ (uint32_t)((s[i].online ? 1u : 0u) | (s[i].identified ? 2u : 0u));
        for (int k = 0; k < 4; k++) {
            h = h * 16777619u ^ s[i].id.fw_ver[k];
        }
        for (const char *p = s[i].id.model; *p != '\0'; p++) {
            h = h * 16777619u ^ (uint8_t)*p;
        }
    }
    return h;
}

static void build_dev_list(void)
{
    dev_entry_t snap[APP_DEV_MAX];
    int cnt = dev_table_snapshot(snap, APP_DEV_MAX);

    lv_obj_clean(s_dev_list);

    if (cnt == 0) {
        lv_obj_t *empty = card(s_dev_list, 0, 0, 984, 120, 18);
        lv_obj_t *l = mk_label(empty);
        /* 没链路和设备表为空是两件事：前者请用户插网线，后者请用户开设备。
         * 链路断开时上层已把表清空，光靠 "No device yet" 会让人以为设备没启动。 */
        if (!net_eth_link_up()) {
            lv_label_set_text(l, i18n_t(S_NO_LINK_HINT));
        } else {
            lv_label_set_text(l, i18n_t(S_NO_DEVICE_HINT));
        }
        lv_obj_set_style_text_font(l, UI_F_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), LV_PART_MAIN);
        lv_obj_center(l);
        return;
    }

    for (int i = 0; i < cnt; i++) {
        const dev_entry_t *e = &snap[i];
        int y = i * 104;

        lv_obj_t *r = card(s_dev_list, 0, y, 984, 92, 18);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, dev_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = plain(r);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_pos(dot, 24, 26);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(e->online ? C_GREEN : C_SUB), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);

        if (e->identified) {
            /* 命中白名单显示规范名（"VENTRIX CPU"），否则原样显示设备自报的字符串 */
            char mname[40];
            text(r, dev_model_display(e->id.model, mname, sizeof(mname)), UI_F_BASE, C_TXT, 48, 18);

            char sub[64];
            snprintf(sub, sizeof(sub), "%d.%d.%d.%d    %s",
                     ip_a(e->ip), ip_b(e->ip), ip_c(e->ip), ip_d(e->ip), dev_mac_str(e->mac));
            text(r, sub, UI_F_SMALL, C_SUB, 48, 50);
        } else {
            text(r, i18n_t(S_UNKNOWN_HOST), UI_F_BASE, C_SUB, 48, 18);

            char sub[64];
            snprintf(sub, sizeof(sub), "%d.%d.%d.%d    %s",
                     ip_a(e->ip), ip_b(e->ip), ip_c(e->ip), ip_d(e->ip),
                     i18n_t(S_LEASES_ONLY));
            text(r, sub, UI_F_SMALL, C_SUB, 48, 50);
        }

        /* 右侧：固件胶囊 + 箭头 */
        lv_obj_t *chip = pill(r, 700, 32, 210, 30, 0xF2F2F7, LV_OPA_COVER);
        lv_obj_t *cl = mk_label(chip);
        if (e->identified) {
            lv_label_set_text_fmt(cl, i18n_t(D_FW),
                                  e->id.fw_ver[0], e->id.fw_ver[1], e->id.fw_ver[2], e->id.fw_ver[3]);
        } else {
            lv_label_set_text(cl, i18n_t(D_FW_NONE));
        }
        lv_obj_set_style_text_font(cl, UI_F_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(cl, lv_color_hex(C_SUB), LV_PART_MAIN);
        lv_obj_center(cl);

        text(r, ">", UI_F_BASE, C_SUB, 932, 32);
    }
}

/* ---------------- 设备详情 ---------------- */

static void dt_upgrade_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_idx >= 0) {
        s_upg_sel = s_detail_idx;
    }
    page_show(APP_UPGRADE, true);
}

static lv_obj_t *kv_row(lv_obj_t *parent, const char *k, int y, lv_obj_t **out)
{
    text(parent, k, UI_F_SMALL, C_SUB, 24, y);
    lv_obj_t *v = text(parent, "-", UI_F_BASE, C_TXT, 240, y - 4);
    if (out) {
        *out = v;
    }
    return v;
}

static void build_detail(lv_obj_t *page)
{
    lv_obj_t *c = card(page, 20, CONTENT_Y, 640, 420, 20);

    kv_row(c, i18n_t(DT_MODEL), 24, &s_dt_model);
    kv_row(c, i18n_t(DT_IP), 70, &s_dt_ip);
    kv_row(c, i18n_t(DT_FIRMWARE), 116, &s_dt_fw);
    kv_row(c, i18n_t(DT_HARDWARE), 162, &s_dt_hw);
    kv_row(c, i18n_t(DT_FLASH), 208, &s_dt_flash);
    kv_row(c, i18n_t(DT_CAPS), 254, &s_dt_caps);
    kv_row(c, i18n_t(DT_UID), 300, &s_dt_uid);
    kv_row(c, i18n_t(DT_HOST), 346, &s_dt_host);

    lv_obj_t *c2 = card(page, 680, CONTENT_Y, 324, 420, 20);
    text(c2, i18n_t(DT_MAC), UI_F_SMALL, C_SUB, 24, 24);
    s_dt_mac = text(c2, "-", UI_F_SMALL, C_TXT, 24, 52);
    lv_obj_set_width(s_dt_mac, 276);
    lv_label_set_long_mode(s_dt_mac, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *btn = plain(page);
    lv_obj_set_pos(btn, 20, 520);
    lv_obj_set_size(btn, 320, 56);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, dt_upgrade_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = mk_label(btn);
    lv_label_set_text(bl, i18n_t(DT_UPGRADE_THIS));
    lv_obj_set_style_text_font(bl, UI_F_BASE, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(bl);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0060DF), LV_STATE_PRESSED);
}

static void detail_fill(int idx)
{
    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);

    if (idx < 0 || idx >= n) {
        set_txt(s_dt_model, i18n_t(S_DEVICE_GONE));
        return;
    }

    const dev_entry_t *e = &snap[idx];

    if (e->identified) {
        char mname[40];
        set_txt(s_dt_model, dev_model_display(e->id.model, mname, sizeof(mname)));
        set_txt_fmt(s_dt_fw, "%u.%u.%u.%u",
                    e->id.fw_ver[0], e->id.fw_ver[1], e->id.fw_ver[2], e->id.fw_ver[3]);
        set_txt_fmt(s_dt_hw, "%u.%u", (unsigned)(e->id.hw_ver >> 8),
                    (unsigned)(e->id.hw_ver & 0xFF));
        set_txt_fmt(s_dt_flash, "%u KB", (unsigned)(e->id.flash_size / 1024u));
        set_txt_fmt(s_dt_caps, "0x%04x", e->id.caps);

        char uid[40];
        snprintf(uid, sizeof(uid), "%02x%02x%02x%02x...%02x%02x",
                 e->id.uid[0], e->id.uid[1], e->id.uid[2], e->id.uid[3],
                 e->id.uid[14], e->id.uid[15]);
        set_txt(s_dt_uid, uid);
    } else {
        set_txt(s_dt_model, i18n_t(S_UNKNOWN_HOST));
        set_txt(s_dt_fw, "-");
        set_txt(s_dt_hw, "-");
        set_txt(s_dt_flash, "-");
        set_txt(s_dt_caps, "-");
        set_txt(s_dt_uid, "-");
    }

    set_txt_fmt(s_dt_ip, "%d.%d.%d.%d", ip_a(e->ip), ip_b(e->ip), ip_c(e->ip), ip_d(e->ip));
    set_txt(s_dt_host, e->hostname[0] ? e->hostname : "-");
    set_txt(s_dt_mac, dev_mac_str(e->mac));
}

/* ---------------- 升级页 ---------------- */

static uint32_t next_version(const uint8_t cur[4])
{
    uint32_t build = cur[3];
    uint32_t patch = cur[2];

    build = (build + 1) & 0xFFu;
    if (build == 0) {
        patch = (patch + 1) & 0xFFu;
    }
    return OTA_FW_VER(cur[0], cur[1], patch, build);
}

typedef struct {
    uint32_t ip;
    uint8_t  uid[DEV_UID_LEN];
    uint8_t  mac[6];
    char     model[DEV_MODEL_LEN + 1];
    uint16_t hw;
    uint8_t  fw[4];
} upg_req_t;

/* 合成镜像要算 256 KB 的 CRC32 + SHA-256，放在独立任务里做，
 * 绝不能塞进 LVGL 定时器（会把界面卡住几百毫秒）。 */
static void upg_task(void *arg)
{
    upg_req_t *r = (upg_req_t *)arg;
    ota_image_t img;                /* 放栈上：ota_client_start 会整体拷走 */

    esp_err_t err = ota_image_make_synthetic(&img, r->model, r->hw, r->hw,
                                             next_version(r->fw),
                                             (uint32_t)APP_OTA_SYNTH_SIZE_KB * 1024u,
                                             APP_OTA_SYNTH_SEED);
    if (err == ESP_OK) {
        err = ota_client_start(r->ip, r->uid, r->mac, &img);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发起升级失败：%s", esp_err_to_name(err));
    }

    free(r);
    vTaskDelete(NULL);
}

static void upg_start_cb(lv_event_t *e)
{
    (void)e;

    if (s_upg_sel < 0 || ota_client_busy()) {
        return;
    }

    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);
    if (s_upg_sel >= n) {
        return;
    }

    const dev_entry_t *d = &snap[s_upg_sel];
    if (!d->identified || d->ip == 0) {
        return;
    }

    upg_req_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return;
    }

    r->ip = d->ip;
    r->hw = d->id.hw_ver;
    memcpy(r->uid, d->id.uid, DEV_UID_LEN);
    memcpy(r->mac, d->mac, 6);
    memcpy(r->fw, d->id.fw_ver, 4);

    /* 不用 strncpy：目标 17 字节、n=16 时 GCC 会判 -Werror=stringop-truncation。
     * r 是 calloc 出来的，拷完手动补 0 即可，语义仍是"线上 16 B 定长、不足补 0"。 */
    size_t mlen = strnlen(d->id.model, DEV_MODEL_LEN);
    memcpy(r->model, d->id.model, mlen);
    r->model[mlen] = '\0';

    if (xTaskCreate(upg_task, "ui_upg", 6144, r, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        free(r);
    }
}

static void upg_chip_cb(lv_event_t *e)
{
    s_upg_sel = (int)(intptr_t)lv_event_get_user_data(e);
    s_upg_have_sig = false;         /* 强制重画选中态 */
}

static void build_upg_list(void)
{
    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);

    lv_obj_clean(s_upg_list);

    if (n == 0) {
        /* 一台设备都没有时，"SELECT DEVICE" 这个标题是空指的（下面没有可选的东西），
         * 先把标题藏掉，只留居中的 "No device"，卡片里不出现两行自相矛盾的文案。 */
        if (s_upg_hdr != NULL) {
            lv_obj_add_flag(s_upg_hdr, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t *l = mk_label(s_upg_list);
        lv_label_set_text(l, i18n_t(S_NO_DEVICE));
        lv_obj_set_style_text_font(l, UI_F_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), LV_PART_MAIN);
        lv_obj_center(l);        /* 标题收起来后整块列表区是空的，居中比贴左上角好看 */
        return;
    }

    if (s_upg_hdr != NULL) {
        lv_obj_remove_flag(s_upg_hdr, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_upg_sel >= n) {
        s_upg_sel = 0;
    }
    if (s_upg_sel < 0) {
        s_upg_sel = 0;
    }

    for (int i = 0; i < n; i++) {
        const dev_entry_t *e = &snap[i];
        bool sel = (i == s_upg_sel);

        lv_obj_t *c = card(s_upg_list, 0, i * 80, 356, 68, 14);
        lv_obj_set_style_border_width(c, sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(c, lv_color_hex(sel ? C_BLUE : C_LINE), LV_PART_MAIN);
        if (sel) {
            lv_obj_set_style_bg_color(c, lv_color_hex(0xEAF3FF), LV_PART_MAIN);
        }
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, upg_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        text(c, e->identified ? e->id.model : i18n_t(S_UNKNOWN), UI_F_BASE, C_TXT, 16, 10);

        char sub[32];
        snprintf(sub, sizeof(sub), "%d.%d.%d.%d", ip_a(e->ip), ip_b(e->ip), ip_c(e->ip), ip_d(e->ip));
        text(c, sub, UI_F_SMALL, C_SUB, 16, 38);
    }
}

static void build_upgrade(lv_obj_t *page)
{
    /* 左：设备选择 */
    lv_obj_t *left = card(page, 20, CONTENT_Y, 380, 492, 20);
    s_upg_hdr = text(left, i18n_t(U_SELECT_DEVICE), UI_F_SMALL, C_SUB, 20, 18);

    s_upg_list = plain(left);
    lv_obj_set_pos(s_upg_list, 12, 50);
    lv_obj_set_size(s_upg_list, 356, 424);
    lv_obj_add_flag(s_upg_list, LV_OBJ_FLAG_CLICKABLE);   /* 滚动判定要求可点 */
    lv_obj_add_flag(s_upg_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_upg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_upg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_upg_list, LV_OPA_TRANSP, LV_PART_MAIN);

    /* 右：进度环 */
    lv_obj_t *right = card(page, 416, CONTENT_Y, 588, 492, 20);

    s_upg_arc = lv_arc_create(right);
    lv_obj_set_size(s_upg_arc, 210, 210);
    lv_obj_align(s_upg_arc, LV_ALIGN_TOP_MID, 0, 34);
    lv_arc_set_rotation(s_upg_arc, 0);
    lv_arc_set_bg_angles(s_upg_arc, 135, 45);
    lv_arc_set_range(s_upg_arc, 0, 100);
    lv_arc_set_value(s_upg_arc, 0);
    lv_obj_remove_flag(s_upg_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_upg_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_upg_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_upg_arc, lv_color_hex(0xE9E9EE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_upg_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_upg_arc, lv_color_hex(C_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_upg_arc, true, LV_PART_INDICATOR);

    /* 百分比：宽度锁成和圆环同宽（210）再居中文字。
     * 让 label 自适应宽度的话，"9%" 变 "100%" 时盒子只有右边变长，
     * 中心跟着左移，看起来数字一直不在环正中。 */
    s_upg_pct = text(right, "0%", UI_F_LARGE, C_TXT, 0, 0);
    lv_obj_set_width(s_upg_pct, 210);
    lv_obj_set_style_text_align(s_upg_pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(s_upg_pct, s_upg_arc, LV_ALIGN_CENTER, 0, 0);

    /* 状态行和副标题同样处理：文案在"正在连接 / 升级成功 / 失败"之间换，
     * 自适应宽度会让它们在环下方左右漂。 */
    s_upg_state = text(right, i18n_t(U_READY), UI_F_BASE, C_TXT, 0, 0);
    lv_obj_set_width(s_upg_state, 480);
    lv_obj_set_style_text_align(s_upg_state, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(s_upg_state, s_upg_arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);

    s_upg_sub = text(right, i18n_t(U_SELECT_HINT), UI_F_SMALL, C_SUB, 0, 0);
    lv_obj_set_width(s_upg_sub, 480);
    lv_obj_set_style_text_align(s_upg_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(s_upg_sub, s_upg_state, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    s_upg_btn = plain(right);
    lv_obj_set_size(s_upg_btn, 260, 54);
    lv_obj_align(s_upg_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_radius(s_upg_btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_upg_btn, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_upg_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_upg_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_upg_btn, upg_start_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = mk_label(s_upg_btn);
    lv_label_set_text(bl, i18n_t(U_START));
    lv_obj_set_style_text_font(bl, UI_F_BASE, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(bl);
}

/* ---------------- 信息页 ---------------- */

static lv_obj_t *info_row(lv_obj_t *parent, const char *k, int y)
{
    text(parent, k, UI_F_SMALL, C_SUB, 24, y);
    lv_obj_t *v = text(parent, "-", UI_F_BASE, C_TXT, 0, y - 4);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -24, y - 4);
    return v;
}

/* 卡片小标题：比行标签大一号、用主文字色，下面压一条 1 px 分隔线。
 * 原来标题和行标签都是 UI_F_SMALL + C_SUB，两行字在视觉上完全同级，
 * 一眼看不出"这张卡从哪开始"。 */
static void info_title(lv_obj_t *card, const char *t)
{
    text(card, t, UI_F_BASE, C_TXT, 24, 14);

    lv_obj_t *sep = plain(card);
    lv_obj_set_pos(sep, 24, 50);
    lv_obj_set_size(sep, 484 - 48, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
}

static void build_info(lv_obj_t *page)
{
    /* 两张卡等高 340：内容（5 / 4 行）铺满即可，原来 460 高只是为了塞下
     * 已经删掉的设备数 / 升级状态 / CPU 三行，留着底下一大片空白反而难看。 */
    lv_obj_t *a = card(page, 20, CONTENT_Y, 484, 340, 20);
    info_title(a, i18n_t(I_NETWORK));
    s_in_ip    = info_row(a, i18n_t(I_ADDRESS), 66);
    s_in_mask  = info_row(a, i18n_t(I_NETMASK), 120);
    s_in_gw    = info_row(a, i18n_t(I_GATEWAY), 174);
    s_in_link  = info_row(a, i18n_t(I_LINK), 228);
    s_in_lease = info_row(a, i18n_t(I_LEASE), 282);

    lv_obj_t *b = card(page, 520, CONTENT_Y, 484, 340, 20);
    info_title(b, i18n_t(I_SYSTEM));

    /* 内存：一条三段条 + 右上角大号百分比。
     * 只看内部 RAM（MALLOC_CAP_INTERNAL）：默认堆里并入了 32 MB PSRAM，
     * 按默认堆算占用率会被稀释到个位数，看不出运行时的真实压力。 */
    text(b, i18n_t(I_RAM), UI_F_SMALL, C_SUB, 24, 68);

    s_in_mem_pct = text(b, "0%", UI_F_LARGE, C_TXT, 0, 54);
    lv_obj_align(s_in_mem_pct, LV_ALIGN_TOP_RIGHT, -24, 54);

    /* 条不用 lv_bar：bar 只有一个 indicator，没法把"系统预留"和"堆已用"
     * 分两种颜色。这里自己拼三段，坐标由 refresh 按实际 KB 数算宽度：
     *   [0, res)          灰   系统/静态预留（开机即固定，768 - 堆总量）
     *   [res, used)       彩色 堆已用（蓝 / 橙 / 红）
     *   [used, 768)       浅灰 堆空闲
     * 彩色段左移 6 px 盖住灰段的圆角，接缝才不会有豁口。 */
    lv_obj_t *trk = plain(b);
    lv_obj_set_pos(trk, 24, 96);
    lv_obj_set_size(trk, UI_MEM_BAR_W, 12);
    lv_obj_set_style_radius(trk, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(trk, lv_color_hex(0xE9E9EE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(trk, LV_OPA_COVER, LV_PART_MAIN);

    s_in_mem_res = plain(b);
    lv_obj_set_pos(s_in_mem_res, 24, 96);
    lv_obj_set_size(s_in_mem_res, 0, 12);
    lv_obj_set_style_radius(s_in_mem_res, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_in_mem_res, lv_color_hex(0xB4B4BF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_in_mem_res, LV_OPA_COVER, LV_PART_MAIN);

    s_in_mem_used = plain(b);
    lv_obj_set_pos(s_in_mem_used, 24, 96);
    lv_obj_set_size(s_in_mem_used, 0, 12);
    lv_obj_set_style_radius(s_in_mem_used, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_in_mem_used, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_in_mem_used, LV_OPA_COVER, LV_PART_MAIN);

    /* 小字就是这条的图例：三段各写一个 KB 数，并用 recolor 染成和条上
     * 一一对应的颜色（已用=条色 / 系统=灰 / 空闲=浅灰），不用再去猜
     * "这根蓝的到底算不算系统"。'#' 只作标记，不进渲染。 */
    s_in_mem_sub = text(b, "", UI_F_SMALL, C_SUB, 24, 118);
    lv_label_set_recolor(s_in_mem_sub, true);

    s_in_psram = info_row(b, i18n_t(I_PSRAM), 152);
    s_in_flash = info_row(b, i18n_t(I_FLASH), 200);
    s_in_min   = info_row(b, i18n_t(I_MIN_HEAP), 248);
    s_in_up    = info_row(b, i18n_t(I_UPTIME), 296);
}

/* ---------------- 设置页 ---------------- */

static void ui_app_rebuild(void);

/* 语言切换走异步：当前是在"点击某一行"的事件派发过程中，
 * 如果直接 lv_obj_clean(screen) 会把正在派发事件的那个对象一起删掉，
 * LVGL 之后取不到它的父链，轻则事件错乱重则死机。
 * lv_async_call 把重建推到本轮事件处理结束之后执行。 */
/* NVS 写在独立任务里做：nvs_commit 的栈需求会叠加到调用者任务上，
 * 而 LVGL 任务的栈本来就紧（还要跑 build_all 这一串递归）。
 * 写失败不影响本次会话，只是下次上电回到默认语言。 */
static void lang_save_task(void *arg)
{
    i18n_save((ui_lang_t)(intptr_t)arg);
    vTaskDelete(NULL);
}

static void rebuild_async(void *arg)
{
    /* 注意别写 if (arg != NULL)：LANG_EN 的值就是 0，转成 void* 等于 NULL，
     * 那样的判空会让"切回英文"永远不生效。这里无条件当语言用。 */
    ui_lang_t lang = (ui_lang_t)(intptr_t)arg;

    i18n_set_mem(lang);          /* 内存里先切，重建时文案和字体就是新语言 */
    ui_app_rebuild();

    if (xTaskCreate(lang_save_task, "ui_lang", 3072, (void *)(intptr_t)lang,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "创建语言保存任务失败，本次切换不会持久化");
    }
}

static void lang_pick_cb(lv_event_t *e)
{
    ui_lang_t want = (ui_lang_t)(intptr_t)lv_event_get_user_data(e);
    if (want == i18n_lang()) {
        return;
    }
    lv_async_call(rebuild_async, (void *)(intptr_t)want);
}

/* 设置行：左侧图标 + 标题，右侧当前值（+ 可选箭头）。
 * 行本身透明，靠卡片的白色底和 1 px 分隔线形成 iOS 分组列表的观感。
 * 以后要加设置项，只要再调一次 set_row / row_sep，卡片高度跟着算就行。
 * 返回值是右侧那个数值 label（没有值时为 NULL），调用方拿它可以后续改写，
 * 比如"亮度"行要在子页里拖滑块时同步更新。
 *
 * ico_scale：图标的绘制缩放，256 = 原样。iconfont 里各字形的实际画幅并不统一
 * （e698 是 18×18，e654 是 22×22），同一字号下有的字看着"矮一头"，
 * 用这个参数按需放大到与其他行视觉一致；以字形中心为支点，行高不动。 */
static lv_obj_t *set_row(lv_obj_t *parent, int y, int w, const char *ico, const char *title,
                         const char *value, lv_event_cb_t cb, void *ud, bool chevron,
                         int ico_scale)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_pos(r, 24, y);
    lv_obj_set_size(r, w, 64);

    if (cb != NULL) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
        /* 按下整行变灰：iOS 的行内按压反馈 */
        lv_obj_set_style_bg_color(r, lv_color_hex(0xE9E9EE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 12, LV_STATE_PRESSED);
    }

    lv_obj_t *ic = icon_glyph(r, ico, C_BLUE, &ui_icon_24);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    if (ico_scale != 0 && ico_scale != 256) {
        lv_obj_set_style_transform_pivot_x(ic, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(ic, lv_pct(50), 0);
        lv_obj_set_style_transform_scale_x(ic, ico_scale, 0);
        lv_obj_set_style_transform_scale_y(ic, ico_scale, 0);
    }

    lv_obj_t *t = text(r, title, UI_F_BASE, C_TXT, 0, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 42, 0);

    int shift = 0;
    if (chevron) {
        lv_obj_t *ar = text(r, ">", UI_F_SMALL, C_LINE, 0, 0);
        lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -4, 0);
        shift = 24;
    }

    lv_obj_t *v = NULL;
    if (value != NULL) {
        v = text(r, value, UI_F_SMALL, C_SUB, 0, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -shift, 0);
    }
    return v;
}

/* 行间分隔线：和 iOS 一样从图标右侧起画，不顶到卡片边缘 */
static void row_sep(lv_obj_t *parent, int y, int w, int x0)
{
    lv_obj_t *l = plain(parent);
    lv_obj_set_pos(l, x0, y);
    lv_obj_set_size(l, w, 1);
    lv_obj_set_style_bg_color(l, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---------------- 亮度 ---------------- */

/* 读上一次的亮度。需要 nvs_flash_init 之后调用（ui_app_start 里）。 */
static void bright_load(void)
{
    nvs_handle_t h;
    if (nvs_open(BRIGHT_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "无亮度记录，用默认 %d%%", s_bright);
        return;
    }

    uint8_t v = 0;
    if (nvs_get_u8(h, BRIGHT_KEY, &v) == ESP_OK && v >= BRIGHT_MIN && v <= 100) {
        s_bright = v;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "亮度：%d%%", s_bright);
}

/* 写 NVS 放独立任务里做：nvs_commit 的栈需求会叠加到调用者身上，
 * 而 LVGL 任务的栈本来就紧。和语言保存同一套做法。 */
static void bright_save_task(void *arg)
{
    int v = (int)(intptr_t)arg;

    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHT_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, BRIGHT_KEY, (uint8_t)v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "亮度保存失败（本次仍生效）：%s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void bright_save_async(void)
{
    if (xTaskCreate(bright_save_task, "ui_bright", 3072, (void *)(intptr_t)s_bright,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "创建亮度保存任务失败，本次设置不会持久化");
    }
}

/* 统一入口：夹范围 → 立刻改背光 → 同步设置行和子页两处百分比。
 * 拖动时每格都会进来，所以这里除了写 label 不干别的重活。 */
static void bright_apply(int pct, bool save)
{
    if (pct < BRIGHT_MIN) {
        pct = BRIGHT_MIN;
    } else if (pct > 100) {
        pct = 100;
    }
    s_bright = pct;

    bsp_display_brightness_set(pct);

    if (s_br_pct != NULL) {
        set_txt_fmt(s_br_pct, "%d%%", pct);
    }
    if (s_set_br_val != NULL) {
        set_txt_fmt(s_set_br_val, "%d%%", pct);
    }

    if (save) {
        bright_save_async();
    }
}

/* 拖动中只改背光；松手才落盘，避免一次拖动写几十遍 NVS */
static void bright_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        bright_apply((int)lv_slider_get_value(sl), false);
    } else if (code == LV_EVENT_RELEASED) {
        bright_apply((int)lv_slider_get_value(sl), true);
    }
}

/* 点"亮度"进子页 */
static void bright_open_cb(lv_event_t *e)
{
    (void)e;
    page_show(APP_BRIGHT, true);
}

/* 亮度子页：一张卡 = 左侧太阳 + 中间滑块 + 右侧大号百分比。
 *
 * 滑块几何（LVGL 9）：横向滑块的旋钮直径直接取**对象高度**，
 * 所以对象高 28 → 旋钮 28；再给 PART_MAIN 上下各 8 px 的 pad，
 * 轨道就只剩 12 px 高的一条细槽，看起来才像 iOS 的亮度条。 */
static void build_bright(lv_obj_t *page)
{
    lv_obj_t *c = card(page, 20, CONTENT_Y, 984, 132, 20);

    /* 图标用 40 px 档（24 px 在这张卡里明显偏小）。
     * 用 LEFT_MID 对齐而不是手算 y：字形在行盒里已归一化到中心，
     * 这样换任何字号都不会偏。 */
    lv_obj_t *ic = icon_glyph(c, ICO_BRIGHT, C_SUB, &ui_icon_40);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 20, 0);

    s_br_pct = text(c, "", UI_F_BASE, C_TXT, 0, 0);
    lv_obj_set_width(s_br_pct, 96);
    lv_obj_set_style_text_align(s_br_pct, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(s_br_pct, LV_ALIGN_RIGHT_MID, -28, 0);

    /* 宽度留到 750：旋钮在 100% 时会以右端为中心向外探出 14 px，
     * 再长就会压到右边的百分比文字上。 */
    lv_obj_t *sl = lv_slider_create(c);
    s_br_slider = sl;
    lv_obj_set_pos(sl, 76, 52);
    lv_obj_set_size(sl, 750, 28);
    lv_slider_set_range(sl, BRIGHT_MIN, 100);
    lv_slider_set_value(sl, s_bright, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_RELEASED, NULL);

    /* 轨道：灰细槽。原 0xE0E0E6 在白色卡片上几乎看不出边界，压深一档 */
    lv_obj_set_style_pad_top(sl, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sl, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xD5D5DE), LV_PART_MAIN);

    /* 已填充段：主题蓝 */
    lv_obj_set_style_radius(sl, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_BLUE), LV_PART_INDICATOR);

    /* 旋钮：白色圆片 + 灰色描边 + 一点投影。
     * 纯白圆片落在白色卡片上边界会消失（尤其 100% 时旋钮整个探到卡片留白区），
     * 所以描一圈 2 px 淡灰，再压一层投影把它"垫"起来。 */
    lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, lv_color_hex(0xB9B9C4), LV_PART_KNOB);
    lv_obj_set_style_border_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sl, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(sl, 2, LV_PART_KNOB);

    /* 按住时旋钮胀 4 px，松手复位 */
    lv_obj_set_style_transform_width(sl, 4, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(sl, 4, LV_PART_KNOB | LV_STATE_PRESSED);

    bright_apply(s_bright, false);      /* 把百分比文字补上（背光在进页面前已设过） */
}

/* ---------------- 设置 → WiFi ---------------- */

#define WIFI_ROW_H     64
#define WIFI_GROUP_H   34       /* 分组标题行（"已知网络" / "其他网络"） */
#define WIFI_LIST_H    320      /* 5 行可见，再多靠滚动 */

/* 给 swipe_poll_cb 用（它在文件前面，只能靠前向声明） */
static bool wifi_modal_open(void)
{
    return s_pw_modal != NULL;
}

/* 失败原因的一句话文案。
 *
 * 归类在 net_wifi.c 里做（reason_kind()），这里只负责取字：
 * 界面不该出现 "reason 15" 这种东西，也不该把 60 多个码位逐个翻译。
 * OTHER 才退回通用"连接失败" —— 那种情况用户确实只能看串口日志。 */
static const char *wifi_fail_text(void)
{
    switch (net_wifi_fail_reason()) {
    case NET_WIFI_ERR_PASSWORD:  return i18n_t(W_ERR_PASSWORD);
    case NET_WIFI_ERR_NO_AP:     return i18n_t(W_ERR_NO_AP);
    case NET_WIFI_ERR_REJECTED:  return i18n_t(W_ERR_REJECTED);
    case NET_WIFI_ERR_TIMEOUT:   return i18n_t(W_ERR_TIMEOUT);
    case NET_WIFI_ERR_LINK_LOST: return i18n_t(W_ERR_LINK_LOST);
    default:                     return i18n_t(W_FAILED);
    }
}

/* 设置页那一行右侧的短状态 */
static void wifi_short_state(char *buf, size_t n)
{
    switch (net_wifi_state()) {
    case NET_WIFI_OFF:         snprintf(buf, n, "%s", i18n_t(W_OFF));         break;
    case NET_WIFI_UNAVAILABLE: snprintf(buf, n, "%s", i18n_t(W_UNAVAILABLE)); break;
    case NET_WIFI_CONNECTING:  snprintf(buf, n, "%s", i18n_t(W_CONNECTING));  break;
    case NET_WIFI_CONNECTED:   snprintf(buf, n, "%s", net_wifi_ssid());       break;
    case NET_WIFI_FAILED: {
        /* 刚进 FAILED 的一秒内先按"连接中"显示：切网/重配可能只是过渡态，
         * 钉上"连接失败"再跳回"已连接"很难看。根因在 net_wifi 的
         * CONN_GRACE_MS 里已处理，这里是第二道保险（比如真失败时也不会
         * 让四个字只闪一帧）。 */
        int32_t age = net_wifi_fail_age_s();
        if (age >= 0 && age < 1) {
            snprintf(buf, n, "%s", i18n_t(W_CONNECTING));
        } else if (!net_wifi_retry_pending()) {
            /* 重连预算用尽：只留原因。这一行是设置页的窄行，加后缀会截断；
             * 详细说明在 WiFi 子页（见 wifi_detail_state）。 */
            snprintf(buf, n, "%s", wifi_fail_text());
        } else if (age >= 5) {
            /* 退避重连期间不能只剩一句"正在重试…"：那正是之前那版的问题
             * —— 失败原因一闪而过，之后用户只看到一个永远转不完的状态。
             * 原因留着，后面缀上"正在重试"，两件事一次说完。 */
            snprintf(buf, n, i18n_t(W_RETRY_REASON_FMT), wifi_fail_text());
        } else {
            snprintf(buf, n, "%s", wifi_fail_text());
        }
        break;
    }
    default:                   snprintf(buf, n, "%s", i18n_t(W_NOT_CONNECTED)); break;
    }
}

/* WiFi 子页状态行的长文本：连上时把 SSID / IP / 信号都摆出来，
 * 这三样是现场排障最先要看的东西（能不能上网、信号够不够）。 */
static void wifi_detail_state(char *buf, size_t n)
{
    net_wifi_state_t st = net_wifi_state();

    switch (st) {
    case NET_WIFI_OFF:
        snprintf(buf, n, "%s", i18n_t(W_OFF));
        break;
    case NET_WIFI_UNAVAILABLE:
        snprintf(buf, n, "%s", i18n_t(W_UNAVAILABLE));
        break;
    case NET_WIFI_CONNECTED: {
        /* 分隔符必须按语言切：
         *   中文走 ui_font_zh_16，字库里补了 U+00B7（见 gen_fonts.py 的
         *   --range 0xB7），中点正常；
         *   英文走 ui_font_en_14（SF Pro Display 的 ASCII 子集），只有
         *   0x20-0x7E，U+00B7 会被渲染成方块（用户反馈"英文界面下那个
         *   也是方块"）。原来用 Montserrat 时同理。
         * 不去给英文字库加 fallback：两套字库行高与字宽不同，
         * 混排会让整行基线抖动，代价远大于换个分隔符。 */
        const char *sep = (i18n_lang() == LANG_ZH) ? " · " : "  |  ";

        /* RSSI 是连上之后每 5 s 才刷一次的（RPC 不便宜），
         * 刚拿到 IP 的那几秒还是无效值 -127；这时候别把 -127 dBm 甩给用户，
         * 只显示 SSID + IP。 */
        int8_t r = net_wifi_rssi();
        if (r <= -127) {
            snprintf(buf, n, "%s%s%s", net_wifi_ssid(), sep, net_wifi_ip());
        } else {
            snprintf(buf, n, "%s%s%s%s%d dBm",
                     net_wifi_ssid(), sep, net_wifi_ip(), sep, (int)r);
        }
        break;
    }
    case NET_WIFI_CONNECTING:
        snprintf(buf, n, "%s %s", i18n_t(W_CONNECTING), net_wifi_ssid());
        break;
    case NET_WIFI_FAILED: {
        /* 同 wifi_short_state：第一秒按"连接中"显示，别闪失败。
         * 之后 1~5 s 显示"原因: SSID"（还能看出在连谁）；
         * 退避重连阶段只留原因 + 正在重试，行宽才够，也不会把
         * SSID 顶到屏幕外。 */
        int32_t age = net_wifi_fail_age_s();
        if (age >= 0 && age < 1) {
            snprintf(buf, n, "%s %s", i18n_t(W_CONNECTING), net_wifi_ssid());
        } else if (!net_wifi_retry_pending()) {
            /* 重连用完了：把"停了"说出来，否则用户会一直等它自己好。
             * 行的宽度够（712 px），"原因 + 已停止重试"一行放得下。 */
            snprintf(buf, n, i18n_t(W_ERR_STOPPED_FMT), wifi_fail_text());
        } else if (age >= 5) {
            snprintf(buf, n, i18n_t(W_RETRY_REASON_FMT), wifi_fail_text());
        } else {
            snprintf(buf, n, "%s: %s", wifi_fail_text(), net_wifi_ssid());
        }
        break;
    }
    default:
        snprintf(buf, n, "%s", i18n_t(W_NOT_CONNECTED));
        break;
    }
}

/* 信号强度分 4 档。原来只用图标颜色深浅区分（深灰 / 中灰 / 浅灰），
 * 实测这块屏上三种灰几乎分不出来，"信号好不好"完全看不出 —— 改成
 * "信号格 + dBm 数字"：格数给一眼的强弱，数字给现场排障要的具体值。 */
static int wifi_sig_level(int8_t rssi)
{
    if (rssi >= -55) {
        return 4;                   /* 满格：贴着路由 */
    }
    if (rssi >= -67) {
        return 3;
    }
    if (rssi >= -78) {
        return 2;
    }
    return 1;                       /* 一格：勉强能扫到 */
}

static uint32_t wifi_sig_color(int8_t rssi)
{
    switch (wifi_sig_level(rssi)) {
    case 4:  return C_TXT;
    case 3:  return 0x5A5A66;
    case 2:  return C_SUB;
    default: return 0xA8A8B2;
    }
}

/* 四段竖条，底边对齐：亮起的格数 = 档位；没亮的那几格用浅灰画出来
 * （不画的话"1 格"和"根本没画图标"分不清）。 */
static void wifi_sig_bars(lv_obj_t *row, int8_t rssi)
{
    static const int8_t hgt[4] = { 8, 12, 16, 20 };
    const int lvl  = wifi_sig_level(rssi);
    const uint32_t on = wifi_sig_color(rssi);
    const int base = (WIFI_ROW_H - 22) / 2;      /* 22 = 最高那格，整体垂直居中 */

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = plain(row);
        lv_obj_set_size(b, 5, hgt[i]);
        lv_obj_set_pos(b, 2 + i * 8, base + (22 - hgt[i]));
        lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < lvl ? on : 0xE2E2E8), LV_PART_MAIN);
    }
}

/* 点击某一行的动作分三种：已连接 → 忽略；存过密码 → 直接连；
 * 其余加密网络 → 弹密码框。开放网络直接连。 */

/* 连接一定要走独立任务：net_wifi_connect() 写 NVS（nvs_commit 要几百字节栈），
 * 在 LVGL 任务里直接做有溢栈风险 —— 亮度和语言当初就是因为这个改成异步的。 */
typedef struct {
    char ssid[NET_WIFI_SSID_MAX];
    char pass[NET_WIFI_PASS_MAX];
    bool use_saved;             /* true = 用已存密码（pass 忽略） */
} wifi_cred_t;

static void open_password(const char *ssid);

static void wifi_connect_task(void *arg)
{
    wifi_cred_t *c = (wifi_cred_t *)arg;

    if (c->use_saved) {
        net_wifi_connect_saved(c->ssid);
    } else {
        net_wifi_connect(c->ssid, c->pass, true);
    }

    free(c);
    vTaskDelete(NULL);
}

static bool wifi_connect_async(const char *ssid, const char *pass, bool use_saved)
{
    wifi_cred_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return false;
    }

    strncpy(c->ssid, ssid, NET_WIFI_SSID_MAX - 1);
    if (pass != NULL) {
        strncpy(c->pass, pass, NET_WIFI_PASS_MAX - 1);
    }
    c->use_saved = use_saved;

    if (xTaskCreate(wifi_connect_task, "wifi_conn", 4096, c, 4, NULL) != pdPASS) {
        free(c);
        return false;
    }
    return true;
}

static void wifi_row_cb(lv_event_t *e)
{
    if (s_row_long) {
        s_row_long = false;         /* 长按弹框后松开的那次 CLICKED，吞掉 */
        return;
    }

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_w_apn) {
        return;
    }

    const net_wifi_ap_t *a = &s_w_aps[idx];
    ESP_LOGI(TAG, "选中网络 \"%s\"（rssi=%d auth=%u saved=%d）",
             a->ssid, (int)a->rssi, (unsigned)a->authmode, (int)a->saved);

    if (net_wifi_state() == NET_WIFI_CONNECTED &&
        strncmp(a->ssid, net_wifi_ssid(), NET_WIFI_SSID_MAX - 1) == 0) {
        return;                     /* 已经连着它了 */
    }

    if (a->saved) {
        /* 上一次就是拿这个网络的"已存密码"连失败 → 存的那份很可能不对
         * （密码改过、或者是从旧固件带进来的脏数据）。此时再走
         * connect_saved 只会用同一份错密码白试一轮，用户会觉得
         * "我密码明明是对的、怎么点都失败"。
         * 改成弹密码框：密文已预填（见 open_password），
         * 想直接重试就按"连接"，想改密码就改——两条路都留着。 */
        if (net_wifi_state() == NET_WIFI_FAILED &&
            strncmp(a->ssid, net_wifi_ssid(), NET_WIFI_SSID_MAX - 1) == 0) {
            ESP_LOGI(TAG, "已存密码连接失败，改为重新输入：\"%s\"", a->ssid);
            open_password(a->ssid);
            return;
        }
        wifi_connect_async(a->ssid, NULL, true);   /* 走已存密码 */
        return;
    }
    if (a->authmode == 0) {                        /* WIFI_AUTH_OPEN */
        wifi_connect_async(a->ssid, "", false);
        return;
    }
    open_password(a->ssid);
}

static void wifi_scan_cb(lv_event_t *e)
{
    (void)e;
    net_wifi_scan_start();
}

/* "断开"：不只是断当前连接，同时清掉 s_want，让重连退避停下来。
 * 用户连错网络、或者不想让它开机自动连回去时，这是唯一的出口。
 * 仍走一次性任务：esp_wifi_disconnect() 是 RPC，别在 LVGL 任务里同步等。 */
static void wifi_disconnect_task(void *arg)
{
    (void)arg;
    net_wifi_disconnect();
    vTaskDelete(NULL);
}

static void wifi_disconnect_cb(lv_event_t *e)
{
    (void)e;
    if (xTaskCreate(wifi_disconnect_task, "wifi_dis", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "断开任务创建失败");
    }
}

/* 开关回调：net_wifi_set_enabled() 会写 NVS（nvs_commit 要几百字节栈），
 * 不能直接在 LVGL 任务里做，扔给一次性任务。 */
static void wifi_enable_task(void *arg)
{
    net_wifi_set_enabled((bool)(intptr_t)arg);
    vTaskDelete(NULL);
}

static void wifi_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_sw_touch_ms = esp_timer_get_time() / 1000;

    if (xTaskCreate(wifi_enable_task, "wifi_en", 3072, (void *)(intptr_t)on, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "开关任务创建失败");
        return;
    }
    if (on) {
        /* 开关一打开就搜一次。此刻驱动大概率还没起来（C6 走一遍 SDIO 握手要几秒），
         * net_wifi_scan_start() 会把这个请求挂起，等 STA_START 之后由 wifi 任务
         * 自动补扫 —— 见 net_wifi.c 的 s_scan_wanted。 */
        net_wifi_scan_start();
    }
}

/* 点"WiFi"进子页 */
static void wifi_fill_list(void);

static void wifi_open_cb(lv_event_t *e)
{
    (void)e;
    page_show(APP_WIFI, true);
    wifi_fill_list();               /* 先把旧结果摆上，别让列表空 400 ms */
    if (net_wifi_enabled()) {
        net_wifi_scan_start();      /* 进页面就刷一次，别让用户先按"扫描" */
    }
}

/* ---- 密码浮层 ---- */

static void close_password(void)
{
    if (s_pw_modal != NULL) {
        lv_obj_delete(s_pw_modal);      /* 子对象（键盘、文本框）一起删 */
        s_pw_modal = NULL;
        s_pw_ta    = NULL;
        s_pw_kb    = NULL;
    }
}

static void pw_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_password();
}

static void pw_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_pw_ta == NULL) {
        return;
    }
    const char *pass = lv_textarea_get_text(s_pw_ta);
    /* 空密码也放行：有的是"开放但广播了加密类型"的怪 AP，
     * 真连不上会走失败重试，用户能看到结果 */
    wifi_connect_async(s_pw_ssid, pass, false);
    close_password();
}

static void pw_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {           /* 键盘右下角的勾 */
        pw_connect_cb(e);
    } else if (code == LV_EVENT_CANCEL) {   /* 键盘左上角的收起 */
        close_password();
    }
}

/* 点一下输入框 → 保证键盘可见并重新绑定。
 * LVGL 的键盘不会"跟着输入框自动出现"（只有按下特定键时才发 READY/CANCEL，
 * 而且默认示例是自己在获得焦点时 remove_flag(HIDDEN)）。这里显式兜底，
 * 免得出现"输入框有焦点、键盘不见了"的死局。 */
static void pw_ta_cb(lv_event_t *e)
{
    (void)e;
    if (s_pw_kb == NULL || s_pw_ta == NULL) {
        return;
    }
    lv_keyboard_set_textarea(s_pw_kb, s_pw_ta);
    lv_obj_remove_flag(s_pw_kb, LV_OBJ_FLAG_HIDDEN);
}

/* 造一个文字按钮（浮层用，尺寸和设置页的行不是一套） */
static lv_obj_t *pw_button(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *label, uint32_t bg, uint32_t fg,
                           lv_event_cb_t cb)
{
    lv_obj_t *b = pill(parent, x, y, w, h, bg, LV_OPA_COVER);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), LV_PART_MAIN);

    lv_obj_t *t = mk_label(b);
    lv_label_set_text(t, label);
    lv_obj_set_style_text_font(t, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_center(t);
    return b;
}

static void open_password(const char *ssid)
{
    if (s_pw_modal != NULL) {
        close_password();
    }
    strncpy(s_pw_ssid, ssid, NET_WIFI_SSID_MAX - 1);
    s_pw_ssid[NET_WIFI_SSID_MAX - 1] = '\0';

    /* 遮罩挂在 lv_layer_top() 上：这一层永远压在所有页面之上，不受页面的
     * 入场动画 / 隐藏标志 / 裁剪影响。设成可点是为了把点击吃掉，别透到下面。 */
    lv_obj_t *modal = plain(lv_layer_top());
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_size(modal, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(modal, LV_OPA_40, LV_PART_MAIN);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    s_pw_modal = modal;

    /* 卡片 210 高 + 键盘 300 高，加起来 40 + 210 + 12 + 300 = 562，
     * 留在 600 的屏里还有 38 px 余量，不会压到屏幕边缘。 */
    lv_obj_t *c = card(modal, 32, 40, 960, 210, 24);

    lv_obj_t *t = text(c, s_pw_ssid, UI_F_BASE, C_TXT, 28, 14);
    lv_obj_set_width(t, 900);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

    text(c, i18n_t(W_PASSWORD), UI_F_SMALL, C_SUB, 28, 48);

    s_pw_ta = lv_textarea_create(c);
    lv_obj_set_pos(s_pw_ta, 28, 74);
    lv_obj_set_size(s_pw_ta, 620, 64);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, NET_WIFI_PASS_MAX - 1);
    lv_textarea_set_placeholder_text(s_pw_ta, "********");
    /* 已存的密码预填进去（掩码显示，屏上看不见明文）：
     * 这个框只在两种情况下弹 —— 第一次给新网络输密码（预填为空串，无影响），
     * 或者已存密码连失败（预填的就是失败的那份）。后者让用户能
     * "直接按连接重试"或"改掉错的那几位"，不必把整串重新敲一遍。 */
    if (net_wifi_state() == NET_WIFI_FAILED &&
        strncmp(ssid, net_wifi_ssid(), NET_WIFI_SSID_MAX - 1) == 0) {
        lv_textarea_set_text(s_pw_ta, net_wifi_pass());
    }
    lv_obj_set_style_text_font(s_pw_ta, UI_F_BASE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pw_ta, lv_color_hex(0xF4F4F8), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pw_ta, lv_color_hex(C_BLUE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_pw_ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_pw_ta, 12, LV_PART_MAIN);

    pw_button(c, 688, 74, 108, 64, i18n_t(W_CANCEL), C_CARD, C_BLUE, pw_cancel_cb);
    pw_button(c, 808, 74, 124, 64, i18n_t(W_CONNECT), C_BLUE, 0xFFFFFF, pw_connect_cb);

    /* 软键盘：和卡片同宽，贴在卡片下方。300 px / 4 行 = 每行 75 px，
     * 手指按得准；之前 210 px 时每行只有 52 px，本来就偏小。
     *
     * 注意用 lv_obj_align(..., LV_ALIGN_DEFAULT, x, y) 而不是 lv_obj_set_pos：
     * lv_keyboard 的构造函数里有 lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0)，
     * 会把对齐方式存成样式；只要 align 不是 DEFAULT，lv_obj_refr_pos() 就按
     * 对齐算坐标、完全忽略 set_pos 写进去的 x/y —— 位置会被"底部居中"吃掉。 */
    lv_obj_t *kb = lv_keyboard_create(modal);
    lv_obj_set_size(kb, 960, 300);
    lv_obj_align(kb, LV_ALIGN_DEFAULT, 32, 262);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_pw_ta);

    /* 这三句是"点了输入框没键盘"的直接对症处理：
     *   remove_flag(HIDDEN) —— 键盘控件默认可能带隐藏标志（LVGL 文档的例子
     *                          里就是自己 add_flag(HIDDEN) 再在获得焦点时放开），
     *                          我们不玩"按需弹出"，建出来就必须是可见的；
     *   move_to_index(-1)   —— 保证键盘压在卡片之后的 top 位置，不被任何
     *                          后建的子对象盖住；
     *   remove_flag(IGNORE_LAYOUT) 不需要，位置是显式 set_pos 算好的。 */
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(kb, -1);
    s_pw_kb = kb;

    lv_obj_add_event_cb(kb, pw_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, pw_kb_cb, LV_EVENT_CANCEL, NULL);

    /* 键盘样式跟着白色卡片走，默认主题偏灰，放在浅色页面上像挖了个洞 */
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(kb, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_radius(kb, 18, LV_PART_MAIN);

    /* 输入框一被点就把键盘放出来（有些情况下键盘会自己消失，
     * 比如按过键、或别处改过标志），这里兜一道底。 */
    lv_obj_add_event_cb(s_pw_ta, pw_ta_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_pw_ta, pw_ta_cb, LV_EVENT_FOCUSED, NULL);

    ESP_LOGI(TAG, "密码浮层：ssid=\"%s\" ta=%p kb=%p", s_pw_ssid,
             (void *)s_pw_ta, (void *)s_pw_kb);
}

/* ---- 忘记某个已知网络（长按列表行） ----
 *
 * 为什么要有这个入口：以前只有串口 `wifi forget` 能清，而且一次清整表。
 * 一条脏记录（比如旧固件在"下发配置就落表"时代写进去的错密码网络）
 * 会一直卡在 Known networks 里，用户既删不掉、点它又只会用错密码白试。
 *
 * 交互：长按已知网络行 → 确认框 → 忘了这一条（连同密码）。 */
static lv_obj_t *s_fg_modal = NULL;
static char      s_fg_ssid[NET_WIFI_SSID_MAX];
static volatile bool s_fg_busy = false;   /* 后台任务正在删，连点只算一次 */

static void close_forget(void)
{
    if (s_fg_modal != NULL) {
        lv_obj_delete(s_fg_modal);
        s_fg_modal = NULL;
    }
}

static void forget_task(void *arg)
{
    char *ssid = (char *)arg;
    net_wifi_forget_one(ssid);
    free(ssid);
    s_fg_busy = false;
    vTaskDelete(NULL);
}

static void fg_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_forget();
}

static void fg_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_fg_busy) {
        return;
    }
    s_fg_busy = true;

    /* 先关框再干活：删除要断开连接 + 写 NVS，都是慢操作，
     * 不能卡在 LVGL 任务里 */
    char *ssid = strdup(s_fg_ssid);
    close_forget();
    if (ssid == NULL || xTaskCreate(forget_task, "wifi_fg", 5120, ssid, 4, NULL) != pdPASS) {
        free(ssid);
        s_fg_busy = false;
        ESP_LOGW(TAG, "忘记网络任务创建失败");
    }
}

static void open_forget_confirm(const char *ssid)
{
    if (s_fg_modal != NULL) {
        close_forget();
    }
    strncpy(s_fg_ssid, ssid, NET_WIFI_SSID_MAX - 1);
    s_fg_ssid[NET_WIFI_SSID_MAX - 1] = '\0';

    lv_obj_t *modal = plain(lv_layer_top());
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_size(modal, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(modal, LV_OPA_40, LV_PART_MAIN);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    s_fg_modal = modal;

    /* 520×180 居中：比密码框窄，因为它只有一句话和两个按钮 */
    lv_obj_t *c = card(modal, 252, 210, 520, 180, 24);

    text(c, i18n_t(W_FORGET_TITLE), UI_F_BASE, C_TXT, 28, 24);

    /* SSID 单独一行：确认框里必须能看清"到底要删哪个" */
    lv_obj_t *nm = text(c, s_fg_ssid, UI_F_SMALL, C_SUB, 28, 62);
    lv_obj_set_width(nm, 464);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);

    pw_button(c, 248, 108, 116, 48, i18n_t(W_CANCEL), C_CARD, C_BLUE, fg_cancel_cb);
    /* 危险操作用红字白底，和"取消"区分开 */
    pw_button(c, 372, 108, 116, 48, i18n_t(W_FORGET), C_CARD, 0xD93025, fg_ok_cb);
}

static void wifi_row_long_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_w_apn) {
        return;
    }

    const net_wifi_ap_t *a = &s_w_aps[idx];
    if (!a->saved) {
        return;                 /* 没存过密码的网络没什么可忘记的 */
    }

    ESP_LOGI(TAG, "长按已知网络 \"%s\"，弹忘记确认", a->ssid);
    s_row_long = true;
    open_forget_confirm(a->ssid);
}

/* ---- 列表 ---- */

/* 分组标题：全大写小字 + 灰色，尺寸对齐 iOS 的"Known Networks"那种分隔条。
 * 高度由调用方按 WIFI_GROUP_H 记账。 */
static void wifi_group_title(lv_obj_t *parent, int y, i18n_str_t s)
{
    lv_obj_t *t = text(parent, i18n_t(s), UI_F_SMALL, C_SUB, 4, y + 10);
    lv_obj_set_width(t, 900);
}

/* 画一行网络。y 是列表内的绝对位置 —— 分组标题占掉一段之后，
 * 行号乘行高就不对了，所以位置必须由调用方累加着传进来。
 * idx 是 s_w_aps 的下标，点击回调直接用它取数据。 */
static void wifi_list_row(lv_obj_t *parent, int y, int idx, bool connected)
{
    const net_wifi_ap_t *a = &s_w_aps[idx];

    lv_obj_t *r = plain(parent);
    lv_obj_set_pos(r, 0, y);
    lv_obj_set_size(r, 936, WIFI_ROW_H);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, wifi_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    /* 长按已知网络 = 忘记它（见 open_forget_confirm）。
     * 未保存的行在回调里自己 return，不额外判断。 */
    lv_obj_add_event_cb(r, wifi_row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    lv_obj_set_style_radius(r, 12, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, lv_color_hex(0xE9E9EE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);

    wifi_sig_bars(r, a->rssi);                     /* 左侧四段信号格 */

    lv_obj_t *nm = text(r, a->ssid, UI_F_BASE, C_TXT, 0, 0);
    lv_obj_set_width(nm, 520);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 42, 0);   /* 定宽之后再对齐，否则按文本宽度算 */

    /* 右侧先摆 dBm 数字（固定贴最右，宽度随机变化），
     * 后面的"已连接 / 已保存 / 开放"标记再从它左边往外排 —— 这样
     * SSID 长短、标记有无都不会让 dBm 的位置左右跳。 */
    char dbm[16];
    snprintf(dbm, sizeof(dbm), "%d dBm", (int)a->rssi);
    lv_obj_t *dv = text(r, dbm, UI_F_SMALL, wifi_sig_color(a->rssi), 0, 0);
    lv_obj_align(dv, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 右侧标记，优先级：已连接 > 已保存 > 开放网络 > （加密，不标） */
    const char *tag = NULL;
    uint32_t    tag_c = C_SUB;
    if (connected && strncmp(a->ssid, net_wifi_ssid(), NET_WIFI_SSID_MAX - 1) == 0) {
        tag   = i18n_t(W_CONNECTED);
        tag_c = C_BLUE;
    } else if (a->saved) {
        tag = i18n_t(W_SAVED);
    } else if (a->authmode == 0) {
        tag = i18n_t(W_OPEN);
    }

    if (tag != NULL) {
        lv_obj_t *tv = text(r, tag, UI_F_SMALL, tag_c, 0, 0);
        lv_obj_align_to(tv, dv, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    }
}

/* 重建列表：只在"条数 / 连接状态 / 是否在扫描"变了时才做
 * （每 400 ms 重建一次列表会让整页持续重绘，就是之前主页闪的老毛病）。 */
static void wifi_fill_list(void)
{
    if (s_w_list == NULL) {
        return;
    }

    lv_obj_clean(s_w_list);

    s_w_apn = net_wifi_scan_get(s_w_aps, NET_WIFI_SCAN_MAX);

    if (s_w_apn <= 0) {
        lv_obj_t *empty = mk_label(s_w_list);
        lv_label_set_text(empty, i18n_t(net_wifi_scanning() ? W_SCANNING : W_NO_AP));
        lv_obj_set_style_text_font(empty, UI_F_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, lv_color_hex(C_SUB), LV_PART_MAIN);
        lv_obj_set_width(empty, 936);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(empty, 0, 120);

        /* 空列表也要更新这三个值：否则 refresh_cb 每次都认为"还没画过"，
         * 每 400 ms 重建一次空标签 —— 屏幕上就是一行字不停闪。 */
        s_w_drawn_apn  = s_w_apn;
        s_w_drawn_st   = (int)net_wifi_state();
        s_w_drawn_scan = net_wifi_scanning();
        s_w_drawn_sv   = net_wifi_saved_count();
        return;
    }

    bool connected = (net_wifi_state() == NET_WIFI_CONNECTED);

    /* 就地刷新 saved 标记：刚连上的那一刻凭据才进表，而 s_aps 里的标记
     * 还是上次扫描时算的。不刷新的话要等下一轮扫描它才会挪到"已知网络"。
     * 两趟遍历分别画两组，组内顺序沿用 net_wifi 给的（信号降序）。 */
    int saved_n = 0;
    for (int k = 0; k < s_w_apn; k++) {
        s_w_aps[k].saved = net_wifi_is_saved(s_w_aps[k].ssid);
        if (s_w_aps[k].saved) {
            saved_n++;
        }
    }

    int y = 0;

    if (saved_n > 0) {
        wifi_group_title(s_w_list, y, W_KNOWN);
        y += WIFI_GROUP_H;

        int left = saved_n;
        for (int i = 0; i < s_w_apn; i++) {
            if (!s_w_aps[i].saved) {
                continue;
            }
            wifi_list_row(s_w_list, y, i, connected);
            if (--left > 0) {
                row_sep(s_w_list, y + WIFI_ROW_H - 1, 852, 44);
            }
            y += WIFI_ROW_H;
        }
    }

    if (saved_n < s_w_apn) {
        wifi_group_title(s_w_list, y, W_OTHER);
        y += WIFI_GROUP_H;

        int left = s_w_apn - saved_n;
        for (int i = 0; i < s_w_apn; i++) {
            if (s_w_aps[i].saved) {
                continue;
            }
            wifi_list_row(s_w_list, y, i, connected);
            if (--left > 0) {
                row_sep(s_w_list, y + WIFI_ROW_H - 1, 852, 44);
            }
            y += WIFI_ROW_H;
        }
    }

    s_w_drawn_apn  = s_w_apn;
    s_w_drawn_st   = (int)net_wifi_state();
    s_w_drawn_scan = net_wifi_scanning();
    s_w_drawn_sv   = net_wifi_saved_count();
}

static void build_wifi(lv_obj_t *page)
{
    /* ---- 开关 ---- */
    lv_obj_t *c = card(page, 20, CONTENT_Y, 984, 76, 20);

    lv_obj_t *ic = icon_glyph(c, ICO_WIFI, C_BLUE, &ui_icon_24);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *t = text(c, i18n_t(W_WIFI), UI_F_BASE, C_TXT, 0, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 66, 0);

    s_w_sw = lv_switch_create(c);
    lv_obj_set_size(s_w_sw, 64, 34);
    lv_obj_align(s_w_sw, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_set_style_bg_color(s_w_sw, lv_color_hex(0xD5D5DE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_w_sw, lv_color_hex(C_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_w_sw, LV_OPA_COVER, LV_PART_MAIN);
    if (net_wifi_enabled()) {
        lv_obj_add_state(s_w_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_w_sw, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 状态行 + 断开 + 扫描 ---- */
    s_w_state = text(page, "", UI_F_SMALL, C_SUB, 32, 176);
    lv_obj_set_width(s_w_state, 712);
    lv_label_set_long_mode(s_w_state, LV_LABEL_LONG_MODE_DOTS);

    /* 断开：连错网络、或不想让它按退避一直重连时的出口。
     * 只在"已连 / 连接中 / 失败重试中"出现（见 refresh 里的显隐逻辑），
     * 关闭和空闲时藏起来 —— 常驻一个点了没反应的按钮比不显示更让人困惑。 */
    s_w_disc = pill(page, 756, 168, 116, 36, C_CARD, LV_OPA_COVER);
    lv_obj_set_style_border_width(s_w_disc, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_w_disc, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_add_flag(s_w_disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_w_disc, wifi_disconnect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dl = mk_label(s_w_disc);
    lv_label_set_text(dl, i18n_t(W_DISCONNECT));
    lv_obj_set_style_text_font(dl, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(dl, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_center(dl);

    lv_obj_t *btn = pill(page, 884, 168, 120, 36, C_CARD, LV_OPA_COVER);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, wifi_scan_cb, LV_EVENT_CLICKED, NULL);

    s_w_scan_lbl = mk_label(btn);
    lv_label_set_text(s_w_scan_lbl, i18n_t(W_SCAN));
    lv_obj_set_style_text_font(s_w_scan_lbl, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_w_scan_lbl, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_center(s_w_scan_lbl);

    /* ---- 网络列表 ---- */
    text(page, i18n_t(W_AVAILABLE), UI_F_SMALL, C_SUB, 32, 216);

    lv_obj_t *box = card(page, 20, 240, 984, 336, 20);

    s_w_list = plain(box);
    lv_obj_set_pos(s_w_list, 24, 8);
    lv_obj_set_size(s_w_list, 936, WIFI_LIST_H);
    lv_obj_add_flag(s_w_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_w_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_w_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_w_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_w_list, LV_OPA_TRANSP, LV_PART_MAIN);
}

/* 点"语言"进子页。page_show 会压栈，返回时回设置页而不是桌面。 */
static void lang_open_cb(lv_event_t *e)
{
    (void)e;
    page_show(APP_LANG, true);
}

/* 语言子页里的一行：选中 = 蓝底淡蓝 + 实心圆点，未选中 = 空心圈。
 * 和设置页的 set_row 同一套尺寸，切页时视觉不跳。 */
static void lang_row(lv_obj_t *parent, int y, int w, const char *ico, i18n_str_t label,
                     ui_lang_t lang)
{
    bool sel = (i18n_lang() == lang);

    lv_obj_t *r = plain(parent);
    lv_obj_set_pos(r, 24, y);
    lv_obj_set_size(r, w, 64);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, lang_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)lang);
    lv_obj_set_style_radius(r, 12, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, lv_color_hex(0xE9E9EE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *ic = icon_glyph(r, ico, sel ? C_BLUE : C_SUB, &ui_icon_24);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *t = text(r, i18n_t(label), UI_F_BASE, sel ? C_BLUE : C_TXT, 0, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 42, 0);

    /* 右侧圆点：选中 = 实心蓝点，未选中 = 空心灰圈 */
    lv_obj_t *dot = plain(r);
    lv_obj_set_size(dot, 22, 22);
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(sel ? C_BLUE : C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(dot, lv_color_hex(sel ? C_BLUE : C_LINE), LV_PART_MAIN);
}

/* 设置主页：手机式的竖向单列。分组标题在卡片外（iOS 的做法），
 * 每个分组 = 一行灰色小标题 + 一张白卡，卡片高度按行数算：8 + 64*n。
 * 现在"通用"里两行：亮度、语言。以后加"网络 / 升级默认值"就是再起一组。 */

/* 亮度图标 e698 的字形画幅是 18×18，语言 e654 是 22×22 —— 同字号下看着小一圈。
 * 按 22/18 ≈ 1.22 放大（LVGL 的 scale 以 256 为 1.0）。 */
#define ICO_SCALE_BRIGHT   312

static void build_settings(lv_obj_t *page)
{
    int y = CONTENT_Y;
    char cur[32];

    /* ---- 通用：亮度 + 语言，两行一张卡（8 + 64×2 + 8 = 144） ---- */
    text(page, i18n_t(SET_GENERAL), UI_F_SMALL, C_SUB, 32, y);
    y += 24;

    lv_obj_t *g = card(page, 20, y, 984, 144, 20);

    snprintf(cur, sizeof(cur), "%d%%", s_bright);
    /* 记下数值 label：在亮度子页拖滑块时直接改这里，返回设置页就是新值 */
    s_set_br_val = set_row(g, 8, 936, ICO_BRIGHT, i18n_t(SET_BRIGHTNESS), cur,
                           bright_open_cb, NULL, true, ICO_SCALE_BRIGHT);

    row_sep(g, 72, 894, 66);        /* 与 iOS 一致：从图标右侧起，不到卡片边 */

    snprintf(cur, sizeof(cur), "%s",
             i18n_t(i18n_lang() == LANG_ZH ? SET_LANG_ZH : SET_LANG_EN));
    set_row(g, 72, 936, ICO_LANG, i18n_t(SET_LANGUAGE), cur, lang_open_cb, NULL, true, 0);
    y += 144 + 20;

    /* ---- 网络：WiFi 一行（8 + 64 + 8 = 80） ---- */
    text(page, i18n_t(SET_NETWORK), UI_F_SMALL, C_SUB, 32, y);
    y += 24;

    lv_obj_t *n = card(page, 20, y, 984, 80, 20);
    wifi_short_state(cur, sizeof(cur));
    s_set_wifi_val = set_row(n, 8, 936, ICO_WIFI, i18n_t(W_WIFI), cur,
                             wifi_open_cb, NULL, true, 0);
    y += 80 + 20;

    /* ---- 关于 ---- */
    text(page, i18n_t(SET_ABOUT), UI_F_SMALL, C_SUB, 32, y);
    y += 24;

    /* 名字改长（Automatic Firmware Update Tool）之后这张卡压到 76：
     * 名字 20 号 + 版本 14 号，上下各留 10 px 就够，省下的高度留给下面的说明。 */
    lv_obj_t *a = card(page, 20, y, 984, 76, 20);
    text(a, APP_NAME, UI_F_BASE, C_TXT, 24, 10);
    text(a, APP_VERSION, UI_F_SMALL, C_SUB, 24, 44);
    y += 76 + 16;

    /* ---- 说明 ---- */
    lv_obj_t *h = card(page, 20, y, 984, 64, 20);
    lv_obj_t *hint = mk_label(h);
    lv_label_set_text(hint, i18n_t(SET_ABOUT_HINT));
    lv_obj_set_style_text_font(hint, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_SUB), LV_PART_MAIN);
    lv_obj_set_width(hint, 936);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(hint, 24, 14);
}

/* 语言子页：单列宽卡，两行选项 */
static void build_lang(lv_obj_t *page)
{
    lv_obj_t *c = card(page, 20, CONTENT_Y, 984, 176, 20);

    lang_row(c, 24, 936, ICO_LANG_ZH, SET_LANG_ZH, LANG_ZH);
    row_sep(c, 88, 894, 66);       /* 分隔线从图标右侧起，止于行右端 */
    lang_row(c, 92, 936, ICO_LANG_EN, SET_LANG_EN, LANG_EN);
}

/* ---------------- 刷新 ---------------- */

/* OTA 状态 → 界面文字。中英文都在 i18n 表里，这里只做状态到 ID 的映射。 */
static const char *state_txt(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_IDLE:    return i18n_t(ST_IDLE);
    case OTA_STATE_CONNECT: return i18n_t(ST_CONNECT);
    case OTA_STATE_BEGIN:   return i18n_t(ST_HANDSHAKE);
    case OTA_STATE_STREAM:  return i18n_t(ST_STREAMING);
    case OTA_STATE_END:     return i18n_t(ST_VERIFYING);
    case OTA_STATE_REBOOT:  return i18n_t(ST_REBOOTING);
    case OTA_STATE_VERIFY:  return i18n_t(ST_WAIT_ONLINE);
    case OTA_STATE_DONE:    return i18n_t(ST_SUCCESS);
    case OTA_STATE_FAILED:  return i18n_t(ST_FAILED);
    default:                return "?";
    }
}

static uint32_t state_color(ota_state_t s)
{
    if (s == OTA_STATE_DONE) {
        return C_GREEN;
    }
    if (s == OTA_STATE_FAILED) {
        return C_RED;
    }
    if (s == OTA_STATE_IDLE) {
        return C_SUB;
    }
    return C_BLUE;
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    /* 刷新统计：只在"明显超过正常刷新量"时才打一行，不破坏
     * "日志只在异常/连接变化时输出"的约定。
     *
     * 阈值 30 次/10 s ≈ 平均 3 fps。桌面 CPU 折线撤掉之后，静止的主页
     * 正常应该接近 0 次；一旦稳定超过这个数，基本就是有对象在被反复
     * invalidate，配合后面的区域坐标能直接定位。 */
    static int64_t s_flush_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (s_flush_log_us == 0) {
        s_flush_log_us = now_us;
    } else if (now_us - s_flush_log_us >= 10000000) {
        if (s_flush_cnt >= 30) {
            ESP_LOGI(TAG, "10 s 内刷新 %u 次，最近区域 (%d,%d)-(%d,%d)",
                     (unsigned)s_flush_cnt, (int)s_flush_x1, (int)s_flush_y1,
                     (int)s_flush_x2, (int)s_flush_y2);
        }
        s_flush_cnt = 0;
        s_flush_log_us = now_us;
    }

    /* ---- 公共数据 ---- */
    bool link = net_eth_link_up();
    esp_netif_ip_info_t ipinfo = {0};
    esp_netif_t *nif = net_eth_netif();
    if (nif != NULL) {
        esp_netif_get_ip_info(nif, &ipinfo);
    }

    ota_status_t st;
    ota_client_get_status(&st);

    unsigned pct = 0;
    if (st.total > 0) {
        pct = (unsigned)(((uint64_t)st.acked * 100u) / st.total);
        if (pct > 100) {
            pct = 100;
        }
    }

    /* ---- 顶部状态栏（全局，所有页面都显示） ----
     * 现在只有两个网络接口图标，没有文字要更新。 */
    if (s_sb != NULL) {
        /* 壁纸改成淡色后，桌面和应用页都用深色字；dark_ui 只留作"配色变过"的判据 */
        bool dark_ui = (s_cur == APP_HOME);
        uint32_t fg = C_TXT;

        /* 网线图标只在"插拔"或"换页面配色"时重画，避免每 400 ms 刷新一次 */
        int key = (link ? 1 : 0) | (dark_ui ? 2 : 0);
        if (key != s_lan_key) {
            s_lan_key = key;
            lan_icon_update(link, fg);
        }

        /* WiFi 图标同理，判重放在 wifi_icon_update() 内部 */
        wifi_icon_update();
    }

    /* ---- WiFi：设置页那一行 + WiFi 子页 ----
     * 设置页的行值无论当前在哪一页都要更新：用户在 WiFi 子页连上/断开，
     * 返回设置页时必须立刻是对的，不能等下一次重建。 */
    if (s_set_wifi_val != NULL) {
        char wb[64];
        wifi_short_state(wb, sizeof(wb));
        set_txt(s_set_wifi_val, wb);
    }

    if (s_cur == APP_WIFI) {
        if (s_w_state != NULL) {
            char wb[128];
            wifi_detail_state(wb, sizeof(wb));
            set_txt(s_w_state, wb);
        }
        if (s_w_scan_lbl != NULL) {
            set_txt(s_w_scan_lbl, i18n_t(net_wifi_scanning() ? W_SCANNING : W_SCAN));
        }

        /* "断开"只在有意义的状态下露出来。用 has_flag 比对再动对象：
         * 每 400 ms 无脑 add/remove 会一直触发无效区重绘（主页闪屏的老毛病）。 */
        if (s_w_disc != NULL) {
            net_wifi_state_t wst = net_wifi_state();
            bool need = (wst == NET_WIFI_CONNECTED ||
                         wst == NET_WIFI_CONNECTING ||
                         wst == NET_WIFI_FAILED);
            bool shown = !lv_obj_has_flag(s_w_disc, LV_OBJ_FLAG_HIDDEN);
            if (shown != need) {
                if (need) {
                    lv_obj_remove_flag(s_w_disc, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_w_disc, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        /* 开关可能被串口命令改过（wifi on/off），反向同步一次。
         * 用户刚拨过的这 1.5 s 里跳过：那时 s_enabled 还没跟上，
         * 跟着同步会把开关弹回原位（见 s_sw_touch_ms 的注释）。 */
        if (s_w_sw != NULL
            && esp_timer_get_time() / 1000 - s_sw_touch_ms > 1500) {
            bool on  = net_wifi_enabled();
            bool chk = lv_obj_has_state(s_w_sw, LV_STATE_CHECKED);
            if (on != chk) {
                if (on) {
                    lv_obj_add_state(s_w_sw, LV_STATE_CHECKED);
                } else {
                    lv_obj_clear_state(s_w_sw, LV_STATE_CHECKED);
                }
            }
        }

        /* 列表只在"条数 / 状态 / 扫描态 / 已知网络条数"变了时才重建。
         * 最后一项是给"连上成功 → 凭据才进已知网络"这条新规则用的：
         * 连上的那一拍还没落表，光看状态不会重建，它会一直留在"其他网络"
         * 里，要等用户手动扫描才归位。 */
        if (net_wifi_scan_count() != s_w_drawn_apn ||
            (int)net_wifi_state() != s_w_drawn_st ||
            net_wifi_saved_count() != s_w_drawn_sv ||
            net_wifi_scanning() != s_w_drawn_scan) {
            wifi_fill_list();
        }
    }

    if (s_cur == APP_HOME) {
        /* ---- 桌面 widget ----
         * 这里全部走 set_txt/set_txt_fmt：内容没变就不写。
         * 桌面两张卡片是半透明的，一次重绘要连带下面的渐变壁纸重新合成，
         * 每 400 ms 无条件重写 8 个字段就是"主页一直闪"的直接原因。 */
        char buf[48];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ipinfo.ip));
        set_txt(s_home_ip, buf);

        if (link) {
            eth_speed_t sp = ETH_SPEED_10M;
            eth_duplex_t dp = ETH_DUPLEX_HALF;
            if (net_eth_get_link(&sp, &dp) == ESP_OK) {
                snprintf(buf, sizeof(buf), "%s  %s/%s", i18n_t(S_LINK_UP),
                         sp == ETH_SPEED_10M ? "10M" : "100M",
                         dp == ETH_DUPLEX_HALF ? "half" : "full");
            } else {
                snprintf(buf, sizeof(buf), "%s", i18n_t(S_LINK_UP));
            }
        } else {
            snprintf(buf, sizeof(buf), "%s", i18n_t(S_LINK_DOWN));
        }
        set_txt(s_home_link, buf);

        set_txt_fmt(s_home_lease, "%u", (unsigned)net_dhcps_lease_count());

        /* 堆余量天然有 1~2 KB 抖动（LVGL 自己也在分配/释放），
         * 死区内的变化不写，否则这个数字每拍都在跳、带着整块卡片重绘。
         * 口径与信息页统一：只看内部 RAM（默认堆含 32 MB PSRAM，
         * 拿它当"内存余量"会一直显示三十多 MB，没有参考意义）。
         * 死区 8 KB → 24 KB：这个数只是给人看的余量，精确到 KB 没有意义，
         * 死区太小时余量会贴着阈值来回跳，等于每 400 ms 重绘一次卡片。 */
        unsigned heap_kb = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL
                                                              | MALLOC_CAP_8BIT) / 1024);
        if (s_heap_shown < 0
            || (heap_kb > (unsigned)s_heap_shown ? heap_kb - (unsigned)s_heap_shown
                                                 : (unsigned)s_heap_shown - heap_kb) >= 24u) {
            s_heap_shown = (int)heap_kb;
            set_txt_fmt(s_home_heap, "%u KB", heap_kb);
        }
    }

    /* ---- 桌面右侧系统状态（CPU / 内存 / TF） ----
     * 放在页面判断外面：CPU 要 1 s 一个采样点，离开桌面也得继续采，
     * 否则从别的页面回来折线上就是一段空白。 */
    home_stat_update(now_us);

    /* ---- 设备列表 ---- */
    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);
    uint32_t sig = dev_sig(snap, n);

    if (s_cur == APP_DEVICES) {
        {
            char cnt[32];
            i18n_dev_count(cnt, sizeof(cnt), n);
            set_txt(s_dev_count, cnt);
        }

        if (!s_dev_have_sig || sig != s_dev_sig || link != s_dev_last_link) {
            s_dev_sig = sig;
            s_dev_have_sig = true;
            s_dev_last_link = link;
            build_dev_list();
        }
    }

    /* ---- 详情 ---- */
    if (s_cur == APP_DETAIL) {
        detail_fill(s_detail_idx);
    }

    /* ---- 升级页 ---- */
    if (s_cur == APP_UPGRADE) {
        if (!s_upg_have_sig || sig != s_upg_sig) {
            s_upg_sig = sig;
            s_upg_have_sig = true;
            build_upg_list();
        }

        bool busy = ota_client_busy() || (st.state != OTA_STATE_IDLE && st.state != OTA_STATE_DONE
                                          && st.state != OTA_STATE_FAILED);

        /* 进度真变了才画：arc 和百分比一起，避免每拍重绘圆环那一大片 */
        if ((int)pct != s_arc_shown) {
            s_arc_shown = (int)pct;
            lv_arc_set_value(s_upg_arc, (int32_t)pct);
            set_txt_fmt(s_upg_pct, "%u%%", pct);
        }

        /* 按钮的显隐只在 busy 翻转时动一次，别每 400 ms 加一次 HIDDEN */
        if ((int)busy != s_busy_shown) {
            s_busy_shown = (int)busy;
            if (busy) {
                lv_obj_add_flag(s_upg_btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(s_upg_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }

        const char *st_txt;
        const char *sub_txt = NULL;
        uint32_t    st_col;
        char        sub_buf[48];

        if (busy) {
            st_txt = state_txt(st.state);
            st_col = state_color(st.state);
            snprintf(sub_buf, sizeof(sub_buf), "%u / %u KB", (unsigned)(st.acked / 1024),
                     (unsigned)(st.total / 1024));
            sub_txt = sub_buf;
        } else if (st.state == OTA_STATE_DONE) {
            st_txt = i18n_t(ST_SUCCESS);
            st_col = C_GREEN;
            sub_txt = i18n_t(U_SUCCESS_HINT);
        } else if (st.state == OTA_STATE_FAILED) {
            st_txt = i18n_t(ST_FAILED);
            st_col = C_RED;
            snprintf(sub_buf, sizeof(sub_buf), i18n_t(U_ERR_FMT), st.err_code);
            sub_txt = sub_buf;
        } else {
            st_txt = i18n_t(U_READY);
            st_col = C_TXT;
            sub_txt = i18n_t(U_SELECT_HINT);
        }

        set_txt(s_upg_state, st_txt);
        set_txt(s_upg_sub, sub_txt);

        if (st_col != s_upg_col_shown) {
            s_upg_col_shown = st_col;
            lv_obj_set_style_text_color(s_upg_state, lv_color_hex(st_col), LV_PART_MAIN);
        }
    }

    /* ---- 信息页 ---- */
    if (s_cur == APP_INFO) {
        char ibuf[48];

        snprintf(ibuf, sizeof(ibuf), IPSTR, IP2STR(&ipinfo.ip));
        set_txt(s_in_ip, ibuf);
        snprintf(ibuf, sizeof(ibuf), IPSTR, IP2STR(&ipinfo.netmask));
        set_txt(s_in_mask, ibuf);
        snprintf(ibuf, sizeof(ibuf), IPSTR, IP2STR(&ipinfo.gw));
        set_txt(s_in_gw, ibuf);

        if (link) {
            eth_speed_t sp = ETH_SPEED_10M;
            eth_duplex_t dp = ETH_DUPLEX_HALF;
            if (net_eth_get_link(&sp, &dp) == ESP_OK) {
                snprintf(ibuf, sizeof(ibuf), "%s %s",
                         sp == ETH_SPEED_10M ? "10M" : "100M",
                         dp == ETH_DUPLEX_HALF ? "half" : "full");
            } else {
                snprintf(ibuf, sizeof(ibuf), "%s", i18n_t(S_LINK_UP));
            }
        } else {
            snprintf(ibuf, sizeof(ibuf), "%s", i18n_t(S_LINK_DOWN));
        }
        set_txt(s_in_link, ibuf);

        set_txt_fmt(s_in_lease, "%u", (unsigned)net_dhcps_lease_count());

        /* ---- 内存：按硬件 768 KB 画全长 ----
         * heap_caps 只统计到 462 KB 左右，差额是静态段 + 启动预留。那段内存
         * 应用拿不到，但确实被占着，所以条上单独涂灰；不把它从总数里悄悄
         * 减掉 —— 否则"共 462 KB"会让人以为这颗芯片只有这么多 RAM。
         * 百分比同样按 768 算，和条保持一致。 */
        const size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        const unsigned hw_kb   = UI_RAM_HW_KB;
        const unsigned heap_kb = (unsigned)(ram_total / 1024u);
        const unsigned free_kb = (unsigned)(ram_free / 1024u);
        const unsigned rsvd_kb = hw_kb > heap_kb ? hw_kb - heap_kb : 0u;
        const unsigned used_kb = hw_kb > free_kb ? hw_kb - free_kb : hw_kb;  /* 含预留 */
        const unsigned hw_safe = hw_kb > 0u ? hw_kb : 1u;
        const unsigned ram_pct = (unsigned)(((uint64_t)used_kb * 100u + hw_safe / 2u) / hw_safe);

        /* 只在百分比整数变化时重画：1% ≈ 7.7 KB，稳态下三段条完全不重绘 */
        if ((int)ram_pct != s_in_mem_shown) {
            s_in_mem_shown = (int)ram_pct;

            const int32_t rpx = (int32_t)((uint32_t)UI_MEM_BAR_W * rsvd_kb / hw_safe);
            const int32_t upx = (int32_t)((uint32_t)UI_MEM_BAR_W * used_kb / hw_safe);
            const int32_t uw  = upx - rpx + 6;    /* 左移 6 px 盖住灰段的圆角 */

            lv_obj_set_width(s_in_mem_res, rpx);
            lv_obj_set_pos(s_in_mem_used, 24 + rpx - 6, 96);
            lv_obj_set_width(s_in_mem_used, uw > 0 ? uw : 6);

            set_txt_fmt(s_in_mem_pct, "%u%%", ram_pct);
            /* 蓝 → 橙 → 红。门槛按 768 口径重设：开机就背着 306 KB 预留，
             * 沿用旧的 85/70 会一上电就亮橙色。88% ≈ 堆用掉 91%，94% ≈ 98%。 */
            const unsigned used_rgb = ram_pct >= 94u ? C_RED
                                    : ram_pct >= 88u ? C_ORANGE : C_BLUE;
            lv_obj_set_style_bg_color(s_in_mem_used, lv_color_hex(used_rgb), LV_PART_MAIN);

            /* 图例就是这行字本身：三个 KB 数各染成条上对应那段的颜色，
             * 再加一个总数，"颜色代表什么"不用再猜。
             * 英文串全 ASCII（Montserrat 没有 ·，之前那两个点会变方框）。 */
            char seg_u[32], seg_s[32], seg_f[32], seg_t[32], line[192];
            snprintf(seg_u, sizeof seg_u, i18n_t(I_RAM_USED_FMT), heap_kb - free_kb);
            snprintf(seg_s, sizeof seg_s, i18n_t(I_RAM_SYS_FMT),  rsvd_kb);
            snprintf(seg_f, sizeof seg_f, i18n_t(I_RAM_FREE_FMT), free_kb);
            snprintf(seg_t, sizeof seg_t, i18n_t(I_RAM_TOTAL_FMT), hw_kb);

            /* 拼串的缓冲要留足：4 组 "#RRGGBB 文字#" 固定 43 字节，编译器按
             * 每段最多 31 字节算，192 才够它确认不会截断（-Wformat-truncation
             * 会按缓冲区上限而不是实际内容长度推最坏情况）。
             * 系统段文字用 9A9AA4 而不是条上那个 B4B4BF：同样是大块面积才
             * 显浓的浅灰，14/16 px 的字按 B4B4BF 上白底几乎看不清。
             *
             * ⚠ recolor 的语法是 "#RRGGBB<空格>文字#"，颜色参数后面**必须是空格**，
             * 写成 "#RRGGBB#文字#" 会整段错乱。lv_draw_label.c 的状态机是：
             *   '#' 之后进入 PARAMETER，非空格字符一律 continue（吞掉），
             *   只有遇到空格且 "#" 到空格恰好 7 个字符时才算颜色解析成功、
             *   进入 TEXT_INPUT 开始染色，并把这个分隔空格跳过不画。
             * 如果第 7 个字符是 '#'：PARAMETER 分支只把状态退回 WAIT_FOR_PARAMETER
             * 而**不 continue**，于是那个 '#' 被当普通字符画出来；紧接着的文字用
             * 默认色绘制；下一段开头的 "#9A9AA4" 又因为没有 TEXT_INPUT 的收尾 '#'
             * 被当正文，屏上就出现 "#已占用 233 KB 9A9AA4…" 这种东西。 */
            snprintf(line, sizeof line,
                     "#%06X %s#  #9A9AA4 %s#  #8E8E93 %s#  #8E8E93 %s#",
                     used_rgb, seg_u, seg_s, seg_f, seg_t);
            set_txt(s_in_mem_sub, line);
        }

        /* PSRAM 单独一行：和上面 INTERNAL RAM 统一成"已用"口径，两处数字
         * 才是同一个方向，不会一处读已用、另一处读空闲。
         * 32 MB 里实际只占着 1024×600 RGB565 单帧缓冲约 1.2 MB，所以是
         * "已用 1 MB / 共 32 MB"，看着空得多是正常的。 */
        size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t ps_used = ps_total > ps_free ? ps_total - ps_free : 0u;
        set_txt_fmt(s_in_psram, i18n_t(I_PSRAM_FMT),
                    (unsigned)((ps_used + 512u * 1024u) / (1024u * 1024u)),
                    (unsigned)((ps_total + 512u * 1024u) / (1024u * 1024u)));

        /* Flash 容量：问驱动要（读 JEDEC ID / SFDP 实测），
         * 不是 CONFIG_ESPTOOLPY_FLASHSIZE —— 那个只是烧录时假设的值，
         * 换了模组不重配就会骗人。容量不会变，只写一次。 */
        if (s_flash_shown < 0) {
            uint32_t fsz = 0;
            if (esp_flash_get_size(NULL, &fsz) == ESP_OK && fsz > 0) {
                s_flash_shown = 1;
                if (fsz >= 0x100000u) {
                    set_txt_fmt(s_in_flash, "%u MB", (unsigned)(fsz >> 20));
                } else {
                    set_txt_fmt(s_in_flash, "%u KB", (unsigned)(fsz >> 10));
                }
            } else {
                s_flash_shown = 0;
                set_txt(s_in_flash, "-");
            }
        }

        /* 最低水位同样只看内部 RAM，才能和上面的进度条对得上 */
        set_txt_fmt(s_in_min, "%u KB",
                    (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL
                                                               | MALLOC_CAP_8BIT) / 1024));

        /* 运行时间：按时长自动选单位，比 0:00:37 这种纯 HH:MM:SS 直观；
         * 注意 ILP32 下 uint32_t 是 unsigned long，%u 必须显式转 (unsigned)。 */
        unsigned up_s = (unsigned)(esp_timer_get_time() / 1000000);
        if (up_s < 60u) {
            set_txt_fmt(s_in_up, "%u s", up_s);
        } else if (up_s < 3600u) {
            set_txt_fmt(s_in_up, "%u m %u s", up_s / 60u, up_s % 60u);
        } else if (up_s < 86400u) {
            set_txt_fmt(s_in_up, "%u h %u m", up_s / 3600u, (up_s / 60u) % 60u);
        } else {
            set_txt_fmt(s_in_up, "%u d %u h",
                        up_s / 86400u, (up_s % 86400u) / 3600u);
        }

        /* 设备数 / 升级状态 / CPU 三行已删：这一页是给现场排障用的，
         * 设备数量在桌面和升级页都看得到，升级进度在升级页更准，
         * CPU 占用率属于开发期数据，日常没人看。 */
    }
}

/* ---------------- 组装 ---------------- */

static lv_obj_t *make_page(lv_obj_t *parent, bool solid_bg)
{
    lv_obj_t *p = plain(parent);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, BSP_LCD_H_RES, BSP_LCD_V_RES);

    if (solid_bg) {
        lv_obj_set_style_bg_color(p, lv_color_hex(C_PAGE_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    }
    return p;
}

/* 返回胶囊的点击回调（头部通用） */
static void back_cb(lv_event_t *e)
{
    (void)e;
    page_back();
}

static lv_obj_t *header(lv_obj_t *page, const char *title, lv_obj_t **chip_out)
{
    /* y=34 起：上面 30 px 留给全局状态栏 */
    lv_obj_t *back = pill(page, 20, 34, 108, 40, C_CARD, LV_OPA_COVER);
    lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(back, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = mk_label(back);
    lv_label_set_text(bl, i18n_t(S_BACK));
    lv_obj_set_style_text_font(bl, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(C_BLUE), LV_PART_MAIN);
    lv_obj_center(bl);

    text(page, title, UI_F_LARGE, C_TXT, 148, 36);

    if (chip_out != NULL) {
        *chip_out = text(page, "", UI_F_SMALL, C_SUB, 0, 46);
        lv_obj_align(*chip_out, LV_ALIGN_TOP_RIGHT, -24, 46);
    }
    return back;
}

static void build_all(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* 壁纸：中性石墨竖向渐变，桌面是唯一带壁纸的页面，应用页都盖在它上面。
     * 想换配色只改 C_WALL_TOP / C_WALL_BOT 两个宏。 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_WALL_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_WALL_BOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 页面栈不在这里重置：rebuild（切语言）要保留用户的返回路径，
     * 栈的初始化放在 ui_app_start，定时器也在那里建（rebuild 不能重复创建）。 */
    for (int i = 0; i < APP_COUNT; i++) {
        s_page[i] = make_page(scr, i != APP_HOME);
        if (i != APP_HOME) {
            lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    build_home(s_page[APP_HOME]);

    header(s_page[APP_DEVICES], i18n_t(D_TITLE), &s_dev_count);
    s_dev_list = plain(s_page[APP_DEVICES]);
    lv_obj_set_pos(s_dev_list, 20, CONTENT_Y);
    lv_obj_set_size(s_dev_list, 984, 492);
    /* 滚动依赖"可点"：LVGL 只对可点的对象做拖动/滑动判定 */
    lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_dev_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_dev_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_dev_list, LV_OPA_TRANSP, LV_PART_MAIN);

    header(s_page[APP_DETAIL], i18n_t(DT_TITLE), NULL);
    build_detail(s_page[APP_DETAIL]);

    header(s_page[APP_UPGRADE], i18n_t(U_TITLE), NULL);
    build_upgrade(s_page[APP_UPGRADE]);

    header(s_page[APP_INFO], i18n_t(I_TITLE), NULL);
    build_info(s_page[APP_INFO]);

    header(s_page[APP_SETTINGS], i18n_t(SET_TITLE), NULL);
    build_settings(s_page[APP_SETTINGS]);

    /* 语言是设置下的子页：文案和字体都要跟着语言变，所以也走整页重建 */
    header(s_page[APP_LANG], i18n_t(SET_LANGUAGE), NULL);
    build_lang(s_page[APP_LANG]);

    /* 亮度子页：标题复用"亮度"这一条文案 */
    header(s_page[APP_BRIGHT], i18n_t(SET_BRIGHTNESS), NULL);
    build_bright(s_page[APP_BRIGHT]);

    /* WiFi 子页：标题就是 "WiFi" */
    header(s_page[APP_WIFI], i18n_t(W_WIFI), NULL);
    build_wifi(s_page[APP_WIFI]);

    /* 状态栏最后创建：屏幕的最后一个子对象 → 永远画在页面之上 */
    build_status_bar(scr);
}

/* 整界面重建（目前只有切语言用得到：文案和字体同时换，逐条改写太容易漏）。
 * 做法是把屏幕上所有对象清掉重新 build，再把用户送回原来那一页。
 * 注意：refresh_cb 里的"上次值"缓存必须一起复位，否则新 label 上不会写内容。 */
static void ui_app_rebuild(void)
{
    app_id_t keep = s_cur;

    s_dev_sig        = 0;
    s_dev_have_sig   = false;
    s_dev_last_link  = false;
    s_upg_sig        = 0;
    s_upg_have_sig   = false;
    s_heap_shown     = -1;
    s_in_mem_shown   = -1;
    s_flash_shown    = -1;
    s_arc_shown      = -1;
    s_busy_shown     = -1;
    s_lan_key        = -1;
    s_lan_drawn      = -1;      /* 图标是新建的对象，必须重画一次 */
    s_lan_fg_drawn   = 0;
    s_upg_col_shown  = 0xFFFFFFFFu;

    /* 桌面卡片：新 label 都是新对象，台数/内存/TF 行必须重写一次 */
    s_home_online_shown    = -1;
    s_home_devoff_shown    = -1;
    s_home_mem_shown = -1;
    s_tf_shown       = -2;

    /* 这三个是新页面里的新对象，旧指针必须清掉：
     * rebuild 期间若 400 ms 定时器或滑块回调进来，写旧对象就是野指针；
     * 手势那边也会用 s_br_slider 算命中带，留着旧指针更危险。 */
    s_br_slider  = NULL;
    s_br_pct     = NULL;
    s_set_br_val = NULL;

    /* 升级页的标题 label 会被隐/显切换，重建后必须先清掉旧指针 */
    s_upg_hdr    = NULL;

    /* 桌面右侧卡片的 label 也会被 lv_obj_clean 删掉 */
    s_home_online    = NULL;
    s_home_dev_off   = NULL;
    s_home_mem_pct = NULL;
    s_home_mem_bar = NULL;
    s_home_tf      = NULL;

    /* WiFi 子页同理。注意密码浮层现在挂在 lv_layer_top() 上，
     * lv_obj_clean(screen) 清不到它，必须显式删掉再清指针 ——
     * 留着的话 swipe_poll 会一直认为"浮层开着"，整机侧滑返回永久失效。 */
    s_w_sw         = NULL;
    s_w_state      = NULL;
    s_w_scan_lbl   = NULL;
    s_w_disc       = NULL;
    s_w_list       = NULL;
    s_set_wifi_val = NULL;
    close_password();
    s_pw_modal     = NULL;
    s_pw_ta        = NULL;
    s_pw_kb        = NULL;
    s_w_drawn_apn  = -1;
    s_w_drawn_st   = -1;
    s_w_drawn_scan = false;
    s_w_drawn_sv   = -1;
    /* 状态栏对象会被 lv_obj_clean() 删掉，指针必须一起清：
     * 只把 s_wifi_lvl 复位成 -1 是不够的，wifi_icon_update() 若在这一帧
     * 被调到，lvl(-1) != 新档位就会去操作已释放的 s_wifi。 */
    s_wifi         = NULL;
    s_wifi_slash   = NULL;
    s_wifi_lvl     = -1;        /* 状态栏是新对象：wifi_icon_create() 会重设 */

    lv_obj_clean(lv_screen_active());
    build_all();

    if (keep != APP_HOME) {
        /* page_show 会判断栈顶，栈顶已经是 keep 时不会重复压栈 */
        s_cur = APP_HOME;
        page_show(keep, false);
    } else {
        s_cur = APP_HOME;
    }

    ESP_LOGI(TAG, "界面已按当前语言重建（停留页 %d）", (int)keep);
}

esp_err_t ui_app_start(void)
{
    /* 语言偏好在 NVS 里，先读出来再建界面 —— 字体和文案在建对象时就定死了，
     * 建完再改只能整页重建。 */
    i18n_init();

    /* 亮度同理：bsp_display_init 上电时把背光点到了 100%，
     * 这里读回用户上次设的档位覆盖掉（LEDC 已就绪）。 */
    bright_load();
    bsp_display_brightness_set(s_bright);

    /* 超时必须给非 0：lvgl_port_lock(0) 是"不等待"，会直接失败 */
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "5 秒内拿不到 LVGL 锁");
        return ESP_FAIL;
    }

    s_stack[0] = APP_HOME;
    s_depth    = 0;
    s_cur      = APP_HOME;

    build_all();

    /* 两个定时器只在这里建一次：ui_app_rebuild() 不会再建，否则越切越多 */
    lv_timer_create(refresh_cb, 400, NULL);
    lv_timer_create(swipe_poll_cb, 30, NULL);   /* 滑动返回：30 ms 轮询指针位移 */

    /* 刷新统计：挂在 display 上而不是 screen 上 —— screen 会被 rebuild 清掉，
     * display 的生命周期才是整个应用。也只在这里注册一次。 */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_display_add_event_cb(disp, disp_flush_stat_cb, LV_EVENT_FLUSH_START, NULL);
    }

    bsp_display_unlock();

    ESP_LOGI(TAG, "界面就绪：桌面 + 设备 / 升级 / 信息 / 设置（%s，400 ms 刷新）",
             i18n_lang() == LANG_ZH ? "中文" : "English");
    return ESP_OK;
}
