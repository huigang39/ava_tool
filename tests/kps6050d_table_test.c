// Compile the real SDK with Win32 serial I/O replaced. No hardware is accessed
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static BOOL WINAPI fake_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI fake_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI fake_purge(HANDLE h, DWORD flags) { (void)h; (void)flags; return TRUE; }
static BOOL WINAPI fake_close(HANDLE h) { (void)h; return TRUE; }
static unsigned opens;
static HANDLE WINAPI fake_open(LPCSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES sa,
                               DWORD creation, DWORD flags, HANDLE template_file)
{
        (void)access; (void)sharing; (void)sa; (void)creation; (void)flags; (void)template_file;
        assert(!strcmp(path, "\\\\.\\COM42"));
        ++opens;
        return (HANDLE)(uintptr_t)123;
}
static BOOL WINAPI fake_get(HANDLE h, LPDCB d) { (void)h; (void)d; return TRUE; }
static BOOL WINAPI fake_set(HANDLE h, LPDCB d) { (void)h; assert(d->BaudRate == 2400); return TRUE; }
static BOOL WINAPI fake_timeout(HANDLE h, LPCOMMTIMEOUTS t) { (void)h; (void)t; return TRUE; }
#define ReadFile fake_read
#define WriteFile fake_write
#define PurgeComm fake_purge
#define CloseHandle fake_close
#define CreateFileA fake_open
#define GetCommState fake_get
#define SetCommState fake_set
#define SetCommTimeouts fake_timeout
#define KPS6050D_STATIC
#include "../sdk/kps6050d/src/kps6050d.c"

static Kps6050dState device = {12.0f, 2.0f, 24.0f, 5.0f, 60.0f, 50.0f, 1, 1, 1, 0, 0};
static unsigned reads, writes;
static int timeout_read, bad_crc, big_endian = 1, empty_ports;
static uint8_t request[15];
static BOOL WINAPI fake_write(HANDLE h, LPCVOID data, DWORD size, LPDWORD done, LPOVERLAPPED ov)
{
        const uint8_t *tx = data;
        (void)h; (void)ov;
        assert(size <= sizeof(request));
        assert(crc16(tx, size - 2) == (uint16_t)(tx[size-2] | ((uint16_t)tx[size-1] << 8)));
        memcpy(request, data, size);
        *done = size;
        if (tx[1] == 3) ++reads;
        else {
                uint16_t v, c;
                assert(tx[1] == 0x10 && size == 15);
                ++writes;
                device.output_on = !!(tx[7] & 1);
                device.ocp_on = !!(tx[7] & 2);
                device.remote_locked = !!(tx[7] & 4);
                v = (uint16_t)((tx[9] << 8) | tx[10]);
                c = (uint16_t)((tx[11] << 8) | tx[12]);
                if (!big_endian) { v = swap16(v); c = swap16(c); }
                device.set_voltage = v * 0.01f;
                device.set_current = c * 0.01f;
        }
        return TRUE;
}
static BOOL WINAPI fake_read(HANDLE h, LPVOID data, DWORD size, LPDWORD done, LPOVERLAPPED ov)
{
        uint8_t *rx = data;
        (void)h; (void)ov;
        if (timeout_read) { *done = 0; return TRUE; }
        memset(rx, 0, size);
        rx[0] = request[0]; rx[1] = request[1];
        if (request[1] == 3) {
                uint16_t regs[8];
                int i;
                assert(size == 21);
                rx[2] = 16;
                regs[0] = (uint16_t)((device.output_on | (device.ocp_on << 1) |
                          (device.remote_locked << 2) | (big_endian << 3)) << 8);
                regs[1] = 0;
                regs[2] = (uint16_t)lroundf(device.voltage * 100);
                regs[3] = (uint16_t)lroundf(device.current * 100);
                regs[4] = (uint16_t)lroundf(device.set_voltage * 100);
                regs[5] = (uint16_t)lroundf(device.set_current * 100);
                regs[6] = 6000; regs[7] = 5000;
                for (i = 0; i < 8; ++i) {
                        uint16_t val = !big_endian && i >= 2 ? swap16(regs[i]) : regs[i];
                        rx[3+i*2] = (uint8_t)(val >> 8); rx[4+i*2] = (uint8_t)val;
                }
        } else { assert(size == 8); rx[5] = 3; }
        add_crc(rx, size - 2);
        if (bad_crc) rx[size-1] ^= 1;
        *done = size;
        return TRUE;
}
uint32_t dyn200_ports_enumerate(char (*names)[64], char (*descs)[160], uint32_t capacity)
{
        assert(capacity >= 3);
        if (empty_ports) return 0;
        snprintf(names[0], 64, "COM3"); snprintf(descs[0], 160, "USB serial A");
        snprintf(names[1], 64, "COM11"); snprintf(descs[1], 160, "USB serial B");
        snprintf(names[2], 64, "COM42"); snprintf(descs[2], 160, "电源串口");
        return 3;
}
int main(void)
{
        kps6050d_port_param port = {0};
        kps6050d_param p = {0};
        kps6050d_rt rt = {0};
        unsigned before;
        char name[64], desc[160];
        assert(kps6050d_read_port_param(&port) == 0 && port.baud_rate == 2400);
        assert(!opens && !reads && !writes);
        port.action = KPS6050D_PORT_RESCAN;
        assert(kps6050d_read_port_param(&port) == 0 && port.count == 3 && !port.action);
        assert(kps6050d_port_count() == 3);
        assert(kps6050d_port_name(2, name, sizeof(name), desc, sizeof(desc)) == 0);
        assert(!strcmp(name, "COM42") && !strcmp(desc, "电源串口"));
        assert(!opens && !reads && !writes);
        snprintf(port.port, sizeof(port.port), "COM42"); port.index = 2;
        port.action = KPS6050D_PORT_CONNECT;
        assert(kps6050d_read_port_param(&port) == 0 && port.connected && !port.action);
        assert(opens == 1 && reads == 1 && !writes && !strcmp(port.active_port, "COM42"));
        assert(kps6050d_read_param(&p) == 0);
        assert(p.voltage.val == 24 && p.current.val == 5 && p.output.enabled && p.ocp.enabled);
        memset(&p, 0, sizeof(p)); before = reads;
        assert(kps6050d_write_param(&p) == 0 && reads == before && !writes);
        p.voltage.val = 30; p.write_voltage = 1;
        assert(kps6050d_write_param(&p) == 0 && writes == 1 && reads == before + 1);
        assert(device.set_voltage == 30 && device.set_current == 5 && device.output_on && device.ocp_on && device.remote_locked);
        assert(!p.write_voltage && p.current.val == 5 && p.output.enabled);
        p.current.val = 6; p.voltage.val = NAN; p.write_current = p.write_voltage = 1; before = reads;
        assert(kps6050d_write_param(&p) == -1 && reads == before && writes == 1);
        assert(!p.write_current && !p.write_voltage);
        p.voltage.val = 61; p.write_voltage = 1;
        assert(kps6050d_write_param(&p) == -1 && reads == before);
        p.output.enabled = 2; p.write_output = 1;
        assert(kps6050d_write_param(&p) == -1 && reads == before);
        p.current.val = 51; p.write_current = 1;
        assert(kps6050d_write_param(&p) == -1 && reads == before);
        big_endian = 0;
        p.voltage.val = 18; p.current.val = 3; p.output.enabled = 0;
        p.write_voltage = p.write_current = p.write_output = 1;
        assert(kps6050d_write_param(&p) == 0 && writes == 2);
        assert(device.set_voltage == 18 && device.set_current == 3 && !device.output_on && device.ocp_on);
        assert(kps6050d_read_rt(&rt) == 0 && rt.fresh && rt.sample_count == 1);
        assert(rt.voltage == 12 && rt.current == 2 && rt.power == 24 && rt.set_voltage == 18);
        timeout_read = 1;
        assert(kps6050d_read_rt(&rt) == -6 && !rt.fresh && isnan(rt.voltage) && rt.sample_count == 1);
        p.current.val = 4; p.write_current = 1;
        assert(kps6050d_write_param(&p) == -6 && writes == 2 && !p.write_current);
        timeout_read = 0; bad_crc = 1; p.write_current = 1;
        assert(kps6050d_write_param(&p) == -8 && writes == 2 && !p.write_current);
        bad_crc = 0;
        assert(kps6050d_read_rt(&rt) == 0 && rt.fresh && rt.sample_count == 2);
        before = reads; empty_ports = 1; port.action = KPS6050D_PORT_RESCAN;
        assert(kps6050d_read_port_param(&port) == 0 && port.count == 0 && !port.name[0]);
        assert(!strcmp(port.port, "COM42"));
        empty_ports = 0; port.action = KPS6050D_PORT_RESCAN;
        assert(kps6050d_read_port_param(&port) == 0 && port.index == 2 && !strcmp(port.port, "COM42"));
        assert(reads == before && writes == 2);
        port.action = KPS6050D_PORT_DISCONNECT;
        assert(kps6050d_read_port_param(&port) == 0 && !port.connected && !port.active_port[0]);
        assert(kps6050d_read_rt(&rt) == -4 && !rt.connected);
        assert(kps6050d_read_param(NULL) == -1 && kps6050d_write_param(NULL) == -1);
        puts("KPS6050D tables: serial selection, read-only connect, gated writes, endian handling, validation, realtime data and failures passed");
        return 0;
}
