#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"
#include "mempool.h"
#include "mpsc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum log_fmt {
    LOG_FMT_TXT, // 格式化文本日志
    LOG_FMT_CSV, // 逗号分隔数据日志
    LOG_FMT_BIN, // 二进制日志
};

enum log_level {
    LOG_LEVEL_DATA,  // 数据记录级别
    LOG_LEVEL_DEBUG, // 调试级别
    LOG_LEVEL_INFO,  // 信息级别
    LOG_LEVEL_WARN,  // 警告级别
    LOG_LEVEL_ERR,   // 错误级别
};

enum log_mode {
    LOG_MODE_SYNC,  // 同步写入模式
    LOG_MODE_ASYNC, // 异步写入模式
};

enum log_ring {
    LOG_RING_TRUNCATE, // 文件写满后清空并重新写入
    LOG_RING_ROTATE,   // 文件写满后切换到新文件
};

struct log_header {
    uint64_t ts; // 日志时间戳
    size_t id;   // 生产者标识
    size_t size; // 数据长度, 单位: byte
};

typedef uint64_t (*log_get_ts_f)(void);
typedef int (*log_flush_f)(void *fp, const void *source, size_t size);

struct log_cfg {
    // 日志模式与过滤
    enum log_mode mode;   // 处理模式
    enum log_level level; // 日志过滤级别
    enum log_fmt fmt;     // 日志存储格式

    // 存储与文件配置
    struct mempool *mempool; // 分配运行缓冲区的内存池
    const char *file_path;
    const char *csv_header; // CSV 文件首行; TXT/BIN 格式忽略
    void *fd;               // 文件或通信句柄
    size_t file_size;       // 单个日志文件大小上限, 单位: byte
    size_t max_files;       // 最多保留的日志文件数
    enum log_ring ring;     // 文件写满后的处理策略

    // 刷新时序与缓冲容量
    uint64_t flush_interval_us; // 仅控制文件流 fflush; 0 表示每次排空都刷新
    size_t chunk_size;          // 每个线程块的最大尺寸
    size_t flush_cap;           // 刷新缓冲区容量, 单位: byte
    size_t producer_cnt;        // 记录最大线程数

    // 时间与输出回调
    log_get_ts_f get_ts; // 读取时间戳的回调
    log_flush_f flush;   // 同步输出数据, 返回前须完成读取
};

struct log_chunk {
    struct mpsc_node node; // 队列链接节点
    size_t offset;         // 当前数据偏移, 单位: byte
};

struct log_producer {
    ATOMIC(uint8_t) lock;
    struct log_chunk *chunk; // 生产者当前日志块
};

struct log_check {
    uint8_t file_failed; // 文件操作已发生错误
    int file_err;        // 最近文件操作错误码
};

struct log_file {
    size_t offset;          // 当前日志文件写入偏移, 单位: byte
    uint64_t prev_flush_ts; // 最近一次文件刷新时间, 单位: us
    void *stream;           // 日志文件流
    uint8_t *buf;           // 文件流缓冲区
    char curr_path[512];    // 当前日志文件路径
    char final_path[512];   // 完成后日志文件路径
};

struct log_lo {
    ATOMIC(uint8_t) busy;   // 当前正在刷新日志
    struct log_check check; // 文件输出错误现场

    // 待输出队列与生产者存储
    struct mpsc mpsc;               // 待输出日志块队列
    uint8_t *flush_buf;             // 日志输出缓冲区
    struct log_producer *producers; // 各生产者独立的日志块状态

    struct log_file file; // 文件输出状态和路径缓冲区
};

struct log {
    struct log_cfg cfg; // 配置参数
    struct log_lo lo;   // 运行状态
};

/**
 * @brief                 初始化日志配置和缓冲区
 * @param[in,out] log     日志对象
 * @param[in]     log_cfg 日志配置
 * @return                成功返回 0, 失败返回负错误码
 */
int log_init(struct log *log, struct log_cfg log_cfg);

/**
 * @brief             刷新日志并释放资源
 * @param[in,out] log 日志对象
 * @return            成功返回 0, 失败返回负错误码
 * @note              先停止写入和刷新调用; 正在刷新时返回 -MEBUSY 并保留资源
 */
int log_deinit(struct log *log);

/**
 * @brief                      写入二进制日志记录
 * @param[in,out] log          日志对象
 * @param[in]     id           生产者 ID
 * @param[in]     header       记录头数据
 * @param[in]     header_size  记录头长度, 单位: byte
 * @param[in]     payload      记录内容
 * @param[in]     payload_size 记录内容长度, 单位: byte
 * @return                     成功返回 0, 失败返回负错误码
 */
int log_write_bin(struct log *log,
                  size_t id,
                  const void *header,
                  size_t header_size,
                  const void *payload,
                  size_t payload_size);

/**
 * @brief              使用可变参数列表写入格式化日志
 * @param[in,out] log  日志对象
 * @param[in]     idx  生产者索引
 * @param[in]     fmt  格式化字符串
 * @param[in]     args 格式化参数列表
 * @return             成功返回 0, 失败返回负错误码
 */
int log_write(struct log *log, size_t idx, const char *fmt, va_list args);

/**
 * @brief             输出待处理日志并刷新文件
 * @param[in,out] log 日志对象
 * @return            成功返回 0, 失败返回负错误码
 */
int log_flush(struct log *log);

/**
 * @brief             写入数据级日志
 * @param[in,out] log 日志对象
 * @param[in]     idx 生产者索引
 * @param[in]     fmt 格式化字符串
 * @param[in]     ... 格式化参数
 * @return            成功或被日志等级过滤返回 0, 失败返回负错误码
 */
int log_data(struct log *log, size_t idx, const char *fmt, ...);

/**
 * @brief             写入调试级日志
 * @param[in,out] log 日志对象
 * @param[in]     idx 生产者索引
 * @param[in]     fmt 格式化字符串
 * @param[in]     ... 格式化参数
 * @return            成功或被日志等级过滤返回 0, 失败返回负错误码
 */
int log_debug(struct log *log, size_t idx, const char *fmt, ...);

/**
 * @brief             写入信息级日志
 * @param[in,out] log 日志对象
 * @param[in]     idx 生产者索引
 * @param[in]     fmt 格式化字符串
 * @param[in]     ... 格式化参数
 * @return            成功或被日志等级过滤返回 0, 失败返回负错误码
 */
int log_info(struct log *log, size_t idx, const char *fmt, ...);

/**
 * @brief             写入警告级日志
 * @param[in,out] log 日志对象
 * @param[in]     idx 生产者索引
 * @param[in]     fmt 格式化字符串
 * @param[in]     ... 格式化参数
 * @return            成功或被日志等级过滤返回 0, 失败返回负错误码
 */
int log_warn(struct log *log, size_t idx, const char *fmt, ...);

/**
 * @brief             写入错误级日志
 * @param[in,out] log 日志对象
 * @param[in]     idx 生产者索引
 * @param[in]     fmt 格式化字符串
 * @param[in]     ... 格式化参数
 * @return            成功或被日志等级过滤返回 0, 失败返回负错误码
 */
int log_err(struct log *log, size_t idx, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
