#include "module.h"

#include <pthread.h>
#include <stdio.h>

static void log_stdout(void *fp, const void *src, usize size);

#define WRITE_THREAD_NUM (1000)
u64 g_producers_cnts[WRITE_THREAD_NUM];

u8       g_log_flush_buf[128];
u8       g_log_buf[1024 * 1024];
mpsc_p_t g_producers[WRITE_THREAD_NUM];

log_t     g_log;
log_cfg_t g_log_cfg = {
    .e_mode     = LOG_MODE_SYNC,
    .e_level    = LOG_LEVEL_DEBUG,
    .buf        = (void *)g_log_buf,
    .cap        = sizeof(g_log_buf),
    .flush_buf  = g_log_flush_buf,
    .flush_cap  = sizeof(g_log_flush_buf),
    .producers  = (mpsc_p_t *)&g_producers,
    .nproducers = ARRAY_LEN(g_producers),
    .f_flush    = log_stdout,
    .f_get_ts   = get_mono_ts_us,
};

static void
log_stdout(void *fp, const void *src, const usize size)
{
        fwrite(src, size, 1, fp);
        fflush(fp);
}

void *
flush_thread_func(void *arg)
{
        ARG_UNUSED(arg);
        for (;;)
                log_flush(&g_log);
}

void *
write_thread_func(void *arg)
{
        const u64 id = *(u64 *)arg;

        for (;;) {
#ifdef _WIN32
                log_debug(&g_log, id, "\tthread %10llu, cnt: %10llu\n", (usize)GetCurrentThreadId(), g_producers_cnts[id]++);
#else
                log_debug(&g_log, id, "\tthread %10llu, cnt: %10llu\n", (usize)pthread_self(), g_producers_cnts[id]++);
#endif

                delay_ms(1, YIELD);
        }
}

int
main()
{
        g_log_cfg.fp = stdout;
        log_init(&g_log, g_log_cfg);

        pthread_t flush_thread;
        pthread_create(&flush_thread, NULL, flush_thread_func, NULL);

        u64       thread_ids[WRITE_THREAD_NUM];
        pthread_t write_thread[WRITE_THREAD_NUM];
        for (u32 i = 0; i < WRITE_THREAD_NUM - 1; i++) {
                thread_ids[i] = i;
                pthread_create(&write_thread[i], NULL, write_thread_func, &thread_ids[i]);
        }

        for (u32 i = 0; i < WRITE_THREAD_NUM - 1; i++)
                pthread_join(write_thread[i], NULL);

        pthread_join(flush_thread, NULL);

        return 0;
}
