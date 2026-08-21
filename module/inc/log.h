#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdint.h>

#include "macrodef.h"

#include "mempool.h"
#include "mpsc.h"
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum log_format {
    LOG_FORMAT_TXT,
    LOG_FORMAT_BIN,
    LOG_FORMAT_CSV,
};

enum log_level {
    LOG_LEVEL_DATA,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERR,
};

enum log_mode {
    LOG_MODE_SYNC,
    LOG_MODE_ASYNC,
};

enum log_ring {
    LOG_RING_TRUNCATE,
    LOG_RING_ROTATE,
};

struct log_header {
    uint64_t ts;
    size_t   id;
    size_t   size;
};

typedef uint64_t (*log_get_ts_f)(void);
typedef void (*log_flush_f)(void *fp, const void *src, size_t size);

struct log_cfg {
    enum log_mode   e_mode;
    enum log_level  e_level;
    enum log_format e_format;
    struct mempool *mempool;
    const char     *file_path;
    const char     *file_header;
    void           *fd;
    size_t          file_size;
    size_t          max_files;
    enum log_ring   e_ring;
    size_t          chunk_size; // 每个线程块的最大尺寸
    size_t          flush_cap;
    size_t          nproducers; // 记录最大线程数
    log_get_ts_f    f_get_ts;
    log_flush_f     f_flush;
};

struct log_chunk {
    struct mpsc_node node;
    size_t           offset;
};

struct log_producer {
    ATOMIC(uint8_t) lock;
    struct log_chunk *chunk;
};

struct log_lo {
    struct mpsc mpsc;
    uint8_t     busy;
    uint8_t    *flush_buf;
    size_t      file_offset;

    void *mmap_ptr;
    void *os_file_handle;
    void *os_map_handle;
    char  curr_file_path[256];

    // 使用 ID 绑定的私有块,每个 producer 独占一个槽位
    struct log_producer *producers;
};

struct log {
    struct log_cfg cfg;
    struct log_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void log_init(struct log *log, struct log_cfg log_cfg);
void log_deinit(struct log *log);
void log_write_bin(struct log *log,
                   size_t      id,
                   const void *header,
                   size_t      header_size,
                   const void *payload,
                   size_t      payload_size);
void log_write(struct log *log, size_t idx, const char *fmt, va_list args);
void log_flush(struct log *log);

void log_data(struct log *log, size_t idx, const char *fmt, ...);
void log_debug(struct log *log, size_t idx, const char *fmt, ...);
void log_info(struct log *log, size_t idx, const char *fmt, ...);
void log_warn(struct log *log, size_t idx, const char *fmt, ...);
void log_error(struct log *log, size_t idx, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // !LOG_H
