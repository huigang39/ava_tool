#ifndef SHM_H
#define SHM_H

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
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
#ifdef __linux__
        SHM_READONLY  = PROT_READ,
        SHM_WRITEONLY = PROT_WRITE,
        SHM_READWRITE = SHM_READONLY | SHM_WRITEONLY,
#elif defined(_WIN32)
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
#ifdef __linux__
        int fd;
#elif defined(_WIN32)
        HANDLE fd;
#endif
        void   *base;
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

int  shm_init(shm_t *shm, shm_cfg_t shm_cfg);
void shm_read(shm_t *shm, void *dst, usize size);
void shm_write(shm_t *shm, const void *src, usize size);

#ifdef __cplusplus
}
#endif

#endif // !SHM_H
