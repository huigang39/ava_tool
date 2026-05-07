#include <stdio.h>
#include <string.h>

#include "log.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static void
log_os_mmap_init(log_t *log)
{
        DECL(log, cfg, lo);

        lo->mmap_ptr       = NULL;
        lo->os_file_handle = NULL;
        lo->os_map_handle  = NULL;

        if (cfg->file_path == NULL || cfg->file_size == 0)
                return;

        if (cfg->e_ring == LOG_RING_ROTATE) {
                snprintf(lo->curr_file_path,
                         sizeof(lo->curr_file_path),
                         "%s.%llu",
                         cfg->file_path,
                         (unsigned long long)lo->curr_file_idx);
        } else
                snprintf(lo->curr_file_path, sizeof(lo->curr_file_path), "%s", cfg->file_path);

#if defined(_WIN32) || defined(_WIN64)
        DWORD  creation_disp = (cfg->e_ring == LOG_RING_ROTATE) ? CREATE_ALWAYS : OPEN_ALWAYS;
        HANDLE hFile         = CreateFileA(lo->curr_file_path,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ,
                                   NULL,
                                   creation_disp,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile == INVALID_HANDLE_VALUE)
                return;

        HANDLE hMap = CreateFileMappingA(
            hFile, NULL, PAGE_READWRITE, (DWORD)(cfg->file_size >> 32), (DWORD)(cfg->file_size & 0xFFFFFFFF), NULL);
        if (hMap == NULL) {
                CloseHandle(hFile);
                return;
        }

        lo->mmap_ptr       = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, cfg->file_size);
        lo->os_file_handle = (void *)hFile;
        lo->os_map_handle  = (void *)hMap;

#elif defined(__linux__) || defined(__APPLE__)
        int flags = O_RDWR | O_CREAT;
        if (cfg->e_ring == LOG_RING_ROTATE)
                flags |= O_TRUNC;

        int fd = open(lo->curr_file_path, flags, 0644);
        if (fd < 0)
                return;

        if (ftruncate(fd, (off_t)cfg->file_size) == -1) {
                close(fd);
                return;
        }

        lo->mmap_ptr = mmap(NULL, cfg->file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (lo->mmap_ptr == MAP_FAILED) {
                lo->mmap_ptr = NULL;
                close(fd);
                return;
        }

        lo->os_file_handle = (void *)(intptr_t)fd;
#endif
}

static void
log_os_mmap_deinit(log_t *log, usize actual_size)
{
        ARG_UNUSED(actual_size);
        DECL(log, cfg, lo);

        if (lo->mmap_ptr == NULL)
                return;

#if defined(_WIN32) || defined(_WIN64)
        UnmapViewOfFile(lo->mmap_ptr);
        if (lo->os_map_handle)
                CloseHandle((HANDLE)lo->os_map_handle);

        if (lo->os_file_handle) {
                if (cfg->e_ring != LOG_RING_WRAP) {
                        LARGE_INTEGER li;
                        li.QuadPart = actual_size;
                        SetFilePointerEx((HANDLE)lo->os_file_handle, li, NULL, FILE_BEGIN);
                        SetEndOfFile((HANDLE)lo->os_file_handle);
                }
                CloseHandle((HANDLE)lo->os_file_handle);
        }
#elif defined(__linux__) || defined(__APPLE__)
        munmap(lo->mmap_ptr, cfg->file_size);
        if (lo->os_file_handle) {
                int fd = (int)(intptr_t)lo->os_file_handle;
                if (cfg->e_ring != LOG_RING_WRAP)
                        ftruncate(fd, (off_t)actual_size);

                close(fd);
        }
#endif

        lo->mmap_ptr       = NULL;
        lo->os_map_handle  = NULL;
        lo->os_file_handle = NULL;
}

static void
log_flush_ring(log_t *log, const void *src, usize size)
{
        DECL(log, cfg, lo);

        if (cfg->file_size == 0) {
                if (cfg->f_flush)
                        cfg->f_flush(cfg->fd, src, size);

                return;
        }

        const u8 use_mmap = (lo->mmap_ptr != NULL);
        if (!use_mmap && cfg->fd == NULL && cfg->f_flush == NULL)
                return;

        FILE *fp = (FILE *)cfg->fd;

        if (cfg->e_ring == LOG_RING_ROTATE) {
                if (lo->file_offset + size > cfg->file_size) {
                        log_os_mmap_deinit(log, lo->file_offset);
                        lo->curr_file_idx++;
                        if (cfg->max_files > 0 && lo->curr_file_idx >= cfg->max_files)
                                lo->curr_file_idx = 0;

                        log_os_mmap_init(log);
                        lo->file_offset = 0;
                }
        }

        const usize remaining_space = cfg->file_size - lo->file_offset;

        if (cfg->e_ring == LOG_RING_TRUNCATE) {
                if (remaining_space == 0)
                        return;

                if (size > remaining_space) {
                        if (use_mmap) {
                                memcpy((u8 *)lo->mmap_ptr + lo->file_offset, src, remaining_space);
                                lo->file_offset = cfg->file_size;
                        } else if (fseek(fp, (long)lo->file_offset, SEEK_SET) == 0) {
                                cfg->f_flush(cfg->fd, src, remaining_space);
                                lo->file_offset = cfg->file_size;
                        }
                        return;
                }
        }

        if (cfg->e_ring == LOG_RING_COMPLETE) {
                if (size > remaining_space)
                        lo->file_offset = 0;
        }

        const u8 *data      = (const u8 *)src;
        usize     remaining = size;

        while (remaining > 0) {
                usize write_size = remaining;
                if (lo->file_offset + write_size > cfg->file_size)
                        write_size = cfg->file_size - lo->file_offset;

                if (use_mmap)
                        memcpy((u8 *)lo->mmap_ptr + lo->file_offset, data, write_size);
                else {
                        if (fseek(fp, (long)lo->file_offset, SEEK_SET) == 0)
                                cfg->f_flush(cfg->fd, data, write_size);
                        else
                                break;
                }

                lo->file_offset += write_size;
                data            += write_size;
                remaining       -= write_size;

                if (lo->file_offset >= cfg->file_size)
                        lo->file_offset = 0;
        }
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
log_init(log_t *log, const log_cfg_t log_cfg)
{
        DECL(log, cfg, lo);
        CFG_INIT(log, log_cfg);

        lo->flush_buf     = (u8 *)mempool_alloc(cfg->mempool, cfg->flush_cap);
        lo->file_offset   = 0;
        lo->curr_file_idx = 0;

        lo->chunks = (log_chunk_t **)mempool_calloc(cfg->mempool, sizeof(log_chunk_t *) * cfg->nproducers);

        mpsc_init(&lo->mpsc);
        log_os_mmap_init(log);
}

void
log_deinit(log_t *log)
{
        DECL(log, cfg, lo);

        // 1. 自动收集并提交所有线程残留在数组里的 Chunk
        if (lo->chunks) {
                for (usize i = 0; i < cfg->nproducers; i++) {
                        log_chunk_t *chunk = lo->chunks[i];
                        if (chunk) {
                                if (chunk->offset > 0)
                                        mpsc_push(&lo->mpsc, (mpsc_node_t *)chunk);
                                else
                                        mempool_free(cfg->mempool, chunk);

                                lo->chunks[i] = NULL;
                        }
                }
        }

        // 2. 最后一次刷盘
        log_flush(log);

        // 3. 安全解除内存映射，并收缩文件尾巴
        log_os_mmap_deinit(log, lo->file_offset);

        // 4. 回收内存池资源
        if (lo->flush_buf) {
                mempool_free(cfg->mempool, lo->flush_buf);
                lo->flush_buf = NULL;
        }
        if (lo->chunks) {
                mempool_free(cfg->mempool, lo->chunks);
                lo->chunks = NULL;
        }
}

void
log_write_bin(
    log_t *log, const usize id, const void *header, const usize header_size, const void *payload, const usize payload_size)
{
        DECL(log, cfg, lo);

        if (id >= cfg->nproducers)
                return; // 安全检查

        const log_header_t log_header = {
            .ts   = cfg->f_get_ts(),
            .id   = id,
            .size = header_size + payload_size,
        };

        const usize total_size = sizeof(log_header) + log_header.size;
        if (total_size > cfg->chunk_size)
                return;

        log_chunk_t *chunk = lo->chunks[id]; // 通过 ID 定位到私有内存块

        if (chunk == NULL || chunk->offset + total_size > cfg->chunk_size) {
                if (chunk != NULL)
                        mpsc_push(&lo->mpsc, (mpsc_node_t *)chunk);

                chunk = mempool_alloc(cfg->mempool, sizeof(log_chunk_t) + cfg->chunk_size);
                if (!chunk) {
                        lo->chunks[id] = NULL;
                        return; // 内存耗尽保护机制
                }
                chunk->offset  = 0;
                lo->chunks[id] = chunk;
        }

        u8 *ptr = chunk->data + chunk->offset;

        memcpy(ptr, &log_header, sizeof(log_header));
        ptr += sizeof(log_header);

        memcpy(ptr, header, header_size);
        ptr += header_size;

        memcpy(ptr, payload, payload_size);
        ptr += payload_size;

        chunk->offset += total_size;
}

void
log_write(log_t *log, const usize idx, const char *fmt, const va_list args)
{
        DECL(log, cfg, lo);

        if (idx >= cfg->nproducers)
                return; // 安全检查

        const log_header_t header = {
            .ts   = cfg->f_get_ts(),
            .id   = idx,
            .size = (usize)vsnprintf(NULL, 0, fmt, args) + 1,
        };

        const usize total_size = sizeof(header) + header.size;
        if (total_size > cfg->chunk_size)
                return;

        log_chunk_t *chunk = lo->chunks[idx]; // 通过 IDX 定位到私有内存块

        if (chunk == NULL || chunk->offset + total_size > cfg->chunk_size) {
                if (chunk != NULL)
                        mpsc_push(&lo->mpsc, (mpsc_node_t *)chunk);

                chunk = mempool_alloc(cfg->mempool, sizeof(log_chunk_t) + cfg->chunk_size);
                if (!chunk) {
                        lo->chunks[idx] = NULL;
                        return; // 内存池耗尽，丢弃日志
                }
                chunk->offset   = 0;
                lo->chunks[idx] = chunk;
        }

        u8 *ptr = chunk->data + chunk->offset;

        memcpy(ptr, &header, sizeof(header));
        ptr += sizeof(header);

        vsnprintf((char *)ptr, header.size, fmt, args);

        chunk->offset += total_size;
}

void
log_flush(log_t *log)
{
        DECL(log, cfg, lo);

        while (!lo->busy) {
                mpsc_node_t *node = mpsc_pop(&lo->mpsc);
                if (node == NULL)
                        break;

                log_chunk_t *chunk = (log_chunk_t *)node;

                if (cfg->e_format == LOG_FORMAT_TEXT) {
                        usize parse_offset = 0;
                        while (parse_offset < chunk->offset) {
                                const log_header_t *header  = (log_header_t *)(chunk->data + parse_offset);
                                parse_offset               += sizeof(log_header_t);

                                const char *payload  = (char *)(chunk->data + parse_offset);
                                parse_offset        += header->size;

#ifdef MCU
                                const usize prefix_size =
                                    snprintf((char *)lo->flush_buf, cfg->flush_cap, "[%llu][%u] ", header->ts, (u32)header->id);
#else
                                const usize prefix_size = snprintf(
                                    (char *)lo->flush_buf, cfg->flush_cap, "[%llu][%llu] ", header->ts, (u64)header->id);
#endif

                                const usize payload_len = header->size - 1;
                                usize       copy_len    = payload_len;
                                if (prefix_size + copy_len > cfg->flush_cap)
                                        copy_len = cfg->flush_cap - prefix_size;

                                memcpy(lo->flush_buf + prefix_size, payload, copy_len);
                                log_flush_ring(log, lo->flush_buf, prefix_size + copy_len);
                        }
                } else
                        log_flush_ring(log, chunk->data, chunk->offset);

                mempool_free(cfg->mempool, chunk);
                lo->busy = cfg->e_mode == LOG_MODE_ASYNC;
        }
}

void
log_data(log_t *log, const usize idx, const char *fmt, ...)
{
        DECL(log, cfg);
        if (cfg->e_level > LOG_LEVEL_DATA)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, idx, fmt, args);
        va_end(args);
}

void
log_debug(log_t *log, const usize idx, const char *fmt, ...)
{
        DECL(log, cfg);
        if (cfg->e_level > LOG_LEVEL_DEBUG)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, idx, fmt, args);
        va_end(args);
}

void
log_info(log_t *log, const usize idx, const char *fmt, ...)
{
        DECL(log, cfg);
        if (cfg->e_level > LOG_LEVEL_INFO)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, idx, fmt, args);
        va_end(args);
}

void
log_warn(log_t *log, const usize idx, const char *fmt, ...)
{
        DECL(log, cfg);
        if (cfg->e_level > LOG_LEVEL_WARN)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, idx, fmt, args);
        va_end(args);
}

void
log_error(log_t *log, const usize idx, const char *fmt, ...)
{
        DECL(log, cfg);
        if (cfg->e_level > LOG_LEVEL_ERR)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, idx, fmt, args);
        va_end(args);
}
