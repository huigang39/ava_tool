#include "module.h"

#if !OS(WIN)
#include <pthread.h>
#endif
#include <stdio.h>
#include <string.h>

#define SENDER_IP      "127.0.0.1"
#define SENDER_PORT    (2333)
#define RECVER_IP      "127.0.0.1"
#define RECVER_PORT    (2334)

#define MEMPOOL_SIZE   (SIZE_2MB)
#define LOG_CHUNK_SIZE (SIZE_4KB)
#define LOG_FLUSH_SIZE (SIZE_1KB)

NO_ASAN
ALIGN(4096)
u8        g_mempool_buf[MEMPOOL_SIZE];
mempool_t g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

net_t g_net;

static void
log_stdout(void *fp, const void *src, const usize size)
{
        if (fp)
                fwrite(src, size, 1, fp);
}

static void
on_recv(net_ch_t *ch, void *buf, int ret)
{
        ARG_UNUSED(ch);
        if (ret == -METIMEOUT)
                print_warn(TRUE, "[RECV] timeout");
        else if (ret < 0)
                print_error(TRUE, "[RECV] error: %d", ret);
        else
                print_success(TRUE, "[RECV] %d bytes: %.*s", ret, ret, (char *)buf);
        mempool_free(&g_mempool, buf);
}

#if OS(WIN)
static DWORD WINAPI
send_thread(LPVOID arg)
#else
static void *
send_thread(void *arg)
#endif
{
        net_ch_t *ch  = arg;
        u64       cnt = 0;

        for (;;) {
                char      tx_buf[64];
                const int len = snprintf(tx_buf, sizeof(tx_buf), "CNT_%llu", cnt++);

                char *rx_buf = mempool_alloc(&g_mempool, 256);
                if (!rx_buf) {
                        delay_ms(10, DELAY_SPIN);
                        continue;
                }

                const isize tx_size = net_send(&g_net, ch, tx_buf, (usize)len);
                if (tx_size < 0)
                        print_error(TRUE, "[SEND] error: %d", (int)tx_size);
                else
                        print_success(TRUE, "[SEND] %d bytes: %.*s", (int)tx_size, (int)tx_size, tx_buf);

                net_async_recv(&g_net, ch, rx_buf, 256, MS2US(200));

                delay_ms(100, DELAY_SPIN);
        }

#if OS(WIN)
        return 0;
#else
        return NULL;
#endif
}

int
main(void)
{
        mempool_init(&g_mempool);

        const log_cfg_t log_cfg = {
            .e_mode   = LOG_MODE_ASYNC,
            .e_level  = LOG_LEVEL_DATA,
            .e_format = LOG_FORMAT_TEXT,
            .mempool  = &g_mempool,

            .file_path = "net_sender",
            .file_size = SIZE_1MB,
            .max_files = 3,
            .e_ring    = LOG_RING_ROTATE,

            .fd         = stdout,
            .f_flush    = log_stdout,
            .chunk_size = LOG_CHUNK_SIZE,
            .flush_cap  = LOG_FLUSH_SIZE,
            .nproducers = 1,
            .f_get_ts   = get_real_ts_us,
        };
        const net_cfg_t net_cfg = {
            .e_type   = NET_TYPE_UDP,
            .mempool  = &g_mempool,
            .f_get_ts = get_real_ts_us,
            .log_cfg  = log_cfg,
        };
        int ret = net_init(&g_net, net_cfg);
        if (ret < 0) {
                print_error(FALSE, "net_init failed: %d", ret);
                return -1;
        }

        net_ch_t ch  = net_cfg_ch(IP_STR_TO_U32(RECVER_IP), RECVER_PORT, NET_MODE_ASYNC);
        ch.src_ip    = IP_STR_TO_U32(SENDER_IP);
        ch.src_port  = SENDER_PORT;
        ch.f_recv_cb = on_recv;

        ret = net_add_ch(&g_net, &ch);
        if (ret < 0) {
                print_error(FALSE, "net_add_ch failed: %d", ret);
                return -1;
        }

        print_info(FALSE, "sender %s:%d -> recver %s:%d", SENDER_IP, SENDER_PORT, RECVER_IP, RECVER_PORT);

#if OS(WIN)
        CreateThread(NULL, 0, send_thread, &ch, 0, NULL);
#else
        pthread_t tid;
        pthread_create(&tid, NULL, send_thread, &ch);
#endif

        for (;;) {
                net_poll(&g_net);
                log_flush(&g_net.lo.log);
        }

        return 0;
}