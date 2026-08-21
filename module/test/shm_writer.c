#include <stdlib.h>

#include "module.h"

int
main()
{
    struct shm           shm     = {0};
    const struct shm_cfg shm_cfg = {
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

    uint64_t cnt = 0;
    for (;;) {
        cnt++;
        shm_write(&shm, &cnt, sizeof(cnt));
        printf("write cnt: %llu, spsc wp: %llu, spsc rp: %llu, spsc "
               "free: %llu\n",
               cnt,
               ATOMIC_LOAD(&shm.lo.spsc->wp),
               ATOMIC_LOAD(&shm.lo.spsc->rp),
               spsc_free(shm.lo.spsc));
        delay_ms(1, DELAY_SPIN);
    }
}
