#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// AVA peer protocol is shared by desktop and Android/JNI. It exports semantic
// memory operations, never raw USB commands or remote DLL/process execution.
namespace ava_peer
{
struct Block {
        uint32_t address{}, size{};
};
struct Device {
        std::string name;
        uint64_t    epoch{};
        bool        ready{}, writable{}, resettable{};
};
struct Backend {
        std::function<Device()>                                         device;
        std::function<bool(uint64_t, uint32_t, uint32_t, void *)>       read;
        std::function<bool(uint64_t, uint32_t, uint32_t, const void *)> write;
        std::function<bool(uint64_t)>                                   reset;
};
struct Peer {
        std::string name, host;
        int         port{};
};
// Guard is also taken by local mutating operations. A remote control lease
// excludes local writes; server callbacks carry the matching session identity.
class WriteGuard
{
        std::unique_lock<std::recursive_mutex> lock_;
        bool                                   allowed_{};

      public:
        WriteGuard();
        explicit operator bool() const { return allowed_; }
};
bool              backendCall();
bool              remoteOwnsWrites();
std::string       randomKey();
std::vector<Peer> discover(int milliseconds = 1200);

class Server
{
        struct Impl;
        std::unique_ptr<Impl> p_;

      public:
        Server();
        ~Server();
        Server(const Server &)            = delete;
        Server &operator=(const Server &) = delete;
        void    start(Backend backend, const std::string &name, uint16_t port, const std::string &key, bool allowWrites);
        void    stop();
        bool    running() const;
        int     port() const;
        int     viewers() const;
};
class Client
{
        struct Impl;
        std::unique_ptr<Impl> p_;

      public:
        Client();
        ~Client();
        Client(const Client &)                = delete;
        Client     &operator=(const Client &) = delete;
        void        connect(const std::string &host, uint16_t port, const std::string &key);
        void        close();
        bool        connected() const;
        bool        ownsControl() const;
        Device      device() const;
        std::string error() const;
        void        claim(bool acquire);
        bool        read(uint32_t address, uint32_t size, void *data);
        bool        write(uint32_t address, uint32_t size, const void *data);
        bool        reset();
        int         startSamples(const std::vector<Block> &blocks, int periodUs);
        void        stopSamples();
        int         samples(void *data, uint32_t capacity);
        uint64_t    dropped() const;
};
Server &server();
Client &client();
} // namespace ava_peer
