/**
 * @file  ota_shell.c
 * @brief 串口命令行：help / dev / status / ota <n> [KB]
 *
 * 实现说明：
 * - 为什么不用 esp_console：那要多挂 console + linenoise 两个组件，
 *   而这里只需要 4 条命令。自己按行解析 30 行代码，MINIMAL_BUILD 也不会被拖大。
 * - 回显由本任务做（UART 驱动不替我们回显），否则串口里看不到自己敲的字。
 * - 升级是异步的：`ota 0` 立刻返回，用 `status` 看进度。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

/* ESP32-P4 板上有两个 USB 口：
 *   - CH343/CP210x 转串口 → UART0(GPIO37/38)，对应 sdkconfig 的主控制台；
 *   - 芯片自带 USB-Serial-JTAG（二级控制台）。
 * 两个口都能看到日志，但日志从哪个口出来 ≠ 命令从哪个口进来。
 * 只读 UART0 时，若线插在 USB-JTAG 口就会"日志一直在刷、敲键盘没反应"。
 * 这里两个通道都收，插哪个口都能用。 */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) || \
    defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#define SHELL_HAS_USJ 1
#else
#define SHELL_HAS_USJ 0
#endif

#include "esp_log.h"

#include "app_config.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "dev_model.h"
#include "dev_table.h"
#include "fw_pkg.h"
#include "fw_store.h"
#include "net_wifi.h"
#include "ota_client.h"
#include "ota_image.h"
#include "ota_proto.h"
#include "sys_clock.h"
#include "ota_shell.h"

static const char *TAG = "shell";

#define SHELL_UART      UART_NUM_0
#define SHELL_LINE_MAX  80

/* 固件库快照缓冲。放静态而不是栈上：单条约 144 B，24 条就是 3.4 KB，
 * 而 shell 任务栈只有 APP_SHELL_TASK_STACK；命令行是单线程的，共用一份安全。 */
static fw_item_t s_fw_items[FW_STORE_MAX];

static void print_help(void)
{
    printf("\r\n"
           "可用命令（回车执行）：\r\n"
           "  help            本帮助\r\n"
           "  dev             打印设备表（编号与日志里的 [n] 一致）\r\n"
           "  dev raw         同上，并打印 model 原串的十六进制（采集真机型号用）\r\n"
           "  status          上一次/当前升级会话的状态\r\n"
           "  ota <n> [KB]    对设备表第 n 台发起升级，合成镜像，默认 %d KB\r\n"
           "                  KB 范围 16~2048\r\n"
           "  ota <n> --file <k>\r\n"
           "                  对第 n 台推固件库第 k 项（真实 .fwp 文件）\r\n"
           "  fw ls           列出固件库（TF + 内部仓库，列之前先探一次 TF 卡）\r\n"
           "  fw info <n>     包头全字段 + SHA-256\r\n"
           "  fw import <n>   把 TF 卡上的第 n 项复制进内部仓库\r\n"
           "  fw del <n>      删除内部仓库条目（不删 TF 卡）\r\n"
           "  fw best <model> 查该型号最高版本\r\n"
           "  model test <s>  看型号匹配结果，例：model test \"ventrix cpu\"\r\n"
           "  touch           查看触摸定标参数\r\n"
           "  touch dump      开/关原始坐标打印（定标时用）\r\n"
           "  touch cal x0 x1 y0 y1\r\n"
           "                  两点定标：raw 值分别对应屏幕 左/右/上/下 边缘\r\n"
           "  touch reset     恢复默认量程\r\n"
           "  time            查看当前时间（无 RTC，初值=编译时间）\r\n"
           "  time set YYYY-MM-DD HH:MM[:SS]\r\n"
           "                  手工校正系统时间（北京时间）\r\n"
           "  disp            显示统计：VSYNC / 整帧计数与帧率\r\n"
           "  disp solid [rrggbb]\r\n"
           "                  冻住 UI 并填纯色，判断静止画面还闪不闪\r\n"
           "  disp ui         解冻，恢复界面\r\n"
           "  disp timing [hbp hpw hfp vbp vpw vfp [MHz]]\r\n"
           "                  看/改 DPI 时序（黑边/偏移），改完重启生效\r\n"
           "  disp timing hbp 96 | vbp 13\r\n"
           "                  只改一个轴，配对端自动补偿（扫黑边用这个）\r\n"
           "  disp timing reset\r\n"
           "                  回到编译期默认时序\r\n"
           "  bl dc on|off    背光脱离 PWM 恒亮 / 恢复调光\r\n"
           "  sd              TF 卡状态与容量（顺带探测一次，插卡后敲它即可识别）\r\n"
           "  wifi            WiFi 状态（开关 / 状态 / SSID / IP / 信号）\r\n"
           "  wifi on|off     开 / 关 WiFi\r\n"
           "  wifi scan       扫描并列出周边网络\r\n"
           "  wifi connect <ssid> [-p 密码]\r\n"
           "                  连接；SSID 可含空格（-p 之前的都算 SSID）\r\n"
           "  wifi disconnect 断开（保留凭据）\r\n"
           "  wifi forget [ssid] 清除保存的凭据（带 SSID 只删这一条）\r\n"
           "  lcd w <cmd> [b...]\r\n"
           "                  直接写面板寄存器（十六进制），如 lcd w C0 12\r\n"
           "\r\n", APP_OTA_SYNTH_SIZE_KB);
}

static void print_status(void)
{
    ota_status_t st;
    ota_client_get_status(&st);

    if (st.state == OTA_STATE_IDLE) {
        printf("\r\n[升级] 还没有会话\r\n\r\n");
        return;
    }

    unsigned pct = st.total ? (unsigned)(((uint64_t)st.acked * 100u) / st.total) : 0;
    printf("\r\n[升级] %s  %s\r\n", ota_state_str(st.state), st.msg);
    printf("  设备 %s  型号 '%s'\r\n", st.dev, st.model);
    printf("  进度 %u%%  %u/%u B  用时 %u ms\r\n",
           pct, (unsigned)st.acked, (unsigned)st.total, (unsigned)st.elapsed_ms);
    if (st.err_code != OTA_ERR_OK) {
        printf("  错误 0x%02x %s\r\n", st.err_code, ota_err_str(st.err_code));
    }
    printf("\r\n");
}

/* raw=true 时额外打印 model 的原串十六进制 —— 用来采集三型真机到底报什么，
 * 采到之后补 dev_model.c 的别名表即可，不用改协议。 */
static void print_devices(bool raw)
{
    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);

    if (n == 0) {
        printf("\r\n[设备] 表为空（PC/设备接入后自动出现）\r\n\r\n");
        return;
    }

    printf("\r\n[设备] 共 %d 台\r\n", n);
    for (int i = 0; i < n; i++) {
        const dev_entry_t *e = &snap[i];
        printf("  [%d] %u.%u.%u.%u  %s  %-10s %s",
               i,
               (unsigned)((e->ip >> 0) & 0xFF), (unsigned)((e->ip >> 8) & 0xFF),
               (unsigned)((e->ip >> 16) & 0xFF), (unsigned)((e->ip >> 24) & 0xFF),
               dev_mac_str(e->mac),
               e->hostname[0] ? e->hostname : "-",
               e->online ? "ONLINE " : "OFFLINE");
        if (e->identified) {
            printf("  model='%s' hw=%u.%u fw=%u.%u.%u.%u",
                   e->id.model,
                   (unsigned)(e->id.hw_ver >> 8), (unsigned)(e->id.hw_ver & 0xFF),
                   e->id.fw_ver[0], e->id.fw_ver[1], e->id.fw_ver[2], e->id.fw_ver[3]);
            if (e->model_id != FW_MODEL_UNKNOWN) {
                printf("  [%s]", dev_model_canon(e->model_id));
            }
            printf("\r\n");
            if (raw) {
                printf("        model hex:");
                for (int k = 0; k < DEV_MODEL_LEN; k++) {
                    printf(" %02x", (unsigned char)e->id.model[k]);
                }
                printf("\r\n");
            }
        } else {
            printf("  身份未知\r\n");
        }
    }
    printf("\r\n");
}

/* 目标版本 = 当前版本 build + 1，保证"比设备新"，避免设备回 E_VERSION */
static uint32_t next_version(const uint8_t cur[4])
{
    uint32_t build = (uint32_t)cur[3];
    uint32_t patch = cur[2];

    build = (build + 1) & 0xFFu;
    if (build == 0) {
        patch = (patch + 1) & 0xFFu;    /* build 溢出就进 patch */
    }
    return OTA_FW_VER(cur[0], cur[1], patch, build);
}

static void cmd_ota(int argc, char **argv)
{
    if (argc < 1) {
        printf("\r\n用法：ota <n> [KB] [--file <k>]\r\n"
               "  ota 0 256         对第 0 台推 256 KB 合成镜像（回归用）\r\n"
               "  ota 0 --file 1    对第 0 台推固件库第 1 项（真实文件）\r\n\r\n");
        return;
    }

    int idx      = atoi(argv[0]);
    int kb       = APP_OTA_SYNTH_SIZE_KB;
    int file_idx = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                printf("\r\n--file 后面要跟固件库序号（fw ls 查）\r\n\r\n");
                return;
            }
            file_idx = atoi(argv[i + 1]);
            i++;
        } else {
            kb = atoi(argv[i]);
        }
    }

    if (file_idx < 0 && (kb < 16 || kb > 2048)) {
        printf("\r\nKB 必须在 16~2048 之间\r\n\r\n");
        return;
    }

    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);
    if (idx < 0 || idx >= n) {
        printf("\r\n没有第 %d 台（当前 %d 台，先 dev 看一下）\r\n\r\n", idx, n);
        return;
    }

    dev_entry_t *e = &snap[idx];
    if (!e->identified) {
        printf("\r\n第 %d 台身份未知（%s 不应答发现协议），无法匹配型号\r\n\r\n",
               idx, dev_mac_str(e->mac));
        return;
    }
    if (e->ip == 0) {
        printf("\r\n第 %d 台没有 IP\r\n\r\n", idx);
        return;
    }
    if (ota_client_busy()) {
        printf("\r\n已有会话在跑，先等它结束（status 查看）\r\n\r\n");
        return;
    }

    ota_image_t img;
    esp_err_t err;

    if (file_idx >= 0) {
        fw_store_scan();                    /* 现场可能刚拷进卡里，先刷一次 */
        fw_item_t *items = s_fw_items;
        int cnt = fw_store_snapshot(items, FW_STORE_MAX);
        if (file_idx >= cnt) {
            printf("\r\n没有第 %d 项固件（当前 %d 项，先 fw ls）\r\n\r\n", file_idx, cnt);
            return;
        }

        const fw_item_t *it = &items[file_idx];

        /* 包与设备不匹配时宁可现在拒，也不要让设备回一句 E_MODEL 让人去猜 */
        if (it->model_id != FW_MODEL_UNKNOWN && e->model_id != FW_MODEL_UNKNOWN &&
            it->model_id != e->model_id) {
            printf("\r\n型号不符：固件是 '%s'，设备是 '%s'。拒绝下发\r\n\r\n",
                   dev_model_canon(it->model_id), dev_model_canon(e->model_id));
            return;
        }
        if (e->id.hw_ver < it->hw_min || e->id.hw_ver > it->hw_max) {
            printf("\r\n硬件版本不符：设备 hw=%u.%u，固件要求 %u.%u~%u.%u\r\n\r\n",
                   (unsigned)(e->id.hw_ver >> 8), (unsigned)(e->id.hw_ver & 0xFF),
                   (unsigned)(it->hw_min >> 8), (unsigned)(it->hw_min & 0xFF),
                   (unsigned)(it->hw_max >> 8), (unsigned)(it->hw_max & 0xFF));
            return;
        }
        if (it->fw_ver <= OTA_FW_VER(e->id.fw_ver[0], e->id.fw_ver[1],
                                     e->id.fw_ver[2], e->id.fw_ver[3])) {
            printf("\r\n提示：固件版本 %s 不高于设备当前版本，设备可能回 E_VERSION\r\n",
                   ota_fw_ver_str(it->fw_ver));
        }

        err = fw_store_open(it, &img);
        if (err != ESP_OK) {
            printf("\r\n打开固件失败：%s\r\n\r\n", esp_err_to_name(err));
            return;
        }
    } else {
        err = ota_image_make_synthetic(&img,
                                       e->id.model,
                                       e->id.hw_ver, e->id.hw_ver,   /* 只接受当前硬件版本 */
                                       next_version(e->id.fw_ver),
                                       (uint32_t)kb * 1024u,
                                       APP_OTA_SYNTH_SEED);
        if (err != ESP_OK) {
            printf("\r\n生成镜像失败：%s\r\n\r\n", esp_err_to_name(err));
            return;
        }
    }

    err = ota_client_start(e->ip, e->id.uid, e->mac, &img);
    if (err != ESP_OK) {
        if (img.release != NULL) {
            img.release(&img);              /* 会话没起来，句柄得自己关 */
        }
        printf("\r\n发起升级失败：%s\r\n\r\n", esp_err_to_name(err));
        return;
    }
    printf("\r\n已发起：设备 [%d] %s -> fw=%s，%u KB。用 status 看进度\r\n\r\n",
           idx, e->id.model, ota_fw_ver_str(img.fw_ver), (unsigned)(img.size / 1024u));
}

/* 触摸定标：把 GT911 的原始坐标量程映射到 1024x600。
 * 现场用法：touch dump 打开打印 -> 点四个角记 raw -> touch cal x0 x1 y0 y1。 */
static void cmd_touch(int argc, char **argv)
{
    if (!bsp_display_ready()) {
        printf("\r\n显示/触摸未初始化，无法定标\r\n\r\n");
        return;
    }

    if (argc >= 1 && strcmp(argv[0], "dump") == 0) {
        bool on = !bsp_display_touch_dump_state();
        bsp_display_touch_dump(on);
        printf("\r\n原始坐标打印已%s：点屏幕，看日志里的 raw=(x,y)\r\n\r\n",
               on ? "打开" : "关闭");
        return;
    }

    if (argc >= 1 && strcmp(argv[0], "reset") == 0) {
        bsp_display_touch_cal_reset();
        printf("\r\n已恢复默认量程（0~%d / 0~%d）\r\n\r\n", BSP_TOUCH_RAW_X, BSP_TOUCH_RAW_Y);
        return;
    }

    if (argc >= 1 && strcmp(argv[0], "cal") == 0) {
        if (argc < 5) {
            printf("\r\n用法：touch cal <x0> <x1> <y0> <y1>\r\n\r\n");
            return;
        }
        int x0 = atoi(argv[1]);
        int x1 = atoi(argv[2]);
        int y0 = atoi(argv[3]);
        int y1 = atoi(argv[4]);

        if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || x1 > 65535 || y1 > 65535) {
            printf("\r\n参数不合法：需 0 <= x0 < x1、0 <= y0 < y1\r\n\r\n");
            return;
        }

        if (bsp_display_touch_cal_set((uint16_t)x0, (uint16_t)x1,
                                      (uint16_t)y0, (uint16_t)y1) == ESP_OK) {
            printf("\r\n已应用：raw_x %d~%d、raw_y %d~%d -> 0~%d / 0~%d。点几个位置验证一下\r\n\r\n",
                   x0, x1, y0, y1, BSP_LCD_H_RES - 1, BSP_LCD_V_RES - 1);
        } else {
            printf("\r\n应用失败，看日志\r\n\r\n");
        }
        return;
    }

    uint16_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bsp_display_touch_cal_get(&x0, &x1, &y0, &y1);

    printf("\r\n[触摸] raw_x %u~%u -> 0~%d，raw_y %u~%u -> 0~%d\r\n",
           x0, x1, BSP_LCD_H_RES - 1, y0, y1, BSP_LCD_V_RES - 1);
    printf("  原始坐标打印：%s\r\n", bsp_display_touch_dump_state() ? "开" : "关");
    printf("  定标：touch dump 打开打印 -> 点四角记 raw -> touch cal x0 x1 y0 y1\r\n\r\n");
}

static void cmd_time(int argc, char **argv)
{
    char buf[40];

    if (argc == 0) {
        sys_clock_full_str(buf, sizeof(buf));
        printf("\r\n[时间] %s（CST-8，无 RTC，重启会回到编译时间）\r\n", buf);
        printf("  校正：time set 2026-11-05 16:30\r\n\r\n");
        return;
    }

    if (strcmp(argv[0], "set") != 0 || argc < 3) {
        printf("\r\n用法：time              查看当前时间\r\n"
               "      time set 2026-11-05 16:30[:00]\r\n\r\n");
        return;
    }

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(argv[1], "%d-%d-%d", &y, &mo, &d) != 3) {
        printf("\r\n日期格式应为 YYYY-MM-DD\r\n\r\n");
        return;
    }
    int n = sscanf(argv[2], "%d:%d:%d", &h, &mi, &se);
    if (n < 2) {
        printf("\r\n时间格式应为 HH:MM 或 HH:MM:SS\r\n\r\n");
        return;
    }

    if (!sys_clock_set(y, mo, d, h, mi, se)) {
        printf("\r\n设置失败：参数超出范围（年份 2020~2099）\r\n\r\n");
        return;
    }

    sys_clock_full_str(buf, sizeof(buf));
    printf("\r\n[时间] 已设置：%s\r\n\r\n", buf);
}

/* ---------------- TF 卡 ---------------- */

/* sd          当前状态（挂载点 / 容量），顺带探测一次
 * 没有后台轮询了，敲 sd 就是"现在去探一下卡"
 */
static void cmd_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 探测是这里的主动作：没插卡时这条路要跑完 SDMMC 初始化（几百毫秒），
     * 串口命令可以接受。插卡后敲一条就能看到容量，不用重启。 */
    esp_err_t err = bsp_sdcard_mount_now();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        printf("\r\n[TF] 挂载失败：%s\r\n"
               "     常见原因：没插卡 / 不是 FAT32 / 卡座接触不良\r\n\r\n",
               esp_err_to_name(err));
        return;
    }

    if (!bsp_sdcard_mounted()) {
        printf("\r\n[TF] 未挂载（没插卡）\r\n\r\n");
        return;
    }

    uint64_t tk = 0;
    uint64_t fk = 0;
    if (!bsp_sdcard_usage(&tk, &fk)) {
        printf("\r\n[TF] 已挂载 %s，容量还没读出来（等下一次刷新）\r\n\r\n",
               BSP_SD_MOUNT_POINT);
        return;
    }

    const uint64_t used = tk > fk ? tk - fk : 0;
    printf("\r\n[TF] %s  已用 %u.%u / %u.%u GB，剩余 %u MB\r\n\r\n",
           BSP_SD_MOUNT_POINT,
           (unsigned)(used / 1048576u), (unsigned)((used % 1048576u) * 10u / 1048576u),
           (unsigned)(tk / 1048576u), (unsigned)((tk % 1048576u) * 10u / 1048576u),
           (unsigned)(fk / 1024u));
}

/* ---------------- 显示诊断（闪屏定位用） ---------------- */

/* disp                     打印 VSYNC / 整帧计数
 * disp solid [rrggbb]      冻住 UI 并填纯色（RGB565），用于"静止画面还闪吗"
 * disp ui                  解冻恢复界面
 * disp timing [6~7 个数]   看/改 DPI 时序（黑边就是这套参数不对），改完重启生效
 * disp timing reset        回到编译期默认值
 */
static void cmd_disp(int argc, char **argv)
{
    if (argc == 0 || strcmp(argv[0], "stat") == 0) {
        bsp_display_stats();
        printf("\r\n  判据：vsync 和 frame 都该接近 60 Hz。只有 vsync 没有 frame\r\n"
               "        = DMA 卡住；两个都远低于 60 = 面板低频或时钟没配好。\r\n\r\n");
        return;
    }

    /* 时序调节：左边黑边 = hbp 偏大，底部黑边 = vbp 偏大。
     * DPI 配置只在面板创建时读一次，所以改完必须重启。 */
    if (strcmp(argv[0], "timing") == 0) {
        if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
            esp_err_t err = bsp_display_timing_reset();
            printf("\r\n[时序] %s（重启生效）\r\n\r\n",
                   (err == ESP_OK) ? "已擦除 NVS，回到默认值" : esp_err_to_name(err));
            return;
        }

        int t[7] = {0};
        bsp_display_timing_get(t);

        static const char *const TN[7] = {"hbp", "hpw", "hfp", "vbp", "vpw", "vfp", "MHz"};
        bool changed = false;

        /* 简写：disp timing hbp 96 —— 只调一个值，配对的那一端自动补上同样的量
         * （hbp↔hfp、vbp↔vfp），行/场总长不变，帧率才不动。
         * 扫黑边时一次只改一个轴，才看得出是哪一边的问题。 */
        if (argc >= 3 && (strcmp(argv[1], "hbp") == 0 || strcmp(argv[1], "hfp") == 0 ||
                          strcmp(argv[1], "vbp") == 0 || strcmp(argv[1], "vfp") == 0)) {
            int idx, pair;
            if (strcmp(argv[1], "hbp") == 0)      { idx = 0; pair = 2; }
            else if (strcmp(argv[1], "hfp") == 0) { idx = 2; pair = 0; }
            else if (strcmp(argv[1], "vbp") == 0) { idx = 3; pair = 5; }
            else                                 { idx = 5; pair = 3; }

            const int v = atoi(argv[2]);
            const int d = v - t[idx];
            if (v < 0 || t[pair] - d < 0) {
                printf("\r\n[时序] %s=%d 会把 %s 顶成负数，拒绝\r\n\r\n",
                       TN[idx], v, TN[pair]);
                return;
            }
            t[idx]  = v;
            t[pair] -= d;
            changed = true;
        } else if (argc >= 7) {               /* hbp hpw hfp vbp vpw vfp [mhz] */
            for (int i = 0; i < 6; i++) {
                t[i] = atoi(argv[i + 1]);
            }
            if (argc >= 8) {
                t[6] = atoi(argv[7]);
            }
            changed = true;
        }

        if (changed) {
            esp_err_t err = bsp_display_timing_set(t);
            if (err != ESP_OK) {
                printf("\r\n[时序] 参数不合法（%s），没有写入\r\n\r\n", esp_err_to_name(err));
                return;
            }
            printf("\r\n[时序] 已写入 NVS，重启生效\r\n");
        }

        const int ht = BSP_LCD_H_RES + t[0] + t[1] + t[2];
        const int vt = BSP_LCD_V_RES + t[3] + t[4] + t[5];
        const uint32_t fps10 = (uint32_t)(((uint64_t)t[6] * 10000000ULL) / ((uint64_t)ht * vt));
        printf("  H %d/%d/%d（总 %d）  V %d/%d/%d（总 %d）  %d MHz  ->  %u.%u Hz\r\n",
               t[0], t[1], t[2], ht, t[3], t[4], t[5], vt, t[6],
               (unsigned)(fps10 / 10), (unsigned)(fps10 % 10));
        printf("  扫左边黑边：disp timing hbp 96     扫底部黑边：disp timing vbp 13\r\n"
               "  一次只改一个轴、步长 hbp 8 / vbp 4；黑边不再变窄就是到物理边框了，别再减。\r\n"
               "  配对端会自动补偿，总长与帧率不变。左黑边→hbp 减，底黑边→vbp 减；\r\n"
               "  若黑边换到右边/顶部（内容被裁），说明过头了，往回加一半。\r\n"
               "  完整写法：disp timing <hbp> <hpw> <hfp> <vbp> <vpw> <vfp> [MHz]；\r\n"
               "  disp timing reset 回到默认（屏不亮时先 reset 再重启）\r\n\r\n");
        return;
    }

    if (strcmp(argv[0], "solid") == 0) {
        uint16_t color = 0x8410;                           /* 默认中灰 */
        if (argc >= 2) {
            color = (uint16_t)strtoul(argv[1], NULL, 16);
        }
        if (bsp_display_ui_freeze(true) != ESP_OK) {
            printf("\r\n冻结 UI 失败\r\n\r\n");
            return;
        }
        if (bsp_display_fill(color) != ESP_OK) {
            printf("\r\n填色失败\r\n\r\n");
            bsp_display_ui_freeze(false);
            return;
        }
        printf("\r\n[显示] 已填 0x%04X 并冻结 UI。现在屏幕上不会有任何重写，\r\n"
               "       盯着看 30 秒：还闪 -> 背光/面板/DSI，与 LVGL 无关；\r\n"
               "       不闪 -> 是刷新引起的撕裂。 disp ui 恢复\r\n\r\n", color);
        return;
    }

    if (strcmp(argv[0], "ui") == 0) {
        bsp_display_ui_freeze(false);
        printf("\r\n[显示] 界面已恢复\r\n\r\n");
        return;
    }

    printf("\r\n用法：disp [stat] | disp solid [rrggbb] | disp ui\r\n"
           "      disp timing [hbp hpw hfp vbp vpw vfp [MHz]] | disp timing reset\r\n\r\n");
}

/* bl dc on|off   背光脱离 PWM 恒亮 / 恢复调光，用来判断闪是不是 PWM 引起的 */
static void cmd_bl(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[0], "dc") == 0) {
        bool on = (strcmp(argv[1], "on") == 0);
        if (bsp_display_backlight_dc(on) != ESP_OK) {
            printf("\r\n切换失败\r\n\r\n");
            return;
        }
        printf("\r\n[背光] %s。若此时闪消失，问题在 20 kHz PWM 与行扫描的拍频。\r\n\r\n",
               on ? "已切成恒亮（无 PWM）" : "已恢复 PWM 调光");
        return;
    }

    printf("\r\n用法：bl dc on | bl dc off\r\n\r\n");
}

/* lcd w <cmd> [b0 b1 ...]   直接写面板寄存器（调 VCOM / 帧率，无需重编译） */
static void cmd_lcd(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[0], "w") != 0) {
        printf("\r\n用法：lcd w <cmd> [b0 b1 ...]   例：lcd w C0 12 34\r\n"
               "      参数是十六进制，可带 0x 前缀\r\n\r\n");
        return;
    }

    uint8_t cmd = (uint8_t)strtoul(argv[1], NULL, 16);
    uint8_t params[8];
    int n = 0;
    for (int i = 2; i < argc && n < (int)sizeof(params); i++) {
        params[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
    }

    if (bsp_display_lcd_write(cmd, params, (size_t)n) != ESP_OK) {
        printf("\r\n写入失败\r\n\r\n");
        return;
    }

    printf("\r\n[面板] 已写 cmd=0x%02X", cmd);
    for (int i = 0; i < n; i++) {
        printf(" %02X", params[i]);
    }
    printf("\r\n\r\n");
}

/* ---------------- 固件仓库命令 ---------------- */

/* 把 argv[from..] 用空格拼成一个串（型号名带空格，没法靠 strtok 保留） */
static void join_args(int argc, char **argv, int from, char *out, size_t cap)
{
    size_t len = 0;

    out[0] = '\0';
    for (int i = from; i < argc && len + 1 < cap; i++) {
        if (len != 0) {
            out[len++] = ' ';
        }
        int m = snprintf(out + len, cap - len, "%s", argv[i]);
        if (m < 0 || (size_t)m >= cap - len) {
            break;
        }
        len += (size_t)m;
    }
}

static void fw_print_list(void)
{
    fw_store_scan();

    fw_item_t *items = s_fw_items;
    int n = fw_store_snapshot(items, FW_STORE_MAX);

    if (n == 0) {
        printf("\r\n[固件] 0 项\r\n"
               "  把 .fwp 放到 TF 卡的 %s 下（或卡根目录），插卡后自动出现\r\n\r\n",
               FW_STORE_SD_DIR);
        return;
    }

    printf("\r\n[固件] 共 %d 项\r\n", n);
    for (int i = 0; i < n; i++) {
        const fw_item_t *it = &items[i];
        char mname[40];
        printf("  [%d] %-5s  %-26s fw=%-9s %5u KB %s\r\n",
               i, fw_src_str(it->src),
               dev_model_display(it->raw_model, mname, sizeof(mname)),
               ota_fw_ver_str(it->fw_ver),
               (unsigned)(it->size / 1024u),
               (it->model_id == FW_MODEL_UNKNOWN) ? "(!未在白名单)" : "");
    }

    uint64_t free_b = 0;
    if (fw_store_space(FW_SRC_FLASH, &free_b)) {
        printf("  内部仓库剩余 %u KB", (unsigned)(free_b / 1024u));
    } else {
        printf("  内部仓库未挂载");
    }
    if (fw_store_space(FW_SRC_SD, &free_b)) {
        printf("；TF 剩余 %u KB\r\n\r\n", (unsigned)(free_b / 1024u));
    } else {
        printf("；TF 未挂载\r\n\r\n");
    }
}

static void fw_print_info(int idx)
{
    fw_store_scan();

    fw_item_t *items = s_fw_items;
    int n = fw_store_snapshot(items, FW_STORE_MAX);
    if (idx < 0 || idx >= n) {
        printf("\r\n没有第 %d 项（当前 %d 项）\r\n\r\n", idx, n);
        return;
    }

    const fw_item_t *it = &items[idx];
    FILE *f = fopen(it->path, "rb");
    if (f == NULL) {
        printf("\r\n打不开 %s\r\n\r\n", it->path);
        return;
    }
    fw_pkg_hdr_t h;
    esp_err_t err = fw_pkg_parse(f, &h);
    fclose(f);
    if (err != ESP_OK) {
        printf("\r\n包头解析失败：%s\r\n\r\n", esp_err_to_name(err));
        return;
    }

    char mname[40];
    printf("\r\n[固件 %d] %s\r\n", idx, it->path);
    printf("  来源   %s\r\n", fw_src_str(it->src));
    printf("  型号   '%s'  →  %s\r\n", h.model,
           dev_model_display(h.model, mname, sizeof(mname)));
    printf("  硬件   %u.%u ~ %u.%u\r\n",
           (unsigned)(h.hw_min >> 8), (unsigned)(h.hw_min & 0xFF),
           (unsigned)(h.hw_max >> 8), (unsigned)(h.hw_max & 0xFF));
    printf("  版本   %s\r\n", ota_fw_ver_str(h.fw_ver));
    printf("  负载   %u 字节（%u KB），CRC32=0x%08x\r\n",
           (unsigned)h.payload_len, (unsigned)(h.payload_len / 1024u),
           (unsigned)h.payload_crc32);
    if (h.build_time[0] != '\0') {
        printf("  构建   %s\r\n", h.build_time);
    }
    printf("  SHA256 ");
    for (int i = 0; i < 32; i++) {
        printf("%02x", h.sha256[i]);
    }
    printf("\r\n\r\n");
}

static void cmd_fw(int argc, char **argv)
{
    const char *sub = (argc >= 1) ? argv[0] : "ls";

    if (strcmp(sub, "ls") == 0) {
        /* 不做后台轮询后，"列固件库"就是最自然的探测时机：先同步探一次卡
         * （插卡 → 挂上，拔卡 → 卸载），再重扫，列表才是当前真实内容。 */
        (void)bsp_sdcard_mount_now();
        fw_store_scan();
        fw_print_list();
        return;
    }

    if (strcmp(sub, "info") == 0) {
        if (argc < 2) {
            printf("\r\n用法：fw info <n>\r\n\r\n");
            return;
        }
        fw_print_info(atoi(argv[1]));
        return;
    }

    if (strcmp(sub, "import") == 0) {
        if (argc < 2) {
            printf("\r\n用法：fw import <n>   把 TF 卡上的第 n 项复制进内部仓库\r\n\r\n");
            return;
        }
        char msg[64];
        esp_err_t err = fw_store_import(atoi(argv[1]), msg, sizeof(msg));
        printf("\r\n[导入] %s%s%s\r\n\r\n", msg,
               (err == ESP_OK) ? "" : "：", (err == ESP_OK) ? "" : esp_err_to_name(err));
        return;
    }

    if (strcmp(sub, "del") == 0) {
        if (argc < 2) {
            printf("\r\n用法：fw del <n>   只允许删内部仓库的条目\r\n\r\n");
            return;
        }
        esp_err_t err = fw_store_remove(atoi(argv[1]));
        if (err == ESP_ERR_INVALID_STATE) {
            printf("\r\n[删除] 该条目在 TF 卡上，工具不删用户的卡\r\n\r\n");
        } else if (err != ESP_OK) {
            printf("\r\n[删除] 失败：%s\r\n\r\n", esp_err_to_name(err));
        } else {
            printf("\r\n[删除] 已删除\r\n\r\n");
        }
        return;
    }

    if (strcmp(sub, "best") == 0) {
        char model[48];
        join_args(argc, argv, 1, model, sizeof(model));
        if (model[0] == '\0') {
            printf("\r\n用法：fw best \"VENTRIX CPU\"\r\n\r\n");
            return;
        }
        fw_store_scan();
        fw_model_t m = dev_model_match(model);
        const fw_item_t *it = fw_store_best(m);
        if (m == FW_MODEL_UNKNOWN) {
            printf("\r\n'%s' 不在白名单里（可用 dev raw 看设备自报的原串）\r\n\r\n", model);
        } else if (it == NULL) {
            printf("\r\n'%s' 没有可用固件\r\n\r\n", dev_model_canon(m));
        } else {
            printf("\r\n'%s' 最高版本 %s（%s，%u KB）\r\n\r\n",
                   dev_model_canon(m), ota_fw_ver_str(it->fw_ver),
                   fw_src_str(it->src), (unsigned)(it->size / 1024u));
        }
        return;
    }

    printf("\r\n用法：fw ls | fw info <n> | fw import <n> | fw del <n> | fw best <model>\r\n\r\n");
}

/* 型号匹配自测：改 dev_model.c 的别名表后，不用烧三台设备也能验证 */
static void cmd_model(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[0], "test") != 0) {
        printf("\r\n用法：model test <字符串>    例：model test \"ventrix  cpu\"\r\n\r\n");
        return;
    }

    char s[48];
    join_args(argc, argv, 1, s, sizeof(s));

    fw_model_t m = dev_model_match(s);
    if (m == FW_MODEL_UNKNOWN) {
        printf("\r\n'%s' → 未命中白名单（升级不受影响，只是不自动匹配）\r\n\r\n", s);
    } else {
        printf("\r\n'%s' → %s（枚举 %d）\r\n\r\n", s, dev_model_canon(m), (int)m);
    }
}

/* ---------------- WiFi（无触屏时的配网入口） ----------------
 * 串口不做引号解析（strtok 按空格切），所以带空格的 SSID 用
 * "把 -p 之前的全部参数拼成 SSID" 的方式传：
 *     wifi connect My Home Net -p 12345678
 *     wifi connect Guest                    ← 开放网络，省略 -p
 */
static void join_range(int argc, char **argv, int from, int to, char *out, size_t cap)
{
    out[0] = '\0';
    size_t len = 0;
    for (int i = from; i < to && i < argc; i++) {
        int m = snprintf(out + len, cap - len, (len == 0) ? "%s" : " %s", argv[i]);
        if (m < 0 || (size_t)m >= cap - len) {
            break;
        }
        len += (size_t)m;
    }
}

static void cmd_wifi(int argc, char **argv)
{
    if (argc < 1) {
        printf("\r\n[WiFi] 开关=%s  状态=%s\r\n",
               net_wifi_enabled() ? "开" : "关", net_wifi_state_str());
        if (net_wifi_ssid()[0] != '\0') {
            printf("       SSID=\"%s\"  IP=%s  RSSI=%d dBm\r\n",
                   net_wifi_ssid(), net_wifi_ip(), (int)net_wifi_rssi());
        }
        printf("\r\n");
        return;
    }

    const char *sub = argv[0];

    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        bool on = (strcmp(sub, "on") == 0);
        esp_err_t err = net_wifi_set_enabled(on);
        printf("\r\n[WiFi] %s%s\r\n\r\n", on ? "已打开" : "已关闭",
               err == ESP_OK ? "" : "（失败，看日志）");
        return;
    }

    if (strcmp(sub, "scan") == 0) {
        if (!net_wifi_enabled()) {
            printf("\r\nWiFi 没打开，先 wifi on\r\n\r\n");
            return;
        }
        esp_err_t err = net_wifi_scan_start();
        if (err != ESP_OK) {
            printf("\r\n扫描启动失败：%s\r\n\r\n", esp_err_to_name(err));
            return;
        }

        /* 扫描 2~4 s，最多等 8 s；期间让出 CPU，别把 shell 任务卡死 */
        for (int i = 0; i < 40 && net_wifi_scanning(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        net_wifi_ap_t aps[NET_WIFI_SCAN_MAX];
        int n = net_wifi_scan_get(aps, NET_WIFI_SCAN_MAX);
        printf("\r\n[扫描] %d 个网络\r\n", n);
        for (int i = 0; i < n; i++) {
            printf("  [%2d] %-32s %4d dBm  %-4s%s\r\n",
                   i, aps[i].ssid, (int)aps[i].rssi,
                   aps[i].authmode == 0 ? "open" : "wpa",
                   aps[i].saved ? "  (saved)" : "");
        }
        printf("\r\n");
        return;
    }

    if (strcmp(sub, "connect") == 0) {
        if (argc < 2) {
            printf("\r\n用法：wifi connect <ssid> [-p 密码]\r\n\r\n");
            return;
        }

        int pidx = -1;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-p") == 0) {
                pidx = i;
                break;
            }
        }

        char ssid[NET_WIFI_SSID_MAX] = {0};
        char pass[NET_WIFI_PASS_MAX] = {0};
        join_range(argc, argv, 1, pidx > 0 ? pidx : argc, ssid, sizeof(ssid));
        if (pidx > 0) {
            join_range(argc, argv, pidx + 1, argc, pass, sizeof(pass));
        }

        esp_err_t err = net_wifi_connect(ssid, pass, true);
        printf("\r\n[WiFi] 连接 \"%s\"%s：%s\r\n\r\n", ssid,
               pass[0] ? "（带密码）" : "（开放）",
               err == ESP_OK ? "已下发，用 wifi 看结果" : esp_err_to_name(err));
        return;
    }

    if (strcmp(sub, "disconnect") == 0) {
        net_wifi_disconnect();
        printf("\r\n[WiFi] 已断开（凭据保留）\r\n\r\n");
        return;
    }

    if (strcmp(sub, "forget") == 0) {
        if (argc >= 1) {
            /* 带 SSID = 只删这一条（界面上长按已知网络行的等价入口）；
             * 不带参数 = 清整表，旧行为不变。 */
            char ssid[NET_WIFI_SSID_MAX] = {0};
            join_range(argc, argv, 0, argc, ssid, sizeof(ssid));
            esp_err_t err = net_wifi_forget_one(ssid);
            printf("\r\n[WiFi] %s \"%s\"%s\r\n\r\n",
                   err == ESP_OK ? "已忘记" : "忘记失败", ssid,
                   err == ESP_OK ? "" : esp_err_to_name(err));
            return;
        }
        net_wifi_forget();
        printf("\r\n[WiFi] 已清除保存的 SSID / 密码\r\n\r\n");
        return;
    }

    printf("\r\n用法：wifi | wifi on|off | wifi scan | wifi connect <ssid> [-p 密码]"
           " | wifi disconnect | wifi forget [ssid]\r\n\r\n");
}

static void handle_line(char *line)
{
    /* 拆参数：最多 9 段（lcd w 要 1 个命令字 + 8 个参数） */
    char *argv[10] = {0};
    int argc = 0;
    char *p = strtok(line, " \t");
    char *cmd = p;
    while (p != NULL && argc < 10) {
        p = strtok(NULL, " \t");
        if (p != NULL) {
            argv[argc++] = p;
        }
    }

    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "dev") == 0) {
        print_devices(argc >= 1 && strcmp(argv[0], "raw") == 0);
    } else if (strcmp(cmd, "fw") == 0) {
        cmd_fw(argc, argv);
    } else if (strcmp(cmd, "model") == 0) {
        cmd_model(argc, argv);
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "ota") == 0) {
        cmd_ota(argc, argv);
    } else if (strcmp(cmd, "touch") == 0) {
        cmd_touch(argc, argv);
    } else if (strcmp(cmd, "time") == 0) {
        cmd_time(argc, argv);
    } else if (strcmp(cmd, "disp") == 0) {
        cmd_disp(argc, argv);
    } else if (strcmp(cmd, "sd") == 0) {
        cmd_sd(argc, argv);
    } else if (strcmp(cmd, "wifi") == 0) {
        cmd_wifi(argc, argv);
    } else if (strcmp(cmd, "bl") == 0) {
        cmd_bl(argc, argv);
    } else if (strcmp(cmd, "lcd") == 0) {
        cmd_lcd(argc, argv);
    } else {
        printf("\r\n未知命令 '%s'，输入 help 查看\r\n\r\n", cmd);
    }
}

static void shell_task(void *arg)
{
    char line[SHELL_LINE_MAX];
    int  len = 0;
    bool src_logged = false;

    printf("\r\n[命令] 输入 help 查看（回车执行）\r\n");

    while (true) {
        uint8_t c = 0;
        int n = uart_read_bytes(SHELL_UART, &c, 1, pdMS_TO_TICKS(20));
        const char *src = "UART0";

#if SHELL_HAS_USJ
        if (n <= 0) {
            /* UART0 没数据就轮到 USB-JTAG；两段各 20 ms，打字感觉不到延迟 */
            n   = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20));
            src = "USB-JTAG";
        }
#endif
        if (n <= 0) {
            continue;
        }
        if (!src_logged) {
            src_logged = true;
            printf("\r\n[命令] 已从 %s 收到输入\r\n", src);
        }

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                printf("\r\n");
                handle_line(line);
                len = 0;
            }
            continue;
        }
        if (c == 0x7F || c == 0x08) {
            if (len > 0) {
                len--;
                printf("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < SHELL_LINE_MAX - 1) {
            line[len++] = (char)c;
            printf("%c", c);            /* 回显，否则看不到自己敲的字 */
        }
    }
}

esp_err_t ota_shell_init(void)
{
    /* 控制台 UART 的驱动可能已被别处装过（返回非 OK 也不影响 read），
     * 所以这里不校验返回值，只看能不能读。 */
    esp_err_t err = uart_driver_install(SHELL_UART, 2048, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "UART%d 驱动：%s", SHELL_UART, esp_err_to_name(err));

#if SHELL_HAS_USJ
    /* 二级控制台可能已经装过 USJ 驱动，重复 install 会返回非 OK，
     * 但 RX 环形缓冲已经存在，read_bytes 照样能拿到数据。 */
    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj.rx_buffer_size = 1024;
    err = usb_serial_jtag_driver_install(&usj);
    ESP_LOGI(TAG, "USB-Serial-JTAG 驱动：%s", esp_err_to_name(err));
#endif

    BaseType_t ok = xTaskCreate(shell_task, "ota_shell", APP_SHELL_TASK_STACK, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "命令行就绪：help / dev / status / ota <n> [KB] / touch");
    return ESP_OK;
}
