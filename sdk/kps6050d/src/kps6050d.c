#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif
#include "kps6050d.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE           g_port = INVALID_HANDLE_VALUE;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static BOOL CALLBACK
init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
        (void)once;
        (void)param;
        (void)ctx;
        InitializeCriticalSection(&g_lock);
        return TRUE;
}
static void
lock_sdk(void)
{
        InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL);
        EnterCriticalSection(&g_lock);
}
static void
unlock_sdk(void)
{
        LeaveCriticalSection(&g_lock);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
static int             g_port = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void
lock_sdk(void)
{
        pthread_mutex_lock(&g_lock);
}
static void
unlock_sdk(void)
{
        pthread_mutex_unlock(&g_lock);
}
#endif

static uint8_t       g_id;
static int           g_error_code;
static char          g_error[256] = "OK";
static Kps6050dState g_state;
static int           g_big_endian;

static int
set_error(int code, const char *text)
{
        g_error_code = code;
        snprintf(g_error, sizeof(g_error), "%s", text);
        return code;
}
static int
set_ok(void)
{
        g_error_code = 0;
        snprintf(g_error, sizeof(g_error), "OK");
        return 0;
}
static uint16_t
crc16(const uint8_t *p, size_t n)
{
        uint16_t crc = 0xffff;
        int      i;
        while (n--) {
                crc ^= *p++;
                for (i = 0; i < 8; i++)
                        crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001) : (uint16_t)(crc >> 1);
        }
        return crc;
}
static void
add_crc(uint8_t *p, size_t n)
{
        uint16_t c = crc16(p, n);
        p[n]       = (uint8_t)c;
        p[n + 1]   = (uint8_t)(c >> 8);
}
static uint16_t
swap16(uint16_t v)
{
        return (uint16_t)((v << 8) | (v >> 8));
}

static int
port_is_open(void)
{
#if defined(_WIN32)
        return g_port != INVALID_HANDLE_VALUE;
#else
        return g_port >= 0;
#endif
}
static void
port_close(void)
{
#if defined(_WIN32)
        if (g_port != INVALID_HANDLE_VALUE)
                CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
#else
        if (g_port >= 0)
                close(g_port);
        g_port = -1;
#endif
}
static int
port_write_all(const uint8_t *p, size_t n)
{
        while (n) {
#if defined(_WIN32)
                DWORD done = 0;
                if (!WriteFile(g_port, p, (DWORD)n, &done, NULL) || !done)
                        return -1;
#else
                ssize_t done = write(g_port, p, n);
                if (done < 0 && errno == EINTR)
                        continue;
                if (done <= 0)
                        return -1;
#endif
                p += done;
                n -= (size_t)done;
        }
        return 0;
}
static int
port_read_all(uint8_t *p, size_t n)
{
        while (n) {
#if defined(_WIN32)
                DWORD done = 0;
                if (!ReadFile(g_port, p, (DWORD)n, &done, NULL))
                        return -1;
                if (!done)
                        return -2;
#else
                ssize_t done = read(g_port, p, n);
                if (done < 0 && errno == EINTR)
                        continue;
                if (done < 0)
                        return -1;
                if (!done)
                        return -2;
#endif
                p += done;
                n -= (size_t)done;
        }
        return 0;
}
static void
trace_frame(const char *name, const uint8_t *data, size_t size)
{
        size_t i;
        if (!getenv("KPS6050D_TRACE"))
                return;
        fprintf(stderr, "KPS6050D %s:", name);
        for (i = 0; i < size; ++i)
                fprintf(stderr, " %02X", data[i]);
        fputc('\n', stderr);
        fflush(stderr);
}
static int
transact(const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx)
{
        int rc;
        if (!port_is_open())
                return set_error(-4, "serial port is not open");
#if defined(_WIN32)
        PurgeComm(g_port, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        tcflush(g_port, TCIOFLUSH);
#endif
        trace_frame("TX", tx, ntx);
        if (port_write_all(tx, ntx))
                return set_error(-5, "serial write failed");
        rc = port_read_all(rx, nrx);
        if (rc == -2)
                return set_error(-6, "serial response timeout");
        if (rc)
                return set_error(-7, "serial read failed");
        trace_frame("RX", rx, nrx);
        if (crc16(rx, nrx - 2) != (uint16_t)(rx[nrx - 2] | ((uint16_t)rx[nrx - 1] << 8)))
                return set_error(-8, "Modbus CRC mismatch");
        if (rx[0] != g_id)
                return set_error(-7, "unexpected device ID");
        if (rx[1] & 0x80)
                return set_error(-9, "Modbus exception response");
        return 0;
}
static int
read_unlocked(Kps6050dState *s)
{
        uint8_t  tx[8] = {g_id, 3, 0, 0, 0, 8, 0, 0}, rx[21] = {0};
        uint16_t r[8];
        uint8_t  status;
        int      big, i, rc;
        float    vs, cs;
        add_crc(tx, 6);
        rc = transact(tx, sizeof(tx), rx, sizeof(rx));
        if (rc)
                return rc;
        if (rx[1] != 3 || rx[2] != 16)
                return set_error(-7, "invalid read response");
        for (i = 0; i < 8; i++)
                r[i] = (uint16_t)(((uint16_t)rx[3 + i * 2] << 8) | rx[4 + i * 2]);
        status = (uint8_t)(r[0] >> 8);
        big         = (status & 8) != 0;
        g_big_endian = big;
#define REG(v) (big ? (v) : swap16(v))
        vs                  = (r[0] & 0x80) ? 0.1f : 0.01f;
        /*
         * KPS6050D encodes current in 0.01 A units. For example, the
         * register value 0x01F4 is 500, i.e. 5.00 A.
         */
        cs                  = 0.01f;
        s->output_on        = (status & 1) != 0;
        s->ocp_on           = (status & 2) != 0;
        s->remote_locked    = (status & 4) != 0;
        s->constant_current = (status & 0x10) != 0;
        s->alarm            = (status & 0x20) != 0;
        s->voltage          = REG(r[2]) * vs;
        s->current          = REG(r[3]) * cs;
        s->set_voltage      = REG(r[4]) * vs;
        s->set_current      = REG(r[5]) * cs;
        s->max_voltage      = REG(r[6]) * vs;
        s->max_current      = REG(r[7]) * cs;
#undef REG
        g_state = *s;
        return set_ok();
}
static int
write_unlocked(const Kps6050dState *s)
{
        uint8_t  flags = (uint8_t)((s->output_on ? 1 : 0) | (s->ocp_on ? 2 : 0) |
                                   (s->remote_locked ? 4 : 0) | (g_big_endian ? 8 : 0));
        uint16_t v[3];
        uint8_t  tx[15] = {g_id, 0x10, 0, 0, 0, 3, 6}, rx[8] = {0};
        int      i, rc;
        v[0] = (uint16_t)(flags << 8);
        v[1] = (uint16_t)lroundf(s->set_voltage * 100.0f);
        v[2] = (uint16_t)lroundf(s->set_current * 100.0f);
        if (!g_big_endian) {
                v[1] = swap16(v[1]);
                v[2] = swap16(v[2]);
        }
        for (i = 0; i < 3; i++) {
                tx[7 + i * 2] = (uint8_t)(v[i] >> 8);
                tx[8 + i * 2] = (uint8_t)v[i];
        }
        add_crc(tx, 13);
        rc = transact(tx, sizeof(tx), rx, sizeof(rx));
        if (rc)
                return rc;
        if (rx[1] != 0x10 || rx[5] != 3)
                return set_error(-7, "invalid write response");
        g_state = *s;
        return set_ok();
}
static int
update_unlocked(int field, int value, float fvalue)
{
        Kps6050dState s;
        int           rc = read_unlocked(&s);
        if (rc)
                return rc;
        if (field == 0)
                s.set_voltage = fvalue;
        else if (field == 1)
                s.set_current = fvalue;
        else if (field == 2)
                s.output_on = value != 0;
        else if (field == 3)
                s.ocp_on = value != 0;
        else
                s.remote_locked = value != 0;
        return write_unlocked(&s);
}

KPS_API int
kps6050d_open(const char *port, int baud, int id)
{
        int rc;
        lock_sdk();
        if (!port || !*port || id < 0 || id > 31 || (baud != 2400 && baud != 4800 && baud != 9600 && baud != 19200)) {
                rc = set_error(-1, "invalid connection settings");
                goto done;
        }
        port_close();
#if defined(_WIN32)
        {
                char         path[64];
                DCB          dcb = {0};
                COMMTIMEOUTS t   = {MAXDWORD, 0, 500, 0, 500};
                snprintf(path, sizeof(path), "\\\\.\\%s", port);
                g_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (g_port == INVALID_HANDLE_VALUE) {
                        rc = set_error(-2, "cannot open RS232 port");
                        goto done;
                }
                dcb.DCBlength = sizeof(dcb);
                GetCommState(g_port, &dcb);
                dcb.BaudRate     = (DWORD)baud;
                dcb.ByteSize     = 8;
                dcb.Parity       = NOPARITY;
                dcb.StopBits     = ONESTOPBIT;
                dcb.fBinary      = TRUE;
                dcb.fParity      = FALSE;
                dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
                dcb.fDtrControl                     = DTR_CONTROL_DISABLE;
                dcb.fRtsControl                     = RTS_CONTROL_DISABLE;
                dcb.fOutX = dcb.fInX = FALSE;
                if (!SetCommState(g_port, &dcb) || !SetCommTimeouts(g_port, &t)) {
                        port_close();
                        rc = set_error(-3, "cannot configure RS232");
                        goto done;
                }
        }
#else
        {
                struct termios t;
                speed_t        speed = baud == 2400 ? B2400 : baud == 4800 ? B4800 : baud == 9600 ? B9600 : B19200;
                g_port               = open(port, O_RDWR | O_NOCTTY);
                if (g_port < 0) {
                        rc = set_error(-2, "cannot open RS232 port");
                        goto done;
                }
                if (tcgetattr(g_port, &t)) {
                        port_close();
                        rc = set_error(-3, "cannot configure RS232");
                        goto done;
                }
                cfmakeraw(&t);
                cfsetispeed(&t, speed);
                cfsetospeed(&t, speed);
                t.c_cflag     |= CLOCAL | CREAD;
                t.c_cflag     &= ~CSTOPB;
                t.c_cflag     &= ~PARENB;
                t.c_cflag      = (t.c_cflag & ~CSIZE) | CS8;
                t.c_cc[VMIN]   = 0;
                t.c_cc[VTIME]  = 5;
                if (tcsetattr(g_port, TCSANOW, &t)) {
                        port_close();
                        rc = set_error(-3, "cannot configure RS232");
                        goto done;
                }
        }
#endif
        g_id = (uint8_t)id;
        rc   = read_unlocked(&g_state);
        if (rc)
                port_close();
done:
        unlock_sdk();
        return rc;
}
KPS_API int
kps6050d_close(void)
{
        lock_sdk();
        port_close();
        set_ok();
        unlock_sdk();
        return 0;
}
KPS_API int
kps6050d_is_open(void)
{
        int r;
        lock_sdk();
        r = port_is_open();
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_read(Kps6050dState *s)
{
        int r;
        lock_sdk();
        r = s ? read_unlocked(s) : set_error(-1, "state pointer is null");
        unlock_sdk();
        return r;
}
KPS_API float
kps6050d_get_voltage(void)
{
        Kps6050dState s;
        float         v;
        lock_sdk();
        v = read_unlocked(&s) ? -1.0f : s.voltage;
        unlock_sdk();
        return v;
}
KPS_API float
kps6050d_get_current(void)
{
        Kps6050dState s;
        float         v;
        lock_sdk();
        v = read_unlocked(&s) ? -1.0f : s.current;
        unlock_sdk();
        return v;
}
KPS_API int
kps6050d_set_voltage(float v)
{
        int r;
        lock_sdk();
        r = !isfinite(v) || v < 0 || v > 60 ? set_error(-1, "voltage must be 0..60 V") : update_unlocked(0, 0, v);
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_set_current(float v)
{
        int r;
        lock_sdk();
        r = !isfinite(v) || v < 0 || v > 50 ? set_error(-1, "current must be 0..50 A") : update_unlocked(1, 0, v);
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_set_output(int v)
{
        int r;
        lock_sdk();
        r = update_unlocked(2, v, 0);
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_set_ocp(int v)
{
        int r;
        lock_sdk();
        r = update_unlocked(3, v, 0);
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_set_remote_lock(int v)
{
        int r;
        lock_sdk();
        r = update_unlocked(4, v, 0);
        unlock_sdk();
        return r;
}
KPS_API int
kps6050d_last_error_code(void)
{
        return g_error_code;
}
KPS_API const char *
kps6050d_last_error(void)
{
        return g_error;
}








