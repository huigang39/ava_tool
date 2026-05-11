#ifndef APP_LOG_HPP
#define APP_LOG_HPP

#include "log.h"

extern log_t g_log;
extern int get_log_idx();

#define LOG_I(fmt, ...) log_info(&g_log, get_log_idx(), fmt "\n", ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_error(&g_log, get_log_idx(), fmt "\n", ##__VA_ARGS__)

#endif
