#include <stdio.h>

#include "log.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
log_init(log_t *log, const log_cfg_t log_cfg)
{
        DECL(log, cfg, lo);
        CFG_INIT(log, log_cfg);

        mpsc_init(&lo->mpsc, cfg->buf, cfg->cap, cfg->producers, cfg->nproducers);
}

void
log_write_bin(
    log_t *log, const usize id, const void *header, const usize header_size, const void *payload, const usize payload_size)
{
        DECL(log, cfg, lo);
        RENAME(&lo->mpsc, mpsc);

        const log_header_t log_header = {
            .ts   = cfg->f_get_ts(),
            .id   = id,
            .size = header_size + payload_size,
        };

        mpsc_reg(&lo->mpsc, id);
        isize offset = mpsc_alloc(&lo->mpsc, id, sizeof(log_header) + header_size + payload_size);
        if (offset < 0) {
                mpsc_unreg(mpsc, id);
                return;
        }

        mpsc_append(&lo->mpsc, offset, &log_header, sizeof(log_header));
        offset += sizeof(log_header);
        mpsc_append(&lo->mpsc, offset, header, header_size);
        offset += (isize)header_size;
        mpsc_append(&lo->mpsc, offset, payload, payload_size);

        mpsc_commit(mpsc, id);
        mpsc_unreg(mpsc, id);
}

void
log_flush_bin(log_t *log)
{
        DECL(log, cfg, lo);

        while (!lo->busy) {
                log_header_t log_header = {0};
                usize        rsize      = mpsc_read(&lo->mpsc, &log_header, sizeof(log_header));
                if (rsize == 0)
                        break;

                rsize = mpsc_read(&lo->mpsc, cfg->flush_buf, log_header.size);

                cfg->f_flush(cfg->fp, cfg->flush_buf, rsize);
                lo->busy = (cfg->e_mode == LOG_MODE_ASYNC);
        }
}

void
log_write(log_t *log, const usize id, const char *fmt, va_list args)
{
        DECL(log, cfg, lo);
        RENAME(&log->lo.mpsc, mpsc);

        va_list args_header;
        va_copy(args_header, args);
        const log_header_t log_header = {
            .ts   = cfg->f_get_ts(),
            .id   = id,
            .size = (usize)vsnprintf(NULL, 0, fmt, args_header) + 1,
        };
        va_end(args_header);

        const usize total_size = sizeof(log_header) + log_header.size;
        if (total_size > cfg->cap)
                return;

        mpsc_reg(mpsc, id);
        isize offset = mpsc_alloc(mpsc, id, total_size);
        if (offset < 0) {
                mpsc_unreg(mpsc, id);
                return;
        }

        mpsc_append(&lo->mpsc, offset, &log_header, sizeof(log_header));
        offset += sizeof(log_header);

        va_list args_msg;
        va_copy(args_msg, args);
        vsnprintf((char *)lo->mpsc.buf + offset, log_header.size, fmt, args_msg);
        va_end(args_msg);

        mpsc_commit(mpsc, id);
        mpsc_unreg(mpsc, id);
}

void
log_flush(log_t *log)
{
        DECL(log, cfg, lo);

        while (!lo->busy) {
                log_header_t header      = {0};
                const usize  header_size = mpsc_read(&lo->mpsc, &header, sizeof(header));
                if (header_size == 0)
                        break;

#ifdef MCU
                usize total_size = snprintf((char *)cfg->flush_buf, cfg->flush_cap, "[%llu][%u]", header.ts, header.id);
#else
                usize total_size = snprintf((char *)cfg->flush_buf, cfg->flush_cap, "[%llu][%llu]", header.ts, header.id);
#endif

                total_size += mpsc_read(&lo->mpsc, cfg->flush_buf + total_size, header.size);

                cfg->f_flush(cfg->fp, cfg->flush_buf, total_size);
                lo->busy = (cfg->e_mode == LOG_MODE_ASYNC);
        }
}

void
log_data(log_t *log, const usize id, const char *fmt, ...)
{
        DECL(log, cfg);

        if (cfg->e_level > LOG_LEVEL_DATA)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, id, fmt, args);
        va_end(args);
}

void
log_debug(log_t *log, const usize id, const char *fmt, ...)
{
        DECL(log, cfg);

        if (cfg->e_level > LOG_LEVEL_DEBUG)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, id, fmt, args);
        va_end(args);
}

void
log_info(log_t *log, const usize id, const char *fmt, ...)
{
        DECL(log, cfg);

        if (cfg->e_level > LOG_LEVEL_INFO)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, id, fmt, args);
        va_end(args);
}

void
log_warn(log_t *log, const usize id, const char *fmt, ...)
{
        DECL(log, cfg);

        if (cfg->e_level > LOG_LEVEL_WARN)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, id, fmt, args);
        va_end(args);
}

void
log_err(log_t *log, const usize id, const char *fmt, ...)
{
        DECL(log, cfg);

        if (cfg->e_level > LOG_LEVEL_ERR)
                return;

        va_list args;
        va_start(args, fmt);
        log_write(log, id, fmt, args);
        va_end(args);
}
