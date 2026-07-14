#include <stdio.h>
#include <string.h>

#include "module.h"

#define RECVER_IP    "127.0.0.1"
#define RECVER_PORT  (2334)
#define SENDER_IP    "127.0.0.1"
#define SENDER_PORT  (2333)

#define MEMPOOL_SIZE (SIZE_1MB)

NO_ASAN
ALIGN(4096)
u8        g_mempool_buf[MEMPOOL_SIZE];
mempool_t g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

net_t g_net;

int
main(void)
{
        mempool_init(&g_mempool);

        const net_cfg_t net_cfg = {
            .e_type   = NET_TYPE_UDP,
            .mempool  = &g_mempool,
            .f_get_ts = get_real_ts_us,
        };
        int ret = net_init(&g_net, net_cfg);
        if (ret < 0) {
                print_error(FALSE, "net_init failed: %d", ret);
                return -1;
        }

        net_ch_t ch = net_cfg_ch(IP_STR_TO_U32(SENDER_IP), SENDER_PORT, NET_MODE_SYNC_YIELD);
        ch.src_ip   = IP_STR_TO_U32(RECVER_IP);
        ch.src_port = RECVER_PORT;

        ret = net_add_ch(&g_net, &ch);
        if (ret < 0) {
                print_error(FALSE, "net_add_ch failed: %d", ret);
                return -1;
        }

        print_info(FALSE, "recver bound to %s:%d, echoing to %s:%d", RECVER_IP, RECVER_PORT, SENDER_IP, SENDER_PORT);

        u8 buf[256];
        for (;;) {
                const isize n = net_recv(&g_net, &ch, buf, sizeof(buf), MS2US(1000));
                if (n <= 0)
                        continue;

                print_success(TRUE, "[RECV] %zd bytes: %.*s", (isize)n, (int)n, (char *)buf);
                net_send(&g_net, &ch, buf, (usize)n);
        }

        return 0;
}
