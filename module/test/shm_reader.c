#include <stdlib.h>

#include "module.h"

int
main()
{
        shm_t           shm     = {0};
        const shm_cfg_t shm_cfg = {
            .name   = "ch1",
            .access = SHM_READWRITE,
            .cap    = SIZE_1KB,
        };

        const int ret = shm_init(&shm, shm_cfg);
        if (ret < 0) {
                printf("writer: shm init failed, errcode: %d\n", ret);
                exit(-1);
        }

        printf("shm_addr: 0x%p, buf_addr: 0x%p\n", shm.lo.addr, shm.lo.spsc->buf);

        u64 cnt = 0;
        for (;;) {
                shm_read(&shm, &cnt, sizeof(cnt));
                printf("read cnt: %llu, spsc wp: %llu, spsc rp: %llu, spsc "
                       "free: %llu\n",
                       cnt,
                       ATOMIC_LOAD(&shm.lo.spsc->wp),
                       ATOMIC_LOAD(&shm.lo.spsc->rp),
                       spsc_free(shm.lo.spsc));
        }
}
