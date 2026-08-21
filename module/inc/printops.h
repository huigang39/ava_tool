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
    COLOR_WHITE
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
    STYLE_HIDDEN
};

// ANSI颜色代码
#define COLOR_CODE_RESET     "\033[0m"
#define COLOR_CODE_BLACK     "\033[30m"
#define COLOR_CODE_RED       "\033[31m"
#define COLOR_CODE_GREEN     "\033[32m"
#define COLOR_CODE_YELLOW    "\033[33m"
#define COLOR_CODE_BLUE      "\033[34m"
#define COLOR_CODE_MAGENTA   "\033[35m"
#define COLOR_CODE_CYAN      "\033[36m"
#define COLOR_CODE_WHITE     "\033[37m"

// 样式代码
#define STYLE_CODE_BOLD      "\033[1m"
#define STYLE_CODE_DIM       "\033[2m"
#define STYLE_CODE_ITALIC    "\033[3m"
#define STYLE_CODE_UNDERLINE "\033[4m"
#define STYLE_CODE_BLINK     "\033[5m"
#define STYLE_CODE_REVERSE   "\033[7m"
#define STYLE_CODE_HIDDEN    "\033[8m"

HAPI int         supports_color(void);
HAPI const char *get_color_code(enum color color);
HAPI const char *get_style_code(enum style style);

HAPI void println(const char *fmt, ...);
HAPI void vprintln(const char *fmt, va_list args);
HAPI void cprintf(enum color color, enum style style, const char *fmt, ...);

HAPI void print_info(uint8_t enable_ts, const char *fmt, ...);
HAPI void print_success(uint8_t enable_ts, const char *fmt, ...);
HAPI void print_warn(uint8_t enable_ts, const char *fmt, ...);
HAPI void print_error(uint8_t enable_ts, const char *fmt, ...);
HAPI void print_debug(uint8_t enable_ts, const char *fmt, ...);

HAPI void print_progress(int percent, const char *label);

// 检测是否支持颜色输出
HAPI int
supports_color(void)
{
#if OS(WIN)
    // Windows下默认不支持ANSI颜色,但可以通过设置支持
    return false; // 暂时禁用Windows下的颜色
#else
    // Linux/macOS/Unix下通常支持
    return true;
#endif
}

HAPI const char *
get_color_code(enum color color)
{
    switch (color) {
        case COLOR_BLACK:
            return COLOR_CODE_BLACK;
        case COLOR_RED:
            return COLOR_CODE_RED;
        case COLOR_GREEN:
            return COLOR_CODE_GREEN;
        case COLOR_YELLOW:
            return COLOR_CODE_YELLOW;
        case COLOR_BLUE:
            return COLOR_CODE_BLUE;
        case COLOR_MAGENTA:
            return COLOR_CODE_MAGENTA;
        case COLOR_CYAN:
            return COLOR_CODE_CYAN;
        case COLOR_WHITE:
            return COLOR_CODE_WHITE;
        default:
            return COLOR_CODE_RESET;
    }
}

HAPI const char *
get_style_code(const enum style style)
{
    switch (style) {
        case STYLE_BOLD:
            return STYLE_CODE_BOLD;
        case STYLE_DIM:
            return STYLE_CODE_DIM;
        case STYLE_ITALIC:
            return STYLE_CODE_ITALIC;
        case STYLE_UNDERLINE:
            return STYLE_CODE_UNDERLINE;
        case STYLE_BLINK:
            return STYLE_CODE_BLINK;
        case STYLE_REVERSE:
            return STYLE_CODE_REVERSE;
        case STYLE_HIDDEN:
            return STYLE_CODE_HIDDEN;
        default:
            return "";
    }
}

HAPI void
println(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintln(fmt, args);
    va_end(args);
}

HAPI void
vprintln(const char *fmt, va_list args)
{
    vprintf(fmt, args);
    printf("\n");
}

HAPI void
cprintf(const enum color color, const enum style style, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    if (supports_color()) {
        const char *color_code = get_color_code(color);
        const char *style_code = get_style_code(style);
        printf("%s%s", style_code, color_code);
        vprintf(fmt, args);
        printf("%s", COLOR_CODE_RESET);
    } else
        vprintf(fmt, args);

    va_end(args);
}

HAPI void
print_info(const uint8_t enable_ts, const char *fmt, ...)
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

HAPI void
print_success(const uint8_t enable_ts, const char *fmt, ...)
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

HAPI void
print_warn(const uint8_t enable_ts, const char *fmt, ...)
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

HAPI void
print_error(const uint8_t enable_ts, const char *fmt, ...)
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

HAPI void
print_debug(const uint8_t enable_ts, const char *fmt, ...)
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

HAPI void
print_progress(const int percent, const char *label)
{
    const int bar_width = 50;
    const int pos       = bar_width * percent / 100;

    printf("\r%s[", label);
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos)
            cprintf(COLOR_GREEN, STYLE_BOLD, "=");
        else if (i == pos)
            cprintf(COLOR_YELLOW, STYLE_BOLD, ">");
        else
            printf(" ");
    }
    printf("] %d%%", percent);
    fflush(stdout);
}

#ifdef __cplusplus
}
#endif

#endif // !PRINTOPS_H
