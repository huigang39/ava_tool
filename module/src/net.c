#ifndef MCU

#include "net.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int32_t
net_init(net_t *net, const net_cfg_t net_cfg)
{
        DECL(net, cfg, lo);

        *cfg = net_cfg;

        list_init(&lo->ch_root);

        i32 ret = 0;
#ifdef OS_LINUX
        ret = io_uring_queue_init(cfg->ring_len, &lo->ring, 0);
#elif defined(OS_MAC)
        lo->kq = kqueue();
        if (lo->kq < 0)
                ret = -1;
        list_init(&lo->pending_reqs);
#elif defined(OS_WIN)
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
net_destroy(net_t *net)
{
        DECL(net, cfg, lo);

        list_head_t *node;
        LIST_FOR_EACH(node, &lo->ch_root)
        {
                const net_ch_t *ch = CONTAINER_OF(node, net_ch_t, ch_node);
                CLOSE_SOCKET(ch->fd);
        }

#ifdef OS_LINUX
        io_uring_queue_exit(&lo->ring);
#elif defined(OS_MAC)
        if (lo->kq >= 0)
                close(lo->kq);
#elif defined(OS_WIN)
        WSACleanup();
#endif
}

i32
net_set_nonblock(sockfd_t fd)
{
#ifdef OS_POSIX
        i32 flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
                return flags;

        flags |= O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags);
#elif defined(OS_WIN)
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode);
#endif
}

net_ch_t
net_cfg_ch(const u32 det_ip, const u16 dst_port, const net_mode_e e_mode)
{
        const net_ch_t ch = {
            .e_mode   = e_mode,
            .dst_ip   = det_ip,
            .dst_port = dst_port,
        };
        return ch;
}

i32
net_add_ch(net_t *net, net_ch_t *ch)
{
        DECL(net, cfg, lo);

        switch (cfg->e_type) {
                case NET_TYPE_UDP: {
#ifdef OS_POSIX
                        ch->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                        if (ch->fd < 0)
                                return -MESYSERR;
#elif defined(OS_WIN)
                        ch->fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
                        if (ch->fd == INVALID_SOCKET)
                                return -MESYSERR;
#endif
                        break;
                }
                case NET_TYPE_TCP: {
#ifdef OS_POSIX
                        ch->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        if (ch->fd < 0)
                                return -MESYSERR;
#elif defined(OS_WIN)
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

        i32 ret;
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

#ifdef OS_WIN
        CreateIoCompletionPort((HANDLE)ch->fd, lo->iocp, (ULONG_PTR)ch, 0);
#endif

        return 0;

cleanup:
        CLOSE_SOCKET(ch->fd);
        return ret;
}

isize
net_sync_send(const net_ch_t *ch, const void *tx_buf, const usize size)
{
#ifdef OS_POSIX
        return send(ch->fd, tx_buf, size, 0);
#elif defined(OS_WIN)
        return send(ch->fd, (const char *)tx_buf, (int)size, 0);
#endif
}

isize
net_sync_recv_yield(const net_ch_t *ch, void *rx_buf, const usize cap, const u32 timeout_us)
{
#ifdef OS_POSIX
        const struct timeval tv = {
            .tv_sec  = (i32)US2S(timeout_us),
            .tv_usec = (i32)(timeout_us % 1000000),
        };
        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return recv(ch->fd, rx_buf, cap, 0);
#elif defined(OS_WIN)
        DWORD tv_ms = (DWORD)US2MS(timeout_us);
        if (tv_ms == 0 && timeout_us > 0)
                tv_ms = 1;

        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
        return recv(ch->fd, (char *)rx_buf, (i32)cap, 0);
#endif
}

isize
net_sync_recv_spin(const net_ch_t *ch, void *rx_buf, const usize cap, const u32 timeout_us)
{
        const u64 start_ns = get_mono_ts_ns();
        u64       curr_ns  = 0;
        while (curr_ns < start_ns + US2NS(timeout_us)) {
#ifdef OS_POSIX
                const i32 ret = recv(ch->fd, rx_buf, cap, MSG_DONTWAIT);
#elif defined(OS_WIN)
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(ch->fd, &read_fds);
                struct timeval tv         = {0, 0};
                const i32      select_ret = select(0, &read_fds, NULL, NULL, &tv);
                i32            ret        = -1;
                if (select_ret > 0 && FD_ISSET(ch->fd, &read_fds))
                        ret = recv(ch->fd, (char *)rx_buf, (i32)cap, 0);
                else if (select_ret < 0)
                        return -MESYSERR;
#endif
                if (ret > 0)
                        return ret;

                curr_ns = get_mono_ts_ns();
        }
        return -METIMEOUT;
}

isize
net_async_send(net_t *net, net_ch_t *ch, void *tx_buf, usize size)
{
        DECL(net, cfg, lo);

#ifdef OS_LINUX
        struct io_uring_sqe *send_sqe = io_uring_get_sqe(&lo->ring);
        if (!send_sqe)
                return -1;

        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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
#elif defined(OS_MAC)
        /* macOS: try non-blocking send immediately (fd already non-blocking from net_add_ch),
         * fall back to EVFILT_WRITE if EAGAIN */
        const isize n = send(ch->fd, tx_buf, size, 0);
        if (n >= 0) {
                if (ch->f_send_cb)
                        ch->f_send_cb(ch, tx_buf, (i32)n);
                return n;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;

        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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
        return (isize)size;
#elif defined(OS_WIN)
        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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

        DWORD     tx_size;
        const i32 ret = WSASend(ch->fd, &buf, 1, &tx_size, 0, &req->ov, NULL);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                mempool_free(cfg->mempool, req);
                return -1;
        }

        req->ov.Pointer = req;
        return (isize)tx_size;
#endif
}

isize
net_async_recv(net_t *net, net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us)
{
        DECL(net, cfg, lo);

#ifdef OS_LINUX
        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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
#elif defined(OS_MAC)
        /* fd already non-blocking from net_add_ch */
        const isize n = recv(ch->fd, rx_buf, cap, 0);
        if (n > 0) {
                if (ch->f_recv_cb)
                        ch->f_recv_cb(ch, rx_buf, (i32)n);
                return n;
        }
        if (n == 0)
                return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;

        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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
        return (isize)cap;
#elif defined(OS_WIN)
        net_async_req_t *req = (net_async_req_t *)mempool_alloc(cfg->mempool, sizeof(net_async_req_t));
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

        DWORD     flags = 0;
        DWORD     rx_size;
        const i32 ret = WSARecv(ch->fd, &buf, 1, &rx_size, &flags, &req->ov, NULL);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                mempool_free(cfg->mempool, req);
                return -1;
        }

        req->ov.Pointer = req;
        if (timeout_us > 0)
                list_add_tail(&req->pending_node, &lo->pending_reqs);

        return (isize)rx_size;
#endif
}

i32
net_poll(net_t *net)
{
        DECL(net, cfg, lo);

#ifdef OS_LINUX
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&lo->ring, &cqe) == 0) {
                net_async_req_t *req = (net_async_req_t *)io_uring_cqe_get_data(cqe);
                if (!req) {
                        io_uring_cqe_seen(&lo->ring, cqe);
                        continue;
                }

                if (ATOMIC_EXCHANGE(&req->processed, TRUE) == 0) {
                        if (req->f_cb)
                                req->f_cb(req->ch, req->buf, cqe->res == -ETIME ? -METIMEOUT : cqe->res);
                        mempool_free(cfg->mempool, req);
                }

                io_uring_cqe_seen(&lo->ring, cqe);
        }
        return 0;
#elif defined(OS_MAC)
        const u64    curr_ts = get_mono_ts_us();
        list_head_t *node, *next;

        /* Phase 1: scan timeouts — mark, EV_DELETE, callback, move to done_list.
         * Do NOT free here; deferred until after kevent to avoid touching freed
         * memory if the kernel had already queued an event for the same req. */
        list_head_t done_list;
        list_init(&done_list);
        for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
                next                 = node->next;
                net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
                        if (ATOMIC_EXCHANGE(&req->processed, TRUE) == 0) {
                                struct kevent kev;
                                const i16     filter = (req->e_op == NET_OP_SEND) ? EVFILT_WRITE : EVFILT_READ;
                                EV_SET(&kev, req->ch->fd, filter, EV_DELETE, 0, 0, NULL);
                                kevent(lo->kq, &kev, 1, NULL, 0, NULL);
                                if (req->f_cb)
                                        req->f_cb(req->ch, req->buf, -METIMEOUT);
                                list_del(&req->pending_node);
                                list_add_tail(&req->pending_node, &done_list);
                        }
                }
        }

        /* Phase 2: compute kevent timeout; always use ts={0,0} when no pending
         * requests so net_poll returns immediately instead of blocking forever. */
        struct timespec ts = {0, 0};
        if (!list_empty(&lo->pending_reqs)) {
                u64 min_deadline = (u64)-1;
                LIST_FOR_EACH(node, &lo->pending_reqs)
                {
                        const net_async_req_t *r = CONTAINER_OF(node, net_async_req_t, pending_node);
                        if (r->timeout_us > 0 && r->timeout_us < min_deadline)
                                min_deadline = r->timeout_us;
                }
                if (min_deadline != (u64)-1 && min_deadline > curr_ts) {
                        const u64 wait_us = min_deadline - curr_ts;
                        ts.tv_sec         = (time_t)(wait_us / 1000000);
                        ts.tv_nsec        = (long)((wait_us % 1000000) * 1000);
                }
        }

        /* Phase 3: collect ready events */
        struct kevent events[16];
        const i32     n = kevent(lo->kq, NULL, 0, events, 16, &ts);

        /* Phase 4: process events — already-timed-out reqs are skipped via
         * processed flag; pending_node is NOT touched for those (they are in
         * done_list, not pending_reqs). */
        for (i32 i = 0; i < n; ++i) {
                net_async_req_t *req = (net_async_req_t *)events[i].udata;
                if (!req)
                        continue;

                if (ATOMIC_EXCHANGE(&req->processed, TRUE) != 0)
                        continue; /* already handled in timeout loop */

                isize ret;
                if (req->e_op == NET_OP_SEND)
                        ret = send(req->ch->fd, req->buf, req->size, 0);
                else
                        ret = recv(req->ch->fd, req->buf, req->size, 0);

                if (events[i].flags & EV_ERROR)
                        ret = -1;

                if (req->f_cb)
                        req->f_cb(req->ch, req->buf, (i32)ret);

                list_del(&req->pending_node);
                mempool_free(cfg->mempool, req);
        }

        /* Phase 5: free timed-out reqs now that kevent is done */
        for (node = done_list.next; node != &done_list; node = next) {
                next                 = node->next;
                net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                mempool_free(cfg->mempool, req);
        }
        return 0;
#elif defined(OS_WIN)
        DWORD       size;
        ULONG_PTR   key;
        OVERLAPPED *ov = NULL;

        const u64    curr_ts = get_mono_ts_us();
        list_head_t *node, *next;
        for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
                next                 = node->next;
                net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
                        if (ATOMIC_EXCHANGE(&req->processed, TRUE) == 0) {
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
                u64 min_timeout_us = (u64)-1;
                LIST_FOR_EACH(node, &lo->pending_reqs)
                {
                        const net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                        if (req->timeout_us > 0 && req->timeout_us < min_timeout_us)
                                min_timeout_us = req->timeout_us;
                }
                if (min_timeout_us != (u64)-1 && min_timeout_us > curr_ts) {
                        timeout_ms = (DWORD)US2MS(min_timeout_us - curr_ts);
                        if (timeout_ms == 0)
                                timeout_ms = 1;
                }
        }

        const BOOL ok = GetQueuedCompletionStatus(lo->iocp, &size, &key, &ov, timeout_ms);
        if (!ok && ov == NULL)
                return 0;

        net_async_req_t *req = (net_async_req_t *)ov->Pointer;
        if (!req)
                return 0;

        if (req->timeout_us > 0)
                list_del(&req->pending_node);

        if (ATOMIC_EXCHANGE(&req->processed, TRUE) == 0) {
                if (req->f_cb)
                        req->f_cb(req->ch, req->buf, ok ? (i32)size : -1);
                mempool_free(cfg->mempool, req);
        }

        return 0;
#endif
}

isize
net_send(net_t *net, net_ch_t *ch, void *tx_buf, const usize size)
{
        DECL(net, cfg, lo);
        RENAME(&lo->log, log);

        isize tx_size;
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

isize
net_recv(net_t *net, net_ch_t *ch, void *rx_buf, const usize cap, const u32 timeout_us)
{
        DECL(net, cfg, lo);
        RENAME(&lo->log, log);

        isize rx_size;
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

isize
net_send_recv(net_t *net, net_ch_t *ch, void *tx_buf, const usize size, void *rx_buf, const usize cap, const u32 timeout_us)
{
        const isize ret = net_send(net, ch, tx_buf, size);
        if (ret <= 0)
                return ret;

        return net_recv(net, ch, rx_buf, cap, timeout_us);
}

i32
net_broadcast(const u32   ip,
              const u16   port,
              const void *tx_buf,
              const usize size,
              net_resp_t *resps,
              const usize resps_cap,
              const u32   timeout_us)
{
        sockfd_t           fd;
        struct sockaddr_in src_addr;
        socklen_t          addr_len;
        usize              start_ts;
        i32                resp_cnt = 0;
        i32                ret;
        const char         opt = 1;

        if (resps == NULL || resps_cap == 0)
                return -MEINVAL;

#ifdef OS_POSIX
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#elif defined(OS_WIN)
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
        ret      = sendto(fd, (const char *)tx_buf, (i32)size, 0, (struct sockaddr *)&dst_addr, addr_len);
        if (ret <= 0)
                goto cleanup;

        addr_len = sizeof(src_addr);
        start_ts = get_mono_ts_us();
        do {
                if ((usize)resp_cnt >= resps_cap)
                        break;
                ret = recvfrom(
                    fd, (char *)resps[resp_cnt].buf, sizeof(resps[resp_cnt].buf), 0, (struct sockaddr *)&src_addr, &addr_len);
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
