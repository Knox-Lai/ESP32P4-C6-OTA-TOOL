/**
 * @file  fw_store.c
 * @brief 固件仓库实现：双介质扫描 + 缓存 + 原子导入
 */
#include "fw_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "bsp_sdcard.h"
#include "fw_pkg.h"

static const char *TAG = "fw_store";

#define SCAN_PERIOD_MS     2000     /* 后台任务唤醒粒度，只有 dirty 才真扫 */
#define IMPORT_CHUNK       4096u
#define FREE_SLACK_B       (32u * 1024u)   /* FAT 簇对齐 + 目录项余量 */
/* 挂载 8 MB 分区要先走 FAT 格式化（wl_sector 缓冲 + VFS 路径），1 KB 级的
 * 栈不够用；扫描本身已不再占栈（见 s_scan_buf），这里给的是足够余量。 */
#define STORE_TASK_STACK   6144

static wl_handle_t       s_wl        = WL_INVALID_HANDLE;
static bool              s_int_mounted = false;
static SemaphoreHandle_t s_mtx       = NULL;

static fw_item_t s_cache[FW_STORE_MAX];
static int       s_count = 0;

/* 扫描用的暂存区必须放静态区：fw_item_t 是 144 B，FW_STORE_MAX=24 就是 3456 B，
 * 放在 fw_store_scan() 的栈上会直接顶穿 6 KB 扫描任务栈（曾表现为开机 3 秒
 * 一次 "Stack protection fault" 死循环重启）。串口命令也会调 fw_store_scan()，
 * 所以用一把独立的锁把并发扫描串起来，避免两边写同一块暂存区。 */
static fw_item_t         s_scan_buf[FW_STORE_MAX];
static SemaphoreHandle_t s_scan_mtx  = NULL;

static volatile bool s_dirty  = true;
static bool          s_sd_was = false;

/* ---------------- 内部挂载 ---------------- */

static esp_err_t int_mount(void)
{
    if (s_int_mounted) {
        return ESP_OK;
    }

    const esp_vfs_fat_mount_config_t cfg = {
        /* 这块分区从没被格式化过，首次必须允许格式化；之后正常挂载不会擦数据 */
        .format_if_mount_failed = true,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FW_STORE_INT_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 %s 失败：%s（内部仓库不可用，TF 卡仍可用）",
                 FW_STORE_INT_MOUNT, esp_err_to_name(err));
        return err;
    }

    s_int_mounted = true;
    if (mkdir(FW_STORE_INT_DIR, 0777) != 0) {
        /* 已存在不算错 */
        ESP_LOGD(TAG, "%s 已存在", FW_STORE_INT_DIR);
    }
    ESP_LOGI(TAG, "内部仓库已挂载：%s", FW_STORE_INT_DIR);
    return ESP_OK;
}

bool fw_store_int_mounted(void)
{
    return s_int_mounted;
}

/* ---------------- 扫描 ---------------- */

static bool has_fwp_suffix(const char *name)
{
    size_t l = strlen(name);
    if (l < 5) {                     /* "a.fwp" 至少 5 字符 */
        return false;
    }
    return strcasecmp(name + l - 4, ".fwp") == 0;
}

static int scan_dir(const char *dir, fw_src_t src, fw_item_t *out, int cap, int n)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return n;                    /* 目录不存在 = 没放过固件，不是错误 */
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (n >= cap) {
            ESP_LOGW(TAG, "条目超过上限 %d，%s 里其余文件不再列出", cap, dir);
            break;
        }
        if (!has_fwp_suffix(de->d_name)) {
            continue;
        }

        char path[FW_STORE_PATH_LEN];
        int m = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (m < 0 || m >= (int)sizeof(path)) {
            ESP_LOGW(TAG, "路径过长，跳过 %s/%s", dir, de->d_name);
            continue;
        }

        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            ESP_LOGW(TAG, "打不开 %s", path);
            continue;
        }
        fw_pkg_hdr_t h;
        esp_err_t err = fw_pkg_parse(f, &h);
        fclose(f);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "跳过 %s：不是合法 .fwp（%s）", path, esp_err_to_name(err));
            continue;
        }

        fw_item_t *it = &out[n];
        memset(it, 0, sizeof(*it));
        it->src = src;
        memcpy(it->path, path, sizeof(it->path));
        memcpy(it->raw_model, h.model, sizeof(it->raw_model));
        it->model_id = dev_model_match(h.model);
        it->hw_min   = h.hw_min;
        it->hw_max   = h.hw_max;
        it->fw_ver   = h.fw_ver;
        it->size     = h.payload_len;
        memcpy(it->sha256, h.sha256, sizeof(it->sha256));
        n++;
    }
    closedir(d);
    return n;
}

/* 排序键：型号升序 → 版本降序 → 内部仓库优先 */
static int item_cmp(const fw_item_t *a, const fw_item_t *b)
{
    if (a->model_id != b->model_id) {
        return (a->model_id < b->model_id) ? -1 : 1;
    }
    if (a->fw_ver != b->fw_ver) {
        return (a->fw_ver > b->fw_ver) ? -1 : 1;      /* 版本高的在前 */
    }
    if (a->src != b->src) {
        return (a->src == FW_SRC_FLASH) ? -1 : 1;
    }
    return 0;
}

static void sort_items(fw_item_t *v, int n)
{
    for (int i = 1; i < n; i++) {
        fw_item_t key = v[i];
        int j = i - 1;
        while (j >= 0 && item_cmp(&v[j], &key) > 0) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

static bool same_fw(const fw_item_t *a, const fw_item_t *b)
{
    if (a->fw_ver != b->fw_ver) {
        return false;
    }
    if (a->model_id != FW_MODEL_UNKNOWN || b->model_id != FW_MODEL_UNKNOWN) {
        return a->model_id == b->model_id;
    }
    return strcmp(a->raw_model, b->raw_model) == 0;
}

/* 同型号同版本在两张卡上都存在时只留一条（内部仓库优先，拔卡后还在） */
static void dedupe_items(fw_item_t *v, int *pn)
{
    int n = *pn;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; ) {
            if (!same_fw(&v[i], &v[j])) {
                j++;
                continue;
            }
            if (v[i].src == FW_SRC_SD && v[j].src == FW_SRC_FLASH) {
                v[i] = v[j];
            }
            memmove(&v[j], &v[j + 1], (size_t)(n - j - 1) * sizeof(v[0]));
            n--;
        }
    }
    *pn = n;
}

int fw_store_scan(void)
{
    if (s_scan_mtx != NULL) {
        xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    }

    fw_item_t *tmp = s_scan_buf;     /* 静态暂存区，理由见文件头注释 */
    int n = 0;

    n = scan_dir(FW_STORE_SD_DIR, FW_SRC_SD, tmp, FW_STORE_MAX, n);
    /* 卡根目录也扫一遍：多数人直接把 .fwp 丢在根上，不建 fw 目录时不该"看不见" */
    n = scan_dir(BSP_SD_MOUNT_POINT, FW_SRC_SD, tmp, FW_STORE_MAX, n);
    n = scan_dir(FW_STORE_INT_DIR, FW_SRC_FLASH, tmp, FW_STORE_MAX, n);

    sort_items(tmp, n);
    dedupe_items(tmp, &n);

    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    memcpy(s_cache, tmp, (size_t)n * sizeof(tmp[0]));
    s_count = n;
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }

    s_dirty = false;

    if (s_scan_mtx != NULL) {
        xSemaphoreGive(s_scan_mtx);
    }
    return n;
}

void fw_store_request_scan(void)
{
    s_dirty = true;
}

int fw_store_snapshot(fw_item_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    int n = (s_count < max) ? s_count : max;
    memcpy(out, s_cache, (size_t)n * sizeof(out[0]));
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }
    return n;
}

/* ---------------- 打开成 ota_image ---------------- */

esp_err_t fw_store_open(const fw_item_t *it, ota_image_t *img)
{
    if (it == NULL || img == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(it->path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开 %s（卡被拔了？）", it->path);
        return ESP_ERR_NOT_FOUND;
    }
    fw_pkg_hdr_t h;
    esp_err_t err = fw_pkg_parse(f, &h);
    fclose(f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 包头校验失败：%s", it->path, esp_err_to_name(err));
        return err;
    }

    /* 目录扫描后文件可能被人换掉，这里以"再读一遍头"的结果为准 */
    err = ota_image_from_file(img, &h, it->path);
    if (err == ESP_OK) {
        fw_pkg_log(&h, it->path);
    }
    return err;
}

/* ---------------- 空间 ---------------- */

bool fw_store_space(fw_src_t src, uint64_t *free_b)
{
    if (free_b == NULL) {
        return false;
    }

    if (src == FW_SRC_FLASH) {
        if (!s_int_mounted) {
            return false;
        }
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(FW_STORE_INT_MOUNT, &total, &free_bytes) != ESP_OK) {
            return false;
        }
        *free_b = free_bytes;
        return true;
    }

    uint64_t total_kb = 0, free_kb = 0;
    if (!bsp_sdcard_usage(&total_kb, &free_kb)) {
        return false;
    }
    *free_b = free_kb * 1024u;
    return true;
}

/* ---------------- 导入 / 删除 ---------------- */

static const char *path_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return (s != NULL) ? s + 1 : path;
}

esp_err_t fw_store_import(int idx, char *msg, size_t cap)
{
    fw_item_t it;

    if (msg == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    if (idx < 0 || idx >= s_count) {
        if (s_mtx != NULL) {
            xSemaphoreGive(s_mtx);
        }
        snprintf(msg, cap, "没有第 %d 项", idx);
        return ESP_ERR_INVALID_ARG;
    }
    it = s_cache[idx];
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }

    if (it.src != FW_SRC_SD) {
        snprintf(msg, cap, "第 %d 项已经在内部仓库里了", idx);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = int_mount();
    if (err != ESP_OK) {
        snprintf(msg, cap, "内部仓库未挂载：%s", esp_err_to_name(err));
        return err;
    }

    uint64_t free_b = 0;
    if (!fw_store_space(FW_SRC_FLASH, &free_b)) {
        snprintf(msg, cap, "读不到内部剩余空间");
        return ESP_FAIL;
    }
    if (free_b < (uint64_t)it.size + FREE_SLACK_B) {
        snprintf(msg, cap, "空间不足：需要 %u KB，剩余 %u KB",
                 (unsigned)((it.size + 1024u) / 1024u), (unsigned)(free_b / 1024u));
        return ESP_ERR_NO_MEM;
    }

    /* 先整包校验源文件：CF 卡抄错/拷一半的文件绝不能进仓库 */
    FILE *src = fopen(it.path, "rb");
    if (src == NULL) {
        snprintf(msg, cap, "打不开源文件");
        return ESP_ERR_NOT_FOUND;
    }
    fw_pkg_hdr_t h;
    err = fw_pkg_parse(src, &h);
    if (err == ESP_OK) {
        err = fw_pkg_verify_payload(src, &h);
    }
    if (err != ESP_OK) {
        fclose(src);
        snprintf(msg, cap, "源文件校验失败：%s", esp_err_to_name(err));
        return err;
    }
    rewind(src);

    /* 目标路径 = FW_STORE_INT_DIR(11) + '/' + basename(最长 FW_STORE_PATH_LEN-1) + ".part"(5)，
     * 直接用 FW_STORE_PATH_LEN 装会被 GCC 判为可能截断（-Werror=format-truncation 下编不过）。 */
    char dst[FW_STORE_PATH_LEN + 24];
    char tmp[FW_STORE_PATH_LEN + 32];
    int plen = snprintf(dst, sizeof(dst), "%s/%s", FW_STORE_INT_DIR, path_basename(it.path));
    if (plen < 0 || (size_t)plen >= sizeof(dst)) {
        fclose(src);
        snprintf(msg, cap, "目标路径过长");
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(tmp, sizeof(tmp), "%s.part", dst);

    uint8_t *buf = malloc(IMPORT_CHUNK);
    if (buf == NULL) {
        fclose(src);
        snprintf(msg, cap, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    FILE *out = fopen(tmp, "wb");
    if (out == NULL) {
        free(buf);
        fclose(src);
        snprintf(msg, cap, "建不了 %s", tmp);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "导入 %s → %s（%u KB）", it.path, dst, (unsigned)(it.size / 1024u));

    uint32_t copied = 0;
    uint32_t total  = h.file_size;         /* 头 + 负载，原样复制 */
    err = ESP_OK;
    while (copied < total) {
        uint32_t want = (total - copied > IMPORT_CHUNK) ? IMPORT_CHUNK : (total - copied);
        size_t got = fread(buf, 1, want, src);
        if (got != want) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buf, 1, got, out) != got) {
            err = ESP_FAIL;
            break;
        }
        copied += (uint32_t)got;
        if ((copied % (512u * 1024u)) < IMPORT_CHUNK) {
            ESP_LOGI(TAG, "  已写 %u/%u KB", (unsigned)(copied / 1024u), (unsigned)(total / 1024u));
        }
    }

    free(buf);
    fclose(src);
    if (fclose(out) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        unlink(tmp);
        snprintf(msg, cap, "写入失败：%s", esp_err_to_name(err));
        return err;
    }

    unlink(dst);                            /* 覆盖同版本旧文件时 rename 在 FAT 上不保证覆盖 */
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        snprintf(msg, cap, "入库失败（rename）");
        return ESP_FAIL;
    }

    fw_store_request_scan();
    snprintf(msg, cap, "已入库 %s", path_basename(dst));
    return ESP_OK;
}

esp_err_t fw_store_remove(int idx)
{
    fw_item_t it;

    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    if (idx < 0 || idx >= s_count) {
        if (s_mtx != NULL) {
            xSemaphoreGive(s_mtx);
        }
        return ESP_ERR_INVALID_ARG;
    }
    it = s_cache[idx];
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }

    if (it.src != FW_SRC_FLASH) {
        /* 用户的卡不归我们管，误删不可恢复 */
        return ESP_ERR_INVALID_STATE;
    }

    if (unlink(it.path) != 0) {
        return ESP_FAIL;
    }
    fw_store_request_scan();
    return ESP_OK;
}

/* ---------------- 型号最优匹配 ---------------- */

const fw_item_t *fw_store_best(fw_model_t m)
{
    const fw_item_t *best = NULL;

    if (m == FW_MODEL_UNKNOWN) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_cache[i].model_id != m) {
            continue;
        }
        if (best == NULL || s_cache[i].fw_ver > best->fw_ver) {
            best = &s_cache[i];
        }
    }
    return best;
}

const char *fw_src_str(fw_src_t s)
{
    switch (s) {
    case FW_SRC_SD:    return "SD";
    case FW_SRC_FLASH: return "FLASH";
    default:           return "?";
    }
}

/* ---------------- 后台任务 ---------------- */

static void fw_store_task(void *arg)
{
    (void)arg;
    bool int_tried = false;      /* 首次挂载只试一次，失败不要每 2 s 刷一条错误日志 */
    bool wm_logged = false;      /* 首次扫描后报一次剩余栈，方便现场判断栈够不够 */

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));

        /* 内部分区首次挂载可能要格式化 8 MB（秒级），必须在任务里做：
         * 放在 app_main 会直接拖慢开机到第一帧的时间。 */
        if (!int_tried) {
            int_tried = true;
            int_mount();
            /* 这里**不**主动去挂 TF 卡：bsp_sdcard 的开机探测已经推迟到 10 s 后
             * （要让开 C6 的 SDIO bringup，见 sd_task 注释）。首轮扫不到卡里的
             * 固件没关系 —— 卡挂上后下面那段 s_sd_was 对比会补一次重扫。 */
            s_dirty = true;
        }

        /* 探测只会由上层动作触发，挂载状态因此是"事件"了：
         * 每次真正变化（插卡后敲 sd / 刷新）都在这里补一次重扫。 */
        bool sd_now = bsp_sdcard_mounted();
        if (sd_now != s_sd_was) {
            s_sd_was = sd_now;
            s_dirty = true;
            ESP_LOGI(TAG, "TF 卡%s，重新扫描固件库", sd_now ? "已挂载" : "已拔出");
        }

        if (!s_dirty) {
            continue;
        }
        int n = fw_store_scan();
        if (!wm_logged) {
            wm_logged = true;
            ESP_LOGI(TAG, "固件库刷新：%d 项（扫描后剩余栈 %u B）", n,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGI(TAG, "固件库刷新：%d 项", n);
        }
    }
}

esp_err_t fw_store_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_scan_mtx == NULL) {
        s_scan_mtx = xSemaphoreCreateMutex();
        if (s_scan_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(fw_store_task, "fw_store", STORE_TASK_STACK, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "固件仓库任务已启动（挂载与扫描在后台进行）");
    return ESP_OK;
}
