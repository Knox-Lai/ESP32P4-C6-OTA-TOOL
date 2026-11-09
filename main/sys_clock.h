/**
 * @file sys_clock.h
 * @brief 极简系统时钟：给 UI 状态栏提供"时间 / 日期"
 *
 * 这块板子没有 RTC 电池、也没有互联网，所以没有任何权威时间源。
 * 策略是：开机时用**固件编译时间**（__DATE__ / __TIME__）做初值，
 * 之后靠 esp_timer 走时；误差大时用串口 `time set` 手动校一次。
 *
 * 时区固定 CST-8（北京时间），不跟随 CONFIG 或环境变量。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化：设时区、用编译时间做初值。需在 UI 启动前调用。 */
void sys_clock_init(void);

/**
 * @brief 手动设置时间（本地时间，北京时间）
 * @return true 成功
 */
bool sys_clock_set(int year, int mon, int day, int hour, int min, int sec);

/** @brief "16:22" */
void sys_clock_time_str(char *buf, size_t n);

/** @brief "11-05 Thu" */
void sys_clock_date_str(char *buf, size_t n);

/** @brief "16:22:05"（串口打印用） */
void sys_clock_full_str(char *buf, size_t n);

/** @brief 自 1970 起的秒数（本地时区已折算） */
int64_t sys_clock_epoch(void);

#ifdef __cplusplus
}
#endif
