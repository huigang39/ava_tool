/*
 * example_bench.c — 量一下"同一套串口参数下 SDK 到底能跑多快"。
 *
 * 用途：把它和 ava_tool 面板日志里的 [rate] 行对比。
 *   - 这里很快、面板很慢  -> 瓶颈在面板的调度/发布，不在 SDK
 *   - 这里也慢            -> 瓶颈在串口/传感器配置（波特率、通信模式、驱动延迟计时器）
 *
 * 两种读法都测，覆盖以前的"绑定函数"用法：
 *   read_rt : dyn200_read_rt()  —— 面板自动读取走的就是它
 *   update  : dyn200_update()   —— 等价于把变量绑到 dyn200_read_*_nm；
 *                                 再配合 dyn200_get_* 就是"一次读、多通道共享"的老用法
 *
 * 用法: example_bench <port> <baud> <protocol 1|6|8> [address] [seconds]
 *      example_bench <port> scan    —— 扫描波特率/协议，找出传感器真实配置
 */
#include "dyn200.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static double now_ms(void)
{
#if defined(_WIN32)
        static LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static void run(const char *label, int (*call)(void), double seconds)
{
        double t0   = now_ms();
        long   n    = 0;
        long   ok   = 0;
        long   fresh = 0, held = 0;
        long   timeout = 0, crc = 0, other = 0;
        double last = t0, minms = 1e9, maxms = 0.0;

        while ((last - t0) < seconds * 1000.0) {
                double c0 = now_ms();
                const int rc = call();
                if (rc >= 0) {
                        ++ok;
                        if (rc == DYN200_OK) ++fresh;
                        else ++held;
                } else {
                        switch (dyn200_last_error_code()) {
                        case DYN200_ERR_TIMEOUT: ++timeout; break;
                        case DYN200_ERR_CRC: ++crc; break;
                        default: ++other; break;
                        }
                }
                double c1 = now_ms();
                double dt = c1 - c0;
                if (dt < minms) minms = dt;
                if (dt > maxms) maxms = dt;
                ++n;
                last = c1;
        }
        const double elapsed = last - t0;
        printf("%-12s calls=%8.1f Hz new=%8.1f Hz held=%ld   ok=%ld timeout=%ld crc=%ld other=%ld   min=%.3f avg=%.3f max=%.3f ms\n",
               label,
               n * 1000.0 / elapsed, fresh * 1000.0 / elapsed, held,
               ok, timeout, crc, other,
               minms, elapsed / (n ? n : 1), maxms);
}

static int call_read_rt(void)
{
        static dyn200_rt rt;
        const int rc = dyn200_read_rt(&rt);
        return rc == DYN200_OK && !rt.fresh ? 1 : rc;
}

static int call_read_rt_fresh(void)
{
        static dyn200_rt rt;
        rt.wait_fresh = 1;
        return dyn200_read_rt(&rt);
}

static int call_update(void)
{
        return dyn200_update();
}

/**
 * @brief 逐个波特率 + 协议试一遍，看哪个组合真正能收到帧
 */
static int open_protocol(const char *port, int baud, int protocol, int address, int timeout)
{
        if (protocol == 6)
                return dyn200_open_active(port, baud, timeout, 1);
        if (protocol == 8)
                return dyn200_open_active8(port, baud, timeout, 1);
        return dyn200_open_ex(port, baud, address, timeout, 1);
}

static void scan(const char *port)
{
        static const int bauds[] = {115200, 57600, 38400, 19200, 9600, 4800, 2400};
        static const int protos[] = {6, 8, 1};
        const double     window  = 800.0;  // ms
        printf("scanning %s (%.0f ms per combo)...\n", port, window);

        for (size_t pi = 0; pi < sizeof(protos) / sizeof(protos[0]); ++pi) {
                for (size_t bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); ++bi) {
                        const int baud = bauds[bi];
                        const int proto = protos[pi];
                        if (open_protocol(port, baud, proto, 1, 120) != DYN200_OK) {
                                printf("  proto %d %6d bps: open failed (%s)\n", proto, baud, dyn200_last_error());
                                continue;
                        }
                        double t0 = now_ms();
                        long   n  = 0;
                        long   ok = 0;
                        while ((now_ms() - t0) < window) {
                                if (dyn200_update() == DYN200_OK)
                                        ++ok;
                                ++n;
                        }
                        const double dt = now_ms() - t0;
                        dyn200_close();
                        printf("  proto %d %6d bps: %6.1f frames/s   %ld/%ld ok\n",
                               proto,
                               baud,
                               ok * 1000.0 / dt,
                               ok,
                               n);
                        // 找到能收帧的组合就提前结束（通常是传感器真实配置）
                        if (ok >= 5 && ok * 1000.0 / dt > 5.0)
                                return;
                }
        }
        printf("no working baud/protocol found\n");
}

/**
 * @brief 把每一帧的到达时刻打出来：dt 若是整齐的小间隔=传感器匀速；若夹着几百 ms 的空档，
 *        就是驱动延迟计时器（或传感器突发输出）在作怪
 */
static void dump_frames(const char *port, int baud, int protocol, double seconds)
{
        if (open_protocol(port, baud, protocol, 1, 300) != DYN200_OK) {
                fprintf(stderr, "open failed: %s\n", dyn200_last_error());
                return;
        }
        printf("frame arrival (ms since start, dt = gap to previous frame):\n");
        double t0 = now_ms(), prev = 0.0;
        int    i = 0;
        while (now_ms() - t0 < seconds * 1000.0) {
                Dyn200Measurement m;
                int rc = protocol == 8 ? dyn200_read_active8(&m) : dyn200_read_active6(&m);
                if (rc != DYN200_OK)
                        continue;
                double t = now_ms() - t0;
                printf("  %3d  +%7.1f ms   dt=%6.1f ms   torque_raw=%d speed_raw=%d\n",
                       i++,
                       t,
                       i > 1 ? t - prev : 0.0,
                       m.torque_raw,
                       m.speed_raw);
                prev = t;
        }
        dyn200_close();
}

int main(int argc, char **argv)
{
        if (argc == 3 && strcmp(argv[2], "scan") == 0) {
                scan(argv[1]);
                return 0;
        }
        if (argc >= 4 && strcmp(argv[3], "dump") == 0) {
                dump_frames(argv[1], atoi(argv[2]), argc > 4 ? atoi(argv[4]) : 6, argc > 5 ? atof(argv[5]) : 3.0);
                return 0;
        }
        if (argc < 4) {
                fprintf(stderr, "usage: %s <port> <baud> <protocol 1|6|8> [address] [seconds]\n", argv[0]);
                return 2;
        }
        const char *port = argv[1];
        const int   baud = atoi(argv[2]);
        const int   protocol = atoi(argv[3]);
        const int   address = argc > 4 ? atoi(argv[4]) : 1;
        const double seconds = argc > 5 ? atof(argv[5]) : 3.0;
        if (!(seconds > 0.0) || !isfinite(seconds) || (protocol != 1 && protocol != 6 && protocol != 8)) {
                fprintf(stderr, "seconds must be positive and protocol must be 1, 6 or 8\n");
                return 2;
        }

        // 超时用 200 ms，和面板里串口表的默认值一致
        const int   timeout = 200;
        int         rc      = 0;
        if (protocol == 6)
                rc = dyn200_open_active(port, baud, timeout, 1);
        else if (protocol == 8)
                rc = dyn200_open_active8(port, baud, timeout, 1);
        else
                rc = dyn200_open_ex(port, baud, address, timeout, 1);
        if (rc != DYN200_OK) {
                fprintf(stderr, "open %s: %s\n", port, dyn200_last_error());
                return 1;
        }
        printf("port=%s baud=%d protocol=%d address=%d, %.1f s each\n", port, baud, protocol, address, seconds);

        puts("calls = API calls/s; new = newly delivered samples/s; held = reused cached values.");
        puts("rt_latest may skip old frames; rt_fresh and update consume frames in arrival order.");
        run("rt_latest", call_read_rt, seconds);
        run("rt_fresh", call_read_rt_fresh, seconds);
        run("update", call_update, seconds);

        dyn200_close();
        return 0;
}
