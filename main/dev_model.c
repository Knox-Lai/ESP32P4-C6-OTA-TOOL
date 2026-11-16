/**
 * @file  dev_model.c
 * @brief 型号白名单匹配（归一化 + 三级降级）
 */
#include "dev_model.h"

#include <stdio.h>
#include <string.h>

/* 归一化后的缓冲长度：原始串最长 16 B，归一化只会更短 */
#define NORM_MAX   40

/* 前缀/包含比对的最小长度，防止 "VEN" 这类三字母误命中 VENTRIX */
#define PREFIX_MIN 4
#define SUBSTR_MIN 6

typedef struct {
    fw_model_t  id;
    const char *alias;
} model_alias_t;

/* 顺序不重要（三级比对各自全表扫一遍），先按最可能的写法排 */
static const model_alias_t ALIASES[] = {
    { FW_MODEL_VENTRIX, "VENTRIXCPU" },
    { FW_MODEL_VENTRIX, "VENTRIX" },

    { FW_MODEL_CS_XFMR, "COLORSCAPINGTRANSFORMER" },
    { FW_MODEL_CS_XFMR, "CSXFMR" },
    { FW_MODEL_CS_XFMR, "TRANSFORMER" },

    { FW_MODEL_WAC_GW,  "WACHOMEGATEWAY" },
    { FW_MODEL_WAC_GW,  "WACHG" },
    { FW_MODEL_WAC_GW,  "HOMEGATEWAY" },
};

#define ALIAS_COUNT (sizeof(ALIASES) / sizeof(ALIASES[0]))

/* 转大写 + 只保留 [A-Z0-9]，其余（空格 / - / _ / . / 非 ASCII）一律丢弃。
 * 设备可能报 "ventrix  cpu"、"VENTRIX-CPU"、"Ventrix_Cpu"，归一后都是 "VENTRIXCPU"。 */
static size_t normalize(const char *raw, char *out, size_t cap)
{
    size_t n = 0;

    for (const char *p = raw; *p != '\0' && n + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 32);
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            continue;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

fw_model_t dev_model_match(const char *raw)
{
    char norm[NORM_MAX];
    char al[NORM_MAX];

    if (raw == NULL) {
        return FW_MODEL_UNKNOWN;
    }
    if (normalize(raw, norm, sizeof(norm)) == 0) {
        return FW_MODEL_UNKNOWN;
    }

    /* 一级：完全相等 */
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        normalize(ALIASES[i].alias, al, sizeof(al));
        if (strcmp(norm, al) == 0) {
            return ALIASES[i].id;
        }
    }

    /* 二级：前缀（"VENTRIXCPU2" 命中 "VENTRIXCPU"）。
     * 两侧长度都要 >= PREFIX_MIN，否则 "VEN" 会把 VENTRIX 命中。 */
    size_t ln = strlen(norm);
    if (ln >= PREFIX_MIN) {
        for (size_t i = 0; i < ALIAS_COUNT; i++) {
            normalize(ALIASES[i].alias, al, sizeof(al));
            size_t la = strlen(al);
            if (la < PREFIX_MIN) {
                continue;
            }
            if (strncmp(norm, al, la) == 0 || strncmp(al, norm, ln) == 0) {
                return ALIASES[i].id;
            }
        }
    }

    /* 三级：包含（"COLORSCAPING-TRANSFORMER-V2" 命中 "COLORSCAPINGTRANSFORMER" 的子串）。
     * 两个方向都要卡长度，否则 norm="CS" 会命中别名 "CSXFMR" 的子串。 */
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        normalize(ALIASES[i].alias, al, sizeof(al));
        size_t la = strlen(al);
        if (la >= SUBSTR_MIN && strstr(norm, al) != NULL) {
            return ALIASES[i].id;
        }
        if (ln >= SUBSTR_MIN && strstr(al, norm) != NULL) {
            return ALIASES[i].id;
        }
    }

    return FW_MODEL_UNKNOWN;
}

const char *dev_model_canon(fw_model_t m)
{
    switch (m) {
    case FW_MODEL_VENTRIX: return "VENTRIX CPU";
    case FW_MODEL_CS_XFMR: return "COLORSCAPING Transformer";
    case FW_MODEL_WAC_GW:  return "WAC HOME GATEWAY";
    default:               return "UNKNOWN";
    }
}

const char *dev_model_display(const char *raw, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return "";
    }

    fw_model_t m = dev_model_match(raw);
    if (m != FW_MODEL_UNKNOWN) {
        snprintf(buf, cap, "%s", dev_model_canon(m));
    } else {
        snprintf(buf, cap, "%s", (raw != NULL) ? raw : "");
    }
    return buf;
}
