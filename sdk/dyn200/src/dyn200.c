#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif
#include "dyn200.h"
#include "dyn200_ports.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dyn200_serial_win32.h"
/* RTU 帧间等待使用毫秒级 Sleep；只在串口打开期间申请精细定时器，关闭时还原。
 * 主动上传直接等待首批字节，不再依赖 Sleep(1) 轮询。 */
__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
__declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
static HANDLE g_port = INVALID_HANDLE_VALUE;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static int g_timer_begun;
static uint32_t g_active_read_timeout_ms;
static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID context)
{
        (void)once; (void)param; (void)context;
        InitializeCriticalSection(&g_lock);
        return TRUE;
}
static void lock_sdk(void) { InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL); EnterCriticalSection(&g_lock); }
static void unlock_sdk(void) { LeaveCriticalSection(&g_lock); }
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
static int g_port = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_sdk(void) { pthread_mutex_lock(&g_lock); }
static void unlock_sdk(void) { pthread_mutex_unlock(&g_lock); }
#endif

// 传感器命令地址（写单线圈），见 DYN-200 说明书
#define DYN200_ZERO_ADDRESS 0x0000u          // 扭矩清零
#define DYN200_FACTORY_RESET_ADDRESS 0x0002u // 恢复出厂设置

// 串口枚举缓存和参数回读的节流时间
#define DYN200_PORT_SCAN_MS 2000u
#define DYN200_PARAM_READBACK_MS 2000u
/* 主动上传取帧时的收集缓冲区（一帧最多 8 字节，留足够余量一次收多个帧）。
 * USB 串口驱动会把连续的 6/8 字节帧打包成约 4 KB 的块、每几百毫秒交付一次，
 * 所以缓冲区要大到能装下整块，否则每次只取到 512 字节就会把剩下的帧全丢掉。 */
#define DYN200_ACTIVE_BUF 16384u

// Modbus 收发节拍：上一帧结束时间 + 连续失败计数（用于错位重试和退避）
static uint64_t g_tx_end_us;
static int      g_tx_fail_streak;

// 主动上传流的节奏：最近一次收到新帧的时间 + 帧间隔估计（判断"流是不是停了"）
static uint32_t g_last_active_frame_ms;
/* 主动上传流的帧队列：USB 打包把帧拆开/攒批，这里统一缓冲、逐帧消费，
 * 保证一帧都不丢（尤其是整块 4 KB 到达时不能再像以前那样只取第一帧就丢余量）。 */
static uint8_t g_active_buf[DYN200_ACTIVE_BUF];
static size_t  g_active_buf_len;
static size_t  g_active_buf_pos;

static uint8_t g_address = 1;
static int g_error_code;
static char g_error[256] = "OK";
static int g_torque_decimals = 1;
static int g_speed_decimals;
static int g_power_decimals;
static int g_protocol = 1;               // 1=Modbus, 6=active mode 0, 8=active mode 3
static Dyn200Measurement g_measurement;

// 当前连接：串口表/参数表/实时表的反馈字段和 dyn200_connection() 用它
static char g_port_name[DYN200_PORT_NAME_MAX];
static int  g_baud_rate = 9600;
static int  g_stop_bits = 1;
static int  g_timeout_ms = 1000;

// 设备设置回读：不支持回读的机型只试一次，之后不再打扰总线
static int      g_readback_unsupported;
static uint32_t g_readback_ms;

// 最近的串口枚举结果（带缓存，避免自动读取时反复枚举）
static char     g_port_names[DYN200_PORT_LIST_MAX][DYN200_PORT_NAME_MAX];
static char     g_port_descs[DYN200_PORT_LIST_MAX][DYN200_PORT_DESC_MAX];
static uint32_t g_port_total;
static uint32_t g_port_scan_ms;
static int      g_port_scanned;

/*
 * 用户最后一次配好的连接（串口表写一次、或者扁平 API 成功打开一次就记住）。
 * 串口断了以后，rt/参数表的读写会用它自动把连接恢复起来，所以用户只要在
 * 串口表里选好口，rt 表就能直接读数，不必每次都手动 CONNECT。
 * dyn200_close() 会忘掉它（彻底关闭），串口表的 DISCONNECT 只是暂停自动重连。
 */
static int      g_link_valid;
static char     g_link_port[DYN200_PORT_NAME_MAX];
static int      g_link_protocol = 1;
static int      g_link_baud = 9600;
static int      g_link_address = 1;
static int      g_link_stop_bits = 1;
static int      g_link_timeout_ms = 1000;
static int      g_link_hold_closed; // 串口表按过 DISCONNECT
static uint32_t g_link_retry_ms;    // 上次自动重连失败的时间，用于节流

static int set_error(int code, const char *message)
{
        g_error_code = code;
        snprintf(g_error, sizeof(g_error), "%s", message);
        return code;
}
static int set_ok(void) { return set_error(DYN200_OK, "OK"); }

static uint32_t monotonic_ms(void);
static uint64_t monotonic_us(void);

// Internal transport seams for deterministic, hardware-free stream tests
#if defined(DYN200_TEST)
static int dyn200_test_port_is_open(void);
static size_t dyn200_test_bytes_available(void);
static int dyn200_test_read_bytes(uint8_t *, size_t, size_t *, uint32_t);
static uint64_t dyn200_test_monotonic_us(void);
#endif

DYN200_API uint16_t dyn200_crc16(const uint8_t *data, size_t size)
{
        uint16_t crc = 0xffff;
        int bit;
        if (!data && size) return 0;
        while (size--) {
                crc ^= *data++;
                for (bit = 0; bit < 8; ++bit)
                        crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xa001u) : (uint16_t)(crc >> 1);
        }
        return crc;
}
static void append_crc(uint8_t *data, size_t size)
{
        uint16_t crc = dyn200_crc16(data, size);
        data[size] = (uint8_t)crc;
        data[size + 1] = (uint8_t)(crc >> 8);
}
static int valid_crc(const uint8_t *data, size_t size)
{
        return size >= 2 && dyn200_crc16(data, size - 2) ==
               (uint16_t)(data[size - 2] | ((uint16_t)data[size - 1] << 8));
}
static int port_is_open(void)
{
#if defined(DYN200_TEST)
        return dyn200_test_port_is_open();
#elif defined(_WIN32)
        return g_port != INVALID_HANDLE_VALUE;
#else
        return g_port >= 0;
#endif
}
static void port_close(void)
{
#if defined(_WIN32)
        if (g_port != INVALID_HANDLE_VALUE) CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
#else
        if (g_port >= 0) close(g_port);
        g_port = -1;
#endif
        g_active_buf_len = 0;
        g_active_buf_pos = 0;
        g_last_active_frame_ms = 0;
        g_tx_end_us = 0;
        g_tx_fail_streak = 0;
#if defined(_WIN32)
        if (g_timer_begun) {
                timeEndPeriod(1);
                g_timer_begun = 0;
        }
#endif
}
static int write_all(const uint8_t *data, size_t size)
{
        while (size) {
#if defined(_WIN32)
                DWORD done = 0;
                if (!WriteFile(g_port, data, (DWORD)size, &done, NULL) || !done) return -1;
#else
                ssize_t done = write(g_port, data, size);
                if (done < 0 && errno == EINTR) continue;
                if (done <= 0) return -1;
#endif
                data += done;
                size -= (size_t)done;
        }
        return 0;
}
static void discard_pending_input(void)
{
#if defined(_WIN32)
        /*
         * 只有真的积压了字节才清。部分 USB 串口驱动上 PurgeComm 是同步的、要等
         * 驱动把缓冲刷完（实测可以到几百毫秒），每帧都调会把采样率拖到几 Hz。
         */
        DWORD   errors = 0;
        COMSTAT status;
        if (ClearCommError(g_port, &errors, &status) && status.cbInQue > 0)
                PurgeComm(g_port, PURGE_RXCLEAR);
#else
        tcflush(g_port, TCIFLUSH);
#endif
}

/*
 * 收字节：一次把"驱动里已经可用的字节"全读出来，而不是一个字节一个 ReadFile。
 *
 * 为什么重要：USB 串口（FTDI 之类）驱动有个 latency timer，短读必须等它过期
 * 才把数据交给应用层；255 ms 的驱动配上"先收 3 字节、再收剩下 14 字节"的两次
 * 短读，一帧就是 ~600 ms —— 实测就是这个量级。一次收整帧只吃一个计时周期。
 *
 * 返回已收字节数（*got）；达到 size 或超时就返回，调用方自己判断够不够。
 */
/**
 * @brief 驱动里现在攒着多少字节（不等待）; 主动上传取帧时先把它全部取走
 */
static size_t bytes_available_unlocked(void)
{
#if defined(DYN200_TEST)
        return dyn200_test_bytes_available();
#elif defined(_WIN32)
        DWORD   errors = 0;
        COMSTAT status;
        if (!ClearCommError(g_port, &errors, &status))
                return 0;
        return (size_t)status.cbInQue;
#else
        int pending = 0;
        if (ioctl(g_port, FIONREAD, &pending) != 0 || pending <= 0)
                return 0;
        return (size_t)pending;
#endif
}

static void read_available(uint8_t *data, size_t size, size_t *got, uint32_t timeout_ms)
{
        const uint32_t deadline = monotonic_ms() + (timeout_ms ? timeout_ms : 1u);
        size_t         have     = 0;

        (void)deadline;
        *got = 0;
#if defined(_WIN32)
        /*
         * 一次 ReadFile 收整帧：驱动配了"字节间隔超时"（ReadIntervalTimeout），
         * 收到 size 字节或帧内出现空档就返回，所以常见情况一次系统调用就够了。
         * 但"首字节还没来"时必须继续等到总超时——有些设备要几十毫秒才开始应答，
         * 直接放弃会误判成超时。
         */
        while (have < size) {
                DWORD done = 0;
                if (!ReadFile(g_port, data + have, (DWORD)(size - have), &done, NULL))
                        break;
                if (done > 0) {
                        have += done;
                        continue;
                }
                if (have > 0)
                        break; // 已经收到一部分：帧内空档，这帧就这些字节
                if ((int32_t)(deadline - monotonic_ms()) <= 0)
                        break; // 首字节迟迟不来：到总超时了
                Sleep(1);
        }
#else
        while (have < size) {
                const int32_t remain = (int32_t)(deadline - monotonic_ms());
                if (remain <= 0)
                        break;
                struct pollfd pfd;
                pfd.fd      = g_port;
                pfd.events  = POLLIN;
                pfd.revents = 0;
                const int ready = poll(&pfd, 1, remain);
                if (ready <= 0)
                        break;
                const ssize_t done = read(g_port, data + have, size - have);
                if (done < 0) {
                        if (errno == EINTR)
                                continue;
                        break;
                }
                if (done == 0)
                        continue;
                have += (size_t)done;
        }
#endif
        *got = have;
}

/**
 * @brief Modbus RTU 要求两帧之间至少有 3.5 个字符时间的静默间隔。轮询很密时补上它，
 *        免得背靠背发帧把设备/驱动压出丢帧、错位；轮询本来就慢的场合它不额外等待
 */
static void modbus_interframe_gap_unlocked(void)
{
        const uint32_t baud   = (uint32_t)(g_baud_rate > 0 ? g_baud_rate : 9600);
        const uint32_t bits = 9u + (uint32_t)g_stop_bits;
        const uint32_t gap_us = baud > 19200u ? 1750u : (3500000u * bits + baud - 1u) / baud;
        if (g_tx_end_us == 0)
                return;
        for (;;) {
                const uint64_t elapsed = monotonic_us() - g_tx_end_us;
                if (elapsed >= gap_us)
                        return;
                const uint32_t wait_us = gap_us - (uint32_t)elapsed;
#if defined(_WIN32)
                Sleep((wait_us + 999u) / 1000u);
#else
                struct timespec ts = {0, (long)wait_us * 1000L};
                nanosleep(&ts, NULL);
#endif
        }
}

static int transact_once(const uint8_t *request, size_t request_size, uint8_t *response, size_t response_size);

static int transact(const uint8_t *request, size_t request_size, uint8_t *response, size_t response_size)
{
        const int rc = transact_once(request, request_size, response, response_size);
        g_tx_end_us  = monotonic_us();
        if (rc == DYN200_OK) {
                g_tx_fail_streak = 0;
                return rc;
        }
        if (g_tx_fail_streak < 99)
                ++g_tx_fail_streak;
        /*
         * 背靠背轮询时最常见的是"上一帧的迟到应答"把这一帧顶歪（CRC 错或机码对不上）。
         * 这种情况清掉残留再发一次就好；但如果已经连续失败好几次，说明是配置/接线
         * 问题，别再重试浪费时间（每次重试都要等一个超时）。
         */
        if ((rc == DYN200_ERR_CRC || rc == DYN200_ERR_PROTOCOL) && g_tx_fail_streak <= 3) {
                discard_pending_input();
                const int retry = transact_once(request, request_size, response, response_size);
                g_tx_end_us     = monotonic_us();
                if (retry == DYN200_OK)
                        g_tx_fail_streak = 0;
                return retry;
        }
        return rc;
}

/**
 * @brief 单次收发
 */
static int transact_once(const uint8_t *request, size_t request_size, uint8_t *response, size_t response_size)
{
        size_t actual_size;
        size_t have = 0;
        if (!port_is_open()) return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open");
        if (g_protocol != DYN200_PROTOCOL_MODBUS)
                return set_error(DYN200_ERR_PROTOCOL, "Modbus commands require protocol=1");
        modbus_interframe_gap_unlocked();
        // 丢掉上一帧没读完的残留字节，避免把旧应答当成新应答
        discard_pending_input();
        if (write_all(request, request_size)) return set_error(DYN200_ERR_WRITE, "serial write failed");
        if (response_size < 5) return set_error(DYN200_ERR_ARGUMENT, "response buffer is too small");
        /*
         * 一次收整帧（正常应答就是 response_size 字节）。异常应答只有 5 字节，
         * 这时等到超时也无所谓——已经收到的 5 字节里就能看出异常码。
         */
        read_available(response, response_size, &have, (uint32_t)g_timeout_ms);
        if (have >= 5 && (response[1] & 0x80u))
                actual_size = 5u;
        else if (have >= response_size)
                actual_size = response_size;
        else
                return set_error(DYN200_ERR_TIMEOUT, "serial response timeout");
        if (!valid_crc(response, actual_size)) return set_error(DYN200_ERR_CRC, "Modbus CRC mismatch");
        /*
         * 机码检查放在 CRC 之后：只有"CRC 正确但机码不是我们要的"才算真的
         * "设备用别的机码回答"；线上乱码会被上面的 CRC 检查拦掉，不会被误当成
         * 另一台设备的应答（诊断时尤其重要）。
         */
        if (response[0] != g_address) {
                snprintf(g_error,
                         sizeof(g_error),
                         "unexpected device address: expected %u, received %u "
                         "(wrong device address, or the sensor is not in Modbus mode / baud mismatch)",
                         (unsigned)g_address, (unsigned)response[0]);
                g_error_code = DYN200_ERR_PROTOCOL;
                return DYN200_ERR_PROTOCOL;
        }
        if (response[1] & 0x80u) {
                snprintf(g_error, sizeof(g_error), "Modbus exception %u", (unsigned)response[2]);
                g_error_code = DYN200_ERR_MODBUS;
                return DYN200_ERR_MODBUS;
        }
        return 0;
}
static int read_registers_unlocked(uint16_t address, uint16_t count, uint16_t *values)
{
        uint8_t tx[8], rx[29];
        size_t rx_size;
        unsigned i;
        int rc;
        if (!values || count == 0 || count > 12) return set_error(DYN200_ERR_ARGUMENT, "register count must be 1..12");
        tx[0] = g_address; tx[1] = 3; tx[2] = (uint8_t)(address >> 8); tx[3] = (uint8_t)address;
        tx[4] = (uint8_t)(count >> 8); tx[5] = (uint8_t)count; append_crc(tx, 6);
        rx_size = 5u + (size_t)count * 2u;
        rc = transact(tx, sizeof(tx), rx, rx_size);
        if (rc) return rc;
        if (rx[1] != 3 || rx[2] != count * 2u) return set_error(DYN200_ERR_PROTOCOL, "invalid Modbus read response");
        for (i = 0; i < count; ++i) values[i] = (uint16_t)(((uint16_t)rx[3 + i * 2] << 8) | rx[4 + i * 2]);
        return set_ok();
}
static int read_i32_unlocked(uint16_t address, int32_t *val)
{
        uint16_t words[2];
        int rc;
        if (!val) return set_error(DYN200_ERR_ARGUMENT, "value pointer is null");
        rc = read_registers_unlocked(address, 2, words);
        if (rc) return rc;
        *val = (int32_t)(((uint32_t)words[0] << 16) | words[1]);
        return set_ok();
}
static int write_i32_unlocked(uint16_t address, int32_t val)
{
        uint8_t tx[13], rx[8];
        uint32_t raw = (uint32_t)val;
        int rc;
        tx[0] = g_address; tx[1] = 0x10; tx[2] = (uint8_t)(address >> 8); tx[3] = (uint8_t)address;
        tx[4] = 0; tx[5] = 2; tx[6] = 4; tx[7] = (uint8_t)(raw >> 24); tx[8] = (uint8_t)(raw >> 16);
        tx[9] = (uint8_t)(raw >> 8); tx[10] = (uint8_t)raw; append_crc(tx, 11);
        rc = transact(tx, sizeof(tx), rx, sizeof(rx));
        if (rc) return rc;
        if (rx[1] != 0x10 || rx[2] != tx[2] || rx[3] != tx[3] || rx[4] != 0 || rx[5] != 2)
                return set_error(DYN200_ERR_PROTOCOL, "invalid Modbus write response");
        return set_ok();
}
static int command_unlocked(uint16_t address)
{
        uint8_t tx[8], rx[8];
        int rc;
        tx[0] = g_address; tx[1] = 5; tx[2] = (uint8_t)(address >> 8); tx[3] = (uint8_t)address;
        tx[4] = 0xff; tx[5] = 0; append_crc(tx, 6);
        rc = transact(tx, sizeof(tx), rx, sizeof(rx));
        if (rc) return rc;
        if (rx[1] != 5 || memcmp(tx + 2, rx + 2, 4) != 0) return set_error(DYN200_ERR_PROTOCOL, "invalid Modbus command response");
        return set_ok();
}
static double scale_i32(int32_t val, int decimals)
{
        static const double powers[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0};
        return (double)val / powers[decimals];
}

// ─── 内部辅助 ─────────────────────────────────────────────────────────────

static unsigned or_default_u32(unsigned val, unsigned fallback)
{
        return val ? val : fallback;
}

static void set_text(char *field, size_t capacity, const char *text)
{
        if (!field || capacity == 0) return;
        const size_t length = text ? strlen(text) : 0;
        const size_t copied = length < capacity ? length : capacity - 1u;
        if (copied) memmove(field, text, copied);
        field[copied] = '\0';
}

static uint64_t monotonic_us(void)
{
#if defined(DYN200_TEST)
        return dyn200_test_monotonic_us();
#elif defined(_WIN32)
        static LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        if (frequency.QuadPart == 0)
                QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000u +
               (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000u / (uint64_t)frequency.QuadPart;
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static uint32_t monotonic_ms(void) { return (uint32_t)(monotonic_us() / 1000u); }

static int ensure_open_unlocked(const char *port, int protocol, int baud, int address, int stop_bits, int timeout_ms);

/**
 * @brief 记住这套连接参数，之后串口掉了可以自动恢复
 */
static void remember_link_unlocked(const char *port, int protocol, int baud, int address, int stop_bits, int timeout_ms)
{
        if (!port || !*port)
                return;
        set_text(g_link_port, sizeof(g_link_port), port);
        g_link_protocol = protocol;
        g_link_baud = baud;
        g_link_address = address;
        g_link_stop_bits = stop_bits;
        g_link_timeout_ms = timeout_ms;
        g_link_valid = 1;
}

/**
 * @brief 需要串口时确保它是开着的：没配过口就报 DYN200_ERR_NOT_OPEN（提示先去串口表
 *        选口），配过而且没按过 DISCONNECT 就自动重连。重连失败后 2 秒内不再重试，
 *        免得自动读取时每 50 ms 去敲一个不存在的口
 */
static int ensure_ready_unlocked(void)
{
        if (port_is_open())
                return set_ok();
        if (!g_link_valid || g_link_hold_closed)
                return set_error(DYN200_ERR_NOT_OPEN,
                                 "serial port is not open: read the dyn200_port_param table and pick the port (index or port field)");
        const uint32_t now = monotonic_ms();
        if (g_link_retry_ms != 0 && (uint32_t)(now - g_link_retry_ms) < 2000u)
                return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open (reconnecting...)");
        int rc = ensure_open_unlocked(g_link_port, g_link_protocol, g_link_baud, g_link_address, g_link_stop_bits,
                                      g_link_timeout_ms);
        g_link_retry_ms = rc == DYN200_OK ? 0 : now;
        return rc;
}

/**
 * @brief 打开串口。调用者必须已持有锁。原来的 dyn200_open_ex 就是这个流程，
 *        只是串口表也要用它，所以单独抽出来
 */
static int open_unlocked(const char *port, int baud, int address, int timeout_ms, int stop_bits)
{
        int rc = 0;
        if (!port || !*port) {
                rc = set_error(DYN200_ERR_ARGUMENT, "serial port is empty");
                goto done;
        }
        if (address < 1 || address > 120) {
                rc = set_error(DYN200_ERR_ARGUMENT, "Modbus device address must be 1..120 (0 is broadcast)");
                goto done;
        }
        if (timeout_ms < 1) {
                rc = set_error(DYN200_ERR_ARGUMENT, "timeout must be greater than zero");
                goto done;
        }
        if (stop_bits != 1 && stop_bits != 2) {
                rc = set_error(DYN200_ERR_ARGUMENT, "stop bits must be 1 or 2");
                goto done;
        }
        if (baud != 2400 && baud != 4800 && baud != 9600 && baud != 14400 && baud != 19200 && baud != 38400 &&
            baud != 57600 && baud != 115200) {
                rc = set_error(DYN200_ERR_ARGUMENT, "unsupported baud rate");
                goto done;
        }
        port_close();
        g_last_active_frame_ms = 0;
#if defined(_WIN32)
        {
                char path[128]; DCB dcb = {0}; COMMTIMEOUTS timeouts = {MAXDWORD, 0, 0, 0, 0};
                snprintf(path, sizeof(path), "\\\\.\\%s", port);
                g_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (g_port == INVALID_HANDLE_VALUE) { rc = set_error(DYN200_ERR_OPEN, "cannot open serial port"); goto done; }
                dcb.DCBlength = sizeof(dcb);
                if (!GetCommState(g_port, &dcb)) { port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot read serial settings"); goto done; }
                dyn200_normalize_binary_dcb(&dcb, (DWORD)baud,
                                             stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT);
                /*
                 * 读超时用"等整帧"的配置：收到请求的字节数就返回；帧内一旦出现
                 * 20 ms 以上空档（一帧已经发完）也返回；总共不超过 timeout_ms。
                 * 这样一次 ReadFile 就能收完整帧，不必轮询（轮询会带入 Sleep 的
                 * 毫秒级抖动，实测每帧多出十几毫秒）。
                 */
                timeouts.ReadIntervalTimeout = 20;
                timeouts.ReadTotalTimeoutMultiplier = 0;
                timeouts.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
                timeouts.WriteTotalTimeoutMultiplier = 0;
                timeouts.WriteTotalTimeoutConstant = (DWORD)timeout_ms;
                if (!SetCommState(g_port, &dcb) || !SetCommTimeouts(g_port, &timeouts) ||
                    !SetCommMask(g_port, 0)) {
                        port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot configure serial port"); goto done;
                }
        }
#else
        {
                struct termios settings; speed_t speed;
                switch (baud) {
                case 2400: speed = B2400; break;
                case 4800: speed = B4800; break;
                case 9600: speed = B9600; break;
#ifdef B14400
                case 14400: speed = B14400; break;
#endif
                case 19200: speed = B19200; break; case 38400: speed = B38400; break;
#ifdef B57600
                case 57600: speed = B57600; break;
#endif
#ifdef B115200
                case 115200: speed = B115200; break;
#endif
                default: rc = set_error(DYN200_ERR_ARGUMENT, "baud rate is unsupported on this platform"); goto done;
                }
                g_port = open(port, O_RDWR | O_NOCTTY);
                if (g_port < 0) { rc = set_error(DYN200_ERR_OPEN, "cannot open serial port"); goto done; }
                if (tcgetattr(g_port, &settings)) { port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot read serial settings"); goto done; }
                cfmakeraw(&settings); cfsetispeed(&settings, speed); cfsetospeed(&settings, speed);
                settings.c_cflag = (settings.c_cflag & ~(CSIZE | PARENB | CSTOPB)) | CS8 | CLOCAL | CREAD;
                if (stop_bits == 2) settings.c_cflag |= CSTOPB;
                // poll() owns the deadline; termios must not add a 100 ms timeout
                settings.c_cc[VMIN] = 0; settings.c_cc[VTIME] = 0;
                if (tcsetattr(g_port, TCSANOW, &settings)) { port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot configure serial port"); goto done; }
        }
#endif
        g_address = (uint8_t)address;
        g_protocol = 1;
        g_baud_rate = baud;
        g_stop_bits = stop_bits;
        g_timeout_ms = timeout_ms;
        set_text(g_port_name, sizeof(g_port_name), port);
#if defined(_WIN32)
        if (!g_timer_begun) {
                g_timer_begun = timeBeginPeriod(1) == 0;
        }
#endif
        // 打开成功就记住这套参数：串口掉了以后 rt/参数表可以自动恢复连接
        remember_link_unlocked(port, g_protocol, baud, address, stop_bits, timeout_ms);
        g_link_hold_closed = 0;
        g_link_retry_ms = 0;
        g_readback_unsupported = 0;
        g_readback_ms = 0;
        rc = set_ok();
done:   return rc;
}

/**
 * @brief 枚举串口（默认 2 秒缓存；force 用于串口表里的 RESCAN）
 */
static void refresh_ports_unlocked(int force)
{
        const uint32_t now = monotonic_ms();
        if (!force && g_port_scanned && (uint32_t)(now - g_port_scan_ms) < DYN200_PORT_SCAN_MS)
                return;
        g_port_total = dyn200_ports_enumerate(g_port_names, g_port_descs, DYN200_PORT_LIST_MAX);
        g_port_scan_ms = now;
        g_port_scanned = 1;
}

/**
 * @brief Windows 的 first-byte 模式：已有字节立即返回，否则只等首批字节
 *        这只控制 ReadFile，不冒充 FTDI 驱动的硬件 latency timer 配置
 */
static int configure_active_read_timeout(uint32_t timeout_ms)
{
#if defined(_WIN32)
        COMMTIMEOUTS to = {MAXDWORD, MAXDWORD, 0, 0, 0};
        to.ReadTotalTimeoutConstant = timeout_ms;
        to.WriteTotalTimeoutConstant = (DWORD)g_timeout_ms;
        if (!SetCommTimeouts(g_port, &to))
                return set_error(DYN200_ERR_CONFIGURE, "cannot configure active stream timeout");
        g_active_read_timeout_ms = timeout_ms;
#else
        (void)timeout_ms;
#endif
        return DYN200_OK;
}

static int configure_active_stream(uint32_t timeout_ms)
{
        const int rc = configure_active_read_timeout(timeout_ms);
        if (rc != DYN200_OK)
                return rc;
#if defined(_WIN32)
        /* FTDI VCP event-character delivery (AN232-06). EvtChar is initialized
         * to NUL by dyn200_normalize_binary_dcb. This flushes short USB packets
         * when NUL occurs, without dropping it or treating it as a frame end.
         * Otherwise continuous streams may arrive as ~4 KB / ~660 ms bursts.
         * Apply on every open/reconnect, not on each read-timeout adjustment.
         * Payloads without NUL still use the driver's normal delivery policy. */
        if (!SetCommMask(g_port, EV_RXFLAG))
                return set_error(DYN200_ERR_CONFIGURE, "cannot configure active stream receive events");
#endif
        return DYN200_OK;
}

/**
 * @brief Read currently queued bytes, or wait for the first arriving bytes up to one deadline
 */
static int read_active_bytes_unlocked(uint8_t *data, size_t capacity, size_t *got, uint32_t timeout_ms)
{
        *got = 0;
#if defined(DYN200_TEST)
        return dyn200_test_read_bytes(data, capacity, got, timeout_ms);
#elif defined(_WIN32)
        DWORD done = 0;
        // A queued read needs no timeout reconfiguration; first-byte mode returns immediately
        if (timeout_ms != 0 && timeout_ms != g_active_read_timeout_ms &&
            configure_active_read_timeout(timeout_ms) != DYN200_OK)
                return DYN200_ERR_CONFIGURE;
        if (!ReadFile(g_port, data, (DWORD)capacity, &done, NULL)) {
                port_close();
                return set_error(DYN200_ERR_READ, "active stream serial read failed");
        }
        *got = (size_t)done;
#else
        const uint32_t start = monotonic_ms();
        for (;;) {
                const uint32_t elapsed = (uint32_t)(monotonic_ms() - start);
                const int remain = elapsed >= timeout_ms ? 0 : (int)(timeout_ms - elapsed);
                struct pollfd pfd = {g_port, POLLIN, 0};
                const int ready = poll(&pfd, 1, remain);
                if (ready < 0 && errno == EINTR)
                        continue;
                if (ready == 0)
                        return DYN200_OK;
                if (ready < 0 || (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) {
                        port_close();
                        return set_error(DYN200_ERR_READ, "active stream serial read failed");
                }
                const ssize_t done = read(g_port, data, capacity);
                if (done < 0 && (errno == EINTR || errno == EAGAIN))
                        continue;
                if (done < 0) {
                        port_close();
                        return set_error(DYN200_ERR_READ, "active stream serial read failed");
                }
                *got = (size_t)done;
                break;
        }
#endif
#if !defined(DYN200_TEST)
        return DYN200_OK;
#endif
}

/**
 * @brief 需要连接时就连接、参数变了就按新参数重连；已经用同一套参数连着就直接返回
 *        串口表的 CONNECT 和"改了参数自动重连"都走这里
 */
static int ensure_open_unlocked(const char *port, int protocol, int baud, int address, int stop_bits, int timeout_ms)
{
        int rc;
        if (port_is_open() && strcmp(port, g_port_name) == 0 && protocol == g_protocol && baud == g_baud_rate &&
            address == (int)g_address && stop_bits == g_stop_bits && timeout_ms == g_timeout_ms)
                return set_ok();
        rc = open_unlocked(port, baud, address, timeout_ms, stop_bits);
        if (!rc) {
                g_protocol = protocol;
                // 换了设备，参数回读重新试一次
                g_readback_unsupported = 0;
                g_readback_ms = 0;
                if (protocol == DYN200_PROTOCOL_ACTIVE6 || protocol == DYN200_PROTOCOL_ACTIVE8) {
                        rc = configure_active_stream((uint32_t)timeout_ms);
                        if (rc != DYN200_OK)
                                port_close();
                }
                if (rc == DYN200_OK)
                        remember_link_unlocked(port, protocol, baud, address, stop_bits, timeout_ms);
        }
        return rc;
}

DYN200_API int dyn200_open_ex(const char *port, int baud, int address, int timeout_ms, int stop_bits)
{
        int rc;
        lock_sdk();
        rc = open_unlocked(port, baud, address, timeout_ms, stop_bits);
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_open(const char *port, int baud, int address, int timeout_ms)
{
        return dyn200_open_ex(port, baud, address, timeout_ms, 1);
}
static int open_active_unlocked(const char *port, int baud, int timeout_ms, int stop_bits, int protocol)
{
        // The address is unused while receiving an unsolicited byte stream
        int rc = open_unlocked(port, baud, 1, timeout_ms, stop_bits);
        if (!rc) {
                g_protocol = protocol;
                g_readback_unsupported = 0;
                g_readback_ms = 0;
                rc = configure_active_stream((uint32_t)timeout_ms);
                if (rc == DYN200_OK)
                        remember_link_unlocked(port, protocol, baud, 1, stop_bits, timeout_ms);
                else
                        port_close();
        }
        return rc;
}
DYN200_API int dyn200_open_active(const char *port, int baud, int timeout_ms, int stop_bits)
{
        int rc;
        lock_sdk();
        rc = open_active_unlocked(port, baud, timeout_ms, stop_bits, 6);
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_open_active8(const char *port, int baud, int timeout_ms, int stop_bits)
{
        int rc;
        lock_sdk();
        rc = open_active_unlocked(port, baud, timeout_ms, stop_bits, 8);
        unlock_sdk();
        return rc;
}
/**
 * @brief 彻底关闭：忘掉"记住了的连接参数"，之后的 rt/参数表读写不会再自动重连
 *        串口表的 DISCONNECT 是"先不连"（保留参数，再 CONNECT 或换口名即可重连）
 */
DYN200_API int dyn200_close(void)
{
        lock_sdk();
        port_close();
        g_link_valid = 0;
        g_link_hold_closed = 1;
        g_link_retry_ms = 0;
        set_ok();
        unlock_sdk();
        return 0;
}
DYN200_API int dyn200_is_open(void) { int val; lock_sdk(); val = port_is_open(); unlock_sdk(); return val; }
DYN200_API int dyn200_connection(char *port, size_t port_size, int *baud_rate, int *protocol)
{
        int open;
        lock_sdk();
        if (port && port_size)
                snprintf(port, port_size, "%s", g_port_name);
        if (baud_rate)
                *baud_rate = g_baud_rate;
        if (protocol)
                *protocol = g_protocol;
        open = port_is_open();
        unlock_sdk();
        return open ? DYN200_OK : DYN200_ERR_NOT_OPEN;
}
DYN200_API uint32_t dyn200_port_count(void)
{
        uint32_t total;
        lock_sdk();
        refresh_ports_unlocked(0);
        total = g_port_total;
        unlock_sdk();
        return total;
}
DYN200_API int dyn200_port_name(uint32_t index, char *name, size_t name_size, char *desc, size_t desc_size)
{
        int rc;
        lock_sdk();
        refresh_ports_unlocked(0);
        if (index >= g_port_total) {
                rc = set_error(DYN200_ERR_ARGUMENT,
                               g_port_total ? "serial port index out of range" : "no serial port found");
                set_text(name, name_size, "");
                set_text(desc, desc_size, "");
        } else {
                set_text(name, name_size, g_port_names[index]);
                set_text(desc, desc_size, g_port_descs[index]);
                rc = set_ok();
        }
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_set_decimals(int torque, int speed, int power)
{
        int rc; lock_sdk();
        if (torque < 0 || torque > 6 || speed < 0 || speed > 6 || power < 0 || power > 6)
                rc = set_error(DYN200_ERR_ARGUMENT, "decimal counts must be 0..6");
        else { g_torque_decimals = torque; g_speed_decimals = speed; g_power_decimals = power; rc = set_ok(); }
        unlock_sdk(); return rc;
}

/**
 * @brief Modbus 轮询：一次取回扭矩/转速/功率三组定点数; 调用者必须已持有锁
 */
static int read_modbus_unlocked(Dyn200Measurement *m)
{
        uint16_t w[6];
        int rc;
        if (!m) return set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        rc = read_registers_unlocked(DYN200_REG_TORQUE, 6, w);
        if (rc) return rc;
        m->torque_raw = (int32_t)(((uint32_t)w[0] << 16) | w[1]);
        m->speed_raw = (int32_t)(((uint32_t)w[2] << 16) | w[3]);
        m->power_raw = (int32_t)(((uint32_t)w[4] << 16) | w[5]);
        m->torque_nm = scale_i32(m->torque_raw, g_torque_decimals);
        m->speed_rpm = scale_i32(m->speed_raw, g_speed_decimals);
        m->power_kw = scale_i32(m->power_raw, g_power_decimals);
        g_measurement = *m;
        return set_ok();
}

DYN200_API int dyn200_read(Dyn200Measurement *m)
{
        int rc;
        lock_sdk();
        rc = read_modbus_unlocked(m);
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_read_registers(uint16_t a, uint16_t n, uint16_t *v) { int rc; lock_sdk(); rc = read_registers_unlocked(a,n,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_read_i32(uint16_t a, int32_t *v) { int rc; lock_sdk(); rc = read_i32_unlocked(a,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_write_i32(uint16_t a, int32_t v) { int rc; lock_sdk(); rc = write_i32_unlocked(a,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_zero(void) { int rc; lock_sdk(); rc = command_unlocked(DYN200_ZERO_ADDRESS); unlock_sdk(); return rc; }
DYN200_API int dyn200_factory_reset(void) { int rc; lock_sdk(); rc = command_unlocked(DYN200_FACTORY_RESET_ADDRESS); unlock_sdk(); return rc; }
DYN200_API int dyn200_set_filter(int v) { int rc; lock_sdk(); rc = v < 1 || v > 100 ? set_error(DYN200_ERR_ARGUMENT,"filter must be 1..100") : write_i32_unlocked(DYN200_REG_FILTER,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_set_zero_on_boot(int v) { int rc; lock_sdk(); rc = write_i32_unlocked(DYN200_REG_ZERO_ON_BOOT,v != 0); unlock_sdk(); return rc; }
DYN200_API int dyn200_set_torque_direction(int v) { int rc; lock_sdk(); rc = write_i32_unlocked(DYN200_REG_TORQUE_DIRECTION,v != 0); unlock_sdk(); return rc; }

DYN200_API int dyn200_decode_active6(const uint8_t frame[6], int32_t *torque, int32_t *speed)
{
        uint16_t t, s;
        if (!frame || !torque || !speed) return set_error(DYN200_ERR_ARGUMENT, "active-frame argument is null");
        if (!valid_crc(frame, 6)) return set_error(DYN200_ERR_CRC, "active-frame CRC mismatch");
        t = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
        s = (uint16_t)(((uint16_t)frame[2] << 8) | frame[3]);
        *torque = (int32_t)t;
        *speed = (int32_t)(s & 0x7fffu);
        if (s & 0x8000u) *torque = -*torque;
        return set_ok();
}
DYN200_API int dyn200_decode_active8(const uint8_t frame[8], int32_t *torque, uint32_t *speed)
{
        uint32_t t;
        if (!frame || !torque || !speed) return set_error(DYN200_ERR_ARGUMENT, "active-frame argument is null");
        if (!valid_crc(frame, 8)) return set_error(DYN200_ERR_CRC, "active-frame CRC mismatch");
        t = ((uint32_t)frame[0] << 16) | ((uint32_t)frame[1] << 8) | frame[2];
        if (t & 0x800000u) t |= 0xff000000u;
        *torque = (int32_t)t;
        *speed = ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 8) | frame[5];
        return set_ok();
}

/**
 * @brief 从字节流里取最新一帧：从末尾往前扫，第一个 CRC 正确的窗口就是最新的帧
 *        主动上传流里可能夹着半帧、噪声、以及"上一帧的尾巴"，这种扫法天然能重新
 *        对齐，而且永远给调用方最新的数据（不会越读越落后）
 */
DYN200_API int dyn200_scan_active_frame(const uint8_t *data, size_t size, int frame_bytes, size_t *offset)
{
        if (!data || !offset || (frame_bytes != 6 && frame_bytes != 8) || size < (size_t)frame_bytes)
                return 0;
        for (size_t i = size - (size_t)frame_bytes + 1; i-- > 0;) {
                if (valid_crc(data + i, (size_t)frame_bytes)) {
                        *offset = i;
                        return 1;
                }
        }
        return 0;
}

/**
 * @brief 记下最近收到有效帧的时间，判断流是否停了
 */
static void note_active_frame_unlocked(void)
{
        const uint32_t now = monotonic_ms();
        g_last_active_frame_ms = now ? now : 1u;
}

/**
 * @brief 从 data 开头找第一帧完整帧（按到达顺序逐帧消费，不能像"取最新"那样从尾部找）
 */
static int active_scan_first_frame(const uint8_t *data, size_t size, int frame_bytes, size_t *offset)
{
        if (!data || !offset || (frame_bytes != 6 && frame_bytes != 8) || size < (size_t)frame_bytes)
                return 0;
        for (size_t i = 0; i + (size_t)frame_bytes <= size; ++i) {
                if (valid_crc(data + i, (size_t)frame_bytes)) {
                        *offset = i;
                        return 1;
                }
        }
        return 0;
}

/**
 * @brief 把已经消费掉的前缀压掉，给新字节腾空间
 */
static void active_compact_unlocked(void)
{
        if (g_active_buf_pos > 0) {
                if (g_active_buf_len > g_active_buf_pos)
                        memmove(g_active_buf,
                                g_active_buf + g_active_buf_pos,
                                g_active_buf_len - g_active_buf_pos);
                g_active_buf_len -= g_active_buf_pos;
                g_active_buf_pos = 0;
        }
}

/**
 * @brief 从缓冲里取下一帧：找到就交出去并把消费游标前移；找不到返回 TIMEOUT
 */
static int active_next_frame_unlocked(uint8_t *frame, size_t frame_size)
{
        size_t offset = 0;
        if (g_active_buf_len - g_active_buf_pos < frame_size)
                return set_error(DYN200_ERR_TIMEOUT, "no new active frame");
        if (!active_scan_first_frame(g_active_buf + g_active_buf_pos,
                                     g_active_buf_len - g_active_buf_pos,
                                     (int)frame_size,
                                     &offset)) {
                /* Only the last frame_size-1 bytes can become a future complete frame.
                 * Drop scanned noise so a full buffer cannot permanently stall recovery. */
                g_active_buf_pos = g_active_buf_len - (frame_size - 1u);
                return set_error(DYN200_ERR_TIMEOUT, "no new active frame");
        }
        memcpy(frame, g_active_buf + g_active_buf_pos + offset, frame_size);
        g_active_buf_pos += offset + frame_size;
        note_active_frame_unlocked();
        return set_ok();
}

static int read_active_frame_unlocked(uint8_t *frame, size_t frame_size)
{
        const uint32_t deadline = monotonic_ms() + (uint32_t)(g_timeout_ms > 0 ? g_timeout_ms : 200);

        if (!port_is_open())
                return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open");
        while ((int32_t)(deadline - monotonic_ms()) > 0) {
                // 缓冲里已经有完整帧时一次只交一帧，余下的留给后续调用，绝不丢弃
                if (active_next_frame_unlocked(frame, frame_size) == DYN200_OK)
                        return DYN200_OK;
                // Compact only when refilling, not once per delivered frame
                active_compact_unlocked();
                const int32_t remain = (int32_t)(deadline - monotonic_ms());
                if (remain <= 0)
                        break;
                size_t got = 0;
                const int rc = read_active_bytes_unlocked(g_active_buf + g_active_buf_len,
                                                          sizeof(g_active_buf) - g_active_buf_len,
                                                          &got, (uint32_t)remain);
                if (rc != DYN200_OK)
                        return rc;
                g_active_buf_len += got;
                // A complete frame received at the deadline still belongs to this call
                if (got > 0 && active_next_frame_unlocked(frame, frame_size) == DYN200_OK)
                        return DYN200_OK;
        }
        return g_active_buf_len > g_active_buf_pos
                   ? set_error(DYN200_ERR_CRC, "cannot synchronize active stream CRC")
                   : set_error(DYN200_ERR_TIMEOUT, "active stream timeout");
}

/**
 * @brief 非阻塞显示模式：取调用时已排队字节中的最新完整帧，保留尾部半帧
 *        快照限定工作量，持续到来的流不会让一次显示读取无限循环
 */
static int read_active_latest_unlocked(uint8_t *frame, size_t frame_size)
{
        int found = 0;
        if (!port_is_open())
                return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open");
        size_t pending = bytes_available_unlocked();
        for (;;) {
                size_t offset = 0;
                const size_t unread = g_active_buf_len - g_active_buf_pos;
                if (dyn200_scan_active_frame(g_active_buf + g_active_buf_pos, unread, (int)frame_size, &offset)) {
                        memcpy(frame, g_active_buf + g_active_buf_pos + offset, frame_size);
                        g_active_buf_pos += offset + frame_size;
                        found = 1;
                } else if (unread >= frame_size) {
                        g_active_buf_pos = g_active_buf_len - (frame_size - 1u);
                }
                if (pending == 0)
                        break;
                active_compact_unlocked();
                size_t take = sizeof(g_active_buf) - g_active_buf_len;
                if (take > pending)
                        take = pending;
                size_t got = 0;
                const int rc = read_active_bytes_unlocked(g_active_buf + g_active_buf_len, take, &got, 0);
                if (rc != DYN200_OK)
                        return rc;
                g_active_buf_len += got;
                pending -= got;
                if (got == 0)
                        break;
        }
        if (!found)
                return set_error(DYN200_ERR_TIMEOUT, "no new active frame");
        note_active_frame_unlocked();
        return set_ok();
}

static void fill_from_frame6_unlocked(Dyn200Measurement *m, const uint8_t frame[6])
{
        int32_t torque = 0, speed = 0;
        if (dyn200_decode_active6(frame, &torque, &speed) != 0)
                return;
        m->torque_raw = torque;
        m->speed_raw = speed;
        m->power_raw = 0;
        m->torque_nm = scale_i32(torque, g_torque_decimals);
        m->speed_rpm = scale_i32(speed, g_speed_decimals);
        m->power_kw = m->torque_nm * m->speed_rpm * 0.00010471975511965977;
        g_measurement = *m;
}

static void fill_from_frame8_unlocked(Dyn200Measurement *m, const uint8_t frame[8])
{
        int32_t  torque = 0;
        uint32_t speed = 0;
        if (dyn200_decode_active8(frame, &torque, &speed) != 0)
                return;
        m->torque_raw = torque;
        m->speed_raw = (int32_t)speed;
        m->power_raw = 0;
        m->torque_nm = scale_i32(torque, g_torque_decimals);
        m->speed_rpm = scale_i32((int32_t)speed, g_speed_decimals);
        m->power_kw = m->torque_nm * m->speed_rpm * 0.00010471975511965977;
        g_measurement = *m;
}

static int read_active6_unlocked(Dyn200Measurement *m)
{
        uint8_t frame[6];
        int rc;
        if (!m) return set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        if ((rc = read_active_frame_unlocked(frame, sizeof(frame))) != 0) return rc;
        fill_from_frame6_unlocked(m, frame);
        return set_ok();
}
static int read_active8_unlocked(Dyn200Measurement *m)
{
        uint8_t frame[8];
        int rc;
        if (!m) return set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        if ((rc = read_active_frame_unlocked(frame, sizeof(frame))) != 0) return rc;
        fill_from_frame8_unlocked(m, frame);
        return set_ok();
}
DYN200_API int dyn200_read_active6(Dyn200Measurement *m)
{
        int rc;
        lock_sdk();
        rc = read_active6_unlocked(m);
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_read_active8(Dyn200Measurement *m)
{
        int rc;
        lock_sdk();
        rc = read_active8_unlocked(m);
        unlock_sdk();
        return rc;
}

/**
 * @brief 按当前通讯模式取一帧（Modbus / 主动上传 0 / 主动上传 3）
 */
static int read_sample_unlocked(Dyn200Measurement *m)
{
        int rc = ensure_ready_unlocked();
        if (rc != DYN200_OK) return rc;
        if (g_protocol == 6) return read_active6_unlocked(m);
        if (g_protocol == 8) return read_active8_unlocked(m);
        return read_modbus_unlocked(m);
}

DYN200_API int dyn200_update(void)
{
        Dyn200Measurement measurement;
        int rc;
        lock_sdk();
        rc = read_sample_unlocked(&measurement);
        unlock_sdk();
        return rc;
}
static double cached_value(int field)
{
        double val;
        lock_sdk();
        val = field == 0 ? g_measurement.torque_nm : field == 1 ? g_measurement.speed_rpm : g_measurement.power_kw;
        unlock_sdk();
        return val;
}
DYN200_API double dyn200_read_torque_nm(void) { return dyn200_update() ? NAN : dyn200_get_torque_nm(); }
DYN200_API double dyn200_read_speed_rpm(void) { return dyn200_update() ? NAN : dyn200_get_speed_rpm(); }
DYN200_API double dyn200_read_power_kw(void) { return dyn200_update() ? NAN : dyn200_get_power_kw(); }
DYN200_API double dyn200_get_torque_nm(void) { return cached_value(0); }
DYN200_API double dyn200_get_speed_rpm(void) { return cached_value(1); }
DYN200_API double dyn200_get_power_kw(void) { return cached_value(2); }
DYN200_API float dyn200_read_torque_f32(void) { return (float)dyn200_read_torque_nm(); }
DYN200_API float dyn200_read_speed_f32(void) { return (float)dyn200_read_speed_rpm(); }
DYN200_API float dyn200_read_power_f32(void) { return (float)dyn200_read_power_kw(); }
DYN200_API float dyn200_get_torque_f32(void) { return (float)dyn200_get_torque_nm(); }
DYN200_API float dyn200_get_speed_f32(void) { return (float)dyn200_get_speed_rpm(); }
DYN200_API float dyn200_get_power_f32(void) { return (float)dyn200_get_power_kw(); }
DYN200_API int32_t dyn200_read_torque_raw(void)
{
        return dyn200_update() ? INT32_MIN : dyn200_get_torque_raw();
}
DYN200_API int32_t dyn200_read_speed_raw(void)
{
        return dyn200_update() ? INT32_MIN : dyn200_get_speed_raw();
}
DYN200_API int32_t dyn200_read_power_raw(void)
{
        return dyn200_update() ? INT32_MIN : dyn200_get_power_raw();
}
DYN200_API int32_t dyn200_get_torque_raw(void)
{
        int32_t val; lock_sdk(); val = g_measurement.torque_raw; unlock_sdk(); return val;
}
DYN200_API int32_t dyn200_get_speed_raw(void)
{
        int32_t val; lock_sdk(); val = g_measurement.speed_raw; unlock_sdk(); return val;
}
DYN200_API int32_t dyn200_get_power_raw(void)
{
        int32_t val; lock_sdk(); val = g_measurement.power_raw; unlock_sdk(); return val;
}
DYN200_API int dyn200_last_error_code(void) { return g_error_code; }
DYN200_API const char *dyn200_last_error(void) { return g_error; }

// ═══ ava_tool 数据表 ══════════════════════════════════════════════════════

/**
 * @brief "0 即默认值"的字段归一化; 归一化后的值会被面板写回表格，用户看到的就是实际参数
 */
static void normalize_port_unlocked(dyn200_port_param *param)
{
        if (param->protocol != DYN200_PROTOCOL_ACTIVE6 && param->protocol != DYN200_PROTOCOL_ACTIVE8)
                param->protocol = DYN200_PROTOCOL_MODBUS;
        param->baud_rate = or_default_u32(param->baud_rate, 9600);
        param->stop_bits = or_default_u32(param->stop_bits, 1);
        /*
         * 默认超时给 200 ms：9600 下一次完整读写线上只要 ~26 ms，200 ms 足够宽松，
         * 但丢了帧也只卡 200 ms —— 默认 1000 ms 时，偶尔丢一帧就会把平均采样率
         * 从几十 Hz 拖到几 Hz。
         */
        param->timeout_ms = or_default_u32(param->timeout_ms, 200);
        param->device_address = or_default_u32(param->device_address, 1);
        if (param->protocol != DYN200_PROTOCOL_MODBUS)
                param->device_address = 1; // 主动上传是单向字节流，不使用机码
}

/**
 * @brief 把当前连接状态和 index 指向的串口写进反馈字段
 */
static void fill_port_feedback_unlocked(dyn200_port_param *param)
{
        param->count = g_port_total;
        if (param->index < g_port_total) {
                set_text(param->name, sizeof(param->name), g_port_names[param->index]);
                set_text(param->desc, sizeof(param->desc), g_port_descs[param->index]);
        } else if (g_port_total > 0) {
                set_text(param->name, sizeof(param->name), "");
                set_text(param->desc, sizeof(param->desc), "");
        }
        // 没手填过 port 就默认选中 index 那一个（之后翻列表不会把它清掉）
        if (!param->port[0] && param->name[0])
                set_text(param->port, sizeof(param->port), param->name);
        param->connected = port_is_open() ? 1u : 0u;
        set_text(param->active_port, sizeof(param->active_port), g_port_name);
        param->active_protocol = (uint32_t)g_protocol;
        param->active_baud_rate = (uint32_t)g_baud_rate;
        param->active_device_address = (uint32_t)g_address;
        param->last_error_code = g_error_code;
        set_text(param->last_error, sizeof(param->last_error), g_error);
}

/**
 * @brief 这张表当前选中的口：手填的 port 优先，否则用 index 指向的枚举结果
 */
static const char *selected_port_unlocked(const dyn200_port_param *param)
{
        if (param->port[0])
                return param->port;
        if (param->index < g_port_total)
                return g_port_names[param->index];
        return NULL;
}

/**
 * @brief 连着的口还在，只是波特率之类参数被改了：自动按新参数重连同一个口
 */
static int active_settings_differ_unlocked(const dyn200_port_param *param)
{
        if (!port_is_open())
                return 0;
        return (int)param->baud_rate != g_baud_rate || (int)param->protocol != g_protocol ||
               (int)param->device_address != (int)g_address || (int)param->stop_bits != g_stop_bits ||
               (int)param->timeout_ms != g_timeout_ms;
}

/**
 * @brief 把当前选中的口连上（必要的话先按新参数重连）。force 表示用户明确要求连接：
 *        会解除 DISCONNECT 的"先不连"状态，也不受失败节流限制
 */
static int connect_selected_unlocked(const dyn200_port_param *param, int force)
{
        const char *port = selected_port_unlocked(param);
        if (!port || !*port)
                return set_error(DYN200_ERR_ARGUMENT,
                                 "no serial port selected: read the list, then set index or type port");

        const int port_changed = !g_link_valid || strcmp(g_link_port, port) != 0;
        remember_link_unlocked(port, (int)param->protocol, (int)param->baud_rate, (int)param->device_address,
                               (int)param->stop_bits, (int)param->timeout_ms);
        if (port_changed || force)
                g_link_hold_closed = 0;
        if (port_is_open() && strcmp(port, g_port_name) == 0 && !active_settings_differ_unlocked(param))
                return set_ok();
        if (g_link_hold_closed && !force)
                return set_ok(); // 用户按过 DISCONNECT：保留参数，保持断开
        if (force)
                g_link_retry_ms = 0;
        if (port_is_open())
                return ensure_open_unlocked(port, (int)param->protocol, (int)param->baud_rate,
                                            (int)param->device_address, (int)param->stop_bits,
                                            (int)param->timeout_ms);
        return ensure_ready_unlocked();
}

/**
 * @brief 串口表的一次性动作：搜索 / 连接 / 断开; 读取和显式调用都走这里
 */
static int apply_port_action_unlocked(dyn200_port_param *param, int action)
{
        switch (action) {
        case DYN200_PORT_DISCONNECT:
                port_close();
                // 只是先不连：参数留着，之后 CONNECT 或者换个口名就会重新连上
                g_link_hold_closed = 1;
                return set_ok();
        case DYN200_PORT_RESCAN:
                refresh_ports_unlocked(1);
                return set_ok();
        case DYN200_PORT_CONNECT:
                return connect_selected_unlocked(param, 1);
        case DYN200_PORT_NONE:
        default:
                // 默认（没有动作）：把选中的口连上，参数变了就按新参数重连
                return connect_selected_unlocked(param, 0);
        }
}

/**
 * @brief 串口表：面板里这是只读表，读一次就是"搜索 + 连接"
 *        搜索：枚举本机串口，回填 count/name/desc，并把选中的口默认填进 port
 *        连接：按选中的口和参数把串口打开（DISCONNECT 之后就保持断开）
 */
DYN200_API int dyn200_read_port_param(dyn200_port_param *param)
{
        if (!param)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        normalize_port_unlocked(param);
        const int action = (int)param->action;
        refresh_ports_unlocked(action == DYN200_PORT_RESCAN);
        fill_port_feedback_unlocked(param);
        const int rc = apply_port_action_unlocked(param, action);
        param->action = DYN200_PORT_NONE;
        fill_port_feedback_unlocked(param);
        unlock_sdk();
        return rc;
}

/**
 * @brief 同一个动作的显式入口，给程序/脚本用（函数名不带 param 后缀，所以面板不会
 *        把它当成"下发"，串口表在面板里保持只有搜索和连接）
 */
DYN200_API int dyn200_port_apply(dyn200_port_param *param)
{
        if (!param)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        normalize_port_unlocked(param);
        const int action = (int)param->action;
        refresh_ports_unlocked(action == DYN200_PORT_RESCAN);
        const int rc = apply_port_action_unlocked(param, action);
        param->action = DYN200_PORT_NONE;
        fill_port_feedback_unlocked(param);
        unlock_sdk();
        return rc;
}

static void normalize_param_unlocked(dyn200_param *param)
{
        if (param->scaling.torque_decimals == 0)
                param->scaling.torque_decimals = 1;
        if (param->scaling.torque_decimals > 6)
                param->scaling.torque_decimals = 6;
        if (param->scaling.speed_decimals > 6)
                param->scaling.speed_decimals = 6;
        if (param->scaling.power_decimals > 6)
                param->scaling.power_decimals = 6;
        g_torque_decimals = (int)param->scaling.torque_decimals;
        g_speed_decimals = (int)param->scaling.speed_decimals;
        g_power_decimals = (int)param->scaling.power_decimals;
}

static void fill_param_feedback_unlocked(dyn200_param *param)
{
        param->connected = port_is_open() ? 1u : 0u;
        // 连接信息放这里（实时表只放高频数据）
        set_text(param->link.port, sizeof(param->link.port), g_port_name);
        param->link.protocol = (uint32_t)g_protocol;
        param->link.baud_rate = (uint32_t)g_baud_rate;
        param->last_error_code = g_error_code;
        set_text(param->last_error, sizeof(param->last_error), g_error);
}

/**
 * @brief 设备里存着的滤波/上电清零/方向; 机型不支持回读时返回错误
 */
static int read_device_state_unlocked(dyn200_device_state *state)
{
        int32_t filter, zero_on_boot, reversed;
        int rc;
        if ((rc = read_i32_unlocked(DYN200_REG_FILTER, &filter)) != 0) return rc;
        if ((rc = read_i32_unlocked(DYN200_REG_ZERO_ON_BOOT, &zero_on_boot)) != 0) return rc;
        if ((rc = read_i32_unlocked(DYN200_REG_TORQUE_DIRECTION, &reversed)) != 0) return rc;
        state->filter = filter;
        state->zero_on_boot = zero_on_boot;
        state->reversed = reversed;
        return set_ok();
}

/**
 * @brief 下发被门控置位的设置组。每组都有自己的门控，改一项只会写一项；
 *        都需要串口已打开且是 Modbus 模式（写寄存器）
 */
static int apply_settings_unlocked(const dyn200_param *param)
{
        int rc;
        if (!param->write_filter && !param->write_zero_on_boot && !param->write_direction)
                return DYN200_OK;
        // 先把要下发的值校验完，这样没接串口时也能报出参数错误
        if (param->write_filter && (param->filter.val < 1 || param->filter.val > 100))
                return set_error(DYN200_ERR_ARGUMENT, "filter must be 1..100");
        if ((rc = ensure_ready_unlocked()) != DYN200_OK)
                return rc;
        if (g_protocol != DYN200_PROTOCOL_MODBUS)
                return set_error(DYN200_ERR_PROTOCOL, "sensor settings need Modbus mode (protocol=1)");

        if (param->write_filter) {
                if ((rc = write_i32_unlocked(DYN200_REG_FILTER, param->filter.val)) != 0)
                        return rc;
        }
        if (param->write_zero_on_boot) {
                if ((rc = write_i32_unlocked(DYN200_REG_ZERO_ON_BOOT, param->zero_on_boot.enable ? 1 : 0)) != 0)
                        return rc;
        }
        if (param->write_direction) {
                if ((rc = write_i32_unlocked(DYN200_REG_TORQUE_DIRECTION, param->direction.reversed ? 1 : 0)) != 0)
                        return rc;
        }
        return DYN200_OK;
}

DYN200_API int dyn200_read_param(dyn200_param *param)
{
        if (!param)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        normalize_param_unlocked(param);

        /*
         * 连上以后顺手把设备里存的设置读回来（约 2 秒一次），这样表里 device.*
         * 和几个输入框显示的都是传感器的真实值。读不到的机型只试一次。
         */
        if (!g_readback_unsupported && port_is_open() && g_protocol == DYN200_PROTOCOL_MODBUS) {
                const uint32_t now = monotonic_ms();
                if (g_readback_ms == 0 || (uint32_t)(now - g_readback_ms) >= DYN200_PARAM_READBACK_MS) {
                        dyn200_device_state state;
                        g_readback_ms = now;
                        if (read_device_state_unlocked(&state) == DYN200_OK) {
                                param->device = state;
                                param->filter.val = state.filter;
                                param->zero_on_boot.enable = state.zero_on_boot ? 1 : 0;
                                param->direction.reversed = state.reversed ? 1 : 0;
                        } else {
                                g_readback_unsupported = 1;
                        }
                }
        }
        fill_param_feedback_unlocked(param);
        unlock_sdk();
        return DYN200_OK;
}

DYN200_API int dyn200_write_param(dyn200_param *param)
{
        int rc;
        if (!param)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        normalize_param_unlocked(param);
        rc = apply_settings_unlocked(param);

        const int applied = param->write_filter || param->write_zero_on_boot || param->write_direction;
        // 一次性门控：清掉后自动写入不会重复下发
        param->write_filter = 0;
        param->write_zero_on_boot = 0;
        param->write_direction = 0;

        if (rc == DYN200_OK && applied && !g_readback_unsupported && port_is_open() &&
            g_protocol == DYN200_PROTOCOL_MODBUS) {
                dyn200_device_state state;
                g_readback_ms = monotonic_ms();
                if (read_device_state_unlocked(&state) == DYN200_OK)
                        param->device = state;
        }
        fill_param_feedback_unlocked(param);
        unlock_sdk();
        return rc;
}

DYN200_API int dyn200_read_rt(dyn200_rt *rt)
{
        Dyn200Measurement measurement = {0};
        int               rc;
        if (!rt)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        rt->fresh = 0u;
        rc = ensure_ready_unlocked();
        const int active = (g_protocol == DYN200_PROTOCOL_ACTIVE6 || g_protocol == DYN200_PROTOCOL_ACTIVE8);
        /*
         * 默认（wait_fresh = 0）走非阻塞：面板/曲线按自己的频率出点，曲线连续；
         * 但还没拿到过任何一帧时仍然先等一帧，免得曲线开头冒一串 0。
         */
        if (rc == DYN200_OK && active && !rt->wait_fresh && rt->sample_count > 0 &&
            g_last_active_frame_ms != 0) {
                const size_t frame_size = g_protocol == DYN200_PROTOCOL_ACTIVE6 ? 6u : 8u;
                uint8_t       frame[8];
                /* Always inspect newly arrived bytes before declaring the stream stale,
                 * so a resumed stream can recover without reconnecting. */
                rc = read_active_latest_unlocked(frame, frame_size);
                if (rc == DYN200_OK) {
                        if (frame_size == 6)
                                fill_from_frame6_unlocked(&measurement, frame);
                        else
                                fill_from_frame8_unlocked(&measurement, frame);
                        rt->measurement = measurement;
                        ++rt->sample_count;
                        rt->fresh = 1u;
                } else if (rc == DYN200_ERR_TIMEOUT) {
                        /* USB chunk gaps are normal; errors other than 'no new frame'
                         * must remain visible instead of being converted to success. */
                        if ((uint32_t)(monotonic_ms() - g_last_active_frame_ms) > 1500u)
                                rc = set_error(DYN200_ERR_TIMEOUT, "active stream stopped: no new frame");
                        else
                                rc = set_ok();
                }
                rt->last_error_code = g_error_code;
                set_text(rt->last_error, sizeof(rt->last_error), g_error);
                unlock_sdk();
                return rc;
        }
        if (rc == DYN200_OK)
                rc = read_sample_unlocked(&measurement);
        if (rc == DYN200_OK) {
                rt->measurement = measurement;
                ++rt->sample_count;
                rt->fresh = 1u;
        }
        // 失败时保留上一帧的数值，避免监视器突然跳到 0
        rt->last_error_code = g_error_code;
        set_text(rt->last_error, sizeof(rt->last_error), g_error);
        unlock_sdk();
        return rc;
}

DYN200_API int dyn200_write_rt(dyn200_rt *rt)
{
        int rc = DYN200_OK;
        if (!rt)
                return DYN200_ERR_ARGUMENT;
        lock_sdk();
        if (rt->write_command) {
                if (rt->command.zero) {
                        rc = ensure_ready_unlocked();
                        if (rc == DYN200_OK)
                                rc = command_unlocked(DYN200_ZERO_ADDRESS);
                }
                if (rc == DYN200_OK && rt->command.factory_reset) {
                        rc = ensure_ready_unlocked();
                        if (rc == DYN200_OK)
                                rc = command_unlocked(DYN200_FACTORY_RESET_ADDRESS);
                }
                // 一次性命令：执行或失败后都清掉，自动写入不会重放
                rt->command.zero = 0;
                rt->command.factory_reset = 0;
                rt->write_command = 0;
        }
        rt->last_error_code = g_error_code;
        set_text(rt->last_error, sizeof(rt->last_error), g_error);
        unlock_sdk();
        return rc;
}
