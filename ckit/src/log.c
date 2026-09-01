#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "macrodef.h"
#include "mathdef.h"
#include "mempool.h"
#include "timeops.h"

/**
 * @brief               获取日志块的数据地址
 * @param[in,out] chunk 日志块
 */
static uint8_t *log_chunk_data(struct log_chunk *chunk);

/**
 * @brief             关闭日志文件流
 * @param[in,out] log 日志对象
 */
static int log_stream_close(struct log *log);

/**
 * @brief               按刷新间隔同步文件流
 * @param[in,out] log   日志对象
 * @param[in]     force 是否强制刷新
 */
static int log_stream_maybe_flush(struct log *log, bool force);

/**
 * @brief                输出日志缓冲数据
 * @param[in,out] log    日志对象
 * @param[in]     source 输入数据
 * @param[in]     size   数据大小, 单位: byte
 */
static int log_flush_ring(struct log *log, const void *source, size_t size);

/**
 * @brief             提交生产者未满的日志块
 * @param[in,out] log 日志对象
 */
static void log_submit_pending(struct log *log);

/**
 * @brief                      按需刷新待输出日志
 * @param[in,out] log          日志对象
 * @param[in]     chunk_queued 本次是否提交了日志块
 */
static int log_auto_flush_file(struct log *log, bool chunk_queued);

/**
 * @brief                          排空日志队列并刷新文件
 * @param[in,out] log              日志对象
 * @param[in]     force_file_flush 是否强制刷新文件
 */
static int log_flush_internal(struct log *log, bool force_file_flush);

#if OS(WIN)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#elif OS(POSIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if OS(HOSTED)

/**
 * @brief          创建日志目录
 * @param[in] path 文件路径
 */
static int log_make_directory(const char *path);

static int log_make_directory(const char *path)
{
#if OS(WIN)
    return _mkdir(path);
#elif OS(POSIX)
    return mkdir(path, 0755);
#else
    ARG_UNUSED(path);
    return -1;
#endif
}

/**
 * @brief          确保日志目录存在
 * @param[in] path 文件路径
 */
static int log_ensure_directory(const char *path);

static int log_ensure_directory(const char *path)
{
#if OS(NONE)
    ARG_UNUSED(path);
    return MERR(MENOTSUP);
#else
    char buf[512];
    size_t len;

    if (path == NULL || *path == '\0') {
        return MERR(MEINVAL);
    }
    len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return MERR(MEOVERFLOW);
    }
    memcpy(buf, path, len + 1U);

    while (len > 1U && (buf[len - 1U] == '/' || buf[len - 1U] == '\\')) {
        buf[--len] = '\0';
    }
    for (size_t i = 1U; i < len; i++) {
        if (buf[i] != '/' && buf[i] != '\\') {
            continue;
        }
        if (i == 2U && buf[1] == ':') {
            continue;
        }

        const char separator = buf[i];
        buf[i] = '\0';
        if (log_make_directory(buf) != 0 && errno != EEXIST) {
            return MERR(MECREATE);
        }
        buf[i] = separator;
    }
    if (log_make_directory(buf) != 0 && errno != EEXIST) {
        return MERR(MECREATE);
    }
    return MOK;
#endif
}

/**
 * @brief               清理超出数量限制的日志文件
 * @param[in] directory 日志目录路径
 * @param[in] max_files 允许保留的文件数量
 * @param[in] extension 日志文件扩展名
 */
static void log_purge_old_files(const char *directory,
                                size_t max_files,
                                const char *extension);

static void log_purge_old_files(const char *directory,
                                size_t max_files,
                                const char *extension)
{
    if (max_files == 0) {
        return;
    }

#if !OS(WIN) && !OS(POSIX)
    ARG_UNUSED(directory);
    ARG_UNUSED(extension);
#endif

#if OS(WIN)
    char search_path[512];
    snprintf(search_path, sizeof(search_path), "%s/*.%s", directory, extension);
    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    // 收集所有匹配的文件名
    struct file_node {
        char name[256];
    };
    struct file_node *files = NULL;
    size_t cnt = 0;

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            struct file_node *new_files = (struct file_node *)realloc(
                files, sizeof(struct file_node) * (cnt + 1));
            if (new_files == NULL) {
                break;
            }
            files = new_files;
            size_t name_len = 0U;
            while (name_len + 1U < sizeof(files[cnt].name) &&
                   find_data.cFileName[name_len] != '\0') {
                name_len++;
            }
            memcpy(files[cnt].name, find_data.cFileName, name_len);
            files[cnt].name[name_len] = '\0';
            cnt++;
        }
    } while (FindNextFileA(hFind, &find_data));
    FindClose(hFind);

    if (cnt > max_files) {
        // 按名称排序 (时间戳格式保证了名称序即时间序)
        for (size_t i = 0; i < cnt - 1; i++) {
            for (size_t j = i + 1; j < cnt; j++) {
                if (strcmp(files[i].name, files[j].name) > 0) {
                    struct file_node tmp = files[i];
                    files[i] = files[j];
                    files[j] = tmp;
                }
            }
        }
        // 删除多余的最旧文件
        for (size_t i = 0; i < cnt - max_files; i++) {
            char full_path[512];
            snprintf(full_path,
                     sizeof(full_path),
                     "%s/%s",
                     directory,
                     files[i].name);
            DeleteFileA(full_path);
        }
    }
    free(files);
#elif OS(POSIX)
    DIR *dp = opendir(directory);
    if (dp == NULL) {
        return;
    }

    struct file_node {
        char name[256];
    };
    struct file_node *files = NULL;
    size_t cnt = 0;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        const size_t n = strlen(name);
        const size_t extension_len = strlen(extension);
        if (n <= extension_len || name[n - extension_len - 1U] != '.' ||
            strcmp(name + n - extension_len, extension) != 0) {
            continue;
        }

        struct file_node *new_files = (struct file_node *)realloc(
            files, sizeof(struct file_node) * (cnt + 1));
        if (new_files == NULL) {
            break;
        }
        files = new_files;
        strncpy(files[cnt].name, name, sizeof(files[cnt].name) - 1);
        files[cnt].name[sizeof(files[cnt].name) - 1] = '\0';
        cnt++;
    }
    closedir(dp);

    if (cnt > max_files) {
        for (size_t i = 0; i < cnt - 1; i++) {
            for (size_t j = i + 1; j < cnt; j++) {
                if (strcmp(files[i].name, files[j].name) > 0) {
                    struct file_node tmp = files[i];
                    files[i] = files[j];
                    files[j] = tmp;
                }
            }
        }
        for (size_t i = 0; i < cnt - max_files; i++) {
            char full_path[512];
            snprintf(full_path,
                     sizeof(full_path),
                     "%s/%s",
                     directory,
                     files[i].name);
            unlink(full_path);
        }
    }
    free(files);
#endif
}

/**
 * @brief              生成时间戳字符串
 * @param[in,out] buf  存储缓冲区
 * @param[in]     size 数据大小, 单位: byte
 */
static void get_ts_str(char *buf, size_t size);

static void get_ts_str(char *buf, size_t size)
{
    const uint64_t ts_us = get_real_ts_us();
    const time_t seconds = (time_t)(ts_us / 1000000U);
    struct tm tm = {0};

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
    const size_t len = strftime(buf, size, "%Y%m%d_%H%M%S", &tm);
    if (len > 0U && len < size) {
        snprintf(
            buf + len, size - len, "_%06u", (unsigned int)(ts_us % 1000000U));
    }
}

#endif

#if OS(HOSTED)

/**
 * @brief          创建不覆盖已有文件的日志文件
 * @param[in] path 文件路径
 */
static FILE *log_open_exclusive(const char *path);

static FILE *log_open_exclusive(const char *path)
{
#if OS(WIN)
    const int fd = _open(
        path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return NULL;
    }

    FILE *file = _fdopen(fd, "wb");
    if (file == NULL) {
        const int saved_errno = errno;
        _close(fd);
        _unlink(path);
        errno = saved_errno;
    }
    return file;
#elif OS(POSIX)
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return NULL;
    }

    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        const int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
    }
    return file;
#else
    ARG_UNUSED(path);
    return NULL;
#endif
}

/**
 * @brief          打开并清空日志文件
 * @param[in] path 文件路径
 */
static FILE *log_open_truncate(const char *path);

static FILE *log_open_truncate(const char *path)
{
#if COMPILER(MSVC)
    FILE *file = NULL;
    return fopen_s(&file, path, "wb") == 0 ? file : NULL;
#else
    return fopen(path, "wb");
#endif
}

/**
 * @brief              同步日志文件到存储
 * @param[in,out] file 文件流
 */
static bool log_sync_file(FILE *file);

static bool log_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return false;
    }
#if OS(WIN)
    return _commit(_fileno(file)) == 0;
#elif OS(POSIX)
    return fsync(fileno(file)) == 0;
#else
    return true;
#endif
}

/**
 * @brief                          打开日志文件流
 * @param[in,out] log              日志对象
 * @param[in]     first_write_size 首次写入长度, 单位: byte
 */
static int log_stream_open(struct log *log, size_t first_write_size);

static int log_stream_open(struct log *log, size_t first_write_size)
{
    DECL(log, cfg, lo);

#if OS(NONE)
    ARG_UNUSED(first_write_size);
    return MERR(MENOTSUP);
#else
    const char *extension = cfg->fmt == LOG_FMT_CSV ? "csv" : "log";
    const char *header = cfg->fmt == LOG_FMT_CSV ? cfg->csv_header : NULL;
    const size_t header_size = header != NULL ? strlen(header) : 0U;

    if (header_size > cfg->file_size ||
        first_write_size > cfg->file_size - header_size) {
        return MERR(MEOVERFLOW);
    }

    if (cfg->ring == LOG_RING_ROTATE) {
        const int directory_result = log_ensure_directory(cfg->file_path);
        if (directory_result < 0) {
            return directory_result;
        }
        char ts[32];
        get_ts_str(ts, sizeof(ts));
        for (unsigned int attempt = 0U; attempt < 1000U; attempt++) {
            char suffix[20] = "";
            if (attempt != 0U) {
                snprintf(suffix, sizeof(suffix), "_%u", attempt);
            }
            const int curr_len = snprintf(lo->file.curr_path,
                                          sizeof(lo->file.curr_path),
                                          "%s/.%s%s.%s.part",
                                          cfg->file_path,
                                          ts,
                                          suffix,
                                          extension);
            const int final_len = snprintf(lo->file.final_path,
                                           sizeof(lo->file.final_path),
                                           "%s/%s%s.%s",
                                           cfg->file_path,
                                           ts,
                                           suffix,
                                           extension);
            if (curr_len < 0 ||
                (size_t)curr_len >= sizeof(lo->file.curr_path) ||
                final_len < 0 ||
                (size_t)final_len >= sizeof(lo->file.final_path)) {
                return MERR(MEOVERFLOW);
            }
            lo->file.stream = log_open_exclusive(lo->file.curr_path);
            if (lo->file.stream != NULL) {
                break;
            }
            if (errno != EEXIST) {
                return MERR(MEACCES);
            }
        }
    } else {
        const int path_len = snprintf(lo->file.curr_path,
                                      sizeof(lo->file.curr_path),
                                      "%s",
                                      cfg->file_path);
        if (path_len < 0 || (size_t)path_len >= sizeof(lo->file.curr_path)) {
            return MERR(MEOVERFLOW);
        }
        lo->file.stream = log_open_truncate(lo->file.curr_path);
    }
    if (lo->file.stream == NULL) {
        return MERR(MECREATE);
    }

    lo->file.buf = malloc(SIZE_64KB);
    if (lo->file.buf == NULL ||
        setvbuf(
            (FILE *)lo->file.stream, (char *)lo->file.buf, _IOFBF, SIZE_64KB) !=
            0 ||
        (header_size != 0U &&
         fwrite(header, 1U, header_size, (FILE *)lo->file.stream) !=
             header_size)) {
        const int saved_errno = errno != 0 ? errno : ENOMEM;
        fclose((FILE *)lo->file.stream);
        lo->file.stream = NULL;
        free(lo->file.buf);
        lo->file.buf = NULL;
        if (cfg->ring == LOG_RING_ROTATE) {
            remove(lo->file.curr_path);
        }
        errno = saved_errno;
        return MERR(MEIO);
    }

    lo->file.offset = header_size;
    lo->check.file_failed = false;
    lo->check.file_err = 0;
    return MOK;
#endif
}

#endif

int log_init(struct log *log, const struct log_cfg log_cfg)
{
    if (!log || !log_cfg.mempool || !log_cfg.get_ts) {
        return MERR(MENULLPTR);
    }
    if (log_cfg.mode < LOG_MODE_SYNC || log_cfg.mode > LOG_MODE_ASYNC ||
        log_cfg.level < LOG_LEVEL_DATA || log_cfg.level > LOG_LEVEL_ERR ||
        log_cfg.fmt < LOG_FMT_TXT || log_cfg.fmt > LOG_FMT_BIN ||
        log_cfg.ring < LOG_RING_TRUNCATE || log_cfg.ring > LOG_RING_ROTATE ||
        !log_cfg.chunk_size || !log_cfg.flush_cap || !log_cfg.producer_cnt) {
        return MERR(MEINVAL);
    }

    if (log_cfg.producer_cnt > SIZE_MAX / sizeof(struct log_producer) ||
        log_cfg.chunk_size > SIZE_MAX - sizeof(struct log_chunk)) {
        return MERR(MEOVERFLOW);
    }

    DECL(log, cfg, lo);

    memset(lo, 0, sizeof(*lo));
    CFG_INIT(log, log_cfg);

    lo->flush_buf = (uint8_t *)mempool_alloc(cfg->mempool, cfg->flush_cap);
    lo->file.offset = 0;
    if (!lo->flush_buf) {
        return MERR(MEALLOC);
    }

    lo->producers = (struct log_producer *)mempool_calloc(
        cfg->mempool, sizeof(struct log_producer) * cfg->producer_cnt);
    if (lo->producers) {
        for (size_t i = 0; i < cfg->producer_cnt; i++) {
            ATOMIC_STORE_EXPLICIT(
                &lo->producers[i].lock, false, ATOMIC_RELAXED);
        }
    } else {
        mempool_free(cfg->mempool, lo->flush_buf);
        lo->flush_buf = NULL;
        return MERR(MEALLOC);
    }

    mpsc_init(&lo->mpsc);
    lo->file.prev_flush_ts = cfg->get_ts();
    return MOK;
}

int log_deinit(struct log *log)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg, lo);

    log_submit_pending(log);
    int flush_ret = log_flush_internal(log, true);
    if (flush_ret == MERR(MEBUSY)) {
        return flush_ret;
    }

    if (lo->producers) {
        for (size_t i = 0; i < cfg->producer_cnt; i++) {
            struct log_producer *producer = &lo->producers[i];
            struct log_chunk *chunk;

            SPIN_LOCK(&producer->lock);
            chunk = producer->chunk;
            producer->chunk = NULL;
            SPIN_UNLOCK(&producer->lock);

            if (chunk) {
                mempool_free(cfg->mempool, chunk);
            }
        }
    }

    const int close_ret = log_stream_close(log);
    if (flush_ret == MOK && close_ret < 0) {
        flush_ret = close_ret;
    }

    if (lo->flush_buf) {
        mempool_free(cfg->mempool, lo->flush_buf);
        lo->flush_buf = NULL;
    }
    if (lo->producers) {
        mempool_free(cfg->mempool, lo->producers);
        lo->producers = NULL;
    }
    return flush_ret;
}

int log_write_bin(struct log *log,
                  const size_t id,
                  const void *header,
                  const size_t header_size,
                  const void *payload,
                  const size_t payload_size)
{
    if (!log || (header_size && !header) || (payload_size && !payload)) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg, lo);

    if (cfg->fmt != LOG_FMT_BIN) {
        return MERR(MENOTSUP);
    }

    if (!lo->producers || !cfg->get_ts || cfg->chunk_size == 0) {
        return MERR(MESTATE);
    }
    if (id >= cfg->producer_cnt) {
        return MERR(MEINVAL);
    }

    if (header_size > SIZE_MAX - payload_size ||
        header_size + payload_size > SIZE_MAX - sizeof(struct log_header)) {
        return MERR(MEOVERFLOW);
    }
    const struct log_header log_header = {
        .ts = cfg->get_ts(),
        .id = id,
        .size = header_size + payload_size,
    };

    const size_t total_size = sizeof(log_header) + log_header.size;
    if (total_size > cfg->chunk_size) {
        return MERR(MEOVERFLOW);
    }

    struct log_producer *producer = &lo->producers[id];
    bool chunk_queued = false;
    SPIN_LOCK(&producer->lock);

    struct log_chunk *chunk = producer->chunk;
    if (chunk == NULL || chunk->offset > cfg->chunk_size ||
        total_size > cfg->chunk_size - chunk->offset) {
        if (chunk != NULL) {
            producer->chunk = NULL;
            mpsc_push(&lo->mpsc, &chunk->node);
            chunk_queued = true;
        }

        chunk = (struct log_chunk *)mempool_alloc(
            cfg->mempool, sizeof(struct log_chunk) + cfg->chunk_size);
        if (!chunk) {
            SPIN_UNLOCK(&producer->lock);
            return MERR(MEALLOC);
        }
        chunk->offset = 0;
        producer->chunk = chunk;
    }

    uint8_t *ptr = log_chunk_data(chunk) + chunk->offset;

    memcpy(ptr, &log_header, sizeof(log_header));
    ptr += sizeof(log_header);

    if (header_size != 0U) {
        memcpy(ptr, header, header_size);
        ptr += header_size;
    }
    if (payload_size != 0U) {
        memcpy(ptr, payload, payload_size);
    }
    chunk->offset += total_size;

    SPIN_UNLOCK(&producer->lock);
    return log_auto_flush_file(log, chunk_queued);
}

int log_write(struct log *log, const size_t idx, const char *fmt, va_list args)
{
    if (!log || !fmt) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg, lo);

    if (!lo->producers || !cfg->get_ts || cfg->chunk_size == 0) {
        return MERR(MESTATE);
    }
    if (idx >= cfg->producer_cnt) {
        return MERR(MEINVAL);
    }

    va_list args_copy;
    va_copy(args_copy, args);
    const int formatted_size = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (formatted_size < 0) {
        return MERR(MEIO);
    }

    const size_t msg_size = (size_t)formatted_size + 1U;
    const size_t raw_payload_size = msg_size;

    // 记录步长保持 8 的倍数, 满足当前 ABI 下 log_header 的类型对齐要求
    const size_t aligned_payload_size = (raw_payload_size + 7) & ~((size_t)7);

    const struct log_header header = {
        .ts = cfg->get_ts(),
        .id = idx,
        .size = raw_payload_size,
    };

    const size_t total_size = sizeof(header) + aligned_payload_size;
    if (total_size > cfg->chunk_size) {
        return MERR(MEOVERFLOW);
    }

    struct log_producer *producer = &lo->producers[idx];
    bool chunk_queued = false;
    SPIN_LOCK(&producer->lock);

    struct log_chunk *chunk = producer->chunk;
    if (chunk == NULL || chunk->offset > cfg->chunk_size ||
        total_size > cfg->chunk_size - chunk->offset) {
        if (chunk != NULL) {
            producer->chunk = NULL;
            mpsc_push(&lo->mpsc, &chunk->node);
            chunk_queued = true;
        }

        chunk = (struct log_chunk *)mempool_alloc(
            cfg->mempool, sizeof(struct log_chunk) + cfg->chunk_size);
        if (!chunk) {
            SPIN_UNLOCK(&producer->lock);
            return MERR(MEALLOC);
        }
        chunk->offset = 0;
        producer->chunk = chunk;
    }

    uint8_t *ptr = log_chunk_data(chunk) + chunk->offset;

    memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);

    vsnprintf((char *)ptr, msg_size, fmt, args);

    chunk->offset += total_size;

    SPIN_UNLOCK(&producer->lock);
    return log_auto_flush_file(log, chunk_queued);
}

int log_flush(struct log *log)
{
    return log_flush_internal(log, true);
}

int log_data(struct log *log, const size_t idx, const char *fmt, ...)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg);

    if (cfg->level > LOG_LEVEL_DATA) {
        return MOK;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = log_write(log, idx, fmt, args);
    va_end(args);
    return ret;
}

int log_debug(struct log *log, const size_t idx, const char *fmt, ...)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg);

    if (cfg->level > LOG_LEVEL_DEBUG) {
        return MOK;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = log_write(log, idx, fmt, args);
    va_end(args);
    return ret;
}

int log_info(struct log *log, const size_t idx, const char *fmt, ...)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg);

    if (cfg->level > LOG_LEVEL_INFO) {
        return MOK;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = log_write(log, idx, fmt, args);
    va_end(args);
    return ret;
}

int log_warn(struct log *log, const size_t idx, const char *fmt, ...)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg);

    if (cfg->level > LOG_LEVEL_WARN) {
        return MOK;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = log_write(log, idx, fmt, args);
    va_end(args);
    return ret;
}

int log_err(struct log *log, const size_t idx, const char *fmt, ...)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg);

    if (cfg->level > LOG_LEVEL_ERR) {
        return MOK;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = log_write(log, idx, fmt, args);
    va_end(args);
    return ret;
}

static uint8_t *log_chunk_data(struct log_chunk *chunk)
{
    return (uint8_t *)(chunk + 1);
}

static int log_stream_close(struct log *log)
{
#if OS(NONE)
    ARG_UNUSED(log);
    return MOK;
#else
    DECL(log, cfg, lo);

    if (lo->file.stream == NULL) {
        return MOK;
    }

    int saved_errno = lo->check.file_err;
    if (!log_sync_file((FILE *)lo->file.stream)) {
        saved_errno = errno != 0 ? errno : EIO;
        lo->check.file_failed = true;
    }
    if (fclose((FILE *)lo->file.stream) != 0 && saved_errno == 0) {
        saved_errno = errno != 0 ? errno : EIO;
        lo->check.file_failed = true;
    }
    lo->file.stream = NULL;
    free(lo->file.buf);
    lo->file.buf = NULL;

    if (cfg->ring == LOG_RING_ROTATE && !lo->check.file_failed &&
        rename(lo->file.curr_path, lo->file.final_path) != 0) {
        saved_errno = errno != 0 ? errno : EIO;
        lo->check.file_failed = true;
    }
    if (lo->check.file_failed) {
        errno = saved_errno;
        return MERR(MEIO);
    }
    if (cfg->ring == LOG_RING_ROTATE) {
        const char *extension = cfg->fmt == LOG_FMT_CSV ? "csv" : "log";
        log_purge_old_files(cfg->file_path, cfg->max_files, extension);
    }
    lo->file.curr_path[0] = '\0';
    lo->file.final_path[0] = '\0';
    lo->file.offset = 0U;
    return MOK;
#endif
}

static int log_stream_maybe_flush(struct log *log, bool force)
{
#if OS(NONE)
    ARG_UNUSED(log);
    ARG_UNUSED(force);
    return MOK;
#else
    DECL(log, cfg, lo);

    if (lo->file.stream == NULL) {
        return MOK;
    }

    const uint64_t now = cfg->get_ts();
    if (!force && cfg->flush_interval_us != 0U &&
        now - lo->file.prev_flush_ts < cfg->flush_interval_us) {
        return MOK;
    }
    if (fflush((FILE *)lo->file.stream) != 0) {
        lo->check.file_failed = true;
        lo->check.file_err = errno != 0 ? errno : EIO;
        return MERR(MEIO);
    }
    lo->file.prev_flush_ts = now;
    return MOK;
#endif
}

static int log_flush_ring(struct log *log, const void *source, size_t size)
{
    DECL(log, cfg, lo);

    if (cfg->file_path == NULL || cfg->file_size == 0U) {
        return cfg->flush != NULL ? cfg->flush(cfg->fd, source, size)
                                  : MERR(MESTATE);
    }
#if OS(NONE)
    ARG_UNUSED(lo);
    return MERR(MENOTSUP);
#else
    if (lo->file.stream == NULL) {
        const int open_result = log_stream_open(log, size);
        if (open_result < 0) {
            return open_result;
        }
    } else if (lo->file.offset > cfg->file_size ||
               size > cfg->file_size - lo->file.offset) {
        const int close_result = log_stream_close(log);
        if (close_result < 0) {
            return close_result;
        }
        const int open_result = log_stream_open(log, size);
        if (open_result < 0) {
            return open_result;
        }
    }

    if (lo->file.offset > cfg->file_size ||
        size > cfg->file_size - lo->file.offset) {
        return MERR(MEOVERFLOW);
    }
    if (fwrite(source, 1U, size, (FILE *)lo->file.stream) != size) {
        lo->check.file_failed = true;
        lo->check.file_err = errno != 0 ? errno : EIO;
        return MERR(MEIO);
    }
    lo->file.offset += size;
    return MOK;
#endif
}

static void log_submit_pending(struct log *log)
{
    DECL(log, cfg, lo);

    if (!lo->producers) {
        return;
    }

    for (size_t i = 0; i < cfg->producer_cnt; i++) {
        struct log_producer *producer = &lo->producers[i];
        struct log_chunk *chunk = NULL;

        SPIN_LOCK(&producer->lock);
        if (producer->chunk && producer->chunk->offset > 0) {
            chunk = producer->chunk;
            producer->chunk = NULL;
        }
        SPIN_UNLOCK(&producer->lock);

        if (chunk) {
            mpsc_push(&lo->mpsc, &chunk->node);
        }
    }
}

static int log_auto_flush_file(struct log *log, bool chunk_queued)
{
    DECL(log, cfg, lo);

    if (cfg->mode != LOG_MODE_SYNC || cfg->file_path == NULL ||
        cfg->file_size == 0U) {
        return MOK;
    }

    if (ATOMIC_EXCHANGE_EXPLICIT(&lo->busy, true, ATOMIC_ACQUIRE)) {
        return MERR(MEBUSY);
    }
    const uint64_t now = cfg->get_ts();
    const bool interval_elapsed =
        cfg->flush_interval_us == 0U ||
        now - lo->file.prev_flush_ts >= cfg->flush_interval_us;
    ATOMIC_STORE_EXPLICIT(&lo->busy, false, ATOMIC_RELEASE);
    return chunk_queued || interval_elapsed ? log_flush_internal(log, false)
                                            : MOK;
}

static int log_flush_internal(struct log *log, bool force_file_flush)
{
    if (!log) {
        return MERR(MENULLPTR);
    }
    DECL(log, cfg, lo);

    if (!lo->flush_buf || !lo->producers) {
        return MERR(MESTATE);
    }

    // 同一时刻只允许一个线程消费队列
    if (ATOMIC_EXCHANGE_EXPLICIT(&lo->busy, true, ATOMIC_ACQUIRE)) {
        return MERR(MEBUSY);
    }

    int flush_ret = MOK;

    // 消费前将生产者未写满的 chunk 转移到 MPSC 队列
    log_submit_pending(log);

    while (true) {
        struct mpsc_node *node = mpsc_pop(&lo->mpsc);
        if (node == NULL) {
            break;
        }

        struct log_chunk *chunk = (struct log_chunk *)node;
        if (chunk->offset > cfg->chunk_size) {
            if (flush_ret == MOK) {
                flush_ret = MERR(MEVERIFY);
            }
            mempool_free(cfg->mempool, chunk);
            continue;
        }

        if (cfg->fmt == LOG_FMT_TXT || cfg->fmt == LOG_FMT_CSV) {
            size_t parse_offset = 0;
            while (parse_offset < chunk->offset) {
                if (chunk->offset - parse_offset < sizeof(struct log_header)) {
                    if (flush_ret == MOK) {
                        flush_ret = MERR(MEVERIFY);
                    }
                    break;
                }
                struct log_header header_copy;
                memcpy(&header_copy,
                       log_chunk_data(chunk) + parse_offset,
                       sizeof(header_copy));
                const struct log_header *header = &header_copy;
                parse_offset += sizeof(struct log_header);
                if (header->size == 0U || header->size > SIZE_MAX - 7U) {
                    if (flush_ret == MOK) {
                        flush_ret = MERR(MEVERIFY);
                    }
                    break;
                }
                const size_t aligned_payload_size =
                    ALIGN_UP(header->size, (size_t)8U);
                if (aligned_payload_size > chunk->offset - parse_offset) {
                    if (flush_ret == MOK) {
                        flush_ret = MERR(MEVERIFY);
                    }
                    break;
                }
                const char *payload =
                    (char *)(log_chunk_data(chunk) + parse_offset);
                parse_offset += aligned_payload_size;

#if OS(NONE)
                const size_t prefix_size =
                    cfg->fmt == LOG_FMT_CSV
                        ? snprintf((char *)lo->flush_buf,
                                   cfg->flush_cap,
                                   "%llu,%u,",
                                   (unsigned long long)header->ts,
                                   (uint32_t)header->id)
                        : snprintf((char *)lo->flush_buf,
                                   cfg->flush_cap,
                                   "[%llu][%u] ",
                                   (unsigned long long)header->ts,
                                   (uint32_t)header->id);
#else
                const size_t prefix_size =
                    cfg->fmt == LOG_FMT_CSV
                        ? snprintf((char *)lo->flush_buf,
                                   cfg->flush_cap,
                                   "%llu,%llu,",
                                   (unsigned long long)header->ts,
                                   (unsigned long long)header->id)
                        : snprintf((char *)lo->flush_buf,
                                   cfg->flush_cap,
                                   "[%llu][%llu] ",
                                   (unsigned long long)header->ts,
                                   (unsigned long long)header->id);
#endif

                const size_t payload_len = header->size - 1;
                size_t copy_len = payload_len;
                if (prefix_size >= cfg->flush_cap) {
                    flush_ret = MERR(MEOVERFLOW);
                    copy_len = 0;
                } else if (copy_len > cfg->flush_cap - prefix_size) {
                    flush_ret = MERR(MEOVERFLOW);
                    copy_len = cfg->flush_cap - prefix_size;
                }

                if (copy_len > 0) {
                    memcpy(lo->flush_buf + prefix_size, payload, copy_len);
                    const int ret = log_flush_ring(
                        log, lo->flush_buf, prefix_size + copy_len);
                    if (ret < 0 && flush_ret == MOK) {
                        flush_ret = ret;
                    }
                }
            }
        } else {
            const int ret =
                log_flush_ring(log, log_chunk_data(chunk), chunk->offset);
            if (ret < 0 && flush_ret == MOK) {
                flush_ret = ret;
            }
        }

        // log_flush_ring() 为同步操作; 异步后端必须延后释放
        mempool_free(cfg->mempool, chunk);
    }

    if (flush_ret == MOK && cfg->file_path != NULL && cfg->file_size != 0U) {
        const int file_flush_ret =
            log_stream_maybe_flush(log, force_file_flush);
        if (file_flush_ret < 0) {
            flush_ret = file_flush_ret;
        }
    }
    ATOMIC_STORE_EXPLICIT(&lo->busy, false, ATOMIC_RELEASE);
    return flush_ret;
}
