/**
 * @file  bsp_display.c
 * @brief 7 寸 1024x600 MIPI-DSI + GT911 + LVGL 端口（精简移植，非完整 BSP）
 *
 * 代码来源：esp-bsp v5.2.3 / bsp/esp32_p4_function_ev_board，
 * 只保留本工程用得到的部分：背光 LEDC、DSI PHY 供电、DSI 总线、DBI、JD9165 面板、
 * GT911 触摸、lvgl_port 挂载。删掉了 HDMI、音频、USB Host、摄像头。
 *
 * 初始化顺序不能变：
 *   LEDC 背光(占空比先 0) -> DSI PHY LDO 上电 -> DSI 总线 -> DBI -> 面板复位/初始化
 *   -> LVGL 端口 -> 挂显示 -> 挂触摸 -> 最后再把背光拉亮
 * 顺序错最典型的表现是白屏或花屏（PHY 没上电就建总线）。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/ledc.h"
#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch_gt911.h"
#include "nvs.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "bsp_display.h"

static const char *TAG = "disp";

#define LCD_LEDC_CH      LEDC_CHANNEL_1
#define LCD_LEDC_TIMER   LEDC_TIMER_1

/* 绘缓冲：单帧缓冲 + 局部刷新（LV_DISPLAY_RENDER_MODE_PARTIAL）。
 *
 * 为什么不走"双帧缓冲 + avoid_tearing + full_refresh"：
 * 那套配置确实能消除撕裂，但代价是 **LVGL 每次刷新都必须整屏重绘**
 * （两张 FB 交替，内容必须各自完整）。于是任何一个小改动 ——
 * 状态栏的帧率数字每秒变一次 —— 都会放大成一次 1024x600x2 = 1.2 MB
 * 的全屏渲染 + 1.2 MB cache 写回，在 PSRAM 上耗时 20~40 ms，
 * 跨过 2 个以上 VSYNC，屏幕中间出现一条"上半旧、下半新"的横线。
 * 每秒一次，看起来就是"主页在闪"，比原来的局部撕裂更明显。
 *
 * 现在回到单 FB + 局部刷新：
 *   - 只有脏区（通常几十到几百像素高）被渲染和写入，耗时 < 1 帧；
 *   - 撕裂依然可能存在，但只发生在脏区那几行上，肉眼不可分辨；
 *   - 真正要做的是把"没必要的重绘"全部掐掉（见 ui_app.c 的判重与死区），
 *     主页静止时 LVGL 的刷新定时器会自己 pause，一帧都不刷。
 *
 * 50 行 = 1024 x 50 x 2 B = 100 KB，放在内部 RAM（buff_spiram = false）。 */
#define LCD_DRAW_BUF_LINES  50

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t s_io   = NULL;   /* DBI 命令通道，LVGL 端口要用 */
static esp_lcd_panel_handle_t    s_panel = NULL;  /* DPI 面板，诊断命令要直接写 FB */
static lv_display_t           *s_disp   = NULL;

/* ---------------- 静止时的刷新与 VSYNC 对齐 ----------------
 *
 * 单帧缓冲 + 局部刷新的固有毛病：CPU 把脏区 memcpy 进 FB 的那一刻，
 * DSI 可能正好在扫描同一片行，屏幕上就出现一条"上半旧、下半新"的接缝。
 * 深色壁纸时这条接缝几乎看不出来，换成淡蓝紫的整屏渐变后对比度被放大，
 * 看起来就是"主页一直在闪"。
 *
 * 正确做法不是回退到双帧缓冲 —— 那要求 LVGL 每帧整屏重绘（LV_DISPLAY_RENDER_MODE_FULL），
 * 1024x600x2 B 的一整帧在 PSRAM 上渲染 + 写回要几十毫秒，帧率会被拖到个位数。
 * 而是把"写 FB"这个动作挪到 VSYNC 之后：VSYNC 一到面板刚从第 0 行开始扫，
 * 脏区通常在屏幕中下部，一次 memcpy（几十 KB）几十微秒就结束，
 * 扫描头还没走到那里数据就已经写完了 —— 接缝消失。
 *
 * 动画期间不做这个等待：一帧动画要刷很多块，每块都等一个 VSYNC 会把 60 fps 拖成 3 fps，
 * 而且动画本来就在动，接缝看不出来。只有"静止页面上的偶发更新"才需要对齐，
 * 那正是眼睛最容易盯住、也最容易看出闪的场景。 */
static SemaphoreHandle_t s_vsync_sem   = NULL;
static volatile bool     s_vsync_seen  = false;
/* 最近一次 VSYNC 的时刻。单位是"esp_timer>>10"（≈1.024 ms 一个 tick）：
 * 不用毫秒是为了避开 ISR 里的 64 位除法，不用 int64 是为了避免 rv32 上
 * 读写撕裂 —— 32 位无符号差值自带回绕，50 天才转一圈。 */
static volatile uint32_t s_vsync_tick  = 0;
static volatile uint32_t s_vsync_cnt   = 0;   /* 累计 VSYNC 次数，仅供诊断 */
static volatile uint32_t s_frame_cnt   = 0;   /* 累计"整帧 DMA 送完"次数，仅供诊断 */

/* 面板把绘制缓冲拷进 FB 完成时回调：替 esp_lvgl_port 通知 LVGL 这一块刷完了。
 * 重新注册回调是"整体覆盖"而不是追加，所以必须原样补回来（见下方 init）。 */
static bool panel_color_done_cb(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

/* 一帧 FB 被 DMA 完整扫完时来一次（单 FB 模式下就是"这一帧结束"）。
 * 它比 VSYNC 更能代表"面板实际帧率"，所以单独计数，disp stat 里对比两者，
 * 数量对不上就说明 DMA 一侧有问题（underrun / 卡住）。 */
static bool panel_frame_cb(esp_lcd_panel_handle_t panel,
                           esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_frame_cnt++;
    return false;
}

/* 每帧一次的 VSYNC：记时刻 + 放行等待中的 flush */
static bool panel_vsync_cb(esp_lcd_panel_handle_t panel,
                           esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    BaseType_t hp = pdFALSE;
    s_vsync_tick = (uint32_t)(esp_timer_get_time() >> 10);
    s_vsync_cnt++;
    s_vsync_seen = true;                 /* 先确认中断真的来了，否则下面只等一次超时 */
    if (s_vsync_sem != NULL) {
        xSemaphoreGiveFromISR(s_vsync_sem, &hp);
    }
    return (hp == pdTRUE);
}

/* 挂在 LV_EVENT_FLUSH_START 上：真正调 flush_cb 之前先等一个 VSYNC。
 *
 * 一轮刷新只对齐第一块：VSYNC 之后面板从第 0 行开始扫，只要所有块都在
 * 随后几毫秒内写完，扫描头还在屏幕上半部，下半部的写入不会被"追上"；
 * 上半部的写入就算被追上，也只是这一帧晚一点显示，不会撕裂。
 * 反过来，如果每块都等一个 VSYNC，一块 50 行 → 一个整屏刷新要 12 × 16.7 ms，
 * 列表重建会卡出 200 ms 的明显停顿，那就得不偿失。
 *
 * 两条判据（以前只有"等"这一条，动画期间干脆不等，等于每帧都有撕窗口）：
 *   1. 距上一次 VSYNC 不到 8 个 tick（≈8 ms）→ 扫描头还在 y<290，直接写；
 *   2. 否则阻塞等下一个 VSYNC，等到再写。
 * 第 1 条是为了不让"等"把 60 fps 锁成 30 fps：渲染 5 ms 的帧刷完以后
 * 距 VSYNC 也就 5 tick，走这条路立刻写，帧率不掉。 */
static uint8_t s_pass_flushed = 0;

static void disp_render_start_cb(lv_event_t *e)
{
    (void)e;
    s_pass_flushed = 0;
}

static void disp_flush_sync_cb(lv_event_t *e)
{
    (void)e;

    if (s_vsync_sem == NULL || !s_vsync_seen) {
        return;
    }
    /* 一轮里只有第一块需要对齐，后面的紧跟其后写就行 */
    if (s_pass_flushed++ > 0) {
        return;
    }

    uint32_t since = (uint32_t)(esp_timer_get_time() >> 10) - s_vsync_tick;
    if (since < 8u) {
        return;
    }

    (void)xSemaphoreTake(s_vsync_sem, 0);                  /* 丢掉上一帧的残留计数 */
    (void)xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(20));  /* 等下一次 VSYNC；超时就照常写，绝不卡死 */
}

/* JD9165 初始化命令表（原样搬自 BSP）。
 * 最后两条 0x11 / 0x29 分别带 120 ms 和 20 ms 延时，面板在这之后才真正出图。 */
static const jd9165_lcd_init_cmd_t s_lcd_cmd[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x38}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x29}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x29}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x05}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x02, (uint8_t[]){0x08}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x16}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x07, (uint8_t[]){0x00}, 1, 0},
    {0x08, (uint8_t[]){0x78}, 1, 0},
    {0x09, (uint8_t[]){0x00}, 1, 0},
    {0x0A, (uint8_t[]){0x04}, 1, 0},
    {0x0B, (uint8_t[]){0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09}, 11, 0},
    {0x0C, (uint8_t[]){0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08}, 11, 0},
    {0x0E, (uint8_t[]){0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F}, 11, 0},
    {0x10, (uint8_t[]){0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E}, 11, 0},
    {0x12, (uint8_t[]){0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x13, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x41, 0x41}, 4, 0},
    {0x15, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0x85}, 1, 0},
    {0x19, (uint8_t[]){0x06, 0x09}, 2, 0},
    {0x1A, (uint8_t[]){0x05, 0x08}, 2, 0},
    {0x1B, (uint8_t[]){0x0A, 0x04}, 2, 0},
    {0x26, (uint8_t[]){0x00}, 1, 0},
    {0x27, (uint8_t[]){0x00}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30}, 14, 0},
    {0x13, (uint8_t[]){0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x05}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x05}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},

    {0x3A, (uint8_t[]){0x55}, 1, 0},      /* 16 bit RGB565 */
    {0x11, (uint8_t[]){0x00}, 1, 120},    /* 退出睡眠，等 120 ms */
    {0x29, (uint8_t[]){0x00}, 1, 20},     /* 开显示，等 20 ms */
};

/* ---------------- 背光 ---------------- */

static int s_bright_percent = 100;   /* 当前背光档位，bl dc off 时用来恢复 */

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LCD_LEDC_TIMER,
        .freq_hz         = 20000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LCD_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "背光定时器配置失败");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "背光通道配置失败");
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (uint32_t)(1023 * percent) / 100;

    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty), TAG, "set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH), TAG, "update duty");

    s_bright_percent = percent;   /* 记住当前档位：bl dc off 要按它恢复 */
    return ESP_OK;
}

/* ---------------- DSI PHY 供电 ---------------- */

static esp_err_t dsi_phy_power_on(void)
{
    static esp_ldo_channel_handle_t ldo = NULL;

    const esp_ldo_channel_config_t cfg = {
        .chan_id    = BSP_DSI_LDO_CHAN,
        .voltage_mv = BSP_DSI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &ldo), TAG, "DSI PHY LDO 申请失败");
    ESP_LOGI(TAG, "DSI PHY 供电 %d mV（LDO ch%d）", BSP_DSI_LDO_MV, BSP_DSI_LDO_CHAN);
    return ESP_OK;
}

/* ---------------- 触摸 ---------------- */

/* GT911 内部配置的分辨率是 800x480，屏是 1024x600。
 * 不做这个映射的话，右下角约 1/4 区域点不动，左侧还会偏。
 *
 * 但"800x480"是 BSP 注释里的说法，实测这块板并不完全对：按 480 去
 * 除会让 Y 轴整体放大，表现为"点在上、响应偏下"。800/480 只是默认值，
 * 实际量程用串口 `touch dump` + `touch cal` 现场测出来再设。
 *
 * 映射模型（逐轴线性，两点定标）：
 *   screen = (raw - r0) * (res - 1) / (r1 - r0)
 * r0/r1 分别是屏幕左/右（上/下）边缘对应的 raw 值，
 * 于是"量程不对（scale）"和"原点不为 0（offset）"两种误差一起被吃掉。 */

static volatile int32_t s_cal_x0 = 0;
static volatile int32_t s_cal_x1 = BSP_TOUCH_RAW_X;
static volatile int32_t s_cal_y0 = 0;
static volatile int32_t s_cal_y1 = BSP_TOUCH_RAW_Y;

static volatile bool     s_touch_dump = false;
static uint32_t          s_dump_last  = 0;      /* dump 节流，ms */
static bool              s_ready      = false;

static uint16_t map_axis(int32_t raw, int32_t r0, int32_t r1, uint16_t res)
{
    int32_t span = r1 - r0;
    if (span == 0) {
        return 0;
    }

    int32_t v = ((raw - r0) * (int32_t)(res - 1)) / span;
    if (v < 0) {
        v = 0;
    } else if (v > (int32_t)(res - 1)) {
        v = (int32_t)(res - 1);
    }
    return (uint16_t)v;
}

static void touch_calibrate(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                            uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    /* 每 120 ms 最多打一条，按住不放也不会刷屏 */
    bool dump = false;
    if (s_touch_dump) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now - s_dump_last) >= 120u) {
            s_dump_last = now;
            dump = true;
        }
    }

    int32_t x0 = s_cal_x0, x1 = s_cal_x1;
    int32_t y0 = s_cal_y0, y1 = s_cal_y1;

    for (uint8_t i = 0; i < *point_num; i++) {
        uint16_t rx = x[i];
        uint16_t ry = y[i];

        x[i] = map_axis(rx, x0, x1, BSP_LCD_H_RES);
        y[i] = map_axis(ry, y0, y1, BSP_LCD_V_RES);

        if (dump) {
            ESP_LOGI(TAG, "触摸 raw=(%u,%u) -> (%u,%u)", rx, ry, x[i], y[i]);
        }
    }
}

/* GT911 的配置表里存着它自己声明的输出量程（0x8048~0x804B），
 * 读出来就知道 raw 坐标的真实范围，不用靠"800x480"这种注释猜。
 *
 * 寄存器布局（GT911 标准配置表）：
 *   0x8047  config version
 *   0x8048  X_Output_Max  低字节   0x8049 高字节
 *   0x804A  Y_Output_Max  低字节   0x804B 高字节
 *
 * 实测意义：若固件里配的是 1024x600 而我们按 480 去除，
 * Y 会被放大 1.25 倍，越往下偏得越多 —— 正是"点在上、响应偏下"的成因。 */
static void gt911_autocal(esp_lcd_panel_io_handle_t tp_io)
{
    uint8_t buf[5] = {0};

    esp_err_t err = esp_lcd_panel_io_rx_param(tp_io, 0x8047, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读不到 GT911 配置表（%s），沿用默认量程 %dx%d",
                 esp_err_to_name(err), BSP_TOUCH_RAW_X, BSP_TOUCH_RAW_Y);
        return;
    }

    uint16_t xmax = (uint16_t)(buf[1] | (buf[2] << 8));
    uint16_t ymax = (uint16_t)(buf[3] | (buf[4] << 8));

    ESP_LOGI(TAG, "GT911 配置：cfg_ver=%u，固件量程 %ux%u", buf[0], xmax, ymax);

    if (xmax == 0 || ymax == 0 || xmax > 4096 || ymax > 4096) {
        ESP_LOGW(TAG, "量程不合理（%ux%u），沿用默认 %dx%d",
                 xmax, ymax, BSP_TOUCH_RAW_X, BSP_TOUCH_RAW_Y);
        return;
    }

    s_cal_x0 = 0;
    s_cal_x1 = (int32_t)xmax;
    s_cal_y0 = 0;
    s_cal_y1 = (int32_t)ymax;

    ESP_LOGI(TAG, "按固件量程自动定标：raw_x 0~%u -> 0~%d，raw_y 0~%u -> 0~%d",
             xmax, BSP_LCD_H_RES - 1, ymax, BSP_LCD_V_RES - 1);
}

static esp_err_t touch_init(lv_display_t *disp)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_GPIO,
        .scl_io_num = BSP_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C 总线创建失败");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = -1,             /* 板上 RST/INT 均未接 */
        .int_gpio_num = -1,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = {
            .swap_xy  = BSP_UI_SWAP_XY, /* 触摸坐标系必须和显示方向一致 */
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .process_coordinates = touch_calibrate,
    };

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = BSP_I2C_CLK_HZ;

    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &tp_io), TAG, "GT911 IO 创建失败");

    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp), TAG, "GT911 初始化失败");

    /* 在把触摸交给 LVGL 之前读一次配置表：此时没有并发轮询，I2C 不会被抢 */
    gt911_autocal(tp_io);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = tp,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        ESP_LOGW(TAG, "触摸挂到 LVGL 失败，界面只能看不能点");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GT911 就绪（I2C%d SDA=%d SCL=%d @%dHz），raw %dx%d -> %dx%d（偏移用 touch 命令定标）",
             BSP_I2C_PORT, BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_CLK_HZ,
             BSP_TOUCH_RAW_X, BSP_TOUCH_RAW_Y, BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

/* ---------------- 面板时序（可调） ----------------
 *
 * DSI 视频模式下 SoC 和面板各有一套 porch：SoC 发多少消隐像素/行，
 * 面板内部寄存器认为"消隐多久"才开始有效区。两边不一致，画面就整体错位，
 * 多出来的那部分就是黑边：
 *
 *   SoC HBP > 面板内部 HBP → 面板提前开始取有效像素，前面取到的是消隐黑
 *                            数据 → **左边一条黑边**（右边丢对应像素）
 *   SoC VBP > 面板内部 VBP → 同理，**底部一条黑边**
 *
 * "左边 + 底部同时有黑边"就是 HBP 与 VBP 都设大了的典型症状。
 * 默认值是点亮这块屏时试出来的，但面板换代/换批次内部值会变，
 * 所以做成可调：串口 `disp timing ...` 现场扫，存 NVS 重启生效，
 * 不用改代码重编译。
 */

typedef struct {
    int32_t hbp, hpw, hfp;
    int32_t vbp, vpw, vfp;
    int32_t mhz;                 /* DPI 像素时钟 */
} disp_timing_t;

#define DISP_NS     "disp"
#define DISP_KEY    "timing"

static const disp_timing_t TIMING_DEF = {136, 24, 160, 21, 2, 12, 52};

static disp_timing_t timing_load(void)
{
    disp_timing_t t = TIMING_DEF;

    nvs_handle_t h;
    if (nvs_open(DISP_NS, NVS_READONLY, &h) != ESP_OK) {
        return t;
    }
    size_t len = sizeof(t);
    if (nvs_get_blob(h, DISP_KEY, &t, &len) != ESP_OK || len != sizeof(t)) {
        t = TIMING_DEF;
    }
    nvs_close(h);

    /* 粗校验：0 或离谱的值会把屏直接搞黑，宁可用默认 */
    if (t.hbp < 0 || t.hpw <= 0 || t.hfp < 0 ||
        t.vbp < 0 || t.vpw <= 0 || t.vfp < 0 ||
        t.mhz < 20 || t.mhz > 120) {
        ESP_LOGW(TAG, "NVS 里的面板时序不合理（h %d/%d/%d v %d/%d/%d %dMHz），改用默认",
                 (int)t.hbp, (int)t.hpw, (int)t.hfp,
                 (int)t.vbp, (int)t.vpw, (int)t.vfp, (int)t.mhz);
        t = TIMING_DEF;
    }
    return t;
}

void bsp_display_timing_get(int out[7])
{
    disp_timing_t t = timing_load();
    out[0] = (int)t.hbp;  out[1] = (int)t.hpw;  out[2] = (int)t.hfp;
    out[3] = (int)t.vbp;  out[4] = (int)t.vpw;  out[5] = (int)t.vfp;
    out[6] = (int)t.mhz;
}

esp_err_t bsp_display_timing_set(const int in[7])
{
    if (in[0] < 0 || in[1] <= 0 || in[2] < 0 ||
        in[3] < 0 || in[4] <= 0 || in[5] < 0 ||
        in[6] < 20 || in[6] > 120) {
        return ESP_ERR_INVALID_ARG;
    }

    const disp_timing_t t = {
        .hbp = in[0], .hpw = in[1], .hfp = in[2],
        .vbp = in[3], .vpw = in[4], .vfp = in[5],
        .mhz = in[6],
    };

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(DISP_NS, NVS_READWRITE, &h), TAG, "打开 NVS");
    esp_err_t err = nvs_set_blob(h, DISP_KEY, &t, sizeof(t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t bsp_display_timing_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(DISP_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_OK;               /* 没写过就是默认值，不算失败 */
    }
    (void)nvs_erase_key(h, DISP_KEY);
    (void)nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ---------------- 面板 ---------------- */

static esp_err_t panel_init(esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_ERROR(dsi_phy_power_on(), TAG, "DSI PHY 上电失败");

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = 2,
        .phy_clk_src        = 0,
        .lane_bit_rate_mbps = 750,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "DSI 总线创建失败");

    /* 命令走 DBI */
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io), TAG, "DBI 创建失败");
    s_io = io;

    /* 像素走 DPI。时序从 NVS 读（没有就是 TIMING_DEF），见文件里"面板时序"一节：
     * 默认 h 136/24/160、v 21/2/12 是这块屏点亮时的实测值。 */
    const disp_timing_t tm = timing_load();

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = (uint32_t)tm.mhz,
        .virtual_channel    = 0,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,          /* 单帧缓冲：单 FB 直写 + 局部刷新，见上方说明 */
        .video_timing = {
            .h_size            = BSP_LCD_H_RES,
            .v_size            = BSP_LCD_V_RES,
            .hsync_back_porch  = (uint32_t)tm.hbp,
            .hsync_pulse_width = (uint32_t)tm.hpw,
            .hsync_front_porch = (uint32_t)tm.hfp,
            .vsync_back_porch  = (uint32_t)tm.vbp,
            .vsync_pulse_width = (uint32_t)tm.vpw,
            .vsync_front_porch = (uint32_t)tm.vfp,
        },
    };

    /* 帧率 = mhz / (HTOTAL × VTOTAL)，用整数算，避免 newlib-nano 没有 %f */
    const uint32_t ht = (uint32_t)BSP_LCD_H_RES + (uint32_t)tm.hbp + (uint32_t)tm.hpw + (uint32_t)tm.hfp;
    const uint32_t vt = (uint32_t)BSP_LCD_V_RES + (uint32_t)tm.vbp + (uint32_t)tm.vpw + (uint32_t)tm.vfp;
    const uint32_t fps10 = (uint32_t)(((uint64_t)(uint32_t)tm.mhz * 10000000ULL) / ((uint64_t)ht * vt));
    ESP_LOGI(TAG, "DPI 时序：%u MHz，H %d/%d/%d（总 %u），V %d/%d/%d（总 %u）→ %u.%u Hz",
             (unsigned)tm.mhz,
             (int)tm.hbp, (int)tm.hpw, (int)tm.hfp, (unsigned)ht,
             (int)tm.vbp, (int)tm.vpw, (int)tm.vfp, (unsigned)vt,
             (unsigned)(fps10 / 10), (unsigned)(fps10 % 10));

    jd9165_vendor_config_t vendor_cfg = {
        .init_cmds      = s_lcd_cmd,
        .init_cmds_size = sizeof(s_lcd_cmd) / sizeof(s_lcd_cmd[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };

    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &dev_cfg, ret_panel), TAG, "JD9165 创建失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*ret_panel), TAG, "面板复位失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*ret_panel), TAG, "面板初始化失败");

    ESP_LOGI(TAG, "JD9165 就绪：%dx%d RGB565 2-lane@750Mbps", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

/* ---------------- 对外 ---------------- */

/* 3 秒后数一次 VSYNC：为 0 说明"对齐"根本没生效，屏幕还是盲写的老样子。 */
static void vsync_selfcheck_cb(void *arg)
{
    (void)arg;

    if (!s_vsync_seen) {
        ESP_LOGE(TAG, "3 秒内一次 VSYNC 都没收到，flush 对齐无效（还是盲写，会撕裂）");
        ESP_LOGE(TAG, "  检查 CONFIG_LCD_DSI_ISR_CACHE_SAFE：打开时回调必须在 IRAM");
    }
}

esp_err_t bsp_display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "背光初始化失败");

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(panel_init(&panel), TAG, "面板初始化失败");
    s_panel = panel;

    /* LVGL 端口任务：优先级 3 —— 必须低于 dev_rx(4)，UI 刷新不能把收包挤掉。
     * 栈放内部 RAM，LVGL 的对象分配才走 PSRAM。 */
    const lvgl_port_cfg_t port_cfg = {
        .task_priority   = 3,
        .task_stack      = 8192,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .task_affinity   = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL 端口初始化失败");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = s_io,             /* DSI 面板的 DBI 命令通道 */
        .panel_handle   = panel,
        .control_handle = NULL,
        .buffer_size    = BSP_LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer  = false,
        .hres           = BSP_LCD_H_RES,
        .vres           = BSP_LCD_V_RES,
        .monochrome     = false,
        .rotation = {
            .swap_xy  = BSP_UI_SWAP_XY,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = false,
            /* 局部刷新：只重绘脏区。整屏重绘会把"每秒一个数字"放大成
             * 一次 1.2 MB 全屏渲染，那才是主页闪的直接来源。 */
            .full_refresh = false,
            .swap_bytes  = false,
            .sw_rotate   = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        /* 关掉 avoid_tearing —— 它要求配 full_refresh，两者是绑定的 */
        .flags = { .avoid_tearing = false },
    };

    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "LVGL 显示注册失败");
        return ESP_FAIL;
    }

    /* 把 flush 与 VSYNC 对齐（详见文件上方说明）。
     * 注册失败不算致命：退回原来的"立即写"，最多还是老样子的轻微接缝。 */
    s_vsync_sem = xSemaphoreCreateCounting(1, 0);
    if (s_vsync_sem != NULL) {
        const esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_color_trans_done  = panel_color_done_cb,
            .on_frame_buf_complete = panel_frame_cb,
            .on_vsync             = panel_vsync_cb,
        };
        /* 这个 API 是"整体覆盖"：esp_lvgl_port 注册的 on_color_trans_done
         * 必须原样补回来，漏掉的话 lv_display_flush_ready 永远不来，界面会直接卡死。 */
        esp_err_t err = esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, s_disp);
        if (err == ESP_OK) {
            lv_display_add_event_cb(s_disp, disp_render_start_cb, LV_EVENT_RENDER_START, NULL);
            lv_display_add_event_cb(s_disp, disp_flush_sync_cb, LV_EVENT_FLUSH_START, NULL);
            ESP_LOGI(TAG, "flush 与 VSYNC 已对齐");
        } else {
            ESP_LOGW(TAG, "VSYNC 回调注册失败(%s)，flush 退回立即写", esp_err_to_name(err));
            vSemaphoreDelete(s_vsync_sem);
            s_vsync_sem = NULL;
        }
    }

    /* 自检：注册成功 ≠ 中断真的在来。CONFIG_LCD_DSI_ISR_CACHE_SAFE 打开时，
     * 回调必须放在 IRAM、user_ctx 必须在内部 RAM，否则注册直接被拒；
     * 就算注册过了，中断被别的驱动关掉也一样收不到。3 秒后数一次，
     * 一个都没有就说明"对齐"是假的 —— 这时候屏幕上看到的还是盲写的老样子。 */
    if (s_vsync_sem != NULL) {
        const esp_timer_create_args_t targs = {
            .callback = vsync_selfcheck_cb,
            .name     = "vsync_chk",
        };
        esp_timer_handle_t t = NULL;
        if (esp_timer_create(&targs, &t) == ESP_OK) {
            esp_timer_start_once(t, 3000000);
        }
    }

    /* 触摸失败不影响出图：屏能看，只是点不动 */
    if (touch_init(s_disp) != ESP_OK) {
        ESP_LOGW(TAG, "无触摸输入，界面仅显示");
    }

    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(100), TAG, "背光点亮失败");
    s_ready = true;
    ESP_LOGI(TAG, "显示就绪");
    return ESP_OK;
}

bool bsp_display_ready(void)
{
    return s_ready;
}

/* ---------------- 触摸定标（串口可调） ---------------- */

esp_err_t bsp_display_touch_cal_set(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1)
{
    if (x1 <= x0 || y1 <= y0) {
        ESP_LOGE(TAG, "定标参数非法：x1(%u) 必须 > x0(%u)，y1(%u) 必须 > y0(%u)", x1, x0, y1, y0);
        return ESP_ERR_INVALID_ARG;
    }

    s_cal_x0 = x0;
    s_cal_x1 = x1;
    s_cal_y0 = y0;
    s_cal_y1 = y1;

    ESP_LOGI(TAG, "触摸定标：raw_x %" PRId32 "~%" PRId32 " -> 0~%d，raw_y %" PRId32 "~%" PRId32 " -> 0~%d",
             (int32_t)x0, (int32_t)x1, BSP_LCD_H_RES - 1,
             (int32_t)y0, (int32_t)y1, BSP_LCD_V_RES - 1);
    return ESP_OK;
}

void bsp_display_touch_cal_get(uint16_t *x0, uint16_t *x1, uint16_t *y0, uint16_t *y1)
{
    if (x0) {
        *x0 = (uint16_t)s_cal_x0;
    }
    if (x1) {
        *x1 = (uint16_t)s_cal_x1;
    }
    if (y0) {
        *y0 = (uint16_t)s_cal_y0;
    }
    if (y1) {
        *y1 = (uint16_t)s_cal_y1;
    }
}

void bsp_display_touch_cal_reset(void)
{
    s_cal_x0 = 0;
    s_cal_x1 = BSP_TOUCH_RAW_X;
    s_cal_y0 = 0;
    s_cal_y1 = BSP_TOUCH_RAW_Y;
    ESP_LOGI(TAG, "触摸定标已复位：raw %dx%d", BSP_TOUCH_RAW_X, BSP_TOUCH_RAW_Y);
}

void bsp_display_touch_dump(bool on)
{
    s_touch_dump = on;
    s_dump_last  = 0;
    ESP_LOGI(TAG, "原始坐标打印：%s", on ? "开" : "关");
}

bool bsp_display_touch_dump_state(void)
{
    return s_touch_dump;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

/* ================== 诊断（串口 disp / bl / lcd 命令用） ==================
 *
 * 目的只有一个：把"闪"这件事一刀切开 ——
 *   disp solid on  把 LVGL 冻住、FB 填成纯色。屏幕这时候一个像素都不会被重写。
 *                  如果此时**还在闪**，那就和 LVGL、刷新、撕裂全都无关，
 *                  只能是背光 / 面板 / DSI 取数（underrun）三者之一。
 *   bl dc on       让背光脱离 PWM 恒亮，排除"20 kHz PWM 与行扫描拍频"。
 *   disp stat      看 VSYNC 和整帧计数是否对得上（对不上就是 DMA 侧的问题）。
 *   lcd w ...      直接写 JD9165 寄存器，现场调 VCOM / 帧率用，不用重编译。
 */

/* 冻结 / 解冻 UI：冻结后 LVGL 不再跑定时器，等于停止一切重绘。 */
esp_err_t bsp_display_ui_freeze(bool freeze)
{
    if (s_disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_display_lock(2000)) {
        ESP_LOGW(TAG, "冻结 UI 时拿不到 LVGL 锁");
        return ESP_ERR_TIMEOUT;
    }
    lv_timer_enable(!freeze);
    if (!freeze) {
        /* 解冻必须整屏重画一次：冻结期间 FB 被纯色盖过，LVGL 并不知道 */
        lv_obj_invalidate(lv_screen_active());
    }
    bsp_display_unlock();

    ESP_LOGW(TAG, "UI %s", freeze ? "已冻结（屏幕内容不再更新）" : "已恢复");
    return ESP_OK;
}

/* 用纯色填满整屏。50 行一块，避免为了 1.2 MB 去申请大缓冲。
 * 返回后屏幕上就是一块纯色，配合 bsp_display_ui_freeze(true) 使用。 */
esp_err_t bsp_display_fill(uint16_t rgb565)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int lines = 50;
    const size_t px = (size_t)BSP_LCD_H_RES * lines;
    uint16_t *buf = heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < px; i++) {
        buf[i] = rgb565;
    }

    for (int y = 0; y < BSP_LCD_V_RES; y += lines) {
        int y2 = (y + lines <= BSP_LCD_V_RES) ? (y + lines) : BSP_LCD_V_RES;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H_RES, y2, buf);
    }

    heap_caps_free(buf);
    return ESP_OK;
}

/* 背光脱离 PWM：ledc_stop 会把引脚按空闲电平钉死（idle_level=1 → 恒亮）。
 * 用来判断"闪是不是 20 kHz PWM 引起的"；关掉后要重新配一次通道才回到调光。 */
esp_err_t bsp_display_backlight_dc(bool on)
{
    if (on) {
        ESP_RETURN_ON_ERROR(ledc_stop(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, 1), TAG, "停止 LEDC");
        ESP_LOGW(TAG, "背光已切成恒亮（无 PWM）");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "重配背光通道");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(s_bright_percent), TAG, "恢复亮度");
    ESP_LOGW(TAG, "背光已恢复 PWM 调光（%d%%）", s_bright_percent);
    return ESP_OK;
}

/* 直接写一条 DBI 命令到 JD9165（VCOM、帧率、极性反转都在这些页寄存器里）。 */
esp_err_t bsp_display_lcd_write(uint8_t cmd, const uint8_t *params, size_t len)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_io_tx_param(s_io, cmd, params, len);
}

/* 打一行统计：VSYNC 与整帧计数的"自上次调用以来"增量 + 折算帧率。
 * 两个数应当接近 60/s；VSYNC 有而整帧没有 = DMA 卡住，
 * 两个都远低于 60 = 面板进入了低频模式或者时钟没配对。 */
void bsp_display_stats(void)
{
    static uint32_t last_v = 0, last_f = 0;
    static int64_t  last_us = 0;

    uint32_t v = s_vsync_cnt;
    uint32_t f = s_frame_cnt;
    int64_t  now = esp_timer_get_time();

    if (last_us == 0) {
        last_us = now;
        last_v  = v;
        last_f  = f;
        printf("[显示] 起始计数 vsync=%u frame=%u\r\n", (unsigned)v, (unsigned)f);
        return;
    }

    int64_t dt = now - last_us;
    if (dt <= 0) {
        dt = 1;
    }
    unsigned dv = v - last_v;
    unsigned df = f - last_f;
    unsigned hz_v = (unsigned)((uint64_t)dv * 1000000ull / (uint64_t)dt);
    unsigned hz_f = (unsigned)((uint64_t)df * 1000000ull / (uint64_t)dt);

    printf("[显示] %u ms 内 vsync=%u(%u Hz) frame=%u(%u Hz) 累计 vsync=%u frame=%u\r\n",
           (unsigned)(dt / 1000), (unsigned)dv, hz_v, (unsigned)df, hz_f,
           (unsigned)v, (unsigned)f);
    printf("       背光 duty=%" PRIu32 "/1023  帧缓冲=1（单 FB）\r\n",
           (uint32_t)ledc_get_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));

    last_us = now;
    last_v  = v;
    last_f  = f;
}
