#include "../module.h"

#include <pthread.h>

#include "fsa_rma/fsa_rma_addr.h"
#include "fsa_rma/fsa_rmaio.h"

#define MAX_FSA_NUM (255)
#define RMAIO_PORT  (2340)

static void
log_stdout(void *fp, const void *src, const usize size)
{
        fwrite(src, size, 1, fp);
        fflush(fp);
}

net_t           g_net;
const net_cfg_t g_net_cfg = {
    .e_type   = NET_TYPE_UDP,
    .f_get_ts = get_real_ts_us,
};

log_t     g_log;
u8        g_log_buf[1024 * 1024], g_flush_buf[1024];
mpsc_p_t  g_producers[MAX_FSA_NUM];
log_cfg_t g_log_cfg = {
    .e_mode     = LOG_MODE_SYNC,
    .e_level    = LOG_LEVEL_DATA,
    .buf        = g_log_buf,
    .cap        = sizeof(g_log_buf),
    .flush_buf  = g_flush_buf,
    .flush_cap  = sizeof(g_flush_buf),
    .producers  = g_producers,
    .nproducers = ARRAY_LEN(g_producers),
    .f_get_ts   = get_real_ts_us,
    .f_flush    = log_stdout,
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
    "192.168.137.110",
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
                TIMED_EXEC(elapsed_us, 1000, {
                        int ret = fsa_get_fdb_pvct(fsa, &fdb_pvct);
                        if (ret < 0)
                                log_info(&g_log, fsa_idx, "get fdb_pvct err!\n");

                        ret = fsa_get_ref_pvct(fsa, &ref_pvct);
                        if (ret < 0)
                                log_info(&g_log, fsa_idx, "get ref_pvct err!\n");
                        else
                                log_info(&g_log,
                                         fsa_idx,
                                         "[%s] ref_pos: %f, fdb_pos: %f;\t ref_vel: "
                                         "%f, fdb_vel: %f;\t ref_cur: %f, fdb_cur: "
                                         "%f;\tref_tor: %f, fdb_tor: %f;\telapsed: "
                                         "%llu us.\n",
                                         ips[fsa_idx],
                                         ref_pvct.pos,
                                         fdb_pvct.pos,
                                         ref_pvct.vel,
                                         fdb_pvct.vel,
                                         ref_pvct.cur,
                                         fdb_pvct.cur,
                                         ref_pvct.elec_tor,
                                         fdb_pvct.elec_tor,
                                         elapsed_us);
                });
        }

        return NULL;
}

HAPI int
fsa_init(fsa_t *fsa, const char *ip, u16 port)
{
        fsa->ch = net_cfg_ch(IP_STR_TO_U32(ip), port, NET_MODE_SYNC_SPIN);
        net_add_ch(&g_net, &fsa->ch);
        return 0;
}

HAPI int
fsa_get_ref_pvct(fsa_t *fsa, foc_ref_pvct_t *ref_pvct)
{
        rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt);
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_BASE, 7);

        const int ret = net_send_recv(&g_net,
                                      &fsa->ch,
                                      fsa->rmaio_pc2fsa.send_buf,
                                      fsa->rmaio_pc2fsa.generate_index,
                                      fsa->rmaio_fsa2pc.recv_buf,
                                      sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                      1000);
        if (ret < 0)
                return ret;

        rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);

        ref_pvct->pos      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_P));
        ref_pvct->vel      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V));
        ref_pvct->cur      = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C));
        ref_pvct->elec_tor = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T));
        ref_pvct->ffd_vel  = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V_FF));
        ref_pvct->ffd_cur  = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C_FF));
        ref_pvct->ffd_tor  = u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T_FF));

        return ret;
}

HAPI int
fsa_get_fdb_pvct(fsa_t *fsa, foc_fdb_pvct_t *fdb_pvct)
{
        rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt);
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_GET_PVCT_BASE, 5);

        const int ret = net_send_recv(&g_net,
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
        g_log_cfg.fp = stdout;
        log_init(&g_log, g_log_cfg);
        net_init(&g_net, g_net_cfg);

        pthread_t flush_thread;
        pthread_create(&flush_thread, NULL, flush_thread_func, NULL);

        const u32 fsa_cnt = ARRAY_LEN(ips);
        for (u32 i = 0; i < fsa_cnt; i++)
                fsa_init(&fsas[i], ips[i], RMAIO_PORT);

        u32       fsa_indices[MAX_FSA_NUM];
        pthread_t fsa_threads[MAX_FSA_NUM];
        for (u32 i = 0; i < fsa_cnt; i++) {
                fsa_indices[i] = i;
                const int ret  = pthread_create(&fsa_threads[i], NULL, fsa_thread_func, &fsa_indices[i]);
                if (ret != 0)
                        print_error(0, "Failed to create thread for fsa[%u], error: %d", i, ret);
                else
                        print_success(0, "Created thread for fsa[%u] (IP: %s)", i, ips[i]);
        }

        pthread_join(flush_thread, NULL);
        for (u32 i = 0; i < fsa_cnt; i++)
                pthread_join(fsa_threads[i], NULL);

        return 0;
}
