#ifndef SHM_H
#define SHM_H

#include "platdef.h"

#ifdef OS_POSIX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(OS_WIN)
#include <windows.h>
#endif

#include "spsc.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum shm_access {
#ifdef OS_POSIX
        SHM_READONLY  = PROT_READ,
        SHM_WRITEONLY = PROT_WRITE,
        SHM_READWRITE = SHM_READONLY | SHM_WRITEONLY,
#elif defined(OS_WIN)
        SHM_READONLY  = PAGE_READONLY,
        SHM_READWRITE = PAGE_READWRITE,
#elif defined(MCU)
        SHM_READONLY,
        SHM_READWRITE,
#endif
} shm_access_e;

typedef struct shm_cfg {
        const char  *name;
        shm_access_e access;
        usize        cap;
} shm_cfg_t;

typedef struct shm_lo {
#ifdef OS_POSIX
        int fd;
#elif defined(OS_WIN)
        HANDLE fd;
#endif
        void   *addr;
        u8      is_creator;
        spsc_t *spsc;
} shm_lo_t;

typedef struct shm {
        shm_cfg_t cfg;
        shm_lo_t  lo;
} shm_t;

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
int shm_init(shm_t *shm, shm_cfg_t shm_cfg);

/**
 * @brief 共享内存数据读取
 *
 * @param shm  共享内存结构体
 * @param dst  待读取的数据目标地址
 * @param size 读取字节数
 */
usize shm_read(shm_t *shm, void *dst, usize size);

/**
 * @brief 共享内存数据写入
 *
 * @param shm  共享内存结构体
 * @param src  待写入的数据源地址
 * @param size 写入字节数
 */
usize shm_write(shm_t *shm, const void *src, usize size);

#ifdef __cplusplus
}
#endif

#endif // !SHM_H
