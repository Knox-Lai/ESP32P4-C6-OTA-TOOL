/**
 * @file  ota_client.c
 * @brief 升级会话引擎实现（工具侧 TCP 客户端）
 *
 * 关键实现取舍：
 * 1. **累计确认 + 超时整窗重发**：设备回的 next_offset 表示"到此为止都已落盘"，
 *    所以超时后不必知道哪一帧丢了 —— 把发送游标退回 acked 重发即可。
 *    代价是丢 1 帧会重发至多一个窗口（默认 4 KB），100M 直连下可忽略。
 * 2. **读超时用 SO_RCVTIMEO + deadline 循环**，不用 select 轮询：一帧最多两段
 *    （头 12 B、剩余），逻辑最简，且不会因分片延迟到达而误判超时。
 * 3. **重同步**：MAGIC 不对就丢 1 字节再找，最多丢 64 字节仍找不到才判协议错。
 *    设备把半帧、多余字节混进来时靠这条自愈，不必整条会话重来。
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        /* close()：socket fd 在 IDF 里走 VFS，POSIX close 可用 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/sockets.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "dev_table.h"
#include "ota_client.h"
#include "ota_frame.h"
#include "ota_proto.h"

static const char *TAG = "ota_client";

/* ---------------- 会话结构 ---------------- */

typedef struct {
    int     fd;
    size_t  len;
    uint8_t buf[OTA_FRAME_MAX];
} ota_rx_t;

typedef struct {
    uint32_t    ip_net;
    uint8_t     uid[DEV_UID_LEN];
    uint8_t     mac[6];
    ota_image_t img;

    int         fd;
    uint32_t    seq;
    ota_rx_t    rx;
    uint8_t     tx[OTA_FRAME_MAX];

    uint32_t    chunk;
    uint8_t     window;
    uint8_t     err;                /* OTA_ERR_*，供任务给出可读结论 */
} ota_session_t;

/* ---------------- 状态（供 UI / 命令行读） ---------------- */

static SemaphoreHandle_t s_lock;
static ota_status_t      s_status;
static volatile bool     s_busy;
static uint32_t          s_start_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 线上 MODEL 字段固定 DEV_MODEL_LEN 字节、不足补 0；目标数组多留 1 字节放结尾 0。
 * 不用 strncpy：源也是 [DEV_MODEL_LEN + 1] 的数组，strncpy 拷满 16 字节时
 * 编译器按"输出可能未终止"报 -Werror=stringop-truncation。 */
static void model_fill(char *dst, const char *src)
{
    size_t n = strnlen(src, DEV_MODEL_LEN);
    memcpy(dst, src, n);
    memset(dst + n, 0, DEV_MODEL_LEN - n);
    dst[DEV_MODEL_LEN] = '\0';
}

static void status_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void status_unlock(void) { xSemaphoreGive(s_lock); }

static void status_set_state(ota_state_t st)
{
    status_lock();
    s_status.state = st;
    status_unlock();
    ESP_LOGD(TAG, "状态 -> %s", ota_state_str(st));
}

static void status_set_progress(uint32_t acked)
{
    status_lock();
    s_status.acked = acked;
    status_unlock();
}

static void status_finish(bool ok, uint8_t err, const char *msg)
{
    status_lock();
    s_status.state      = ok ? OTA_STATE_DONE : OTA_STATE_FAILED;
    s_status.err_code   = ok ? OTA_ERR_OK : err;
    s_status.elapsed_ms = now_ms() - s_start_ms;
    strncpy(s_status.msg, msg ? msg : "", sizeof(s_status.msg) - 1);
    s_status.msg[sizeof(s_status.msg) - 1] = '\0';
    status_unlock();
}

const char *ota_state_str(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_IDLE:    return "空闲";
    case OTA_STATE_CONNECT: return "连接中";
    case OTA_STATE_BEGIN:   return "协商中";
    case OTA_STATE_STREAM:  return "传输中";
    case OTA_STATE_END:     return "校验中";
    case OTA_STATE_REBOOT:  return "重启中";
    case OTA_STATE_VERIFY:  return "等设备回连";
    case OTA_STATE_DONE:    return "成功";
    case OTA_STATE_FAILED:  return "失败";
    default:                return "?";
    }
}

/* ---------------- 套接字 ---------------- */

static void set_sock_timeout(int fd, uint32_t ms)
{
    struct timeval tv;
    if (ms < 50) {
        ms = 50;
    }
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int tcp_connect(uint32_t ip_net, uint16_t port, uint32_t timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() 失败 errno=%d", errno);
        return -1;
    }

    /* 非阻塞 connect + select 等完成：阻塞 connect 在 lwIP 里由 TCP_SYNMAXRTX
     * 决定重试次数，可能拖几十秒，工具侧必须有明确上限。
     *
     * 注意用 lwip_fcntl 而不是 fcntl：IDF 的 lwipopts.h 里
     * LWIP_POSIX_SOCKETS_IO_NAMES = 0，POSIX 那个 fcntl 宏根本没定义。 */
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = ip_net;

    int r = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (r < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect 立即失败 errno=%d", errno);
        close(fd);
        return -1;
    }
    if (r < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        r = select(fd + 1, NULL, &wset, NULL, &tv);
        if (r <= 0) {
            ESP_LOGW(TAG, "connect 超时 %u ms", (unsigned)timeout_ms);
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t el = sizeof(so_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &el) < 0 || so_err != 0) {
            ESP_LOGW(TAG, "connect 被拒 so_err=%d", so_err);
            close(fd);
            return -1;
        }
    }

    lwip_fcntl(fd, F_SETFL, flags);       /* 恢复阻塞 */

    int one = 1;
    /* 必须关 Nagle：协议是"发一窗等一 ACK"，Nagle 会攒到 40 ms 级别，
     * 每轮都多等几十毫秒，1 MB 固件要多花几十秒。 */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return fd;
}

static esp_err_t sock_send_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n > 0) {
            p   += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_frame(ota_session_t *s, uint8_t cmd, const void *payload, uint16_t plen)
{
    ota_frame_t f;
    f.ver   = OTA_PROTO_VER;
    f.cmd   = cmd;
    f.len   = plen;
    f.seq   = ++s->seq;
    f.flags = 0;
    if (plen > 0 && payload != NULL) {
        memcpy(f.payload, payload, plen);
    }

    size_t n = ota_frame_encode(&f, s->tx, sizeof(s->tx));
    if (n == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return sock_send_all(s->fd, s->tx, n);
}

/* ---------------- 收帧（带重同步） ---------------- */

static esp_err_t rx_fill(ota_session_t *s, size_t need, int64_t deadline)
{
    ota_rx_t *r = &s->rx;

    while (r->len < need) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        set_sock_timeout(s->fd, (uint32_t)(left_us / 1000) + 1);

        int n = recv(s->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n > 0) {
            r->len += (size_t)n;
            continue;
        }
        if (n == 0) {
            ESP_LOGW(TAG, "对端关闭连接");
            return ESP_FAIL;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;               /* 交给 deadline 判超时 */
        }
        ESP_LOGW(TAG, "recv 失败 errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void rx_drop(ota_session_t *s, size_t n)
{
    ota_rx_t *r = &s->rx;
    if (n >= r->len) {
        r->len = 0;
        return;
    }
    memmove(r->buf, r->buf + n, r->len - n);
    r->len -= n;
}

/**
 * @brief 取一帧。忽略 expect_cmd 的不匹配（只记日志），由调用方按 f->cmd 分支，
 *        这样设备提前回 RESULT 时能立刻识别，而不是死等。
 */
static esp_err_t rx_frame(ota_session_t *s, uint8_t expect_cmd, ota_frame_t *f, uint32_t timeout_ms)
{
    ota_rx_t *r = &s->rx;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int resync = 0;

    for (;;) {
        while (r->len >= 2 && ota_get_u16(r->buf) != OTA_FRAME_MAGIC) {
            rx_drop(s, 1);
            if (++resync > 64) {
                ESP_LOGE(TAG, "连续 %d 字节找不到帧头，判协议错", resync);
                return ESP_FAIL;
            }
        }

        if (r->len < OTA_HDR_LEN) {
            esp_err_t err = rx_fill(s, OTA_HDR_LEN, deadline);
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        uint16_t plen = ota_get_u16(r->buf + 4);
        if (plen > OTA_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "非法 LEN=%u，丢 1 字节重同步", plen);
            rx_drop(s, 1);
            resync++;
            continue;
        }

        size_t total = OTA_HDR_LEN + (size_t)plen + OTA_CRC_LEN;
        if (r->len < total) {
            esp_err_t err = rx_fill(s, total, deadline);
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        if (!ota_frame_decode(r->buf, r->len, f)) {
            ESP_LOGW(TAG, "CRC 错，丢 1 字节重同步");
            rx_drop(s, 1);
            resync++;
            continue;
        }
        rx_drop(s, total);

        if (f->ver != OTA_PROTO_VER) {
            ESP_LOGW(TAG, "协议版本 %u != %u，丢弃该帧", (unsigned)f->ver, (unsigned)OTA_PROTO_VER);
            continue;
        }
        if (expect_cmd != 0 && f->cmd != expect_cmd) {
            ESP_LOGW(TAG, "期望 cmd=0x%02x，收到 0x%02x", expect_cmd, (unsigned)f->cmd);
        }
        return ESP_OK;
    }
}

/* ---------------- 各阶段 ---------------- */

static esp_err_t do_begin(ota_session_t *s, uint32_t *resume_off)
{
    ota_begin_t b;
    memset(&b, 0, sizeof(b));
    b.fw_size  = s->img.size;
    b.fw_crc32 = s->img.crc32;
    b.fw_ver   = s->img.fw_ver;
    b.hw_min   = s->img.hw_min;
    b.hw_max   = s->img.hw_max;
    model_fill(b.model, s->img.model);
    memcpy(b.sha256, s->img.sha256, 32);

    uint8_t pl[OTA_BEGIN_PAYLOAD_LEN];
    if (ota_begin_encode(&b, pl, sizeof(pl)) != OTA_BEGIN_PAYLOAD_LEN) {
        s->err = OTA_ERR_LOCAL_PROTO;
        return ESP_FAIL;
    }

    for (int attempt = 1; attempt <= APP_OTA_N_RETRY; attempt++) {
        esp_err_t err = send_frame(s, OTA_CMD_OTA_BEGIN, pl, sizeof(pl));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "发 OTA_BEGIN 失败(%s)，第 %d 次", esp_err_to_name(err), attempt);
            continue;
        }

        ota_frame_t f;
        err = rx_frame(s, OTA_CMD_OTA_BEGIN_ACK, &f, APP_OTA_T_ACK_MS);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "等 BEGIN_ACK 超时，第 %d 次", attempt);
            s->err = OTA_ERR_TIMEOUT;
            continue;
        }
        if (err != ESP_OK) {
            s->err = OTA_ERR_LOCAL_PROTO;
            return err;
        }

        if (f.cmd == OTA_CMD_OTA_RESULT) {
            ota_result_t r;
            if (ota_result_decode(f.payload, f.len, &r)) {
                s->err = r.err_code ? r.err_code : OTA_ERR_LOCAL_PROTO;
                ESP_LOGE(TAG, "设备直接返回 RESULT：%s", ota_err_str(s->err));
            }
            return ESP_FAIL;
        }

        ota_begin_ack_t ack;
        if (!ota_begin_ack_decode(f.payload, f.len, &ack)) {
            ESP_LOGW(TAG, "BEGIN_ACK 长度不足 %u", (unsigned)f.len);
            continue;
        }
        if (ack.status != OTA_ERR_OK) {
            s->err = ack.status;
            ESP_LOGE(TAG, "设备拒绝升级：0x%02x %s", ack.status, ota_err_str(ack.status));
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* 协商结果做合法性收敛：设备报怪值也不能把工具带崩 */
        uint16_t chunk = ack.chunk_size;
        if (chunk < OTA_CHUNK_MIN) {
            chunk = OTA_CHUNK_MIN;
        }
        if (chunk > OTA_CHUNK_MAX) {
            chunk = OTA_CHUNK_MAX;
        }
        chunk = (uint16_t)(chunk & ~3u);
        if (chunk < OTA_CHUNK_MIN) {
            chunk = OTA_CHUNK_MIN;
        }
        s->chunk = chunk;

        uint8_t win = ack.window;
        if (win < OTA_WINDOW_MIN) {
            win = OTA_WINDOW_MIN;
        }
        if (win > APP_OTA_WINDOW) {
            win = APP_OTA_WINDOW;
        }
        if (win > OTA_WINDOW_MAX) {
            win = OTA_WINDOW_MAX;
        }
        s->window = win;

        uint32_t off = ack.resume_offset;
        if (off > s->img.size) {
            off = 0;
        }
        off -= off % chunk;             /* 必须按 chunk 对齐，否则设备判 E_OFFSET */

        ESP_LOGI(TAG, "协商成功：session=0x%08" PRIx32 " chunk=%u window=%u 续传起点=%" PRIu32,
                 ack.session_id, (unsigned)chunk, (unsigned)win, off);
        *resume_off = off;
        return ESP_OK;
    }

    s->err = OTA_ERR_TIMEOUT;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t do_stream(ota_session_t *s, uint32_t start_off)
{
    const uint32_t size      = s->img.size;
    const uint32_t chunk     = s->chunk;
    const uint32_t win_bytes = (uint32_t)s->window * chunk;

    uint32_t acked    = start_off;
    uint32_t send_off = start_off;
    int      retries  = 0;
    uint32_t next_log = 10;

    uint8_t data[OTA_DATA_HDR_LEN + OTA_MAX_CHUNK];

    while (acked < size) {
        while (send_off < size && (send_off - acked) < win_bytes) {
            uint32_t n = size - send_off;
            if (n > chunk) {
                n = chunk;
            }
            esp_err_t err = s->img.read(&s->img, send_off, data + OTA_DATA_HDR_LEN, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "读镜像失败 off=%" PRIu32 " len=%" PRIu32 " (%s)",
                         send_off, n, esp_err_to_name(err));
                s->err = OTA_ERR_LOCAL_PROTO;
                return err;
            }
            ota_put_u32(data, send_off);

            err = send_frame(s, OTA_CMD_OTA_DATA, data, (uint16_t)(OTA_DATA_HDR_LEN + n));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "发 DATA off=%" PRIu32 " 失败(%s)", send_off, esp_err_to_name(err));
                s->err = OTA_ERR_LOCAL_PROTO;
                return err;
            }
            send_off += n;
        }

        ota_frame_t f;
        esp_err_t err = rx_frame(s, OTA_CMD_OTA_DATA_ACK, &f, APP_OTA_T_ACK_MS);

        if (err == ESP_ERR_TIMEOUT) {
            if (++retries > APP_OTA_N_RETRY) {
                ESP_LOGE(TAG, "DATA_ACK 连续 %d 次超时（已确认 %" PRIu32 "/%" PRIu32 "）",
                         retries, acked, size);
                s->err = OTA_ERR_TIMEOUT;
                return ESP_ERR_TIMEOUT;
            }
            ESP_LOGW(TAG, "等 DATA_ACK 超时，从 %" PRIu32 " 重发（第 %d 次）", acked, retries);
            send_off = acked;           /* 累计确认：整窗重发即可 */
            continue;
        }
        if (err != ESP_OK) {
            s->err = OTA_ERR_LOCAL_PROTO;
            return err;
        }

        if (f.cmd == OTA_CMD_OTA_RESULT) {
            ota_result_t r;
            if (ota_result_decode(f.payload, f.len, &r)) {
                s->err = r.err_code ? r.err_code : OTA_ERR_LOCAL_PROTO;
                ESP_LOGE(TAG, "设备提前返回 RESULT：%s (detail=0x%08" PRIx32 ")",
                         ota_err_str(s->err), r.detail);
            }
            return ESP_FAIL;
        }

        ota_data_ack_t ack;
        if (!ota_data_ack_decode(f.payload, f.len, &ack)) {
            ESP_LOGW(TAG, "DATA_ACK 长度不足 %u", (unsigned)f.len);
            continue;
        }

        if (ack.status != OTA_ERR_OK) {
            s->err = ack.status;
            ESP_LOGE(TAG, "设备在偏移 %" PRIu32 " 报错：0x%02x %s",
                     ack.next_offset, ack.status, ota_err_str(ack.status));
            if (ack.status == OTA_ERR_CRC && ++retries <= APP_OTA_N_RETRY) {
                acked    = ack.next_offset;
                send_off = ack.next_offset;   /* 单帧损坏：从确认点重发 */
                continue;
            }
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (ack.next_offset > size) {
            ESP_LOGE(TAG, "设备确认偏移越界：%" PRIu32 " > %" PRIu32, ack.next_offset, size);
            s->err = OTA_ERR_OFFSET;
            return ESP_FAIL;
        }
        if (ack.next_offset > send_off) {
            ESP_LOGE(TAG, "设备确认了尚未发送的偏移：%" PRIu32 " > %" PRIu32,
                     ack.next_offset, send_off);
            s->err = OTA_ERR_LOCAL_PROTO;
            return ESP_FAIL;
        }

        if (ack.next_offset != acked) {
            acked   = ack.next_offset;
            retries = 0;
            status_set_progress(acked);

            uint32_t pct = (uint32_t)(((uint64_t)acked * 100u) / size);
            if (pct >= next_log) {
                ESP_LOGI(TAG, "进度 %" PRIu32 "%%（%" PRIu32 "/%" PRIu32 " B）",
                         pct, acked, size);
                next_log = pct + 10;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t do_end(ota_session_t *s)
{
    for (int attempt = 1; attempt <= APP_OTA_N_RETRY; attempt++) {
        esp_err_t err = send_frame(s, OTA_CMD_OTA_END, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "发 OTA_END 失败(%s)", esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "已发送 OTA_END，等设备整包校验（CRC32 + SHA-256）…");

        ota_frame_t f;
        err = rx_frame(s, OTA_CMD_OTA_RESULT, &f, APP_OTA_T_RESULT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "等 RESULT 超时（%u ms），第 %d 次",
                     (unsigned)APP_OTA_T_RESULT_MS, attempt);
            s->err = OTA_ERR_TIMEOUT;
            continue;
        }
        if (err != ESP_OK) {
            s->err = OTA_ERR_LOCAL_PROTO;
            return err;
        }

        ota_result_t r;
        if (!ota_result_decode(f.payload, f.len, &r)) {
            ESP_LOGW(TAG, "RESULT 长度不足 %u", (unsigned)f.len);
            continue;
        }
        if (r.status != 0) {
            s->err = r.err_code ? r.err_code : OTA_ERR_CRC;
            ESP_LOGE(TAG, "设备校验失败：0x%02x %s (detail=0x%08" PRIx32 ")",
                     r.err_code, ota_err_str(s->err), r.detail);
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "设备校验通过");
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static uint32_t fw_of(const uint8_t v[4])
{
    return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
           ((uint32_t)v[2] << 8) | (uint32_t)v[3];
}

static dev_entry_t *find_target(const ota_session_t *s)
{
    bool uid_zero = true;
    for (int i = 0; i < DEV_UID_LEN; i++) {
        if (s->uid[i] != 0) {
            uid_zero = false;
            break;
        }
    }
    dev_entry_t *e = NULL;
    if (!uid_zero) {
        e = dev_table_find_uid(s->uid);     /* 换 IP 也能认到人 */
    }
    if (e == NULL) {
        e = dev_table_find_ip(s->ip_net);
    }
    return e;
}

static esp_err_t do_verify(ota_session_t *s)
{
    const uint32_t deadline = now_ms() + APP_OTA_T_REBOOT_MS;
    bool seen_offline = false;

    while ((int32_t)(deadline - now_ms()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(APP_OTA_VERIFY_PERIOD_MS));

        dev_entry_t *e = find_target(s);
        if (e == NULL) {
            continue;
        }
        if (!e->online || !e->identified) {
            if (!seen_offline) {
                seen_offline = true;
                ESP_LOGI(TAG, "设备已离线（正在重启）");
            }
            continue;
        }

        uint32_t ver = fw_of(e->id.fw_ver);
        if (ver == s->img.fw_ver) {
            ESP_LOGI(TAG, "设备已用新固件上线：fw=%s", ota_fw_ver_str(ver));
            return ESP_OK;
        }

        if (seen_offline) {
            ESP_LOGW(TAG, "设备已回连但版本仍为 %s（目标 %s），继续观察",
                     ota_fw_ver_str(ver), ota_fw_ver_str(s->img.fw_ver));
            seen_offline = false;
        }
    }

    ESP_LOGE(TAG, "等了 %u ms 没等到目标版本上线", (unsigned)APP_OTA_T_REBOOT_MS);
    s->err = OTA_ERR_LOCAL_VERIFY;
    return ESP_ERR_TIMEOUT;
}

/* ---------------- 任务 ---------------- */

static void ota_task(void *arg)
{
    ota_session_t *s = (ota_session_t *)arg;
    esp_err_t err = ESP_OK;
    const char *msg = "未知结果";

    s->fd = -1;

    do {
        status_set_state(OTA_STATE_CONNECT);
        s->fd = tcp_connect(s->ip_net, APP_OTA_PORT, APP_OTA_CONNECT_TIMEOUT_MS);
        if (s->fd < 0) {
            s->err = OTA_ERR_LOCAL_CONNECT;
            msg = "连不上设备的 4011 端口（设备端 OTA 服务没起来？）";
            err = ESP_FAIL;
            break;
        }

        status_set_state(OTA_STATE_BEGIN);
        uint32_t resume = 0;
        err = do_begin(s, &resume);
        if (err != ESP_OK) {
            msg = "协商失败（见错误码）";
            break;
        }

        status_set_state(OTA_STATE_STREAM);
        err = do_stream(s, resume);
        if (err != ESP_OK) {
            msg = "数据传输失败";
            break;
        }

        status_set_state(OTA_STATE_END);
        err = do_end(s);
        if (err != ESP_OK) {
            msg = "设备整包校验失败";
            break;
        }

        /* 设备不回应答，断开即可；它会在 100 ms 后重启。
         * 这里等 200 ms 再 close，保证 REBOOT 帧真的送达 —— 设备要是没收到，
         * 新固件已经在 Flash 里但一直不重启，工具会白等 60 s 超时。 */
        status_set_state(OTA_STATE_REBOOT);
        send_frame(s, OTA_CMD_REBOOT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        shutdown(s->fd, SHUT_WR);
        close(s->fd);
        s->fd = -1;
        ESP_LOGI(TAG, "已请求设备重启，等它重新上线…");

        status_set_state(OTA_STATE_VERIFY);
        err = do_verify(s);
        if (err != ESP_OK) {
            msg = "设备没有以目标版本上线（跳转失败或已回滚）";
            break;
        }
        msg = "升级成功（设备已用新固件上线）";
    } while (0);

    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "会话结束：成功 —— %s", msg);
    } else {
        ESP_LOGE(TAG, "会话结束：失败 [0x%02x %s] —— %s",
                 s->err, ota_err_str(s->err), msg);
    }

    status_finish(err == ESP_OK, s->err, msg);

    free(s);
    s_busy = false;
    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t ota_client_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    s_busy = false;
    return ESP_OK;
}

bool ota_client_busy(void)
{
    return s_busy;
}

void ota_client_get_status(ota_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return;
    }
    status_lock();
    *out = s_status;
    status_unlock();
}

esp_err_t ota_client_start(uint32_t ip_net,
                           const uint8_t uid[DEV_UID_LEN],
                           const uint8_t mac[6],
                           const ota_image_t *img)
{
    if (s_lock == NULL || img == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (img->size == 0 || img->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_session_t *s = (ota_session_t *)calloc(1, sizeof(ota_session_t));
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s->ip_net = ip_net;
    if (uid != NULL) {
        memcpy(s->uid, uid, DEV_UID_LEN);
    }
    if (mac != NULL) {
        memcpy(s->mac, mac, 6);
    }
    s->img   = *img;                    /* 整体拷贝，调用方不必保持有效 */
    s->chunk = APP_OTA_DEFAULT_CHUNK;
    s->window = APP_OTA_DEFAULT_WINDOW;

    s_busy = true;
    s_start_ms = now_ms();

    status_lock();
    memset(&s_status, 0, sizeof(s_status));
    s_status.state   = OTA_STATE_CONNECT;
    s_status.total   = img->size;
    model_fill(s_status.model, img->model);
    snprintf(s_status.dev, sizeof(s_status.dev), "%02x:%02x:%02x:%02x:%02x:%02x",
             s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
    strncpy(s_status.msg, "会话已启动", sizeof(s_status.msg) - 1);
    status_unlock();

    ESP_LOGI(TAG, "开始升级 %s（%s，fw=%s，%u KB）",
             s_status.dev, img->model, ota_fw_ver_str(img->fw_ver),
             (unsigned)(img->size / 1024));

    BaseType_t ok = xTaskCreate(ota_task, "ota_session", APP_OTA_TASK_STACK, s,
                                tskIDLE_PRIORITY + 4, NULL);
    if (ok != pdPASS) {
        free(s);
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
