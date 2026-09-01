#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "cJSON.h"
#include "firmware_match.hpp"
#include "remote_device.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

namespace ava_peer
{
namespace
{
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using Socket             = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
void
closeSocket(Socket s)
{
        if (s != invalid)
                closesocket(s);
}
void
sockets()
{
        static const bool initialized = [] {
                WSADATA d{};
                return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        if (!initialized)
                throw std::runtime_error("Winsock unavailable");
}
void
nonblock(Socket s)
{
        u_long one = 1;
        ioctlsocket(s, FIONBIO, &one);
}
void
shut(Socket s)
{
        if (s != invalid)
                shutdown(s, SD_BOTH);
}
#else
using Socket             = int;
constexpr Socket invalid = -1;
void
closeSocket(Socket s)
{
        if (s != invalid)
                ::close(s);
}
void
sockets()
{
}
void
nonblock(Socket s)
{
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
void
shut(Socket s)
{
        if (s != invalid)
                shutdown(s, SHUT_RDWR);
}
#endif
struct SocketOwner {
        Socket value{invalid};
        ~SocketOwner() { closeSocket(value); }
};
struct WireError : std::runtime_error {
        using std::runtime_error::runtime_error;
};
constexpr size_t      maxPacket     = 192 * 1024;
constexpr uint16_t    discoveryPort = 45992;
constexpr const char *group         = "239.255.42.100";
std::recursive_mutex  ownership;
uint64_t              owner{};
std::atomic<uint64_t> sessionCounter{1};
thread_local uint64_t callingSession{};
struct Context {
        uint64_t previous;
        explicit Context(uint64_t id) : previous(callingSession) { callingSession = id; }
        ~Context() { callingSession = previous; }
};
struct Json {
        cJSON *p{};
        Json() : p(cJSON_CreateObject()) {}
        explicit Json(cJSON *value) : p(value)
        {
                if (!p)
                        throw std::runtime_error("Invalid JSON");
        }
        explicit Json(const std::string &value) : Json(cJSON_ParseWithLength(value.c_str(), value.size() + 1)) {}
        ~Json() { cJSON_Delete(p); }
        Json(const Json &) = delete;
        std::string str() const
        {
                char *s = cJSON_PrintUnformatted(p);
                if (!s)
                        throw std::bad_alloc();
                std::string result(s);
                cJSON_free(s);
                return result;
        }
        void text(const char *k, const std::string &v) { cJSON_AddStringToObject(p, k, v.c_str()); }
        void num(const char *k, double v) { cJSON_AddNumberToObject(p, k, v); }
        void flag(const char *k, bool v) { cJSON_AddBoolToObject(p, k, v); }
};
std::string
text(cJSON *p, const char *k)
{
        auto *v = cJSON_GetObjectItemCaseSensitive(p, k);
        return cJSON_IsString(v) ? v->valuestring : "";
}
double
number(cJSON *p, const char *k)
{
        auto *v = cJSON_GetObjectItemCaseSensitive(p, k);
        if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble))
                throw std::runtime_error("Missing numeric field");
        return v->valuedouble;
}
uint64_t
integer(cJSON *p, const char *k, uint64_t max = 0xffffffffULL)
{
        double n = number(p, k);
        if (n < 0 || n > double(max) || n != std::floor(n))
                throw std::runtime_error("Integer outside range");
        return uint64_t(n);
}
bool
flag(cJSON *p, const char *k)
{
        return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, k));
}
std::string
hex(const uint8_t *p, size_t n)
{
        return firmware_match::hex(p, n);
}
std::vector<uint8_t>
unhex(const std::string &s)
{
        if (s.size() % 2 || s.size() > 65536)
                throw std::runtime_error("Invalid byte string");
        auto digit = [](char c) {
                if (c >= '0' && c <= '9')
                        return c - '0';
                if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                throw std::runtime_error("Invalid hex");
        };
        std::vector<uint8_t> b(s.size() / 2);
        for (size_t i = 0; i < b.size(); ++i)
                b[i] = uint8_t(digit(s[2 * i]) * 16 + digit(s[2 * i + 1]));
        return b;
}
std::string
hmac(const std::string &key, const std::string &message)
{
        std::vector<uint8_t> inner(64, 0x36), outer(64, 0x5c);
        for (size_t i = 0; i < key.size(); ++i) {
                inner[i] ^= uint8_t(key[i]);
                outer[i] ^= uint8_t(key[i]);
        }
        inner.insert(inner.end(), message.begin(), message.end());
        auto hash = firmware_match::sha256(std::move(inner));
        outer.insert(outer.end(), hash.begin(), hash.end());
        hash = firmware_match::sha256(std::move(outer));
        return hex(hash.data(), hash.size());
}
bool
equal(const std::string &a, const std::string &b)
{
        if (a.size() != b.size())
                return false;
        unsigned delta = 0;
        for (size_t i = 0; i < a.size(); ++i)
                delta |= unsigned(a[i] ^ b[i]);
        return delta == 0;
}
void
validateKey(const std::string &key)
{
        if (key.size() != 32 || unhex(key).size() != 16)
                throw std::runtime_error("Pairing key must contain 32 hexadecimal characters");
}
bool
ready(Socket s, bool output, int ms)
{
        fd_set f;
        FD_ZERO(&f);
        FD_SET(s, &f);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        return select(int(s + 1), output ? nullptr : &f, output ? &f : nullptr, nullptr, &tv) > 0;
}
void
transfer(Socket s, void *ptr, size_t n, bool output, Clock::time_point deadline)
{
        auto *data = static_cast<char *>(ptr);
        while (n) {
                int remaining = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
                if (remaining <= 0 || !ready(s, output, remaining))
                        throw WireError("Peer timeout");
#ifdef MSG_NOSIGNAL
                constexpr int sendFlags = MSG_NOSIGNAL;
#else
                constexpr int sendFlags = 0;
#endif
                int done = output ? int(send(s, data, int(n), sendFlags)) : int(recv(s, data, int(n), 0));
                if (done <= 0)
                        throw WireError("Peer disconnected");
                data += done;
                n    -= size_t(done);
        }
}
void
sendFrame(Socket s, const std::string &data)
{
        if (data.size() > maxPacket)
                throw WireError("Packet too large");
        auto     end = Clock::now() + std::chrono::seconds(4);
        uint32_t n   = htonl(uint32_t(data.size()));
        transfer(s, &n, 4, true, end);
        transfer(s, const_cast<char *>(data.data()), data.size(), true, end);
}
std::string
receiveFrame(Socket s, size_t maximum = maxPacket)
{
        auto     end = Clock::now() + std::chrono::seconds(5);
        uint32_t n{};
        transfer(s, &n, 4, false, end);
        n = ntohl(n);
        if (!n || n > maximum)
                throw WireError("Invalid packet length");
        std::string data(n, '\0');
        transfer(s, data.data(), n, false, end);
        return data;
}
struct Channel {
        Socket      socket;
        std::string key, nonce;
        bool        server;
        uint64_t    tx{}, rx{};
        void        send(const std::string &payload)
        {
                Json envelope;
                envelope.num("seq", double(++tx));
                envelope.text("payload", payload);
                envelope.text("mac", hmac(key, nonce + (server ? "S" : "C") + std::to_string(tx) + ":" + payload));
                sendFrame(socket, envelope.str());
        }
        std::string receive()
        {
                Json envelope(receiveFrame(socket));
                auto seq     = integer(envelope.p, "seq", 9007199254740991ULL);
                auto payload = text(envelope.p, "payload");
                if (seq != rx + 1 || !equal(text(envelope.p, "mac"),
                                            hmac(key, nonce + (server ? "C" : "S") + std::to_string(seq) + ":" + payload)))
                        throw WireError("Peer authentication/sequence failed");
                rx = seq;
                return payload;
        }
};
void
range(uint64_t address, uint64_t size)
{
        if (!size || size > 4096 || address + size > 0x100000000ULL)
                throw std::runtime_error("Memory range outside limits");
}
void
putDevice(Json &j, const Device &d, bool writes)
{
        j.text("name", d.name);
        j.num("epoch", double(d.epoch));
        j.flag("ready", d.ready);
        j.flag("write", writes && d.writable);
        j.flag("reset", writes && d.resettable);
        j.num("minPeriodUs", 20000);
        j.num("maxBlocks", 32);
        j.text("sampling", "timestamped-poll");
        j.flag("flash", false);
        j.flag("rtt", false);
}
Device
getDevice(cJSON *j)
{
        return {
            text(j, "name"), integer(j, "epoch", 9007199254740991ULL), flag(j, "ready"), flag(j, "write"), flag(j, "reset")};
}
Socket
udpSocket(bool listener)
{
        sockets();
        Socket s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == invalid)
                throw WireError("UDP unavailable");
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(listener ? discoveryPort : 0);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
                closeSocket(s);
                throw WireError("Discovery port unavailable");
        }
        if (listener) {
                ip_mreq membership{};
                inet_pton(AF_INET, group, &membership.imr_multiaddr);
                membership.imr_interface.s_addr = htonl(INADDR_ANY);
                setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char *>(&membership), sizeof(membership));
        }
        nonblock(s);
        return s;
}
} // namespace
WriteGuard::WriteGuard() : lock_(ownership), allowed_(!owner || owner == callingSession) {}
bool
backendCall()
{
        return callingSession != 0;
}
bool
remoteOwnsWrites()
{
        std::lock_guard lock(ownership);
        return owner != 0;
}
std::string
randomKey()
{
        std::array<uint8_t, 16> bytes{};
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("Secure randomness unavailable");
#else
        std::ifstream random("/dev/urandom", std::ios::binary);
        if (!random.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
                throw std::runtime_error("Secure randomness unavailable");
#endif
        return hex(bytes.data(), bytes.size());
}
std::vector<Peer>
discover(int milliseconds)
{
        SocketOwner socket{udpSocket(false)};
        std::string query = "AVA_PEER_DISCOVER/1 " + randomKey();
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port   = htons(discoveryPort);
        inet_pton(AF_INET, group, &target.sin_addr);
        sendto(socket.value, query.data(), int(query.size()), 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
        std::vector<Peer> peers;
        auto              end = Clock::now() + std::chrono::milliseconds(std::clamp(milliseconds, 100, 3000));
        while (Clock::now() < end) {
                if (!ready(socket.value, false, 100))
                        continue;
                char        buffer[2048];
                sockaddr_in from{};
#ifdef _WIN32
                int length = sizeof(from);
#else
                socklen_t length = sizeof(from);
#endif
                int n = int(recvfrom(socket.value, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &length));
                try {
                        if (n <= 0)
                                continue;
                        Json j(std::string(buffer, size_t(n)));
                        if (text(j.p, "query") != query || integer(j.p, "protocol") != 1)
                                continue;
                        char host[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
                        Peer peer{text(j.p, "name"), host, int(integer(j.p, "port", 65535))};
                        if (peer.port && peers.size() < 64 && std::none_of(peers.begin(), peers.end(), [&](const Peer &p) {
                                    return p.host == peer.host && p.port == peer.port;
                            }))
                                peers.push_back(peer);
                } catch (...) {
                }
        }
        return peers;
}

struct Server::Impl {
        Backend                  backend;
        std::string              name, key;
        bool                     allowWrites{};
        std::atomic<bool>        alive{};
        Socket                   listener{invalid};
        int                      boundPort{};
        std::vector<std::thread> workers;
        std::thread              discovery;
        std::mutex               clientsMutex;
        std::set<Socket>         clients;
        std::atomic<int>         authenticated{};
        struct Session {
                Impl                            &server;
                uint64_t                         id;
                std::atomic<bool>                sampling{};
                std::thread                      sampler;
                std::mutex                       mutex;
                std::deque<std::vector<uint8_t>> frames;
                uint64_t                         epoch{}, lost{};
                std::string                      sampleError;
                int                              frameSize{};
                ~Session()
                {
                        stop();
                        std::lock_guard lock(ownership);
                        if (owner == id)
                                owner = 0;
                }
                void stop()
                {
                        sampling = false;
                        if (sampler.joinable())
                                sampler.join();
                        std::lock_guard lock(mutex);
                        frames.clear();
                }
                void check(uint64_t expected)
                {
                        Context context(id);
                        auto    d = server.backend.device();
                        if (!d.ready || !expected || d.epoch != expected)
                                throw std::runtime_error("Target changed/disconnected; reconnect explicitly");
                }
                void start(const std::vector<Block> &blocks, int period, uint64_t expected)
                {
                        stop();
                        check(expected);
                        epoch     = expected;
                        frameSize = 4;
                        for (auto b : blocks)
                                frameSize += int(b.size);
                        lost = 0;
                        sampleError.clear();
                        sampling = true;
                        sampler  = std::thread([this, blocks, period] {
                                Context context(id);
                                auto    next = Clock::now();
                                while (sampling && server.alive) {
                                        if (Clock::now() < next) {
                                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                                continue;
                                        }
                                        try {
                                                check(epoch);
                                                std::vector<uint8_t> frame(size_t(frameSize), 0);
                                                auto stamp = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                          Clock::now().time_since_epoch())
                                                                          .count());
                                                for (int i = 0; i < 4; ++i)
                                                        frame[i] = uint8_t(stamp >> (i * 8));
                                                size_t offset = 4;
                                                for (auto b : blocks) {
                                                        if (!server.backend.read(
                                                                epoch, b.address, b.size, frame.data() + offset))
                                                                throw std::runtime_error("Device sampling failed");
                                                        offset += b.size;
                                                }
                                                check(epoch);
                                                std::lock_guard lock(mutex);
                                                if (frames.size() >= 256) {
                                                        frames.pop_front();
                                                        ++lost;
                                                }
                                                frames.push_back(std::move(frame));
                                        } catch (const std::exception &e) {
                                                std::lock_guard lock(mutex);
                                                sampleError = e.what();
                                                sampling    = false;
                                        }
                                        next = Clock::now() + std::chrono::microseconds(period);
                                }
                        });
                }
        };
        std::string dispatch(Session &s, const std::string &request)
        {
                Json q(request), out;
                out.flag("ok", true);
                Context    context(s.id);
                const auto op = text(q.p, "op");
                if (op == "info") {
                        putDevice(out, backend.device(), allowWrites);
                        out.text("peer", name);
                        out.num("protocol", 1);
                        std::lock_guard lock(ownership);
                        out.flag("control", owner == s.id);
                } else if (op == "claim") {
                        std::lock_guard lock(ownership);
                        bool            acquire = flag(q.p, "acquire");
                        if (acquire) {
                                auto d = backend.device();
                                if (!allowWrites || !d.writable)
                                        throw std::runtime_error("Device shared read-only");
                                if (owner && owner != s.id)
                                        throw std::runtime_error("Another controller holds this device");
                                owner = s.id;
                        } else if (owner == s.id)
                                owner = 0;
                        out.flag("control", owner == s.id);
                } else if (op == "stop")
                        s.stop();
                else if (op == "samples") {
                        s.check(s.epoch);
                        auto capacity = integer(q.p, "capacity", 32768);
                        if (capacity < uint64_t(s.frameSize))
                                throw std::runtime_error("Sample buffer too small");
                        std::vector<uint8_t> bytes;
                        std::lock_guard      lock(s.mutex);
                        if (!s.sampleError.empty())
                                throw std::runtime_error(s.sampleError);
                        while (!s.frames.empty() && bytes.size() + s.frames.front().size() <= capacity) {
                                auto &frame = s.frames.front();
                                bytes.insert(bytes.end(), frame.begin(), frame.end());
                                s.frames.pop_front();
                        }
                        out.text("data", hex(bytes.data(), bytes.size()));
                        out.num("dropped", double(s.lost));
                } else {
                        auto epoch = integer(q.p, "epoch", 9007199254740991ULL);
                        s.check(epoch);
                        if (op == "start") {
                                auto *list = cJSON_GetObjectItemCaseSensitive(q.p, "blocks");
                                if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) < 1 || cJSON_GetArraySize(list) > 32)
                                        throw std::runtime_error("Expected 1..32 blocks");
                                std::vector<Block> blocks;
                                uint64_t           total = 0;
                                for (auto *b = list->child; b; b = b->next) {
                                        auto addr = integer(b, "address"), size = integer(b, "size");
                                        range(addr, size);
                                        total += size;
                                        if (total > 4096)
                                                throw std::runtime_error("Sample frame too large");
                                        blocks.push_back({uint32_t(addr), uint32_t(size)});
                                }
                                int period = int(std::clamp<uint64_t>(integer(q.p, "periodUs", 60000000), 20000, 60000000));
                                s.start(blocks, period, epoch);
                                out.num("periodUs", period);
                                out.num("frameSize", s.frameSize);
                        } else if (op == "read" || op == "write") {
                                auto addr = integer(q.p, "address"), size = integer(q.p, "size");
                                range(addr, size);
                                if (op == "read") {
                                        std::vector<uint8_t> data(size_t(size), 0);
                                        if (!backend.read(epoch, uint32_t(addr), uint32_t(size), data.data()))
                                                throw std::runtime_error("Read failed");
                                        s.check(epoch);
                                        out.text("data", hex(data.data(), data.size()));
                                } else {
                                        auto data = unhex(text(q.p, "data"));
                                        if (data.size() != size)
                                                throw std::runtime_error("Write length mismatch");
                                        std::lock_guard lock(ownership);
                                        if (owner != s.id || !allowWrites)
                                                throw std::runtime_error("Acquire control before writing");
                                        s.check(epoch);
                                        if (!backend.write(epoch, uint32_t(addr), uint32_t(size), data.data()))
                                                throw std::runtime_error("Write rejected/failed");
                                }
                        } else if (op == "reset") {
                                std::lock_guard lock(ownership);
                                if (owner != s.id || !allowWrites || !backend.device().resettable)
                                        throw std::runtime_error("Reset unavailable without control");
                                s.stop();
                                if (!backend.reset(epoch))
                                        throw std::runtime_error("Reset rejected/failed");
                        } else
                                throw std::runtime_error("Unsupported capability");
                }
                return out.str();
        }
        void handle(Socket socket)
        {
                Session session{*this, sessionCounter.fetch_add(1)};
                Context context(session.id);
                bool    paired = false;
                try {
                        std::string nonce = randomKey();
                        Json        hello;
                        hello.num("protocol", 1);
                        hello.text("nonce", nonce);
                        sendFrame(socket, hello.str());
                        Json auth(receiveFrame(socket, 512));
                        if (!equal(text(auth.p, "proof"), hmac(key, "AVA_PEER/1:" + nonce)))
                                throw WireError("Pairing failed");
                        Channel wire{socket, key, nonce, true};
                        Json    welcome;
                        welcome.flag("ok", true);
                        putDevice(welcome, backend.device(), allowWrites);
                        welcome.text("peer", name);
                        welcome.num("protocol", 1);
                        wire.send(welcome.str());
                        ++authenticated;
                        paired = true;
                        while (alive) {
                                auto        request = wire.receive();
                                std::string response;
                                try {
                                        response = dispatch(session, request);
                                } catch (const std::exception &e) {
                                        Json error;
                                        error.flag("ok", false);
                                        error.text("error", e.what());
                                        response = error.str();
                                }
                                wire.send(response);
                        }
                } catch (...) {
                }
                if (paired)
                        --authenticated;
        }
        void answerDiscovery()
        {
                try {
                        SocketOwner socket{udpSocket(true)};
                        while (alive) {
                                if (!ready(socket.value, false, 200))
                                        continue;
                                char        buffer[256];
                                sockaddr_in from{};
#ifdef _WIN32
                                int length = sizeof(from);
#else
                                socklen_t length = sizeof(from);
#endif
                                int n = int(recvfrom(
                                    socket.value, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &length));
                                if (n <= 0)
                                        continue;
                                std::string query(buffer, size_t(n));
                                if (query.rfind("AVA_PEER_DISCOVER/1 ", 0) != 0)
                                        continue;
                                Json reply;
                                reply.text("query", query);
                                reply.text("name", name);
                                reply.num("protocol", 1);
                                reply.num("port", boundPort);
                                auto bytes = reply.str();
                                sendto(socket.value,
                                       bytes.data(),
                                       int(bytes.size()),
                                       0,
                                       reinterpret_cast<sockaddr *>(&from),
                                       length);
                        }
                } catch (...) {
                } // Manual IP remains available on networks without multicast.
        }
};
Server::Server() : p_(std::make_unique<Impl>()) {}
Server::~Server()
{
        stop();
}
void
Server::start(Backend backend, const std::string &name, uint16_t port, const std::string &key, bool allowWrites)
{
        stop();
        validateKey(key);
        sockets();
        if (!backend.device || !backend.read || !backend.write || !backend.reset)
                throw std::runtime_error("Incomplete device backend");
        SocketOwner listener{socket(AF_INET, SOCK_STREAM, 0)};
        if (listener.value == invalid)
                throw WireError("Cannot create peer listener");
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listener.value, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener.value, 8))
                throw WireError("Peer port unavailable");
#ifdef _WIN32
        int length = sizeof(addr);
#else
        socklen_t length = sizeof(addr);
#endif
        getsockname(listener.value, reinterpret_cast<sockaddr *>(&addr), &length);
        p_->boundPort = ntohs(addr.sin_port);
        nonblock(listener.value);
        p_->backend     = std::move(backend);
        p_->name        = name.substr(0, 128);
        p_->key         = key;
        p_->allowWrites = allowWrites;
        p_->listener    = listener.value;
        listener.value  = invalid;
        p_->alive       = true;
        for (int i = 0; i < 8; ++i)
                p_->workers.emplace_back([this] {
                        while (p_->alive) {
                                if (!ready(p_->listener, false, 200))
                                        continue;
                                Socket s = accept(p_->listener, nullptr, nullptr);
                                if (s == invalid)
                                        continue;
                                nonblock(s);
#ifdef SO_NOSIGPIPE
                                int one = 1;
                                setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                                {
                                        std::lock_guard lock(p_->clientsMutex);
                                        p_->clients.insert(s);
                                }
                                p_->handle(s);
                                {
                                        std::lock_guard lock(p_->clientsMutex);
                                        p_->clients.erase(s);
                                }
                                closeSocket(s);
                        }
                });
        p_->discovery = std::thread([this] { p_->answerDiscovery(); });
}
void
Server::stop()
{
        p_->alive = false;
        shut(p_->listener);
        {
                std::lock_guard lock(p_->clientsMutex);
                for (auto s : p_->clients)
                        shut(s);
        }
        for (auto &t : p_->workers)
                if (t.joinable())
                        t.join();
        p_->workers.clear();
        if (p_->discovery.joinable())
                p_->discovery.join();
        closeSocket(p_->listener);
        p_->listener = invalid;
        p_->backend  = {};
}
bool
Server::running() const
{
        return p_->alive;
}
int
Server::port() const
{
        return p_->boundPort;
}
int
Server::viewers() const
{
        return p_->authenticated;
}

struct Client::Impl {
        mutable std::mutex       mutex;
        Socket                   socket{invalid};
        std::unique_ptr<Channel> channel;
        std::atomic<bool>        alive{}, control{};
        std::thread              heartbeat;
        Device                   device;
        std::string              error;
        std::atomic<uint64_t>    dropped{};
        std::string              rpc(Json &q)
        {
                if (!alive || !channel)
                        throw WireError("Peer not connected");
                try {
                        channel->send(q.str());
                        auto response = channel->receive();
                        Json r(response);
                        if (!flag(r.p, "ok"))
                                throw std::runtime_error(text(r.p, "error"));
                        return response;
                } catch (const WireError &e) {
                        alive   = false;
                        control = false;
                        error   = e.what();
                        shut(socket);
                        throw;
                }
        }
        void epoch(Json &q) { q.num("epoch", double(device.epoch)); }
};
Client::Client() : p_(std::make_unique<Impl>()) {}
Client::~Client()
{
        close();
}
void
Client::connect(const std::string &host, uint16_t port, const std::string &key)
{
        close();
        validateKey(key);
        sockets();
        SocketOwner socket{::socket(AF_INET, SOCK_STREAM, 0)};
        if (socket.value == invalid)
                throw WireError("Cannot create connection");
        nonblock(socket.value);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
                throw WireError("Enter an IPv4 address");
        ::connect(socket.value, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        if (!ready(socket.value, true, 4000))
                throw WireError("Peer connection timed out");
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
#else
        socklen_t size = sizeof(error);
#endif
        getsockopt(socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size);
        if (error)
                throw WireError("Cannot connect to peer");
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(socket.value, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        Json hello(receiveFrame(socket.value, 512));
        if (integer(hello.p, "protocol") != 1)
                throw WireError("Unsupported peer protocol");
        auto nonce = text(hello.p, "nonce");
        validateKey(nonce);
        Json auth;
        auth.text("proof", hmac(key, "AVA_PEER/1:" + nonce));
        sendFrame(socket.value, auth.str());
        auto channel = std::make_unique<Channel>(Channel{socket.value, key, nonce, false});
        Json welcome(channel->receive());
        if (!flag(welcome.p, "ok"))
                throw WireError("Pairing rejected");
        auto device = getDevice(welcome.p);
        if (!device.ready)
                throw WireError("Remote target is disconnected; connect it on the sharing device first");
        {
                std::lock_guard lock(p_->mutex);
                p_->socket   = socket.value;
                socket.value = invalid;
                p_->channel  = std::move(channel);
                p_->device   = device;
                p_->error.clear();
                p_->dropped = 0;
                p_->alive   = true;
                p_->control = false;
        }
        p_->heartbeat = std::thread([this] {
                for (int tick = 0; p_->alive; ++tick) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        if (tick % 10 != 0)
                                continue;
                        std::lock_guard lock(p_->mutex);
                        try {
                                Json q;
                                q.text("op", "info");
                                Json r(p_->rpc(q));
                                auto current = getDevice(r.p);
                                if (current.epoch != p_->device.epoch || !current.ready) {
                                        p_->alive   = false;
                                        p_->control = false;
                                        p_->error   = "Target changed/disconnected; reconnect explicitly";
                                        shut(p_->socket);
                                } else {
                                        p_->device  = current;
                                        p_->control = flag(r.p, "control");
                                }
                        } catch (const std::exception &e) {
                                p_->error   = e.what();
                                p_->alive   = false;
                                p_->control = false;
                        }
                }
        });
}
void
Client::close()
{
        p_->alive = false;
        {
                std::lock_guard lock(p_->mutex);
                shut(p_->socket);
        }
        if (p_->heartbeat.joinable())
                p_->heartbeat.join();
        std::lock_guard lock(p_->mutex);
        closeSocket(p_->socket);
        p_->socket = invalid;
        p_->channel.reset();
        p_->control = false;
}
bool
Client::connected() const
{
        return p_->alive;
}
bool
Client::ownsControl() const
{
        return p_->control;
}
Device
Client::device() const
{
        std::lock_guard lock(p_->mutex);
        return p_->device;
}
std::string
Client::error() const
{
        std::lock_guard lock(p_->mutex);
        return p_->error;
}
void
Client::claim(bool acquire)
{
        std::lock_guard lock(p_->mutex);
        Json            q;
        q.text("op", "claim");
        q.flag("acquire", acquire);
        Json r(p_->rpc(q));
        p_->control = flag(r.p, "control");
}
bool
Client::read(uint32_t address, uint32_t size, void *data)
{
        std::lock_guard lock(p_->mutex);
        try {
                range(address, size);
                Json q;
                q.text("op", "read");
                p_->epoch(q);
                q.num("address", address);
                q.num("size", size);
                Json r(p_->rpc(q));
                auto bytes = unhex(text(r.p, "data"));
                if (bytes.size() != size)
                        throw WireError("Read length mismatch");
                std::memcpy(data, bytes.data(), size);
                return true;
        } catch (const std::exception &e) {
                p_->error = e.what();
                return false;
        }
}
bool
Client::write(uint32_t address, uint32_t size, const void *data)
{
        std::lock_guard lock(p_->mutex);
        try {
                range(address, size);
                Json q;
                q.text("op", "write");
                p_->epoch(q);
                q.num("address", address);
                q.num("size", size);
                q.text("data", hex(static_cast<const uint8_t *>(data), size));
                p_->rpc(q);
                return true;
        } catch (const std::exception &e) {
                p_->error = e.what();
                return false;
        }
}
bool
Client::reset()
{
        std::lock_guard lock(p_->mutex);
        try {
                Json q;
                q.text("op", "reset");
                p_->epoch(q);
                p_->rpc(q);
                p_->alive   = false;
                p_->control = false;
                shut(p_->socket);
                return true;
        } catch (const std::exception &e) {
                p_->error = e.what();
                return false;
        }
}
int
Client::startSamples(const std::vector<Block> &blocks, int periodUs)
{
        std::lock_guard lock(p_->mutex);
        Json            q;
        q.text("op", "start");
        p_->epoch(q);
        q.num("periodUs", periodUs);
        auto *array = cJSON_AddArrayToObject(q.p, "blocks");
        for (auto b : blocks) {
                auto *v = cJSON_CreateObject();
                cJSON_AddNumberToObject(v, "address", b.address);
                cJSON_AddNumberToObject(v, "size", b.size);
                cJSON_AddItemToArray(array, v);
        }
        Json r(p_->rpc(q));
        return int(integer(r.p, "frameSize", 4100));
}
void
Client::stopSamples()
{
        std::lock_guard lock(p_->mutex);
        if (!p_->alive)
                return;
        try {
                Json q;
                q.text("op", "stop");
                p_->rpc(q);
        } catch (const std::exception &e) {
                p_->error = e.what();
        }
}
int
Client::samples(void *data, uint32_t capacity)
{
        std::lock_guard lock(p_->mutex);
        try {
                Json q;
                q.text("op", "samples");
                q.num("capacity", std::min<uint32_t>(capacity, 32768));
                Json r(p_->rpc(q));
                auto bytes = unhex(text(r.p, "data"));
                if (bytes.size() > capacity)
                        throw WireError("Sample buffer overflow");
                std::memcpy(data, bytes.data(), bytes.size());
                p_->dropped = integer(r.p, "dropped", 9007199254740991ULL);
                return int(bytes.size());
        } catch (const std::exception &e) {
                p_->error = e.what();
                return -1;
        }
}
uint64_t
Client::dropped() const
{
        return p_->dropped;
}
// Hardware owners explicitly stop these before teardown; avoid static destruction order.
Server &
server()
{
        static auto *instance = new Server;
        return *instance;
}
Client &
client()
{
        static auto *instance = new Client;
        return *instance;
}
} // namespace ava_peer
