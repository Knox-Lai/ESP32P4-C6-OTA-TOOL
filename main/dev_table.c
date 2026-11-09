/**
 * @file  dev_table.c
 * @brief 设备表实现
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "app_config.h"
#include "dev_table.h"

static const char *TAG = "dev_table";

static dev_entry_t s_table[APP_DEV_MAX];
static SemaphoreHandle_t s_lock = NULL;

/* ------------------------------------------------------------------ */
/* 内部：调用前必须已持锁                                               */
/* ------------------------------------------------------------------ */

static inline void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static dev_entry_t *find_ip_locked(uint32_t ip_net)
{
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (s_table[i].used && s_table[i].ip == ip_net) {
            return &s_table[i];
        }
    }
    return NULL;
}

static dev_entry_t *find_mac_locked(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};
    if (mac == NULL || memcmp(mac, zero, 6) == 0) {
        return NULL;   /* 全 0 MAC 不参与匹配 */
    }
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (s_table[i].used && memcmp(s_table[i].mac, mac, 6) == 0) {
            return &s_table[i];
        }
    }
    return NULL;
}

static dev_entry_t *find_uid_locked(const uint8_t uid[DEV_UID_LEN])
{
    static const uint8_t zero[DEV_UID_LEN] = {0};
    if (uid == NULL || memcmp(uid, zero, DEV_UID_LEN) == 0) {
        return NULL;
    }
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (s_table[i].used && s_table[i].identified &&
            memcmp(s_table[i].id.uid, uid, DEV_UID_LEN) == 0) {
            return &s_table[i];
        }
    }
    return NULL;
}

/** 占一个空槽；满了就回收「离线且最久没消息」的那一个 */
static dev_entry_t *alloc_locked(void)
{
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (!s_table[i].used) {
            return &s_table[i];
        }
    }

    dev_entry_t *victim = NULL;
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (!s_table[i].online) {
            if (victim == NULL || s_table[i].last_seen_ms < victim->last_seen_ms) {
                victim = &s_table[i];
            }
        }
    }
    if (victim) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 victim->mac[0], victim->mac[1], victim->mac[2],
                 victim->mac[3], victim->mac[4], victim->mac[5]);
        ESP_LOGW(TAG, "设备表已满，回收离线槽（%s）", mac);
        memset(victim, 0, sizeof(*victim));
    }
    return victim;
}

static void refresh_online_locked(uint32_t now_ms)
{
    /* 两条判定线，语义不同，日志级别也不同：
     *   已识别设备：连续 APP_DEV_OFFLINE_MISS 次心跳无应答 -> 真掉线，W 级；
     *   未识别设备：它压根不响应该协议（典型是 PC 或非 OTA 设备），
     *               只把 online 置 false 并降频，不能报“离线”，否则任何一台
     *               插上网线的电脑都会刷一条警告，真正的问题反而被淹掉。 */
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (!s_table[i].used) {
            continue;
        }
        bool was = s_table[i].online;
        s_table[i].online = (s_table[i].miss < APP_DEV_OFFLINE_MISS);
        if (was == s_table[i].online) {
            continue;
        }

        if (!s_table[i].online) {
            if (s_table[i].identified) {
                /* 已识别设备掉线：降级为低频探测，留一条“复活”通道 */
                s_table[i].next_poll_ms = now_ms + APP_OFFLINE_PROBE_MS;
                ESP_LOGW(TAG, "设备离线：%s（连续 %u 次心跳无应答），改用 %d s 探测",
                         dev_mac_str(s_table[i].mac), (unsigned)s_table[i].miss,
                         APP_OFFLINE_PROBE_MS / 1000);
            } else {
                s_table[i].next_poll_ms = now_ms + APP_IDENTIFY_SLOW_MS;
                if (!s_table[i].probe_notified) {
                    s_table[i].probe_notified = true;
                    ESP_LOGI(TAG, "%08" PRIx32 " 上有主机应答 DHCP，但不响应设备发现协议"
                                   "（多为 PC / 非 OTA 设备），%d s 探测一次即可",
                             s_table[i].ip, APP_IDENTIFY_SLOW_MS / 1000);
                }
            }
        } else if (s_table[i].identified) {
            /* 离线期的慢探探到了：补一条恢复日志，否则只有离线警告、没有闭环 */
            ESP_LOGI(TAG, "设备恢复在线：%s", dev_mac_str(s_table[i].mac));
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t dev_table_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_table, 0, sizeof(s_table));
    ESP_LOGI(TAG, "设备表就绪：%d 槽", APP_DEV_MAX);
    return ESP_OK;
}

void dev_table_clear(void)
{
    lock();
    memset(s_table, 0, sizeof(s_table));
    unlock();
}

int dev_table_count(void)
{
    int n = 0;
    lock();
    for (int i = 0; i < APP_DEV_MAX; i++) {
        if (s_table[i].used) {
            n++;
        }
    }
    unlock();
    return n;
}

dev_entry_t *dev_table_find_mac(const uint8_t mac[6])
{
    lock();
    dev_entry_t *e = find_mac_locked(mac);
    unlock();
    return e;
}

dev_entry_t *dev_table_find_ip(uint32_t ip_net)
{
    lock();
    dev_entry_t *e = find_ip_locked(ip_net);
    unlock();
    return e;
}

dev_entry_t *dev_table_find_uid(const uint8_t uid[DEV_UID_LEN])
{
    lock();
    dev_entry_t *e = find_uid_locked(uid);
    unlock();
    return e;
}

dev_entry_t *dev_table_upsert_lease(const uint8_t mac[6], uint32_t ip_net, const char *hostname)
{
    lock();

    dev_entry_t *e = find_mac_locked(mac);
    if (e == NULL) {
        /* 同一台设备可能先以「身份应答」进表（MAC 未知），再收到租约事件 */
        e = find_ip_locked(ip_net);
    }
    if (e == NULL) {
        e = alloc_locked();
    }
    if (e == NULL) {
        unlock();
        ESP_LOGE(TAG, "设备表已满且无可回收槽，丢弃 %08" PRIx32, ip_net);
        return NULL;
    }

    if (!e->used) {
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->first_seen_ms = 0;   /* 由调用者首轮 poll_pass 补上真实时刻 */
    }
    if (mac) {
        memcpy(e->mac, mac, 6);
    }
    e->ip = ip_net;
    e->online = true;
    e->miss = 0;
    if (hostname && hostname[0]) {
        strncpy(e->hostname, hostname, sizeof(e->hostname) - 1);
        e->hostname[sizeof(e->hostname) - 1] = '\0';
    }
    e->next_poll_ms = 0;   /* 立刻探测一次 */

    unlock();
    return e;
}

dev_entry_t *dev_table_claim_ip(uint32_t ip_net, const char *why)
{
    lock();
    dev_entry_t *e = find_ip_locked(ip_net);
    if (e == NULL) {
        e = alloc_locked();
        if (e) {
            memset(e, 0, sizeof(*e));
            e->used = true;
            e->ip = ip_net;
            e->online = true;
            ESP_LOGI(TAG, "新设备进表（%s）：%08" PRIx32, why ? why : "未知来源", ip_net);
        }
    }
    unlock();
    return e;
}

void dev_table_apply_identity(dev_entry_t *e, const dev_identity_t *id, uint32_t now_ms)
{
    if (e == NULL || id == NULL) {
        return;
    }

    lock();
    /* 身份变了 = 换了设备（IP 被别人抢走），清掉旧身份重新填 */
    if (e->identified && memcmp(e->id.uid, id->uid, DEV_UID_LEN) != 0) {
        ESP_LOGW(TAG, "IP %08" PRIx32 " 上的设备已更换（UID 变化），刷新身份", e->ip);
    }
    e->id = *id;
    e->identified = true;
    e->online = true;
    e->miss = 0;
    e->next_poll_ms = 0;   /* 从离线状态复活时，立刻恢复高频心跳 */
    e->last_seen_ms = now_ms;
    if (e->first_seen_ms == 0) {
        e->first_seen_ms = now_ms;
    }
    unlock();
}

void dev_table_poll_pass(uint32_t now_ms, dev_poll_cb_t cb)
{
    if (cb == NULL) {
        return;
    }

    lock();
    for (int i = 0; i < APP_DEV_MAX; i++) {
        dev_entry_t *e = &s_table[i];
        if (!e->used || e->ip == 0) {
            continue;
        }
        if (now_ms < e->next_poll_ms) {
            continue;
        }
        if (e->first_seen_ms == 0) {
            e->first_seen_ms = now_ms;
        }

        uint32_t interval = cb(e, now_ms);   /* 只做一次 sendto，不阻塞 */
        if (interval > 0) {
            e->next_poll_ms = now_ms + interval;
            if (e->miss < 0xFF) {
                e->miss++;
            }
        }
    }
    refresh_online_locked(now_ms);
    unlock();
}

void dev_table_refresh_online(uint32_t now_ms)
{
    lock();
    refresh_online_locked(now_ms);
    unlock();
}

int dev_table_snapshot(dev_entry_t *out, int max)
{
    int n = 0;
    if (out == NULL || max <= 0) {
        return 0;
    }
    lock();
    for (int i = 0; i < APP_DEV_MAX && n < max; i++) {
        if (s_table[i].used) {
            out[n++] = s_table[i];
        }
    }
    unlock();
    return n;
}

/* ------------------------------------------------------------------ */
/* 身份序列化：固定 44 字节，大端                                       */
/* ------------------------------------------------------------------ */

size_t dev_identity_serialize(const dev_identity_t *id, uint8_t *out, size_t cap)
{
    if (id == NULL || out == NULL || cap < DEV_IDENTITY_WIRE_LEN) {
        return 0;
    }
    memset(out, 0, DEV_IDENTITY_WIRE_LEN);

    memcpy(out + 0, id->uid, DEV_UID_LEN);

    size_t mlen = strnlen(id->model, DEV_MODEL_LEN);
    memcpy(out + 16, id->model, mlen);                 /* 不足部分补 0 */

    out[32] = (uint8_t)(id->hw_ver >> 8);
    out[33] = (uint8_t)(id->hw_ver & 0xFF);

    memcpy(out + 34, id->fw_ver, 4);

    out[38] = (uint8_t)(id->flash_size >> 24);
    out[39] = (uint8_t)(id->flash_size >> 16);
    out[40] = (uint8_t)(id->flash_size >> 8);
    out[41] = (uint8_t)(id->flash_size);

    out[42] = (uint8_t)(id->caps >> 8);
    out[43] = (uint8_t)(id->caps & 0xFF);

    return DEV_IDENTITY_WIRE_LEN;
}

bool dev_identity_parse(const uint8_t *buf, size_t len, dev_identity_t *id)
{
    if (buf == NULL || id == NULL || len < DEV_IDENTITY_WIRE_LEN) {
        return false;
    }
    memset(id, 0, sizeof(*id));

    memcpy(id->uid, buf + 0, DEV_UID_LEN);
    memcpy(id->model, buf + 16, DEV_MODEL_LEN);
    id->model[DEV_MODEL_LEN] = '\0';

    id->hw_ver = (uint16_t)(((uint16_t)buf[32] << 8) | buf[33]);
    memcpy(id->fw_ver, buf + 34, 4);
    id->flash_size = ((uint32_t)buf[38] << 24) | ((uint32_t)buf[39] << 16) |
                     ((uint32_t)buf[40] << 8) | (uint32_t)buf[41];
    id->caps = (uint16_t)(((uint16_t)buf[42] << 8) | buf[43]);
    return true;
}

const char *dev_mac_str(const uint8_t mac[6])
{
    static char buf[4][18];
    static int idx = 0;

    char *p = buf[idx];
    idx = (idx + 1) & 3;
    if (mac == NULL) {
        snprintf(p, 18, "--:--:--:--:--:--");
    } else {
        snprintf(p, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return p;
}
