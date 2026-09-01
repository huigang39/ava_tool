#include "shm.h"
#include "errdef.h"
#include "mathdef.h"

int shm_init(struct shm *shm, const struct shm_cfg shm_cfg)
{
    if (!shm || !shm_cfg.name) {
        return MERR(MENULLPTR);
    }
    if (shm_cfg.cap < sizeof(struct spsc) * 2U ||
        !IS_POWER_OF_2(shm_cfg.cap >> 1U)) {
        return MERR(MEINVAL);
    }

    if (shm_cfg.access != SHM_READWRITE) {
        return MERR(MENOTSUP);
    }
#if OS(WIN)
    if (shm_cfg.cap > UINT32_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif

    DECL(shm, cfg, lo);
    memset(lo, 0, sizeof(*lo));
    CFG_INIT(shm, shm_cfg);

#if OS(POSIX)
    lo->fd = shm_open(cfg->name, O_RDWR, 0666);
    if (lo->fd == -1) {
        lo->fd = shm_open(cfg->name, O_CREAT | O_RDWR, 0666);
        if (lo->fd == -1) {
            return MERR(MEACCES);
        }

        lo->is_creator = 1;

        if (ftruncate(lo->fd, cfg->cap) == -1) {
            close(lo->fd);
            return MERR(MEACCES);
        }
    } else {
        lo->is_creator = 0;
    }

    lo->addr = mmap(NULL, cfg->cap, cfg->access, MAP_SHARED, lo->fd, 0);
    if (lo->addr == MAP_FAILED) {
        close(lo->fd);
        if (lo->is_creator) {
            shm_unlink(cfg->name);
        }

        return MERR(MEACCES);
    }
#elif OS(WIN)
    lo->fd = OpenFileMapping(FILE_MAP_ALL_ACCESS, // 读写权限
                             0,                   // 不继承句柄
                             cfg->name);          // 共享内存名称
    if (lo->fd == NULL) {
        lo->fd = CreateFileMapping(INVALID_HANDLE_VALUE, // 使用物理内存
                                   NULL,                 // 默认安全属性
                                   cfg->access,          // 可读可写
                                   0,                    // 内存大小高32位
                                   (DWORD)cfg->cap,      // 内存大小低32位
                                   cfg->name);           // 命名对象
        if (lo->fd == NULL) {
            return MERR(MECREATE);
        }

        lo->is_creator = GetLastError() != ERROR_ALREADY_EXISTS;
    } else {
        lo->is_creator = 0;
    }

    // 映射到进程地址空间
    lo->addr = MapViewOfFile(lo->fd,              // 文件映射句柄
                             FILE_MAP_ALL_ACCESS, // 读写权限
                             0,
                             0,         // 偏移量
                             cfg->cap); // 映射大小
    if (lo->addr == NULL) {
        CloseHandle(lo->fd);
        return MERR(MEACCES);
    }
#else
    return MERR(MENOTSUP);
#endif

    lo->spsc = (struct spsc *)lo->addr;
    const size_t queue_cap = cfg->cap >> 1U;
    const int ret = lo->is_creator
                        ? spsc_init_buf(lo->spsc, queue_cap, SPSC_POLICY_REJECT)
                        : (lo->spsc->cap == queue_cap &&
                                   lo->spsc->policy == SPSC_POLICY_REJECT
                               ? MOK
                               : MERR(MESTATE));
    if (ret != MOK) {
        (void)shm_deinit(shm);
    }
    return ret;
}

int shm_deinit(struct shm *shm)
{
    if (!shm) {
        return MERR(MENULLPTR);
    }
    DECL(shm, cfg, lo);

    if (!lo->addr || !lo->spsc) {
        return MERR(MESTATE);
    }

    int ret = MOK;
#if OS(POSIX)
    if (munmap(lo->addr, cfg->cap) != 0) {
        ret = MERR(MEIO);
    }
    if (close(lo->fd) != 0) {
        ret = MERR(MEIO);
    }
    if (lo->is_creator && shm_unlink(cfg->name) != 0) {
        ret = MERR(MEIO);
    }
#elif OS(WIN)
    if (UnmapViewOfFile(lo->addr) == 0) {
        ret = MERR(MEIO);
    }
    if (CloseHandle(lo->fd) == 0) {
        ret = MERR(MEIO);
    }
#else
    ret = MERR(MENOTSUP);
#endif
    memset(lo, 0, sizeof(*lo));
    return ret;
}

ptrdiff_t shm_read(struct shm *shm, void *dest, const size_t size)
{
    if (!shm || !dest) {
        return MERR(MENULLPTR);
    }
    if (!shm->lo.addr || !shm->lo.spsc) {
        return MERR(MESTATE);
    }
    DECL(shm, lo);

    return (ptrdiff_t)spsc_read_buf(
        lo->spsc, (uint8_t *)lo->addr + sizeof(*lo->spsc), dest, size);
}

ptrdiff_t shm_write(struct shm *shm, const void *source, const size_t size)
{
    if (!shm || !source) {
        return MERR(MENULLPTR);
    }
    if (!shm->lo.addr || !shm->lo.spsc) {
        return MERR(MESTATE);
    }
    DECL(shm, lo);

    return (ptrdiff_t)spsc_write_buf(
        lo->spsc, (uint8_t *)lo->addr + sizeof(*lo->spsc), source, size);
}
