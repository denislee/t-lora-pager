/**
 * @file      link_transport_tcp.cpp
 * @brief     Emulator TCP transport for PagerLink (PROTOCOL.md §5) + the
 *            hw_link_* API for the emulator. Hardware uses the BLE GATT
 *            transport in link_transport_ble.cpp.
 *
 * The emulator listens on 127.0.0.1:7600 (env PAGERLINK_PORT, "0" keeps the
 * link off) and serves one client at a time. Each packet is framed as a u16
 * little-endian length (1..65535) followed by the packet bytes; the TCP packet
 * size P is fixed at 244.
 */
#include "hal/link.h"
#include "hal/link/link_service.h"

#ifndef ARDUINO

#include "hal/link/link_log.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int DEFAULT_PORT = 7600;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_enabled{false};
std::atomic<int> g_listen_fd{-1};
std::thread g_thread;

/** Write exactly `len` bytes; false on error (SIGPIPE suppressed). */
bool write_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

/** Read exactly `len` bytes; false on EOF or error. */
bool read_all(int fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false; /* EOF */
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

/** Serve one client until it drops or the service is asked to stop. */
void serve_client(int cfd, const std::string &peer_name) {
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    pagerlink::TransportOps ops;
    ops.send_packet = [cfd](const uint8_t *pkt, size_t len) -> bool {
        const uint8_t hdr[2] = {static_cast<uint8_t>(len & 0xFF),
                                static_cast<uint8_t>((len >> 8) & 0xFF)};
        return write_all(cfd, hdr, sizeof(hdr)) && write_all(cfd, pkt, len);
    };
    ops.packet_size = pagerlink::TCP_PACKET;
    ops.peer_name = peer_name;
    pagerlink::service_on_connect(ops);

    while (!g_stop.load()) {
        pollfd cp{cfd, POLLIN, 0};
        const int pc = ::poll(&cp, 1, 1000);
        if (pc == 0) {
            pagerlink::service_tick();
            continue;
        }
        if (pc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (cp.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (!(cp.revents & POLLIN)) {
            continue;
        }

        uint8_t hdr[2];
        if (!read_all(cfd, hdr, sizeof(hdr))) {
            break;
        }
        const uint16_t len = static_cast<uint16_t>(hdr[0]) |
                             static_cast<uint16_t>(static_cast<uint16_t>(hdr[1]) << 8);
        if (len < 1 || len > 512) {
            LINK_LOG("bad record length %u; dropping client", static_cast<unsigned>(len));
            break;
        }
        uint8_t buf[512];
        if (!read_all(cfd, buf, len)) {
            break;
        }
        pagerlink::service_on_packet(buf, len);
    }

    pagerlink::service_on_disconnect();
}

void thread_main(int listen_fd) {
    while (!g_stop.load()) {
        pollfd lp{listen_fd, POLLIN, 0};
        const int pr = ::poll(&lp, 1, 500);
        if (pr <= 0) {
            if (pr < 0 && errno != EINTR) {
                break;
            }
            continue;
        }
        if (lp.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (!(lp.revents & POLLIN)) {
            continue;
        }

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &plen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (g_stop.load()) {
                break;
            }
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        const std::string peer_name =
            "tcp:" + std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        serve_client(cfd, peer_name);
        ::close(cfd);
    }
}

}  // namespace

bool hw_link_enable(bool en) {
    if (!en) {
        g_enabled.store(false);
        g_stop.store(true);
        const int fd = g_listen_fd.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (g_thread.joinable()) {
            g_thread.join();
        }
        return false;
    }

    if (g_enabled.load()) {
        return true;
    }

    int port = DEFAULT_PORT;
    if (const char *p = getenv("PAGERLINK_PORT")) {
        port = static_cast<int>(strtol(p, nullptr, 10));
    }
    if (port <= 0) {
        LINK_LOG("disabled by PAGERLINK_PORT");
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LINK_LOG("socket() failed: %s", strerror(errno));
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        LINK_LOG("bind 127.0.0.1:%d failed: %s", port, strerror(errno));
        ::close(fd);
        return false;
    }
    if (::listen(fd, 1) < 0) {
        LINK_LOG("listen 127.0.0.1:%d failed: %s", port, strerror(errno));
        ::close(fd);
        return false;
    }

    g_stop.store(false);
    g_listen_fd.store(fd);
    g_enabled.store(true);
    LINK_LOG("listening on 127.0.0.1:%d", port);
    g_thread = std::thread(thread_main, fd);
    return true;
}

bool hw_link_enabled() { return g_enabled.load(); }

bool hw_link_connected() { return pagerlink::service_connected(); }

std::string hw_link_peer_name() { return pagerlink::service_peer_name(); }

void hw_link_get_stats(HwLinkStats &out) { pagerlink::service_get_stats(out); }

#endif  // !ARDUINO
