#ifndef NET_H
#define NET_H

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <liburing.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sockfd_t;
#define CLOSE_SOCKET close
#elif defined(_WIN32)
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

typedef enum net_type {
        NET_TYPE_NULL,
        NET_TYPE_UDP,
        NET_TYPE_TCP,
} net_type_e;

typedef enum net_mode {
        NET_MODE_SYNC_YIELD,
        NET_MODE_SYNC_SPIN,
        NET_MODE_ASYNC,
} net_mode_e;

typedef enum net_op : u8 {
        NET_OP_SEND,
        NET_OP_RECV,
} net_op_e;

#pragma pack(push, 1)
typedef struct net_log_header {
        usize    ts;       // 时间戳
        net_op_e e_op;     // 收发标志
        u32      dst_ip;   // 设备IP
        u16      dst_port; // 设备端口
        u16      size;     // 数据长度
} net_log_header_t;
#pragma pack(pop)

typedef struct net_resp {
        char ip[MAX_IP_SIZE];
        char buf[MAX_RESP_BUF_SIZE];
} net_resp_t;

struct net_ch;
typedef void (*net_async_cb_f)(struct net_ch *ch, void *buf, int ret);

typedef struct net_ch {
        list_head_t    ch_node;
        sockfd_t       fd;
        net_async_cb_f f_send_cb, f_recv_cb;
        net_mode_e     e_mode;
        u32            dst_ip, src_ip;
        u16            dst_port, src_port;
} net_ch_t;

typedef struct net_async_req {
        net_ch_t      *ch;
        void          *buf;
        usize          size;
        net_async_cb_f f_cb;
        ATOMIC(u8) processed;
#ifdef _WIN32
        OVERLAPPED  ov;
        u64         timeout_us;
        list_head_t pending_node; // 待处理请求链表节点
#endif
} net_async_req_t;

typedef u64 (*net_get_ts_f)(void);

typedef struct net_cfg {
        net_type_e   e_type;
        mempool_t   *mempool;
        u32          ring_len;
        log_cfg_t    log_cfg;
        net_get_ts_f f_get_ts;
} net_cfg_t;

typedef struct net_lo {
        list_head_t ch_root;
        log_t       log;
#ifdef __linux__
        struct io_uring ring;
#elif defined(_WIN32)
        HANDLE      iocp;
        list_head_t pending_reqs; // 待处理的异步请求列表
#endif
} net_lo_t;

typedef struct net {
        net_cfg_t cfg;
        net_lo_t  lo;
} net_t;

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
int net_init(net_t *net, net_cfg_t net_cfg);

/**
 * @brief 清理 net 相关 socket
 *
 * @param net net 结构体
 * @return    错误码
 */
void net_destroy(net_t *net);

/**
 * @brief 返回 net 通道
 *
 * @param det_ip    目标 IP
 * @param dst_port  目标端口
 * @param e_mode    收发模式
 * @return          net_ch_t
 */
net_ch_t net_cfg_ch(u32 det_ip, u16 dst_port, net_mode_e e_mode);

/**
 * @brief 添加 net 通道
 *
 * @param net net 结构体
 * @param ch  通道
 * @return    错误码
 */
int net_add_ch(net_t *net, net_ch_t *ch);

/**
 * @brief 同步发送
 *
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
isize net_sync_send(const net_ch_t *ch, const void *tx_buf, usize size);

/**
 * @brief 让出等待同步接收
 *
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
isize net_sync_recv_yield(const net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us);

/**
 * @brief 自旋等待同步接收
 *
 * @param ch         通道
 * @param rx_buf     接收缓冲区
 * @param cap        接收缓冲区容量字节数
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回接收的字节数, 失败返回错误码
 */
isize net_sync_recv_spin(const net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us);

/**
 * @brief 异步发送
 *
 * @param net    net 结构体
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
isize net_async_send(net_t *net, net_ch_t *ch, void *tx_buf, usize size);

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
isize net_async_recv(net_t *net, net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us);

/**
 * @brief 轮询处理异步请求
 *
 * @param net net 结构体
 * @return    错误码
 */
int net_poll(net_t *net);

/**
 * @brief 根据 net 配置自动选择模式发送
 *
 * @param net    net 结构体
 * @param ch     通道
 * @param tx_buf 发送缓冲区
 * @param size   发送字节数
 * @return       成功返回发送的字节数, 失败返回错误码
 */
isize net_send(net_t *net, net_ch_t *ch, void *tx_buf, usize size);

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
isize net_recv(net_t *net, net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us);

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
isize net_send_recv(net_t *net, net_ch_t *ch, void *tx_buf, usize size, void *rx_buf, usize cap, u32 timeout_us);

/**
 * @brief 向指定广播 IP 和端口发送并接收回复内容
 *
 * @param ip         广播 IP
 * @param port       广播端口
 * @param tx_buf     发送缓冲区
 * @param size       发送字节数
 * @param resps      回复内容数组
 * @param timeout_us 超时时间(微秒)
 * @return           成功返回回复的 IP 个数, 失败返回错误码
 */
int net_broadcast(u32 ip, u16 port, const void *tx_buf, usize size, net_resp_t *resps, u32 timeout_us);

#ifdef __cplusplus
}
#endif

#endif // !NET_H
