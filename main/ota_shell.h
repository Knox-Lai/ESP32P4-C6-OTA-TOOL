/**
 * @file  ota_shell.h
 * @brief 串口命令行（触摸屏 UI 出来之前的临时入口）
 *
 * 只做三件事：看设备表、发起升级、看升级进度。
 * 有了 LVGL 页面之后这里可以整体删掉，不影响 ota_client / dev_table。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 接管 UART0 接收，创建命令任务（在 app_main 末尾调用） */
esp_err_t ota_shell_init(void);

#ifdef __cplusplus
}
#endif
