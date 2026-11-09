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

#include "lvgl.h"

#include "app_config.h"
#include "bsp_display.h"
#include "dev_table.h"
#include "i18n.h"
#include "net_dhcps.h"
#include "net_eth.h"
#include "ota_client.h"
#include "ota_image.h"
#include "ota_proto.h"
#include "ui_app.h"

static const char *TAG = "ui";

/* 工具名/版本：桌面浮条和设置页关于都用这两个宏，改版本只改这里。
 * 名字本身是产品名不翻译；tagline 走 i18n。 */
#define APP_NAME     "ESP32-P4 OTA Tool"
#define APP_VERSION  "v0.9.0"

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
    APP_COUNT,
} app_id_t;

static app_id_t  s_cur = APP_HOME;
static lv_obj_t *s_page[APP_COUNT];

/* 页面栈：栈底固定是桌面，返回时弹一页而不是直接跳桌面。
 * 例：HOME -> Devices -> Device 详情 -> Upgrade，返回依次回退。 */
#define NAV_MAX  8
static app_id_t s_stack[NAV_MAX];
static int      s_depth = 0;

/* 顶部状态栏（全局，浮在所有页之上） */
static lv_obj_t *s_sb      = NULL;
static lv_obj_t *s_sb_dev  = NULL;
static lv_obj_t *s_sb_fps  = NULL;

/* 状态栏缓存：只在文字/配色真的变了才写 label，避免 400 ms 无条件重绘 */
static int  s_sb_last_dev  = -1;
static bool s_sb_last_dark = false;

/* ---- 帧率统计 ----
 * LVGL 只在有脏区时才走一遍 refr，所以 LV_EVENT_RENDER_READY 的触发次数
 * 就是"真实出帧数"，不是"屏幕刷了几次"。静态页面会稳定在 0，
 * 有动画/滚动才上去 —— 反过来说，纯静止时长期非 0 就说明有东西在乱重绘。
 * 不用 LV_USE_PERF_MONITOR：那个要在 lv_conf 里打开，还会往屏幕上盖一层
 * 半透明面板，正好和我们自己在画的状态栏打架。 */
static volatile uint32_t s_fps_frames = 0;
static uint32_t s_fps_last_cnt = 0;
static int64_t  s_fps_last_us  = 0;
static int      s_fps_shown    = -1;

/* 桌面上那张"网络"卡里的 IP 是否已经写过（重建后需重置） */
static lv_obj_t *s_home_ip   = NULL;
static lv_obj_t *s_home_link = NULL;
static lv_obj_t *s_home_lease = NULL;
static lv_obj_t *s_home_heap = NULL;
static lv_obj_t *s_home_state = NULL;
static lv_obj_t *s_home_sub   = NULL;
static lv_obj_t *s_home_bar   = NULL;

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
static lv_obj_t *s_in_mem_pct, *s_in_mem_bar, *s_in_mem_sub, *s_in_psram;
static lv_obj_t *s_in_flash, *s_in_min, *s_in_up, *s_in_dev, *s_in_ota;

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

/* ------------------------------------------------------------------
 * 刷新判重缓存。
 * 原来是 refresh_cb 里的函数内 static，切换语言重建界面后这些值还留着，
 * 会出现"重建完文字不刷新"的假象，因此统一提到文件作用域，由
 * ui_app_rebuild() 一并复位。
 * ------------------------------------------------------------------ */
static int      s_heap_shown     = -1;          /* 桌面空闲内存（8 KB 死区） */
static int      s_in_mem_shown   = -1;          /* 信息页内部 RAM 占用率 % */
static int      s_flash_shown    = -1;          /* 信息页 Flash 容量（-1 未读，只读一次） */
static int      s_bar_shown      = -1;          /* 桌面进度条 */
static int      s_arc_shown      = -1;          /* 升级页进度环 */
static int      s_busy_shown     = -1;          /* 升级按钮显隐 */
static int      s_lan_key        = -1;          /* 网线图标 (up|dark) */
static int      s_lan_drawn      = -1;          /* 图标实际画出的状态（-1 未画） */
static uint32_t s_lan_fg_drawn   = 0;           /* 图标实际画出的颜色 */
static uint32_t s_home_col_shown = 0xFFFFFFFFu; /* 桌面状态文字色 */
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

/* 斜杠本体：3 x 24 的圆角竖条，绕自身中心顺时针转 45° 就是 "/"。
 * 转 45° 后对角跨度 ≈ (24 + 3) × 0.707 ≈ 19 px，正好盖过 18 px 的图标 ink，
 * 两端各露出一点点，看起来才是"划掉"而不是"图标上贴了个点"。
 *
 * pivot 必须显式设成对象中心：LVGL 的 transform 默认绕**左上角**转，
 * 3×24 的条绕 (0,0) 转 45° 后几何中心会跑到 (-7.4, 9.5)，
 * 也就是左偏 9 px、上移 2.5 px —— 这就是"斜杠没经过图标正中间"的原因，
 * 跟布局和字体都无关。 */
static lv_obj_t *lan_slash(lv_obj_t *parent)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, 3, 24);
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
     * 斜杠转 45° 后跨度约 19 px，横穿整个图标。 */
    s_slash = lan_slash(parent);
    lv_obj_align_to(s_slash, s_lan, LV_ALIGN_CENTER, 0, 0);
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

/* 状态栏：左边设备数，右边帧率 + 网线图标。
 * 它是屏幕直属子对象、最后创建，所以永远压在页面之上；
 * 透明且不可点，不会挡住下面的图标和列表。
 * 时间/日期先不做：本机没有 RTC 也没有 NTP，显示一个需要手动 time set
 * 才准的时钟意义不大，先留空位，等有对时来源再加回来。 */
static void build_status_bar(lv_obj_t *scr)
{
    s_sb = plain(scr);
    lv_obj_set_pos(s_sb, 0, 0);
    lv_obj_set_size(s_sb, BSP_LCD_H_RES, 30);

    char cnt[32];
    i18n_dev_count(cnt, sizeof(cnt), 0);
    s_sb_dev = text(s_sb, cnt, UI_F_SMALL, C_TXT, 24, 6);

    /* 帧率贴右侧，给 24 px 网线图标让出 24+18 的宽度。
     * 宽度写死 72 px 并右对齐：读数从 "9 FPS" 变 "60 FPS" 时位数变化，
     * 若让 label 自适应宽度，右边缘会跟着左右跳，看起来像图标在抖。 */
    s_sb_fps = text(s_sb, "", UI_F_SMALL, C_TXT, 0, 6);
    lv_obj_set_style_text_align(s_sb_fps, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(s_sb_fps, 72);
    lv_obj_set_style_text_opa(s_sb_fps, LV_OPA_70, LV_PART_MAIN);  /* 弱于左侧设备数 */
    lv_obj_align(s_sb_fps, LV_ALIGN_TOP_RIGHT, -52, 6);

    lan_icon_create(s_sb);
    lv_obj_move_to_index(s_sb, -1);
}

/* display 每真正渲染完一帧发一次 RENDER_READY（core/lv_refr.c 末尾），
 * 拿它计数比用刷新定时器次数准：LVGL 没脏区时刷新定时器是 pause 状态，
 * 根本不会跑，所以静止页面这里就是 0。 */
static void disp_frame_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_READY) {
        s_fps_frames++;
    }
}

/* ---- 刷新统计（诊断"到底是谁在闪"） ----
 * 每 10 s 往串口打一行：这段时间 flush 了多少次、最近一次刷的是哪块区域。
 * 判据很直接：
 *   静止时一行应该是 "0 次"        → LVGL 没在画，闪就不是软件重绘引起的；
 *   静止时持续非 0，区域又是整屏   → 有对象在周期性 invalidate，看区域就能定位到是谁；
 *   区域只有一小块（状态栏那块）    → 只是数字更新，配合 VSYNC 对齐后不该看得见。 */
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

/* 1 秒窗口算一次帧率：窗口内渲染帧数 / 实际时长。
 * 用 esp_timer 而不是"400 ms × 次数"是因为定时器本身有抖动，
 * 用它算分母会让 55~60 之间来回跳。 */
static void fps_tick(void)
{
    if (s_sb_fps == NULL) {
        return;
    }

    int64_t  now = esp_timer_get_time();
    uint32_t cnt = s_fps_frames;

    if (s_fps_last_us == 0) {          /* 第一个窗口只取基准，不出数 */
        s_fps_last_us  = now;
        s_fps_last_cnt = cnt;
        return;
    }

    int64_t dt_us = now - s_fps_last_us;
    if (dt_us < 900000) {              /* 攒够约 1 s 再算，避免被 400 ms 节拍切碎 */
        return;
    }

    uint32_t fps = (uint32_t)((uint64_t)(cnt - s_fps_last_cnt) * 1000000ull / (uint64_t)dt_us);

    s_fps_last_us  = now;
    s_fps_last_cnt = cnt;

    /* 死区 2 帧：静止页面每秒唯一的一帧重绘，来源就是"上一拍写了 FPS 标签"本身。
     * 写标签 → 渲染一帧 → 下一窗口算出 1 → 又写标签…… 永不收敛，
     * 等于自己给自己制造周期重绘。差值 < 2 就不写，静止时稳定停在 0，
     * 屏幕才真的完全静止（也能反过来证明没有别的东西在乱 invalidate）。 */
    int diff = (int)fps - s_fps_shown;
    if (s_fps_shown < 0 || diff >= 2 || diff <= -2) {
        s_fps_shown = (int)fps;
        set_txt_fmt(s_sb_fps, "%u FPS", (unsigned)fps);
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

    bool dark_ui = (s_cur == APP_HOME);
    /* 壁纸改成淡色后，桌面和应用页都用深色字；dark_ui 只留作"配色变过"的判据 */
    uint32_t fg = C_TXT;

    lv_obj_set_style_text_color(s_sb_dev, lv_color_hex(fg), LV_PART_MAIN);

    s_sb_last_dark = dark_ui;
    lan_icon_update(net_eth_link_up(), fg);
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

static void build_home(lv_obj_t *page)
{
    /* 状态栏不在这里建：它是全局的（s_sb），浮在所有页之上，见 build_status_bar() */

    /* ---- 左：网络 widget ---- */
    lv_obj_t *c1 = card(page, 32, 56, 466, 236, 24);
    lv_obj_set_style_bg_opa(c1, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(c1, 0, LV_PART_MAIN);

    text(c1, i18n_t(H_NETWORK), UI_F_SMALL, C_SUB, 24, 20);
    s_home_ip = text(c1, "0.0.0.0", UI_F_LARGE, C_TXT, 24, 42);

    text(c1, i18n_t(H_LINK), UI_F_SMALL, C_SUB, 24, 108);
    s_home_link = text(c1, "--", UI_F_SMALL, C_TXT, 0, 108);
    lv_obj_align(s_home_link, LV_ALIGN_TOP_RIGHT, -24, 108);

    text(c1, i18n_t(H_DHCP_LEASE), UI_F_SMALL, C_SUB, 24, 140);
    s_home_lease = text(c1, "0", UI_F_SMALL, C_TXT, 0, 140);
    lv_obj_align(s_home_lease, LV_ALIGN_TOP_RIGHT, -24, 140);

    text(c1, i18n_t(H_FREE_HEAP), UI_F_SMALL, C_SUB, 24, 172);
    s_home_heap = text(c1, "0 KB", UI_F_SMALL, C_TXT, 0, 172);
    lv_obj_align(s_home_heap, LV_ALIGN_TOP_RIGHT, -24, 172);

    /* ---- 右：会话 widget ---- */
    lv_obj_t *c2 = card(page, 526, 56, 466, 236, 24);
    lv_obj_set_style_bg_opa(c2, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(c2, 0, LV_PART_MAIN);

    text(c2, i18n_t(H_LAST_SESSION), UI_F_SMALL, C_SUB, 24, 20);
    s_home_state = text(c2, i18n_t(ST_IDLE), UI_F_LARGE, C_TXT, 24, 42);
    s_home_sub = text(c2, i18n_t(H_NO_SESSION), UI_F_SMALL, C_SUB, 24, 84);

    s_home_bar = lv_bar_create(c2);
    lv_obj_remove_flag(s_home_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_home_bar, 24, 124);
    lv_obj_set_size(s_home_bar, 418, 10);
    lv_bar_set_range(s_home_bar, 0, 100);
    lv_bar_set_value(s_home_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_home_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_home_bar, lv_color_hex(0xE9E9EE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_home_bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_home_bar, lv_color_hex(C_BLUE), LV_PART_INDICATOR);

    /* ---- 图标网格：4 个居中排布，4*96 + 3*32 = 480，(1024-480)/2 = 272 ---- */
    app_tile(page, 272, 322, A_DEVICES,  0xEAF3FF, 0xD4E6FF, 0x1A63C8, ICO_DEVICES,  APP_DEVICES);
    app_tile(page, 400, 322, A_UPGRADE,  0xE8F9EF, 0xD0F1DD, 0x127A3E, ICO_UPLOAD,   APP_UPGRADE);
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

    lv_obj_t *dtg = mk_label(dock);
    lv_label_set_text(dtg, i18n_t(H_TAGLINE));
    lv_obj_set_style_text_font(dtg, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(dtg, lv_color_hex(C_SUB), LV_PART_MAIN);
    lv_obj_align(dtg, LV_ALIGN_LEFT_MID, 300, 0);

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
            text(r, e->id.model, UI_F_BASE, C_TXT, 48, 18);

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
        set_txt(s_dt_model, e->id.model);
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
        lv_obj_t *l = mk_label(s_upg_list);
        lv_label_set_text(l, i18n_t(S_NO_DEVICE));
        lv_obj_set_style_text_font(l, UI_F_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), LV_PART_MAIN);
        return;
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
    text(left, i18n_t(U_SELECT_DEVICE), UI_F_SMALL, C_SUB, 20, 18);

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

static void build_info(lv_obj_t *page)
{
    lv_obj_t *a = card(page, 20, CONTENT_Y, 484, 460, 20);
    text(a, i18n_t(I_NETWORK), UI_F_SMALL, C_SUB, 24, 20);
    s_in_ip    = info_row(a, i18n_t(I_ADDRESS), 56);
    s_in_mask  = info_row(a, i18n_t(I_NETMASK), 100);
    s_in_gw    = info_row(a, i18n_t(I_GATEWAY), 144);
    s_in_link  = info_row(a, i18n_t(I_LINK), 188);
    s_in_lease = info_row(a, i18n_t(I_LEASE), 232);

    lv_obj_t *b = card(page, 520, CONTENT_Y, 484, 460, 20);
    text(b, i18n_t(I_SYSTEM), UI_F_SMALL, C_SUB, 24, 20);

    /* 内存：一条进度条 + 右上角大号百分比。
     * 只取内部 RAM（MALLOC_CAP_INTERNAL）：默认堆里还并入了 32 MB PSRAM，
     * 按默认堆算占用率会被稀释到个位数，看不出运行时的真实压力。
     * PSRAM 单独一行显示容量，够判断大块分配有没有吃满。 */
    text(b, i18n_t(I_RAM), UI_F_SMALL, C_SUB, 24, 56);

    s_in_mem_pct = text(b, "0%", UI_F_LARGE, C_TXT, 0, 42);
    lv_obj_align(s_in_mem_pct, LV_ALIGN_TOP_RIGHT, -24, 42);

    s_in_mem_bar = lv_bar_create(b);
    lv_obj_remove_flag(s_in_mem_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_in_mem_bar, 24, 96);
    lv_obj_set_size(s_in_mem_bar, 436, 12);
    lv_bar_set_range(s_in_mem_bar, 0, 100);
    lv_bar_set_value(s_in_mem_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_in_mem_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_in_mem_bar, lv_color_hex(0xE9E9EE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_in_mem_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_in_mem_bar, lv_color_hex(C_BLUE), LV_PART_INDICATOR);

    s_in_mem_sub = text(b, "-", UI_F_SMALL, C_SUB, 24, 116);

    s_in_psram = info_row(b, i18n_t(I_PSRAM), 160);
    s_in_flash = info_row(b, i18n_t(I_FLASH), 200);
    s_in_min   = info_row(b, i18n_t(I_MIN_HEAP), 240);
    s_in_up    = info_row(b, i18n_t(I_UPTIME), 280);
    s_in_dev   = info_row(b, i18n_t(I_DEVICES), 320);
    s_in_ota   = info_row(b, i18n_t(I_OTA_STATE), 360);
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

    /* ---- 关于 ---- */
    text(page, i18n_t(SET_ABOUT), UI_F_SMALL, C_SUB, 32, y);
    y += 24;

    lv_obj_t *a = card(page, 20, y, 984, 96, 20);
    text(a, APP_NAME, UI_F_BASE, C_TXT, 24, 20);
    text(a, APP_VERSION, UI_F_SMALL, C_SUB, 24, 56);
    y += 96 + 20;

    /* ---- 说明 ---- */
    lv_obj_t *h = card(page, 20, y, 984, 88, 20);
    lv_obj_t *hint = mk_label(h);
    lv_label_set_text(hint, i18n_t(SET_ABOUT_HINT));
    lv_obj_set_style_text_font(hint, UI_F_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_SUB), LV_PART_MAIN);
    lv_obj_set_width(hint, 936);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(hint, 24, 22);
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

    /* 刷新统计：只在"这段时间真的刷过"时才打一行，静止（0 次）完全静默，
     * 不破坏"日志只在异常/连接变化时输出"的约定。 */
    static int64_t s_flush_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (s_flush_log_us == 0) {
        s_flush_log_us = now_us;
    } else if (now_us - s_flush_log_us >= 10000000) {
        if (s_flush_cnt > 0) {
            ESP_LOGI(TAG, "10 s 内刷新 %u 次，最近区域 (%d,%d)-(%d,%d)",
                     (unsigned)s_flush_cnt, (int)s_flush_x1, (int)s_flush_y1,
                     (int)s_flush_x2, (int)s_flush_y2);
            s_flush_cnt = 0;
        }
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

    int devn = dev_table_count();
    unsigned pct = 0;
    if (st.total > 0) {
        pct = (unsigned)(((uint64_t)st.acked * 100u) / st.total);
        if (pct > 100) {
            pct = 100;
        }
    }

    /* ---- 顶部状态栏（全局，所有页面都显示） ---- */
    if (s_sb != NULL) {
        if (devn != s_sb_last_dev) {
            s_sb_last_dev = devn;
            char cnt[32];
            i18n_dev_count(cnt, sizeof(cnt), devn);
            lv_label_set_text(s_sb_dev, cnt);
        }

        /* 桌面是深色壁纸用白字，应用页是浅灰底用深字 */
        bool dark_ui = (s_cur == APP_HOME);
        /* 壁纸改成淡色后，桌面和应用页都用深色字；dark_ui 只留作"配色变过"的判据 */
        uint32_t fg = C_TXT;

        if (dark_ui != s_sb_last_dark) {
            s_sb_last_dark = dark_ui;
            lv_obj_set_style_text_color(s_sb_dev, lv_color_hex(fg), LV_PART_MAIN);
        }

        /* 网线图标只在"插拔"或"换页面配色"时重画，避免每 400 ms 刷新一次 */
        int key = (link ? 1 : 0) | (dark_ui ? 2 : 0);
        if (key != s_lan_key) {
            s_lan_key = key;
            lan_icon_update(link, fg);
        }

        fps_tick();
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

        set_txt(s_home_state, state_txt(st.state));

        /* 颜色只在真的换了才设：set_style 本身也会 invalidate */
        uint32_t sc_now = state_color(st.state);
        if (sc_now != s_home_col_shown) {
            s_home_col_shown = sc_now;
            lv_obj_set_style_text_color(s_home_state, lv_color_hex(sc_now), LV_PART_MAIN);
        }

        if (st.state == OTA_STATE_IDLE) {
            set_txt(s_home_sub, i18n_t(H_NO_SESSION));
        } else {
            set_txt_fmt(s_home_sub, "%u%%   %u / %u KB   %u ms",
                        pct, (unsigned)(st.acked / 1024), (unsigned)(st.total / 1024),
                        (unsigned)st.elapsed_ms);
        }

        if ((int)pct != s_bar_shown) {
            s_bar_shown = (int)pct;
            lv_bar_set_value(s_home_bar, (int32_t)pct, LV_ANIM_OFF);
        }
    }

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

        /* ---- 内存：内部 RAM 占用率（进度条 + 百分比） ----
         * 取 MALLOC_CAP_INTERNAL：默认堆里还并入了 32 MB PSRAM
         * （CONFIG_SPIRAM_USE_MALLOC=y），按默认堆算占用率永远只有个位数，
         * 看不出运行时压力。真正会耗尽的正是内部 RAM。 */
        size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t ram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        unsigned ram_pct = ram_total ? (unsigned)(((ram_total - ram_free) * 100u) / ram_total) : 0u;

        /* 只在百分比整数变化时重画：内部 RAM 的 1% ≈ 4 KB，
         * 比桌面那个 8 KB 死区还稳，稳态下进度条完全不重绘。 */
        if ((int)ram_pct != s_in_mem_shown) {
            s_in_mem_shown = (int)ram_pct;

            set_txt_fmt(s_in_mem_pct, "%u%%", ram_pct);
            lv_bar_set_value(s_in_mem_bar, (int32_t)ram_pct, LV_ANIM_OFF);
            /* 蓝 → 橙 → 红：还能跑 / 该留意 / 快没了 */
            lv_obj_set_style_bg_color(s_in_mem_bar,
                                      lv_color_hex(ram_pct >= 85u ? C_RED
                                                   : ram_pct >= 70u ? C_ORANGE : C_BLUE),
                                      LV_PART_INDICATOR);
            set_txt_fmt(s_in_mem_sub, i18n_t(I_RAM_DETAIL),
                        (unsigned)(ram_free / 1024), (unsigned)(ram_total / 1024));
        }

        /* PSRAM 单独一行：总量不变，free 变化用 MB 取整，实际很少重绘 */
        size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        set_txt_fmt(s_in_psram, "%u / %u MB",
                    (unsigned)((ps_free + 512u * 1024u) / (1024u * 1024u)),
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

        set_txt_fmt(s_in_dev, "%d", devn);
        set_txt_fmt(s_in_ota, "%s %u%%", state_txt(st.state), pct);
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

    /* 状态栏最后创建：屏幕的最后一个子对象 → 永远画在页面之上 */
    build_status_bar(scr);
}

/* 整界面重建（目前只有切语言用得到：文案和字体同时换，逐条改写太容易漏）。
 * 做法是把屏幕上所有对象清掉重新 build，再把用户送回原来那一页。
 * 注意：refresh_cb 里的"上次值"缓存必须一起复位，否则新 label 上不会写内容。 */
static void ui_app_rebuild(void)
{
    app_id_t keep = s_cur;

    s_sb_last_dev    = -1;
    s_sb_last_dark   = false;
    s_dev_sig        = 0;
    s_dev_have_sig   = false;
    s_dev_last_link  = false;
    s_upg_sig        = 0;
    s_upg_have_sig   = false;
    s_heap_shown     = -1;
    s_in_mem_shown   = -1;
    s_flash_shown    = -1;
    s_fps_shown      = -1;      /* 帧率 label 是新对象，下次 tick 必须重写 */
    s_bar_shown      = -1;
    s_arc_shown      = -1;
    s_busy_shown     = -1;
    s_lan_key        = -1;
    s_lan_drawn      = -1;      /* 图标是新建的对象，必须重画一次 */
    s_lan_fg_drawn   = 0;
    s_home_col_shown = 0xFFFFFFFFu;
    s_upg_col_shown  = 0xFFFFFFFFu;

    /* 这三个是新页面里的新对象，旧指针必须清掉：
     * rebuild 期间若 400 ms 定时器或滑块回调进来，写旧对象就是野指针；
     * 手势那边也会用 s_br_slider 算命中带，留着旧指针更危险。 */
    s_br_slider  = NULL;
    s_br_pct     = NULL;
    s_set_br_val = NULL;

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

    /* 帧率计数：挂在 display 上而不是 screen 上 —— screen 会被 rebuild 清掉，
     * display 的生命周期才是整个应用。也只在这里注册一次。 */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_display_add_event_cb(disp, disp_frame_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, disp_flush_stat_cb, LV_EVENT_FLUSH_START, NULL);
    }

    bsp_display_unlock();

    ESP_LOGI(TAG, "界面就绪：桌面 + 设备 / 升级 / 信息 / 设置（%s，400 ms 刷新）",
             i18n_lang() == LANG_ZH ? "中文" : "English");
    return ESP_OK;
}
