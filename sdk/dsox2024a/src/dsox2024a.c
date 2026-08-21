#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200112L
#endif
#include "dsox2024a.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static BOOL CALLBACK
init_lock(PINIT_ONCE a, PVOID b, PVOID *c)
{
        (void)a;
        (void)b;
        (void)c;
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
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int            socket_t;
#define BAD_SOCKET (-1)
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

enum transport_e { TRANSPORT_NONE, TRANSPORT_SOCKET, TRANSPORT_VISA };
static enum transport_e g_transport = TRANSPORT_NONE;
static socket_t         g_socket    = BAD_SOCKET;
static int              g_error_code;
static char             g_error[256] = "OK";
#if defined(_WIN32)
typedef long           ViStatus;
typedef unsigned long  ViSession, ViUInt32;
typedef unsigned char *ViBuf;
struct visa_api {
        HMODULE module;
        ViStatus(__stdcall *open_rm)(ViSession *);
        ViStatus(__stdcall *find)(ViSession, const char *, ViSession *, ViUInt32 *, char *);
        ViStatus(__stdcall *next)(ViSession, char *);
        ViStatus(__stdcall *open)(ViSession, char *, ViUInt32, ViUInt32, ViSession *);
        ViStatus(__stdcall *close)(ViSession);
        ViStatus(__stdcall *set)(ViSession, ViUInt32, ViUInt32);
        ViStatus(__stdcall *write)(ViSession, ViBuf, ViUInt32, ViUInt32 *);
        ViStatus(__stdcall *read)(ViSession, ViBuf, ViUInt32, ViUInt32 *);
} g_visa;
static ViSession g_rm, g_instr;
static int       g_wsa;
#endif
static int
fail(int c, const char *m)
{
        g_error_code = c;
        snprintf(g_error, sizeof(g_error), "%s", m);
        return c;
}
static int
ok(void)
{
        g_error_code = 0;
        snprintf(g_error, sizeof(g_error), "OK");
        return 0;
}
static void
socket_close(socket_t s)
{
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
}
#if defined(_WIN32)
static void
visa_close(void)
{
        if (g_instr && g_visa.close)
                g_visa.close(g_instr);
        if (g_rm && g_visa.close)
                g_visa.close(g_rm);
        g_instr = g_rm = 0;
}
static int
visa_load(void)
{
        if (g_visa.module)
                return 1;
        /*
         * Keysight installs its 64-bit VISA implementation as visa32.dll
         * below the Win64 directory. Prefer it over the IVI dispatcher,
         * which may be installed without a working 64-bit provider.
         */
        g_visa.module =
            LoadLibraryA("C:\\Program Files\\IVI Foundation\\VISA\\Win64\\ktvisa\\ktbin\\visa32.dll");
        if (!g_visa.module)
                g_visa.module = LoadLibraryA("visa64.dll");
        if (!g_visa.module)
                g_visa.module = LoadLibraryA("visa32.dll");
        if (!g_visa.module)
                return 0;
#define VP(f, n) g_visa.f = (void *)GetProcAddress(g_visa.module, n)
        VP(open_rm, "viOpenDefaultRM");
        VP(find, "viFindRsrc");
        VP(next, "viFindNext");
        VP(open, "viOpen");
        VP(close, "viClose");
        VP(set, "viSetAttribute");
        VP(write, "viWrite");
        VP(read, "viRead");
#undef VP
        return g_visa.open_rm && g_visa.find && g_visa.next && g_visa.open && g_visa.close && g_visa.set && g_visa.write &&
               g_visa.read;
}
#endif
static int
send_all(const char *p, size_t n)
{
        if (g_transport == TRANSPORT_NONE)
                return fail(DSOX2024A_ERR_NOT_OPEN, "oscilloscope is not connected");
        while (n) {
                int done = 0, chunk = (int)(n > 0x3fffffff ? 0x3fffffff : n);
                if (g_transport == TRANSPORT_SOCKET) {
                        done = send(g_socket, p, chunk, 0);
                        if (done <= 0)
                                return fail(DSOX2024A_ERR_NETWORK, "SCPI socket write failed");
                } else {
#if defined(_WIN32)
                        ViUInt32 out = 0;
                        if (g_visa.write(g_instr, (ViBuf)(void *)p, (ViUInt32)chunk, &out) < 0 || !out)
                                return fail(DSOX2024A_ERR_VISA, "VISA write failed");
                        done = (int)out;
#else
                        return fail(DSOX2024A_ERR_VISA, "USB/VISA is supported on Windows only");
#endif
                }
                p += done;
                n -= (size_t)done;
        }
        return 0;
}
static int
recv_exact(void *p, size_t n)
{
        unsigned char *q = (unsigned char *)p;
        while (n) {
                int done = 0, chunk = (int)(n > 0x3fffffff ? 0x3fffffff : n);
                if (g_transport == TRANSPORT_SOCKET) {
                        done = recv(g_socket, (char *)q, chunk, 0);
                        if (done <= 0)
                                return fail(DSOX2024A_ERR_NETWORK, "SCPI socket read failed");
                } else {
#if defined(_WIN32)
                        ViUInt32 in = 0;
                        if (g_visa.read(g_instr, q, (ViUInt32)chunk, &in) < 0 || !in)
                                return fail(DSOX2024A_ERR_VISA, "VISA read failed or timed out");
                        done = (int)in;
#else
                        return fail(DSOX2024A_ERR_VISA, "USB/VISA is supported on Windows only");
#endif
                }
                q += done;
                n -= (size_t)done;
        }
        return 0;
}
static int
write_u(const char *c)
{
        size_t n;
        char  *line;
        int    r;
        if (!c || !*c)
                return fail(DSOX2024A_ERR_ARGUMENT, "command is empty");
        n    = strlen(c);
        line = (char *)malloc(n + 2);
        if (!line)
                return fail(DSOX2024A_ERR_PROTOCOL, "out of memory");
        memcpy(line, c, n);
        if (c[n - 1] != '\n')
                line[n++] = '\n';
        r = send_all(line, n);
        free(line);
        return r ? r : ok();
}
static int
query_u(const char *c, char *out, size_t cap)
{
        size_t n = 0;
        char   ch;
        int    r;
        if (!out || cap < 2)
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid response buffer");
        r = write_u(c);
        if (r)
                return r;
        for (;;) {
                if ((r = recv_exact(&ch, 1)))
                        return r;
                if (ch == '\n')
                        break;
                if (ch == '\r')
                        continue;
                if (n + 1 >= cap) {
                        out[n] = 0;
                        return fail(DSOX2024A_ERR_BUFFER_TOO_SMALL, "response buffer too small");
                }
                out[n++] = ch;
        }
        out[n] = 0;
        return ok();
}
static int
valid_ch(int c)
{
        return c >= 1 && c <= 4;
}
static int
simple(const char *c)
{
        int r;
        lock_sdk();
        r = write_u(c);
        unlock_sdk();
        return r;
}
static int
chval(const char *leaf, int ch, double v)
{
        char c[96];
        if (!valid_ch(ch) || !isfinite(v))
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid channel or value");
        snprintf(c, sizeof(c), ":CHANnel%d:%s %.17g", ch, leaf, v);
        return simple(c);
}
static int
measure(const char *n, int ch, double *v)
{
        char c[80], a[128], *end;
        int  r;
        if (!valid_ch(ch) || !v)
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid measurement argument");
        snprintf(c, sizeof(c), ":MEASure:%s? CHANnel%d", n, ch);
        lock_sdk();
        r = query_u(c, a, sizeof(a));
        if (!r) {
                *v = strtod(a, &end);
                if (end == a || !isfinite(*v))
                        r = fail(DSOX2024A_ERR_PROTOCOL, "invalid measurement response");
        }
        unlock_sdk();
        return r;
}
static void close_u(void);

DSOX2024A_API int
dsox2024a_open(const char *host, unsigned short port, int timeout)
{
        struct addrinfo hints, *list, *p;
        char            service[8];
        int             r = 0;
        lock_sdk();
        if (!host || !*host || timeout <= 0) {
                r = fail(DSOX2024A_ERR_ARGUMENT, "invalid host or timeout");
                goto done;
        }
        close_u();
        if (!port)
                port = 5025;
#if defined(_WIN32)
        if (!g_wsa) {
                WSADATA w;
                if (WSAStartup(MAKEWORD(2, 2), &w)) {
                        r = fail(DSOX2024A_ERR_NETWORK, "WSAStartup failed");
                        goto done;
                }
                g_wsa = 1;
        }
#endif
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(service, sizeof(service), "%u", (unsigned)port);
        if (getaddrinfo(host, service, &hints, &list)) {
                r = fail(DSOX2024A_ERR_NETWORK, "host resolution failed");
                goto done;
        }
        for (p = list; p; p = p->ai_next) {
                g_socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (g_socket == BAD_SOCKET)
                        continue;
#if defined(_WIN32)
                {
                        DWORD tv = (DWORD)timeout;
                        setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
                        setsockopt(g_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));
                }
#else
                {
                        struct timeval tv = {timeout / 1000, (timeout % 1000) * 1000};
                        setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        setsockopt(g_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                }
#endif
                if (connect(g_socket, p->ai_addr, (int)p->ai_addrlen) == 0)
                        break;
                socket_close(g_socket);
                g_socket = BAD_SOCKET;
        }
        freeaddrinfo(list);
        if (g_socket == BAD_SOCKET)
                r = fail(DSOX2024A_ERR_NETWORK, "could not connect");
        else {
                g_transport = TRANSPORT_SOCKET;
                r           = ok();
        }
done:
        unlock_sdk();
        return r;
}
/* close without locking; callers that already own g_lock use this helper */
static void
close_u(void)
{
        if (g_socket != BAD_SOCKET)
                socket_close(g_socket);
        g_socket = BAD_SOCKET;
#if defined(_WIN32)
        visa_close();
#endif
        g_transport = TRANSPORT_NONE;
}
DSOX2024A_API int
dsox2024a_open_usb(const char *resource, int timeout)
{
        int r = 0;
        lock_sdk();
        close_u();
        if (timeout <= 0) {
                r = fail(DSOX2024A_ERR_ARGUMENT, "timeout must be positive");
                goto done;
        }
#if !defined(_WIN32)
        (void)resource;
        r = fail(DSOX2024A_ERR_VISA, "USB/VISA is supported on Windows only");
#else
        if (!visa_load()) {
                r = fail(DSOX2024A_ERR_VISA, "install Keysight IO Libraries Suite");
                goto done;
        }
        {
                ViStatus visa_status = g_visa.open_rm(&g_rm);
                if (visa_status < 0) {
                        char module_path[MAX_PATH] = {0};
                        char message[512];
                        GetModuleFileNameA(g_visa.module, module_path, (DWORD)sizeof(module_path));
                        snprintf(message,
                                 sizeof(message),
                                 "cannot open VISA manager: status=0x%08lX, dll=%s",
                                 (unsigned long)visa_status,
                                 module_path);
                        r = fail(DSOX2024A_ERR_VISA, message);
                        goto done;
                }
        }
        if (resource && *resource) {
                if (g_visa.open(g_rm, (char *)resource, 0, (ViUInt32)timeout, &g_instr) < 0) {
                        r = fail(DSOX2024A_ERR_VISA, "cannot open VISA resource");
                        goto done;
                }
                g_visa.set(g_instr, 0x3FFF001AUL, (ViUInt32)timeout);
                g_transport = TRANSPORT_VISA;
                r           = ok();
        } else {
                ViSession fl    = 0;
                ViUInt32  count = 0, i;
                char      desc[512], id[256];
                if (g_visa.find(g_rm, "USB?*::INSTR", &fl, &count, desc) < 0 || !count) {
                        r = fail(DSOX2024A_ERR_VISA, "no USBTMC instrument found");
                        goto done;
                }
                for (i = 0; i < count; i++) {
                        if (i && g_visa.next(fl, desc) < 0)
                                break;
                        if (g_visa.open(g_rm, desc, 0, (ViUInt32)timeout, &g_instr) < 0)
                                continue;
                        g_visa.set(g_instr, 0x3FFF001AUL, (ViUInt32)timeout);
                        g_transport = TRANSPORT_VISA;
                        if (!query_u("*IDN?", id, sizeof(id)) && strstr(id, "DSOX2024A")) {
                                r = ok();
                                break;
                        }
                        g_visa.close(g_instr);
                        g_instr     = 0;
                        g_transport = TRANSPORT_NONE;
                }
                g_visa.close(fl);
                if (g_transport != TRANSPORT_VISA)
                        r = fail(DSOX2024A_ERR_VISA, "DSOX2024A USB instrument not found");
        }
#endif
done:
        if (r)
                close_u();
        unlock_sdk();
        return r;
}
DSOX2024A_API int
dsox2024a_close(void)
{
        lock_sdk();
        close_u();
        ok();
        unlock_sdk();
        return 0;
}
DSOX2024A_API int
dsox2024a_is_open(void)
{
        int r;
        lock_sdk();
        r = g_transport != TRANSPORT_NONE;
        unlock_sdk();
        return r;
}
DSOX2024A_API int
dsox2024a_write(const char *c)
{
        return simple(c);
}
DSOX2024A_API int
dsox2024a_query(const char *c, char *r, size_t n)
{
        int x;
        lock_sdk();
        x = query_u(c, r, n);
        unlock_sdk();
        return x;
}
DSOX2024A_API int
dsox2024a_identify(char *r, size_t n)
{
        return dsox2024a_query("*IDN?", r, n);
}
DSOX2024A_API int
dsox2024a_reset(void)
{
        return simple("*RST;*CLS");
}
DSOX2024A_API int
dsox2024a_autoscale(void)
{
        return simple(":AUToscale");
}
DSOX2024A_API int
dsox2024a_run(void)
{
        return simple(":RUN");
}
DSOX2024A_API int
dsox2024a_stop(void)
{
        return simple(":STOP");
}
DSOX2024A_API int
dsox2024a_single(void)
{
        return simple(":SINGle");
}
#define CHCMD(fn, fmt)                                                               \
        DSOX2024A_API int fn(int ch)                                                 \
        {                                                                            \
                char c[64];                                                          \
                if (!valid_ch(ch))                                                   \
                        return fail(DSOX2024A_ERR_ARGUMENT, "channel must be 1..4"); \
                snprintf(c, sizeof(c), fmt, ch);                                     \
                return simple(c);                                                    \
        }
CHCMD(dsox2024a_digitize, ":DIGitize CHANnel%d")
DSOX2024A_API int
dsox2024a_set_channel_enabled(int ch, int e)
{
        char c[64];
        if (!valid_ch(ch))
                return fail(DSOX2024A_ERR_ARGUMENT, "channel must be 1..4");
        snprintf(c, sizeof(c), ":CHANnel%d:DISPlay %s", ch, e ? "ON" : "OFF");
        return simple(c);
}
DSOX2024A_API int
dsox2024a_set_channel_scale(int c, double v)
{
        return v > 0 ? chval("SCALe", c, v) : fail(DSOX2024A_ERR_ARGUMENT, "scale must be positive");
}
DSOX2024A_API int
dsox2024a_set_channel_offset(int c, double v)
{
        return chval("OFFSet", c, v);
}
DSOX2024A_API int
dsox2024a_set_probe_ratio(int c, double v)
{
        return v > 0 ? chval("PROBe", c, v) : fail(DSOX2024A_ERR_ARGUMENT, "probe ratio must be positive");
}
DSOX2024A_API int
dsox2024a_set_channel_coupling(int ch, const char *s)
{
        char c[64];
        if (!valid_ch(ch) || !s || (strcmp(s, "AC") && strcmp(s, "DC")))
                return fail(DSOX2024A_ERR_ARGUMENT, "coupling must be AC or DC");
        snprintf(c, sizeof(c), ":CHANnel%d:COUPling %s", ch, s);
        return simple(c);
}
DSOX2024A_API int
dsox2024a_set_timebase_scale(double v)
{
        char c[64];
        if (!isfinite(v) || v <= 0)
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid timebase scale");
        snprintf(c, sizeof(c), ":TIMebase:SCALe %.17g", v);
        return simple(c);
}
DSOX2024A_API int
dsox2024a_set_timebase_position(double v)
{
        char c[64];
        if (!isfinite(v))
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid timebase position");
        snprintf(c, sizeof(c), ":TIMebase:POSition %.17g", v);
        return simple(c);
}
DSOX2024A_API int
dsox2024a_set_edge_trigger(int ch, double v, const char *s)
{
        char c[180];
        if (!valid_ch(ch) || !isfinite(v) || !s)
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid trigger");
        snprintf(c,
                 sizeof(c),
                 ":TRIGger:MODE EDGE;:TRIGger:EDGE:SOURce CHANnel%d;SLOPe %s;:TRIGger:LEVel CHANnel%d,%.17g",
                 ch,
                 s,
                 ch,
                 v);
        return simple(c);
}
DSOX2024A_API int
dsox2024a_measure_frequency(int c, double *v)
{
        return measure("FREQuency", c, v);
}
DSOX2024A_API int
dsox2024a_measure_vpp(int c, double *v)
{
        return measure("VPP", c, v);
}
DSOX2024A_API int
dsox2024a_measure_rms(int c, double *v)
{
        return measure("VRMS", c, v);
}
DSOX2024A_API int
dsox2024a_read_waveform(int ch, size_t req, double *s, size_t cap, Dsox2024aWaveformInfo *i)
{
        char           cmd[128], pre[512], *cur, *end, hash, digit, nl, len[10] = {0};
        double         p[10];
        size_t         pts, bytes, k;
        unsigned char *raw;
        int            r;
        if (!valid_ch(ch) || !i)
                return fail(DSOX2024A_ERR_ARGUMENT, "invalid waveform argument");
        if (!req)
                req = 1000;
        lock_sdk();
        snprintf(cmd, sizeof(cmd), ":WAVeform:SOURce CHANnel%d;FORMat BYTE;UNSigned 1;POINts:MODE RAW;POINts %zu", ch, req);
        if ((r = write_u(cmd)) || (r = query_u(":WAVeform:PREamble?", pre, sizeof(pre))))
                goto done;
        cur = pre;
        for (k = 0; k < 10; k++) {
                p[k] = strtod(cur, &end);
                if (end == cur) {
                        r = fail(DSOX2024A_ERR_PROTOCOL, "invalid waveform preamble");
                        goto done;
                }
                cur = end;
                if (k < 9 && *cur++ != ',') {
                        r = fail(DSOX2024A_ERR_PROTOCOL, "invalid waveform preamble");
                        goto done;
                }
        }
        pts            = (size_t)p[2];
        i->points      = pts;
        i->x_increment = p[4];
        i->x_origin    = p[5];
        i->x_reference = p[6];
        i->y_increment = p[7];
        i->y_origin    = p[8];
        i->y_reference = p[9];
        if (!s && !cap) {
                r = ok();
                goto done;
        }
        if (!s || cap < pts) {
                r = fail(DSOX2024A_ERR_BUFFER_TOO_SMALL, "sample buffer too small");
                goto done;
        }
        if ((r = write_u(":WAVeform:DATA?")) || (r = recv_exact(&hash, 1)) || hash != '#' || (r = recv_exact(&digit, 1)) ||
            digit < '1' || digit > '9') {
                if (!r)
                        r = fail(DSOX2024A_ERR_PROTOCOL, "invalid binary block");
                goto done;
        }
        if ((r = recv_exact(len, (size_t)(digit - '0'))))
                goto done;
        bytes = (size_t)strtoull(len, NULL, 10);
        raw   = (unsigned char *)malloc(bytes);
        if (!raw) {
                r = fail(DSOX2024A_ERR_PROTOCOL, "out of memory");
                goto done;
        }
        r = recv_exact(raw, bytes);
        if (!r)
                r = recv_exact(&nl, 1);
        if (!r && nl == '\r')
                r = recv_exact(&nl, 1);
        if (!r && nl != '\n')
                r = fail(DSOX2024A_ERR_PROTOCOL, "missing block terminator");
        if (!r && bytes < pts)
                r = fail(DSOX2024A_ERR_PROTOCOL, "short waveform block");
        if (!r)
                for (k = 0; k < pts; k++)
                        s[k] = ((double)raw[k] - i->y_reference) * i->y_increment + i->y_origin;
        free(raw);
done:
        unlock_sdk();
        return r;
}
DSOX2024A_API int
dsox2024a_check_error(char *r, size_t n)
{
        int x = dsox2024a_query(":SYSTem:ERRor?", r, n);
        return x ? x : ((r[0] == '0' && r[1] == ',') ? 0 : fail(DSOX2024A_ERR_INSTRUMENT, r));
}
DSOX2024A_API int
dsox2024a_last_error_code(void)
{
        return g_error_code;
}
DSOX2024A_API const char *
dsox2024a_last_error(void)
{
        return g_error;
}




