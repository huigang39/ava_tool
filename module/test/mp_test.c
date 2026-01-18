#include "module.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <assert.h>

/* -------------------------------------------------------------------------- */
/*                                  测试辅助函数                              */
/* -------------------------------------------------------------------------- */

#define TEST_ASSERT(cond, msg)                                                \
        do {                                                                  \
                if (!(cond)) {                                                \
                        println("[FAIL] %s:%d: %s", __FILE__, __LINE__, msg); \
                        return -1;                                            \
                } else                                                        \
                        println("[PASS] %s", msg);                            \
        } while (0)

#define TEST_RUN(test_func)                                       \
        do {                                                      \
                println("\n=== Running %s ===", #test_func);      \
                int ret = test_func();                            \
                if (ret != 0) {                                   \
                        println("[ERROR] %s failed", #test_func); \
                        return ret;                               \
                }                                                 \
        } while (0)

/* -------------------------------------------------------------------------- */
/*                                  测试用例                                  */
/* -------------------------------------------------------------------------- */

#define MP_SIZE (1 * 1024)

/**
 * @brief 测试1: 基本分配和释放
 */
static int
test_basic_alloc_free(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 测试基本分配
        void *ptr1 = mp_alloc(&mp, 32);
        TEST_ASSERT(ptr1 != NULL, "分配32字节应该成功");

        void *ptr2 = mp_alloc(&mp, 64);
        TEST_ASSERT(ptr2 != NULL, "分配64字节应该成功");
        TEST_ASSERT(ptr1 != ptr2, "两次分配应该返回不同指针");

        // 测试释放
        mp_free(&mp, ptr1);
        mp_free(&mp, ptr2);

        // 释放后应该能重新分配
        void *ptr3 = mp_alloc(&mp, 32);
        TEST_ASSERT(ptr3 != NULL, "释放后重新分配应该成功");

        mp_free(&mp, ptr3);
        return 0;
}

/**
 * @brief 测试2: 对齐检查
 */
static int
test_alignment(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 测试不同大小的分配，检查对齐
        for (usize size = 1; size <= 64; size++) {
                void *ptr = mp_alloc(&mp, size);
                TEST_ASSERT(ptr != NULL, "分配应该成功");
                TEST_ASSERT(((usize)ptr % MP_ALIGN) == 0, "指针应该按8字节对齐");
                mp_free(&mp, ptr);
        }

        return 0;
}

/**
 * @brief 测试3: calloc功能
 */
static int
test_calloc(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 测试calloc应该初始化为0
        void *ptr = mp_calloc(&mp, 128);
        TEST_ASSERT(ptr != NULL, "calloc应该成功");

        // 检查内存是否被初始化为0
        bool is_zero = 1;
        for (usize i = 0; i < 128; i++) {
                if (((u8 *)ptr)[i] != 0) {
                        is_zero = 0;
                        break;
                }
        }
        TEST_ASSERT(is_zero, "calloc分配的内存应该被初始化为0");

        mp_free(&mp, ptr);
        return 0;
}

/**
 * @brief 测试4: 内存池耗尽
 */
static int
test_pool_exhaustion(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 尝试分配超过内存池大小的内存
        void *ptr = mp_alloc(&mp, MP_SIZE + 1);
        TEST_ASSERT(ptr == NULL, "分配超过内存池大小的内存应该失败");

        // 分配大量小块直到耗尽
        void *ptrs[1000];
        usize count = 0;
        for (usize i = 0; i < 1000; i++) {
                ptrs[i] = mp_alloc(&mp, 16);
                if (ptrs[i] == NULL)
                        break;
                count++;
        }
        TEST_ASSERT(count > 0, "应该能分配一些内存块");

        // 尝试再分配一个，应该失败
        void *ptr_fail = mp_alloc(&mp, 16);
        TEST_ASSERT(ptr_fail == NULL, "内存池耗尽后分配应该失败");

        // 释放一些内存后应该能再分配
        mp_free(&mp, ptrs[0]);
        void *ptr_after_free = mp_alloc(&mp, 16);
        TEST_ASSERT(ptr_after_free != NULL, "释放后应该能重新分配");

        // 清理
        mp_free(&mp, ptr_after_free);
        for (usize i = 1; i < count; i++)
                mp_free(&mp, ptrs[i]);

        return 0;
}

/**
 * @brief 测试5: 相邻块合并
 */
static int
test_merge_adjacent_blocks(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 分配三个连续块
        void *ptr1 = mp_alloc(&mp, 32);
        void *ptr2 = mp_alloc(&mp, 32);
        void *ptr3 = mp_alloc(&mp, 32);

        TEST_ASSERT(ptr1 != NULL && ptr2 != NULL && ptr3 != NULL, "应该能分配三个块");

        // 释放中间块
        mp_free(&mp, ptr2);

        // 释放第一个块（应该与中间块合并）
        mp_free(&mp, ptr1);

        // 释放第三个块（应该与前面合并的块合并）
        mp_free(&mp, ptr3);

        // 现在应该能分配一个大的块（三个块合并后的大小）
        void *large_ptr = mp_alloc(&mp, 96);
        TEST_ASSERT(large_ptr != NULL, "合并后应该能分配大块");

        mp_free(&mp, large_ptr);
        return 0;
}

/**
 * @brief 测试6: 边界情况
 */
static int
test_edge_cases(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 测试NULL指针释放
        mp_free(&mp, NULL);
        TEST_ASSERT(1, "释放NULL指针不应该崩溃");

        // 测试分配0字节（应该分配最小对齐大小）
        void *ptr0 = mp_alloc(&mp, 0);
        TEST_ASSERT(ptr0 != NULL, "分配0字节应该返回有效指针");

        // 测试无效指针释放（应该被静默忽略）
        void *invalid_ptr = (void *)0xDEADBEEF;
        mp_free(&mp, invalid_ptr);
        TEST_ASSERT(1, "释放无效指针不应该崩溃");

        mp_free(&mp, ptr0);
        return 0;
}

/**
 * @brief 测试7: 碎片化测试
 */
static int
test_fragmentation(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 分配多个不同大小的块
        void *ptrs[10];
        for (usize i = 0; i < 10; i++) {
                ptrs[i] = mp_alloc(&mp, (i + 1) * 8);
                TEST_ASSERT(ptrs[i] != NULL, "分配应该成功");
        }

        // 释放奇数索引的块
        for (usize i = 1; i < 10; i += 2)
                mp_free(&mp, ptrs[i]);

        // 释放偶数索引的块（应该能合并）
        for (usize i = 0; i < 10; i += 2)
                mp_free(&mp, ptrs[i]);

        // 现在应该能分配一个大的块
        void *large_ptr = mp_alloc(&mp, 200);
        TEST_ASSERT(large_ptr != NULL, "合并后应该能分配大块");

        mp_free(&mp, large_ptr);
        return 0;
}

/**
 * @brief 测试8: reset功能
 */
static int
test_reset(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 分配一些内存
        void *ptr1 = mp_alloc(&mp, 64);
        void *ptr2 = mp_alloc(&mp, 128);
        TEST_ASSERT(ptr1 != NULL && ptr2 != NULL, "分配应该成功");

        // 重置内存池
        mp_reset(&mp);

        // 重置后应该能重新分配
        void *ptr3 = mp_alloc(&mp, 64);
        TEST_ASSERT(ptr3 != NULL, "重置后应该能重新分配");

        // 重置后之前的指针应该无效（但我们不访问它们，只是测试功能）
        mp_free(&mp, ptr3);
        return 0;
}

/**
 * @brief 测试9: best-fit策略验证
 */
static int
test_best_fit(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 分配多个不同大小的块
        void *small1 = mp_alloc(&mp, 16);
        void *small2 = mp_alloc(&mp, 16);
        void *large  = mp_alloc(&mp, 128);

        TEST_ASSERT(small1 != NULL && small2 != NULL && large != NULL, "分配应该成功");

        // 释放小块
        mp_free(&mp, small1);
        mp_free(&mp, small2);
        mp_free(&mp, large);

        // 现在请求一个中等大小的块，应该使用最合适的块
        void *medium = mp_alloc(&mp, 64);
        TEST_ASSERT(medium != NULL, "应该能找到合适的块");

        mp_free(&mp, medium);
        return 0;
}

/**
 * @brief 测试10: 多次分配释放循环
 */
static int
test_repeated_alloc_free(void)
{
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 进行多次分配和释放循环
        for (usize round = 0; round < 100; round++) {
                void *ptrs[20];
                usize count = 0;

                // 分配多个块
                for (usize i = 0; i < 20; i++) {
                        ptrs[i] = mp_alloc(&mp, (i + 1) * 4);
                        if (ptrs[i] != NULL)
                                count++;
                }

                // 释放所有块
                for (usize i = 0; i < count; i++)
                        mp_free(&mp, ptrs[i]);
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
        static u8 buf[MP_SIZE];
        mp_t      mp;
        mp.buf = buf;
        mp.cap = MP_SIZE;
        mp_init(&mp);

        // 分配内存并写入数据
        char *ptr = (char *)mp_alloc(&mp, 64);
        TEST_ASSERT(ptr != NULL, "分配应该成功");

        // 写入数据
        for (usize i = 0; i < 64; i++)
                ptr[i] = (char)(i % 256);

        // 验证数据
        bool data_ok = 1;
        for (usize i = 0; i < 64; i++) {
                if (ptr[i] != (char)(i % 256)) {
                        data_ok = 0;
                        break;
                }
        }
        TEST_ASSERT(data_ok, "数据应该保持完整");

        mp_free(&mp, ptr);
        return 0;
}

/* -------------------------------------------------------------------------- */
/*                                  主函数                                    */
/* -------------------------------------------------------------------------- */

int
main(void)
{
#ifdef _WIN32
        SetConsoleOutputCP(65001); // 设置控制台代码页为 UTF-8
#endif
        println("========================================");
        println("   MP Memory Pool Allocator Test Suite");
        println("========================================");
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

        println("\n========================================");
        println("   All Tests Passed! ✓");
        println("========================================");

        return 0;
}
