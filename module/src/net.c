#include "platdef.h"

#if !OS(NONE)

#include "net.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
net_init(struct net *net, const struct net_cfg net_cfg)
{
    DECL(net, cfg, lo);

    memset(lo, 0, sizeof(*lo));
    *cfg = net_cfg;

    list_init(&lo->ch_root);

    int ret = 0;
#if OS(LINUX)
    if (cfg->ring_len > 0U) {
        ret = io_uring_queue_init(cfg->ring_len, &lo->ring, 0);
        if (ret == 0) {
            lo->ring_initialized = true;
        }
    }
#elif OS(MAC)
    lo->kq = kqueue();
    if (lo->kq < 0)
        ret = -1;
    list_init(&lo->pending_reqs);
#elif OS(WIN)
    WSADATA wsaData;
    ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
        return -MESYSERR;
    lo->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (lo->iocp == NULL) {
        WSACleanup();
        return -MESYSERR;
    }
    list_init(&lo->pending_reqs);
#endif

    if (cfg->log_cfg.fd)
        log_init(&lo->log, cfg->log_cfg);

    return ret;
}

void
net_destroy(struct net *net)
{
    DECL(net, cfg, lo);

    struct list_head *node;
    LIST_FOR_EACH(node, &lo->ch_root)
    {
        const struct net_ch *ch = CONTAINER_OF(node, struct net_ch, ch_node);
        CLOSE_SOCKET(ch->fd);
    }

#if OS(LINUX)
    if (lo->ring_initialized) {
        io_uring_queue_exit(&lo->ring);
        lo->ring_initialized = false;
    }
#elif OS(MAC)
    if (lo->kq >= 0)
        close(lo->kq);
#elif OS(WIN)
    WSACleanup();
#endif
}

int
net_set_nonblock(sockfd_t fd)
{
#if OS(POSIX)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return flags;

    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#elif OS(WIN)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#endif
}

struct net_ch
net_cfg_ch(const uint32_t det_ip, const uint16_t dst_port, const enum net_mode e_mode)
{
    const struct net_ch ch = {
        .e_mode   = e_mode,
        .dst_ip   = det_ip,
        .dst_port = dst_port,
    };
    return ch;
}

int
net_add_ch(struct net *net, struct net_ch *ch)
{
    DECL(net, cfg, lo);

#if OS(LINUX)
    if (ch->e_mode == NET_MODE_ASYNC && !lo->ring_initialized) {
        return -MEINVAL;
    }
#endif

    switch (cfg->e_type) {
        case NET_TYPE_UDP: {
#if OS(POSIX)
            ch->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (ch->fd < 0)
                return -MESYSERR;
#elif OS(WIN)
            ch->fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (ch->fd == INVALID_SOCKET)
                return -MESYSERR;
#endif
            break;
        }
        case NET_TYPE_TCP: {
#if OS(POSIX)
            ch->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (ch->fd < 0)
                return -MESYSERR;
#elif OS(WIN)
            ch->fd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (ch->fd == INVALID_SOCKET)
                return -MESYSERR;
#endif
            break;
        }
        default:
            return -MEINVAL;
    }

    struct sockaddr_in dst_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(ch->dst_port),
    };
    dst_addr.sin_addr.s_addr = ch->dst_ip;

    int ret;
    if (ch->src_ip != 0 && ch->src_port != 0) {
        struct sockaddr_in src_addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(ch->src_port),
        };
        src_addr.sin_addr.s_addr = ch->src_ip;

        ret = bind(ch->fd, (struct sockaddr *)&src_addr, sizeof(src_addr));
        if (ret < 0)
            goto cleanup;
    }

    ret = connect(ch->fd, (struct sockaddr *)&dst_addr, sizeof(dst_addr));
    if (ret < 0)
        goto cleanup;

    if (ch->e_mode == NET_MODE_ASYNC)
        net_set_nonblock(ch->fd);

    list_add(&ch->ch_node, &lo->ch_root);

#if OS(WIN)
    CreateIoCompletionPort((HANDLE)ch->fd, lo->iocp, (ULONG_PTR)ch, 0);
#endif

    return 0;

cleanup:
    CLOSE_SOCKET(ch->fd);
    return ret;
}

ptrdiff_t
net_sync_send(const struct net_ch *ch, const void *tx_buf, const size_t size)
{
#if OS(POSIX)
    return send(ch->fd, tx_buf, size, 0);
#elif OS(WIN)
    return send(ch->fd, (const char *)tx_buf, (int)size, 0);
#endif
}

ptrdiff_t
net_sync_recv_yield(const struct net_ch *ch,
                    void                *rx_buf,
                    const size_t         cap,
                    const uint32_t       timeout_us)
{
#if OS(POSIX)
    const struct timeval tv = {
        .tv_sec  = (int32_t)US2S(timeout_us),
        .tv_usec = (int32_t)(timeout_us % 1000000),
    };
    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(ch->fd, rx_buf, cap, 0);
#elif OS(WIN)
    DWORD tv_ms = (DWORD)US2MS(timeout_us);
    if (tv_ms == 0 && timeout_us > 0)
        tv_ms = 1;

    setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
    return recv(ch->fd, (char *)rx_buf, (int32_t)cap, 0);
#endif
}

ptrdiff_t
net_sync_recv_spin(const struct net_ch *ch,
                   void                *rx_buf,
                   const size_t         cap,
                   const uint32_t       timeout_us)
{
    const uint64_t start_ns = get_mono_ts_ns();
    uint64_t       curr_ns  = 0;
    while (curr_ns < start_ns + US2NS(timeout_us)) {
#if OS(POSIX)
        const ptrdiff_t ret = recv(ch->fd, rx_buf, cap, MSG_DONTWAIT);
#elif OS(WIN)
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ch->fd, &read_fds);
        struct timeval tv         = {0, 0};
        const int select_ret = select(0, &read_fds, NULL, NULL, &tv);
        int       ret        = -1;
        if (select_ret > 0 && FD_ISSET(ch->fd, &read_fds))
            ret = recv(ch->fd, (char *)rx_buf, (int32_t)cap, 0);
        else if (select_ret < 0)
            return -MESYSERR;
#endif
        if (ret > 0)
            return ret;

        curr_ns = get_mono_ts_ns();
    }
    return -METIMEOUT;
}

ptrdiff_t
net_async_send(struct net *net, struct net_ch *ch, void *tx_buf, size_t size)
{
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct io_uring_sqe *send_sqe = io_uring_get_sqe(&lo->ring);
    if (!send_sqe)
        return -1;

    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed = 0;
    req->ch        = ch;
    req->buf       = tx_buf;
    req->size      = size;
    req->f_cb      = ch->f_send_cb;

    io_uring_prep_send(send_sqe, ch->fd, tx_buf, size, 0);
    io_uring_sqe_set_data(send_sqe, req);

    io_uring_submit(&lo->ring);
    return size;
#elif OS(MAC)
    /* macOS: 先尝试非阻塞发送(fd 已由 net_add_ch 设为非阻塞),
     * 遇到 EAGAIN
     * 时改用 EVFILT_WRITE.
     */
    const ptrdiff_t n = send(ch->fd, tx_buf, size, 0);
    if (n >= 0) {
        if (ch->f_send_cb)
            ch->f_send_cb(ch, tx_buf, (int32_t)n);
        return n;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return -1;

    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed  = 0;
    req->ch         = ch;
    req->buf        = tx_buf;
    req->size       = size;
    req->f_cb       = ch->f_send_cb;
    req->e_op       = NET_OP_SEND;
    req->timeout_us = 0;

    struct kevent kev;
    EV_SET(&kev, ch->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, req);
    if (kevent(lo->kq, &kev, 1, NULL, 0, NULL) < 0) {
        mempool_free(cfg->mempool, req);
        return -1;
    }
    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)size;
#elif OS(WIN)
    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed = 0;
    req->ch        = ch;
    req->buf       = tx_buf;
    req->size      = size;
    req->f_cb      = ch->f_send_cb;

    WSABUF buf;
    buf.buf = (CHAR *)tx_buf;
    buf.len = (ULONG)size;

    DWORD         tx_size;
    const int ret = WSASend(ch->fd, &buf, 1, &tx_size, 0, &req->ov, NULL);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        mempool_free(cfg->mempool, req);
        return -1;
    }

    req->ov.Pointer = req;
    return (ptrdiff_t)tx_size;
#endif
}

ptrdiff_t
net_async_recv(struct net *net, struct net_ch *ch, void *rx_buf, size_t cap, uint32_t timeout_us)
{
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed = 0;
    req->ch        = ch;
    req->buf       = rx_buf;
    req->size      = cap;
    req->f_cb      = ch->f_recv_cb;

    struct io_uring_sqe *recv_sqe = io_uring_get_sqe(&lo->ring);
    if (!recv_sqe) {
        mempool_free(cfg->mempool, req);
        return -1;
    }

    io_uring_prep_recv(recv_sqe, ch->fd, rx_buf, cap, 0);
    io_uring_sqe_set_data(recv_sqe, req);
    io_uring_sqe_set_flags(recv_sqe, IOSQE_IO_LINK);

    struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&lo->ring);
    if (!timeout_sqe) {
        mempool_free(cfg->mempool, req);
        return -1;
    }
    struct __kernel_timespec ts = {
        .tv_sec  = (int)US2S(timeout_us),
        .tv_nsec = (int)US2NS(timeout_us % 1000000),
    };
    io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
    io_uring_sqe_set_data(timeout_sqe, req);

    return io_uring_submit(&lo->ring);
#elif OS(MAC)
    /* fd 已由 net_add_ch 设为非阻塞. */
    const ptrdiff_t n = recv(ch->fd, rx_buf, cap, 0);
    if (n > 0) {
        if (ch->f_recv_cb)
            ch->f_recv_cb(ch, rx_buf, (int32_t)n);
        return n;
    }
    if (n == 0)
        return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return -1;

    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed  = 0;
    req->ch         = ch;
    req->buf        = rx_buf;
    req->size       = cap;
    req->f_cb       = ch->f_recv_cb;
    req->e_op       = NET_OP_RECV;
    req->timeout_us = timeout_us > 0 ? get_mono_ts_us() + timeout_us : 0;

    struct kevent kev;
    EV_SET(&kev, ch->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, req);
    if (kevent(lo->kq, &kev, 1, NULL, 0, NULL) < 0) {
        mempool_free(cfg->mempool, req);
        return -1;
    }
    list_add_tail(&req->pending_node, &lo->pending_reqs);
    return (ptrdiff_t)cap;
#elif OS(WIN)
    struct net_async_req *req =
        (struct net_async_req *)mempool_alloc(cfg->mempool, sizeof(struct net_async_req));
    if (!req)
        return -MEALLOC;

    req->processed  = 0;
    req->ch         = ch;
    req->buf        = rx_buf;
    req->size       = cap;
    req->f_cb       = ch->f_recv_cb;
    req->timeout_us = timeout_us > 0 ? get_mono_ts_us() + timeout_us : 0;

    WSABUF buf;
    buf.buf = (CHAR *)rx_buf;
    buf.len = (ULONG)cap;

    DWORD         flags = 0;
    DWORD         rx_size;
    const int ret = WSARecv(ch->fd, &buf, 1, &rx_size, &flags, &req->ov, NULL);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        mempool_free(cfg->mempool, req);
        return -1;
    }

    req->ov.Pointer = req;
    if (timeout_us > 0)
        list_add_tail(&req->pending_node, &lo->pending_reqs);

    return (ptrdiff_t)rx_size;
#endif
}

int
net_poll(struct net *net)
{
    DECL(net, cfg, lo);

#if OS(LINUX)
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&lo->ring, &cqe) == 0) {
        struct net_async_req *req = (struct net_async_req *)io_uring_cqe_get_data(cqe);
        if (!req) {
            io_uring_cqe_seen(&lo->ring, cqe);
            continue;
        }

        if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
            if (req->f_cb)
                req->f_cb(req->ch, req->buf, cqe->res == -ETIME ? -METIMEOUT : cqe->res);
            mempool_free(cfg->mempool, req);
        }

        io_uring_cqe_seen(&lo->ring, cqe);
    }
    return 0;
#elif OS(MAC)
    const uint64_t    curr_ts = get_mono_ts_us();
    struct list_head *node, *next;

    /* 阶段 1: 扫描超时请求, 标记并执行 EV_DELETE 和回调, 随后移入 done_list.
     *
     * 此处不能释放; 若内核已为同一请求排队事件, 提前释放会导致 kevent 访问无效内存. */
    struct list_head done_list;
    list_init(&done_list);
    for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
        next                      = node->next;
        struct net_async_req *req = CONTAINER_OF(node, struct net_async_req, pending_node);
        if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
            if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
                struct kevent kev;
                const int16_t filter = (req->e_op == NET_OP_SEND) ? EVFILT_WRITE : EVFILT_READ;
                EV_SET(&kev, req->ch->fd, filter, EV_DELETE, 0, 0, NULL);
                kevent(lo->kq, &kev, 1, NULL, 0, NULL);
                if (req->f_cb)
                    req->f_cb(req->ch, req->buf, -METIMEOUT);
                list_del(&req->pending_node);
                list_add_tail(&req->pending_node, &done_list);
            }
        }
    }

    /* 阶段 2: 计算 kevent 超时; 无待处理请求时使用 ts={0,0},
     * 确保
     * net_poll 立即返回而非永久阻塞.
     */
    struct timespec ts = {0, 0};
    if (!list_empty(&lo->pending_reqs)) {
        uint64_t min_deadline = (uint64_t)-1;
        LIST_FOR_EACH(node, &lo->pending_reqs)
        {
            const struct net_async_req *r = CONTAINER_OF(node, struct net_async_req, pending_node);
            if (r->timeout_us > 0 && r->timeout_us < min_deadline)
                min_deadline = r->timeout_us;
        }
        if (min_deadline != (uint64_t)-1 && min_deadline > curr_ts) {
            const uint64_t wait_us = min_deadline - curr_ts;
            ts.tv_sec              = (time_t)(wait_us / 1000000);
            ts.tv_nsec             = (long)((wait_us % 1000000) * 1000);
        }
    }

    /* 阶段 3: 收集就绪事件. */
    struct kevent events[16];
    const int n = kevent(lo->kq, NULL, 0, events, 16, &ts);

    /* 阶段 4: 处理事件; 通过 processed 标志跳过已超时请求.
     * 这些请求已在 done_list
     * 中, 不再访问其 pending_node.
     */
    for (int i = 0; i < n; ++i) {
        struct net_async_req *req = (struct net_async_req *)events[i].udata;
        if (!req)
            continue;

        if (ATOMIC_EXCHANGE(&req->processed, true) != 0)
            continue; /* 已在超时循环中处理. */

        ptrdiff_t ret;
        if (req->e_op == NET_OP_SEND)
            ret = send(req->ch->fd, req->buf, req->size, 0);
        else
            ret = recv(req->ch->fd, req->buf, req->size, 0);

        if (events[i].flags & EV_ERROR)
            ret = -1;

        if (req->f_cb)
            req->f_cb(req->ch, req->buf, (int32_t)ret);

        list_del(&req->pending_node);
        mempool_free(cfg->mempool, req);
    }

    /* 阶段 5: kevent 完成后释放超时请求. */
    for (node = done_list.next; node != &done_list; node = next) {
        next                      = node->next;
        struct net_async_req *req = CONTAINER_OF(node, struct net_async_req, pending_node);
        mempool_free(cfg->mempool, req);
    }
    return 0;
#elif OS(WIN)
    DWORD       size;
    ULONG_PTR   key;
    OVERLAPPED *ov = NULL;

    const uint64_t    curr_ts = get_mono_ts_us();
    struct list_head *node, *next;
    for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
        next                      = node->next;
        struct net_async_req *req = CONTAINER_OF(node, struct net_async_req, pending_node);
        if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
            if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
                CancelIoEx((HANDLE)req->ch->fd, &req->ov);
                if (req->f_cb)
                    req->f_cb(req->ch, req->buf, -METIMEOUT);
                list_del(&req->pending_node);
                mempool_free(cfg->mempool, req);
            }
        }
    }

    DWORD timeout_ms = 0;
    if (!list_empty(&lo->pending_reqs)) {
        uint64_t min_timeout_us = (uint64_t)-1;
        LIST_FOR_EACH(node, &lo->pending_reqs)
        {
            const struct net_async_req *req =
                CONTAINER_OF(node, struct net_async_req, pending_node);
            if (req->timeout_us > 0 && req->timeout_us < min_timeout_us)
                min_timeout_us = req->timeout_us;
        }
        if (min_timeout_us != (uint64_t)-1 && min_timeout_us > curr_ts) {
            timeout_ms = (DWORD)US2MS(min_timeout_us - curr_ts);
            if (timeout_ms == 0)
                timeout_ms = 1;
        }
    }

    const BOOL ok = GetQueuedCompletionStatus(lo->iocp, &size, &key, &ov, timeout_ms);
    if (!ok && ov == NULL)
        return 0;

    struct net_async_req *req = (struct net_async_req *)ov->Pointer;
    if (!req)
        return 0;

    if (req->timeout_us > 0)
        list_del(&req->pending_node);

    if (ATOMIC_EXCHANGE(&req->processed, true) == 0) {
        if (req->f_cb)
            req->f_cb(req->ch, req->buf, ok ? (int32_t)size : -1);
        mempool_free(cfg->mempool, req);
    }

    return 0;
#endif
}

ptrdiff_t
net_send(struct net *net, struct net_ch *ch, void *tx_buf, const size_t size)
{
    DECL(net, cfg, lo);
    RENAME(&lo->log, log);

    ptrdiff_t tx_size;
    switch (ch->e_mode) {
        case NET_MODE_SYNC_SPIN:
        case NET_MODE_SYNC_YIELD: {
            tx_size = net_sync_send(ch, tx_buf, size);
            if (tx_size > 0 && cfg->log_cfg.fd)
                log_data(log,
                         0,
                         "[SEND] %u.%u.%u.%u:%u %d bytes\n",
                         ch->dst_ip & 0xFF,
                         (ch->dst_ip >> 8) & 0xFF,
                         (ch->dst_ip >> 16) & 0xFF,
                         (ch->dst_ip >> 24) & 0xFF,
                         ch->dst_port,
                         (int)tx_size);
            break;
        }
        case NET_MODE_ASYNC: {
            tx_size = net_async_send(net, ch, tx_buf, size);
            break;
        }
        default:
            return -MEINVAL;
    }

    return tx_size;
}

ptrdiff_t
net_recv(
    struct net *net, struct net_ch *ch, void *rx_buf, const size_t cap, const uint32_t timeout_us)
{
    DECL(net, cfg, lo);
    RENAME(&lo->log, log);

    ptrdiff_t rx_size;
    switch (ch->e_mode) {
        case NET_MODE_SYNC_YIELD: {
            rx_size = net_sync_recv_yield(ch, rx_buf, cap, timeout_us);
            if (rx_size > 0 && cfg->log_cfg.fd)
                log_data(log,
                         0,
                         "[RECV] %u.%u.%u.%u:%u %d bytes\n",
                         ch->dst_ip & 0xFF,
                         (ch->dst_ip >> 8) & 0xFF,
                         (ch->dst_ip >> 16) & 0xFF,
                         (ch->dst_ip >> 24) & 0xFF,
                         ch->dst_port,
                         (int)rx_size);
            break;
        }
        case NET_MODE_SYNC_SPIN: {
            rx_size = net_sync_recv_spin(ch, rx_buf, cap, timeout_us);
            if (rx_size > 0 && cfg->log_cfg.fd)
                log_data(log,
                         0,
                         "[RECV] %u.%u.%u.%u:%u %d bytes\n",
                         ch->dst_ip & 0xFF,
                         (ch->dst_ip >> 8) & 0xFF,
                         (ch->dst_ip >> 16) & 0xFF,
                         (ch->dst_ip >> 24) & 0xFF,
                         ch->dst_port,
                         (int)rx_size);
            break;
        }
        case NET_MODE_ASYNC: {
            rx_size = net_async_recv(net, ch, rx_buf, cap, timeout_us);
            break;
        }
        default:
            return -MEINVAL;
    }

    return rx_size;
}

ptrdiff_t
net_send_recv(struct net    *net,
              struct net_ch *ch,
              void          *tx_buf,
              const size_t   size,
              void          *rx_buf,
              const size_t   cap,
              const uint32_t timeout_us)
{
    const ptrdiff_t ret = net_send(net, ch, tx_buf, size);
    if (ret <= 0)
        return ret;

    return net_recv(net, ch, rx_buf, cap, timeout_us);
}

int
net_broadcast(const uint32_t   ip,
              const uint16_t   port,
              const void      *tx_buf,
              const size_t     size,
              struct net_resp *resps,
              const size_t     resps_cap,
              const uint32_t   timeout_us)
{
    sockfd_t           fd;
    struct sockaddr_in src_addr;
    socklen_t          addr_len;
    size_t             start_ts;
    int                resp_cnt = 0;
    int                ret;
    const char         opt = 1;

    if (resps == NULL || resps_cap == 0)
        return -MEINVAL;

#if OS(POSIX)
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#elif OS(WIN)
    fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#endif

    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    net_set_nonblock(fd);

    struct sockaddr_in dst_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    dst_addr.sin_addr.s_addr = ip;

    addr_len = sizeof(dst_addr);
    ret =
        sendto(fd, (const char *)tx_buf, (int32_t)size, 0, (struct sockaddr *)&dst_addr, addr_len);
    if (ret <= 0)
        goto cleanup;

    addr_len = sizeof(src_addr);
    start_ts = get_mono_ts_us();
    do {
        if ((size_t)resp_cnt >= resps_cap)
            break;
        ret = recvfrom(fd,
                       (char *)resps[resp_cnt].buf,
                       sizeof(resps[resp_cnt].buf),
                       0,
                       (struct sockaddr *)&src_addr,
                       &addr_len);
        if (ret > 0) {
            IP_U32_TO_STR(&src_addr.sin_addr, resps[resp_cnt].ip, sizeof(resps[resp_cnt].ip));
            resp_cnt++;
        }
    } while (get_mono_ts_us() - start_ts < timeout_us);

    ret = resp_cnt;

cleanup:
    CLOSE_SOCKET(fd);
    return ret;
}

#endif
