/**
 * @file  net_wifi.c
 * @brief WiFi STA 实现（P4 + C6 / esp_hosted）
 *
 * 三条设计约束，都是踩过才定下来的：
 *
 * 1) **初始化不能放在 app_main 主路径**。
 *    P4 的 esp_wifi_init 要先和 C6 走一遍 SDIO 握手，最坏几秒；
 *    直接放在 app_main 里会让屏幕第一帧白等。所以 init 只读 NVS 就返回，
 *    真正的 bringup 在自己的任务里做。
 *
 * 2) **事件回调和任务分工明确**。
 *    事件回调只改状态字段（几条 8 位写），不做 esp_wifi_connect 这种耗时调用；
 *    连接、重试、RSSI 采样都在 wifi 任务的 1 s 循环里做。这样既不会在
 *    WiFi 事件任务里阻塞 lwIP，也不会因为重入搞乱状态机。
 *
 * 3) **凭据自己存**，不用 esp_wifi_set_storage(FLASH)。
 *    驱动自带的存储是"单条覆盖"语义，且和 hosted 的 NVS 分区布局耦合；
 *    自己用 namespace "wifi" 存三样东西，想忘就忘、想改就改，还能同时
 *    记录"用户是不是把开关关了"。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"   /* esp_netif_create_default_wifi_sta */
#include "nvs.h"

#include "net_wifi.h"

static const char *TAG = "wifi";

#define NVS_NS          "wifi"
#define KEY_EN          "en"
#define KEY_SSID        "ssid"      /* 最近一次连接的目标（开机自动重连用） */
#define KEY_PASS        "pass"
#define KEY_VER         "cfgv"
/* 多网络凭据表：连过的每个网络都记住密码，列表里置顶显示。
 * key 用 "sn0"/"pw0" 这种短名（NVS key 上限 15 字节）。 */
#define KEY_SV_N        "sv_n"
#define KEY_SV_S        "sn"
#define KEY_SV_P        "pw"
/* 配置版本：读到旧版本（或没有版本号）时，不认里面存的 en。
 * 原因：上一版固件默认是"关"，用户为了用 WiFi 反复拨过开关，NVS 里很可能
 * 留着一个不是他本意的 0；新版默认改成"开"，这里统一按新默认走一次，
 * 之后用户再关开关就会写上新版本号，下次开机就照他的选择来。 */
#define CFG_VER         1

#define WIFI_TASK_STACK 5120
#define WIFI_TASK_PRIO  4

#define SCAN_MAX        NET_WIFI_SCAN_MAX   /* 扫描结果上限（再多屏幕也放不下） */
#define RSSI_PERIOD_MS  5000    /* 已连接时刷新信号强度的周期 */
#define RETRY_BASE_MS   2000
#define RETRY_MAX_MS    30000
/* 一轮里最多自动重连几次，用完就停手。
 *
 * 退避重连原来是"2 s 起步、每次 +2 s、封顶 30 s、永不放弃"。现场日志里
 * 密码错 / AP 不在时的表现是一长串"第 N 次重连"，界面永远一句"正在重试…"：
 * 既看不出结论，也没人知道它还要试多久。
 * 3 次（间隔 2 / 4 / 6 s，合计约 12 s）够覆盖"AP 刚重启、信号抖一下"这类
 * 临时故障；再失败基本就是确定性问题（密码错、AP 不在、hosted 链路不通），
 * 停下来把原因钉在界面上，等人来处理更诚实。
 * 计数在每次用户动作（点选网络 / 拨开关）或成功拿到 IP 时清零。 */
#define RETRY_MAX_TIMES 3
/* 连接尝试的超时：正常 WPA2 关联 1~3 s。hosted 环境下偶尔收不到
 * WIFI_EVENT_STA_DISCONNECTED，状态会永远停在 CONNECTING，
 * 而重连逻辑只在 FAILED 触发 —— 没有这个超时，一次卡死就再也不连了。 */
#define CONN_TIMEOUT_MS 20000
/* 单次扫描的超时：正常一轮扫描 2~5 s。hosted 环境下偶尔收不到 SCAN_DONE，
 * s_scanning 就一直为真 —— 之后所有扫描请求都会被 "if (s_scanning) return OK"
 * 吃掉，界面永远"扫描中…"。 */
#define SCAN_TIMEOUT_MS 15000
/* 主动连接后的"重配窗口"。
 *
 * esp_wifi_set_config() 在已连接状态下调用会让驱动先断掉旧链路，
 * 于是发出一条第 WIFI_EVENT_STA_DISCONNECTED，reason 通常是
 * 3 (AUTH_LEAVE) / 8 (ASSOC_LEAVE)。旧代码把它和真正的握手失败一视同仁，
 * 直接进 FAILED —— 界面先闪一下"连接失败"，2 s 退避后才跳到"已连接"。
 * 这个窗口就是用来吃掉这条"自己制造"的断开事件。 */
#define CONN_GRACE_MS   3000
/* 主动 disconnect 之后事件的抑制窗口。
 * push_cfg_and_connect() 每次重配前都会 disconnect 一次以清理驱动状态，
 * 那条 DISCONNECTED（reason 8）是我们自己制造的，必须吞掉；
 * 否则它会立刻被当成断开、触发又一次重连，来回打架。 */
#define DISC_SUPPRESS_MS 1000
/* set_config 之后隔多久才发 connect。
 *
 * 见 push_cfg_and_connect() 里的实测：hosted 下 disconnect / set_config 都会让
 * C6 迟一条 ASSOC_LEAVE(reason 8)，实测落点在 set_config 之后 3 ms ~ 2.9 s。
 * 三连发（disconnect → set_config → connect 同一次调用里发完）时，这条 leave
 * 会追上来把刚开始的关联踢掉，现场只剩 20 s 后的一句"连接超时"。
 * 留 300 ms 给 C6 先把离开处理完 —— 任务按 1 s 节拍，实际是 0.3~1.3 s，
 * 和人手动"断开 → 等一下 → 再点连接"是同一个时序。 */
#define CONNECT_DELAY_MS 300
/* 起驱动（esp_wifi_init / esp_wifi_start）时的重试次数与间隔。
 *
 * C6 的 bringup 是异步的（复位 + 1.5 s 静置 + 最多 5 s 等它应答），而 hosted
 * 的超时策略是"不重试"。开机阶段 LVGL 建页面、FAT 挂载、以太网 PHY 协商都挤在
 * 同一两秒里，C6 偶尔赶不上，RPC 直接 5 s 无响应（现场日志：
 * `eh_host_feat_rpc: request: no response uid=4 msg_id=280 (5000 ms)`）。
 * 4 次 × 2 s 足够把这段竞争让过去，又不会让"确实坏了"的情况拖太久。 */
#define WIFI_BOOT_TRIES     4
#define WIFI_BOOT_RETRY_MS  2000

/* ---------------- 状态 ---------------- */

static bool s_init       = false;   /* net_wifi_init 调用过 */
static bool s_task_up    = false;   /* wifi_task 已经创建过（只允许一次） */
static bool s_enabled    = false;   /* 用户开关 */
static bool s_driver_up  = false;   /* esp_wifi_start() 成功且未 stop */
static volatile bool s_want = false;/* 期望维持连接（决定要不要重连） */
static bool s_have_cfg   = false;   /* NVS 里有可用 SSID */

static volatile net_wifi_state_t s_state = NET_WIFI_OFF;

static char s_ssid[NET_WIFI_SSID_MAX];
static char s_pass[NET_WIFI_PASS_MAX];
static char s_ip[16] = "0.0.0.0";
static volatile int8_t s_rssi = -127;
static volatile int    s_retry = 0;
/* 本次连上的网络要不要写进"已知网络"表（见 net_wifi_connect 的注释）。
 * 用"事后落表"而不是"事前落表"：只有真的连上过，才算已知网络。 */
static volatile bool   s_save_ok = false;
/* 本轮重连预算已用尽（见 RETRY_MAX_TIMES）。
 * 为真时守护循环不再自动重连，状态停在 FAILED 等用户动作；
 * 界面靠 net_wifi_retry_pending() 区分"还在重试"和"已放弃"，
 * 免得两边都显示一句"正在重试…"。 */
static volatile bool   s_retry_stop = false;

/* 连过的网络（SSID + 密码）。下标越小越旧，满了淘汰 [0]。
 * 只有 wifi 任务 / 串口命令 / UI 的一次性任务会改它，且都在 NVS 写之前，
 * 读方（列表标记）只做 strncmp，不需要额外加锁。 */
static char s_sv_ssid[NET_WIFI_SAVED_MAX][NET_WIFI_SSID_MAX];
static char s_sv_pass[NET_WIFI_SAVED_MAX][NET_WIFI_PASS_MAX];
static int  s_sv_n     = 0;
static bool s_sv_dirty = false;     /* 表变过才写 NVS，避免每次开机都擦写 */

static esp_netif_t   *s_netif = NULL;
static net_wifi_cb_t  s_cb    = NULL;

/* 扫描结果：只有 wifi 任务和 UI 读，写入发生在事件回调里，
 * 用 s_scan_seq 做"发布"栅栏 —— UI 先读 seq 再读数组，读到奇数代次就跳过。 */
static net_wifi_ap_t s_aps[SCAN_MAX];
static volatile int  s_ap_n     = 0;
static volatile bool s_scanning = false;
/* 驱动还没起来时收到的扫描请求（典型场景：用户刚把开关拨开就想看列表）。
 * esp_wifi_scan_start() 在 s_driver_up=false 时一定失败，直接返回错误的话
 * 界面上什么都不会发生 —— 所以先挂起，等 STA_START 之后由 wifi 任务补扫。 */
static volatile bool s_scan_wanted = false;
/* 连续"驱动还没就绪"类失败的次数：刚 esp_wifi_start() 的那几百毫秒里
 * esp_wifi_scan_start() 会返回 WIFI_STOP_STATE，这是"再等等"不是"坏了"，
 * 所以自动重试；但要封顶，否则驱动真卡死时任务每秒重试一次、日志刷满。 */
static volatile int  s_scan_fail   = 0;
/* "因为正在连接而推迟扫描"这条日志只打一次（每个扫描请求一次），
 * 否则任务每秒循环一次，串口会被同一句话刷满。 */
static volatile bool s_scan_defer_logged = false;
/* 进入 CONNECTING 的时刻（ms）：用来给卡死的连接尝试兜底超时 */
static volatile int64_t s_conn_since = 0;
/* 本次扫描开始的时刻（ms）：给 SCAN_DONE 丢失兜底 */
static volatile int64_t s_scan_since = 0;
/* 用户在扫描期间点了某个网络：凭据已存，但"下发配置 + connect"被推迟到
 * 扫描结束（见 net_wifi_connect）。这是一次性标志，不会造成重连风暴。 */
static volatile bool s_conn_pending = false;
/* 重配窗口（见 CONN_GRACE_MS）：apply_cfg 时置为 now+3 s。
 * 窗口内收到的 DISCONNECTED 若不属于"真失败"，就不判 FAILED，
 * 改为保持 CONNECTING 并立即重连一次。 */
static volatile int64_t s_grace_until = 0;
/* 窗口内已经"立即重连"过几次。只给 1 次：第二次还断就说明真的有问题，
 * 该老老实实显示失败（比如密码错，第二次也会 reason=15）。 */
static volatile int     s_soft_retry  = 0;
/* 事件回调不碰协议栈（hosted 下 RPC 可能阻塞事件任务），
 * 只置这个标志，由 wifi 任务在 1 s 内发起重连。 */
static volatile bool    s_imm_retry   = false;
/* 主动 disconnect 造成的事件，在这个时刻之前到的 DISCONNECTED 直接忽略 */
static volatile int64_t s_suppress_until = 0;
/* "立即重连"连续失败的次数。切网时驱动常有一小段时间还在清理上一条连接，
 * esp_wifi_connect() 会返回 ESP_ERR_WIFI_STATE；下一拍再试即可。
 * 封顶 5 次，免得驱动真卡住时每秒刷一条日志、永远转圈。 */
static volatile int     s_imm_try     = 0;
/* 进入 FAILED 的时刻（整秒）。UI 用它把"连接失败"在 5 s 后换成"正在重试…"，
 * 否则那行字会一直挂在那儿（用户反馈的问题）。用 32 位秒数是为了
 * 在 32 位目标上读得原子 —— 64 位变量在 UI 任务里读可能被撕裂。 */
static volatile uint32_t s_fail_at_s = 0;
/* 最近一次失败的原因分类（net_wifi_err_t）。同样是 8 位，UI 任务里直接读。
 * 只在 FAILED 期间有意义，set_state() 离开 FAILED 时清零。 */
static volatile uint8_t  s_fail_reason = NET_WIFI_ERR_NONE;
/* 配置已下发、connect 还没发：等 CONNECT_DELAY_MS 到点由 wifi 任务发。
 * 这期间收到的 DISCONNECTED 一律当"离开旧链路的尾巴"丢掉 —— 那时我们
 * 连一次都还没试过，任何断开都不可能是本次连接的结果（见 CONNECT_DELAY_MS）。 */
static volatile bool     s_conn_armed = false;
static volatile int64_t  s_conn_at_ms = 0;
/* connect 本身返回错误（多为 WIFI_STATE："还在清理上一条连接"）的连续次数 */
static volatile int      s_connect_fail = 0;

/* 任务只允许起一次：它自己出错退出后（C6 没响应）再建也还是失败，
 * 反复建只会把内存和日志刷一遍。 */
static void wifi_task(void *arg);

static esp_err_t start_task(void)
{
    if (s_task_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(wifi_task, "wifi", WIFI_TASK_STACK, NULL,
                    WIFI_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task_up = true;
    return ESP_OK;
}

static void notify(void)
{
    if (s_cb != NULL) {
        s_cb();
    }
}

static void set_state(net_wifi_state_t st)
{
    /* 每次进入 FAILED 都重新计时（连续失败时 s_state 不变，会在下面的
     * early-return 里被吃掉，所以这行必须先于它）。界面据此在 5 s 后
     * 把"连接失败"换成"正在重试…"。 */
    if (st == NET_WIFI_FAILED) {
        s_fail_at_s = (uint32_t)(esp_timer_get_time() / 1000000);
    } else {
        s_fail_at_s = 0;
        /* 离开 FAILED 就没必要再留着上一条失败原因：界面此时显示的是
         * 连接中/已连接，残留的原因只会在下一次失败的一瞬间闪一下旧文案。 */
        s_fail_reason = NET_WIFI_ERR_NONE;
    }

    if (s_state == st) {
        return;
    }
    s_state = st;
    if (st == NET_WIFI_CONNECTING) {
        s_conn_since = esp_timer_get_time() / 1000;
    }
    if (st == NET_WIFI_UNAVAILABLE) {
        /* 驱动根本起不来（C6 没烧 slave 固件之类）：挂起的扫描请求永远等不到
         * 兑现，不清掉的话界面会一直显示"正在搜索…"。 */
        s_scan_wanted = false;
    }
    ESP_LOGI(TAG, "状态 → %s", net_wifi_state_str());
    notify();
}

const char *net_wifi_state_str(void)
{
    switch (s_state) {
    case NET_WIFI_OFF:         return "已关闭";
    case NET_WIFI_IDLE:        return "未连接";
    case NET_WIFI_CONNECTING:  return "连接中";
    case NET_WIFI_CONNECTED:   return "已连接";
    case NET_WIFI_FAILED:      return "连接失败";
    case NET_WIFI_UNAVAILABLE: return "硬件不可用";
    default:                   return "未知";
    }
}

/* 失败原因的英文短标签，只给串口日志用（界面走 i18n 的 W_ERR_*）。
 * "停止重试"那条日志带上它，现场贴日志过来就能一眼看出是密码错
 * 还是 hosted 链路根本没回事件，不用再往回翻几十行。 */
static const char *fail_reason_tag(net_wifi_err_t r)
{
    switch (r) {
    case NET_WIFI_ERR_PASSWORD:  return "password";
    case NET_WIFI_ERR_NO_AP:     return "no_ap";
    case NET_WIFI_ERR_REJECTED:  return "rejected";
    case NET_WIFI_ERR_TIMEOUT:   return "timeout";
    case NET_WIFI_ERR_LINK_LOST: return "link_lost";
    case NET_WIFI_ERR_NONE:      return "none";
    default:                     return "other";
    }
}

/* ---------------- 已保存网络表 ---------------- */

static int saved_find(const char *ssid)
{
    if (ssid == NULL) {
        return -1;
    }
    for (int i = 0; i < s_sv_n; i++) {
        if (strncmp(s_sv_ssid[i], ssid, NET_WIFI_SSID_MAX - 1) == 0) {
            return i;
        }
    }
    return -1;
}

/* 记住一组凭据：已存在就原地更新（不改变"新旧"顺序），
 * 不存在就追加；表满则丢掉最旧的一条（[0]）并整体前移。 */
static void saved_put(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    int i = saved_find(ssid);
    if (i < 0) {
        if (s_sv_n < NET_WIFI_SAVED_MAX) {
            i = s_sv_n++;
        } else {
            for (int k = 1; k < NET_WIFI_SAVED_MAX; k++) {
                memcpy(s_sv_ssid[k - 1], s_sv_ssid[k], NET_WIFI_SSID_MAX);
                memcpy(s_sv_pass[k - 1], s_sv_pass[k], NET_WIFI_PASS_MAX);
            }
            i = NET_WIFI_SAVED_MAX - 1;
        }
    }

    /* 按长度拷（同 fill_cfg 的理由）：目标是定长数组，
     * strncpy(dst, src, sizeof(dst)-1) 会被 -Wstringop-truncation 判"可能截断"，
     * 而工程开着 -Werror，等于编不过。 */
    size_t n = strlen(ssid);
    if (n > NET_WIFI_SSID_MAX - 1) {
        n = NET_WIFI_SSID_MAX - 1;
    }
    memset(s_sv_ssid[i], 0, NET_WIFI_SSID_MAX);
    memcpy(s_sv_ssid[i], ssid, n);

    const char *pw = (pass != NULL) ? pass : "";
    n = strlen(pw);
    if (n > NET_WIFI_PASS_MAX - 1) {
        n = NET_WIFI_PASS_MAX - 1;
    }
    memset(s_sv_pass[i], 0, NET_WIFI_PASS_MAX);
    memcpy(s_sv_pass[i], pw, n);

    s_sv_dirty = true;
}

static void saved_load(nvs_handle_t h)
{
    s_sv_n = 0;

    uint8_t n = 0;
    if (nvs_get_u8(h, KEY_SV_N, &n) != ESP_OK) {
        return;
    }
    if (n > NET_WIFI_SAVED_MAX) {
        n = (uint8_t)NET_WIFI_SAVED_MAX;
    }

    for (uint8_t i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "%s%u", KEY_SV_S, (unsigned)i);
        snprintf(kp, sizeof(kp), "%s%u", KEY_SV_P, (unsigned)i);

        size_t len = sizeof(s_sv_ssid[i]);
        if (nvs_get_str(h, ks, s_sv_ssid[i], &len) != ESP_OK || s_sv_ssid[i][0] == '\0') {
            continue;               /* 中间缺一条就跳过，别让后面整体错位 */
        }
        len = sizeof(s_sv_pass[i]);
        if (nvs_get_str(h, kp, s_sv_pass[i], &len) != ESP_OK) {
            s_sv_pass[i][0] = '\0';
        }
        s_sv_n++;
    }
}

static void saved_save(nvs_handle_t h)
{
    if (!s_sv_dirty) {
        return;
    }
    for (int i = 0; i < NET_WIFI_SAVED_MAX; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "%s%d", KEY_SV_S, i);
        snprintf(kp, sizeof(kp), "%s%d", KEY_SV_P, i);

        if (i >= s_sv_n) {
            /* n 变小时清掉尾部残留，否则下次可能读回已"忘记"的网络 */
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
            continue;
        }
        nvs_set_str(h, ks, s_sv_ssid[i]);
        nvs_set_str(h, kp, s_sv_pass[i]);
    }
    nvs_set_u8(h, KEY_SV_N, (uint8_t)s_sv_n);
    s_sv_dirty = false;
}

/* ---------------- NVS ---------------- */

static void cfg_load(void)
{
    /* 默认是"开"：这是插电常驻的升级工具，用户不该每次上电都先摸进设置里拨一下开关。
     * 只有 NVS 里明确写了 0（用户自己关过）才当关闭 —— 用户的选择优先于默认值。
     * 注意这一行必须在 nvs_open 失败也能生效，所以放在最前面。 */
    s_enabled = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint8_t ver = 0;
    const bool ver_ok = (nvs_get_u8(h, KEY_VER, &ver) == ESP_OK && ver >= CFG_VER);

    uint8_t en = 0;
    if (ver_ok && nvs_get_u8(h, KEY_EN, &en) == ESP_OK) {
        s_enabled = (en != 0);
    }

    size_t len = sizeof(s_ssid);
    if (nvs_get_str(h, KEY_SSID, s_ssid, &len) == ESP_OK && s_ssid[0] != '\0') {
        s_have_cfg = true;
        len = sizeof(s_pass);
        if (nvs_get_str(h, KEY_PASS, s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';
        }
    }

    /* 旧固件只存了"最后一组"，把它迁进多网络表。
     * 必须先 saved_load() 再把缺的补进去 —— 反过来的话会被 load 里的
     * s_sv_n = 0 清掉。迁移只置 dirty，真正落盘等下一次 cfg_save。
     *
     * 只在**从旧固件升级上来**时做（!ver_ok）：新固件里"最后一组"可能
     * 是一次没连上的尝试，每次开机都往已知网络里补的话，用户刚把它清掉
     * 的错密码又会被塞回去（那正是"没连上却出现在 Known networks 里"的根源）。 */
    saved_load(h);
    if (!ver_ok && s_have_cfg && saved_find(s_ssid) < 0) {
        saved_put(s_ssid, s_pass);
    }

    nvs_close(h);
}

static void cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败，凭据仅本次生效：%s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(h, KEY_VER, CFG_VER);
    nvs_set_u8(h, KEY_EN, s_enabled ? 1 : 0);
    if (s_have_cfg) {
        nvs_set_str(h, KEY_SSID, s_ssid);
        nvs_set_str(h, KEY_PASS, s_pass);
    } else {
        nvs_erase_key(h, KEY_SSID);
        nvs_erase_key(h, KEY_PASS);
    }
    saved_save(h);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 事件 ---------------- */

static void scan_collect(void)
{
    wifi_ap_record_t recs[SCAN_MAX];
    uint16_t n = SCAN_MAX;

    esp_err_t err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "取扫描结果失败：%s", esp_err_to_name(err));
        n = 0;
    }

    /* 同名 SSID（mesh / 多 AP）只留最强的一条，否则列表里一堆重复项 */
    int out = 0;
    for (int i = 0; i < (int)n && out < SCAN_MAX; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;                       /* 隐藏 SSID */
        }

        int dup = -1;
        for (int k = 0; k < out; k++) {
            if (strncmp(s_aps[k].ssid, (const char *)recs[i].ssid, NET_WIFI_SSID_MAX - 1) == 0) {
                dup = k;
                break;
            }
        }
        if (dup >= 0) {
            if (recs[i].rssi > s_aps[dup].rssi) {
                s_aps[dup].rssi = recs[i].rssi;
            }
            continue;
        }

        net_wifi_ap_t *a = &s_aps[out++];
        memset(a, 0, sizeof(*a));
        strncpy(a->ssid, (const char *)recs[i].ssid, NET_WIFI_SSID_MAX - 1);
        a->rssi     = recs[i].rssi;
        a->authmode = (uint8_t)recs[i].authmode;
        a->saved    = net_wifi_is_saved(a->ssid);
    }

    /* 排序：**已保存的置顶**，同组内按信号从强到弱。
     * 这样界面不用再分两趟取数据，"已知网络"就是数组的前 saved 条
     * （插入排序最多 20 条，比 qsort 的适配器短）。 */
    for (int i = 1; i < out; i++) {
        net_wifi_ap_t t = s_aps[i];
        int j = i - 1;
        while (j >= 0 &&
               ((int)s_aps[j].saved < (int)t.saved ||
                (s_aps[j].saved == t.saved && s_aps[j].rssi < t.rssi))) {
            s_aps[j + 1] = s_aps[j];
            j--;
        }
        s_aps[j + 1] = t;
    }

    s_ap_n = out;
    ESP_LOGI(TAG, "扫描完成：%d 个网络（去重后）", out);
}

/* 断开原因解码：现场排障最需要的就这一行。
 * reason 是 ESP-IDF 的 wifi_err_reason_t，hosted 下也是从 C6 原样回传的，
 * 所以 15/204 基本等于"密码或加密方式不对"，201 是"根本没这个 AP"。 */
static const char *disc_hint(int r)
{
    switch (r) {
    case 2:   return "AUTH_EXPIRE 认证超时";
    case 4:   return "AUTH_FAIL 认证失败（多为密码错）";
    case 8:   return "ASSOC_LEAVE 主动离开";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT 四次握手超时（密码错/加密不兼容）";
    case 200: return "BEACON_TIMEOUT AP 消失";
    case 201: return "NO_AP_FOUND 找不到该 AP";
    case 202: return "AUTH_FAIL 认证失败（密码错）";
    case 203: return "ASSOC_FAIL 关联失败";
    case 204: return "HANDSHAKE_TIMEOUT 握手超时（密码错）";
    case 205: return "CONNECTION_FAIL AP 拒绝连接";
    default:  return "其他原因";
    }
}

/* 把 60 多个 reason 码位归成界面用得上的 4 类（+ 兜底）。
 *
 * 归类的依据是"用户下一步该做什么"：
 *   密码错  → 重拨密码；
 *   找不到  → 看路由/看是不是 5G-only、ch12-13；
 *   被拒绝  → 看 MAC 白名单、客户端数、band steering；
 *   其余    → 只能报"连接失败"，具体码位去串口日志里看 disc_hint()。
 * 所以 4/15/202/204 归一类：它们在现场都是"密码或加密方式不对"的同一种表现。 */
static net_wifi_err_t reason_kind(int r)
{
    switch (r) {
    case 4:                    /* AUTH_FAIL */
    case 15:                   /* 4WAY_HANDSHAKE_TIMEOUT */
    case 202:                  /* AUTH_FAIL */
    case 204:                  /* HANDSHAKE_TIMEOUT */
        return NET_WIFI_ERR_PASSWORD;
    case 200:                  /* BEACON_TIMEOUT */
    case 201:                  /* NO_AP_FOUND */
        return NET_WIFI_ERR_NO_AP;
    case 203:                  /* ASSOC_FAIL */
    case 205:                  /* CONNECTION_FAIL */
        return NET_WIFI_ERR_REJECTED;
    default:
        return NET_WIFI_ERR_OTHER;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        s_driver_up = true;
        set_state(s_want ? NET_WIFI_CONNECTING : NET_WIFI_IDLE);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        int r = (d != NULL) ? (int)d->reason : -1;

        /* 用户主动断开（s_want=false）：状态已经由 net_wifi_disconnect() 置好，
         * 这里只需要保证状态别停在"已连接"。放在 suppress 之前判断，
         * 是为了不被我们自己发起的 disconnect 的抑制窗口吞掉。 */
        if (!s_want) {
            s_rssi = -127;
            snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
            set_state(NET_WIFI_IDLE);
            break;
        }

        /* 配置刚下发、connect 还没发出去（见 CONNECT_DELAY_MS）。
         * 这时候收到的断开只可能是 C6 在收拾旧链路 —— disconnect 和
         * set_config 都会让它迟一条 ASSOC_LEAVE。本次尝试连一次都还没试过，
         * 拿它当结果就会误判失败、触发"立即重连"，把排好队的 connect 顶掉。 */
        if (s_conn_armed) {
            ESP_LOGI(TAG, "关联前的离开事件（reason=%d，%s），忽略", r, disc_hint(r));
            break;
        }

        /* 这次断开是我们自己在 push_cfg_and_connect() 里 disconnect 出来的：
         * 既不算失败，也不触发重连，否则会和紧随其后的 connect 打架。 */
        if (esp_timer_get_time() / 1000 < s_suppress_until) {
            ESP_LOGI(TAG, "忽略主动 disconnect 产生的断开（reason=%d，%s）", r, disc_hint(r));
            break;
        }

        s_rssi = -127;
        snprintf(s_ip, sizeof(s_ip), "0.0.0.0");

        /* ---- 区分"自己造成的断开"和"真连不上" ----
         *
         * 点一个已保存的网络（或从 A 切到 B）时，驱动要先离开旧链路：
         *   reason 3 (AUTH_LEAVE) / 8 (ASSOC_LEAVE) = 本机主动离开;
         *   set_config 之后 3 s 内到达的其他 reason = 重配过程中的中间态。
         * 这两种都不该显示"连接失败" —— 用户看到的是点了之后闪一下失败、
         * 2 s 退避后才连上（就是这次反馈的现象）。
         *
         * 处理方式：状态保持 CONNECTING，置 s_imm_retry 让任务下一拍立刻重连，
         * 不走 2 s→30 s 的退避；窗口内的"立即重连"只给 1 次，第二次还断
         * （例如密码错的 15）就照旧判 FAILED，错误反馈不受影响。 */
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool leaving   = (r == 3 || r == 8);
        bool in_grace  = (now_ms < s_grace_until);
        /* 只有"正在连接"或"刚下过配置"这两种情形才算切换过程。
         * FAILED 后再掉一次（reason 8）不加这个判断就会插一次多余的 connect，
         * 和退避重连撞在一起。 */
        bool switching = (s_state == NET_WIFI_CONNECTING) || in_grace;

        if (switching && (leaving || s_soft_retry < 1)) {
            if (!leaving) {
                s_soft_retry++;
            }
            ESP_LOGI(TAG, "断开（reason=%d，%s）：连接切换/重配过程中的正常断开，立即重连",
                     r, disc_hint(r));
            s_imm_retry = true;
            break;
        }

        /* reason 常见值：15 = 4WAY_HANDSHAKE_TIMEOUT（密码错），
         * 201 = NO_AP_FOUND（信号没了/关路由），205 = 关联被拒 */
        ESP_LOGW(TAG, "断开（reason=%d，%s），准备重试", r, disc_hint(r));
        s_fail_reason = reason_kind(r);
        set_state(NET_WIFI_FAILED);
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        scan_collect();
        s_scanning = false;
        s_scan_fail = 0;
        notify();
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_retry      = 0;
    s_retry_stop = false;       /* 连上了，下一轮故障重新给满 3 次 */
    set_state(NET_WIFI_CONNECTED);
    ESP_LOGI(TAG, "已连接 \"%s\"，IP %s", s_ssid, s_ip);
}

/* ---------------- 连接 ---------------- */

/* wifi_config_t 里的 ssid[32] / password[64] 是定长字节数组，不是 C 字符串。
 * 用 strncpy(dst, src, sizeof(dst) - 1) 会被 GCC 判成"可能截断"（源串可长达
 * 32/64 字节且不以 '\0' 结尾），工程开着 -Werror，等于直接编不过。
 * 这里按长度拷贝：SSID 允许满 32 字节、密码允许满 64 字节（64 位 hex PSK 是合法长度），
 * 多出来的尾部由前面的 memset 保证为 0。 */
static void fill_cfg(wifi_config_t *wc, const char *ssid, const char *pass)
{
    memset(wc, 0, sizeof(*wc));

    size_t n = strlen(ssid);
    if (n > sizeof(wc->sta.ssid)) {
        n = sizeof(wc->sta.ssid);
    }
    memcpy(wc->sta.ssid, ssid, n);

    n = strlen(pass);
    if (n > sizeof(wc->sta.password)) {
        n = sizeof(wc->sta.password);
    }
    memcpy(wc->sta.password, pass, n);

    /* 阈值放到 OPEN：WPA2/WPA3/开放网络通吃。
     * 设成 WPA2_PSK 会把 WPA3-only 的 AP 直接筛掉，现场排障时很容易被坑。 */
    wc->sta.threshold.authmode = WIFI_AUTH_OPEN;
}

/* 把 s_ssid/s_pass 下发给驱动并发起连接。
 *
 * **只调 esp_wifi_connect() 是不够的。** 这是这块板子上最要命的一条经验：
 * hosted（C6 over SDIO）下，一次连接尝试失败后，驱动侧的连接上下文就失效了，
 * 之后再裸调 connect 无论重试多少次都连不上 —— 现场表现就是
 * "第一次失败 → 一直正在重试 → 必须手动点断开再连一次才成功"。
 * 手动断开之所以有效，正是因为它重新走了 set_config + connect 这一整套。
 * 所以每一次重连都必须重新下发配置。
 *
 * 状态由调用方负责：首次连接走 apply_cfg()（切 CONNECTING、清 s_retry）；
 * 退避重连保持 FAILED，界面继续显示"正在重试…"。
 *
 * 注意这里只做前两步（disconnect + set_config），**connect 由任务延迟
 * CONNECT_DELAY_MS 再发**（do_connect_now），原因见该宏的注释。 */
static esp_err_t push_cfg_and_connect(void)
{
    if (!s_driver_up) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 第一步：把驱动拉回干净状态。
     *
     * 这是"第一次连接必失败、手动点断开再连才成功"的根因修复。
     * C6 侧（hosted）还残留着上一次的关联/重配上下文时，新的 connect 会被
     * 顶掉或直接返回错误；而 esp_wifi_disconnect() 正好清掉这层状态 ——
     * 用户手动点"断开"能连上，效果就来自这一步。
     * 断开本身会发一条 DISCONNECTED（reason 8），用 s_suppress_until 吞掉，
     * 免得它又被当成"连接失败"。 */
    s_suppress_until = esp_timer_get_time() / 1000 + DISC_SUPPRESS_MS;
    esp_err_t derr = esp_wifi_disconnect();
    if (derr != ESP_OK && derr != ESP_ERR_WIFI_NOT_CONNECT &&
        derr != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "预清理 disconnect 返回 %s（继续）", esp_err_to_name(derr));
    }

    wifi_config_t wc;
    fill_cfg(&wc, s_ssid, s_pass);

    /* 密码只打长度，不打内容（串口日志经常被贴出去）。
     * 长度 0 配加密 AP 必失败 —— 一眼就能区分"密码没传进来"和"密码错"。 */
    ESP_LOGI(TAG, "下发配置：ssid=\"%s\"（%u 字节）pass=%u 字节",
             s_ssid, (unsigned)strlen(s_ssid), (unsigned)strlen(s_pass));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config 失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 开一个重配窗口：此后 3 s 内来的 DISCONNECTED 多半是 driver 自己
     * 离开旧链路造成的，不该算连接失败（见 CONN_GRACE_MS）。 */
    s_grace_until = esp_timer_get_time() / 1000 + CONN_GRACE_MS;
    s_soft_retry  = 0;
    s_conn_since  = esp_timer_get_time() / 1000;

    /* connect 不在这里发，交给任务延迟 CONNECT_DELAY_MS 再发。
     * 三连发的后果见 CONNECT_DELAY_MS 的注释：C6 那条 ASSOC_LEAVE 迟到时
     * 会把关联踢掉，而且会被抑制窗口吞掉，界面上只剩"连接超时"。 */
    s_conn_at_ms = s_conn_since + CONNECT_DELAY_MS;
    s_conn_armed = true;
    ESP_LOGI(TAG, "配置已就绪，%d ms 后发 connect", CONNECT_DELAY_MS);
    return ESP_OK;
}

/* 真正发 connect。由任务在 s_conn_armed 到点时调用。 */
static esp_err_t do_connect_now(void)
{
    s_conn_armed = false;
    s_conn_since = esp_timer_get_time() / 1000;

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* 切网时驱动常有一小段时间还在清理上一条连接，connect 会返回
         * ESP_ERR_WIFI_STATE。那是"再等等"，不是"连不上" —— 自己重排一次
         * 延迟重发即可，不动外层的 s_imm_retry / s_imm_try（那是配置下发
         * 失败时的计数，混用会让两边都算不准）。封顶 5 次，免得驱动真卡住
         * 时每秒刷一条日志。 */
        ESP_LOGW(TAG, "esp_wifi_connect 暂不可用：%s", esp_err_to_name(err));
        if (++s_connect_fail < 5) {
            s_conn_at_ms = s_conn_since + CONNECT_DELAY_MS;
            s_conn_armed = true;
        } else {
            s_connect_fail = 0;
            s_fail_reason  = NET_WIFI_ERR_OTHER;
            set_state(NET_WIFI_FAILED);      /* 转入退避重连 */
        }
    } else {
        s_connect_fail = 0;
        /* connect 是同步 RPC：这条出来说明 C6 确实收下了这次连接请求。
         * 成功时原来完全静默，出问题只能靠"后面有没有事件"倒推。 */
        ESP_LOGI(TAG, "已发出 connect（等 AP 回应）");
    }
    return err;
}

/* 首次连接 / 用户主动点选：下发配置并把状态切到"连接中"。 */
static esp_err_t apply_cfg(void)
{
    if (!s_driver_up) {
        return ESP_ERR_INVALID_STATE;
    }

    s_want      = true;
    s_retry     = 0;
    s_retry_stop = false;   /* 新一轮尝试：重新给满 RETRY_MAX_TIMES 次 */
    s_imm_retry = false;
    s_imm_try   = 0;
    s_connect_fail = 0;
    set_state(NET_WIFI_CONNECTING);

    esp_err_t err = push_cfg_and_connect();
    if (err != ESP_OK) {
        s_imm_retry = true;     /* 交给任务下一拍再来（WIFI_STATE 是暂时的） */
        s_imm_try   = 0;
    }
    return err;
}

static void wifi_task(void *arg)
{
    (void)arg;

    /* ---- 1) 起驱动 ----
     *
     * C6 是异步起来的：hosted 在 app_main 之前就复位 CP 并等它就绪
     * （CP_RESET_SETTLE_MS=1500 + CP_BRINGUP_TIMEOUT_MS=5000），但超时策略
     * 配的是 CP_BRINGUP_ON_TIMEOUT_NONE —— 超时后既不重试也不重启。
     * 开机负载重（LVGL 建页面、FAT 挂载、PHY 协商都在同一时间段）时，
     * C6 常常还没应第一包，esp_wifi_init 就返回 ESP_ERR_TIMEOUT。
     * 这是"还没好"，不是"坏了"：重试几次比直接判硬件不可用合理得多。 */
    /* netif 只建一次：任务可能因为 C6 没就绪退出后重建（见 wifi_task），
     * 每次重建都 new 一个 netif 会白丢内存。 */
    if (s_netif == NULL) {
        s_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "创建 STA netif 失败");
        set_state(NET_WIFI_UNAVAILABLE);
        s_task_up = false;      /* 允许开关/扫描重新建任务，否则只能断电 */
        vTaskDelete(NULL);
        return;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < WIFI_BOOT_TRIES; i++) {
        err = esp_wifi_init(&ic);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_wifi_init 第 %d/%d 次失败：%s（C6 hosted slave 还没就绪？）",
                 i + 1, WIFI_BOOT_TRIES, esp_err_to_name(err));
        if (i + 1 < WIFI_BOOT_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_RETRY_MS));
        }
    }
    if (err != ESP_OK) {
        /* 真失败了：C6 没跑 hosted slave 固件（RPC 全程超时）、SDIO 引脚不对，
         * 或者 CP 一直没 bringup 成功。 */
        ESP_LOGE(TAG, "esp_wifi_init 连续 %d 次失败，判硬件不可用（拨一下 WiFi 开关可重来）",
                 WIFI_BOOT_TRIES);
        set_state(NET_WIFI_UNAVAILABLE);
        s_task_up = false;      /* 不清的话 start_task() 永远返回"已启动"，只能断电重启 */
        vTaskDelete(NULL);
        return;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);

    /* 凭据自己管（见文件头第 3 条） */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_have_cfg) {
        wifi_config_t wc;
        fill_cfg(&wc, s_ssid, s_pass);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
    }

    /* start 也是同步 RPC，同样可能撞上 C6 没就绪：
     * 现场 8159 ms 那条 `no response uid=4 msg_id=280 (5000 ms)` 就是 start
     * 打到 C6 没回。按"还没好"重试；ESP_ERR_WIFI_STATE 说明上一次其实已经
     * 生效了（host 超时放弃、C6 后来执行了），当作成功继续往下走。 */
    err = ESP_FAIL;
    for (int i = 0; i < WIFI_BOOT_TRIES; i++) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            break;
        }
        if (err == ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "esp_wifi_start 返回 STATE（上一次其实已生效），按成功处理");
            err = ESP_OK;
            break;
        }
        ESP_LOGW(TAG, "esp_wifi_start 第 %d/%d 次失败：%s",
                 i + 1, WIFI_BOOT_TRIES, esp_err_to_name(err));
        if (i + 1 < WIFI_BOOT_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_RETRY_MS));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 连续 %d 次失败，判硬件不可用（拨一下 WiFi 开关可重来）",
                 WIFI_BOOT_TRIES);
        set_state(NET_WIFI_UNAVAILABLE);
        s_task_up = false;      /* 允许开关/扫描重建任务，否则只能断电 */
        vTaskDelete(NULL);
        return;
    }

    /* STA_START 事件里已经把 s_driver_up 置位、状态切到 CONNECTING/IDLE。
     * 这里主动补一次连接：事件可能先于本行到达，也可能因为 C6 侧时序晚到。
     * 但只要有挂起的扫描（开机预扫 / 用户刚拨开开关），就先扫再连 ——
     * 先连的话状态马上变 CONNECTING，补扫就得等连接结果，连接一卡壳
     * 界面就是"一直扫描中、列表全空"（见 net_wifi_set_enabled 的注释）。 */
    s_want = s_have_cfg;
    if (s_want && !s_scan_wanted) {
        apply_cfg();
    } else {
        set_state(NET_WIFI_IDLE);
    }

    /* ---- 2) 守护循环 ---- */
    int64_t next_rssi = 0;
    int64_t retry_at  = 0;
    int     link_miss = 0;      /* 已连接状态下"探活失败"的连续次数 */
    int64_t last_tick = esp_timer_get_time() / 1000;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time() / 1000;

        /* 本应 1 s 一拍。超时分支、退避重连全靠这个节拍，一旦某个 RPC 卡住
         * （hosted 等 C6 回包），整条守护逻辑就跟着停摆，现场只能看到
         * "状态停在连接中、没有任何超时日志"。有这条就能一眼分清是
         * "逻辑没走到"还是"任务被卡住"。 */
        if (now - last_tick > 3000) {
            ESP_LOGW(TAG, "守护循环被阻塞 %lld ms", (long long)(now - last_tick));
        }
        last_tick = now;

        /* 连上了才把凭据记进"已知网络"（见 net_wifi_connect 的注释）。
         *
         * 放在任务里而不是 on_ip_event 里：这里要写 NVS（nvs_commit 几十毫秒 +
         * 几百字节栈），事件回调跑在 sys_evt 任务上，不适合干这个。
         * 顺手把 cfg 也刷一遍，让"下次开机自动连"和已知网络表同时落地。 */
        if (s_save_ok && s_state == NET_WIFI_CONNECTED) {
            s_save_ok = false;
            saved_put(s_ssid, s_pass);
            cfg_save();
            ESP_LOGI(TAG, "已连上 \"%s\"，加入已知网络", s_ssid);
        }

        /* 到点的 connect（见 CONNECT_DELAY_MS）。放在最前面：后面的扫描、
         * 超时、退避规则都建立在"这次尝试已经真的发出去了"之上。
         * 扫描中不打断（push_cfg 会 disconnect，扫描结果全丢）。 */
        if (s_conn_armed && now >= s_conn_at_ms) {
            if (s_scanning) {
                s_conn_at_ms = now + 200;   /* 等扫完这一拍 */
            } else {
                do_connect_now();
            }
        }

        /* 扫描卡死兜底：没收到 SCAN_DONE 时把 s_scanning 放掉，
         * 否则之后每一次扫描请求都会被"已在扫描"挡掉（见 SCAN_TIMEOUT_MS）。 */
        if (s_scanning && now - s_scan_since > SCAN_TIMEOUT_MS) {
            ESP_LOGW(TAG, "扫描超时 %d ms（没收到 SCAN_DONE），放行重试", SCAN_TIMEOUT_MS);
            s_scanning = false;
        }

        /* 补发挂起的扫描请求（见 s_scan_wanted 的注释）。
         * 放在任务里而不是 STA_START 事件回调里：事件回调只做"改状态"这一件事，
         * 扫描要碰协议栈 + 之后还要收 SCAN_DONE，在事件任务里发起容易和
         * 连接流程互相插队。
         * 连接进行中（CONNECTING）先不扫，否则会把正在进行的那次关联打断；
         * 但只在"开机自动重连"这一种情况下会遇到，用户拨开关走的是
         * "先扫再连"（见 net_wifi_set_enabled）。 */
        if (s_scan_wanted) {
            if (s_driver_up && !s_scanning) {
                if (s_state == NET_WIFI_CONNECTING) {
                    if (!s_scan_defer_logged) {
                        s_scan_defer_logged = true;
                        ESP_LOGI(TAG, "扫描推迟：正在连接（等连接结果，最多 %d s）",
                                 CONN_TIMEOUT_MS / 1000);
                    }
                } else {
                    ESP_LOGI(TAG, "补发挂起的扫描（状态 %s）", net_wifi_state_str());
                    net_wifi_scan_start();
                }
            }
        } else {
            s_scan_defer_logged = false;
        }

        /* 重配/切网引起的断开：事件回调只置标志，这里立刻重连。
         * 不走 2 s→30 s 退避，状态保持 CONNECTING —— 界面因此不会闪
         * "连接失败"，用户看到的是"连接中…"直接过渡到"已连接"。 */
        /* 扫描中不动连接：push_cfg_and_connect() 里会先 disconnect，
         * 那会直接打断正在跑的扫描（结果全丢）。等扫完这一拍再连，
         * 最多晚 1 s，比丢一次扫描结果划算。 */
        if (s_imm_retry && s_driver_up && s_want && !s_scanning) {
            s_imm_retry  = false;
            s_conn_since = now;             /* 重新计时，别被 20 s 超时误杀 */
            if (s_state != NET_WIFI_CONNECTING) {
                set_state(NET_WIFI_CONNECTING);
            }

            /* 同样走完整重下发，理由见 push_cfg_and_connect()：
             * 裸 connect 在失败一次之后就再也连不上了。 */
            esp_err_t e2 = push_cfg_and_connect();
            if (e2 != ESP_OK && ++s_imm_try < 5) {
                /* 驱动还在清理上一条连接：下一拍再来，别傻等 20 s 超时 */
                s_imm_retry = true;
                ESP_LOGW(TAG, "立即重连暂不可用：%s，下一拍再试", esp_err_to_name(e2));
            } else if (e2 != ESP_OK) {
                s_imm_try = 0;
                ESP_LOGE(TAG, "立即重连连续失败：%s，转入退避重连", esp_err_to_name(e2));
                if (s_state != NET_WIFI_FAILED) {
                    /* 连 set_config/connect 都返回错误：不是 AP 那边的问题，
                     * 只能报通用失败，具体拿 esp_err_to_name 去日志里看。 */
                    s_fail_reason = NET_WIFI_ERR_OTHER;
                    set_state(NET_WIFI_FAILED);
                }
            } else {
                s_imm_try = 0;
                ESP_LOGI(TAG, "立即重连（重新下发配置）");
            }
        }

        /* 连接尝试的兜底超时：C6/hosted 偶尔不报 DISCONNECTED，
         * 状态停在 CONNECTING，而重连只在 FAILED 触发（见 CONN_TIMEOUT_MS）。 */
        if (s_state == NET_WIFI_CONNECTING && s_want &&
            now - s_conn_since > CONN_TIMEOUT_MS) {
            ESP_LOGW(TAG, "连接超时 %d ms，判失败并重连", CONN_TIMEOUT_MS);
            s_fail_reason = NET_WIFI_ERR_TIMEOUT;
            set_state(NET_WIFI_FAILED);
            retry_at = now;         /* 下一轮循环立刻走重连分支 */
        }

        /* 扫描期间点了某个网络：等扫描结束再下发配置 + 连接。
         * 用一次性的 s_conn_pending 而不是复用下面的规则：那个规则要求状态
         * 正好是 IDLE，而这里可能停在 FAILED / 刚被 stop 过的 OFF 上。 */
        if (s_conn_pending && !s_scanning && !s_scan_wanted && s_driver_up) {
            s_conn_pending = false;
            s_retry = 0;
            apply_cfg();
        }

        /* 扫描结束后，把"先扫再连"推迟掉的连接补上：
         * 条件是"空闲 + 期望连接 + 有凭据 + 没有在扫/待扫"。
         * 断开/忘记密码时 s_want 或 s_have_cfg 已经为假，不会误连。
         * FAILED 不在这里处理：那种情况要走下面的退避重连，否则会变成
         * 每秒一次的紧密重连风暴。 */
        if (s_want && s_have_cfg && s_state == NET_WIFI_IDLE &&
            !s_scanning && !s_scan_wanted) {
            apply_cfg();
        }

        /* 重连退避：2 s 起步，每次 +2 s，**最多 RETRY_MAX_TIMES 次**。
         *
         * 用完就停在 FAILED 不再自动重连：再试也是同样的结果（密码错、
         * AP 不在、hosted 链路不通），继续只会刷一串日志、界面永远"正在重试"。
         * 用户重新点选网络 / 拨一下开关会清掉 s_retry_stop 再来一轮。
         *
         * 注意这里调的是 push_cfg_and_connect() **而不是裸 esp_wifi_connect()**：
         * 失败一次之后驱动侧的配置上下文就失效了，裸 connect 会一直失败，
         * 用户看到的就是"正在重试…"永远不停、必须手动断开再连一次。
         * 完整重下发配置才是有效的重连。 */
        if (s_want && s_state == NET_WIFI_FAILED && !s_retry_stop &&
            now >= retry_at && !s_scanning) {
            if (s_retry >= RETRY_MAX_TIMES) {
                s_retry_stop = true;
                ESP_LOGE(TAG, "已自动重连 %d 次仍失败（%s），停止重试"
                              "（点选网络或拨开关可再来一轮）",
                         RETRY_MAX_TIMES, fail_reason_tag((net_wifi_err_t)s_fail_reason));
            } else {
                int delay = RETRY_BASE_MS * (s_retry + 1);
                if (delay > RETRY_MAX_MS) {
                    delay = RETRY_MAX_MS;
                }
                s_retry++;
                retry_at = now + delay;
                ESP_LOGI(TAG, "第 %d/%d 次重连（间隔 %d ms）：重新下发配置再连",
                         s_retry, RETRY_MAX_TIMES, delay);
                push_cfg_and_connect();
            }
        }

        /* 已连接时定期刷信号强度：esp_wifi_sta_get_ap_info 是 RPC 调用，
         * 不能每帧都问，5 s 一次足够画图标。
         *
         * 这里同时兼作**链路探活**：hosted（C6）会丢 WIFI_EVENT_STA_DISCONNECTED，
         * 事件一丢，s_state 就永远停在 CONNECTED —— 界面继续显示旧 IP、旧 RSSI，
         * 右上角图标也一直亮着，看起来像"没连上也亮"。只信状态字段是不够的，
         * 必须问一次驱动：拿不到 AP 信息就说明链路真的没了。
         * 单次失败可能只是 RPC 抖动，连续 2 次（约 10 s）才判掉线。 */
        if (s_state == NET_WIFI_CONNECTED && now >= next_rssi) {
            next_rssi = now + RSSI_PERIOD_MS;

            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                link_miss = 0;
                s_rssi = ap.rssi;
                notify();
            } else if (++link_miss >= 2) {
                link_miss = 0;
                ESP_LOGW(TAG, "链路探活连续失败，判掉线（IP %s → 0.0.0.0）", s_ip);
                s_rssi = -127;
                snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
                /* s_want 为真 → 走退避重连；用户主动断开的不重连 */
                if (s_want) {
                    s_fail_reason = NET_WIFI_ERR_LINK_LOST;
                }
                set_state(s_want ? NET_WIFI_FAILED
                                 : (s_enabled ? NET_WIFI_IDLE : NET_WIFI_OFF));
            }
        } else if (s_state != NET_WIFI_CONNECTED) {
            link_miss = 0;      /* 别的状态不累计，免得下次连上就"带着"旧计数 */
        }
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t net_wifi_init(void)
{
    if (s_init) {
        return ESP_OK;
    }
    s_init = true;

    cfg_load();
    s_ssid[NET_WIFI_SSID_MAX - 1] = '\0';
    s_pass[NET_WIFI_PASS_MAX - 1] = '\0';

    if (!s_enabled) {
        s_state = NET_WIFI_OFF;
        ESP_LOGI(TAG, "WiFi 处于关闭状态（设置里打开）");
        return ESP_OK;
    }

    s_state = NET_WIFI_IDLE;
    esp_err_t err = start_task();
    if (err != ESP_OK) {
        s_state = NET_WIFI_UNAVAILABLE;
        return err;
    }

    ESP_LOGI(TAG, "WiFi 任务已启动（%s）", s_have_cfg ? s_ssid : "未配置");

    /* 开机就是"已打开"的情况：顺手排一次扫描，等用户进设置页时列表已经有内容，
     * 不用先盯着"没有搜索到网络"看一秒。 */
    s_scan_wanted = true;
    return ESP_OK;
}

bool net_wifi_enabled(void)
{
    return s_enabled;
}

net_wifi_state_t net_wifi_state(void)
{
    return s_state;
}

const char *net_wifi_ssid(void)
{
    return s_ssid;
}

const char *net_wifi_ip(void)
{
    return s_ip;
}

const char *net_wifi_pass(void)
{
    /* 只给"重新输入密码"的输入框做预填 —— 那里是掩码显示的，
     * 预填只是省掉用户把整串密码再敲一遍（敲错还看不出来）。
     * 不要拿它去打日志。 */
    return s_pass;
}

int8_t net_wifi_rssi(void)
{
    return s_rssi;
}

/* 保证驱动处于 start 状态。三种情况：
 *   任务还没建     → 建任务（任务自己会 start 并连接，这里不等它）
 *   任务在跑但 stop → 补一次 esp_wifi_start()
 *   已经 up        → 直接返回
 * 注意：start_task() 之后驱动是**异步**起来的，调用方不能马上假设 s_driver_up，
 * 所以这个分支返回后由调用方决定"再等下一次操作"。 */
static esp_err_t ensure_driver_up(void)
{
    if (s_driver_up) {
        return ESP_OK;
    }
    if (!s_task_up) {
        return start_task();
    }

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_driver_up = true;
    } else {
        ESP_LOGW(TAG, "重新启动 WiFi 驱动失败：%s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_wifi_set_enabled(bool on)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == s_enabled) {
        return ESP_OK;
    }

    s_enabled = on;
    s_scan_defer_logged = false;
    cfg_save();

    if (!on) {
        s_want = false;
        s_conn_armed = false;
        s_retry      = 0;
        s_retry_stop = false;   /* 再拨开就是新的一轮，预算重新给满 */
        if (s_driver_up) {
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_driver_up = false;
        }
        /* 扫描被 stop 掐掉时不会再有 SCAN_DONE 事件，
         * 不手动清就会永远停在"扫描中…" */
        s_scanning     = false;
        s_scan_wanted  = false;
        s_scan_fail    = 0;
        s_conn_pending = false;
        s_imm_retry    = false;
        s_grace_until  = 0;
        s_ap_n         = 0;         /* 关掉 WiFi 就没有列表可看，别留上次的 */
        s_rssi = -127;
        snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
        set_state(NET_WIFI_OFF);
        ESP_LOGI(TAG, "WiFi 已关闭");
        return ESP_OK;
    }

    /* 每次"打开"都排一次扫描：用户关掉再打开，十次里有九次是想重新挑网络，
     * 上一次的扫描结果早过期了。这里只置挂起标志，真正发起由 wifi 任务在
     * 驱动就绪时做（本函数可能刚 esp_wifi_start()，STA_START 还没到，
     * 此刻直接扫必定失败）。 */
    s_scan_wanted = true;
    s_scan_fail   = 0;
    s_conn_armed  = false;      /* 重新打开按"先扫再连"走，别继承上次排队的 connect */

    bool was_task_up = s_task_up;
    esp_err_t err = ensure_driver_up();
    if (err != ESP_OK) {
        s_enabled     = false;
        s_scan_wanted = false;
        cfg_save();
        return err;
    }

    /* 任务是新建的：它起来后会自己 apply_cfg()，这里不要抢着下发，
     * 否则会和在途的 esp_wifi_init 撞在一起。 */
    if (!was_task_up) {
        return ESP_OK;
    }

    /* 先扫再连，而不是先连再扫。
     *
     * 打开开关的动因十次有九次是"想换个网络"。老写法直接 apply_cfg() 进 CONNECTING，
     * 而任务里的补扫规则是"连接中不扫"—— 连接一旦卡住（C6 版本不匹配时很常见，
     * 收不到 DISCONNECTED 就永远停在 CONNECTING），挂起的扫描永远等不到兑现：
     * 界面一直显示"扫描中…"、列表永远空，直到用户手动再点一次扫描。
     * 这就是"拨开开关只转圈、什么都搜不出来"的现场。
     *
     * 现在置 IDLE 让任务先扫一轮（1~2 s 就有列表），扫完由任务里的
     * "扫后补连"用 s_want 把连接接回去。 */
    s_want = s_have_cfg;
    set_state(NET_WIFI_IDLE);
    return ESP_OK;
}

esp_err_t net_wifi_scan_start(void)
{
    if (!s_driver_up) {
        /* 任务已经退出（开机那次 esp_wifi_init 连续失败，见 wifi_task）：
         * 点一下"扫描"就当一次重试，不必非得去拨开关。 */
        if (!s_task_up && s_init) {
            (void)start_task();
        }
        /* 驱动还在起（C6 走一遍 SDIO 握手要几秒）：记下请求，起来后自动补扫。
         * 这里返回"未就绪"，但调用方（UI）不需要处理 —— net_wifi_scanning()
         * 会把挂起状态也算成"扫描中"，界面直接显示转圈那行字。 */
        s_scan_wanted = true;
        s_scan_defer_logged = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_OK;
    }

    s_scan_wanted = false;
    s_scanning = true;
    s_scan_since = esp_timer_get_time() / 1000;
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        s_scanning = false;
        /* 驱动刚 start、STA_START 还没到：这两类错误是"再等等就好"，
         * 挂起让任务下一拍重试（否则"开关拨开 → 没反应"的老问题会以
         * 另一种形态复现：请求发出去了，但在驱动就绪前就被丢掉）。
         * 其他错误（比如已经在扫描）不重试，免得每秒刷一条日志。 */
        if ((err == ESP_ERR_WIFI_STOP_STATE || err == ESP_ERR_WIFI_NOT_STARTED ||
             err == ESP_ERR_WIFI_STATE)
            && s_scan_fail < 8) {
            s_scan_fail++;
            s_scan_wanted = true;
        }
        ESP_LOGW(TAG, "启动扫描失败：%s", esp_err_to_name(err));
        return err;
    }
    s_scan_fail = 0;
    ESP_LOGI(TAG, "开始扫描");
    return ESP_OK;
}

bool net_wifi_scanning(void)
{
    /* 挂起中的请求也算"扫描中"：否则开关刚打开那两三秒里，
     * 列表会先显示"没有搜索到网络"，比转圈更让人以为坏了。 */
    return s_scanning || s_scan_wanted;
}

int net_wifi_scan_get(net_wifi_ap_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = s_ap_n;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_aps, (size_t)n * sizeof(net_wifi_ap_t));
    return n;
}

int net_wifi_scan_count(void)
{
    return s_ap_n;
}

esp_err_t net_wifi_connect_saved(const char *ssid)
{
    int i = saved_find(ssid);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;   /* 没存过，调用方应弹密码框 */
    }

    /* save=true：把"下次开机自动连这个"更新成它，同时刷新一下表里的顺序
     * 语义（命中已有项是原地更新，不会打乱） */
    return net_wifi_connect(ssid, s_sv_pass[i], true);
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass, bool save)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 已经连着它了（且密码没变）：直接忽略。
     * 再走一遍 set_config + connect，驱动会先断开旧链路再重连 ——
     * 用户看到的就是"点了之后闪一下连接失败，2 s 后又连上"。
     * 这里挡掉，状态、IP、图标都不动。
     * 密码不同（用户重新输了）时不能挡，那是真的想重配。 */
    if (s_state == NET_WIFI_CONNECTED &&
        strncmp(ssid, s_ssid, NET_WIFI_SSID_MAX - 1) == 0 &&
        (pass == NULL || strncmp(pass, s_pass, NET_WIFI_PASS_MAX - 1) == 0)) {
        ESP_LOGI(TAG, "已连接到 \"%s\"，忽略重复连接请求", ssid);
        return ESP_OK;
    }

    /* 正在连它、而且还在超时窗口内：忽略重复点击。
     *
     * hosted 下 set_config / connect 都是同步 RPC（等 C6 回包，最长
     * CONFIG_ESP_HOSTED_HOST_RPC_TIMEOUT_MS=5 s）。一次点击没反应时用户必然
     * 反复点，每次都会再走一遍 disconnect + set_config + connect —— RPC 越积
     * 越多，最后一次点击之后要排很久才轮到自己，界面就一直停在"连接中"。
     * 现场日志的特征：只在第一条 set_config 之前有输出，之后全静默
     * （eh_wifi 的 set_config 日志是发包前打的，不代表 C6 收到了）。 */
    if (s_state == NET_WIFI_CONNECTING &&
        strncmp(ssid, s_ssid, NET_WIFI_SSID_MAX - 1) == 0 &&
        (pass == NULL || strncmp(pass, s_pass, NET_WIFI_PASS_MAX - 1) == 0) &&
        esp_timer_get_time() / 1000 - s_conn_since < CONN_TIMEOUT_MS) {
        ESP_LOGI(TAG, "正在连接 \"%s\"（已 %lld ms），忽略重复请求",
                 ssid, (long long)(esp_timer_get_time() / 1000 - s_conn_since));
        return ESP_OK;
    }

    strncpy(s_ssid, ssid, NET_WIFI_SSID_MAX - 1);
    s_ssid[NET_WIFI_SSID_MAX - 1] = '\0';

    s_pass[0] = '\0';
    if (pass != NULL) {
        strncpy(s_pass, pass, NET_WIFI_PASS_MAX - 1);
        s_pass[NET_WIFI_PASS_MAX - 1] = '\0';
    }
    s_have_cfg = true;

    /* 先不写"已知网络"表：这次还没连上。
     *
     * 旧写法在这里直接 saved_put()，结果是密码错 / AP 不在的连接也会立刻
     * 出现在 Known networks 里 —— 用户看到它像"存好了"，其实从没通过，
     * 而且列表里点它只会又用同一份凭据重试，连重新输密码的入口都没有。
     * 现在改成只挂"待保存"标志，真正落表在拿到 IP 之后（见 wifi_task 里
     * 的 s_save_ok 处理）：连上过的才算已知网络。
     * save=false 是"试一下"的临时连接，成功也不落表。 */
    s_save_ok = save;

    /* 入口打一次就够：这条能直接回答"密码到底有没有从界面传下来"。
     * 之前排查失败只看到 FAILED，分不清是密码空、密码错还是驱动不匹配。 */
    ESP_LOGI(TAG, "连接请求：ssid=\"%s\"（%u 字节）pass=%u 字节，%s",
             s_ssid, (unsigned)strlen(s_ssid), (unsigned)strlen(s_pass),
             save ? "成功后加入已知网络" : "不保存");

    /* 连一个之前关着的 WiFi 就顺手打开开关：用户点了"连接"却什么都没发生
     * 是最难解释的状态 */
    if (!s_enabled) {
        s_enabled = true;
    }

    /* 这里**不写 NVS**：s_ssid/s_pass 只先放在内存里。
     * 落盘挪到"真的连上之后"（见 wifi_task 里的 s_save_ok 处理）——
     * 否则一次输错密码就会把"下次开机自动连"改成那个连不上的网络：
     * 设备重启后先对着它白试 3 轮（每轮 20 s 超时），原来那个能用的
     * 反倒被顶掉了。开关状态由 net_wifi_set_enabled() 自己落盘，不受影响。 */

    bool was_task_up = s_task_up;
    esp_err_t err = ensure_driver_up();
    if (err != ESP_OK) {
        return err;
    }
    if (!was_task_up) {
        return ESP_OK;      /* wifi_task 起来后会自己 apply_cfg() */
    }

    /* 正在扫（或马上就要扫）、而且还没连着：这时 esp_wifi_set_config/connect
     * 会把扫描打断，驱动还可能直接返回 ESP_ERR_WIFI_STATE。凭据已经写进
     * s_ssid/s_pass，这里只把"期望连接"置上，任务里的"扫后补连"会接上。
     * 已经连着的场景走下面的老路（换 AP 时由驱动自己处理切换），不在这里拦。 */
    if ((s_scanning || s_scan_wanted) && s_state != NET_WIFI_CONNECTED) {
        s_want         = true;
        s_conn_pending = true;
        return ESP_OK;
    }

    s_retry = 0;
    return apply_cfg();
}

esp_err_t net_wifi_disconnect(void)
{
    s_want         = false;
    s_conn_pending = false;
    s_imm_retry    = false;     /* 别让挂起的"立即重连"在断开之后又发一次 */
    s_conn_armed   = false;     /* 也别让排队中的 connect 在断开之后再发 */
    s_grace_until  = 0;
    s_retry        = 0;
    s_retry_stop   = false;     /* 手动断开是"重来一轮"的入口之一，预算清零 */
    if (s_driver_up) {
        esp_wifi_disconnect();
    }
    s_rssi = -127;
    snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
    set_state(s_enabled ? NET_WIFI_IDLE : NET_WIFI_OFF);
    return ESP_OK;
}

esp_err_t net_wifi_forget_one(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int i = saved_find(ssid);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 删的正好是"当前这组 / 开机自动连的那组"：先断开再清配置，
     * 否则驱动侧还攥着凭据，忘记完照样能连回去，
     * 而且下次开机还会拿这套凭据去试。 */
    if (strncmp(ssid, s_ssid, NET_WIFI_SSID_MAX - 1) == 0) {
        net_wifi_disconnect();
        s_save_ok  = false;
        s_have_cfg = false;
        s_ssid[0]  = '\0';
        s_pass[0]  = '\0';
    }

    /* 尾部整体前移：表是有序的"新旧"队列，留空洞会让 saved_save 写回时错位 */
    for (int k = i; k + 1 < s_sv_n; k++) {
        memcpy(s_sv_ssid[k], s_sv_ssid[k + 1], NET_WIFI_SSID_MAX);
        memcpy(s_sv_pass[k], s_sv_pass[k + 1], NET_WIFI_PASS_MAX);
    }
    s_sv_n--;
    memset(s_sv_ssid[s_sv_n], 0, NET_WIFI_SSID_MAX);
    memset(s_sv_pass[s_sv_n], 0, NET_WIFI_PASS_MAX);

    s_sv_dirty = true;
    cfg_save();

    ESP_LOGI(TAG, "已忘记网络 \"%s\"（已知网络剩 %d 组）", ssid, s_sv_n);
    return ESP_OK;
}

esp_err_t net_wifi_forget(void)
{
    net_wifi_disconnect();

    s_save_ok  = false;
    s_have_cfg = false;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    /* 整表清空：用户点"忘记"是彻底不想留痕，只删当前一条会留下别的网络 */
    for (int i = 0; i < NET_WIFI_SAVED_MAX; i++) {
        memset(s_sv_ssid[i], 0, NET_WIFI_SSID_MAX);
        memset(s_sv_pass[i], 0, NET_WIFI_PASS_MAX);
    }
    s_sv_n     = 0;
    s_sv_dirty = true;

    cfg_save();

    ESP_LOGI(TAG, "已清除保存的 WiFi 凭据");
    return ESP_OK;
}

bool net_wifi_is_saved(const char *ssid)
{
    return saved_find(ssid) >= 0;
}

int net_wifi_saved_count(void)
{
    return s_sv_n;
}

const char *net_wifi_saved_ssid(int idx)
{
    if (idx < 0 || idx >= s_sv_n) {
        return NULL;
    }
    return s_sv_ssid[idx];
}

int32_t net_wifi_fail_age_s(void)
{
    if (s_state != NET_WIFI_FAILED || s_fail_at_s == 0) {
        return -1;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    return (int32_t)(now - s_fail_at_s);
}

net_wifi_err_t net_wifi_fail_reason(void)
{
    if (s_state != NET_WIFI_FAILED) {
        return NET_WIFI_ERR_NONE;
    }
    return (net_wifi_err_t)s_fail_reason;
}

bool net_wifi_retry_pending(void)
{
    /* FAILED 且预算没用完 —— 任务的退避分支还会再试。
     * 预算用尽后 s_retry_stop 为真，界面必须换文案，
     * 否则"正在重试…"会永远挂着（这正是用户抱怨的那种状态）。 */
    return s_want && !s_retry_stop && s_state == NET_WIFI_FAILED;
}

void net_wifi_set_cb(net_wifi_cb_t cb)
{
    s_cb = cb;
}
