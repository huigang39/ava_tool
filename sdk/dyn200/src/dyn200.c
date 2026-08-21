#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif
#include "dyn200.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE g_port = INVALID_HANDLE_VALUE;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
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
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
static int g_port = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_sdk(void) { pthread_mutex_lock(&g_lock); }
static void unlock_sdk(void) { pthread_mutex_unlock(&g_lock); }
#endif

static uint8_t g_address = 1;
static int g_error_code;
static char g_error[256] = "OK";
static int g_torque_decimals = 2;
static int g_speed_decimals;
static int g_power_decimals;
static int g_protocol = 1; /* 1=Modbus, 6=active mode 0, 8=active mode 3 */
static Dyn200Measurement g_measurement;

static int set_error(int code, const char *message)
{
        g_error_code = code;
        snprintf(g_error, sizeof(g_error), "%s", message);
        return code;
}
static int set_ok(void) { return set_error(DYN200_OK, "OK"); }

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
#if defined(_WIN32)
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
static int read_all(uint8_t *data, size_t size)
{
        while (size) {
#if defined(_WIN32)
                DWORD done = 0;
                if (!ReadFile(g_port, data, (DWORD)size, &done, NULL)) return -1;
                if (!done) return -2;
#else
                ssize_t done = read(g_port, data, size);
                if (done < 0 && errno == EINTR) continue;
                if (done < 0) return -1;
                if (!done) return -2;
#endif
                data += done;
                size -= (size_t)done;
        }
        return 0;
}
static int transact(const uint8_t *request, size_t request_size, uint8_t *response, size_t response_size)
{
        int rc;
        size_t actual_size;
        if (!port_is_open()) return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open");
#if defined(_WIN32)
        PurgeComm(g_port, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        tcflush(g_port, TCIOFLUSH);
#endif
        if (write_all(request, request_size)) return set_error(DYN200_ERR_WRITE, "serial write failed");
        if (response_size < 5) return set_error(DYN200_ERR_ARGUMENT, "response buffer is too small");
        rc = read_all(response, 3);
        if (rc == -2) return set_error(DYN200_ERR_TIMEOUT, "serial response timeout");
        if (rc) return set_error(DYN200_ERR_READ, "serial read failed");
        if (response[0] != g_address) {
                snprintf(g_error, sizeof(g_error), "unexpected device address: expected %u, received %u",
                         (unsigned)g_address, (unsigned)response[0]);
                g_error_code = DYN200_ERR_PROTOCOL;
                return DYN200_ERR_PROTOCOL;
        }
        actual_size = (response[1] & 0x80u) ? 5u : response_size;
        rc = read_all(response + 3, actual_size - 3);
        if (rc == -2) return set_error(DYN200_ERR_TIMEOUT, "serial response timeout");
        if (rc) return set_error(DYN200_ERR_READ, "serial read failed");
        if (!valid_crc(response, actual_size)) return set_error(DYN200_ERR_CRC, "Modbus CRC mismatch");
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
static int read_i32_unlocked(uint16_t address, int32_t *value)
{
        uint16_t words[2];
        int rc;
        if (!value) return set_error(DYN200_ERR_ARGUMENT, "value pointer is null");
        rc = read_registers_unlocked(address, 2, words);
        if (rc) return rc;
        *value = (int32_t)(((uint32_t)words[0] << 16) | words[1]);
        return set_ok();
}
static int write_i32_unlocked(uint16_t address, int32_t value)
{
        uint8_t tx[13], rx[8];
        uint32_t raw = (uint32_t)value;
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
static double scale_i32(int32_t value, int decimals)
{
        static const double powers[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0};
        return (double)value / powers[decimals];
}

DYN200_API int dyn200_open_ex(const char *port, int baud, int address, int timeout_ms, int stop_bits)
{
        int rc = 0;
        lock_sdk();
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
#if defined(_WIN32)
        {
                char path[128]; DCB dcb = {0}; COMMTIMEOUTS timeouts = {MAXDWORD, 0, 0, 0, 0};
                snprintf(path, sizeof(path), "\\\\.\\%s", port);
                g_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (g_port == INVALID_HANDLE_VALUE) { rc = set_error(DYN200_ERR_OPEN, "cannot open serial port"); goto done; }
                dcb.DCBlength = sizeof(dcb);
                if (!GetCommState(g_port, &dcb)) { port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot read serial settings"); goto done; }
                dcb.BaudRate = (DWORD)baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY;
                dcb.StopBits = stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
                dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
                dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE; dcb.fOutX = dcb.fInX = FALSE;
                timeouts.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
                timeouts.WriteTotalTimeoutConstant = (DWORD)timeout_ms;
                if (!SetCommState(g_port, &dcb) || !SetCommTimeouts(g_port, &timeouts)) {
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
                settings.c_cc[VMIN] = 0; settings.c_cc[VTIME] = (cc_t)((timeout_ms + 99) / 100 > 255 ? 255 : (timeout_ms + 99) / 100);
                if (tcsetattr(g_port, TCSANOW, &settings)) { port_close(); rc = set_error(DYN200_ERR_CONFIGURE, "cannot configure serial port"); goto done; }
        }
#endif
        g_address = (uint8_t)address; g_protocol = 1; rc = set_ok();
done:   unlock_sdk(); return rc;
}
DYN200_API int dyn200_open(const char *port, int baud, int address, int timeout_ms)
{
        return dyn200_open_ex(port, baud, address, timeout_ms, 1);
}
DYN200_API int dyn200_open_active(const char *port, int baud, int timeout_ms, int stop_bits)
{
        int rc;
        /* The address is unused while receiving an unsolicited byte stream. */
        rc = dyn200_open_ex(port, baud, 1, timeout_ms, stop_bits);
        if (!rc) {
                lock_sdk();
                g_protocol = 6;
                unlock_sdk();
        }
        return rc;
}
DYN200_API int dyn200_open_active8(const char *port, int baud, int timeout_ms, int stop_bits)
{
        int rc = dyn200_open_ex(port, baud, 1, timeout_ms, stop_bits);
        if (!rc) {
                lock_sdk();
                g_protocol = 8;
                unlock_sdk();
        }
        return rc;
}
DYN200_API int dyn200_close(void) { lock_sdk(); port_close(); set_ok(); unlock_sdk(); return 0; }
DYN200_API int dyn200_is_open(void) { int value; lock_sdk(); value = port_is_open(); unlock_sdk(); return value; }
DYN200_API int dyn200_set_decimals(int torque, int speed, int power)
{
        int rc; lock_sdk();
        if (torque < 0 || torque > 6 || speed < 0 || speed > 6 || power < 0 || power > 6)
                rc = set_error(DYN200_ERR_ARGUMENT, "decimal counts must be 0..6");
        else { g_torque_decimals = torque; g_speed_decimals = speed; g_power_decimals = power; rc = set_ok(); }
        unlock_sdk(); return rc;
}
DYN200_API int dyn200_read(Dyn200Measurement *m)
{
        uint16_t w[6]; int rc; lock_sdk();
        if (!m) rc = set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        else if ((rc = read_registers_unlocked(DYN200_REG_TORQUE, 6, w)) == 0) {
                m->torque_raw = (int32_t)(((uint32_t)w[0] << 16) | w[1]);
                m->speed_raw = (int32_t)(((uint32_t)w[2] << 16) | w[3]);
                m->power_raw = (int32_t)(((uint32_t)w[4] << 16) | w[5]);
                m->torque_nm = scale_i32(m->torque_raw, g_torque_decimals);
                m->speed_rpm = scale_i32(m->speed_raw, g_speed_decimals);
                m->power_kw = scale_i32(m->power_raw, g_power_decimals);
                g_measurement = *m;
        }
        unlock_sdk(); return rc;
}
DYN200_API int dyn200_read_registers(uint16_t a, uint16_t n, uint16_t *v) { int rc; lock_sdk(); rc = read_registers_unlocked(a,n,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_read_i32(uint16_t a, int32_t *v) { int rc; lock_sdk(); rc = read_i32_unlocked(a,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_write_i32(uint16_t a, int32_t v) { int rc; lock_sdk(); rc = write_i32_unlocked(a,v); unlock_sdk(); return rc; }
DYN200_API int dyn200_zero(void) { int rc; lock_sdk(); rc = command_unlocked(0); unlock_sdk(); return rc; }
DYN200_API int dyn200_factory_reset(void) { int rc; lock_sdk(); rc = command_unlocked(2); unlock_sdk(); return rc; }
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
static int read_active_frame_unlocked(uint8_t *frame, size_t frame_size)
{
        size_t have = 0;
        unsigned discarded = 0;
        int rc;
        if (!port_is_open()) return set_error(DYN200_ERR_NOT_OPEN, "serial port is not open");
        while (discarded < 256) {
                if (have < frame_size) {
                        rc = read_all(frame + have, 1);
                        if (rc == -2) return set_error(DYN200_ERR_TIMEOUT, "active stream timeout");
                        if (rc) return set_error(DYN200_ERR_READ, "active stream read failed");
                        ++have;
                }
                if (have == frame_size && valid_crc(frame, frame_size)) return set_ok();
                if (have == frame_size) {
                        memmove(frame, frame + 1, frame_size - 1);
                        have = frame_size - 1;
                        ++discarded;
                }
        }
        return set_error(DYN200_ERR_CRC, "cannot synchronize active stream CRC");
}
DYN200_API int dyn200_read_active6(Dyn200Measurement *m)
{
        uint8_t frame[6];
        int32_t torque, speed;
        int rc;
        lock_sdk();
        if (!m) rc = set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        else if ((rc = read_active_frame_unlocked(frame, sizeof(frame))) == 0 &&
                 (rc = dyn200_decode_active6(frame, &torque, &speed)) == 0) {
                m->torque_raw = torque;
                m->speed_raw = speed;
                m->power_raw = 0;
                m->torque_nm = scale_i32(torque, g_torque_decimals);
                m->speed_rpm = scale_i32(speed, g_speed_decimals);
                m->power_kw = m->torque_nm * m->speed_rpm * 0.00010471975511965977;
                g_measurement = *m;
        }
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_read_active8(Dyn200Measurement *m)
{
        uint8_t frame[8];
        int32_t torque;
        uint32_t speed;
        int rc;
        lock_sdk();
        if (!m) rc = set_error(DYN200_ERR_ARGUMENT, "measurement pointer is null");
        else if ((rc = read_active_frame_unlocked(frame, sizeof(frame))) == 0 &&
                 (rc = dyn200_decode_active8(frame, &torque, &speed)) == 0) {
                m->torque_raw = torque;
                m->speed_raw = (int32_t)speed;
                m->power_raw = 0;
                m->torque_nm = scale_i32(torque, g_torque_decimals);
                m->speed_rpm = scale_i32((int32_t)speed, g_speed_decimals);
                m->power_kw = m->torque_nm * m->speed_rpm * 0.00010471975511965977;
                g_measurement = *m;
        }
        unlock_sdk();
        return rc;
}
DYN200_API int dyn200_update(void)
{
        Dyn200Measurement measurement;
        int protocol;
        lock_sdk();
        protocol = g_protocol;
        unlock_sdk();
        if (protocol == 6) return dyn200_read_active6(&measurement);
        if (protocol == 8) return dyn200_read_active8(&measurement);
        return dyn200_read(&measurement);
}
static double cached_value(int field)
{
        double value;
        lock_sdk();
        value = field == 0 ? g_measurement.torque_nm : field == 1 ? g_measurement.speed_rpm : g_measurement.power_kw;
        unlock_sdk();
        return value;
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
        int32_t value; lock_sdk(); value = g_measurement.torque_raw; unlock_sdk(); return value;
}
DYN200_API int32_t dyn200_get_speed_raw(void)
{
        int32_t value; lock_sdk(); value = g_measurement.speed_raw; unlock_sdk(); return value;
}
DYN200_API int32_t dyn200_get_power_raw(void)
{
        int32_t value; lock_sdk(); value = g_measurement.power_raw; unlock_sdk(); return value;
}
DYN200_API int dyn200_last_error_code(void) { return g_error_code; }
DYN200_API const char *dyn200_last_error(void) { return g_error; }
