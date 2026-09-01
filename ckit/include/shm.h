#ifndef SHM_H
#define SHM_H

#include "errdef.h"
#include "platdef.h"

#if OS(POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif OS(WIN)
#include <windows.h>
#endif

#include "spsc.h"
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

enum shm_access {
#if OS(POSIX)
    SHM_READONLY = PROT_READ,                     // 只读访问
    SHM_WRITEONLY = PROT_WRITE,                   // 只写访问
    SHM_READWRITE = SHM_READONLY | SHM_WRITEONLY, // 读写访问
#elif OS(WIN)
    SHM_READONLY = PAGE_READONLY,   // 只读访问
    SHM_READWRITE = PAGE_READWRITE, // 读写访问
#elif OS(NONE)
    SHM_READONLY,  // 只读访问
    SHM_READWRITE, // 读写访问
#endif
};

struct shm_cfg {
    // 共享内存标识与访问权限
    const char *name;
    enum shm_access access; // 共享内存访问权限

    // 共享存储容量
    size_t cap; // 存储容量, 单位: byte
};

struct shm_lo {
#if OS(POSIX)
    int fd; // 文件或通信句柄
#elif OS(WIN)
    HANDLE fd; // 文件或通信句柄
#endif

    // 映射存储与创建状态
    void *addr;         // 共享内存映射地址
    uint8_t is_creator; // 本进程创建了共享内存

    // 共享通信队列
    struct spsc *spsc; // 共享通信队列
};

struct shm {
    struct shm_cfg cfg; // 配置参数
    struct shm_lo lo;   // 运行状态
};

/**
 * @brief                 共享内存结构体初始化
 * @param[in,out] shm     共享内存结构体
 * @param[in]     shm_cfg 共享内存配置
 * @return                成功返回 0, 失败返回负错误码
 * @note                  队列读写会更新游标, 仅支持 SHM_READWRITE
 */
int shm_init(struct shm *shm, struct shm_cfg shm_cfg);

/**
 * @brief             释放共享内存资源
 * @param[in,out] shm 共享内存对象
 * @return            操作结果, 失败时返回对应错误码
 */
int shm_deinit(struct shm *shm);

/**
 * @brief              共享内存数据读取
 * @param[in,out] shm  共享内存结构体
 * @param[out]    dest 读取目标存储
 * @param[in]     size 读取字节数
 * @return             操作结果, 失败时返回对应错误码
 */
ptrdiff_t shm_read(struct shm *shm, void *dest, size_t size);

/**
 * @brief                共享内存数据写入
 * @param[in,out] shm    共享内存结构体
 * @param[in]     source 待写入的数据源地址
 * @param[in]     size   写入字节数
 * @return               操作结果, 失败时返回对应错误码
 */
ptrdiff_t shm_write(struct shm *shm, const void *source, size_t size);

#ifdef __cplusplus
}
#endif

#endif
