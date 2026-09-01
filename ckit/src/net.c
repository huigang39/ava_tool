#include "platdef.h"

#if !OS(NONE)

#include "net.h"

#include <limits.h>

#if OS(WIN)
enum net_win_req_state {
    NET_WIN_REQ_PENDING, // 请求尚未处理
    NET_WIN_REQ_TIMEOUT, // 已请求超时取消
    NET_WIN_REQ_IO_ERR,  // I/O 操作失败
};

/**
 * @brief                 收集一个 Windows 异步完成请求
 * @param[in,out] net     网络对象
 * @param[in]     wait_ms 完成端口等待时间, 单位: ms
 * @param[in,out] done    已完成请求链表
 * @return                已收集返回 1, 无事件返回 0, 失败返回负错误码
 */
static int net_win_collect(struct net *net,
                           DWORD wait_ms,
                           struct list_head *done);

/**
 * @brief                回调并释放已完成的 Windows 请求
 * @param[in,out] net    网络对象
 * @param[in,out] done   已完成请求链表
 * @param[in]     notify 是否调用完成回调
 */
static void net_win_finish(struct net *net,
                           struct list_head *done,
                           bool notify);

/**
 * @brief         检查通道是否已加入网络对象
 * @param[in] net 网络对象
 * @param[in] ch  待检查通道
 * @return        已加入返回 true, 否则返回 false
 */
static bool is_net_win_channel_registered(const struct net *net,
                                          const struct net_ch *ch);
#endif

int net_init(struct net *net, const struct net_cfg net_cfg)
{
    if (!net) {
        return MERR(MENULLPTR);
    }
    DECL(net, cfg, lo);

    memset(lo, 0, sizeof(*lo));
    *cfg = net_cfg;

    list_init(&lo->ch_root);

    int32_t ret = 0;
#if OS(LINUX)
    if (cfg->ring_len > 0U) {
        ret = io_uring_queue_init(cfg->ring_len, &lo->ring, 0);
        if (ret == 0) {
            lo->ring_initialized = true;
        } else {
            ret = MERR(MECREATE);
        }
    }
#elif OS(MAC)
    lo->kq = kqueue();
    if (lo->kq < 0) {
        ret = MERR(MESYSERR);
    }
    list_init(&lo->pending_reqs);
#elif OS(WIN)
    WSADATA wsaData;
    ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0) {
        return MERR(MESYSERR);
    }
    lo->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (lo->iocp == NULL) {
        WSACleanup();
        return MERR(MESYSERR);
    }
    list_init(&lo->pending_reqs);
#endif

    if (ret < 0) {
        net_destroy(net);
        return ret;
    }

    if (cfg->log_cfg.fd) {
        const int log_ret = log_init(&lo->log, cfg->log_cfg);
        if (log_ret < 0) {
            net_destroy(net);
            return log_ret;
        }
    }

    return ret;
}

void net_destroy(struct net *net)
{
    if (!net) {
        return;
    }
    DECL(net, cfg, lo);

    if (lo->log.lo.producers) {
        (void)log_deinit(&lo->log);
    }

    struct list_head *node; // 当前遍历节点
#if OS(WIN)
    while (!is_list_empty(&lo->ch_root)) {
        node = lo->ch_root.next;
        struct net_ch *ch = CONTAINER_OF(node, struct net_ch, ch_node);
        CLOSE_SOCKET(ch->fd);
        ch->fd = INVALID_SOCKET;
        list_del(&ch->ch_node);
    }
#else
    LIST_FOR_EACH(node, &lo->ch_root)
    {
        const struct net_ch *ch = CONTAINER_OF(node, struct net_ch, ch_node);
        CLOSE_SOCKET(ch->fd);
    }
#endif

#if OS(LINUX)
    if (lo->ring_initialized) {
        io_uring_queue_exit(&lo->ring);
        lo->ring_initialized = false;
    }
#elif OS(MAC)
    if (lo->kq >= 0) {
        close(lo->kq);
    }
#elif OS(WIN)
    if (lo->iocp) {
        // socket 关闭后, 收到完成通知再释放请求
        struct list_head done;
        list_init(&done);
        while (!is_list_empty(&lo->pending_reqs)) {
            const int ret = net_win_collect(net, INFINITE, &done);
            if (ret < 0) {
                lo->prev_err = ret;
                break;
            }
            net_win_finish(net, &done, false);
        }
        CloseHandle(lo->iocp);
        lo->iocp = NULL;
        WSACleanup();
    }
#endif
}

int net_set_nonblock(sockfd_t fd)
{
#if OS(POSIX)
    int32_t flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return MERR(MESYSERR);
    }

    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0 ? MOK : MERR(MESYSERR);
#elif OS(WIN)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? MOK : MERR(MESYSERR);
#endif
}

int net_prev_err(const struct net *net)
{
    return net ? net->lo.prev_err : MERR(MENULLPTR);
}

struct net_ch net_cfg_ch(const uint32_t destination_ip,
                         const uint16_t destination_port,
                         const enum net_mode mode)
{
    const struct net_ch ch = {
        .mode = mode,
        .destination_ip = destination_ip,
        .destination_port = destination_port,
    };
    return ch;
}

int net_add_ch(struct net *net, struct net_ch *ch)
{
    if (!net || !ch) {
        return MERR(MENULLPTR);
    }
    DECL(net, cfg, lo);

    if ((uint32_t)ch->mode > NET_MODE_ASYNC) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (!lo->iocp) {
        return MERR(MESTATE);
    }
    if (is_net_win_channel_registered(net, ch)) {
        return MERR(MEXIST);
    }
#endif

#if OS(LINUX)
    if (ch->mode == NET_MODE_ASYNC && !lo->ring_initialized) {
        return MERR(MESTATE);
    }
#endif

    switch (cfg->type) {
    case NET_TYPE_UDP: {
#if OS(POSIX)
        ch->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ch->fd < 0) {
            return MERR(MESYSERR);
        }
#elif OS(WIN)
        ch->fd = WSASocketW(
            AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (ch->fd == INVALID_SOCKET) {
            return MERR(MESYSERR);
        }
#endif
        break;
    }
    case NET_TYPE_TCP: {
#if OS(POSIX)
        ch->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ch->fd < 0) {
            return MERR(MESYSERR);
        }
#elif OS(WIN)
        ch->fd = WSASocketW(
            AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (ch->fd == INVALID_SOCKET) {
            return MERR(MESYSERR);
        }
#endif
        break;
    }
    default: {
        return MERR(MEINVAL);
    }
    }

    struct sockaddr_in destination_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ch->destination_port),
    };
    destination_addr.sin_addr.s_addr = ch->destination_ip;

    int32_t ret;
    if (ch->source_ip != 0 || ch->source_port != 0) {
        struct sockaddr_in src_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(ch->source_port),
        };
        src_addr.sin_addr.s_addr = ch->source_ip;

        ret = bind(ch->fd, (struct sockaddr *)&src_addr, sizeof(src_addr));
        if (ret < 0) {
            ret = MERR(MESYSERR);
            goto cleanup;
        }
    }

    ret = connect(
        ch->fd, (struct sockaddr *)&destination_addr, sizeof(destination_addr));
    if (ret < 0) {
        ret = MERR(MESYSERR);
        goto cleanup;
    }

    if (ch->mode == NET_MODE_ASYNC) {
        ret = net_set_nonblock(ch->fd);
        if (ret < 0) {
            goto cleanup;
        }
    }

#if OS(WIN)
    if (!CreateIoCompletionPort((HANDLE)ch->fd, lo->iocp, (ULONG_PTR)ch, 0)) {
        ret = MERR(MESYSERR);
        goto cleanup;
    }
#endif

    list_add(&ch->ch_node, &lo->ch_root);

    return 0;

cleanup:
    CLOSE_SOCKET(ch->fd);
    return ret;
}

ptrdiff_t net_sync_send(const struct net_ch *ch,
                        const void *tx_buf,
                        const size_t size)
{
    if (!ch || !tx_buf) {
        return MERR(MENULLPTR);
    }
    if (size == 0) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (size > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif
#if OS(POSIX)
    const ptrdiff_t ret = send(ch->fd, tx_buf, size, 0);
#elif OS(WIN)
    const ptrdiff_t ret = send(ch->fd, (const char *)tx_buf, (int)size, 0);
#endif
    return ret < 0 ? MERR(MESYSERR) : ret;
}

ptrdiff_t net_sync_recv_yield(const struct net_ch *ch,
                              void *rx_buf,
                              const size_t cap,
                              const uint32_t timeout_us)
{
    if (!ch || !rx_buf) {
        return MERR(MENULLPTR);
    }
    if (cap == 0) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (cap > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif
#if OS(POSIX)
    const struct timeval tv = {
        .tv_sec = (int32_t)US2S(timeout_us),
        .tv_usec = (int32_t)(timeout_us % 1000000),
    };
    if (setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return MERR(MESYSERR);
    }
    const ptrdiff_t ret = recv(ch->fd, rx_buf, cap, 0);
#elif OS(WIN)
    DWORD tv_ms = (DWORD)US2MS(timeout_us);
    if (tv_ms == 0 && timeout_us > 0) {
        tv_ms = 1;
    }

    if (setsockopt(ch->fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   (const char *)&tv_ms,
                   sizeof(tv_ms)) < 0) {
        return MERR(MESYSERR);
    }
    const ptrdiff_t ret = recv(ch->fd, (char *)rx_buf, (int32_t)cap, 0);
#endif
    if (ret >= 0) {
        return ret;
    }
#if OS(POSIX)
    return errno == EAGAIN || errno == EWOULDBLOCK ? MERR(METIMEOUT)
                                                   : MERR(MESYSERR);
#else
    return WSAGetLastError() == WSAETIMEDOUT ? MERR(METIMEOUT) : MERR(MESYSERR);
#endif
}

ptrdiff_t net_sync_recv_spin(const struct net_ch *ch,
                             void *rx_buf,
                             const size_t cap,
                             const uint32_t timeout_us)
{
    if (!ch || !rx_buf) {
        return MERR(MENULLPTR);
    }
    if (cap == 0) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (cap > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif
    const uint64_t start_ns = get_mono_ts_ns();
    uint64_t curr_ns = start_ns;
    do {
#if OS(POSIX)
        const int32_t ret = recv(ch->fd, rx_buf, cap, MSG_DONTWAIT);
#elif OS(WIN)
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ch->fd, &read_fds);
        struct timeval tv = {0, 0};
        const int32_t select_ret = select(0, &read_fds, NULL, NULL, &tv);
        int32_t ret = -1;
        if (select_ret > 0 && FD_ISSET(ch->fd, &read_fds)) {
            ret = recv(ch->fd, (char *)rx_buf, (int32_t)cap, 0);
            if (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK &&
                WSAGetLastError() != WSAEINTR) {
                return MERR(MESYSERR);
            }
        } else if (select_ret < 0) {
            return MERR(MESYSERR);
        }
#endif
        if (ret >= 0) {
            return ret;
        }
#if OS(POSIX)
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            return MERR(MESYSERR);
        }
#endif

        curr_ns = get_mono_ts_ns();
    } while (curr_ns - start_ns < US2NS((uint64_t)timeout_us));
    return MERR(METIMEOUT);
}

ptrdiff_t net_async_send(struct net *net,
                         struct net_ch *ch,
                         void *tx_buf,
                         size_t size)
{
    if (!net || !ch || !tx_buf) {
        return MERR(MENULLPTR);
    }
    if (!net->cfg.mempool) {
        return MERR(MESTATE);
    }
    if (size == 0) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (size > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct io_uring_sqe *send_sqe = io_uring_get_sqe(&lo->ring);
    if (!send_sqe) {
        return MERR(MEBUSY);
    }

    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    req->processed = 0;
    req->ch = ch;
    req->buf = tx_buf;
    req->size = size;
    req->cb = ch->send_cb;

    io_uring_prep_send(send_sqe, ch->fd, tx_buf, size, 0);
    io_uring_sqe_set_data(send_sqe, req);

    io_uring_submit(&lo->ring);
    return size;
#elif OS(MAC)
    /**
     * 遇到 EAGAIN
     * 时改用 EVFILT_WRITE
     */
    const ptrdiff_t n = send(ch->fd, tx_buf, size, 0);
    if (n >= 0) {
        if (ch->send_cb) {
            ch->send_cb(ch, tx_buf, (int32_t)n);
        }
        return n;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return MERR(MESYSERR);
    }

    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    req->processed = 0;
    req->ch = ch;
    req->buf = tx_buf;
    req->size = size;
    req->cb = ch->send_cb;
    req->operation = NET_OPERATION_SEND;
    req->timeout_us = 0;

    struct kevent kev;
    EV_SET(&kev, ch->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, req);
    if (kevent(lo->kq, &kev, 1, NULL, 0, NULL) < 0) {
        mempool_free(cfg->mempool, req);
        return MERR(MESYSERR);
    }
    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)size;
#elif OS(WIN)
    if (!lo->iocp || !is_net_win_channel_registered(net, ch)) {
        return MERR(MESTATE);
    }
    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    memset(req, 0, sizeof(*req));
    ATOMIC_STORE(&req->processed, 0);
    req->ch = ch;
    req->buf = tx_buf;
    req->size = size;
    req->cb = ch->send_cb;

    WSABUF buf;
    buf.buf = (CHAR *)tx_buf;
    buf.len = (ULONG)size;

    const int ret = WSASend(ch->fd, &buf, 1, NULL, 0, &req->ov, NULL);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        mempool_free(cfg->mempool, req);
        return MERR(MESYSERR);
    }

    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)size;
#endif
}

ptrdiff_t net_async_recv(struct net *net,
                         struct net_ch *ch,
                         void *rx_buf,
                         size_t cap,
                         uint32_t timeout_us)
{
    if (!net || !ch || !rx_buf) {
        return MERR(MENULLPTR);
    }
    if (!net->cfg.mempool) {
        return MERR(MESTATE);
    }
    if (cap == 0) {
        return MERR(MEINVAL);
    }
#if OS(WIN)
    if (cap > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
#endif
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    req->processed = 0;
    req->ch = ch;
    req->buf = rx_buf;
    req->size = cap;
    req->cb = ch->rx_cb;

    struct io_uring_sqe *recv_sqe = io_uring_get_sqe(&lo->ring);
    if (!recv_sqe) {
        mempool_free(cfg->mempool, req);
        return MERR(MEBUSY);
    }

    io_uring_prep_recv(recv_sqe, ch->fd, rx_buf, cap, 0);
    io_uring_sqe_set_data(recv_sqe, req);
    io_uring_sqe_set_flags(recv_sqe, IOSQE_IO_LINK);

    struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&lo->ring);
    if (!timeout_sqe) {
        mempool_free(cfg->mempool, req);
        return MERR(MEBUSY);
    }
    struct __kernel_timespec ts = {
        .tv_sec = (int)US2S(timeout_us),
        .tv_nsec = (int)US2NS(timeout_us % 1000000),
    };
    io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
    io_uring_sqe_set_data(timeout_sqe, req);

    const int submit_ret = io_uring_submit(&lo->ring);
    return submit_ret < 0 ? MERR(MESYSERR) : submit_ret;
#elif OS(MAC)
    // fd 已由 net_add_ch 设为非阻塞
    const ptrdiff_t n = recv(ch->fd, rx_buf, cap, 0);
    if (n > 0) {
        if (ch->rx_cb) {
            ch->rx_cb(ch, rx_buf, (int32_t)n);
        }
        return n;
    }
    if (n == 0) {
        return 0;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return MERR(MESYSERR);
    }

    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    req->processed = 0;
    req->ch = ch;
    req->buf = rx_buf;
    req->size = cap;
    req->cb = ch->rx_cb;
    req->operation = NET_OPERATION_RX;
    req->timeout_us = timeout_us > 0 ? get_mono_ts_us() + timeout_us : 0;

    struct kevent kev;
    EV_SET(&kev, ch->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, req);
    if (kevent(lo->kq, &kev, 1, NULL, 0, NULL) < 0) {
        mempool_free(cfg->mempool, req);
        return MERR(MESYSERR);
    }
    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)cap;
#elif OS(WIN)
    if (!lo->iocp || !is_net_win_channel_registered(net, ch)) {
        return MERR(MESTATE);
    }
    struct net_async_req *req = (struct net_async_req *)mempool_alloc(
        cfg->mempool, sizeof(struct net_async_req));
    if (!req) {
        return MERR(MEALLOC);
    }

    memset(req, 0, sizeof(*req));
    ATOMIC_STORE(&req->processed, 0);
    req->ch = ch;
    req->buf = rx_buf;
    req->size = cap;
    req->cb = ch->rx_cb;
    req->timeout_us = timeout_us > 0 ? get_mono_ts_us() + timeout_us : 0;

    WSABUF buf;
    buf.buf = (CHAR *)rx_buf;
    buf.len = (ULONG)cap;

    DWORD flags = 0;
    const int ret = WSARecv(ch->fd, &buf, 1, NULL, &flags, &req->ov, NULL);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        mempool_free(cfg->mempool, req);
        return MERR(MESYSERR);
    }

    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)cap;
#endif
}

int net_poll(struct net *net)
{
    if (!net) {
        return MERR(MENULLPTR);
    }
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&lo->ring, &cqe) == 0) {
        struct net_async_req *req =
            (struct net_async_req *)io_uring_cqe_get_data(cqe);
        if (!req) {
            io_uring_cqe_seen(&lo->ring, cqe);
            continue;
        }

        if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
            if (req->cb) {
                req->cb(req->ch,
                        req->buf,
                        cqe->res == -ETIME
                            ? MERR(METIMEOUT)
                            : (cqe->res < 0 ? MERR(MEIO) : cqe->res));
            }
            mempool_free(cfg->mempool, req);
        }

        io_uring_cqe_seen(&lo->ring, cqe);
    }
    return 0;
#elif OS(MAC)
    const uint64_t curr_ts = get_mono_ts_us();
    struct list_head *node; // 当前遍历节点
    struct list_head *next; // 预先保存的下一节点

    struct list_head done_list;
    list_init(&done_list);
    for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
        next = node->next;
        struct net_async_req *req =
            CONTAINER_OF(node, struct net_async_req, pending_node);
        if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
            if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
                struct kevent kev;
                const int16_t filter = (req->operation == NET_OPERATION_SEND)
                                           ? EVFILT_WRITE
                                           : EVFILT_READ;
                EV_SET(&kev, req->ch->fd, filter, EV_DELETE, 0, 0, NULL);
                kevent(lo->kq, &kev, 1, NULL, 0, NULL);
                if (req->cb) {
                    req->cb(req->ch, req->buf, -METIMEOUT);
                }
                list_del(&req->pending_node);
                list_add_tail(&req->pending_node, &done_list);
            }
        }
    }

    // 按最近的超时设置等待时间
    struct timespec ts = {0, 0};
    if (!is_list_empty(&lo->pending_reqs)) {
        uint64_t min_deadline = (uint64_t)-1;
        LIST_FOR_EACH(node, &lo->pending_reqs)
        {
            const struct net_async_req *r =
                CONTAINER_OF(node, struct net_async_req, pending_node);
            if (r->timeout_us > 0 && r->timeout_us < min_deadline) {
                min_deadline = r->timeout_us;
            }
        }
        if (min_deadline != (uint64_t)-1 && min_deadline > curr_ts) {
            const uint64_t wait_us = min_deadline - curr_ts;
            ts.tv_sec = (time_t)(wait_us / 1000000);
            ts.tv_nsec = (long)((wait_us % 1000000) * 1000);
        }
    }

    struct kevent events[16];
    const int32_t n = kevent(lo->kq, NULL, 0, events, 16, &ts);

    // 超时请求保持有效, 本轮事件处理后再释放
    for (int32_t i = 0; i < n; ++i) {
        struct net_async_req *req = (struct net_async_req *)events[i].udata;
        if (!req) {
            continue;
        }

        if (ATOMIC_EXCHANGE(&req->processed, true) != 0) {
            continue; // 已在超时循环中处理
        }

        ptrdiff_t ret;
        if (req->operation == NET_OPERATION_SEND) {
            ret = send(req->ch->fd, req->buf, req->size, 0);
        } else {
            ret = recv(req->ch->fd, req->buf, req->size, 0);
        }

        if (events[i].flags & EV_ERROR) {
            ret = -1;
        }

        if (req->cb) {
            req->cb(req->ch, req->buf, (int32_t)ret);
        }

        list_del(&req->pending_node);
        mempool_free(cfg->mempool, req);
    }

    // 本轮事件处理后释放超时请求
    for (node = done_list.next; node != &done_list; node = next) {
        next = node->next;
        struct net_async_req *req =
            CONTAINER_OF(node, struct net_async_req, pending_node);
        mempool_free(cfg->mempool, req);
    }
    return 0;
#elif OS(WIN)
    if (!lo->iocp) {
        return MERR(MESTATE);
    }
    struct list_head done;
    list_init(&done);
    int ret;
    while ((ret = net_win_collect(net, 0, &done)) > 0) {
    }
    if (ret < 0) {
        net_win_finish(net, &done, true);
        return ret;
    }

    const uint64_t curr_ts = get_mono_ts_us();
    struct list_head *node;
    LIST_FOR_EACH(node, &lo->pending_reqs)
    {
        struct net_async_req *req =
            CONTAINER_OF(node, struct net_async_req, pending_node);
        if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
            ATOMIC_STORE(&req->processed, NET_WIN_REQ_TIMEOUT);
            req->timeout_us = 0;
            // 取消只发起请求, 完成通知到达后才能回调并释放缓冲区
            CancelIoEx((HANDLE)req->ch->fd, &req->ov);
        }
    }

    if (is_list_empty(&done)) {
        DWORD timeout_ms = 0;
        LIST_FOR_EACH(node, &lo->pending_reqs)
        {
            const struct net_async_req *req =
                CONTAINER_OF(node, struct net_async_req, pending_node);
            if (req->timeout_us > curr_ts) {
                const DWORD wait_ms =
                    (DWORD)((req->timeout_us - curr_ts + 999U) / 1000U);
                if (timeout_ms == 0 || wait_ms < timeout_ms) {
                    timeout_ms = wait_ms;
                }
            }
        }
        ret = net_win_collect(net, timeout_ms, &done);
    }
    net_win_finish(net, &done, true);
    return ret < 0 ? ret : 0;
#endif
}

ptrdiff_t net_send(struct net *net,
                   struct net_ch *ch,
                   void *tx_buf,
                   const size_t size)
{
    if (!net || !ch || !tx_buf) {
        return MERR(MENULLPTR);
    }
    if (size == 0) {
        return MERR(MEINVAL);
    }
    DECL(net, cfg, lo);

    RENAME(&lo->log, log);

    ptrdiff_t tx_size;
    switch (ch->mode) {
    case NET_MODE_SYNC_SPIN: {
    }
    case NET_MODE_SYNC_YIELD: {
        tx_size = net_sync_send(ch, tx_buf, size);
        if (tx_size > 0 && cfg->log_cfg.fd) {
            lo->prev_err = log_data(log,
                                    0,
                                    "[SEND] %u.%u.%u.%u:%u %d bytes\n",
                                    ch->destination_ip & 0xFF,
                                    (ch->destination_ip >> 8) & 0xFF,
                                    (ch->destination_ip >> 16) & 0xFF,
                                    (ch->destination_ip >> 24) & 0xFF,
                                    ch->destination_port,
                                    (int)tx_size);
        }
        break;
    }
    case NET_MODE_ASYNC: {
        tx_size = net_async_send(net, ch, tx_buf, size);
        break;
    }
    default: {
        return MERR(MEINVAL);
    }
    }

    return tx_size;
}

ptrdiff_t net_recv(struct net *net,
                   struct net_ch *ch,
                   void *rx_buf,
                   const size_t cap,
                   const uint32_t timeout_us)
{
    if (!net || !ch || !rx_buf) {
        return MERR(MENULLPTR);
    }
    if (cap == 0) {
        return MERR(MEINVAL);
    }
    DECL(net, cfg, lo);

    RENAME(&lo->log, log);

    ptrdiff_t rx_size;
    switch (ch->mode) {
    case NET_MODE_SYNC_YIELD: {
        rx_size = net_sync_recv_yield(ch, rx_buf, cap, timeout_us);
        if (rx_size > 0 && cfg->log_cfg.fd) {
            lo->prev_err = log_data(log,
                                    0,
                                    "[RECV] %u.%u.%u.%u:%u %d bytes\n",
                                    ch->destination_ip & 0xFF,
                                    (ch->destination_ip >> 8) & 0xFF,
                                    (ch->destination_ip >> 16) & 0xFF,
                                    (ch->destination_ip >> 24) & 0xFF,
                                    ch->destination_port,
                                    (int)rx_size);
        }
        break;
    }
    case NET_MODE_SYNC_SPIN: {
        rx_size = net_sync_recv_spin(ch, rx_buf, cap, timeout_us);
        if (rx_size > 0 && cfg->log_cfg.fd) {
            lo->prev_err = log_data(log,
                                    0,
                                    "[RECV] %u.%u.%u.%u:%u %d bytes\n",
                                    ch->destination_ip & 0xFF,
                                    (ch->destination_ip >> 8) & 0xFF,
                                    (ch->destination_ip >> 16) & 0xFF,
                                    (ch->destination_ip >> 24) & 0xFF,
                                    ch->destination_port,
                                    (int)rx_size);
        }
        break;
    }
    case NET_MODE_ASYNC: {
        rx_size = net_async_recv(net, ch, rx_buf, cap, timeout_us);
        break;
    }
    default: {
        return MERR(MEINVAL);
    }
    }

    return rx_size;
}

ptrdiff_t net_send_recv(struct net *net,
                        struct net_ch *ch,
                        void *tx_buf,
                        const size_t size,
                        void *rx_buf,
                        const size_t cap,
                        const uint32_t timeout_us)
{
    const ptrdiff_t ret = net_send(net, ch, tx_buf, size);
    if (ret <= 0) {
        return ret;
    }

    return net_recv(net, ch, rx_buf, cap, timeout_us);
}

int net_broadcast(const uint32_t ip,
                  const uint16_t port,
                  const void *tx_buf,
                  const size_t size,
                  struct net_rsp *rsp,
                  const size_t rsp_cap,
                  const uint32_t timeout_us)
{
    sockfd_t fd;
    struct sockaddr_in src_addr;
    socklen_t addr_len;
    uint64_t start_ts;
    int32_t rsp_cnt = 0;
    int32_t ret;
    const int opt = 1;

    if (!tx_buf || size == 0 || !rsp || rsp_cap == 0) {
        return MERR(MEINVAL);
    }
    if (size > INT_MAX || rsp_cap > INT_MAX) {
        return MERR(MEOVERFLOW);
    }

#if OS(POSIX)
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#elif OS(WIN)
    fd = WSASocketW(
        AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#endif

#if OS(POSIX)
    if (fd < 0)
#elif OS(WIN)
    if (fd == INVALID_SOCKET)
#endif
    {
        return MERR(MESYSERR);
    }

    if (setsockopt(
            fd, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt)) <
        0) {
        ret = MERR(MESYSERR);
        goto cleanup;
    }
    ret = net_set_nonblock(fd);
    if (ret < 0) {
        goto cleanup;
    }

    struct sockaddr_in destination_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    destination_addr.sin_addr.s_addr = ip;

    addr_len = sizeof(destination_addr);
    ret = sendto(fd,
                 (const char *)tx_buf,
                 (int32_t)size,
                 0,
                 (struct sockaddr *)&destination_addr,
                 addr_len);
    if (ret <= 0) {
        ret = MERR(MESYSERR);
        goto cleanup;
    }

    addr_len = sizeof(src_addr);
    start_ts = get_mono_ts_us();
    do {
        if ((size_t)rsp_cnt >= rsp_cap) {
            break;
        }
        ret = recvfrom(fd,
                       (char *)rsp[rsp_cnt].buf,
                       sizeof(rsp[rsp_cnt].buf),
                       0,
                       (struct sockaddr *)&src_addr,
                       &addr_len);
        if (ret > 0) {
            IP_U32_TO_STR(
                &src_addr.sin_addr, rsp[rsp_cnt].ip, sizeof(rsp[rsp_cnt].ip));
            rsp_cnt++;
        }
    } while (get_mono_ts_us() - start_ts < timeout_us);

    ret = rsp_cnt;

cleanup:
    CLOSE_SOCKET(fd);
    return ret;
}

#if OS(WIN)
static int net_win_collect(struct net *net,
                           const DWORD wait_ms,
                           struct list_head *done)
{
    DWORD size = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;
    const BOOL ok =
        GetQueuedCompletionStatus(net->lo.iocp, &size, &key, &ov, wait_ms);
    if (!ov) {
        return ok || GetLastError() == WAIT_TIMEOUT ? 0 : MERR(MESYSERR);
    }

    struct net_async_req *req = CONTAINER_OF(ov, struct net_async_req, ov);
    list_del(&req->pending_node);
    req->size = size;
    if (!ok && ATOMIC_LOAD(&req->processed) == NET_WIN_REQ_PENDING) {
        ATOMIC_STORE(&req->processed, NET_WIN_REQ_IO_ERR);
    }
    list_add_tail(&req->pending_node, done);
    return 1;
}

static void net_win_finish(struct net *net,
                           struct list_head *done,
                           const bool notify)
{
    while (!is_list_empty(done)) {
        struct net_async_req *req =
            CONTAINER_OF(done->next, struct net_async_req, pending_node);
        list_del(&req->pending_node);
        if (notify && req->cb) {
            const uint8_t state = ATOMIC_LOAD(&req->processed);
            const int ret =
                state == NET_WIN_REQ_TIMEOUT
                    ? MERR(METIMEOUT)
                    : (state == NET_WIN_REQ_IO_ERR ? MERR(MEIO)
                                                   : (int)req->size);
            req->cb(req->ch, req->buf, ret);
        }
        mempool_free(net->cfg.mempool, req);
    }
}

static bool is_net_win_channel_registered(const struct net *net,
                                          const struct net_ch *ch)
{
    struct list_head *node;
    LIST_FOR_EACH(node, &net->lo.ch_root)
    {
        if (CONTAINER_OF(node, struct net_ch, ch_node) == ch) {
            return true;
        }
    }
    return false;
}
#endif

#endif
