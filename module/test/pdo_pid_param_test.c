#include "sdk/ava_debug/inc/ava_ecat.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef CM7_ADDR_VEL_PI_KP
#define CM7_ADDR_VEL_PI_KP (0x2000366C)
#endif
#ifndef CM7_ADDR_VEL_PI_KI
#define CM7_ADDR_VEL_PI_KI (0x20003670)
#endif
#ifndef CM7_ADDR_POS_P_KP
#define CM7_ADDR_POS_P_KP (0x20001D3C)
#endif
#ifndef CM7_ADDR_POS_VEL_PD_KP
#define CM7_ADDR_POS_VEL_PD_KP 0U
#endif
#ifndef CM7_ADDR_POS_VEL_PD_KD
#define CM7_ADDR_POS_VEL_PD_KD 0U
#endif

struct pid_param_addr {
    const char *name;
    uint32_t    addr;
};

static const struct pid_param_addr s_pid_param_addrs[] = {
    {"vel_pair", CM7_ADDR_VEL_PI_KP},
    {"vel_kp", CM7_ADDR_VEL_PI_KP},
    {"vel_ki", CM7_ADDR_VEL_PI_KI},
    {"pos_kp", CM7_ADDR_POS_P_KP},
    {"pd_kp", CM7_ADDR_POS_VEL_PD_KP},
    {"pd_kd", CM7_ADDR_POS_VEL_PD_KD},
};

static void
usage(const char *argv0)
{
    printf("usage:\n");
    printf("  %s read  <name|addr> [slave_idx]\n", argv0);
    printf("  %s write <name|addr> <float_value> [slave_idx]\n", argv0);
    printf("\n");
    printf("names: vel_kp vel_ki pos_kp pd_kp pd_kd\n");
    printf("example:\n");
    printf("  %s read  vel_kp\n", argv0);
    printf("  %s write vel_kp 2.0\n", argv0);
    printf("  %s read  0x20001234\n", argv0);
}

static uint32_t
parse_addr_or_name(const char *s)
{
    char    *end = NULL;
    size_t   i;
    uint32_t addr;

    for (i = 0; i < sizeof(s_pid_param_addrs) / sizeof(s_pid_param_addrs[0]); i++) {
        if (strcmp(s, s_pid_param_addrs[i].name) == 0) {
            return s_pid_param_addrs[i].addr;
        }
    }

    addr = (uint32_t)strtoul(s, &end, 0);
    if ((end != NULL) && (*end == '\0')) {
        return addr;
    }

    return 0U;
}

static const char *
status_name(uint8_t status)
{
    switch (status) {
        case AVA_ECAT_PDO_STATUS_IDLE:
            return "IDLE";
        case AVA_ECAT_PDO_STATUS_OK:
            return "OK";
        case AVA_ECAT_PDO_STATUS_BAD_CMD:
            return "BAD_CMD";
        case AVA_ECAT_PDO_STATUS_BAD_ADDR:
            return "BAD_ADDR";
        case AVA_ECAT_PDO_STATUS_BAD_LEN:
            return "BAD_LEN";
        case AVA_ECAT_PDO_STATUS_UNALIGNED:
            return "UNALIGNED";
        default:
            return "UNKNOWN";
    }
}

static void
print_pdo_debug_state(const struct ava_ecat_pdo *mem, uint32_t addr)
{
    const int stale = mem->res->ack != mem->req->seq;
    printf("pdo debug: req.seq=%u ack=%u status=%u(%s%s) addr=0x%08X len=%u cmd=%u\n",
           mem->req->seq,
           mem->res->ack,
           mem->res->status,
           stale ? "STALE_" : "",
           stale ? "ACK_TIMEOUT" : status_name(mem->res->status),
           addr,
           mem->req->length,
           mem->req->command);
    {
        const volatile uint8_t *req = (const volatile uint8_t *)mem->req;
        const volatile uint8_t *res = (const volatile uint8_t *)mem->res;
        int                     i;
        printf("req raw:");
        for (i = -8; i < 24; ++i)
            printf(" %02X", req[i]);
        printf("\nres raw:");
        for (i = -8; i < 24; ++i)
            printf(" %02X", res[i]);
        printf("\n");
    }
}
static int
wait_one_pdo_cycle(void *user, uint32_t seq)
{
    (void)user;
    (void)seq;
#ifdef _WIN32
    Sleep(1);
#endif
    return 0;
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
    HANDLE             map_handle;
    struct shm_header *shm;
#endif
    uint32_t            slave_idx = 0U;
    uint32_t            addr;
    struct ava_ecat_pdo mem;
    int                 ret;

    if ((argc < 3) || ((strcmp(argv[1], "write") == 0) && (argc < 4))) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "read") != 0 && strcmp(argv[1], "write") != 0) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "read") == 0 && argc >= 4) {
        slave_idx = (uint32_t)strtoul(argv[3], NULL, 0);
    } else if (strcmp(argv[1], "write") == 0 && argc >= 5) {
        slave_idx = (uint32_t)strtoul(argv[4], NULL, 0);
    }

    addr = parse_addr_or_name(argv[2]);
    if (addr == 0U) {
        printf("invalid or unresolved address/name: %s\n", argv[2]);
        printf("edit CM7_ADDR_* macros or pass a raw address from CM7 map/elf.\n");
        return 2;
    }

#ifdef _WIN32
    map_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, SHM_NAME);
    if (map_handle == NULL) {
        printf("OpenFileMappingA(%s) failed, err=%lu\n", SHM_NAME, GetLastError());
        return 3;
    }

    shm = (struct shm_header *)MapViewOfFile(
        map_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct shm_header));
    if (shm == NULL) {
        printf("MapViewOfFile failed, err=%lu\n", GetLastError());
        CloseHandle(map_handle);
        return 4;
    }

    if ((shm->magic != SHM_MAGIC) || (shm->version != 1U)) {
        printf("invalid shared memory magic/version: 0x%08X/%u\n", shm->magic, shm->version);
        UnmapViewOfFile(shm);
        CloseHandle(map_handle);
        return 5;
    }

    if (slave_idx >= shm->slave_count) {
        printf("slave_idx %u out of range, slave_count=%u\n", slave_idx, shm->slave_count);
        UnmapViewOfFile(shm);
        CloseHandle(map_handle);
        return 6;
    }

    ret = ava_ecat_pdo_init(&mem,
                            shm->slaves[slave_idx].output_data,
                            shm->slaves[slave_idx].output_size,
                            shm->slaves[slave_idx].input_data,
                            shm->slaves[slave_idx].input_size,
                            wait_one_pdo_cycle,
                            NULL);
    if (ret != AVA_ECAT_OK) {
        printf("ava_ecat_pdo_init failed: %d, output_size=%u, input_size=%u\n",
               ret,
               shm->slaves[slave_idx].output_size,
               shm->slaves[slave_idx].input_size);
        if ((shm->slaves[slave_idx].output_size == FSA_NEO_BASE_RX_PDO_SIZE) &&
            (shm->slaves[slave_idx].input_size == FSA_NEO_BASE_TX_PDO_SIZE)) {
            printf("default PDO is active, RAM debug PDO window is not mapped.\n");
            printf("Configure the master PDO assignment to Rx {0x1600,0x1603} and Tx "
                   "{0x1A00,0x1A03}, then restart "
                   "the freerun daemon.\n");
        }
        UnmapViewOfFile(shm);
        CloseHandle(map_handle);
        return 7;
    }

    if (strcmp(argv[1], "read") == 0) {
        float    values[2] = {0.0F, 0.0F};
        uint32_t  read_size = strcmp(argv[2], "vel_pair") == 0 ? 8U : 4U;
        ret                 = ava_ecat_pdo_read(&mem, addr, values, read_size, 100U);
        if (ret == AVA_ECAT_OK) {
            if (read_size == 8U)
                printf("vel_pair @ 0x%08X: kp=%.9g ki=%.9g ack=%u\n",
                       addr,
                       values[0],
                       values[1],
                       mem.res->ack);
            else
                printf("%s @ 0x%08X = %.9g ack=%u\n", argv[2], addr, values[0], mem.res->ack);
        } else {
            printf("read failed: %d\n", ret);
            print_pdo_debug_state(&mem, addr);
        }
    } else {
        float value = (float)strtod(argv[3], NULL);
        ret         = ava_ecat_pdo_write(&mem, addr, &value, sizeof(value), 100U);
        if (ret == AVA_ECAT_OK) {
            printf("%s @ 0x%08X <= %.9g\n", argv[2], addr, value);
        } else {
            printf("write failed: %d\n", ret);
            print_pdo_debug_state(&mem, addr);
        }
    }

    UnmapViewOfFile(shm);
    CloseHandle(map_handle);
    return ret == AVA_ECAT_OK ? 0 : 8;
#else
    (void)addr;
    (void)slave_idx;
    (void)mem;
    (void)ret;
    printf("pdo_pid_param_test currently supports Windows shared memory only.\n");
    return 9;
#endif
}
