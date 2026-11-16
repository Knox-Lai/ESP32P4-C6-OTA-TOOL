/**
 * @file  bsp_sdcard.c
 * @brief TF 卡按需挂载与容量缓存
 *
 * 板上没有卡检测（CD）脚：卡座不产生任何插拔中断，"有没有卡"只能靠试。
 * 早先的做法是每 3 s 试挂一次——插卡即识别，代价是没卡时每 3 s 跑一遍完整
 * 的 SD 初始化（SDMMC 上电时序 + 几条命令超时，几百毫秒），还得以 ESP_LOG_NONE
 * 把 sdmmc 全家桶的日志静音才不刷屏。
 *
 * 现在改成**只在上层需要时探测一次**：
 *   - bsp_sdcard_probe()      异步投给 sd 任务，不阻塞调用者（UI 侧用）
 *   - bsp_sdcard_mount_now()  在调用者任务里同步探测（shell / 后台用）
 * 开机自动探测一次，之后没有任何周期性动作；插卡后由用户动作（`sd` 命令、
 * 固件库刷新等）触发重新探测。
 */
#include "bsp_sdcard.h"

#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_LDO_CHAN      4         /* 原理图：TF 卡 IO 供电走片内 LDO_VO4 */
/* 开机后多久再做第一次探测。C6 走 SDIO、TF 卡走 SDMMC slot0，同一个主机控制器，
 * 开机阶段抢它会让 hosted 的 RPC 超时（见 sd_task 里的注释）。 */
#define SD_BOOT_DELAY_MS 10000

static sdmmc_card_t      *s_card    = NULL;
static sd_pwr_ctrl_handle_t s_pwr   = NULL;
static volatile bool      s_mounted = false;

static SemaphoreHandle_t  s_mtx     = NULL;   /* 挂载/卸载互斥（sd 任务是主用，命令是旁路） */
static TaskHandle_t       s_task    = NULL;   /* 探测任务：只在收到通知时动 SD 总线 */

/* 容量缓存：只由探测方写，其他任务只读 */
static uint64_t s_total_kb  = 0;
static uint64_t s_free_kb   = 0;

/* 这些 tag 在"没插卡"时会以 E 级反复打印（每次挂载失败一条），
 * 真正的状态变化由本文件自己打。 */
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

/* 刷新容量缓存。FATFS 的 f_getfree 要遍历 FAT，32 GB 卡上一次十几毫秒，
 * 所以只在探测时做一次，不在 LVGL 任务里碰。 */
static void refresh_info(void)
{
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
        refresh_info();                         /* 挂上就取一次容量，UI 立刻有值 */
    }
    return err;
}

static void do_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    s_card     = NULL;
    s_mounted  = false;
    s_total_kb = 0;
    s_free_kb  = 0;
}

/* 一次完整探测。调用者必须已持有 s_mtx。
 * 没挂载 → 试挂；已挂载 → stat 一遍当"卡还在不在"的校验，顺便刷容量。 */
static esp_err_t probe_locked(void)
{
    if (!s_mounted) {
        esp_err_t err = do_mount();
        if (err != ESP_OK) {
            /* 不打印：没插卡是常态，上层只看返回值 */
            return err;
        }
        ESP_LOGI(TAG, "TF 卡已挂载 %s", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    struct stat st;
    if (stat(BSP_SD_MOUNT_POINT, &st) != 0) {
        ESP_LOGW(TAG, "TF 卡已拔出");
        do_unmount();
        return ESP_ERR_NOT_FOUND;
    }

    refresh_info();
    return ESP_OK;
}

esp_err_t bsp_sdcard_mount_now(void)
{
    if (s_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = probe_locked();
    xSemaphoreGive(s_mtx);
    return err;
}

void bsp_sdcard_probe(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void bsp_sdcard_unmount(void)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    do_unmount();
    xSemaphoreGive(s_mtx);
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

/* 没有周期：等通知，来一条探一次。
 * 探测（完整 SD 初始化）放在这个任务里，别让 UI 或 shell 任务直接承担。 */
static void sd_task(void *arg)
{
    (void)arg;

    /* 开机先让路：TF 卡走 SDMMC slot0，C6 的 SDIO 走 slot1，两者归同一个
     * 主机控制器管。hosted 在开机那几秒正做 CP 复位 + bringup，这时候去挂
     * TF 卡会和它抢控制器与 GPIO，现场表现就是 esp_wifi_init / esp_wifi_start
     * 的 RPC 5 s 无响应（`no response uid=4 msg_id=280`），WiFi 直接判不可用。
     * 等开机那阵子过去再探，代价只是主页 TF 那一行晚 10 s 出结果。 */
    vTaskDelay(pdMS_TO_TICKS(SD_BOOT_DELAY_MS));

    (void)bsp_sdcard_mount_now();       /* 开机唯一一次自动探测 */

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void)bsp_sdcard_mount_now();
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
    if (xTaskCreate(sd_task, "sd_card", 5120, NULL, tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "TF 卡任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    /* 开机那次探测由 sd_task 自己延迟发起（避让 C6 的 SDIO bringup），
     * 这里不再立刻投一次请求。 */
    return ESP_OK;
}
