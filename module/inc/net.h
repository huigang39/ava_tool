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

#include "list.h"
#include "log.h"
#include "mempool.h"
#include "timeops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#define MAX_IP_SIZE                 (16)
#define MAX_RESP_BUF_SIZE           (1024)
#define MAX_IP_NUM                  (255)

#define IP_STR_TO_U32(ip)           inet_addr(ip)
#define IP_U32_TO_STR(ip, buf, cap) inet_ntop(AF_INET, ip, buf, cap)

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum net_type {
    NET_TYPE_NULL,
    NET_TYPE_UDP,
    NET_TYPE_TCP,
};

enum net_mode {
    NET_MODE_SYNC_YIELD,
    NET_MODE_SYNC_SPIN,
    NET_MODE_ASYNC,
};

enum net_op {
    NET_OP_SEND,
    NET_OP_RECV,
};

#pragma pack(push, 1)
struct net_log_header {
    size_t   ts;       // 时间戳
    uint8_t  e_op;     // 收发标志 (net_op_e, 存储为 uint8_t 以保持包紧凑)
    uint32_t dst_ip;   // 设备IP
    uint16_t dst_port; // 设备端口
    uint16_t size;     // 数据长度
};
#pragma pack(pop)

struct net_resp {
    char ip[MAX_IP_SIZE];
    char buf[MAX_RESP_BUF_SIZE];
};

struct net_ch;
typedef void (*net_async_cb_f)(struct net_ch *ch, void *buf, int ret);

struct net_ch {
    struct list_head ch_node;
    sockfd_t         fd;
    net_async_cb_f   f_send_cb, f_recv_cb;
    enum net_mode    e_mode;
    uint32_t         dst_ip, src_ip;
    uint16_t         dst_port, src_port;
};

struct net_async_req {
    struct net_ch *ch;
    void          *buf;
    size_t         size;
    net_async_cb_f f_cb;
    ATOMIC(uint8_t) processed;
#if OS(WIN) || OS(MAC)
    uint64_t         timeout_us;
    struct list_head pending_node; // 待处理请求链表节点
#endif
#if OS(WIN)
    OVERLAPPED ov;
#endif
#if OS(MAC)
    enum net_op e_op;
#endif
};

typedef uint64_t (*net_get_ts_f)(void);

struct net_cfg {
    enum net_type   e_type;
    struct mempool *mempool;
    uint32_t        ring_len;
    struct log_cfg  log_cfg;
    net_get_ts_f    f_get_ts;
};

struct net_lo {
    struct list_head ch_root;
    struct log       log;
#if OS(LINUX)
    struct io_uring ring;
    bool            ring_initialized;
#elif OS(MAC)
    int              kq;
    struct list_head pending_reqs;
#elif OS(WIN)
    HANDLE           iocp;
    struct list_head pending_reqs; // 待处理的异步请求列表
#endif
};

struct net {
    struct net_cfg cfg;
    struct net_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 将 socket 设置为非阻塞模式
 *
 * @param fd socket 文件描述符
 * @return   错误码
 */
int net_set_nonblock(sockfd_t fd);

/**
 * @brief net 结构体初始化
 *
 * @param net     net 结构体
 * @param net_cfg net 配置
 * @return        错误码
 */
int net_init(struct net *net, struct net_cfg net_cfg);

/**
 * @brief 清理 net 相关 socket
 *
 * @param net net 结构体
 * @return    错误码
 */
void net_destroy(struct net *net);

/**
 * @brief 返回 net 通道
 *
 * @param det_ip    目标 IP
 * @param dst_port  目标端口
 * @param e_mode    收发模式
 * @return          struct net_ch
 */
struct net_ch net_cfg_ch(uint32_t det_ip, uint16_t dst_port, enum net_mode e_mode);

/**
 * @brief 添加 net 通道
 *
 * @param net net 结构体
 * @param ch  通道
 * @return    错误码
 */
int net_add_ch(struct net *net, struct net_ch *ch);

/**
 * @brief 同步发送
 *
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
ptrdiff_t net_sync_send(const struct net_ch *ch, const void *tx_buf, size_t size);

/**
 * @brief 让出等待同步接收
 *
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t
net_sync_recv_yield(const struct net_ch *ch, void *rx_buf, size_t cap, uint32_t timeout_us);

/**
 * @brief 自旋等待同步接收
 *
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t
net_sync_recv_spin(const struct net_ch *ch, void *rx_buf, size_t cap, uint32_t timeout_us);

/**
 * @brief 异步发送
 *
 * @param net    net 结构体
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
ptrdiff_t net_async_send(struct net *net, struct net_ch *ch, void *tx_buf, size_t size);

/**
 * @brief 异步接收
 *
 * @param net        net 结构体
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t
net_async_recv(struct net *net, struct net_ch *ch, void *rx_buf, size_t cap, uint32_t timeout_us);

/**
 * @brief 轮询处理异步请求
 *
 * @param net net 结构体
 * @return    错误码
 */
int net_poll(struct net *net);

/**
 * @brief 根据 net 配置自动选择模式发送
 *
 * @param net    net 结构体
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
ptrdiff_t net_send(struct net *net, struct net_ch *ch, void *tx_buf, size_t size);

/**
 * @brief 根据 net 配置自动选择模式接收
 *
 * @param net        net 结构体
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t
net_recv(struct net *net, struct net_ch *ch, void *rx_buf, size_t cap, uint32_t timeout_us);

/**
 * @brief 根据 net 配置自动选择模式发送并接收
 *
 * @param net        net 结构体
 * @param ch         通道
 * @param tx_buf     发送缓冲区
 * @param size       发送字节数
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
ptrdiff_t net_send_recv(struct net    *net,
                        struct net_ch *ch,
                        void          *tx_buf,
                        size_t         size,
                        void          *rx_buf,
                        size_t         cap,
                        uint32_t       timeout_us);

/**
 * @brief 向指定广播 IP 和端口发送并接收回复内容
 *
 * @param ip         广播 IP
 * @param port       广播端口
 * @param tx_buf     发送缓冲区
 * @param size       发送字节数
 * @param resps      回复内容数组
 * @param resps_cap  回复内容数组容量 (resps[] 元素个数)
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回回复的 IP 个数, 失败返回错误码
 */
int net_broadcast(uint32_t         ip,
                  uint16_t         port,
                  const void      *tx_buf,
                  size_t           size,
                  struct net_resp *resps,
                  size_t           resps_cap,
                  uint32_t         timeout_us);

#ifdef __cplusplus
}
#endif

#endif // !NET_H
