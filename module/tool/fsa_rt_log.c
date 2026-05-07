#include <pthread.h>

#include "module.h"

#include "fsa_rma/fsa_rma_addr.h"
#include "fsa_rma/fsa_rmaio.h"

#define MAX_FSA_NUM        (1)
#define RMAIO_PORT         (2340)
#define MEMPOOL_SIZE       (SIZE_4MB)
#define LOG_BUF_SIZE       (SIZE_2MB)
#define LOG_FLUSH_BUF_SIZE (SIZE_2KB)
#define LOG_FILE_SIZE      (1 * SIZE_1KB) // 日志文件大小限制为 1KB (环形缓冲区)

static void
log_stdout(void *fp, const void *src, const usize size)
{
        fwrite(src, size - 1, 1, fp);
        fflush(fp);
}

log_t g_log;

net_t           g_net;
const net_cfg_t g_net_cfg = {
    .e_type   = NET_TYPE_UDP,
    .f_get_ts = get_real_ts_us,
};

u8        g_mempool_buf[MEMPOOL_SIZE];
mempool_t g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

typedef struct fsa {
        net_ch_t       ch;
        u32            cnt;
        rmaio_fsa2pc_t rmaio_fsa2pc;
        rmaio_pc2fsa_t rmaio_pc2fsa;
} fsa_t;

fsa_t fsas[MAX_FSA_NUM];

const char *const ips[] = {
    "192.168.137.102",
    // "192.168.137.110",
};

HAPI int fsa_get_ref_pvct(fsa_t *fsa, foc_ref_pvct_t *ref_pvct);
HAPI int fsa_get_fdb_pvct(fsa_t *fsa, foc_fdb_pvct_t *fdb_pvct);

void *
flush_thread_func(void *arg)
{
        ARG_UNUSED(arg);
        for (;;)
                log_flush(&g_log);
}

void *
fsa_thread_func(void *arg)
{
        const u32 fsa_idx = *(u32 *)arg;
        fsa_t    *fsa     = &fsas[fsa_idx];

        foc_fdb_pvct_t fdb_pvct;
        foc_ref_pvct_t ref_pvct;
        u64            elapsed_us = 0;

        for (;;) {
                TIMED_EXEC(elapsed_us, MS2US(1000), {
                        int ret = fsa_get_fdb_pvct(fsa, &fdb_pvct);
                        if (ret < 0)
                                log_info(&g_log, fsa_idx, "get fdb_pvct err!\n");

                        ret = fsa_get_ref_pvct(fsa, &ref_pvct);
                        if (ret < 0)
                                log_info(&g_log, fsa_idx, "get ref_pvct err!\n");
                        else
                                log_info(&g_log,
                                         fsa_idx,
                                         "%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%llu\n",
                                         ref_pvct.pos,
                                         fdb_pvct.pos,
                                         ref_pvct.vel,
                                         fdb_pvct.vel,
                                         ref_pvct.cur,
                                         fdb_pvct.cur,
                                         ref_pvct.tor,
                                         fdb_pvct.elec_tor,
                                         elapsed_us);
                });
        }
}

HAPI int
fsa_init(fsa_t *fsa, const char *ip, u16 port)
{
        fsa->ch = net_cfg_ch(IP_STR_TO_U32(ip), port, NET_MODE_SYNC_SPIN);
        net_add_ch(&g_net, &fsa->ch);

        foc_fdb_pvct_t fdb_pvct;
        return fsa_get_fdb_pvct(fsa, &fdb_pvct);
}

HAPI int
fsa_get_ref_pvct(fsa_t *fsa, foc_ref_pvct_t *ref_pvct)
{
        rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt);
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_BASE, 7);

        const int ret = (int)net_send_recv(&g_net,
                                           &fsa->ch,
                                           fsa->rmaio_pc2fsa.send_buf,
                                           fsa->rmaio_pc2fsa.generate_index,
                                           fsa->rmaio_fsa2pc.recv_buf,
                                           sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                           1000);
        if (ret < 0)
                return ret;

        rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);

        ref_pvct->pos     = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_P));
        ref_pvct->vel     = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V));
        ref_pvct->cur     = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C));
        ref_pvct->tor     = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T));
        ref_pvct->ffd_vel = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V_FF));
        ref_pvct->ffd_cur = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C_FF));
        ref_pvct->ffd_tor = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T_FF));

        return ret;
}

HAPI int
fsa_get_fdb_pvct(fsa_t *fsa, foc_fdb_pvct_t *fdb_pvct)
{
        rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt);
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_GET_PVCT_BASE, 5);

        const int ret = (int)net_send_recv(&g_net,
                                           &fsa->ch,
                                           fsa->rmaio_pc2fsa.send_buf,
                                           fsa->rmaio_pc2fsa.generate_index,
                                           fsa->rmaio_fsa2pc.recv_buf,
                                           sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                           1000);
        if (ret < 0)
                return ret;

        rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);

        fdb_pvct->pos      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_P));
        fdb_pvct->vel      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_V));
        fdb_pvct->cur      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_C));
        fdb_pvct->load_tor = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_T));
        fdb_pvct->elec_tor = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_TE));

        return ret;
}

int
main(void)
{
        mempool_init(&g_mempool);

        FILE *file = fopen("fsa_rt.log", "w");

        const log_cfg_t log_cfg = {
            .e_mode     = LOG_MODE_SYNC,
            .e_level    = LOG_LEVEL_DATA,
            .mempool    = &g_mempool,
            .fd         = file,
            .file_size  = LOG_FILE_SIZE,
            .e_ring     = LOG_RING_COMPLETE,
            .cap        = LOG_BUF_SIZE,
            .flush_cap  = LOG_FLUSH_BUF_SIZE,
            .nproducers = MAX_FSA_NUM,
            .f_get_ts   = get_real_ts_us,
            .f_flush    = log_stdout,
        };
        log_init(&g_log, log_cfg);

        net_init(&g_net, g_net_cfg);

        pthread_t flush_thread;
        pthread_create(&flush_thread, NULL, flush_thread_func, NULL);

        const u32 fsa_cnt = ARRAY_LEN(ips);
        for (u32 i = 0; i < fsa_cnt; i++) {
                const int ret = fsa_init(&fsas[i], ips[i], RMAIO_PORT);
                if (ret < 0)
                        print_error(FALSE, "failed to initialize fsa[%u], error: %d", i, ret);
                else
                        print_success(FALSE, "initialized fsa[%u] (IP: %s)", i, ips[i]);
        }

        u32       fsa_indices[MAX_FSA_NUM];
        pthread_t fsa_threads[MAX_FSA_NUM];
        for (u32 i = 0; i < fsa_cnt; i++) {
                fsa_indices[i] = i;
                const int ret  = pthread_create(&fsa_threads[i], NULL, fsa_thread_func, &fsa_indices[i]);
                if (ret != 0)
                        print_error(FALSE, "failed to create thread for fsa[%u], error: %d", i, ret);
                else
                        print_success(FALSE, "created thread for fsa[%u] (IP: %s)", i, ips[i]);
        }

        pthread_join(flush_thread, NULL);
        for (u32 i = 0; i < fsa_cnt; i++)
                pthread_join(fsa_threads[i], NULL);

        log_deinit(&g_log);

        return 0;
}
