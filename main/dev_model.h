/**
 * @file  dev_model.h
 * @brief 三个在售型号的白名单与匹配
 *
 * 为什么需要它：设备自报的 model 只是 16 字节 ASCII（见 dev_identity），
 * 不同固件版本/不同批次的写法并不统一（"VENTRIX CPU" / "ventrix-cpu" / "VENTRIXCPU"）。
 * 直接字符串相等会在产线上把同一型号判成"未知"。
 *
 * 匹配不变量：
 *   归一化 = 转大写 + 丢弃 空白/-/_/./: 等分隔符，然后按
 *   **完全相等 > 前缀 > 包含** 三级降级比对。
 *
 * 重要：白名单**不是升级门槛**。未命中的型号照样进设备表、照样能手动选固件升级，
 * 只是不做自动匹配、UI 显示原始字符串。产线不因字符串差异被卡死。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_MODEL_UNKNOWN = 0,
    FW_MODEL_VENTRIX,      /* VENTRIX CPU */
    FW_MODEL_CS_XFMR,      /* COLORSCAPING Transformer */
    FW_MODEL_WAC_GW,       /* WAC HOME GATEWAY */
    FW_MODEL_COUNT,
} fw_model_t;

/**
 * @brief 把设备自报的原始 model 串映射到枚举。
 * @param raw  可为 NULL 或空串（返回 FW_MODEL_UNKNOWN）
 * @note  纯函数、无静态状态，可在任意任务里调用
 */
fw_model_t dev_model_match(const char *raw);

/** @brief 枚举 → 规范显示名（ASCII，如 "VENTRIX CPU"）；UNKNOWN 返回 "UNKNOWN" */
const char *dev_model_canon(fw_model_t m);

/**
 * @brief 给 UI 用：命中则写规范名，未命中则原样写 raw。
 * @param buf/cap  输出缓冲；调用方保证 cap >= 24 才不会被截断
 */
const char *dev_model_display(const char *raw, char *buf, size_t cap);

/** @brief 型号是否属于白名单（等价于 dev_model_match() != UNKNOWN） */
static inline int dev_model_known(const char *raw)
{
    return dev_model_match(raw) != FW_MODEL_UNKNOWN;
}

#ifdef __cplusplus
}
#endif
