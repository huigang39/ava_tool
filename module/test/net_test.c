#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "module.h"

#define MP_SIZE (1 * 1024)

net_t     net;
mp_t      mp;
static u8 mp_buf[MP_SIZE];

#define WRITE_THREAD_NUM 255

u8       g_log_flush_buf[128];
u8       g_log_buf[1024 * 1024];
mpsc_p_t g_producers[WRITE_THREAD_NUM];

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

        printf("[SEND][%llu] %d bytes: %.*s\n", get_mono_ts_ms(), ret, ret < 0 ? 0 : ret, (char *)buf);

        mp_free(&mp, buf);
}

void
on_recv_done(net_ch_t *ch, void *buf, int ret)
{
        ARG_UNUSED(ch);

        if (ret == -METIMEOUT)
                print_error(0, "[RECV][%llu] timeout", get_mono_ts_ms());
        else
                print_success(0, "[RECV][%llu] %d bytes: %.*s", get_mono_ts_ms(), ret, ret < 0 ? 0 : ret, (char *)buf);

        mp_free(&mp, buf);
}

void *
send_recv_thread(void *arg)
{
        net_ch_t *ch  = (net_ch_t *)arg;
        u64       cnt = 0;
        for (;;) {
                char *tx_buf = mp_calloc(&mp, 64);
                if (!tx_buf) {
                        printf("mempool full!\n");
                        continue;
                }
                sprintf(tx_buf, "CNT_%llu", cnt++);

                char *rx_buf = mp_calloc(&mp, 64);
                if (!rx_buf) {
                        printf("mempool full!\n");
                        continue;
                }

                const u64 start_us = get_mono_ts_us();
                net_send_recv(&net, ch, tx_buf, strlen(tx_buf), rx_buf, 1024, MS2US(2));
                const u64 end_us = get_mono_ts_us();
                println("cnt: %llu, elapsed: %llu us", cnt, end_us - start_us);
                delay_ms(20, SPIN);
        }
}

int
init(void)
{
        mp.buf = mp_buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        FILE *fp = fopen("net_log.bin", "w");

        const net_cfg_t net_cfg = {
            .e_type   = NET_TYPE_UDP,
            .mp       = &mp,
            .ring_len = 256,
            .log_cfg =
                {
                    .e_mode     = LOG_MODE_SYNC,
                    .e_level    = LOG_LEVEL_DEBUG,
                    .fp         = fp,
                    .buf        = (void *)g_log_buf,
                    .cap        = sizeof(g_log_buf),
                    .flush_buf  = g_log_flush_buf,
                    .flush_cap  = sizeof(g_log_flush_buf),
                    .producers  = (mpsc_p_t *)&g_producers,
                    .nproducers = ARRAY_LEN(g_producers),
                    .f_flush    = log_stdout,
                    .f_get_ts   = get_real_ts_ms,
                },
            .f_get_ts = get_real_ts_ms,
        };
        int ret = net_init(&net, net_cfg);

        net_resp_t  resps[255];
        const char *tx_buf = "{\"method\":\"GET\",\"reqTarget\":\"/custom\",\"cnt\":\"    "
                             "0\",\"type\":true,\"mcu_fw_version\":true,\"mac_address\":true,"
                             "\"static_IP\":true}";
        ret                = net_broadcast(IP_STR_TO_U32("192.168.137.255"), 2334, tx_buf, strlen(tx_buf), resps, 10000);

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

        int ret = net_add_ch(&net, &ch);
        if (ret < 0)
                return -1;

        pthread_t tid;
        pthread_create(&tid, NULL, send_recv_thread, &ch);

        // pthread_create(&tid, NULL, send_recv_thread, &ch);

        for (;;) {
                // log_flush_bin(&net.lo.log);
                net_poll(&net);
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
