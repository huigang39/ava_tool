#include "ava_ecat.h"
#include "soem/soem.h"

#include <mmsystem.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define IO_MAP_BYTES     32768U
#define DEFAULT_CYCLE_US 1000U
#define FSA_VENDOR_ID    0x46494946U
#define FSA_PRODUCT_CODE 0x41434341U
#define SDO_TIMEOUT_US   200000

struct master {
    ecx_contextt       context;
    uint8_t            iomap[IO_MAP_BYTES];
    HANDLE             map_handle;
    struct shm_header *shm;
    uint32_t           cycle_us;
    int                expected_wkc;
};

static volatile LONG g_run = 1;

static BOOL WINAPI
console_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_run, 0);
        return true;
    }
    return false;
}

static void
list_adapters(void)
{
    ec_adaptert *head = ec_find_adapters();
    ec_adaptert *it   = head;

    puts("Available adapters:");
    while (it != NULL) {
        printf("  %s\n    %s\n", it->name, it->desc);
        it = it->next;
    }
    ec_free_adapters(head);
}

static int
sdo_write(
    struct master *m, uint16_t slave, uint16_t index, uint8_t subindex, const void *data, int size)
{
    int bytes = size;
    int wkc = ecx_SDOwrite(&m->context, slave, index, subindex, false, bytes, data, SDO_TIMEOUT_US);
    if (wkc <= 0) {
        ec_slavet *s = &m->context.slavelist[slave];
        fprintf(stderr,
                "SDO write failed: slave=%u %04X:%02X state=0x%02X AL=0x%04X (%s)\n",
                slave,
                index,
                subindex,
                s->state,
                s->ALstatuscode,
                ec_ALstatuscode2string(s->ALstatuscode));
        while (ecx_iserror(&m->context))
            fprintf(stderr, "  SOEM: %s\n", ecx_elist2string(&m->context));
    }
    return wkc > 0;
}

static int
assign_pdos(
    struct master *m, uint16_t slave, uint16_t assign_index, uint16_t first, uint16_t second)
{
    uint8_t count = 0;
    if (!sdo_write(m, slave, assign_index, 0, &count, sizeof(count)))
        return 0;
    if (!sdo_write(m, slave, assign_index, 1, &first, sizeof(first)))
        return 0;
    if (!sdo_write(m, slave, assign_index, 2, &second, sizeof(second)))
        return 0;
    count = 2;
    return sdo_write(m, slave, assign_index, 0, &count, sizeof(count));
}

static int
assign_default_pdo(struct master *m, uint16_t slave, uint16_t assign_index, uint16_t pdo_index)
{
    uint8_t count = 0;
    if (!sdo_write(m, slave, assign_index, 0, &count, sizeof(count)))
        return 0;
    if (!sdo_write(m, slave, assign_index, 1, &pdo_index, sizeof(pdo_index)))
        return 0;
    count = 1;
    return sdo_write(m, slave, assign_index, 0, &count, sizeof(count));
}

static void
restore_default_pdos(struct master *m)
{
    int i;
    if (m->context.slavecount <= 0)
        return;

    m->context.slavelist[0].state = EC_STATE_PRE_OP;
    ecx_writestate(&m->context, 0);
    ecx_statecheck(&m->context, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);

    for (i = 1; i <= m->context.slavecount; ++i) {
        if (!assign_default_pdo(m, (uint16_t)i, 0x1C12, 0x1600) ||
            !assign_default_pdo(m, (uint16_t)i, 0x1C13, 0x1A00))
            fprintf(stderr, "Warning: failed to restore default PDOs on slave %d\n", i);
    }
}
static int
configure_fsa_pdos(struct master *m)
{
    int i;
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet *s = &m->context.slavelist[i];
        if (s->eep_man != FSA_VENDOR_ID || s->eep_id != FSA_PRODUCT_CODE) {
            fprintf(stderr,
                    "Slave %d is not FSANeo (vendor=%08X product=%08X)\n",
                    i,
                    s->eep_man,
                    s->eep_id);
            return 0;
        }
        if (!assign_pdos(m, (uint16_t)i, 0x1C12, 0x1600, 0x1603) ||
            !assign_pdos(m, (uint16_t)i, 0x1C13, 0x1A00, 0x1A03))
            return 0;
    }
    return 1;
}

static int
open_shared_memory(struct master *m)
{
    m->map_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(struct shm_header), SHM_NAME);
    if (m->map_handle == NULL)
        return 0;

    m->shm = (struct shm_header *)MapViewOfFile(
        m->map_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct shm_header));
    if (m->shm == NULL)
        return 0;

    memset(m->shm, 0, sizeof(*m->shm));
    m->shm->magic       = SHM_MAGIC;
    m->shm->version     = 1;
    m->shm->slave_count = (uint32_t)m->context.slavecount;
    return 1;
}

static void
close_shared_memory(struct master *m)
{
    if (m->shm != NULL) {
        m->shm->magic = 0;
        UnmapViewOfFile(m->shm);
        m->shm = NULL;
    }
    if (m->map_handle != NULL) {
        CloseHandle(m->map_handle);
        m->map_handle = NULL;
    }
}

static int
copy_layout_to_shm(struct master *m)
{
    int i;
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet            *s = &m->context.slavelist[i];
        struct shm_slave_pdo *d = &m->shm->slaves[i - 1];
        if (s->Obytes > SHM_MAX_PDO_SIZE || s->Ibytes > SHM_MAX_PDO_SIZE) {
            fprintf(stderr, "Slave %d PDO exceeds shared-memory capacity\n", i);
            return 0;
        }
        d->output_size = s->Obytes;
        d->input_size  = s->Ibytes;
        printf("Slave %d %s: physical PDO=%u/%u, SDK image=%u/%u bytes, startbit=%u/%u\n",
               i,
               s->name,
               s->Obytes,
               s->Ibytes,
               d->output_size,
               d->input_size,
               s->Ostartbit,
               s->Istartbit);
        if (s->Obytes != FSA_NEO_RX_PDO_SIZE || s->Ibytes != FSA_NEO_TX_PDO_SIZE) {
            fprintf(stderr,
                    "Unexpected extended FSANeo PDO size; expected %u/%u\n",
                    (unsigned)FSA_NEO_RX_PDO_SIZE,
                    (unsigned)FSA_NEO_TX_PDO_SIZE);
            return 0;
        }
    }
    return 1;
}

static void
copy_outputs(struct master *m)
{
    int i;
    MemoryBarrier();
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet *s = &m->context.slavelist[i];
        memcpy(s->outputs, m->shm->slaves[i - 1].output_data, s->Obytes);
    }
}

static void
copy_inputs(struct master *m)
{
    int i;
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet *s = &m->context.slavelist[i];
        memcpy(m->shm->slaves[i - 1].input_data, s->inputs, s->Ibytes);
    }
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)&m->shm->heartbeat);
}

static int
roundtrip(struct master *m)
{
    copy_outputs(m);
    ecx_send_processdata(&m->context);
    {
        int wkc = ecx_receive_processdata(&m->context, EC_TIMEOUTRET);
        if (wkc > 0) {
            copy_inputs(m);
        }
        return wkc;
    }
}

static void
recover_slaves(struct master *m)
{
    int i;
    ecx_readstate(&m->context);
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet *s = &m->context.slavelist[i];
        if (s->state == EC_STATE_OPERATIONAL)
            continue;
        if (s->state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
            s->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ecx_writestate(&m->context, (uint16_t)i);
        } else if (s->state == EC_STATE_SAFE_OP) {
            s->state = EC_STATE_OPERATIONAL;
            ecx_writestate(&m->context, (uint16_t)i);
        } else if (s->state > EC_STATE_NONE) {
            ecx_reconfig_slave(&m->context, (uint16_t)i, EC_TIMEOUTSTATE);
        } else {
            ecx_recover_slave(&m->context, (uint16_t)i, EC_TIMEOUTSTATE);
        }
    }
}

static int
start_master(struct master *m, const char *adapter)
{
    ec_slavet *broadcast;
    ec_groupt *group;
    int        i;

    if (!ecx_init(&m->context, adapter)) {
        fprintf(
            stderr, "Cannot open adapter: %s (install Npcap and run as Administrator)\n", adapter);
        return 0;
    }
    if (ecx_config_init(&m->context) <= 0) {
        fprintf(stderr, "No EtherCAT slaves found\n");
        return 0;
    }
    if (m->context.slavecount > SHM_MAX_SLAVES) {
        fprintf(stderr, "Too many slaves\n");
        return 0;
    }
    ecx_readstate(&m->context);
    for (i = 1; i <= m->context.slavecount; ++i) {
        ec_slavet *s = &m->context.slavelist[i];
        printf("Slave %d discovered: %s state=0x%02X vendor=%08X product=%08X\n",
               i,
               s->name,
               s->state,
               s->eep_man,
               s->eep_id);
        if (s->state != EC_STATE_PRE_OP) {
            s->state = EC_STATE_PRE_OP;
            ecx_writestate(&m->context, (uint16_t)i);
            ecx_statecheck(&m->context, (uint16_t)i, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
        }
    }
    if (!configure_fsa_pdos(m))
        return 0;

    if (ecx_config_map_group(&m->context, m->iomap, 0) <= 0) {
        fprintf(stderr, "PDO mapping failed\n");
        return 0;
    }
    ecx_configdc(&m->context);
    group           = &m->context.grouplist[0];
    m->expected_wkc = group->outputsWKC * 2 + group->inputsWKC;

    if (!open_shared_memory(m) || !copy_layout_to_shm(m))
        return 0;

    ecx_statecheck(&m->context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    roundtrip(m);
    broadcast        = &m->context.slavelist[0];
    broadcast->state = EC_STATE_OPERATIONAL;
    ecx_writestate(&m->context, 0);
    for (i = 0; i < 40; ++i) {
        roundtrip(m);
        ecx_statecheck(&m->context, 0, EC_STATE_OPERATIONAL, 50000);
        if (broadcast->state == EC_STATE_OPERATIONAL)
            break;
    }
    if (broadcast->state != EC_STATE_OPERATIONAL) {
        fprintf(stderr, "Failed to enter OP\n");
        recover_slaves(m);
        return 0;
    }
    printf("OP reached: %d slave(s), expected WKC=%d, cycle=%u us\n",
           m->context.slavecount,
           m->expected_wkc,
           m->cycle_us);
    return 1;
}

static void
run_cycles(struct master *m)
{
    LARGE_INTEGER freq, next, now;
    unsigned      bad_wkc = 0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);

    while (InterlockedCompareExchange(&g_run, 1, 1)) {
        int wkc = roundtrip(m);
        if (wkc < m->expected_wkc) {
            if (++bad_wkc >= 10) {
                fprintf(stderr, "WKC %d/%d; recovering slaves\n", wkc, m->expected_wkc);
                recover_slaves(m);
                bad_wkc = 0;
            }
        } else {
            bad_wkc = 0;
        }

        next.QuadPart += (LONGLONG)m->cycle_us * freq.QuadPart / 1000000LL;
        do {
            QueryPerformanceCounter(&now);
            if (next.QuadPart - now.QuadPart > freq.QuadPart / 500)
                Sleep(1);
            else
                YieldProcessor();
        } while (now.QuadPart < next.QuadPart && InterlockedCompareExchange(&g_run, 1, 1));
    }
}

static void
stop_master(struct master *m)
{
    if (m->context.slavecount > 0) {
        puts("Restoring default FSANeo PDO assignment...");
        restore_default_pdos(m);
        m->context.slavelist[0].state = EC_STATE_INIT;
        ecx_writestate(&m->context, 0);
    }
    close_shared_memory(m);
    ecx_close(&m->context);
}

static void
usage(const char *exe)
{
    printf(
        "Usage:\n  %s --list-adapters\n  %s --adapter <NPF device> [--cycle-us 1000]\n", exe, exe);
}

int
main(int argc, char **argv)
{
    struct master master;
    const char   *adapter = NULL;
    int           i;

    memset(&master, 0, sizeof(master));
    master.cycle_us = DEFAULT_CYCLE_US;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list-adapters") == 0) {
            list_adapters();
            return 0;
        }
        if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc)
            adapter = argv[++i];
        else if (strcmp(argv[i], "--cycle-us") == 0 && i + 1 < argc)
            master.cycle_us = (uint32_t)strtoul(argv[++i], NULL, 0);
    }
    if (adapter == NULL || master.cycle_us < 250U) {
        usage(argv[0]);
        return 1;
    }

    SetConsoleCtrlHandler(console_handler, true);
    timeBeginPeriod(1);
    if (!start_master(&master, adapter)) {
        stop_master(&master);
        timeEndPeriod(1);
        return 2;
    }
    run_cycles(&master);
    stop_master(&master);
    timeEndPeriod(1);
    return 0;
}
