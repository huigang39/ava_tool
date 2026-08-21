#include "sdk/ava_debug/inc/ava_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

struct ref_pvct {
    float pos, vel, cur, tor, ffd_vel, ffd_cur, ffd_tor;
};

struct fdb_pvct {
    float pos, vel, cur, elec_tor, load_tor;
};

static volatile LONG running = 1;

static BOOL WINAPI
on_console(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&running, 0);
        return true;
    }
    return false;
}

int
main(int argc, char **argv)
{
    const char        *ref_name = "g_foc.in.ref_pvct";
    const char        *fdb_name = "g_foc.out.fdb_pvct";
    struct ava_sdk    *sdk      = NULL;
    struct ref_pvct    ref;
    struct fdb_pvct    fdb;
    uint32_t           ref_addr, fdb_addr, slave = 0;
    LARGE_INTEGER      freq, next, now;
    unsigned long long cycles = 0, errors = 0, overruns = 0;
    int                ret;
    if (argc < 2) {
        printf("usage: %s <axf/elf> [slave_idx]\n", argv[0]);
        return 1;
    }
    if (argc > 2)
        slave = (uint32_t)strtoul(argv[2], NULL, 0);
    ret = ava_sdk_init(&sdk, argv[1], slave);
    if (ret != AVA_SDK_OK) {
        printf("init failed: %s\n", ava_sdk_strerror(ret));
        return 2;
    }
    if (ava_sdk_resolve(sdk, ref_name, &ref_addr) != AVA_SDK_OK ||
        ava_sdk_resolve(sdk, fdb_name, &fdb_addr) != AVA_SDK_OK) {
        puts("resolve failed");
        ava_sdk_close(sdk);
        return 3;
    }
    printf("ref %s -> 0x%08X (%u bytes)\n", ref_name, ref_addr, (unsigned)sizeof(ref));
    printf("fdb %s -> 0x%08X (%u bytes)\n", fdb_name, fdb_addr, (unsigned)sizeof(fdb));
    puts("1 kHz target polling; Ctrl+C to stop.");
    SetConsoleCtrlHandler(on_console, true);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    while (InterlockedCompareExchange(&running, 1, 1)) {
        ret = ava_sdk_read_at(sdk, ref_addr, &ref, sizeof(ref));
        if (ret == AVA_SDK_OK)
            ret = ava_sdk_read_at(sdk, fdb_addr, &fdb, sizeof(fdb));
        if (ret != AVA_SDK_OK)
            ++errors;
        else
            printf("ref p=% .5F v=% .5F c=% .5F t=% .5F ffv=% .5F ffc=% .5F fft=% .5F | fdb p=% "
                   ".5F v=% .5F c=% "
                   ".5F te=% .5F tl=% .5F\n",
                   ref.pos,
                   ref.vel,
                   ref.cur,
                   ref.tor,
                   ref.ffd_vel,
                   ref.ffd_cur,
                   ref.ffd_tor,
                   fdb.pos,
                   fdb.vel,
                   fdb.cur,
                   fdb.elec_tor,
                   fdb.load_tor);
        ++cycles;
        next.QuadPart += freq.QuadPart / 1000;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next.QuadPart) {
            ++overruns;
            next = now;
        } else
            do {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < next.QuadPart);
        if ((cycles % 1000U) == 0U)
            fprintf(stderr, "cycles=%llu errors=%llu overruns=%llu\n", cycles, errors, overruns);
    }
    ava_sdk_close(sdk);
    return 0;
}
