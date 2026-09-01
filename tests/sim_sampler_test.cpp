// Use the application's real initialization, globals and logging, but a test entry point
#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "cJSON.h"
#include <fstream>
#include <stdexcept>

struct SimSamplerTestAccess {
    static void save(Gui &gui, const std::string &path) { gui.saveSession(path); }
};

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

static void make_session(const std::string &path, const std::string &region, unsigned fieldCount = ava_sim::fieldCount) {
    cJSON *root = cJSON_CreateObject();
    cJSON *monitors = cJSON_AddArrayToObject(root, "monitors");
    for (const char *name : {"first", "second"}) {
        cJSON *monitor = cJSON_CreateObject();
        cJSON_AddStringToObject(monitor, "name", name);
        cJSON_AddNumberToObject(monitor, "historySeconds", 10);
        cJSON *scopes = cJSON_AddArrayToObject(monitor, "scopes");
        for (unsigned scopeIndex = 0; scopeIndex < 2; ++scopeIndex) {
            cJSON *scope = cJSON_CreateObject();
            cJSON_AddStringToObject(scope, "name", scopeIndex ? "other" : "main");
            cJSON *channels = cJSON_AddArrayToObject(scope, "channels");
            for (unsigned field = 0; field < fieldCount; ++field) {
                cJSON *ch = cJSON_CreateObject();
                // Channel names deliberately differ from protocol field names
                cJSON_AddStringToObject(ch, "name", ("alias_" + std::to_string(field)).c_str());
                cJSON_AddStringToObject(ch, "device", ava_sim::device);
                cJSON_AddStringToObject(ch, "type", "f32");
                cJSON_AddStringToObject(ch, "shmRegionName", region.c_str());
                cJSON_AddNumberToObject(ch, "simField", field);
                cJSON_AddBoolToObject(ch, "writable", false);
                cJSON_AddItemToArray(channels, ch);
            }
            cJSON_AddItemToArray(scopes, scope);
        }
        cJSON_AddItemToArray(monitors, monitor);
    }
    char *text = cJSON_Print(root);
    std::ofstream(path) << text;
    cJSON_free(text);
    cJSON_Delete(root);
}

static MonitorChannel &channel(Gui &gui, const char *monitor, const char *scope, unsigned field) {
    return *gui.getMonitors().at(monitor)->getScopes().at(scope)->findChannel("alias_" + std::to_string(field));
}

struct SimulationChild {
    PROCESS_INFORMATION process{};
    ~SimulationChild() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 2000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
    }
};

static void real_simulation_loop(Gui &gui, const std::string &exe, const std::string &model, const std::string &region) {
    // Queue a target before the producer exists, through the actual channel API
    {
        std::lock_guard lock(gui.getMonitorMtx());
        auto &ch = channel(gui, "first", "main", 0);
        ch.setWVal(0.5f); ch.markWValDirty();
    }
    SimulationChild child;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    std::string command = "\"" + exe + "\" --headless --steps 8000 --realtime --reset-at 2000 --model \"" +
        model + "\" --shm-name \"" + region + "\"";
    check(CreateProcessA(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child.process) != FALSE, "start isolated real simulation");
    auto wait_for = [&](auto predicate, const char *message) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(gui.getMonitorMtx());
                if (predicate()) return;
            }
            check(WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT, "simulation stays running");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error(message);
    };
    wait_for([&]() {
        return channel(gui, "first", "main", 0).getDispVal() == 0.5f &&
               channel(gui, "first", "main", 1).getDispVal() > 0.1f;
    }, "queued target reaches actual simulation and moves the joint");
    {
        std::lock_guard lock(gui.getMonitorMtx());
        auto &ch = channel(gui, "second", "other", 2);
        ch.setWVal(-0.15f); ch.markWValDirty();
    }
    wait_for([&]() { return std::abs(channel(gui, "first", "main", 2).getDispVal() + 0.15f) < 1e-6f; },
             "velocity command reaches actual simulation");
    {
        std::lock_guard lock(gui.getMonitorMtx());
        const float position = channel(gui, "first", "main", 1).getDispVal();
        const float velocity = channel(gui, "first", "main", 3).getDispVal();
        const float expected = 100.0f * (0.5f - position) + 10.0f * (-0.15f - velocity);
        check(std::abs(channel(gui, "first", "main", 4).getDispVal() - expected) < 1e-4f,
              "actual actuator torque uses configured references");
        auto &ch = channel(gui, "first", "main", 0);
        ch.setWVal(10); ch.markWValDirty();
    }
    wait_for([&]() { return channel(gui, "first", "main", 0).getDispVal() == 10.0f; },
             "unclamped position target is echoed through actual sampler");
    wait_for([&]() {
        return channel(gui, "first", "main", 0).getDispVal() == 0 &&
               channel(gui, "first", "main", 2).getDispVal() == 0;
    }, "real simulation reset zeros targets");
    {
        std::lock_guard lock(gui.getMonitorMtx());
        auto &ch = channel(gui, "first", "main", 0);
        ch.setWVal(-0.4f); ch.markWValDirty();
    }
    wait_for([&]() { return channel(gui, "second", "other", 0).getDispVal() == -0.4f; },
             "post-reset target reaches new epoch");
    {
        std::lock_guard lock(gui.getMonitorMtx());
        auto &p = channel(gui, "first", "main", 6);
        auto &d = channel(gui, "second", "other", 7);
        p.setWVal(220); p.markWValDirty();
        d.setWVal(0); d.markWValDirty();
    }
    wait_for([&]() {
        return channel(gui, "first", "main", 6).getDispVal() == 220 &&
               channel(gui, "first", "main", 7).getDispVal() == 0;
    }, "PD gains reach real simulation and echo back");
    {
        std::lock_guard lock(gui.getMonitorMtx());
        const float q = channel(gui, "first", "main", 1).getDispVal();
        check(std::abs(channel(gui, "first", "main", 4).getDispVal() - 220 * (-0.4f - q)) < 2e-4f,
              "actual torque uses updated PD gains");
    }
    {
        std::lock_guard lock(gui.getMonitorMtx());
        auto &ch = channel(gui, "first", "main", 0);
        ch.waveCfgPending_.freq.store(0);
        ch.waveCfgPending_.amp.store(0);
        ch.waveCfgPending_.offset.store(-0.25f);
        ch.waveCfgPending_.dirty.store(true);
        ch.waveEnable_ = true;
    }
    wait_for([&]() { return channel(gui, "first", "main", 0).getDispVal() == -0.25f; },
             "wave generator drives actual MuJoCo reference with applied-value feedback");
    puts("PASS: actual AvA sampler -> separate command SHM -> real MuJoCo -> applied-reference feedback");
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) return 2;
    module_init();
    int result = 0;
    std::thread sampler;
    std::unique_ptr<Gui> gui;
    try {
        const std::string session = argv[1];
        const std::string region = "AvA.SamplerTest." + std::to_string(GetCurrentProcessId());
        make_session(session, region);
        // Initialize the real GUI without opening a visible interactive window
        check(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(session);
        check(gui->getMonitors().size() == 2, "session loads two monitors");
        ava_sim::Publisher writer;
        check(writer.open(region), "publisher open");
        g_samplerRunMode.store(0);
        sampler = std::thread(threadFunc, gui.get());
        auto wait = [&](uint64_t frames) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (gui->getMonitors().at("first")->simFramesReceived_.load() < frames &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(gui->getMonitors().at("first")->simFramesReceived_.load() == frames, "sampler frame count");
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        };
        auto send = [&](uint64_t epoch, uint64_t sequence, double time) {
            ava_sim::Sample sample;
            sample.epoch = epoch; sample.sequence = sequence; sample.time = time;
            sample.q_ref = float(sequence + 1); sample.q = float(sequence);
            sample.dq_ref = float(sequence + 3); sample.dq = float(sequence + 2); sample.tau = float(sequence + 4);
            check(writer.publish(sample), "publish test sample");
        };
        for (uint64_t i = 0; i < 100; ++i) send(7, i, i * 0.001);
        wait(100);
        {
            std::lock_guard lock(gui->getMonitorMtx());
            for (const char *monitor : {"first", "second"})
                for (const char *scope : {"main", "other"})
                    for (unsigned field = 0; field < ava_sim::fieldCount; ++field) {
                        auto &ch = channel(*gui, monitor, scope, field);
                        check(ch.isWritable() == ava_sim::writableField(field) && ch.getSimField() == field,
                              "legacy sessions migrate only reference fields to writable");
                        check(ch.read_.rawSize() == 100, "fanout retains every frame in every sink");
                        for (unsigned i = 0; i < 100; ++i) {
                            check(std::abs(ch.read_.rawTs(i) - i * 0.001) < 1e-7, "all sinks share simulation time");
                            const float expected = field == 0 ? float(i + 1) : field == 1 ? float(i) :
                                field == 2 ? float(i + 3) : field == 3 ? float(i + 2) : field == 4 ? float(i + 4) : field == 5 ? 1.0f : field == 6 ? 100.0f : 10.0f;
                            check(ch.read_.rawVal(i) == expected, "field selection and derived error");
                        }
                    }
            gui->getMonitors().at("first")->setSamplingPaused(true);
        }
        for (uint64_t i = 100; i < 120; ++i) send(7, i, i * 0.001);
        wait(120);
        {
            std::lock_guard lock(gui->getMonitorMtx());
            check(channel(*gui, "first", "main", 0).read_.rawSize() == 100, "paused monitor discards frames");
            check(channel(*gui, "second", "other", 0).read_.rawSize() == 120, "other monitor continues sampling");
            gui->getMonitors().at("first")->setSamplingPaused(false);
        }
        send(8, 0, 0);
        send(8, 1, 0.001);
        wait(122);
        {
            std::lock_guard lock(gui->getMonitorMtx());
            for (const char *monitor : {"first", "second"}) {
                auto &ch = channel(*gui, monitor, "main", 0);
                check(ch.read_.rawSize() == 2 && ch.read_.rawTs(0) == 0, "reset replaces old epoch history");
            }
        }
        ava_sim::Reader commands;
        check(commands.open(ava_sim::commandRegion(region)), "simulation opens independent command queue");
        auto read_command = [&]() {
            ava_sim::Command command;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!commands.read(command) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(ava_sim::valid(command) && command.epoch == 8, "manual write produces current-epoch reference command");
            return command;
        };
        {
            std::lock_guard lock(gui->getMonitorMtx());
            auto &ch = channel(*gui, "first", "main", 0);
            ch.setWVal(0.65f); ch.markWValDirty();
        }
        auto command = read_command();
        check(command.field == 0 && command.val == 0.65f, "position write reaches command queue");
        {
            std::lock_guard lock(gui->getMonitorMtx());
            auto &ch = channel(*gui, "second", "other", 2);
            ch.setWVal(-0.2f); ch.markWValDirty();
        }
        command = read_command();
        check(command.field == 2 && command.val == -0.2f, "another scope shares the same command producer");
        for (unsigned field : {6u, 7u}) {
            {
                std::lock_guard lock(gui->getMonitorMtx());
                auto &ch = channel(*gui, "first", "main", field);
                ch.setWVal(field == 6 ? 240.0f : 0.0f); ch.markWValDirty();
            }
            command = read_command();
            check(command.field == field && command.val == (field == 6 ? 240.0 : 0.0), "PD gain write reaches command queue");
        }
        {
            std::lock_guard lock(gui->getMonitorMtx());
            auto &ch = channel(*gui, "first", "main", 0);
            ch.waveCfgPending_.freq.store(0);
            ch.waveCfgPending_.amp.store(0);
            ch.waveCfgPending_.offset.store(0.3f);
            ch.waveCfgPending_.dirty.store(true);
            ch.waveEnable_ = true;
        }
        send(8, 2, 0.002);
        wait(123);
        command = read_command();
        check(command.field == 0 && command.val == 0.3f, "basic SIM waveform reaches command queue");
        {
            std::lock_guard lock(gui->getMonitorMtx());
            auto &ch = channel(*gui, "first", "main", 0);
            auto &param = ch.waveCfgPending_.params[MonitorChannel::WaveCfgPending::OFFSET];
            param.start.store(-0.7f); param.end.store(-0.7f);
            ch.waveCfgPending_.mode.store(int(WaveGeneratorMode::DYNAMIC));
            ch.waveCfgPending_.restart.store(true);
        }
        send(8, 3, 0.003);
        wait(124);
        command = read_command();
        check(command.field == 0 && command.val == -0.7f, "dynamic SIM waveform reaches command queue");
        {
            std::lock_guard lock(gui->getMonitorMtx());
            channel(*gui, "first", "main", 0).waveEnable_ = false;
        }
        send(8, 4, 0.004);
        wait(125);
        check(!commands.read(command), "switching off generator stops simulation waveform commands");
        {
            std::lock_guard lock(gui->getMonitorMtx());
            auto &ch = channel(*gui, "first", "main", 1);
            ch.setWritable(true);
            check(!ch.isWritable(), "measured fields cannot become writable");
            ch.setWVal(10); ch.markWValDirty();
            channel(*gui, "first", "main", 0).setWritable(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!commands.read(command), "measured-field write is not sent");
        g_appRunning.store(false);
        sampler.join();
        SimSamplerTestAccess::save(*gui, session);
        gui.reset();
        check(glfwInit() == GLFW_TRUE, "GLFW reinitialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(session);
        {
            std::lock_guard lock(gui->getMonitorMtx());
            for (unsigned field = 0; field < ava_sim::fieldCount; ++field) {
                auto &ch = channel(*gui, "first", "main", field);
                const bool expectedWritable = field == 2 || field == 6 || field == 7; // q_ref was explicitly switched to read-only
                check(ch.getSimField() == field && ch.getShmRegionName() == region && ch.isWritable() == expectedWritable,
                      "save/reload preserves stream mappings");
            }
        }
        gui.reset();
        make_session(session, region, 6);         // Actual old session with no gain fields
        check(glfwInit() == GLFW_TRUE, "GLFW migration initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(session);
        for (const char *name : {"first", "second"}) {
            const auto &scope = gui->getMonitors().at(name)->getScopes().at("PD Parameters");
            for (const char *gain : {"kp", "kd"}) {
                auto *ch = scope->findChannel(gain);
                check(ch && ch->isWritable() && ch->getShmRegionName() == region,
                      "old session automatically gains writable PD table rows");
            }
        }
        SimSamplerTestAccess::save(*gui, session);
        gui.reset();
        check(glfwInit() == GLFW_TRUE, "GLFW migrated reload initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(session);
        check(gui->getMonitors().at("first")->getScopes().at("PD Parameters")->getChannels().size() == 2,
              "PD row migration is idempotent on save/reload");
        auto monitor = gui->getMonitors().at("first");
        monitor->addScope("Targets");
        auto pd = monitor->getScopes().at("PD Parameters");
        auto targets = monitor->getScopes().at("Targets");
        targets->getChannels() = pd->getChannels();
        pd->getChannels().clear();
        targets->setLabel("Targets / PD");
        SimSamplerTestAccess::save(*gui, session);
        gui.reset();
        check(glfwInit() == GLFW_TRUE, "GLFW combined scope reload");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(session);
        check(gui->getMonitors().at("first")->getScopes().at("Targets")->getChannels().empty() &&
              gui->getMonitors().at("first")->getScopes().at("PD Parameters")->getChannels().size() == 2,
              "previous combined PD rows move to independent scope");
        puts("PASS: sampler fanout, position/velocity commands, read-only fields, pause/reset, migration and .ava persistence");
        if (argc == 4) {
            gui.reset();
            const std::string realRegion = region + ".real";
            make_session(session, realRegion);
            check(glfwInit() == GLFW_TRUE, "GLFW real-loop initialization");
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            gui = std::make_unique<Gui>(session);
            g_appRunning.store(true);
            sampler = std::thread(threadFunc, gui.get());
            real_simulation_loop(*gui, argv[2], argv[3], realRegion);
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        result = 1;
    }
    g_appRunning.store(false);
    if (sampler.joinable()) sampler.join();
    gui.reset();
    JLinkPort::instance().close();
    log_deinit(&g_log_ops);
    log_deinit(&g_log_perf);
    return result;
}
