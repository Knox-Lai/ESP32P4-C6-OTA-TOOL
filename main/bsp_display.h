/**
 * @file  bsp_display.h
 * @brief 7 寸 1024x600 MIPI-DSI 屏 + GT911 触摸 + LVGL 端口
 *
 * 为什么不用乐鑫官方 BSP 组件 `espressif/esp32_p4_function_ev_board`：
 * 那个组件会把 esp_video / esp_codec_dev / ek79007 / ili9881c / lt8912b / usb
 * 全部拉进来（esp_video 单独就要编译好几分钟），而我们只需要屏和触摸。
 * 这里只从它里面搬了 DSI 总线配置、JD9165 初始化命令表和 GT911 挂载，
 * 代码来源：esp-bsp v5.2.3 / bsp/esp32_p4_function_ev_board。
 *
 * 板级事实（原理图 + 规格书已核对）：
 *   - 屏 1024x600，MIPI-DSI 2 lane @750Mbps，驱动 IC JD9165，RGB565
 *   - 屏复位 GPIO0，背光 GPIO23（LEDC ch1，20 kHz，10 bit）
 *   - 触摸 GT911，I2C0 SDA=GPIO7 SCL=GPIO8 @400kHz，RST/INT 未接
 *   - DSI PHY 供电走内部 LDO ch3 @2.5V
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 板级引脚 ---------------- */
#define BSP_LCD_H_RES           1024
#define BSP_LCD_V_RES           600

#define BSP_LCD_BACKLIGHT_GPIO  23
#define BSP_LCD_RST_GPIO        0

#define BSP_I2C_PORT            0
#define BSP_I2C_SDA_GPIO        7
#define BSP_I2C_SCL_GPIO        8
#define BSP_I2C_CLK_HZ          400000

/* DSI PHY 的内部 LDO 通道与电压 */
#define BSP_DSI_LDO_CHAN        3
#define BSP_DSI_LDO_MV          2500

/* GT911 上报的原始坐标范围。BSP 注释里写明这块屏的 GT911 固件配的是 800x480，
 * 需要线性映射到 1024x600，否则右下角一大片区域点不到。
 *
 * 注意：这两个值只是"默认值"，实际量程因 GT911 内部配置而异，
 * 不必改这里重新编译 —— 用串口命令 `touch cal x0 x1 y0 y1` 现场定标即可。 */
#define BSP_TOUCH_RAW_X         800
#define BSP_TOUCH_RAW_Y         480

/* 面板是否要转 90 度（swap_xy）。先用 0（1024x600 横向）；若显示方向反了再改 1。 */
#define BSP_UI_SWAP_XY          0

/**
 * @brief 初始化背光、DSI、面板、触摸，并把显示与输入设备挂到 LVGL。
 * @note  内部会初始化 LVGL 端口任务；调用前不要自己调 lv_init()。
 */
esp_err_t bsp_display_init(void);

/** @brief 取 LVGL 互斥锁。timeout_ms = 0 表示不等待（拿不到立刻返回 false）。 */
bool bsp_display_lock(uint32_t timeout_ms);

/** @brief 释放 LVGL 互斥锁 */
void bsp_display_unlock(void);

/** @brief 背光亮度 0~100 */
esp_err_t bsp_display_brightness_set(int percent);

/* ---------------- 触摸定标 ---------------- */

/**
 * @brief 两点定标：给出"屏幕四边"对应的 GT911 原始值。
 *
 * 映射关系（线性，逐轴独立）：
 *   screen_x = (raw_x - x0) * (H-1) / (x1 - x0)
 *   screen_y = (raw_y - y0) * (V-1) / (y1 - y0)
 *
 * 这样同时能吃掉两种误差：
 *   - 量程不符（scale）：x1-x0 不等于屏宽
 *   - 原点偏移（offset）：x0/y0 不为 0（表现为固定方向偏一点）
 *
 * 取法：打开 dump 后点屏幕四个角，记下左上角 (x0,y0) 与右下角 (x1,y1) 的 raw 值。
 *
 * @param x0 屏幕最左列对应的 raw_x  @param x1 屏幕最右列对应的 raw_x
 * @param y0 屏幕最上行对应的 raw_y  @param y1 屏幕最下行对应的 raw_y
 * @note  x1 必须大于 x0、y1 必须大于 y0，否则返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t bsp_display_touch_cal_set(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1);

/** @brief 读回当前定标参数（任一指针可为 NULL） */
void bsp_display_touch_cal_get(uint16_t *x0, uint16_t *x1, uint16_t *y0, uint16_t *y1);

/** @brief 恢复默认量程（0~BSP_TOUCH_RAW_X / 0~BSP_TOUCH_RAW_Y） */
void bsp_display_touch_cal_reset(void);

/** @brief 开关原始坐标打印（定标时打开，平时关掉免得刷屏） */
void    bsp_display_touch_dump(bool on);
bool    bsp_display_touch_dump_state(void);

/** @brief 串口命令用来判断能不能定标 */
bool bsp_display_ready(void);

/* ---------------- 诊断（串口 disp / bl / lcd） ----------------
 *
 * 这几条不是为了功能，是为了把"屏幕闪"这件事定位到具体一层：
 *   UI 冻结 + 纯色   → 静止画面还闪 = 背光 / 面板 / DSI 取数，跟 LVGL 无关
 *   背光恒亮         → 还闪 = 不是 PWM
 *   disp stat        → VSYNC 与整帧计数对不上 = DMA 侧
 *   lcd w            → 现场改 JD9165 寄存器（VCOM / 帧率），不用重编译
 */

/** @brief 冻结/解冻 UI。冻结后 LVGL 不再跑定时器，屏幕内容停止更新。 */
esp_err_t bsp_display_ui_freeze(bool freeze);

/** @brief 用纯色（RGB565）填满整屏，配合 ui_freeze 做静止画面测试 */
esp_err_t bsp_display_fill(uint16_t rgb565);

/** @brief 背光脱离 PWM 恒亮（true）/ 恢复 PWM 调光（false） */
esp_err_t bsp_display_backlight_dc(bool on);

/** @brief 直接写一条 DBI 命令给面板（params 可为 NULL，len 可为 0） */
esp_err_t bsp_display_lcd_write(uint8_t cmd, const uint8_t *params, size_t len);

/** @brief 打印 VSYNC / 整帧计数与折算帧率（两次调用之间取增量） */
void bsp_display_stats(void);

/* ---------------- 面板时序（DPI porch + 像素时钟） ----------------
 *
 * 画面整体错位、多出黑边，根因通常是 SoC 的 HBP/VBP 与面板内部 porch
 * 不一致：SoC 的 HBP 大 → 左边黑边，VBP 大 → 底部黑边。改完存 NVS，
 * **重启生效**（DPI 配置只在面板创建时读一次）。
 *
 * 顺序：out[0..2] = hbp/hpw/hfp，out[3..5] = vbp/vpw/vfp，out[6] = MHz。
 */

/** @brief 读当前（NVS 或默认）时序参数 */
void bsp_display_timing_get(int out[7]);

/** @brief 写入新时序（hbp hpw hfp vbp vpw vfp mhz），重启后生效 */
esp_err_t bsp_display_timing_set(const int in[7]);

/** @brief 擦掉 NVS 里的时序，回到编译期默认值（重启生效） */
esp_err_t bsp_display_timing_reset(void);

#ifdef __cplusplus
}
#endif
