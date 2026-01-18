#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>

#include "mpsc.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum log_level {
        LOG_LEVEL_DATA,  // 数据
        LOG_LEVEL_DEBUG, // 调试
        LOG_LEVEL_INFO,  // 一般
        LOG_LEVEL_WARN,  // 警告
        LOG_LEVEL_ERR,   // 错误
} log_level_e;

typedef enum log_mode {
        LOG_MODE_SYNC,
        LOG_MODE_ASYNC,
} log_mode_e;

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
        void        *buf;
        usize        cap;
        u8          *flush_buf;
        usize        flush_cap;
        mpsc_p_t    *producers;
        usize        nproducers;
        log_get_ts_f f_get_ts;
        void        *fp;
        log_flush_f  f_flush;
} log_cfg_t;

typedef struct log_lo {
        mpsc_t mpsc;
        bool   busy;
} log_lo_t;

typedef struct log {
        log_cfg_t cfg;
        log_lo_t  lo;
} log_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void log_init(log_t *log, log_cfg_t log_cfg);
void log_write_bin(log_t *log, usize id, const void *header, usize header_size, const void *payload, usize payload_size);
void log_flush_bin(log_t *log);
void log_write(log_t *log, usize id, const char *fmt, va_list args);
void log_flush(log_t *log);

void log_data(log_t *log, usize id, const char *fmt, ...);
void log_debug(log_t *log, usize id, const char *fmt, ...);
void log_info(log_t *log, usize id, const char *fmt, ...);
void log_warn(log_t *log, usize id, const char *fmt, ...);
void log_err(log_t *log, usize id, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // !LOG_H
