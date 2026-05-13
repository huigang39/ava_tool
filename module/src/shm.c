#include "shm.h"
#include "errdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

i32
shm_init(shm_t *shm, const shm_cfg_t shm_cfg)
{
        DECL(shm, cfg, lo);
        CFG_INIT(shm, shm_cfg);

#ifdef OS_POSIX
        lo->fd = shm_open(cfg->name, O_RDWR, 0666);
        if (lo->fd == -1) {
                lo->fd = shm_open(cfg->name, O_CREAT | O_RDWR, 0666);
                if (lo->fd == -1)
                        return -MEACCES;

                lo->is_creator = 1;

                if (ftruncate(lo->fd, cfg->cap) == -1) {
                        close(lo->fd);
                        return -MEACCES;
                }
        } else
                lo->is_creator = 0;

        lo->addr = mmap(NULL, cfg->cap, cfg->access, MAP_SHARED, lo->fd, 0);
        if (lo->addr == MAP_FAILED) {
                close(lo->fd);
                if (lo->is_creator)
                        shm_unlink(cfg->name);

                return -MEACCES;
        }
#elif defined(_WIN32)
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
                if (lo->fd == NULL)
                        return -MECREATE;

                lo->is_creator = 1;
        } else
                lo->is_creator = 0;

        // 映射到进程地址空间
        lo->addr = MapViewOfFile(lo->fd,              // 文件映射句柄
                                 FILE_MAP_ALL_ACCESS, // 读写权限
                                 0,
                                 0,         // 偏移量
                                 cfg->cap); // 映射大小
        if (lo->addr == NULL) {
                UnmapViewOfFile(lo->addr);
                CloseHandle(lo->fd);
                return -MEACCES;
        }
#endif

        lo->spsc = (spsc_t *)lo->addr;
        if (lo->is_creator)
                spsc_init_buf(lo->spsc, cfg->cap >> 1, SPSC_POLICY_REJECT);

        return 0;
}

usize
shm_read(shm_t *shm, void *dst, const usize size)
{
        DECL(shm, lo);

        return spsc_read_buf(lo->spsc, (u8 *)lo->addr + sizeof(*lo->spsc), dst, size);
}

usize
shm_write(shm_t *shm, const void *src, const usize size)
{
        DECL(shm, lo);

        return spsc_write_buf(lo->spsc, (u8 *)lo->addr + sizeof(*lo->spsc), src, size);
}
