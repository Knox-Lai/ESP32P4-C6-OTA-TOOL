/**
 * @file  i18n.h
 * @brief 界面文案与字体：中英双语切换
 *
 * 为什么要单独一层：
 *   LVGL 内置 Montserrat 只有拉丁字形，中文全部画成方块。要让界面出中文，
 *   必须自带一份中文字库（用 tools/fontgen 从 simhei.ttf 子集化生成）。
 *   而"换语言"实际是两件事同时发生 —— 文案换表 + 字体换文件，所以把
 *   这两件事绑在一个模块里，页面上只需要 i18n_t(id) 和 UI_F20 两个宏。
 *
 * 字体选择规则：
 *   英文 → LVGL 自带 Montserrat（字形更规整，体积为 0）
 *   中文 → 生成的 simhei 子集（含 ASCII，所以数字/IP/单位在中文下也能排）
 *
 * 语言选择存在 NVS（namespace "ui"，key "lang"），掉电保持。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_EN = 0,
    LANG_ZH = 1,
} ui_lang_t;

/* ---------------- 文案 ID ----------------
 * 命名：A_ 应用名 / H_ 桌面 widget / D_ 设备页 / DT_ 详情页 /
 *       U_ 升级页 / I_ 信息页 / ST_ OTA 状态 / SET_ 设置页 / S_ 通用 */
typedef enum {
    /* 通用 */
    S_BACK = 0,
    S_DEV_COUNT,          /* "%d device(s)" */
    S_NO_LINK_HINT,       /* 网线没插 */
    S_NO_DEVICE_HINT,     /* 链路在但没有设备 */
    S_UNKNOWN_HOST,
    S_LEASES_ONLY,        /* 只有 DHCP 租约，没有发现应答 */
    S_NO_DEVICE,
    S_UNKNOWN,
    S_DEVICE_GONE,
    S_LINK_UP,
    S_LINK_DOWN,

    /* 应用名 */
    A_DEVICES,
    A_UPGRADE,
    A_INFO,
    A_SETTINGS,

    /* 桌面 */
    H_NETWORK,
    H_LINK,
    H_DHCP_LEASE,
    H_FREE_HEAP,
    H_LAST_SESSION,
    H_NO_SESSION,
    H_TAGLINE,            /* 浮条上的工具副标题 */

    /* 设备列表 */
    D_TITLE,
    D_FW,                 /* "fw %u.%u.%u.%u" */
    D_FW_NONE,

    /* 设备详情 */
    DT_TITLE,
    DT_MODEL,
    DT_IP,
    DT_FIRMWARE,
    DT_HARDWARE,
    DT_FLASH,
    DT_CAPS,
    DT_UID,
    DT_HOST,
    DT_MAC,
    DT_UPGRADE_THIS,

    /* 升级 */
    U_TITLE,
    U_SELECT_DEVICE,
    U_START,
    U_READY,
    U_SELECT_HINT,
    U_SUCCESS_HINT,
    U_ERR_FMT,            /* "error 0x%02x ..." */

    /* 信息 */
    I_TITLE,
    I_NETWORK,
    I_ADDRESS,
    I_NETMASK,
    I_GATEWAY,
    I_LINK,
    I_LEASE,
    I_SYSTEM,
    I_RAM,                /* 内部 RAM 占用（配进度条） */
    I_RAM_DETAIL,         /* "free %u KB / %u KB" */
    I_PSRAM,
    I_FLASH,              /* 本机 SPI Flash 容量 */
    I_MIN_HEAP,
    I_UPTIME,
    I_DEVICES,
    I_OTA_STATE,

    /* OTA 状态词 */
    ST_IDLE,
    ST_CONNECT,
    ST_HANDSHAKE,
    ST_STREAMING,
    ST_VERIFYING,
    ST_REBOOTING,
    ST_WAIT_ONLINE,
    ST_SUCCESS,
    ST_FAILED,

    /* 设置页 */
    SET_TITLE,
    SET_DISPLAY,          /* "显示"分组标题（当前已并入"通用"，先留着备用） */
    SET_BRIGHTNESS,       /* 亮度（行标题 / 子页标题共用） */
    SET_GENERAL,          /* "通用"分组标题 */
    SET_LANGUAGE,
    SET_LANG_ZH,
    SET_LANG_EN,
    SET_ABOUT,
    SET_ABOUT_HINT,

    /* 桌面"系统状态"卡（CPU 折线 / 内存 / TF 卡）
     * 追加在末尾而不是插进"桌面"那一段：枚举整体后移会让 i18n.c 的
     * 顺序表全部错位，而这些 ID 没有顺序上的意义。 */
    H_SYS_STATUS,
    H_CPU,
    H_MEM,
    H_TF,
    H_TF_NONE,

    I18N_STR_MAX
} i18n_str_t;

/* ---------------- 字体 ---------------- */

/* 生成的中文字库（tools/fontgen/gen_fonts.py 产出，HarmonyOS Sans SC Medium 子集） */
extern const lv_font_t ui_font_zh_16;
extern const lv_font_t ui_font_zh_22;
extern const lv_font_t ui_font_zh_30;

/* iconfont 子集：桌面 App 图标（40 px，字形已做视觉归一化）
 * 与状态栏/列表小图标（24 px） */
extern const lv_font_t ui_icon_40;
extern const lv_font_t ui_icon_24;

/**
 * 三档界面字号。中文和英文各取适合自己字面的号数：
 *   英文 Montserrat   14 / 20 / 28
 *   中文 HarmonyOS    16 / 22 / 30
 * 汉字的字面比拉丁小、笔画密，和拉丁用同一个 px 会显小，所以中文抬 2 px。
 */
typedef enum {
    UI_TXT_SMALL = 0,   /* 标签、副文字 */
    UI_TXT_BASE,        /* 行值、按钮文字 */
    UI_TXT_LARGE,       /* 大数值、进度百分比 */
} ui_txt_size_t;

#define UI_F_SMALL  i18n_font(UI_TXT_SMALL)
#define UI_F_BASE   i18n_font(UI_TXT_BASE)
#define UI_F_LARGE  i18n_font(UI_TXT_LARGE)

/* ---------------- 图标码位（iconfont.ttf 的 UTF-8 编码） ---------------- */
#define ICO_LINK        "\xEE\x98\xB1"   /* e631 网络接口（以太网口） */
#define ICO_DISCONNECT  "\xEE\x9F\xA1"   /* e7e1 disconnect  */
#define ICO_DEVICES     "\xEE\xAE\xB7"   /* ebb7 设备管理     */
#define ICO_UPLOAD      "\xEE\x9F\x99"   /* e7d9 cloud-upload */
#define ICO_INFO        "\xEE\x9A\x9E"   /* e69e information */
#define ICO_SETTINGS    "\xEE\xAE\x8D"   /* eb8d 设置        */
#define ICO_LANG        "\xEE\x99\x94"   /* e654 多语言（设置里的"语言"行） */
#define ICO_LANG_EN     "\xEE\xAF\xA0"   /* ebe0 英文        */
#define ICO_LANG_ZH     "\xEE\xAF\xA2"   /* ebe2 中文        */
#define ICO_BRIGHT      "\xEE\x9A\x98"   /* e698 brightness（亮度行 / 亮度子页的太阳） */

/* ---------------- API ---------------- */

/** @brief 从 NVS 读语言设置（没有记录时默认英文）。需在 nvs_flash_init 之后调用。 */
void i18n_init(void);

/** @brief 当前语言 */
ui_lang_t i18n_lang(void);

/**
 * @brief 切换语言并写 NVS（内存 + 持久化一起做）。
 *
 * 注意：nvs_commit 有几百字节的栈需求，如果在 LVGL 任务里调用要把栈算进去。
 * 界面侧推荐用 i18n_set_mem() + 独立任务调用 i18n_save()。
 */
esp_err_t i18n_set(ui_lang_t lang);

/** @brief 只切换内存中的语言（不写 NVS），供 LVGL 任务里立即重建界面用 */
void i18n_set_mem(ui_lang_t lang);

/** @brief 只把语言写进 NVS（不切内存），供独立任务调用 */
esp_err_t i18n_save(ui_lang_t lang);

/** @brief 取文案。id 越界返回 "-" */
const char *i18n_t(i18n_str_t id);

/** @brief 取指定档位的界面字体（小/中/大三档，越界回落到小号） */
const lv_font_t *i18n_font(ui_txt_size_t size);

/**
 * @brief 设备数量文案（英文区分单复数，中文不加量词后缀）
 * @param buf  输出缓冲
 * @param sz   缓冲大小
 * @param n    设备数
 */
void i18n_dev_count(char *buf, size_t sz, int n);

#ifdef __cplusplus
}
#endif
