#include <stdio.h>
#include <string.h>

#include "log.h"
#include "macrodef.h"
#include "mathdef.h"
#include "mempool.h"
#include "timeops.h"

#if OS(WIN)
#include <windows.h>
#elif OS(POSIX)
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static void
purge_old_logs(const char *dir, size_t max_files, const char *extension)
{
    if (max_files == 0)
        return;

#if !OS(WIN) && !OS(POSIX)
    ARG_UNUSED(dir);
    ARG_UNUSED(extension);
#endif

#if OS(WIN)
    char search_path[256];
    snprintf(search_path, sizeof(search_path), "%s/*.%s", dir, extension);
    WIN32_FIND_DATAA find_data;
    HANDLE           hFind = FindFirstFileA(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    // 收集所有匹配的文件名
    struct file_node {
        char name[256];
    };
    struct file_node *files = NULL;
    size_t            count = 0;

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files = (struct file_node *)realloc(files, sizeof(struct file_node) * (count + 1));
            size_t name_len = 0U;
            while (name_len + 1U < sizeof(files[count].name) &&
                   find_data.cFileName[name_len] != '\0') {
                name_len++;
            }
            memcpy(files[count].name, find_data.cFileName, name_len);
            files[count].name[name_len] = '\0';
            count++;
        }
    } while (FindNextFileA(hFind, &find_data));
    FindClose(hFind);

    if (count > max_files) {
        // 按名称排序 (时间戳格式保证了名称序即时间序)
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (strcmp(files[i].name, files[j].name) > 0) {
                    struct file_node tmp = files[i];
                    files[i]             = files[j];
                    files[j]             = tmp;
                }
            }
        }
        // 删除多余的最旧文件
        for (size_t i = 0; i < count - max_files; i++) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, files[i].name);
            DeleteFileA(full_path);
        }
    }
    free(files);
#elif OS(POSIX)
    DIR *dp = opendir(dir);
    if (dp == NULL)
        return;

    struct file_node {
        char name[256];
    };
    struct file_node *files = NULL;
    size_t            count = 0;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char  *name          = ent->d_name;
        const size_t n             = strlen(name);
        const size_t extension_len = strlen(extension);
        if (n <= extension_len || name[n - extension_len - 1U] != '.' ||
            strcmp(name + n - extension_len, extension) != 0)
            continue;

        struct file_node *new_files =
            (struct file_node *)realloc(files, sizeof(struct file_node) * (count + 1));
        if (new_files == NULL)
            break;
        files = new_files;
        strncpy(files[count].name, name, sizeof(files[count].name) - 1);
        files[count].name[sizeof(files[count].name) - 1] = '\0';
        count++;
    }
    closedir(dp);

    if (count > max_files) {
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (strcmp(files[i].name, files[j].name) > 0) {
                    struct file_node tmp = files[i];
                    files[i]             = files[j];
                    files[j]             = tmp;
                }
            }
        }
        for (size_t i = 0; i < count - max_files; i++) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, files[i].name);
            unlink(full_path);
        }
    }
    free(files);
#endif
}

static void
get_timestamp_str(char *buf, size_t size)
{
    const uint64_t timestamp_us = get_real_ts_us();
    const time_t   seconds      = (time_t)(timestamp_us / 1000000U);
    struct tm      tm           = {0};

#if OS(WIN)
    localtime_s(&tm, &seconds);
#elif OS(POSIX)
    localtime_r(&seconds, &tm);
#else
    const struct tm *local = localtime(&seconds);
    if (local != NULL) {
        tm = *local;
    }
#endif
    const size_t length = strftime(buf, size, "%Y%m%d_%H%M%S", &tm);
    if (length > 0U && length < size) {
        snprintf(buf + length, size - length, "_%06u", (unsigned int)(timestamp_us % 1000000U));
    }
}

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static uint8_t *
log_chunk_data(struct log_chunk *chunk)
{
    return (uint8_t *)(chunk + 1);
}

static void
log_os_mmap_init(struct log *log)
{
    DECL(log, cfg, lo);
    const char *extension = cfg->e_format == LOG_FORMAT_CSV ? "csv" : "log";

    lo->mmap_ptr       = NULL;
    lo->os_file_handle = NULL;
    lo->os_map_handle  = NULL;

    if (cfg->file_path == NULL || cfg->file_size == 0)
        return;

    if (cfg->e_ring == LOG_RING_ROTATE) {
#if OS(WIN)
        CreateDirectoryA(cfg->file_path, NULL);
#elif OS(POSIX)
        mkdir(cfg->file_path, 0755);
#endif
        char ts[32];
        get_timestamp_str(ts, sizeof(ts));
        snprintf(lo->curr_file_path,
                 sizeof(lo->curr_file_path),
                 "%s/%s.%s",
                 cfg->file_path,
                 ts,
                 extension);
    } else
        snprintf(lo->curr_file_path, sizeof(lo->curr_file_path), "%s", cfg->file_path);

#if OS(WIN)
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

    HANDLE hMap = CreateFileMappingA(hFile,
                                     NULL,
                                     PAGE_READWRITE,
                                     (DWORD)(cfg->file_size >> 32),
                                     (DWORD)(cfg->file_size & 0xFFFFFFFF),
                                     NULL);
    if (hMap == NULL) {
        CloseHandle(hFile);
        return;
    }

    lo->mmap_ptr       = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, cfg->file_size);
    lo->os_file_handle = (void *)hFile;
    lo->os_map_handle  = (void *)hMap;

#elif OS(POSIX)
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

    if (cfg->file_header != NULL && lo->mmap_ptr != NULL) {
        const size_t header_size = strlen(cfg->file_header);
        if (header_size <= cfg->file_size) {
            memcpy(lo->mmap_ptr, cfg->file_header, header_size);
            lo->file_offset = header_size;
        }
    }

    if (cfg->e_ring == LOG_RING_ROTATE && lo->mmap_ptr != NULL) {
        purge_old_logs(cfg->file_path, cfg->max_files, extension);
    }
}

static void
log_os_mmap_deinit(struct log *log, size_t actual_size)
{
    ARG_UNUSED(actual_size);
    DECL(log, cfg, lo);

    if (lo->mmap_ptr == NULL)
        return;

#if OS(WIN)
    UnmapViewOfFile(lo->mmap_ptr);
    if (lo->os_map_handle)
        CloseHandle((HANDLE)lo->os_map_handle);

    if (lo->os_file_handle) {
        LARGE_INTEGER li;
        li.QuadPart = actual_size;
        SetFilePointerEx((HANDLE)lo->os_file_handle, li, NULL, FILE_BEGIN);
        SetEndOfFile((HANDLE)lo->os_file_handle);
        CloseHandle((HANDLE)lo->os_file_handle);
    }
#elif OS(POSIX)
    munmap(lo->mmap_ptr, cfg->file_size);
    if (lo->os_file_handle) {
        int32_t fd = (int32_t)(intptr_t)lo->os_file_handle;
        int     _r = ftruncate(fd, (off_t)actual_size);
        (void)_r;
        close(fd);
    }
#endif

    lo->mmap_ptr       = NULL;
    lo->os_map_handle  = NULL;
    lo->os_file_handle = NULL;
}

static void
log_flush_ring(struct log *log, const void *src, size_t size)
{
    DECL(log, cfg, lo);

    if (cfg->file_size == 0 || cfg->file_path == NULL) {
        if (cfg->f_flush)
            cfg->f_flush(cfg->fd, src, size);
        return;
    }

    const uint8_t use_mmap = (lo->mmap_ptr != NULL);
    if (!use_mmap && cfg->fd == NULL && cfg->f_flush == NULL)
        return;

    if (cfg->e_ring == LOG_RING_ROTATE) {
        if (lo->file_offset + size > cfg->file_size) {
            log_os_mmap_deinit(log, lo->file_offset);
            lo->file_offset = 0;
            log_os_mmap_init(log);
            if (lo->mmap_ptr == NULL && cfg->fd == NULL && cfg->f_flush == NULL) {
                return;
            }
        }
    }

    if (cfg->e_ring == LOG_RING_TRUNCATE) {
        const size_t remaining_space =
            (lo->file_offset < cfg->file_size) ? (cfg->file_size - lo->file_offset) : 0;
        if (size > remaining_space)
            size = remaining_space;
    }

    const uint8_t *data      = (const uint8_t *)src;
    size_t         remaining = size;

    while (remaining > 0) {
        size_t write_size = remaining;
        if (lo->file_offset + write_size > cfg->file_size)
            write_size = cfg->file_size - lo->file_offset;

        if (write_size > 0) {
            if (use_mmap)
                memcpy((uint8_t *)lo->mmap_ptr + lo->file_offset, data, write_size);
            else {
                FILE *fp = (FILE *)cfg->fd;
                if (fseek(fp, (long)lo->file_offset, SEEK_SET) == 0)
                    cfg->f_flush(cfg->fd, data, write_size);
                else
                    break;
            }

            lo->file_offset += write_size;
            data            += write_size;
            remaining       -= write_size;
        }

        if (lo->file_offset >= cfg->file_size)
            break;
    }
}

static void
log_submit_pending(struct log *log)
{
    DECL(log, cfg, lo);

    if (!lo->producers)
        return;

    for (size_t i = 0; i < cfg->nproducers; i++) {
        struct log_producer *producer = &lo->producers[i];
        struct log_chunk    *chunk    = NULL;

        SPIN_LOCK(&producer->lock);
        if (producer->chunk && producer->chunk->offset > 0) {
            chunk           = producer->chunk;
            producer->chunk = NULL;
        }
        SPIN_UNLOCK(&producer->lock);

        if (chunk)
            mpsc_push(&lo->mpsc, &chunk->node);
    }
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
log_init(struct log *log, const struct log_cfg log_cfg)
{
    DECL(log, cfg, lo);
    CFG_INIT(log, log_cfg);

    lo->flush_buf   = (uint8_t *)mempool_alloc(cfg->mempool, cfg->flush_cap);
    lo->file_offset = 0;

    lo->producers = (struct log_producer *)mempool_calloc(
        cfg->mempool, sizeof(struct log_producer) * cfg->nproducers);
    if (lo->producers) {
        for (size_t i = 0; i < cfg->nproducers; i++)
            ATOMIC_STORE_EXPLICIT(&lo->producers[i].lock, false, ATOMIC_RELAXED);
    }

    mpsc_init(&lo->mpsc);
    log_os_mmap_init(log);
}
void
log_deinit(struct log *log)
{
    DECL(log, cfg, lo);

    log_submit_pending(log);
    log_flush(log);

    if (lo->producers) {
        for (size_t i = 0; i < cfg->nproducers; i++) {
            struct log_producer *producer = &lo->producers[i];
            struct log_chunk    *chunk;

            SPIN_LOCK(&producer->lock);
            chunk           = producer->chunk;
            producer->chunk = NULL;
            SPIN_UNLOCK(&producer->lock);

            if (chunk)
                mempool_free(cfg->mempool, chunk);
        }
    }

    log_os_mmap_deinit(log, lo->file_offset);

    if (lo->flush_buf) {
        mempool_free(cfg->mempool, lo->flush_buf);
        lo->flush_buf = NULL;
    }
    if (lo->producers) {
        mempool_free(cfg->mempool, lo->producers);
        lo->producers = NULL;
    }
}
void
log_write_bin(struct log  *log,
              const size_t id,
              const void  *header,
              const size_t header_size,
              const void  *payload,
              const size_t payload_size)
{
    DECL(log, cfg, lo);

    if (id >= cfg->nproducers || !lo->producers || cfg->chunk_size == 0)
        return;

    const struct log_header log_header = {
        .ts   = cfg->f_get_ts(),
        .id   = id,
        .size = header_size + payload_size,
    };

    const size_t total_size = sizeof(log_header) + log_header.size;
    if (total_size > cfg->chunk_size)
        return;

    struct log_producer *producer = &lo->producers[id];
    SPIN_LOCK(&producer->lock);

    struct log_chunk *chunk = producer->chunk;
    if (chunk == NULL || chunk->offset + total_size > cfg->chunk_size) {
        if (chunk != NULL) {
            producer->chunk = NULL;
            mpsc_push(&lo->mpsc, &chunk->node);
        }

        chunk = (struct log_chunk *)mempool_alloc(cfg->mempool,
                                                  sizeof(struct log_chunk) + cfg->chunk_size);
        if (!chunk) {
            SPIN_UNLOCK(&producer->lock);
            return;
        }
        chunk->offset   = 0;
        producer->chunk = chunk;
    }

    uint8_t *ptr = log_chunk_data(chunk) + chunk->offset;

    memcpy(ptr, &log_header, sizeof(log_header));
    ptr += sizeof(log_header);

    memcpy(ptr, header, header_size);
    ptr += header_size;

    memcpy(ptr, payload, payload_size);
    chunk->offset += total_size;

    SPIN_UNLOCK(&producer->lock);
}

void
log_write(struct log *log, const size_t idx, const char *fmt, va_list args)
{
    DECL(log, cfg, lo);

    if (idx >= cfg->nproducers || !lo->producers || cfg->chunk_size == 0)
        return;

    va_list args_copy;
    va_copy(args_copy, args);
    const int sz = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    const size_t raw_payload_size = (size_t)(sz < 0 ? 0 : sz) + 1;

    // 保持每个 header 按 8 字节对齐, 以适配使用 LDRD 的目标平台.
    const size_t aligned_payload_size = (raw_payload_size + 7) & ~((size_t)7);

    const struct log_header header = {
        .ts   = cfg->f_get_ts(),
        .id   = idx,
        .size = raw_payload_size,
    };

    const size_t total_size = sizeof(header) + aligned_payload_size;
    if (total_size > cfg->chunk_size)
        return;

    struct log_producer *producer = &lo->producers[idx];
    SPIN_LOCK(&producer->lock);

    struct log_chunk *chunk = producer->chunk;
    if (chunk == NULL || chunk->offset + total_size > cfg->chunk_size) {
        if (chunk != NULL) {
            producer->chunk = NULL;
            mpsc_push(&lo->mpsc, &chunk->node);
        }

        chunk = (struct log_chunk *)mempool_alloc(cfg->mempool,
                                                  sizeof(struct log_chunk) + cfg->chunk_size);
        if (!chunk) {
            SPIN_UNLOCK(&producer->lock);
            return;
        }
        chunk->offset   = 0;
        producer->chunk = chunk;
    }

    uint8_t *ptr = log_chunk_data(chunk) + chunk->offset;

    memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);

    vsnprintf((char *)ptr, raw_payload_size, fmt, args);

    chunk->offset += total_size;

    SPIN_UNLOCK(&producer->lock);
}

void
log_flush(struct log *log)
{
    DECL(log, cfg, lo);

    // 同一时刻只允许一个线程消费队列.
    if (lo->busy)
        return;

    lo->busy = true;

    // 消费前将生产者未写满的 chunk 转移到 MPSC 队列.
    log_submit_pending(log);

    while (true) {
        struct mpsc_node *node = mpsc_pop(&lo->mpsc);
        if (node == NULL)
            break;

        struct log_chunk *chunk = (struct log_chunk *)node;

        if (cfg->e_format == LOG_FORMAT_TXT || cfg->e_format == LOG_FORMAT_CSV) {
            size_t parse_offset = 0;
            while (parse_offset < chunk->offset) {
                const struct log_header *header =
                    (struct log_header *)(log_chunk_data(chunk) + parse_offset);
                parse_offset += sizeof(struct log_header);

                const char *payload = (char *)(log_chunk_data(chunk) + parse_offset);

                // 跳过 log_write() 添加的对齐填充.
                const size_t aligned_payload_size  = (header->size + 7) & ~((size_t)7);
                parse_offset                      += aligned_payload_size;

#if OS(NONE)
                const size_t prefix_size = cfg->e_format == LOG_FORMAT_CSV
                                               ? snprintf((char *)lo->flush_buf,
                                                          cfg->flush_cap,
                                                          "%llu,%u,",
                                                          header->ts,
                                                          (uint32_t)header->id)
                                               : snprintf((char *)lo->flush_buf,
                                                          cfg->flush_cap,
                                                          "[%llu][%u] ",
                                                          header->ts,
                                                          (uint32_t)header->id);
#else
                const size_t prefix_size = cfg->e_format == LOG_FORMAT_CSV
                                               ? snprintf((char *)lo->flush_buf,
                                                          cfg->flush_cap,
                                                          "%llu,%llu,",
                                                          header->ts,
                                                          (uint64_t)header->id)
                                               : snprintf((char *)lo->flush_buf,
                                                          cfg->flush_cap,
                                                          "[%llu][%llu] ",
                                                          header->ts,
                                                          (uint64_t)header->id);
#endif

                const size_t payload_len = header->size - 1;
                size_t       copy_len    = payload_len;
                if (prefix_size + copy_len > cfg->flush_cap)
                    copy_len = cfg->flush_cap - prefix_size;

                memcpy(lo->flush_buf + prefix_size, payload, copy_len);
                log_flush_ring(log, lo->flush_buf, prefix_size + copy_len);
            }
        } else {
            log_flush_ring(log, log_chunk_data(chunk), chunk->offset);
        }

        // log_flush_ring() 为同步操作; 异步后端必须延后释放.
        mempool_free(cfg->mempool, chunk);
    }

    lo->busy = false;
}

void
log_data(struct log *log, const size_t idx, const char *fmt, ...)
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
log_debug(struct log *log, const size_t idx, const char *fmt, ...)
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
log_info(struct log *log, const size_t idx, const char *fmt, ...)
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
log_warn(struct log *log, const size_t idx, const char *fmt, ...)
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
log_error(struct log *log, const size_t idx, const char *fmt, ...)
{
    DECL(log, cfg);
    if (cfg->e_level > LOG_LEVEL_ERR)
        return;

    va_list args;
    va_start(args, fmt);
    log_write(log, idx, fmt, args);
    va_end(args);
}
