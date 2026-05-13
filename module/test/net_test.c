#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "module.h"

#define DST_IP             "192.168.137.255"
#define DST_PORT           (2334)

#define MEMPOOL_SIZE       (SIZE_4MB)
#define WRITE_THREAD_NUM   (255)

#define LOG_BUF_SIZE       (SIZE_1MB)
#define LOG_FLUSH_BUF_SIZE (SIZE_128B)

net_t g_net;

NO_ASAN ALIGN(SIZE_16KB) u8 g_mempool_buf[MEMPOOL_SIZE];
mempool_t g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

void
log_stdout(void *fp, const void *src, const usize size)
{
        fwrite(src, size, 1, fp);
        fflush(fp);
}

void
on_send_done(net_ch_t *ch, void *buf, int ret)
{
        ARG_UNUSED(ch);

        print_success(TRUE, "[SEND][%llu] %d bytes: %.*s\n", get_mono_ts_ms(), ret, ret < 0 ? 0 : ret, (char *)buf);

        mempool_free(&g_mempool, buf);
}

void
on_recv_done(net_ch_t *ch, void *buf, int ret)
{
        ARG_UNUSED(ch);

        if (ret == -METIMEOUT)
                print_error(TRUE, "[RECV][%llu] timeout", get_mono_ts_ms());
        else
                print_success(TRUE, "[RECV][%llu] %d bytes: %.*s", get_mono_ts_ms(), ret, ret < 0 ? 0 : ret, (char *)buf);

        mempool_free(&g_mempool, buf);
}

void *
send_recv_thread(void *arg)
{
        net_ch_t *ch  = arg;
        u64       cnt = 0;
        for (;;) {
                char *tx_buf = mempool_calloc(&g_mempool, 64);
                if (!tx_buf) {
                        print_error(TRUE, "mempool full!\n");
                        continue;
                }
                snprintf(tx_buf, 64, "CNT_%llu", cnt++);

                char *rx_buf = mempool_calloc(&g_mempool, 64);
                if (!rx_buf) {
                        print_error(TRUE, "mempool full!\n");
                        continue;
                }

                const u64 start_us = get_mono_ts_us();
                net_send_recv(&g_net, ch, tx_buf, strlen(tx_buf), rx_buf, 1024, MS2US(2));
                const u64 end_us = get_mono_ts_us();
                print_success(TRUE, "cnt: %llu, elapsed: %llu us", cnt, end_us - start_us);
                delay_ms(20, DELAY_SPIN);
        }
}

int
init(void)
{
        mempool_init(&g_mempool);

        FILE *file = fopen("net_log.bin", "w");

        const net_cfg_t net_cfg = {
            .e_type   = NET_TYPE_UDP,
            .mempool  = &g_mempool,
            .ring_len = SIZE_256B,
            .log_cfg =
                {
                    .e_mode     = LOG_MODE_SYNC,
                    .e_level    = LOG_LEVEL_DEBUG,
                    .mempool    = &g_mempool,
                    .flush_cap  = LOG_FLUSH_BUF_SIZE,
                    .nproducers = WRITE_THREAD_NUM,
                    .fd         = file,
                    .f_flush    = log_stdout,
                    .f_get_ts   = get_real_ts_ms,
                },
            .f_get_ts = get_real_ts_ms,
        };
        int ret = net_init(&g_net, net_cfg);
        if (ret < 0)
                return ret;

        net_resp_t  resps[255];
        const char *tx_buf = "{\"method\":\"GET\",\"reqTarget\":\"/custom\",\"cnt\":\"    "
                             "0\",\"type\":true,\"mcu_fw_version\":true,\"mac_address\":true,"
                             "\"static_IP\":true}";
        ret                = net_broadcast(IP_STR_TO_U32(DST_IP), DST_PORT, tx_buf, strlen(tx_buf), resps, ARRAY_LEN(resps), MS2US(1));

        for (int i = 0; i < ret; i++)
                printf("%s\n", resps[i].buf);

        return ret;
}

int
exec(void)
{
        net_ch_t ch = {
            .dst_ip   = IP_STR_TO_U32("127.0.0.1"),
            .dst_port = 2333,
            // .dst_ip   = "192.168.137.101",
            // .dst_port = 2340,
            // .src_ip    = "127.0.0.1",
            // .src_port  = 2334,
            .e_mode    = NET_MODE_ASYNC,
            .f_send_cb = on_send_done,
            .f_recv_cb = on_recv_done,
        };

        const int ret = net_add_ch(&g_net, &ch);
        if (ret < 0)
                return -1;

        pthread_t tid;
        pthread_create(&tid, NULL, send_recv_thread, &ch);

        // pthread_create(&tid, NULL, send_recv_thread, &ch);

        for (;;) {
                // log_flush_bin(&g_net.lo.log);
                net_poll(&g_net);
                // delay_ms(10, YIELD);
        }
}

int
main()
{
        init();
        exec();
        return 0;
}
