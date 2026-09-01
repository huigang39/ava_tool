#ifndef AVA_SIM_STREAM_HPP
#define AVA_SIM_STREAM_HPP

// Timestamped simulation frames over the existing SHM/SPSC transport
// One producer and one sampler own a region; the sampler fans out to scopes
#include "shm.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#if OS(POSIX)
#include <sys/file.h>
#endif

namespace ava_sim {
inline constexpr size_t regionBytes = 1024 * 1024;
inline constexpr uint32_t magic = 0x41565331; // AVS1
inline constexpr const char *device = "SIM_SHM";
#if OS(WIN)
inline constexpr const char *defaultRegion = "Local\\AvA.MuJoCo.SingleJoint.v1";
#else
inline constexpr const char *defaultRegion = "/ava_mujoco_single_joint_v1";
#endif
inline constexpr const char *fields[] = {"q_ref", "q", "dq_ref", "dq", "tau", "error", "kp", "kd"};
inline constexpr unsigned fieldCount = 8;
inline bool writableField(unsigned field) { return field == 0 || field == 2 || field == 6 || field == 7; }
inline std::string commandRegion(const std::string &telemetryRegion) { return telemetryRegion + ".commands.v1"; }

struct Command {
    uint32_t signature{0x41564331}; // AVC1, separate from telemetry
    uint16_t version{1};
    uint16_t bytes{64};
    uint64_t epoch{}; // Target simulation epoch; old/reset commands are ignored
    uint64_t sequence{};
    uint32_t field{}; // 0 = q_ref, 2 = dq_ref, 6 = kp, 7 = kd
    uint32_t reserved{};
    double val{};
    uint64_t reserved2[3]{};
};
static_assert(sizeof(Command) == 64 && offsetof(Command, val) == 32);
static_assert(std::is_trivially_copyable<Command>::value);
inline bool valid(const Command &c) {
    return c.signature == 0x41564331 && c.version == 1 && c.bytes == sizeof(Command) &&
           writableField(c.field) && std::isfinite(c.val) &&
           std::abs(c.val) <= std::numeric_limits<float>::max();
}

struct Sample {
    uint32_t signature{magic};
    uint16_t version{2};
    uint16_t bytes{72};
    uint64_t epoch{};    // Changes on process restart and simulation reset
    uint64_t sequence{}; // Advances even when a full queue rejects a frame
    double time{};       // Simulation seconds, shared by all fields
    float q_ref{}, q{}, dq_ref{}, dq{}, tau{};
    uint32_t reserved{};
    uint64_t dropped{}; // Cumulative producer rejection count for this run
    float kp{100.0f}, kd{10.0f}; // v2 extension; v1 frames use the original gains
};
static_assert(sizeof(Sample) == 72 && offsetof(Sample, dropped) == 56);
static_assert(std::is_trivially_copyable<Sample>::value);

inline bool valid(const Sample &s) {
    return s.signature == magic && ((s.version == 1 && s.bytes == 64) || (s.version == 2 && s.bytes == sizeof(Sample))) &&
           std::isfinite(s.time) && s.time >= 0 && std::isfinite(s.q_ref) &&
           std::isfinite(s.q) && std::isfinite(s.dq_ref) && std::isfinite(s.dq) && std::isfinite(s.tau) && std::isfinite(s.kp) && std::isfinite(s.kd);
}
inline float val(const Sample &s, unsigned field) {
    switch (field) {
    case 0: return s.q_ref;
    case 1: return s.q;
    case 2: return s.dq_ref;
    case 3: return s.dq;
    case 4: return s.tau;
    case 6: return s.kp;
    case 7: return s.kd;
    default:
        // Embedded control-mode marker uses the existing reserved slot, so the
        // v1/v2 packet size and external producer compatibility remain intact
        return (s.reserved & 0xFFFF0000u) == 0x4D430000u && (s.reserved & 0xFFFFu) == 1u ?
            s.dq_ref - s.dq : s.q_ref - s.q;
    }
}

// OS locks also prevent two application instances from consuming one SPSC
class Gate {
#if OS(WIN)
    HANDLE handle_{};
#else
    int fd_{-1};
#endif
public:
    Gate() = default;
    Gate(const Gate &) = delete;
    Gate &operator=(const Gate &) = delete;
    ~Gate() { close(); }
    bool open(const std::string &name, const char *role) {
        close();
#if OS(WIN)
        handle_ = CreateMutexA(nullptr, FALSE, (name + "." + role).c_str());
        if (!handle_) return false;
        const DWORD result = WaitForSingleObject(handle_, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return true;
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
#else
        std::string key = "/tmp/ava_sim_";
        for (unsigned char c : name) key += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                             (c >= '0' && c <= '9')) ? char(c) : '_';
        key += std::string(".") + role;
        fd_ = ::open(key.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0) return true;
        close();
        return false;
#endif
    }
    void close() {
#if OS(WIN)
        if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); handle_ = nullptr; }
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }
};

class Endpoint {
protected:
    shm region_{};
    std::string name_;
    Gate gate_;
    bool compatible() const {
        return region_.lo.spsc && region_.lo.spsc->cap == regionBytes / 2 &&
               region_.lo.spsc->policy == SPSC_POLICY_REJECT;
    }
public:
    Endpoint() = default;
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    ~Endpoint() { close(); }
    void close() {
        if (region_.lo.addr) shm_deinit(&region_);
        region_ = {};
        gate_.close();
    }
    bool isOpen() const { return region_.lo.addr != nullptr; }
};

class Publisher : public Endpoint {
    uint64_t rejected_{};
public:
    bool open(const std::string &name = defaultRegion) {
        close();
        name_ = name;
        rejected_ = 0;
        if (!gate_.open(name_, "writer")) return false;
        const shm_cfg cfg{name_.c_str(), SHM_READWRITE, regionBytes};
        if (shm_init(&region_, cfg) != 0 || !compatible()) { close(); return false; }
        return true;
    }
    bool publish(Sample sample) {
        if (!isOpen()) return false;
        sample.dropped = rejected_;
        if (shm_write(&region_, &sample, sizeof(sample)) == sizeof(sample)) return true;
        ++rejected_;
        return false;
    }
    uint64_t dropped() const { return rejected_; }
    bool publish(const Command &command) {
        if (!isOpen()) return false;
        if (shm_write(&region_, &command, sizeof(command)) == sizeof(command)) return true;
        ++rejected_;
        return false;
    }
};

class Reader : public Endpoint {
public:
    /**
     * @brief Open only: a missing producer never creates an empty persistent region
     */
    bool open(const std::string &name = defaultRegion) {
        close();
        name_ = name;
        if (!gate_.open(name_, "reader")) return false;
        region_.cfg = {name_.c_str(), SHM_READWRITE, regionBytes};
#if OS(WIN)
        region_.lo.fd = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());
        if (!region_.lo.fd) { gate_.close(); return false; }
        region_.lo.addr = MapViewOfFile(region_.lo.fd, FILE_MAP_ALL_ACCESS, 0, 0, regionBytes);
        if (!region_.lo.addr) { CloseHandle(region_.lo.fd); region_ = {}; gate_.close(); return false; }
#else
        region_.lo.fd = shm_open(name_.c_str(), O_RDWR, 0600);
        if (region_.lo.fd < 0) { gate_.close(); return false; }
        struct stat info{};
        if (fstat(region_.lo.fd, &info) != 0 || info.st_size != static_cast<off_t>(regionBytes)) {
            ::close(region_.lo.fd); region_ = {}; gate_.close(); return false;
        }
        region_.lo.addr = mmap(nullptr, regionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, region_.lo.fd, 0);
        if (region_.lo.addr == MAP_FAILED) {
            ::close(region_.lo.fd); region_ = {}; gate_.close(); return false;
        }
#endif
        region_.lo.spsc = static_cast<spsc *>(region_.lo.addr);
        if (!compatible()) { close(); return false; }
        return true;
    }
    bool read(Sample &sample) {
        if (!isOpen()) return false;
        // 先查看公共头部, 完整帧到达前不移动队列读位置
        Sample next{};
        const auto *buffer = static_cast<const uint8_t *>(region_.lo.addr) + sizeof(spsc);
        if (spsc_peek_buf(region_.lo.spsc, buffer, &next, 64) != 64) return false;
        const size_t size = next.version == 2 && next.bytes == sizeof(Sample) ? sizeof(Sample) : 64;
        if (spsc_avail(region_.lo.spsc) < size || shm_read(&region_, &next, size) != size) return false;
        sample = next;
        return true;
    }
    bool read(Command &command) {
        if (!isOpen() || spsc_avail(region_.lo.spsc) < sizeof(command)) return false;
        return shm_read(&region_, &command, sizeof(command)) == sizeof(command);
    }
};

// Per-channel cursor also handles channels added midway through a run
struct Cursor {
    bool initialized{};
    uint64_t epoch{}, sequence{};
    double time{};
    bool accept(const Sample &s, bool &reset) {
        reset = !initialized || epoch != s.epoch;
        if (!reset && (s.sequence <= sequence || s.time <= time)) return false;
        initialized = true;
        epoch = s.epoch;
        sequence = s.sequence;
        time = s.time;
        return true;
    }
};
} // namespace ava_sim
#endif
