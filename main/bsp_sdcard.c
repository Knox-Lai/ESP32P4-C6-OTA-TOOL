/**
 * @file  bsp_sdcard.c
 * @brief TF 卡后台挂载与容量缓存
 */
#include "bsp_sdcard.h"

#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_RETRY_MS      3000      /* 没卡时的重试间隔 */
#define SD_INFO_MS       6000      /* 容量缓存有效期 */
#define SD_LDO_CHAN      4         /* 原理图：TF 卡 IO 供电走片内 LDO_VO4 */

static sdmmc_card_t      *s_card    = NULL;
static sd_pwr_ctrl_handle_t s_pwr   = NULL;
static volatile bool      s_mounted = false;

static SemaphoreHandle_t  s_mtx     = NULL;   /* 挂载/卸载互斥（任务是主用，命令是旁路） */

/* 容量缓存：只由 sd 任务写，其他任务只读 */
static uint64_t s_total_kb  = 0;
static uint64_t s_free_kb   = 0;
static int64_t  s_info_us   = 0;

/* 这些 tag 在"没插卡"时会以 E 级反复打印（每次重试一条），
 * 3 s 一次会刷满串口，把它们压掉；真正的状态变化由本文件自己打。 */
static void quiet_sd_logs(void)
{
    static const char *const tags[] = {
        "sdmmc_common", "sdmmc_init", "sdmmc_io", "sdmmc_cmd",
        "sdmmc_req", "vfs_fat_sdmmc", "sdmmc_periph",
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_NONE);
    }
}

static esp_err_t do_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;      /* Slot 0 走 IO MUX，不必显式指定引脚 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    if (s_pwr == NULL) {
        const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
            s_pwr = NULL;                       /* 建不出来就退回默认供电，仍可试挂载 */
        }
    }
    host.pwr_ctrl_handle = s_pwr;

    const sdmmc_slot_config_t slot = {
        .cd    = SDMMC_SLOT_NO_CD,              /* 板上没有卡检测脚 */
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    const esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,        /* 绝不动用户的卡 */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err == ESP_OK) {
        s_mounted = true;
        s_info_us = 0;                          /* 强制下一轮刷新容量 */
    }
    return err;
}

esp_err_t bsp_sdcard_mount_now(void)
{
    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    esp_err_t err = do_mount();
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }
    return err;
}

void bsp_sdcard_unmount(void)
{
    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        s_card    = NULL;
        s_mounted = false;
        s_total_kb = 0;
        s_free_kb  = 0;
    }
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }
}

bool bsp_sdcard_mounted(void)
{
    return s_mounted;
}

bool bsp_sdcard_usage(uint64_t *total_kb, uint64_t *free_kb)
{
    if (!s_mounted || s_total_kb == 0) {
        return false;
    }
    if (total_kb != NULL) {
        *total_kb = s_total_kb;
    }
    if (free_kb != NULL) {
        *free_kb = s_free_kb;
    }
    return true;
}

/* 刷新容量缓存；顺便当"卡被拔掉"的探测：stat 失败就卸载 */
static void refresh_info(int64_t now_us)
{
    if (now_us - s_info_us < (int64_t)SD_INFO_MS * 1000) {
        return;
    }
    s_info_us = now_us;

    struct stat st;
    if (stat(BSP_SD_MOUNT_POINT, &st) != 0) {
        ESP_LOGW(TAG, "TF 卡已拔出");
        bsp_sdcard_unmount();
        return;
    }

    uint64_t total = 0;
    uint64_t freeb = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb) == ESP_OK && total > 0) {
        s_total_kb = total / 1024;
        s_free_kb  = freeb / 1024;
    } else {
        /* 典型原因：卡是 exFAT/NTFS，FATFS 挂上了但读不出容量 */
        s_total_kb = 0;
        s_free_kb  = 0;
    }
}

static void sd_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!s_mounted) {
            if (bsp_sdcard_mount_now() == ESP_OK) {
                ESP_LOGI(TAG, "TF 卡已挂载 %s", BSP_SD_MOUNT_POINT);
            }
            /* 失败不打印：没插卡是常态 */
        } else {
            refresh_info(esp_timer_get_time());
        }
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
    }
}

esp_err_t bsp_sdcard_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    quiet_sd_logs();

    /* 栈 5 KB：SD 初始化链路（sdmmc_init_*）与 FATFS 挂载都在这个任务里跑 */
    if (xTaskCreate(sd_task, "sd_card", 5120, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "TF 卡任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
