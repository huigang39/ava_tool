#ifndef PRINTOPS_H
#define PRINTOPS_H

#include <stdarg.h>
#include <stdio.h>

#include "macrodef.h"
#include "timeops.h"

#ifdef __cplusplus
extern "C" {
#endif

// 颜色枚举
enum color {
    COLOR_RESET = 0,
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_MAGENTA,
    COLOR_CYAN,
    COLOR_WHITE,
};

// 样式枚举
enum style {
    STYLE_NONE = 0,
    STYLE_BOLD,
    STYLE_DIM,
    STYLE_ITALIC,
    STYLE_UNDERLINE,
    STYLE_BLINK,
    STYLE_REVERSE,
    STYLE_HIDDEN,
};

// ANSI颜色代码
#define COLOR_CODE_RESET "\033[0m"
#define COLOR_CODE_BLACK "\033[30m"
#define COLOR_CODE_RED "\033[31m"
#define COLOR_CODE_GREEN "\033[32m"
#define COLOR_CODE_YELLOW "\033[33m"
#define COLOR_CODE_BLUE "\033[34m"
#define COLOR_CODE_MAGENTA "\033[35m"
#define COLOR_CODE_CYAN "\033[36m"
#define COLOR_CODE_WHITE "\033[37m"

// 样式代码
#define STYLE_CODE_BOLD "\033[1m"
#define STYLE_CODE_DIM "\033[2m"
#define STYLE_CODE_ITALIC "\033[3m"
#define STYLE_CODE_UNDERLINE "\033[4m"
#define STYLE_CODE_BLINK "\033[5m"
#define STYLE_CODE_REVERSE "\033[7m"
#define STYLE_CODE_HIDDEN "\033[8m"

/**
 * @brief  检查终端是否支持颜色
 * @return 支持终端颜色返回 true, 否则返回 false
 */
HAPI int is_terminal_color_supported(void);

/**
 * @brief           获取颜色控制序列
 * @param[in] color 节点颜色或终端颜色
 * @return          终端颜色控制字符串
 */
HAPI const char *get_color_code(enum color color);

/**
 * @brief           获取样式控制序列
 * @param[in] style 输出样式
 * @return          终端样式控制字符串
 */
HAPI const char *get_style_code(enum style style);

/**
 * @brief         格式化输出并换行
 * @param[in] fmt 格式化字符串
 */
HAPI void println(const char *fmt, ...);

/**
 * @brief          使用可变参数列表输出并换行
 * @param[in] fmt  格式化字符串
 * @param[in] args 格式化参数列表
 */
HAPI void vprintln(const char *fmt, va_list args);

/**
 * @brief           按指定颜色和样式输出
 * @param[in] color 节点颜色或终端颜色
 * @param[in] style 输出样式
 * @param[in] fmt   格式化字符串
 */
HAPI void cprintf(enum color color, enum style style, const char *fmt, ...);

/**
 * @brief               输出信息提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_info(uint8_t enable_ts, const char *fmt, ...);

/**
 * @brief               输出成功提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_success(uint8_t enable_ts, const char *fmt, ...);

/**
 * @brief               输出警告提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_warn(uint8_t enable_ts, const char *fmt, ...);

/**
 * @brief               输出错误提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_err(uint8_t enable_ts, const char *fmt, ...);

/**
 * @brief               输出调试提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_debug(uint8_t enable_ts, const char *fmt, ...);

/**
 * @brief             输出进度提示
 * @param[in] percent 进度百分比
 * @param[in] label   进度标签
 */
HAPI void print_progress(int percent, const char *label);

/**
 * @brief  检查终端是否支持颜色
 * @note   检测是否支持颜色输出
 * @return 支持终端颜色返回 true, 否则返回 false
 */
HAPI int is_terminal_color_supported(void)
{
#if OS(WIN)
    // Windows下默认不支持ANSI颜色,但可以通过设置支持
    return false; // 暂时禁用Windows下的颜色
#else
    // Linux/macOS/Unix下通常支持
    return true;
#endif
}

/**
 * @brief           获取颜色控制序列
 * @param[in] color 节点颜色或终端颜色
 * @return          终端颜色控制字符串
 */
HAPI const char *get_color_code(enum color color)
{
    switch (color) {
    case COLOR_BLACK: {
        return COLOR_CODE_BLACK;
    }
    case COLOR_RED: {
        return COLOR_CODE_RED;
    }
    case COLOR_GREEN: {
        return COLOR_CODE_GREEN;
    }
    case COLOR_YELLOW: {
        return COLOR_CODE_YELLOW;
    }
    case COLOR_BLUE: {
        return COLOR_CODE_BLUE;
    }
    case COLOR_MAGENTA: {
        return COLOR_CODE_MAGENTA;
    }
    case COLOR_CYAN: {
        return COLOR_CODE_CYAN;
    }
    case COLOR_WHITE: {
        return COLOR_CODE_WHITE;
    }
    default: {
        return COLOR_CODE_RESET;
    }
    }
}

/**
 * @brief           获取样式控制序列
 * @param[in] style 输出样式
 * @return          终端样式控制字符串
 */
HAPI const char *get_style_code(const enum style style)
{
    switch (style) {
    case STYLE_BOLD: {
        return STYLE_CODE_BOLD;
    }
    case STYLE_DIM: {
        return STYLE_CODE_DIM;
    }
    case STYLE_ITALIC: {
        return STYLE_CODE_ITALIC;
    }
    case STYLE_UNDERLINE: {
        return STYLE_CODE_UNDERLINE;
    }
    case STYLE_BLINK: {
        return STYLE_CODE_BLINK;
    }
    case STYLE_REVERSE: {
        return STYLE_CODE_REVERSE;
    }
    case STYLE_HIDDEN: {
        return STYLE_CODE_HIDDEN;
    }
    default: {
        return "";
    }
    }
}

/**
 * @brief         格式化输出并换行
 * @param[in] fmt 格式化字符串
 */
HAPI void println(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief          使用可变参数列表输出并换行
 * @param[in] fmt  格式化字符串
 * @param[in] args 格式化参数列表
 */
HAPI void vprintln(const char *fmt, va_list args)
{
    vprintf(fmt, args);
    printf("\n");
}

/**
 * @brief           按指定颜色和样式输出
 * @param[in] color 节点颜色或终端颜色
 * @param[in] style 输出样式
 * @param[in] fmt   格式化字符串
 */
HAPI void cprintf(const enum color color,
                  const enum style style,
                  const char *fmt,
                  ...)
{
    va_list args;
    va_start(args, fmt);

    if (is_terminal_color_supported()) {
        const char *color_code = get_color_code(color);
        const char *style_code = get_style_code(style);
        printf("%s%s", style_code, color_code);
        vprintf(fmt, args);
        printf("%s", COLOR_CODE_RESET);
    } else {
        vprintf(fmt, args);
    }

    va_end(args);
}

/**
 * @brief               输出信息提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_info(const uint8_t enable_ts, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cprintf(COLOR_BLUE,
            STYLE_NONE,
            enable_ts ? "[%llu][INFO]" : "[INFO]",
            enable_ts ? get_real_ts_ms() : 0);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief               输出成功提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_success(const uint8_t enable_ts, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cprintf(COLOR_GREEN,
            STYLE_NONE,
            enable_ts ? "[%llu][SUCCESS]" : "[SUCCESS]",
            enable_ts ? get_real_ts_ms() : 0);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief               输出警告提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_warn(const uint8_t enable_ts, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cprintf(COLOR_YELLOW,
            STYLE_NONE,
            enable_ts ? "[%llu][WARN]" : "[WARN]",
            enable_ts ? get_real_ts_ms() : 0);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief               输出错误提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_err(const uint8_t enable_ts, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cprintf(COLOR_RED,
            STYLE_NONE,
            enable_ts ? "[%llu][ERROR]" : "[ERROR]",
            enable_ts ? get_real_ts_ms() : 0);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief               输出调试提示
 * @param[in] enable_ts 是否输出时间戳
 * @param[in] fmt       格式化字符串
 */
HAPI void print_debug(const uint8_t enable_ts, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    cprintf(COLOR_CYAN,
            STYLE_NONE,
            enable_ts ? "[%llu][DEBUG]" : "[DEBUG]",
            enable_ts ? get_real_ts_ms() : 0);
    vprintln(fmt, args);
    va_end(args);
}

/**
 * @brief             输出进度提示
 * @param[in] percent 进度百分比
 * @param[in] label   进度标签
 */
HAPI void print_progress(const int percent, const char *label)
{
    const int bar_width = 50;
    const int pos = bar_width * percent / 100;

    printf("\r%s[", label);
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) {
            cprintf(COLOR_GREEN, STYLE_BOLD, "=");
        } else if (i == pos) {
            cprintf(COLOR_YELLOW, STYLE_BOLD, ">");
        } else {
            printf(" ");
        }
    }
    printf("] %d%%", percent);
    fflush(stdout);
}

#ifdef __cplusplus
}
#endif

#endif
