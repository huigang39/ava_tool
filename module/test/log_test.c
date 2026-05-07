#include "module.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

static void log_stdout(void *fp, const void *src, usize size);

#define WRITE_THREAD_NUM   (1000)
#define MEMPOOL_SIZE       (SIZE_16MB)

#define LOG_CHUNK_SIZE     (SIZE_4KB)
#define LOG_FLUSH_BUF_SIZE (SIZE_1KB)

u64   g_producers_cnts[WRITE_THREAD_NUM];
log_t g_log;

u8        g_mempool_buf[MEMPOOL_SIZE];
mempool_t g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

volatile bool g_stop_flush = false;

static void
log_stdout(void *fp, const void *src, const usize size)
{
        if (fp != NULL) {
                fwrite(src, size, 1, fp);
                fflush(fp);
        }
}

void *
flush_thread_func(void *arg)
{
        ARG_UNUSED(arg);

        while (!g_stop_flush) {
                log_flush(&g_log);
                delay_ms(1, DELAY_YIELD);
        }

        // 收到主线程安全退出信号后，再执行一次最终收尾
        log_flush(&g_log);
        return NULL;
}

void *
write_thread_func(void *arg)
{
        const u64 idx = *(u64 *)arg;

        for (int i = 0; i < 1000; i++) {

#ifdef _WIN32
                log_debug(&g_log, idx, "thread_id %10llu, cnt: %10llu\n", (usize)GetCurrentThreadId(), g_producers_cnts[idx]++);
#else
                log_debug(&g_log, idx, "thread_id %10llu, cnt: %10llu\n", (usize)pthread_self(), g_producers_cnts[idx]++);
#endif
                // delay_ms(1, YIELD);
        }

        return NULL;
}

int
main()
{
        mempool_init(&g_mempool);

        const log_cfg_t log_cfg = {
            .e_mode   = LOG_MODE_SYNC,
            .e_level  = LOG_LEVEL_DEBUG,
            .e_format = LOG_FORMAT_TEXT,
            .mempool  = &g_mempool,

            //     .file_path = "test_log.log",
            .file_size = SIZE_1MB,
            .max_files = 3,
            .e_ring    = LOG_RING_ROTATE,

            .fd      = stdout,
            .f_flush = log_stdout,

            .chunk_size = LOG_CHUNK_SIZE,
            .flush_cap  = LOG_FLUSH_BUF_SIZE,
            .nproducers = WRITE_THREAD_NUM,
            .f_get_ts   = get_mono_ts_us,
        };
        log_init(&g_log, log_cfg);

        pthread_t flush_thread;
        pthread_create(&flush_thread, NULL, flush_thread_func, NULL);

        u64       thread_ids[WRITE_THREAD_NUM];
        pthread_t write_thread[WRITE_THREAD_NUM];
        for (u32 i = 0; i < WRITE_THREAD_NUM; i++) {
                thread_ids[i] = i;
                pthread_create(&write_thread[i], NULL, write_thread_func, &thread_ids[i]);
        }

        // 1. 挂起等待所有生产者的写动作完毕
        for (u32 i = 0; i < WRITE_THREAD_NUM; i++)
                pthread_join(write_thread[i], NULL);

        // 2. 告诉后台落盘线程安全退出
        g_stop_flush = true;
        pthread_join(flush_thread, NULL);

        // 3. 安全地回收模块，它会自动将所有还没满的 Chunk 刷进文件里
        log_deinit(&g_log);

        printf("\nWait-Free Chunk MPSC Log Test Complete!\n");
        return 0;
}
