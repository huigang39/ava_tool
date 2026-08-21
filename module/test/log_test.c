#include "module.h"

#if !OS(WIN)
#include <pthread.h>
#endif
#include <stdbool.h>
#include <stdio.h>

static void log_stdout(void *fp, const void *src, size_t size);

#define WRITE_THREAD_NUM   (100)
#define MEMPOOL_SIZE       (SIZE_16MB)

#define LOG_CHUNK_SIZE     (SIZE_4KB)
#define LOG_FLUSH_BUF_SIZE (SIZE_1KB)

NO_ASAN
ALIGN(4096)
uint64_t   g_producers_cnts[WRITE_THREAD_NUM];
struct log g_log;

NO_ASAN
ALIGN(4096)
uint8_t        g_mempool_buf[MEMPOOL_SIZE];
struct mempool g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

volatile bool g_stop_flush = false;

static void
log_stdout(void *fp, const void *src, const size_t size)
{
    if (fp != NULL) {
        fwrite(src, size, 1, fp);
        fflush(fp);
    }
}

#if OS(WIN)
static DWORD WINAPI
flush_thread_func(LPVOID arg)
#else
static void *
flush_thread_func(void *arg)
#endif
{
    ARG_UNUSED(arg);

    while (!g_stop_flush) {
        log_flush(&g_log);
        delay_ms(1, DELAY_YIELD);
    }

    // 收到主线程安全退出信号后,再执行一次最终收尾
    log_flush(&g_log);
#if OS(WIN)
    return 0;
#else
    return NULL;
#endif
}

#if OS(WIN)
static DWORD WINAPI
write_thread_func(LPVOID arg)
#else
static void *
write_thread_func(void *arg)
#endif
{
    const uint64_t idx = *(uint64_t *)arg;

    for (int i = 0; i < 1000; i++) {

#if OS(WIN)
        log_debug(&g_log,
                  idx,
                  "thread_id %10LLU, cnt: %10LLU\n",
                  (size_t)GetCurrentThreadId(),
                  g_producers_cnts[idx]++);
#else
        log_debug(&g_log,
                  idx,
                  "thread_id %10LLU, cnt: %10LLU\n",
                  (size_t)pthread_self(),
                  g_producers_cnts[idx]++);
#endif
        // delay_ms(1, YIELD);
    }

#if OS(WIN)
    return 0;
#else
    return NULL;
#endif
}

int
main()
{
    mempool_init(&g_mempool);

    const struct log_cfg log_cfg = {
        .e_mode   = LOG_MODE_SYNC,
        .e_level  = LOG_LEVEL_DEBUG,
        .e_format = LOG_FORMAT_TXT,
        .mempool  = &g_mempool,

        //     .file_path = "log_test.log",
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

#if OS(WIN)
    HANDLE flush_thread = CreateThread(NULL, 0, flush_thread_func, NULL, 0, NULL);
#else
    pthread_t flush_thread;
    pthread_create(&flush_thread, NULL, flush_thread_func, NULL);
#endif

    uint64_t thread_ids[WRITE_THREAD_NUM];
#if OS(WIN)
    HANDLE write_thread[WRITE_THREAD_NUM];
#else
    pthread_t write_thread[WRITE_THREAD_NUM];
#endif
    for (uint32_t i = 0; i < WRITE_THREAD_NUM; i++) {
        thread_ids[i] = i;
#if OS(WIN)
        write_thread[i] = CreateThread(NULL, 0, write_thread_func, &thread_ids[i], 0, NULL);
#else
        pthread_create(&write_thread[i], NULL, write_thread_func, &thread_ids[i]);
#endif
    }

#if OS(WIN)
    WaitForMultipleObjects(WRITE_THREAD_NUM, write_thread, true, INFINITE);
    for (uint32_t i = 0; i < WRITE_THREAD_NUM; i++)
        CloseHandle(write_thread[i]);
#else
    for (uint32_t i = 0; i < WRITE_THREAD_NUM; i++)
        pthread_join(write_thread[i], NULL);
#endif

    g_stop_flush = true;
#if OS(WIN)
    WaitForSingleObject(flush_thread, INFINITE);
    CloseHandle(flush_thread);
#else
    pthread_join(flush_thread, NULL);
#endif

    log_deinit(&g_log);

    printf("\nWait-Free Chunk MPSC Log Test Complete!\n");
    return 0;
}
