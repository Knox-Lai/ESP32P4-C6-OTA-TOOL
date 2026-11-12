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
#include "dev_table.h"
#include "ota_client.h"
#include "ota_image.h"
#include "ota_proto.h"
#include "sys_clock.h"
#include "ota_shell.h"

static const char *TAG = "shell";

#define SHELL_UART      UART_NUM_0
#define SHELL_LINE_MAX  80

static void print_help(void)
{
    printf("\r\n"
           "可用命令（回车执行）：\r\n"
           "  help            本帮助\r\n"
           "  dev             打印设备表（编号与日志里的 [n] 一致）\r\n"
           "  status          上一次/当前升级会话的状态\r\n"
           "  ota <n> [KB]    对设备表第 n 台发起升级，合成镜像，默认 %d KB\r\n"
           "                  KB 范围 16~2048\r\n"
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
           "  sd              TF 卡状态与容量（没插卡显示未挂载）\r\n"
           "  sd mount        立即重试挂载\r\n"
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

static void print_devices(void)
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
            printf("  model='%s' hw=%u.%u fw=%u.%u.%u.%u\r\n",
                   e->id.model,
                   (unsigned)(e->id.hw_ver >> 8), (unsigned)(e->id.hw_ver & 0xFF),
                   e->id.fw_ver[0], e->id.fw_ver[1], e->id.fw_ver[2], e->id.fw_ver[3]);
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
        printf("\r\n用法：ota <n> [KB]\r\n\r\n");
        return;
    }

    int idx = atoi(argv[0]);
    int kb  = (argc >= 2) ? atoi(argv[1]) : APP_OTA_SYNTH_SIZE_KB;
    if (kb < 16 || kb > 2048) {
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
    esp_err_t err = ota_image_make_synthetic(&img,
                                             e->id.model,
                                             e->id.hw_ver, e->id.hw_ver,   /* 只接受当前硬件版本 */
                                             next_version(e->id.fw_ver),
                                             (uint32_t)kb * 1024u,
                                             APP_OTA_SYNTH_SEED);
    if (err != ESP_OK) {
        printf("\r\n生成镜像失败：%s\r\n\r\n", esp_err_to_name(err));
        return;
    }

    err = ota_client_start(e->ip, e->id.uid, e->mac, &img);
    if (err != ESP_OK) {
        printf("\r\n发起升级失败：%s\r\n\r\n", esp_err_to_name(err));
        return;
    }
    printf("\r\n已发起：设备 [%d] %s -> fw=%s，%d KB。用 status 看进度\r\n\r\n",
           idx, e->id.model, ota_fw_ver_str(img.fw_ver), kb);
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

/* sd          当前状态（挂载点 / 容量）
 * sd mount    立即重试挂载（刚插卡不想等后台 3 s 轮询时用）
 */
static void cmd_sd(int argc, char **argv)
{
    if (argc >= 1 && strcmp(argv[0], "mount") == 0) {
        esp_err_t err = bsp_sdcard_mount_now();
        if (err != ESP_OK) {
            printf("\r\n[TF] 挂载失败：%s\r\n"
                   "     常见原因：没插卡 / 不是 FAT32 / 卡座接触不良\r\n\r\n",
                   esp_err_to_name(err));
            return;
        }
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
        print_devices();
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
