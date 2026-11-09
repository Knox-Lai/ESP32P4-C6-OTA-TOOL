/**
 * @file  ui_app.h
 * @brief iPadOS 风格界面：桌面 + Devices / Upgrade / Info 三个应用页
 *
 * 结构：
 *   桌面  —— 渐变壁纸、两张 widget 卡（网络 / 最近会话）、三个 App 图标、底部浮条
 *   Devices —— 设备卡片列表，点进去看详情
 *   Upgrade —— 左侧选设备、右侧进度环，一键发起升级（合成镜像）
 *   Info    —— 网络参数、堆占用、运行时长
 *
 * 刷新策略：单个 400 ms 定时器。设备表按指纹比较，内容不变就不重建列表，
 * 避免每拍都把整页对象删了重建（那会让 LVGL 任务吃掉 CPU，影响收包）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建界面并启动刷新定时器。必须在 bsp_display_init() 之后调用。 */
esp_err_t ui_app_start(void);

#ifdef __cplusplus
}
#endif
