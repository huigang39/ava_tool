#include "core/remote_device.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
using namespace ava_peer;
int
main(int argc, char **argv)
{
        std::array<uint8_t, 4096> memory{};
        std::mutex                m;
        std::atomic<uint64_t>     epoch{1};
        std::atomic<int>          writes{};
        Backend                   backend;
        backend.device = [&] { return Device{"Fixture", epoch, true, true, true}; };
        backend.read   = [&](uint64_t e, uint32_t a, uint32_t n, void *out) {
                std::lock_guard l(m);
                if (a >= 0x20000000)
                        a -= 0x20000000;
                if (e != epoch || uint64_t(a) + n > memory.size())
                        return false;
                std::memcpy(out, memory.data() + a, n);
                return true;
        };
        backend.write = [&](uint64_t e, uint32_t a, uint32_t n, const void *in) {
                WriteGuard      guard;
                std::lock_guard l(m);
                if (a >= 0x20000000)
                        a -= 0x20000000;
                if (!guard || e != epoch || uint64_t(a) + n > memory.size())
                        return false;
                std::memcpy(memory.data() + a, in, n);
                ++writes;
                return true;
        };
        backend.reset = [&](uint64_t e) {
                if (e != epoch)
                        return false;
                ++epoch;
                return true;
        };
        if (argc > 1 && std::string(argv[1]) == "--serve") {
                float initial = 1.25f;
                std::memcpy(memory.data(), &initial, 4);
                Server fixture;
                fixture.start(backend, "Desktop fixture", 45993, "0123456789abcdef0123456789abcdef", true);
                std::cout << "READY 45993 0123456789abcdef0123456789abcdef" << std::endl;
                std::string finish;
                std::getline(std::cin, finish);
                fixture.stop();
                return 0;
        }
        auto   key = randomKey();
        Server server;
        server.start(backend, "Test peer", 0, key, true);
        Client a, b;
        bool   rejected = false;
        try {
                Client bad;
                bad.connect("127.0.0.1", server.port(), randomKey());
        } catch (...) {
                rejected = true;
        }
        assert(rejected);
        a.connect("127.0.0.1", server.port(), key);
        b.connect("127.0.0.1", server.port(), key);
        uint32_t input = 0x12345678, out = 0;
        assert(!a.write(4, 4, &input));
        assert(writes == 0);
        a.claim(true);
        assert(a.write(4, 4, &input));
        assert(b.read(4, 4, &out) && out == input);
        rejected = false;
        try {
                b.claim(true);
        } catch (...) {
                rejected = true;
        }
        assert(rejected);
        assert(remoteOwnsWrites());
        {
                WriteGuard local;
                assert(!local);
        }
        a.claim(false);
        b.claim(true);
        assert(b.write(8, 4, &input));
        b.claim(false);
        {
                WriteGuard local;
                assert(local);
        }
        assert(a.startSamples({{4, 4}, {8, 4}}, 1) == 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        std::array<uint8_t, 4096> frames{};
        int                       n = a.samples(frames.data(), frames.size());
        assert(n >= 24 && n % 12 == 0);
        assert(std::memcmp(frames.data() + 4, &input, 4) == 0);
        a.stopSamples();
        assert(!a.read(0xffffffff, 4, &out));
        a.claim(true);
        a.close();
        for (int i = 0; i < 30 && remoteOwnsWrites(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!remoteOwnsWrites());
        b.claim(true);
        assert(b.reset());
        assert(epoch == 2);
        b.close();
        server.stop();
        assert(!server.running());
        assert(writes == 2);
        server.start(backend, "Read-only", 0, key, false);
        a.connect("127.0.0.1", server.port(), key);
        rejected = false;
        try {
                a.claim(true);
        } catch (...) {
                rejected = true;
        }
        assert(rejected);
        a.close();
        server.stop();
        std::cout << "PASS: pairing, read-only access, writer lease, local exclusion, timestamped batched sampling, range "
                     "checks, reset epoch and disconnect release\n";
}
