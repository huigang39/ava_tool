#ifndef NET_H
#define NET_H

#include "platdef.h"

#if OS(LINUX)
#include <arpa/inet.h>
#include <fcntl.h>
#include <liburing.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sockfd_t;
#define CLOSE_SOCKET close
#elif OS(MAC)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sockfd_t;
#define CLOSE_SOCKET close
#elif OS(WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sockfd_t;
#define CLOSE_SOCKET closesocket
#else
typedef int sockfd_t;
#endif

#include <stdio.h>
#include <string.h>

#include "errdef.h"
#include "list.h"
#include "log.h"
#include "mempool.h"
#include "timeops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_IP_SIZE (16)
#define MAX_RSP_BUF_SIZE (1024)
#define MAX_IP_CNT (255)

#define IP_STR_TO_U32(ip) inet_addr(ip)
#define IP_U32_TO_STR(ip, buf, cap) inet_ntop(AF_INET, ip, buf, cap)

enum net_type {
    NET_TYPE_NULL, // 未设置网络类型
    NET_TYPE_UDP,  // UDP 数据报通信
    NET_TYPE_TCP,  // TCP 流通信
};

enum net_mode {
    NET_MODE_SYNC_YIELD, // 同步等待并让出处理器
    NET_MODE_SYNC_SPIN,  // 同步忙等待
    NET_MODE_ASYNC,      // 异步通信
};

enum net_operation {
    NET_OPERATION_SEND, // 发送请求
    NET_OPERATION_RX,   // 接收请求
};

#pragma pack(push, 1)

struct net_log_header {
    size_t ts;                 // 时间戳
    uint8_t operation;         // 收发标志, 取值见 net_operation
    uint32_t destination_ip;   // 设备IP
    uint16_t destination_port; // 设备端口
    uint16_t size;             // 数据长度
};

#pragma pack(pop)

struct net_rsp {
    char ip[MAX_IP_SIZE];       // 响应来源 IP 地址
    char buf[MAX_RSP_BUF_SIZE]; // 算法或通信存储缓冲区
};

struct net_ch;
typedef void (*net_async_cb_f)(struct net_ch *ch, void *buf, int ret);

struct net_ch {
    struct list_head ch_node;  // 通信通道链表节点
    sockfd_t fd;               // 文件或通信句柄
    net_async_cb_f send_cb;    // 发送完成回调
    net_async_cb_f rx_cb;      // 接收完成回调
    enum net_mode mode;        // 处理模式
    uint32_t destination_ip;   // 目标 IP 地址
    uint32_t source_ip;        // 源 IP 地址
    uint16_t destination_port; // 目标端口
    uint16_t source_port;      // 源端口
};

struct net_async_req {
    struct net_ch *ch;         // 请求使用的通信通道
    void *buf;                 // 算法或通信存储缓冲区
    size_t size;               // 数据长度, 单位: byte
    net_async_cb_f cb;         // 异步操作完成回调
    ATOMIC(uint8_t) processed; // 请求完成或超时处理标志
#if OS(WIN) || OS(MAC)
    uint64_t timeout_us;           // 操作超时, 单位: us
    struct list_head pending_node; // 待处理请求链表节点
#endif
#if OS(WIN)
    OVERLAPPED ov; // Windows 异步操作状态
#endif
#if OS(MAC)
    enum net_operation operation; // 异步请求类型
#endif
};

typedef uint64_t (*net_get_ts_f)(void);

struct net_cfg {
    enum net_type type;      // 处理类型
    struct mempool *mempool; // 分配运行缓冲区的内存池
    uint32_t ring_len;       // 异步请求队列容量
    struct log_cfg log_cfg;  // 网络日志配置
    net_get_ts_f get_ts;     // 读取时间戳的回调
};

struct net_lo {
    struct list_head ch_root; // 通信通道链表头
    struct log log;           // 网络日志运行对象
    int prev_err;             // 最近操作错误码
#if OS(LINUX)
    struct io_uring ring;  // Linux 异步 I/O 队列
    bool ring_initialized; // 异步 I/O 队列已初始化
#elif OS(MAC)
    int kq;                        // macOS 事件队列句柄
    struct list_head pending_reqs; // 待完成的异步请求链表
#elif OS(WIN)
    HANDLE iocp;                   // Windows 完成端口句柄
    struct list_head pending_reqs; // 待处理的异步请求列表
#endif
};

struct net {
    struct net_cfg cfg; // 配置参数
    struct net_lo lo;   // 运行状态
};

/**
 * @brief        将 socket 设置为非阻塞模式
 * @param[in] fd socket 文件描述符
 * @return       错误码
 */
int net_set_nonblock(sockfd_t fd);

/**
 * @brief         获取最近一次网络错误
 * @param[in] net 网络对象
 * @return        最近一次网络错误码
 */
int net_prev_err(const struct net *net);

/**
 * @brief                 net 结构体初始化
 * @param[in,out] net     net 结构体
 * @param[in]     net_cfg net 配置
 * @return                错误码
 */
int net_init(struct net *net, struct net_cfg net_cfg);

/**
 * @brief             关闭网络对象并释放请求
 * @param[in,out] net 网络对象
 * @note              停止其他线程访问后调用; Windows 等待取消完成, 不调用请求回调
 */
void net_destroy(struct net *net);

/**
 * @brief                      返回 net 通道
 * @param[in] destination_ip   目标 IP
 * @param[in] destination_port 目标端口
 * @param[in] mode             收发模式
 */
struct net_ch net_cfg_ch(uint32_t destination_ip,
                         uint16_t destination_port,
                         enum net_mode mode);

/**
 * @brief             添加 net 通道
 * @param[in,out] net 网络对象
 * @param[in,out] ch  通道
 * @return            成功返回 0, 失败返回负错误码
 * @note              ch 须保持有效直到 net_destroy 返回
 */
int net_add_ch(struct net *net, struct net_ch *ch);

/**
 * @brief            同步发送
 * @param[in] ch     通道
 * @param[in] tx_buf 发送缓冲区
 * @param[in] size   发送字节数
 * @return           成功返回发送的字节数, 失败返回错误码
 */
ptrdiff_t net_sync_send(const struct net_ch *ch,
                        const void *tx_buf,
                        size_t size);

/**
 * @brief                 让出等待同步接收
 * @param[in]  ch         通道
 * @param[out] rx_buf     接收缓冲区
 * @param[in]  cap        接收缓冲区容量字节数
 * @param[in]  timeout_us 超时时间, 单位: us
 * @return                成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t net_sync_recv_yield(const struct net_ch *ch,
                              void *rx_buf,
                              size_t cap,
                              uint32_t timeout_us);

/**
 * @brief                 自旋等待同步接收
 * @param[in]  ch         通道
 * @param[out] rx_buf     接收缓冲区
 * @param[in]  cap        接收缓冲区容量字节数
 * @param[in]  timeout_us 超时时间, 单位: us
 * @return                成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t net_sync_recv_spin(const struct net_ch *ch,
                             void *rx_buf,
                             size_t cap,
                             uint32_t timeout_us);

/**
 * @brief                异步发送
 * @param[in,out] net    net 结构体
 * @param[in]     ch     通道
 * @param[in]     tx_buf 发送缓冲区
 * @param[in]     size   发送字节数
 * @return               Windows 提交成功返回 size, 失败返回负错误码
 * @note                 ch 须先加入 net; 缓冲区须保留至完成回调或 net_destroy 返回
 */
ptrdiff_t net_async_send(struct net *net,
                         struct net_ch *ch,
                         void *tx_buf,
                         size_t size);

/**
 * @brief                    异步接收
 * @param[in,out] net        net 结构体
 * @param[in]     ch         通道
 * @param[out]    rx_buf     接收缓冲区
 * @param[in]     cap        接收缓冲区容量字节数
 * @param[in]     timeout_us 超时时间, 单位: us
 * @return                   Windows 提交成功返回 cap, 失败返回负错误码
 * @note                     ch 须先加入 net; 缓冲区须保留至完成回调或 net_destroy 返回; 0 不自动取消
 */
ptrdiff_t net_async_recv(struct net *net,
                         struct net_ch *ch,
                         void *rx_buf,
                         size_t cap,
                         uint32_t timeout_us);

/**
 * @brief             轮询处理异步请求
 * @param[in,out] net 网络对象
 * @return            成功返回 0, 失败返回负错误码
 * @note              与通道添加和请求提交串行调用; 回调中不得销毁 net
 */
int net_poll(struct net *net);

/**
 * @brief                根据 net 配置自动选择模式发送
 * @param[in,out] net    net 结构体
 * @param[in]     ch     通道
 * @param[in]     tx_buf 发送缓冲区
 * @param[in]     size   发送字节数
 * @return               成功返回发送的字节数, 失败返回错误码
 */
ptrdiff_t net_send(struct net *net,
                   struct net_ch *ch,
                   void *tx_buf,
                   size_t size);

/**
 * @brief                    根据 net 配置自动选择模式接收
 * @param[in,out] net        net 结构体
 * @param[in]     ch         通道
 * @param[out]    rx_buf     接收缓冲区
 * @param[in]     cap        接收缓冲区容量字节数
 * @param[in]     timeout_us 超时时间, 单位: us
 * @return                   成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t net_recv(struct net *net,
                   struct net_ch *ch,
                   void *rx_buf,
                   size_t cap,
                   uint32_t timeout_us);

/**
 * @brief                    根据 net 配置自动选择模式发送并接收
 * @param[in,out] net        net 结构体
 * @param[in]     ch         通道
 * @param[in]     tx_buf     发送缓冲区
 * @param[in]     size       发送字节数
 * @param[out]    rx_buf     接收缓冲区
 * @param[in]     cap        接收缓冲区容量字节数
 * @param[in]     timeout_us 超时时间, 单位: us
 * @return                   成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t net_send_recv(struct net *net,
                        struct net_ch *ch,
                        void *tx_buf,
                        size_t size,
                        void *rx_buf,
                        size_t cap,
                        uint32_t timeout_us);

/**
 * @brief                 向指定广播 IP 和端口发送并接收回复内容
 * @param[in]  ip         广播 IP
 * @param[in]  port       广播端口
 * @param[in]  tx_buf     发送缓冲区
 * @param[in]  size       发送字节数
 * @param[out] rsp        回复内容数组
 * @param[in]  rsp_cap    回复内容数组容量 (rsp[] 元素个数)
 * @param[in]  timeout_us 超时时间, 单位: us
 * @return                成功返回回复的 IP 个数, 失败返回错误码
 */
int net_broadcast(uint32_t ip,
                  uint16_t port,
                  const void *tx_buf,
                  size_t size,
                  struct net_rsp *rsp,
                  size_t rsp_cap,
                  uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif
