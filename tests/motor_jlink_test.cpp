#include "core/motor_jlink.hpp"
#include "core/elf_parser.hpp"
#include "core/motor_controller.hpp"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

/**
 * @brief The ELF reader's only platform dependency. This test never loads a J-Link DLL
 */
FILE *nativeFopen(const std::string &path, const char *mode) { return std::fopen(path.c_str(), mode); }
using namespace motor_jlink;
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejected(F action, const char *message) {
    try { action(); } catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}
struct Memory {
    Config cfg;
    std::array<uint64_t, Count> memory{};
    std::vector<std::pair<Field,double>> writes;
    std::atomic<uint64_t> epoch{1};
    std::atomic<bool> refuseWrite{false};
    bool refuseMode = false, refuseEnable = false;
    Memory() {
        for (int i = 0; i < Count; ++i) {
            const bool analog = (i >= TargetPosition && i <= TargetTorque) || (i >= FeedVelocity && i <= InverterTemperature);
            const bool byte = i == Mode || i == ActualMode || i == TargetMode || i == TargetValid || i == Special || i == PositionLatch;
            cfg.fields[i] = {"fixture.axf", preset(Field(i)), uint32_t(0x20000000 + i * 16), uint8_t(byte ? 1 : 4), analog, !analog && i != Special};
        }
        set(Status,0x40); set(CiaEnable,1); set(ShmEnable,1); set(ActiveConfigValid,1); set(Direction,-1);
        set(Position,-73.125); set(Velocity,-60); set(VelocityUnit,2); set(Current,-2);
        set(LoadTorque,-3); set(ElectromagneticTorque,-4); set(TargetPosition,999);
    }
    void set(Field f, double val) {
        memory[f] = 0;
        if (cfg.fields[f].floating) { const float x = float(val); std::memcpy(&memory[f],&x,4); }
        else memory[f] = uint64_t(int64_t(val));
    }
    double get(Field f) {
        if (cfg.fields[f].floating) { float x; std::memcpy(&x,&memory[f],4); return x; }
        const int shift = 64 - cfg.fields[f].size * 8;
        return cfg.fields[f].signedValue ? double(int64_t(memory[f] << shift) >> shift) : double(memory[f]);
    }
    void tick() {
        if (!refuseMode && get(Mode) != get(ActualMode)) {
            const int old = int(get(ActualMode));
            const int mode = int(get(Mode));
            auto pos = [](int m){return m == 8 || m == -8 || m == 1 || m == -1 || m == -128;};
            set(ActualMode,mode);
            if (pos(mode) && !pos(old)) { set(TargetPosition,get(Position) * get(Direction)); set(PositionLatch,1); }
            set(TargetVelocity,0); set(TargetCurrent,0); set(TargetTorque,0);
        }
        if (get(Special) == 1) set(Status,0x40);
        else if (get(Special) == 6) set(Status,0x40);
        else if (!get(Special)) {
            const auto control = uint32_t(get(Control));
            if (!control) set(Status,0x40);
            else if (control == 6) set(Status,0x21);
            else if (control == 7) set(Status,0x23);
            else if ((control & 15) == 15 && !refuseEnable) set(Status,0x27 | ((control & 16) ? 0x1000 : 0));
        }
    }
    Io io() {
        return {[&]{return epoch.load();},
            [&](uint64_t e,uint32_t a,uint32_t n,void *dst){
                if (e != epoch || a < 0x20000000 || (a-0x20000000)%16) return false;
                const int f = (a-0x20000000)/16;
                if (f >= Count || n != cfg.fields[f].size) return false;
                tick(); std::memcpy(dst,&memory[f],n); return true;
            },
            [&](uint64_t e,uint32_t a,uint32_t n,const void *src){
                if (refuseWrite || e != epoch || a < 0x20000000 || (a-0x20000000)%16) return false;
                const int f = (a-0x20000000)/16;
                if (f >= Count || n != cfg.fields[f].size) return false;
                memory[f] = 0; std::memcpy(&memory[f],src,n); writes.push_back({Field(f),get(Field(f))}); tick(); return true;
            }};
    }
};
static Memory *actorMemory = nullptr;
motor_jlink::Io motor_jlink::portIo() {
    if (!actorMemory) throw std::runtime_error("fixture not selected");
    return actorMemory->io();
}
template<class F> MotorController::State waitActor(MotorController &controller, F condition, bool pulse = true) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (pulse) controller.pulse(0);
        auto state = controller.state();
        if (!state.busy && condition(state)) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("actor timeout: " + controller.state().message);
}
int main(int argc, char **argv) {
    try {
        // Read the actual ELF when supplied, without writing it or running firmware
        if (argc > 1) {
            ElfParser elf; check(elf.parse(argv[1]), "parse CM7 ELF");
            const auto all = symbols(elf.getDwarfInfo(), argv[1]);
            Config config;
            for (int i = 0; i < Count; ++i) {
                for (const auto &s : all) if (s.name == preset(Field(i))) config.fields[i] = s;
                check(config.fields[i].address != 0, preset(Field(i)));
                std::cout << preset(Field(i)) << " @ " << std::hex << config.fields[i].address << std::dec << " size=" << int(config.fields[i].size) << '\n';
            }
            validate(config);
            std::cout << "CM7 ELF preset resolution PASS\n";
        }
        Memory m; Session s(m.io()); s.connect(m.cfg);
        check(!(s.feedback().flags & AVA_ETH_ENABLED), "connect enabled motor");
        check(m.get(ShmEnable) == 0, "SHM still overwrites J-Link commands");
        auto f = s.feedback();
        check(f.position == 73.125f && std::abs(f.velocity - 6.2831853f) < 1e-5 && f.torque == 3 && f.current == 2, "direction/units/torque");
        for (int mode : {8,-8,1,-1,-128}) {
            s.setMode({0,9,999,10,2,3});
            auto t = s.setMode({0,mode,999,10,2,3});
            check(t.position == 73.125f && t.velocity == 0 && t.current == 0 && t.torque == 0, "stale target replayed on position mode entry");
            check(m.get(TargetPosition) == 73.125 && m.get(Mode) == mode, "signed mode / position write");
            s.enable();
            check(s.feedback().flags & AVA_ETH_ENABLED, "enable handshake");
            t.position = 80; s.apply(t);
            check(m.get(TargetPosition) == 80 && m.get(PositionLatch) == 0, "explicit target not released from latch");
            auto switched = s.setMode({0,mode == 8 ? -8 : 8,999,3,4,5});
            check(switched.position == 73.125f && (s.feedback().flags & AVA_ETH_ENABLED), "enabled mode change dropped enable / replayed target");
            s.stop();
        }
        s.setMode({0,9,0,0,0,0});
        s.apply({0,9,0,6.2831853f,0,0});
        check(std::abs(m.get(TargetVelocity) - 60) < 1e-4, "target speed conversion");
        s.special(6); s.special(0); check(!(s.feedback().flags & AVA_ETH_ENABLED), "clear fault/NONE re-enabled");
        const auto beat = m.get(DriveHeartbeat);
        s.feedback();
        check(m.get(DriveHeartbeat) == beat + 1, "commissioning heartbeat not advanced");
        auto legacy = m.cfg; legacy.fields[DriveHeartbeat] = {};
        validate(legacy); // Older firmware must remain connectable.
        auto bad = m.cfg; bad.fields[Control] = bad.fields[Status];
        rejected([&]{validate(bad);}, "overlapping control/status mapping accepted");
        bad = m.cfg; bad.fields[Mode].signedValue = false;
        rejected([&]{validate(bad);}, "unsigned mode accepted");
        bad = m.cfg; bad.fields[TargetPosition].floating = false;
        rejected([&]{validate(bad);}, "wrong target type accepted");
        s.disconnect(); check(m.get(ShmEnable) == 0, "disconnect revived old SHM enable command");
        m.refuseEnable = true; s.connect(m.cfg);
        rejected([&]{s.enable();}, "unacknowledged enable accepted"); s.stop();
        m.refuseMode = true;
        rejected([&]{s.setMode({0,8,999,0,0,0});}, "unacknowledged mode accepted"); s.stop();
        m.refuseMode = false;
        m.set(AutoEnable,1); rejected([&]{s.apply({0,9,0,0,0,0});}, "competing owner accepted"); m.set(AutoEnable,0);
        const auto writes = m.writes.size(); ++m.epoch;
        rejected([&]{s.stop();}, "reset target accepted stale command");
        check(m.writes.size() == writes, "write after target reset");
        Memory active; active.set(Special,2); active.set(Status,0x27); Session blocked(active.io());
        rejected([&]{blocked.connect(active.cfg);}, "connected while enabled");
        check(active.writes.empty(), "changed active motor before rejecting connection");
        Memory actorFixture; actorMemory = &actorFixture;
        actorFixture.set(Mode,8); actorFixture.set(ActualMode,8);
        MotorController controller;
        MotorController::Command command;
        command.config.backend = motor_jlink::backend; command.config.jlink = actorFixture.cfg;
        command.action = MotorController::Action::Connect; controller.post(command);
        auto connected = waitActor(controller,[](const auto &s){return s.connected && s.snapshot.count == 1;});
        check(connected.targets[0].position == 73.125f && !(connected.snapshot.axes[0].flags & AVA_ETH_ENABLED), "actor connection/current position seed");
        command.action = MotorController::Action::Enable; command.target={0,8,999,2,3,4}; controller.post(command);
        auto enabled = waitActor(controller,[](const auto &s){return (s.snapshot.axes[0].flags & AVA_ETH_ENABLED) != 0;});
        check(enabled.targets[0].position == 73.125f && enabled.target_sync_revisions[0] > connected.target_sync_revisions[0], "actor initial enable replayed draft");
        command.action = MotorController::Action::SetMode; command.target={0,-8,999,2,3,4}; controller.post(command);
        auto changed = waitActor(controller,[](const auto &s){return s.snapshot.axes[0].mode == -8;});
        check(changed.targets[0].position == 73.125f && changed.mode_revisions[0] && (changed.snapshot.axes[0].flags & AVA_ETH_ENABLED), "actor mode change / editor revision");
        waitActor(controller,[](const auto &s){return !(s.snapshot.axes[0].flags & AVA_ETH_ENABLED);},false);
        command.action = MotorController::Action::Enable; command.target={0,-8,73.125f,0,0,0}; controller.pulse(0); controller.post(command);
        waitActor(controller,[](const auto &s){return (s.snapshot.axes[0].flags & AVA_ETH_ENABLED) != 0;});
        ++actorFixture.epoch;
        auto lost = waitActor(controller,[](const auto &s){return !s.connected && s.error;});
        check(!lost.snapshot.count, "actor kept stale feedback after reset");
        controller.shutdown(); actorMemory = nullptr;
        Memory writeFailure; actorMemory = &writeFailure;
        MotorController failingController;
        command.action=MotorController::Action::Connect; command.config.jlink=writeFailure.cfg;
        failingController.post(command); waitActor(failingController,[](const auto &s){return s.connected;});
        command.action=MotorController::Action::Enable; command.target={0,8,999,0,0,0};
        failingController.post(command); waitActor(failingController,[](const auto &s){return (s.snapshot.axes[0].flags & AVA_ETH_ENABLED) != 0;});
        writeFailure.refuseWrite=true;
        command.action=MotorController::Action::Disconnect; failingController.post(command);
        auto unconfirmed=waitActor(failingController,[](const auto &s){return !s.connected && s.error;});
        check(unconfirmed.message.find("失能未确认") != std::string::npos,"failed disable was reported as successful disconnect");
        failingController.shutdown(); actorMemory=nullptr;
        std::cout << "J-Link actor connection, target/editor synchronization, UI lease and reset invalidation PASS\n";
        std::cout << "J-Link mapping, mode handover, CiA402 handshake, ownership, failures and reset guards PASS\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
