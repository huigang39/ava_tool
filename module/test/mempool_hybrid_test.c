#include <stdio.h>
#include <string.h>

#include "module.h"

#define CHECK(expr)                                                \
    do {                                                           \
        if (!(expr)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                              \
        }                                                          \
    } while (0)

int
main(void)
{
    ALIGN(8) uint8_t variable_buf[256];
    ALIGN(8) uint8_t small_buf[MEMPOOL_FIXED_POOL_SIZE(32, 2)];
    ALIGN(8) uint8_t medium_buf[MEMPOOL_FIXED_POOL_SIZE(64, 1)];
    struct mempool   mempool = {.buf = variable_buf, .cap = sizeof(variable_buf)};

    mempool_init(&mempool);
    CHECK(mempool_add_fixed_pool(&mempool, medium_buf, 64, 1) == 0);
    CHECK(mempool_add_fixed_pool(&mempool, small_buf, 32, 2) == 0);
    CHECK(mempool_add_fixed_pool(&mempool, small_buf, 32, 2) == -MEXIST);

    void *a = mempool_alloc_fast(&mempool, 24);
    void *b = mempool_alloc_fast(&mempool, 24);
    void *c = mempool_alloc_fast(&mempool, 24);
    CHECK((uint8_t *)a >= small_buf && (uint8_t *)a < small_buf + sizeof(small_buf));
    CHECK((uint8_t *)b >= small_buf && (uint8_t *)b < small_buf + sizeof(small_buf));
    CHECK((uint8_t *)c >= medium_buf && (uint8_t *)c < medium_buf + sizeof(medium_buf));
    CHECK(mempool_alloc_fast(&mempool, 24) == NULL);

    void *fallback = mempool_alloc(&mempool, 24);
    CHECK(fallback != NULL && (uint8_t *)fallback >= variable_buf &&
          (uint8_t *)fallback < variable_buf + sizeof(variable_buf));

    mempool_free(&mempool, b);
    CHECK(mempool_alloc_fast(&mempool, 24) == b);
    mempool_free(&mempool, a);
    mempool_free(&mempool, c);
    mempool_free(&mempool, fallback);

    void *zeroed = mempool_calloc_fast(&mempool, 20);
    CHECK(zeroed != NULL);
    for (size_t i = 0; i < 20; ++i)
        CHECK(((uint8_t *)zeroed)[i] == 0);

    mempool_reset(&mempool);
    CHECK(mempool_fixed_available(&mempool, 32) == 2);
    CHECK(mempool_fixed_available(&mempool, 64) == 1);
    CHECK(mempool.offset == 0);

    ALIGN(8) uint8_t overlapping_buf[128];
    struct mempool   overlapping = {.buf = overlapping_buf, .cap = sizeof(overlapping_buf)};
    mempool_init(&overlapping);
    CHECK(mempool_add_fixed_pool(&overlapping, overlapping_buf, 32, 2) == -MEINVAL);

    printf("hybrid memory pool tests passed\n");
    return 0;
}
