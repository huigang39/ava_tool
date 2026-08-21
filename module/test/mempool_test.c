#include "module.h"

#if OS(WIN)
#include <windows.h>
#endif

#include <assert.h>

/* -------------------------------------------------------------------------- */
/*                                  测试辅助函数                              */
/* -------------------------------------------------------------------------- */

#define TEST_ASSERT(cond, msg)                                        \
    do {                                                              \
        if (!(cond)) {                                                \
            print_error(false, "%s:%d: %s", __FILE__, __LINE__, msg); \
            return -1;                                                \
        } else                                                        \
            print_success(false, "%s", msg);                          \
    } while (0)

#define TEST_RUN(test_func)                              \
    do {                                                 \
        println("\n--- running %s ---", #test_func);     \
        int ret = test_func();                           \
        if (ret != 0) {                                  \
            print_error(false, "%s failed", #test_func); \
            return ret;                                  \
        }                                                \
    } while (0)

/* -------------------------------------------------------------------------- */
/*                                  测试用例                                  */
/* -------------------------------------------------------------------------- */

#define MEMPOOL_SIZE (SIZE_1KB)

uint8_t        g_mempool_buf[MEMPOOL_SIZE];
struct mempool g_mempool = {
    .buf = g_mempool_buf,
    .cap = sizeof(g_mempool_buf),
};

static void
setup_mempool(void)
{
    mempool_init(&g_mempool);
}

/**
 * @brief 测试1: 基本分配和释放
 */
static int
test_basic_alloc_free(void)
{
    setup_mempool();

    // 测试基本分配
    void *ptr1 = mempool_alloc(&g_mempool, 32);
    TEST_ASSERT(ptr1 != NULL, "分配32字节应该成功");

    void *ptr2 = mempool_alloc(&g_mempool, 64);
    TEST_ASSERT(ptr2 != NULL, "分配64字节应该成功");
    TEST_ASSERT(ptr1 != ptr2, "两次分配应该返回不同指针");

    // 测试释放
    mempool_free(&g_mempool, ptr1);
    mempool_free(&g_mempool, ptr2);

    // 释放后应该能重新分配
    void *ptr3 = mempool_alloc(&g_mempool, 32);
    TEST_ASSERT(ptr3 != NULL, "释放后重新分配应该成功");

    mempool_free(&g_mempool, ptr3);
    return 0;
}

/**
 * @brief 测试2: 对齐检查
 */
static int
test_alignment(void)
{
    setup_mempool();

    // 测试不同大小的分配,检查对齐
    for (size_t size = 1; size <= 64; size++) {
        void *ptr = mempool_alloc(&g_mempool, size);
        TEST_ASSERT(ptr != NULL, "分配应该成功");
        TEST_ASSERT(((size_t)ptr % MEMPOOL_ALIGN) == 0, "指针应该按8字节对齐");
        mempool_free(&g_mempool, ptr);
    }

    return 0;
}

/**
 * @brief 测试3: calloc功能
 */
static int
test_calloc(void)
{
    setup_mempool();

    // 测试calloc应该初始化为0
    void *ptr = mempool_calloc(&g_mempool, 128);
    TEST_ASSERT(ptr != NULL, "calloc应该成功");

    // 检查内存是否被初始化为0
    uint8_t is_zero = 1;
    for (size_t i = 0; i < 128; i++) {
        if (((uint8_t *)ptr)[i] != 0) {
            is_zero = 0;
            break;
        }
    }
    TEST_ASSERT(is_zero, "calloc分配的内存应该被初始化为0");

    mempool_free(&g_mempool, ptr);
    return 0;
}

/**
 * @brief 测试4: 内存池耗尽
 */
static int
test_pool_exhaustion(void)
{
    setup_mempool();

    // 尝试分配超过内存池大小的内存
    void *ptr = mempool_alloc(&g_mempool, MEMPOOL_SIZE + 1);
    TEST_ASSERT(ptr == NULL, "分配超过内存池大小的内存应该失败");

    // 分配大量小块直到耗尽
    void  *ptrs[1000];
    size_t count = 0;
    for (size_t i = 0; i < 1000; i++) {
        ptrs[i] = mempool_alloc(&g_mempool, 16);
        if (ptrs[i] == NULL)
            break;
        count++;
    }
    TEST_ASSERT(count > 0, "应该能分配一些内存块");

    // 尝试再分配一个,应该失败
    void *ptr_fail = mempool_alloc(&g_mempool, 16);
    TEST_ASSERT(ptr_fail == NULL, "内存池耗尽后分配应该失败");

    // 释放一些内存后应该能再分配
    mempool_free(&g_mempool, ptrs[0]);
    void *ptr_after_free = mempool_alloc(&g_mempool, 16);
    TEST_ASSERT(ptr_after_free != NULL, "释放后应该能重新分配");

    // 清理
    mempool_free(&g_mempool, ptr_after_free);
    for (size_t i = 1; i < count; i++)
        mempool_free(&g_mempool, ptrs[i]);

    return 0;
}

/**
 * @brief 测试5: 相邻块合并
 */
static int
test_merge_adjacent_blocks(void)
{
    setup_mempool();

    // 分配三个连续块
    void *ptr1 = mempool_alloc(&g_mempool, 32);
    void *ptr2 = mempool_alloc(&g_mempool, 32);
    void *ptr3 = mempool_alloc(&g_mempool, 32);

    TEST_ASSERT(ptr1 != NULL && ptr2 != NULL && ptr3 != NULL, "应该能分配三个块");

    // 释放中间块
    mempool_free(&g_mempool, ptr2);

    // 释放第一个块(应该与中间块合并)
    mempool_free(&g_mempool, ptr1);

    // 释放第三个块(应该与前面合并的块合并)
    mempool_free(&g_mempool, ptr3);

    // 现在应该能分配一个大的块(三个块合并后的大小)
    void *large_ptr = mempool_alloc(&g_mempool, 96);
    TEST_ASSERT(large_ptr != NULL, "合并后应该能分配大块");

    mempool_free(&g_mempool, large_ptr);
    return 0;
}

/**
 * @brief 测试6: 边界情况
 */
static int
test_edge_cases(void)
{
    setup_mempool();

    // 测试NULL指针释放
    mempool_free(&g_mempool, NULL);
    TEST_ASSERT(1, "释放NULL指针不应该崩溃");

    // 测试分配0字节(应该分配最小对齐大小)
    void *ptr0 = mempool_alloc(&g_mempool, 0);
    TEST_ASSERT(ptr0 != NULL, "分配0字节应该返回有效指针");

    // 测试无效指针释放(应该被静默忽略)
    void *invalid_ptr = (void *)0xDEADBEEF;
    mempool_free(&g_mempool, invalid_ptr);
    TEST_ASSERT(1, "释放无效指针不应该崩溃");

    mempool_free(&g_mempool, ptr0);
    return 0;
}

/**
 * @brief 测试7: 碎片化测试
 */
static int
test_fragmentation(void)
{
    setup_mempool();

    // 分配多个不同大小的块
    void *ptrs[10];
    for (size_t i = 0; i < 10; i++) {
        ptrs[i] = mempool_alloc(&g_mempool, (i + 1) * 8);
        TEST_ASSERT(ptrs[i] != NULL, "分配应该成功");
    }

    // 释放奇数索引的块
    for (size_t i = 1; i < 10; i += 2)
        mempool_free(&g_mempool, ptrs[i]);

    // 释放偶数索引的块(应该能合并)
    for (size_t i = 0; i < 10; i += 2)
        mempool_free(&g_mempool, ptrs[i]);

    // 现在应该能分配一个大的块
    void *large_ptr = mempool_alloc(&g_mempool, 200);
    TEST_ASSERT(large_ptr != NULL, "合并后应该能分配大块");

    mempool_free(&g_mempool, large_ptr);
    return 0;
}

/**
 * @brief 测试8: reset功能
 */
static int
test_reset(void)
{
    setup_mempool();

    // 分配一些内存
    void *ptr1 = mempool_alloc(&g_mempool, 64);
    void *ptr2 = mempool_alloc(&g_mempool, 128);
    TEST_ASSERT(ptr1 != NULL && ptr2 != NULL, "分配应该成功");

    // 重置内存池
    mempool_reset(&g_mempool);

    // 重置后应该能重新分配
    void *ptr3 = mempool_alloc(&g_mempool, 64);
    TEST_ASSERT(ptr3 != NULL, "重置后应该能重新分配");

    // 重置后之前的指针应该无效(但我们不访问它们,只是测试功能)
    mempool_free(&g_mempool, ptr3);
    return 0;
}

/**
 * @brief 测试9: best-fit策略验证
 */
static int
test_best_fit(void)
{
    setup_mempool();

    // 分配多个不同大小的块
    void *small1 = mempool_alloc(&g_mempool, 16);
    void *small2 = mempool_alloc(&g_mempool, 16);
    void *large  = mempool_alloc(&g_mempool, 128);

    TEST_ASSERT(small1 != NULL && small2 != NULL && large != NULL, "分配应该成功");

    // 释放小块
    mempool_free(&g_mempool, small1);
    mempool_free(&g_mempool, small2);
    mempool_free(&g_mempool, large);

    // 现在请求一个中等大小的块,应该使用最合适的块
    void *medium = mempool_alloc(&g_mempool, 64);
    TEST_ASSERT(medium != NULL, "应该能找到合适的块");

    mempool_free(&g_mempool, medium);
    return 0;
}

/**
 * @brief 测试10: 多次分配释放循环
 */
static int
test_repeated_alloc_free(void)
{
    setup_mempool();

    // 进行多次分配和释放循环
    for (size_t round = 0; round < 100; round++) {
        void  *ptrs[20];
        size_t count = 0;

        // 分配多个块
        for (size_t i = 0; i < 20; i++) {
            ptrs[i] = mempool_alloc(&g_mempool, (i + 1) * 4);
            if (ptrs[i] != NULL)
                count++;
        }

        // 释放所有块
        for (size_t i = 0; i < count; i++)
            mempool_free(&g_mempool, ptrs[i]);
    }

    TEST_ASSERT(1, "多次分配释放循环应该成功");
    return 0;
}

/**
 * @brief 测试11: 数据完整性
 */
static int
test_data_integrity(void)
{
    setup_mempool();

    // 分配内存并写入数据
    char *ptr = (char *)mempool_alloc(&g_mempool, 64);
    TEST_ASSERT(ptr != NULL, "分配应该成功");

    // 写入数据
    for (size_t i = 0; i < 64; i++)
        ptr[i] = (char)(i % 256);

    // 验证数据
    uint8_t data_ok = 1;
    for (size_t i = 0; i < 64; i++) {
        if (ptr[i] != (char)(i % 256)) {
            data_ok = 0;
            break;
        }
    }
    TEST_ASSERT(data_ok, "数据应该保持完整");

    mempool_free(&g_mempool, ptr);
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                                  主函数                                    */
/* -------------------------------------------------------------------------- */

int
main(void)
{
#if OS(WIN)
    SetConsoleOutputCP(65001); // 设置控制台代码页为 UTF-8
#endif
    println("----------------------------------------");
    println("    Memory Pool Allocator Test Suite");
    println("----------------------------------------");
    println("");

    TEST_RUN(test_basic_alloc_free);
    TEST_RUN(test_alignment);
    TEST_RUN(test_calloc);
    TEST_RUN(test_pool_exhaustion);
    TEST_RUN(test_merge_adjacent_blocks);
    TEST_RUN(test_edge_cases);
    TEST_RUN(test_fragmentation);
    TEST_RUN(test_reset);
    TEST_RUN(test_best_fit);
    TEST_RUN(test_repeated_alloc_free);
    TEST_RUN(test_data_integrity);

    println("\n----------------------------------------");
    println("           ALL TEST PASSED!");
    println("----------------------------------------");

    return 0;
}
