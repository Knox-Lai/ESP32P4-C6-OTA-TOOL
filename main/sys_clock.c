#include "sys_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"

static const char *TAG = "clock";

#define CLOCK_TZ       "CST-8"      /* 北京时间（东八区），POSIX 写法是反的 */
#define CLOCK_TZ_OFFS  8            /* 秒数换算用 */

static const char *const k_wday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* 公历日期 -> 1970-01-01 起的天数（Howard Hinnant 算法，无时区问题） */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2u);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2u ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int month_from_name(const char *s)
{
    for (int i = 0; i < 12; i++) {
        if (strncmp(s, k_mon[i], 3) == 0) {
            return i + 1;
        }
    }
    return 1;
}

/* 打印用的 tm（本地时间） */
static void now_tm(struct tm *out)
{
    time_t t = time(NULL);
    localtime_r(&t, out);
}

int64_t sys_clock_epoch(void)
{
    return (int64_t)time(NULL);
}

bool sys_clock_set(int year, int mon, int day, int hour, int min, int sec)
{
    if (year < 2020 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return false;
    }

    int64_t utc = days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400 +
                  (int64_t)hour * 3600 + (int64_t)min * 60 + sec -
                  (int64_t)CLOCK_TZ_OFFS * 3600;

    struct timeval tv = {
        .tv_sec  = (time_t)utc,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        return false;
    }

    return true;
}

void sys_clock_init(void)
{
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    /* __DATE__ = "Nov  5 2026"，__TIME__ = "16:22:44" */
    const char *d = __DATE__;
    const char *t = __TIME__;

    int mon = month_from_name(d);
    int day = atoi(d + 4);
    int year = atoi(d + 7);
    int hh = atoi(t);
    int mm = atoi(t + 3);
    int ss = atoi(t + 6);

    if (!sys_clock_set(year, mon, day, hh, mm, ss)) {
        ESP_LOGW(TAG, "编译时间解析失败，时钟保持 1970");
        return;
    }

    char buf[32];
    sys_clock_full_str(buf, sizeof(buf));
    ESP_LOGW(TAG, "系统时钟初值 = 编译时间 %s（无 RTC / 无网络，用串口 time set 校正）", buf);
}

void sys_clock_time_str(char *buf, size_t n)
{
    struct tm t;
    now_tm(&t);
    snprintf(buf, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

void sys_clock_date_str(char *buf, size_t n)
{
    struct tm t;
    now_tm(&t);
    int wd = (t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0;
    snprintf(buf, n, "%02d-%02d %s", t.tm_mon + 1, t.tm_mday, k_wday[wd]);
}

void sys_clock_full_str(char *buf, size_t n)
{
    struct tm t;
    now_tm(&t);
    int wd = (t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0;
    snprintf(buf, n, "%04d-%02d-%02d %s %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, k_wday[wd],
             t.tm_hour, t.tm_min, t.tm_sec);
}
