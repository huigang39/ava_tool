#ifndef MCU

#include "net.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
net_init(net_t *net, const net_cfg_t net_cfg)
{
        DECL(net, cfg, lo);

        *cfg = net_cfg;

        list_init(&lo->ch_root);

        int ret = 0;
#ifdef __linux__
        ret = io_uring_queue_init(cfg->ring_len, &lo->ring, 0);
#elif defined(_WIN32)
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        lo->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        list_init(&lo->pending_reqs);
#endif

        if (cfg->log_cfg.fp)
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

#ifdef __linux__
        io_uring_queue_exit(&lo->ring);
#elif defined(_WIN32)
        WSACleanup();
#endif
}

int
net_set_nonblock(sockfd_t fd)
{
#ifdef __linux__
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
                return flags;

        flags |= O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags);
#elif defined(_WIN32)
        unsigned long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode);
#endif
}

net_ch_t
net_cfg_ch(const u32 det_ip, const u16 dst_port, const net_mode_e e_mode)
{
        const net_ch_t ch = {.e_mode = e_mode, .dst_ip = det_ip, .dst_port = dst_port};
        return ch;
}

int
net_add_ch(net_t *net, net_ch_t *ch)
{
        DECL(net, cfg, lo);

        switch (cfg->e_type) {
                case NET_TYPE_UDP: {
#ifdef __linux__
                        ch->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#elif defined(_WIN32)
                        ch->fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#endif
                        break;
                }
                case NET_TYPE_TCP: {
#ifdef __linux__
                        ch->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#elif defined(_WIN32)
                        ch->fd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
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

        list_add(&ch->ch_node, &lo->ch_root);

#ifdef _WIN32
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
#ifdef __linux__
        return send(ch->fd, tx_buf, size, 0);
#elif defined(_WIN32)
        return send(ch->fd, (const char *)tx_buf, (int)size, 0);
#endif
}

isize
net_sync_recv_yield(const net_ch_t *ch, void *rx_buf, const usize cap, const u32 timeout_us)
{
#ifdef __linux__
        const struct timeval tv = {
            .tv_sec  = (int)US2S(timeout_us),
            .tv_usec = (int)(timeout_us % 1000000),
        };
        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return recv(ch->fd, rx_buf, cap, 0);
#elif defined(_WIN32)
        DWORD tv_ms = US2MS(timeout_us);
        if (tv_ms == 0 && timeout_us > 0)
                tv_ms = 1;

        setsockopt(ch->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
        return recv(ch->fd, (char *)rx_buf, (int)cap, 0);
#endif
}

isize
net_sync_recv_spin(const net_ch_t *ch, void *rx_buf, const usize cap, const u32 timeout_us)
{
        const u64 start_ns = get_mono_ts_ns();
        u64       curr_ns  = 0;
        while (curr_ns < start_ns + US2NS(timeout_us)) {
#ifdef __linux__
                const int ret = recv(ch->fd, rx_buf, cap, MSG_DONTWAIT);
#elif defined(_WIN32)
                const int ret = recv(ch->fd, (char *)rx_buf, (int)cap, 0);
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

#ifdef __linux__
        struct io_uring_sqe *send_sqe = io_uring_get_sqe(&lo->ring);
        if (!send_sqe)
                return -1;

        net_async_req_t *req = (net_async_req_t *)mempool_calloc(cfg->mp, sizeof(net_async_req_t));
        if (!req)
                return -MEALLOC;

        req->ch   = ch;
        req->buf  = tx_buf;
        req->size = size;
        req->f_cb = ch->f_send_cb;

        io_uring_prep_send(send_sqe, ch->fd, tx_buf, size, 0);
        io_uring_sqe_set_data(send_sqe, req);

        io_uring_submit(&lo->ring);
        return size;
#elif defined(_WIN32)
        net_async_req_t *req = (net_async_req_t *)mempool_calloc(cfg->mp, sizeof(net_async_req_t));
        if (!req)
                return -MEALLOC;

        req->ch   = ch;
        req->buf  = tx_buf;
        req->size = size;
        req->f_cb = ch->f_send_cb;

        WSABUF buf;
        buf.buf = (CHAR *)tx_buf;
        buf.len = (ULONG)size;

        DWORD     tx_size;
        const int ret = WSASend(ch->fd, &buf, 1, &tx_size, 0, &req->ov, NULL);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                mempool_free(cfg->mp, req);
                return -1;
        }

        req->ov.Pointer = req;
        return tx_size;
#endif
}

isize
net_async_recv(net_t *net, net_ch_t *ch, void *rx_buf, usize cap, u32 timeout_us)
{
        DECL(net, cfg, lo);

#ifdef __linux__
        net_async_req_t *req = (net_async_req_t *)mempool_calloc(cfg->mp, sizeof(net_async_req_t));
        if (!req)
                return -MEALLOC;

        req->ch   = ch;
        req->buf  = rx_buf;
        req->size = cap;
        req->f_cb = ch->f_recv_cb;

        struct io_uring_sqe *recv_sqe = io_uring_get_sqe(&lo->ring);
        if (!recv_sqe)
                return -1;

        io_uring_prep_recv(recv_sqe, ch->fd, rx_buf, cap, 0);
        io_uring_sqe_set_data(recv_sqe, req);
        io_uring_sqe_set_flags(recv_sqe, IOSQE_IO_LINK);

        struct io_uring_sqe     *timeout_sqe = io_uring_get_sqe(&lo->ring);
        struct __kernel_timespec ts          = {
                     .tv_sec  = (int)US2S(timeout_us),
                     .tv_nsec = (int)US2NS(timeout_us % 1000000),
        };
        io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
        io_uring_sqe_set_data(timeout_sqe, req);

        return io_uring_submit(&lo->ring);
#elif defined(_WIN32)
        net_async_req_t *req = (net_async_req_t *)mempool_calloc(cfg->mp, sizeof(net_async_req_t));
        if (!req)
                return -MEALLOC;

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
        const int ret = WSARecv(ch->fd, &buf, 1, &rx_size, &flags, &req->ov, NULL);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                mempool_free(cfg->mp, req);
                return -1;
        }

        req->ov.Pointer = req;
        if (timeout_us > 0)
                list_add_tail(&req->pending_node, &lo->pending_reqs);

        return rx_size;
#endif
}

int
net_poll(net_t *net)
{
        DECL(net, cfg, lo);

#ifdef __linux__
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&lo->ring, &cqe) == 0) {
                net_async_req_t *req = (net_async_req_t *)io_uring_cqe_get_data(cqe);
                if (!req) {
                        io_uring_cqe_seen(&lo->ring, cqe);
                        continue;
                }

                if (ATOMIC_EXCHANGE(&req->processed, 1) == 0) {
                        req->f_cb(req->ch, req->buf, cqe->res == -ETIME ? -METIMEOUT : cqe->res);
                        mempool_free(cfg->mp, req);
                }

                io_uring_cqe_seen(&lo->ring, cqe);
        }
        return 0;
#elif defined(_WIN32)
        DWORD       size;
        ULONG_PTR   key;
        OVERLAPPED *ov = NULL;

        const u64    curr_ts = get_mono_ts_us();
        list_head_t *node, *next;
        for (node = lo->pending_reqs.next; node != &lo->pending_reqs; node = next) {
                next                 = node->next;
                net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                if (req->timeout_us > 0 && curr_ts >= req->timeout_us) {
                        if (ATOMIC_EXCHANGE(&req->processed, 1) == 0) {
                                CancelIoEx((HANDLE)req->ch->fd, &req->ov);
                                req->f_cb(req->ch, req->buf, -METIMEOUT);
                                list_del(&req->pending_node);
                                mempool_free(cfg->mp, req);
                        }
                }
        }

        DWORD timeout_ms = INFINITE;
        if (!list_empty(&lo->pending_reqs)) {
                u64 min_timeout_us = (u64)-1;
                LIST_FOR_EACH(node, &lo->pending_reqs)
                {
                        const net_async_req_t *req = CONTAINER_OF(node, net_async_req_t, pending_node);
                        if (req->timeout_us > 0 && req->timeout_us < min_timeout_us) {
                                min_timeout_us = req->timeout_us;
                        }
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

        if (ATOMIC_EXCHANGE(&req->processed, 1) == 0) {
                req->f_cb(req->ch, req->buf, ok ? (int)size : -1);
                mempool_free(cfg->mp, req);
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
                        tx_size                               = net_sync_send(ch, tx_buf, size);
                        const net_log_header_t net_log_header = {
                            .ts       = cfg->f_get_ts(),
                            .e_op     = NET_OP_SEND,
                            .dst_ip   = ch->dst_ip,
                            .dst_port = ch->dst_port,
                            .size     = (u16)tx_size,
                        };

                        if (cfg->log_cfg.fp)
                                log_write_bin(log, 0, &net_log_header, sizeof(net_log_header), tx_buf, tx_size);

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
                        rx_size                               = net_sync_recv_yield(ch, rx_buf, cap, timeout_us);
                        const net_log_header_t net_log_header = {
                            .ts       = cfg->f_get_ts(),
                            .e_op     = NET_OP_RECV,
                            .dst_ip   = ch->dst_ip,
                            .dst_port = ch->dst_port,
                            .size     = (u16)rx_size,
                        };

                        if (cfg->log_cfg.fp)
                                log_write_bin(log, 0, &net_log_header, sizeof(net_log_header), rx_buf, rx_size);

                        break;
                }
                case NET_MODE_SYNC_SPIN: {
                        rx_size                               = net_sync_recv_spin(ch, rx_buf, cap, timeout_us);
                        const net_log_header_t net_log_header = {
                            .ts       = cfg->f_get_ts(),
                            .e_op     = NET_OP_RECV,
                            .dst_ip   = ch->dst_ip,
                            .dst_port = ch->dst_port,
                            .size     = (u16)rx_size,
                        };

                        if (cfg->log_cfg.fp)
                                log_write_bin(log, 0, &net_log_header, sizeof(net_log_header), rx_buf, rx_size);

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

int
net_broadcast(const u32 ip, const u16 port, const void *tx_buf, const usize size, net_resp_t *resps, const u32 timeout_us)
{
        sockfd_t           fd;
        struct sockaddr_in src_addr;
        socklen_t          addr_len;
        usize              start_ts;
        int                resp_cnt = 0;
        int                ret;
        const char         opt = 1;

#ifdef __linux__
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#elif defined(_WIN32)
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
        ret      = sendto(fd, (const char *)tx_buf, (int)size, 0, (struct sockaddr *)&dst_addr, addr_len);
        if (ret <= 0)
                goto cleanup;

        addr_len = sizeof(src_addr);
        start_ts = get_mono_ts_us();
        do {
                ret =
                    recvfrom(fd, resps[resp_cnt].buf, sizeof(resps[resp_cnt].buf), 0, (struct sockaddr *)&src_addr, &addr_len);
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
