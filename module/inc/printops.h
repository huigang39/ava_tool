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
typedef enum color {
        COLOR_RESET = 0,
        COLOR_BLACK,
        COLOR_RED,
        COLOR_GREEN,
        COLOR_YELLOW,
        COLOR_BLUE,
        COLOR_MAGENTA,
        COLOR_CYAN,
        COLOR_WHITE
} color_t;

// 样式枚举
typedef enum style {
        STYLE_NONE = 0,
        STYLE_BOLD,
        STYLE_DIM,
        STYLE_ITALIC,
        STYLE_UNDERLINE,
        STYLE_BLINK,
        STYLE_REVERSE,
        STYLE_HIDDEN
} style_t;

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
HAPI const char *get_color_code(color_t color);
HAPI const char *get_style_code(style_t style);

HAPI void println(const char *fmt, ...);
HAPI void vprintln(const char *fmt, va_list args);
HAPI void cprintf(color_t color, style_t style, const char *fmt, ...);

HAPI void print_info(bool with_timestamp, const char *fmt, ...);
HAPI void print_success(bool with_timestamp, const char *fmt, ...);
HAPI void print_warn(bool with_timestamp, const char *fmt, ...);
HAPI void print_error(bool with_timestamp, const char *fmt, ...);
HAPI void print_debug(bool with_timestamp, const char *fmt, ...);

HAPI void print_progress(const int percent, const char *label);

// 检测是否支持颜色输出
HAPI int
supports_color(void)
{
#ifdef _WIN32
        // Windows下默认不支持ANSI颜色，但可以通过设置支持
        return 0; // 暂时禁用Windows下的颜色
#else
        // Linux/Unix下通常支持
        return 1;
#endif
}

HAPI const char *
get_color_code(color_t color)
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
get_style_code(style_t style)
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
cprintf(color_t color, style_t style, const char *fmt, ...)
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
print_info(bool with_timestamp, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        cprintf(COLOR_BLUE, STYLE_NONE, with_timestamp ? "[%llu][INFO]" : "[INFO]", with_timestamp ? get_real_ts_ms() : 0);
        vprintln(fmt, args);
        va_end(args);
}

HAPI void
print_success(bool with_timestamp, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        cprintf(
            COLOR_GREEN, STYLE_NONE, with_timestamp ? "[%llu][SUCCESS]" : "[SUCCESS]", with_timestamp ? get_real_ts_ms() : 0);
        vprintln(fmt, args);
        va_end(args);
}

HAPI void
print_warn(bool with_timestamp, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        cprintf(COLOR_YELLOW, STYLE_NONE, with_timestamp ? "[%llu][WARN]" : "[WARN]", with_timestamp ? get_real_ts_ms() : 0);
        vprintln(fmt, args);
        va_end(args);
}

HAPI void
print_error(bool with_timestamp, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        cprintf(COLOR_RED, STYLE_NONE, with_timestamp ? "[%llu][ERROR]" : "[ERROR]", with_timestamp ? get_real_ts_ms() : 0);
        vprintln(fmt, args);
        va_end(args);
}

HAPI void
print_debug(bool with_timestamp, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        cprintf(COLOR_CYAN, STYLE_NONE, with_timestamp ? "[%llu][DEBUG]" : "[DEBUG]", with_timestamp ? get_real_ts_ms() : 0);
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
