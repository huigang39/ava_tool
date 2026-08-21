#if !defined(_WIN32)
#define _POSIX_C_SOURCE 199309L
#endif
#include "kps6050d.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
static void
sleep_ms(unsigned ms)
{
        Sleep(ms);
}
#else
#include <time.h>
static void
sleep_ms(unsigned ms)
{
        struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
        nanosleep(&t, NULL);
}
#endif
static volatile sig_atomic_t g_stop;
static void
on_signal(int sig)
{
        (void)sig;
        g_stop = 1;
}
static int
check(int rc, const char *op)
{
        if (!rc)
                return 1;
        fprintf(stderr, "%s failed: %d, %s\n", op, kps6050d_last_error_code(), kps6050d_last_error());
        return 0;
}
static int
monitor(const char *phase)
{
        int           sec;
        Kps6050dState s = {0};
        for (sec = 1; sec <= 10 && !g_stop; sec++) {
                if (!check(kps6050d_read(&s), "read"))
                        return 0;
                printf("[%s %2d/10 s] actual=%.2f V / %.3f A, limit=%.2f V / %.2f A, mode=%s\n",
                       phase,
                       sec,
                       s.voltage,
                       s.current,
                       s.set_voltage,
                       s.set_current,
                       s.constant_current ? "CC" : "CV");
                sleep_ms(1000);
        }
        return 1;
}
int
main(int argc, char **argv)
{
#if defined(_WIN32)
        const char *default_port = "COM7";
#else
        const char *default_port = "/dev/ttyUSB0";
#endif
        const char *port      = argc > 1 ? argv[1] : default_port;
        int         baud      = argc > 2 ? atoi(argv[2]) : 2400;
        int         id        = argc > 3 ? atoi(argv[3]) : 0;
        int         connected = 0, output = 0, result = EXIT_FAILURE;
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        printf("Connecting on %s, %d baud, ID %d...\n", port, baud, id);
        if (!check(kps6050d_open(port, baud, id), "connect"))
                goto done;
        connected = 1;
        if (!check(kps6050d_set_output(0), "disable output") || !check(kps6050d_set_voltage(48.0f), "set voltage") ||
            !check(kps6050d_set_current(5.0f), "set current"))
                goto done;
        puts("Cycling: ON 10 s, OFF 10 s. Press Ctrl+C to stop.");
        while (!g_stop) {
                if (!check(kps6050d_set_output(1), "enable output"))
                        goto done;
                output = 1;
                if (!monitor("ON "))
                        goto done;
                if (!check(kps6050d_set_output(0), "disable output"))
                        goto done;
                output = 0;
                if (!monitor("OFF"))
                        goto done;
        }
        result = EXIT_SUCCESS;
done:
        if (connected && output)
                (void)kps6050d_set_output(0);
        if (connected)
                (void)kps6050d_close();
        return result;
}


