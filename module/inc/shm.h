#ifndef SHM_H
#define SHM_H

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

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum shm_access {
#if OS(POSIX)
    SHM_READONLY  = PROT_READ,
    SHM_WRITEONLY = PROT_WRITE,
    SHM_READWRITE = SHM_READONLY | SHM_WRITEONLY,
#elif OS(WIN)
    SHM_READONLY  = PAGE_READONLY,
    SHM_READWRITE = PAGE_READWRITE,
#elif OS(NONE)
    SHM_READONLY,
    SHM_READWRITE,
#endif
};

struct shm_cfg {
    const char     *name;
    enum shm_access access;
    size_t          cap;
};

struct shm_lo {
#if OS(POSIX)
    int fd;
#elif OS(WIN)
    HANDLE fd;
#endif
    void        *addr;
    uint8_t      is_creator;
    struct spsc *spsc;
};

struct shm {
    struct shm_cfg cfg;
    struct shm_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 共享内存结构体初始化
 *
 * @param shm     共享内存结构体
 * @param shm_cfg 共享内存配置
 * @return        int 状态码
 */
int shm_init(struct shm *shm, struct shm_cfg shm_cfg);

/**
 * @brief 共享内存数据读取
 *
 * @param shm  共享内存结构体
 * @param dst  待读取的数据目标地址
 * @param size 读取字节数
 */
size_t shm_read(struct shm *shm, void *dst, size_t size);

/**
 * @brief 共享内存数据写入
 *
 * @param shm  共享内存结构体
 * @param src  待写入的数据源地址
 * @param size 写入字节数
 */
size_t shm_write(struct shm *shm, const void *src, size_t size);

#ifdef __cplusplus
}
#endif

#endif // !SHM_H
