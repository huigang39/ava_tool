#ifndef APP_LOG_HPP
#define APP_LOG_HPP

#include "module.h"

extern log_t g_log_ops;
extern log_t g_log_perf;
extern int   get_log_idx();

#define LOG_D(fmt, ...)    log_debug(&g_log_ops, get_log_idx(), fmt "\n", ##__VA_ARGS__)
#define LOG_I(fmt, ...)    log_info(&g_log_ops, get_log_idx(), fmt "\n", ##__VA_ARGS__)
#define LOG_W(fmt, ...)    log_warn(&g_log_ops, get_log_idx(), fmt "\n", ##__VA_ARGS__)
#define LOG_E(fmt, ...)    log_error(&g_log_ops, get_log_idx(), fmt "\n", ##__VA_ARGS__)

#define LOG_PERF(fmt, ...) log_info(&g_log_perf, get_log_idx(), fmt "\n", ##__VA_ARGS__)

#endif
