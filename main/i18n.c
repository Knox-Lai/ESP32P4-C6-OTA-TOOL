/**
 * @file  i18n.c
 * @brief 中英文案表 + 语言持久化 + 字体选择
 *
 * 表是 [I18N_STR_MAX][2]，第一列英文、第二列中文，顺序必须与 i18n.h 里
 * 的枚举逐项对应。加一条文案的做法：i18n.h 枚举里插一行 → 这里插一行，
 * 位置保持一致（编译期有 "excess elements in array initializer" 兜底）。
 *
 * 中文字库是子集：只包含这些字符串里出现过的汉字（见 tools/fontgen）。
 * 所以**改文案后必须重跑 gen_fonts.py 重新生成字库**，否则新加的字会显示成方块。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"        /* nvs_open / nvs_get_u8 / nvs_set_u8 / nvs_commit */

#include "i18n.h"

static const char *TAG = "i18n";

#define NVS_NS   "ui"
#define NVS_KEY  "lang"

static ui_lang_t s_lang = LANG_EN;

/* clang-format off */
static const char *const k_str[I18N_STR_MAX][2] = {
/*                        English                                    中文 */
/* S_BACK            */ { "< Back",                                  "< 返回" },
/* S_DEV_COUNT       */ { "%d device%s",                              "%d 台设备" },
/* S_NO_LINK_HINT    */ { "No link.  Plug the Ethernet cable into the tool.",
                                                                        "网络未连接，请把网线插入本机。" },
/* S_NO_DEVICE_HINT  */ { "No device yet.  Connect a device or run ota_sim.py on the PC.",
                                                                        "暂无设备。请接入设备，或在电脑上运行 ota_sim.py。" },
/* S_UNKNOWN_HOST    */ { "Unknown host",                            "未知主机" },
/* S_LEASES_ONLY     */ { "IP assigned, no discovery reply",          "仅分配到 IP，未应答发现" },
/* S_NO_DEVICE       */ { "No device",                                "暂无设备" },
/* S_UNKNOWN         */ { "Unknown",                                  "未知" },
/* S_DEVICE_GONE     */ { "Device gone",                              "设备已离线" },
/* S_LINK_UP         */ { "UP",                                       "已连接" },
/* S_LINK_DOWN       */ { "DOWN",                                     "未连接" },

/* A_DEVICES         */ { "Devices",                                  "设备" },
/* A_UPGRADE         */ { "Upgrade",                                  "升级" },
/* A_INFO            */ { "Info",                                     "信息" },
/* A_SETTINGS        */ { "Settings",                                 "设置" },

/* H_NETWORK         */ { "NETWORK",                                  "网络" },
/* H_LINK            */ { "LINK",                                     "链路" },
/* H_DHCP_LEASE      */ { "ASSIGNED IP",                              "已分配 IP" },
/* H_FREE_HEAP       */ { "FREE HEAP",                                "空闲内存" },
/* H_LAST_SESSION    */ { "LAST SESSION",                             "上次升级" },
/* H_NO_SESSION      */ { "no session yet",                           "尚未开始" },
/* H_TAGLINE         */ { "Ethernet OTA Upgrader",                    "以太网 OTA 升级工具" },

/* D_TITLE           */ { "Devices",                                  "设备" },
/* D_FW              */ { "fw %u.%u.%u.%u",                           "固件 %u.%u.%u.%u" },
/* D_FW_NONE         */ { "fw -",                                     "固件 -" },

/* DT_TITLE          */ { "Device",                                   "设备详情" },
/* DT_MODEL          */ { "MODEL",                                    "型号" },
/* DT_IP             */ { "IP",                                       "IP 地址" },
/* DT_FIRMWARE       */ { "FIRMWARE",                                 "固件版本" },
/* DT_HARDWARE       */ { "HARDWARE",                                 "硬件版本" },
/* DT_FLASH          */ { "FLASH",                                    "闪存容量" },
/* DT_CAPS           */ { "CAPS",                                     "功能位" },
/* DT_UID            */ { "UID",                                      "唯一 ID" },
/* DT_HOST           */ { "HOST",                                     "主机名" },
/* DT_MAC            */ { "MAC",                                      "MAC 地址" },
/* DT_UPGRADE_THIS   */ { "Upgrade this device",                      "升级此设备" },

/* U_TITLE           */ { "Upgrade",                                  "升级" },
/* U_SELECT_DEVICE   */ { "SELECT DEVICE",                            "选择设备" },
/* U_START           */ { "Start upgrade",                            "开始升级" },
/* U_READY           */ { "READY",                                    "就绪" },
/* U_SELECT_HINT     */ { "select a device, then start",              "请先选择设备，再开始升级" },
/* U_SUCCESS_HINT    */ { "device is running the new firmware",       "设备已运行新固件" },
/* U_ERR_FMT         */ { "error 0x%02x  (see serial log)",           "错误码 0x%02x（详见串口日志）" },

/* I_TITLE           */ { "Info",                                     "信息" },
/* I_NETWORK         */ { "NETWORK",                                  "网络" },
/* I_ADDRESS         */ { "ADDRESS",                                  "本机地址" },
/* I_NETMASK         */ { "NETMASK",                                  "子网掩码" },
/* I_GATEWAY         */ { "GATEWAY",                                  "网关" },
/* I_LINK            */ { "LINK",                                     "链路" },
/* I_LEASE           */ { "ASSIGNED IP",                              "已分配 IP" },
/* I_SYSTEM          */ { "SYSTEM",                                   "系统" },
/* I_RAM             */ { "INTERNAL RAM",                             "内部 RAM 占用" },
/* I_RAM_DETAIL      */ { "free %u KB  /  %u KB",                     "空闲 %u KB  /  共 %u KB" },
/* I_PSRAM           */ { "PSRAM",                                    "PSRAM" },
/* I_FLASH           */ { "FLASH",                                    "闪存容量" },
/* I_MIN_HEAP        */ { "LOWEST FREE RAM",                          "历史最低空闲" },
/* I_UPTIME          */ { "UPTIME",                                   "运行时间" },
/* I_DEVICES         */ { "DEVICES",                                  "设备数" },
/* I_OTA_STATE       */ { "OTA STATE",                                "升级状态" },

/* ST_IDLE           */ { "IDLE",                                     "空闲" },
/* ST_CONNECT        */ { "CONNECT",                                  "连接中" },
/* ST_HANDSHAKE      */ { "HANDSHAKE",                                "握手中" },
/* ST_STREAMING      */ { "STREAMING",                                "传输中" },
/* ST_VERIFYING      */ { "VERIFYING",                                "校验中" },
/* ST_REBOOTING      */ { "REBOOTING",                                "重启中" },
/* ST_WAIT_ONLINE    */ { "WAIT ONLINE",                              "等待上线" },
/* ST_SUCCESS        */ { "SUCCESS",                                  "成功" },
/* ST_FAILED         */ { "FAILED",                                   "失败" },

/* SET_TITLE         */ { "Settings",                                 "设置" },
/* SET_DISPLAY       */ { "DISPLAY",                                  "显示" },
/* SET_BRIGHTNESS    */ { "BRIGHTNESS",                               "亮度" },
/* SET_GENERAL       */ { "GENERAL",                                  "通用" },
/* SET_LANGUAGE      */ { "LANGUAGE",                                 "语言" },
/* SET_LANG_ZH       */ { "Simplified Chinese",                       "简体中文" },
/* SET_LANG_EN       */ { "English",                                  "English" },
/* SET_ABOUT         */ { "ABOUT",                                    "关于" },
/* SET_ABOUT_HINT    */ { "Serial console: type help for commands.   Swipe from the left third of the screen to go back.",
                                                                        "串口控制台输入 help 查看命令。在屏幕左侧三分之一区域内横滑可返回上一页。" },

/* H_SYS_STATUS      */ { "SYSTEM",                                   "系统状态" },
/* H_CPU             */ { "CPU",                                      "CPU" },
/* H_MEM             */ { "MEMORY",                                   "内存" },
/* H_TF              */ { "TF CARD",                                  "TF 卡" },
/* H_TF_NONE         */ { "not inserted",                             "未插入" },
};
/* clang-format on */

void i18n_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "无语言记录，默认英文");
        return;
    }

    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK && v <= (uint8_t)LANG_ZH) {
        s_lang = (ui_lang_t)v;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "语言：%s", s_lang == LANG_ZH ? "中文" : "English");
}

ui_lang_t i18n_lang(void)
{
    return s_lang;
}

void i18n_set_mem(ui_lang_t lang)
{
    if (lang == LANG_EN || lang == LANG_ZH) {
        s_lang = lang;
    }
}

esp_err_t i18n_save(ui_lang_t lang)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败，语言仅本次生效：%s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(h, NVS_KEY, (uint8_t)lang);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "语言已保存为 %s%s", lang == LANG_ZH ? "中文" : "English",
             err == ESP_OK ? "" : "（写入失败）");
    return err;
}

esp_err_t i18n_set(ui_lang_t lang)
{
    if (lang != LANG_EN && lang != LANG_ZH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lang == s_lang) {
        return ESP_OK;
    }
    s_lang = lang;
    return i18n_save(lang);
}

const char *i18n_t(i18n_str_t id)
{
    if ((int)id < 0 || id >= I18N_STR_MAX) {
        return "-";
    }
    return k_str[id][s_lang == LANG_ZH ? 1 : 0];
}

const lv_font_t *i18n_font(ui_txt_size_t size)
{
    if (s_lang == LANG_ZH) {
        switch (size) {
        case UI_TXT_LARGE: return &ui_font_zh_30;
        case UI_TXT_BASE:  return &ui_font_zh_22;
        default:           return &ui_font_zh_16;
        }
    }

    switch (size) {
    case UI_TXT_LARGE: return &lv_font_montserrat_28;
    case UI_TXT_BASE:  return &lv_font_montserrat_20;
    default:           return &lv_font_montserrat_14;
    }
}

void i18n_dev_count(char *buf, size_t sz, int n)
{
    if (s_lang == LANG_ZH) {
        snprintf(buf, sz, i18n_t(S_DEV_COUNT), n);
    } else {
        snprintf(buf, sz, i18n_t(S_DEV_COUNT), n, n == 1 ? "" : "s");
    }
}
