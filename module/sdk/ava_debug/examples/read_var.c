#include "ava_sdk.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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

static void
print_value(
    struct ava_sdk *sdk, const char *name, uint32_t address, const uint8_t *data, uint32_t size)
{
    char     formatted[16384];
    uint32_t i;
    if (ava_sdk_format(sdk, name, data, size, formatted, sizeof(formatted)) == AVA_SDK_OK) {
        printf("%s @ 0x%08X [%u] = %s\n", name, address, size, formatted);
        return;
    }
    printf("%s @ 0x%08X [%u] =", name, address, size);
    for (i = 0; i < size; ++i)
        printf(" %02X", data[i]);
    if (size == 4U) {
        uint32_t  u;
        int32_t   d;
        float f;
        memcpy(&u, data, sizeof(u));
        memcpy(&d, data, sizeof(d));
        memcpy(&f, data, sizeof(f));
        printf(" | uint32_t=%u int32_t=%d float32_t=%g", u, d, f);
    }
    putchar('\n');
}

static int
read_command(int argc, char **argv)
{
    struct ava_sdk *sdk  = NULL;
    const char     *name = NULL, *elf_path = NULL;
    uint8_t        *buffer;
    uint32_t        address, size, transfer_size, slave = 0;
    double          rate_hz;
    LARGE_INTEGER   timer_freq, next, now;
    LONGLONG        period;
    int             ret, i, consecutive_failures = 0;

    rate_hz = 0.0;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--elf") == 0 && i + 1 < argc)
            elf_path = argv[++i];
        else if (strcmp(argv[i], "--var") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc)
            rate_hz = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--slave") == 0 && i + 1 < argc)
            slave = (uint32_t)strtoul(argv[++i], NULL, 0);
        else {
            printf("unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }
    if (!elf_path || !name || rate_hz < 0.0) {
        printf("usage: %s --elf <axf/elf> --var <variable> [--freq hz] [--slave index]\n", argv[0]);
        return 1;
    }

    ret = ava_sdk_init(&sdk, elf_path, slave);
    if (ret != AVA_SDK_OK) {
        printf("init failed: %s\n", ava_sdk_strerror(ret));
        return 2;
    }
    ret = ava_sdk_query(sdk, name, &address, &size);
    if (ret != AVA_SDK_OK) {
        printf("resolve failed: %s\n", ava_sdk_strerror(ret));
        ava_sdk_close(sdk);
        return 3;
    }
    if ((address & 3U) != 0U) {
        printf("0x%08X is not 4-byte aligned; current EtherCAT memory protocol cannot read it\n",
               address);
        ava_sdk_close(sdk);
        return 4;
    }
    transfer_size = (size + 3U) & ~3U;
    buffer        = (uint8_t *)calloc(1, transfer_size);
    if (!buffer) {
        ava_sdk_close(sdk);
        return 5;
    }

    SetConsoleCtrlHandler(on_console, true);
    if (rate_hz > 0.0) {
        QueryPerformanceFrequency(&timer_freq);
        period = (LONGLONG)((double)timer_freq.QuadPart / rate_hz + 0.5);
        if (period < 1)
            period = 1;
        QueryPerformanceCounter(&next);
        printf("polling %s @ 0x%08X, size=%u, rate=%g Hz; Ctrl+C to stop\n",
               name,
               address,
               size,
               rate_hz);
    }

    while (InterlockedCompareExchange(&running, 1, 1)) {
        ret = ava_sdk_read_at(sdk, address, buffer, transfer_size);
        if (ret == AVA_SDK_OK) {
            print_value(sdk, name, address, buffer, size);
            consecutive_failures = 0;
        } else {
            fprintf(stderr, "read failed: %s\n", ava_sdk_strerror(ret));
            {
                struct ava_sdk_debug d;
                if (ava_sdk_get_debug(sdk, &d) == AVA_SDK_OK) {
                    fprintf(
                        stderr,
                        "pdo: heartbeat=%u req.seq=%u ack=%u status=%u addr=0x%08X len=%u cmd=%u\n",
                        d.heartbeat,
                        d.req_seq,
                        d.res_ack,
                        d.res_status,
                        d.req_address,
                        d.req_length,
                        d.req_command);
                    fprintf(stderr, "pdo input raw @ base+60:");
                    {
                        unsigned raw_i;
                        for (raw_i = 0; raw_i < sizeof(d.res_raw); ++raw_i)
                            fprintf(stderr, " %02X", d.res_raw[raw_i]);
                    }
                    fputc('\n', stderr);
                }
            }
            if (++consecutive_failures >= 3) {
                fprintf(stderr, "master communication lost; stopping poll loop\n");
                break;
            }
        }

        if (rate_hz <= 0.0)
            break;
        next.QuadPart += period;
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < next.QuadPart);
    }

    free(buffer);
    ava_sdk_close(sdk);
    return 0;
}

#include "ava_sdk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
write_command(int argc, char **argv)
{
    struct ava_sdk *sdk  = NULL;
    const char     *root = NULL, *elf_path = NULL;
    uint8_t        *data = NULL, *verify = NULL;
    uint32_t        address, size, transfer_size, slave = 0;
    char            expression[1024], formatted[16384];
    int             i, ret, assignment_count = 0;
    double          rate_hz = 0.0;
    LARGE_INTEGER   timer_freq, next, now;
    LONGLONG        period = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--elf") == 0 && i + 1 < argc)
            elf_path = argv[++i];
        else if (strcmp(argv[i], "--var") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "--slave") == 0 && i + 1 < argc)
            slave = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc)
            rate_hz = strtod(argv[++i], NULL);
        else if (strchr(argv[i], '=') != NULL)
            ++assignment_count;
        else {
            printf("unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }
    if (!elf_path || !root || assignment_count == 0 || rate_hz < 0.0) {
        printf("usage: %s --elf <axf/elf> --var <struct_variable> <member=value>... [--freq hz] "
               "[--slave index]\n",
               argv[0]);
        return 1;
    }

    ret = ava_sdk_init(&sdk, elf_path, slave);
    if (ret != AVA_SDK_OK) {
        printf("init failed: %s\n", ava_sdk_strerror(ret));
        return 2;
    }
    ret = ava_sdk_query(sdk, root, &address, &size);
    if (ret != AVA_SDK_OK || size == 0U || (address & 3U) != 0U) {
        printf("invalid root variable: %s\n", ava_sdk_strerror(ret));
        ava_sdk_close(sdk);
        return 3;
    }
    transfer_size = (size + 3U) & ~3U;
    data          = (uint8_t *)calloc(1, transfer_size);
    verify        = (uint8_t *)calloc(1, transfer_size);
    if (!data || !verify) {
        ret = 4;
        goto done;
    }
    SetConsoleCtrlHandler(on_console, true);
    if (rate_hz > 0.0) {
        QueryPerformanceFrequency(&timer_freq);
        period = (LONGLONG)((double)timer_freq.QuadPart / rate_hz + 0.5);
        if (period < 1)
            period = 1;
        QueryPerformanceCounter(&next);
    }
write_cycle:
    ret = ava_sdk_read_at(sdk, address, data, transfer_size);
    if (ret != AVA_SDK_OK) {
        printf("read-before-write failed: %s\n", ava_sdk_strerror(ret));
        ret = 5;
        goto done;
    }

    for (i = 1; i < argc; ++i) {
        char    *equal;
        uint32_t member_address, member_size, offset;
        if (strcmp(argv[i], "--elf") == 0 || strcmp(argv[i], "--var") == 0 ||
            strcmp(argv[i], "--slave") == 0 || strcmp(argv[i], "--freq") == 0) {
            ++i;
            continue;
        }
        equal = strchr(argv[i], '=');
        if (!equal || equal == argv[i] || equal[1] == '\0') {
            printf("invalid assignment: %s\n", argv[i]);
            ret = 6;
            goto done;
        }
        if ((size_t)(equal - argv[i]) + strlen(root) + 2U > sizeof(expression)) {
            ret = 6;
            goto done;
        }
        if (strncmp(argv[i], root, strlen(root)) == 0 && argv[i][strlen(root)] == '.')
            snprintf(expression, sizeof(expression), "%.*s", (int)(equal - argv[i]), argv[i]);
        else
            snprintf(
                expression, sizeof(expression), "%s.%.*s", root, (int)(equal - argv[i]), argv[i]);
        ret = ava_sdk_query(sdk, expression, &member_address, &member_size);
        if (ret != AVA_SDK_OK || member_address < address || member_size > size ||
            member_address - address > size - member_size) {
            printf("unknown/out-of-range member: %s\n", expression);
            ret = 7;
            goto done;
        }
        offset = member_address - address;
        ret    = ava_sdk_encode(sdk, expression, equal + 1, data + offset, member_size);
        if (ret != AVA_SDK_OK) {
            printf("value/type mismatch: %s=%s\n", expression, equal + 1);
            ret = 8;
            goto done;
        }
    }

    if (ava_sdk_format(sdk, root, data, size, formatted, sizeof(formatted)) == AVA_SDK_OK)
        printf("write %s @ 0x%08X = %s\n", root, address, formatted);
    ret = ava_sdk_write_at(sdk, address, data, transfer_size);
    if (ret != AVA_SDK_OK) {
        printf("write failed: %s\n", ava_sdk_strerror(ret));
        ret = 9;
        goto done;
    }
    ret = ava_sdk_read_at(sdk, address, verify, transfer_size);
    if (ret != AVA_SDK_OK || memcmp(data, verify, size) != 0) {
        puts("write verification failed");
        ret = 10;
        goto done;
    }
    if (ava_sdk_format(sdk, root, verify, size, formatted, sizeof(formatted)) == AVA_SDK_OK)
        printf("verified = %s\n", formatted);
    ret = 0;
    if (rate_hz > 0.0 && InterlockedCompareExchange(&running, 1, 1)) {
        next.QuadPart += period;
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < next.QuadPart && InterlockedCompareExchange(&running, 1, 1));
        if (InterlockedCompareExchange(&running, 1, 1))
            goto write_cycle;
    }

done:
    free(verify);
    free(data);
    ava_sdk_close(sdk);
    return ret;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage:\n");
        printf("  %s read  --elf <axf/elf> --var <variable> [--freq hz] [--slave index]\n",
               argv[0]);
        printf("  %s write --elf <axf/elf> --var <struct_variable> <member=value>... [--freq hz] "
               "[--slave index]\n",
               argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "read") == 0)
        return read_command(argc - 1, argv + 1);
    if (strcmp(argv[1], "write") == 0)
        return write_command(argc - 1, argv + 1);
    printf("unknown command: %s (expected read or write)\n", argv[1]);
    return 1;
}
