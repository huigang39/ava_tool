#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdint.h>

#include "mempool.h"
#include "mpsc.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum log_format {
        LOG_FORMAT_TEXT,
        LOG_FORMAT_BIN,
} log_format_e;

typedef enum log_level {
        LOG_LEVEL_DATA,
        LOG_LEVEL_DEBUG,
        LOG_LEVEL_INFO,
        LOG_LEVEL_WARN,
        LOG_LEVEL_ERR,
} log_level_e;

typedef enum log_mode {
        LOG_MODE_SYNC,
        LOG_MODE_ASYNC,
} log_mode_e;

typedef enum log_ring {
        LOG_RING_WRAP,
        LOG_RING_TRUNCATE,
        LOG_RING_COMPLETE,
        LOG_RING_ROTATE,
} log_ring_e;

typedef struct log_header {
        u64   ts;
        usize id;
        usize size;
} log_header_t;

typedef u64 (*log_get_ts_f)(void);
typedef void (*log_flush_f)(void *fp, const void *src, usize size);

typedef struct log_cfg {
        log_mode_e   e_mode;
        log_level_e  e_level;
        log_format_e e_format;
        mempool_t   *mempool;
        const char  *file_path;
        void        *fd;
        usize        file_size;
        usize        max_files;
        log_ring_e   e_ring;
        usize        chunk_size; // 每个线程块的最大尺寸
        usize        flush_cap;
        usize        nproducers; // 记录最大线程数
        log_get_ts_f f_get_ts;
        log_flush_f  f_flush;
} log_cfg_t;

typedef struct log_chunk {
        mpsc_node_t node;
        usize       offset;
        u8          data[];
} log_chunk_t;

typedef struct log_lo {
        mpsc_t mpsc;
        u8     busy;
        u8    *flush_buf;
        usize  file_offset;

        void *mmap_ptr;
        void *os_file_handle;
        void *os_map_handle;
        usize curr_file_idx;
        char  curr_file_path[256];

        // 使用 ID 绑定的私有块数组
        log_chunk_t **chunks;
} log_lo_t;

typedef struct log {
        log_cfg_t cfg;
        log_lo_t  lo;
} log_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void log_init(log_t *log, log_cfg_t log_cfg);
void log_deinit(log_t *log);
void log_write_bin(log_t *log, usize id, const void *header, usize header_size, const void *payload, usize payload_size);
void log_flush_bin(log_t *log);
void log_write(log_t *log, usize idx, const char *fmt, va_list args);
void log_flush(log_t *log);

void log_data(log_t *log, usize idx, const char *fmt, ...);
void log_debug(log_t *log, usize idx, const char *fmt, ...);
void log_info(log_t *log, usize idx, const char *fmt, ...);
void log_warn(log_t *log, usize idx, const char *fmt, ...);
void log_error(log_t *log, usize idx, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // !LOG_H