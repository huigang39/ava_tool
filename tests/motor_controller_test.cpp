#include "core/motor_controller.hpp"
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

/**
 * @brief Actor regressions below use SDK fixtures only; never resolve a physical probe
 */
motor_jlink::Io motor_jlink::portIo() { return {[] { return uint64_t(0); }, {}, {}}; }

static void
check(bool val, const char *message)
{
        if (!val)
                throw std::runtime_error(message);
}

template <class Predicate>
static MotorController::State
waitFor(MotorController &controller, Predicate predicate, bool pulse = true)
{
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
                if (pulse)
                        controller.pulse(0);
                auto state = controller.state();
                if (!state.busy && predicate(state))
                        return state;
                std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Timed out: " + controller.state().message);
}

int
main(int argc, char **argv)
{
        try {
                check(argc == 2 || argc == 3, "SDK library path required");
                MotorController          controller;
                MotorController::Command command;
                command.action         = MotorController::Action::Connect;
                command.config.library = argv[1];
                command.config.axes    = 2;
                command.action = MotorController::Action::WritePid;
                command.pid.values[2] = 2.5f; command.pidMask = 4;
                controller.post(command);
                auto pidState = waitFor(controller, [](const auto &s) { return s.error; });
                check(!pidState.connected && !pidState.pidPending && !pidState.pidSentMask,
                      "live PID requires an existing connection");
                command.action = MotorController::Action::Connect;
                controller.post(command);
                auto state = waitFor(controller, [](const auto &s) { return s.connected && s.snapshot.count == 2; });
                check(!(state.snapshot.axes[0].flags & AVA_ETH_ENABLED), "connect must not enable");
                check(state.info.axes == 2 && !state.diagnostics_valid, "simulation capabilities");
                command.action = MotorController::Action::SetMode;
                command.target = {0, 8, 999, 0, 0, 0};
                controller.post(command);
                state = waitFor(controller, [](const auto &s) { return s.mode_revisions[0] == 1; });
                check(!(state.snapshot.axes[0].flags & AVA_ETH_ENABLED), "disabled mode switch must not enable");
                check(state.targets[0].position == state.snapshot.axes[0].position,
                      "disabled position switch starts at feedback, not stale target");
                command.action = MotorController::Action::Enable;
                command.axis   = 0;
                command.target = {0, -9, 0, 1, 0, 0};
                controller.post(command);
                state = waitFor(controller, [](const auto &s) { return s.snapshot.axes[0].velocity == 1; });
                check(state.snapshot.axes[0].flags & AVA_ETH_ENABLED, "selected axis enabled");
                check(!(state.snapshot.axes[1].flags & AVA_ETH_ENABLED), "other axis remains disabled");
                for (int i = 0; i < 40; ++i) {
                        controller.pulse(0);
                        std::this_thread::sleep_for(10ms);
                }
                check(controller.state().snapshot.axes[0].flags & AVA_ETH_ENABLED, "live UI renews lease");
                state = waitFor(controller, [](const auto &s) { return !(s.snapshot.axes[0].flags & AVA_ETH_ENABLED); }, false);
                check(state.snapshot.axes[0].velocity == 0, "stale UI stops simulation");
                controller.pulse(0);
                controller.post(command);
                waitFor(controller, [](const auto &s) { return s.snapshot.axes[0].velocity == 1; });
                command.action          = MotorController::Action::Apply;
                command.target.velocity = std::numeric_limits<float>::quiet_NaN();
                controller.post(command);
                state =
                    waitFor(controller, [](const auto &s) { return s.error && !(s.snapshot.axes[0].flags & AVA_ETH_ENABLED); });
                check(state.targets[0].velocity == 1, "invalid target was not committed");
                command.action          = MotorController::Action::Enable;
                command.target.velocity = 2;
                controller.post(command);
                waitFor(controller, [](const auto &s) { return s.snapshot.axes[0].velocity == 2; });
                command.action = MotorController::Action::Stop;
                controller.post(command);
                state = waitFor(controller, [](const auto &s) { return s.snapshot.axes[0].velocity == 0; });
                check(!(state.snapshot.axes[0].flags & AVA_ETH_ENABLED), "stop all axes");
                command.action = MotorController::Action::Disconnect;
                controller.post(command);
                waitFor(controller, [](const auto &s) { return !s.connected; });
                command.action          = MotorController::Action::Connect;
                command.config.library += ".missing";
                controller.post(command);
                state = waitFor(controller, [](const auto &s) { return !s.connected && s.error; });
                check(state.message.find("load") != std::string::npos || state.message.find(".missing") != std::string::npos,
                      "DLL load failure is visible");
                controller.shutdown();
                if (std::getenv("AVA_TEST_NPCAP_NAMES")) {
                        MotorController scanner;
                        MotorController::Command scan;
                        scan.action=MotorController::Action::Scan;
                        scan.config.backend=AVA_ETH_NPCAP; scan.config.library=argv[1];
                        scanner.post(scan);
                        auto found=waitFor(scanner,[](const auto &s) { return !s.devices.empty() || s.error; });
                        check(!found.error && !found.devices.empty(),"read-only Npcap enumeration");
                        int named=0;
                        for (const auto &device : found.devices)
                                if (device.name!=device.path) {
                                        ++named;
                                        std::cout<<"Npcap adapter: "<<device.name<<"\n";
                                }
                        check(named>0,"Npcap paths failed to match Windows friendly names");
                }
                if (argc == 3) {
                        for (int mode : {8,-8,1,-1,-128}) {
                                MotorController initial;
                                MotorController::Command c;
                                c.action=MotorController::Action::Connect; c.config.library=argv[2];
                                c.config.backend=AVA_ETH_NPCAP; c.config.device="live-mode";
                                initial.post(c);
                                auto connected=waitFor(initial,[](const auto &s){return s.snapshot.axes[0].position==73.125f;});
                                check(connected.targets[0].position==73.125f && !connected.targets[0].mode,
                                      "first feedback seeds position editor without sending a mode");
                                c.action=MotorController::Action::Enable; c.target={0,mode,999,7,8,9};
                                initial.post(c);
                                auto enabled=waitFor(initial,[](const auto &s){return s.snapshot.axes[0].flags&AVA_ETH_ENABLED;});
                                check(enabled.targets[0].position==73.125f && enabled.targets[0].velocity==0 &&
                                      enabled.targets[0].current==0 && enabled.targets[0].torque==0,
                                      "first position enable replays an old target instead of holding current feedback");
                                check(enabled.mode_revisions[0]==0 && enabled.target_sync_revisions[0]>connected.target_sync_revisions[0],
                                      "Enable path must synchronize GUI even without a SetMode revision");
                        }
                        {
                                MotorController explicitTarget;
                                MotorController::Command c;
                                c.action=MotorController::Action::Connect; c.config.library=argv[2];
                                c.config.backend=AVA_ETH_NPCAP; c.config.device="live-mode"; explicitTarget.post(c);
                                waitFor(explicitTarget,[](const auto &s){return s.snapshot.axes[0].position==73.125f;});
                                c.action=MotorController::Action::Apply; c.target={0,8,17.25f,0,0,0}; explicitTarget.post(c);
                                waitFor(explicitTarget,[](const auto &s){return s.targets[0].position==17.25f;});
                                c.action=MotorController::Action::Enable; explicitTarget.post(c);
                                auto enabled=waitFor(explicitTarget,[](const auto &s){return s.snapshot.axes[0].flags&AVA_ETH_ENABLED;});
                                check(enabled.targets[0].position==17.25f,"explicitly confirmed target must survive enable");
                        }
                        {
                                MotorController live;
                                MotorController::Command c;
                                c.action=MotorController::Action::Connect;
                                c.config.library=argv[2]; c.config.backend=AVA_ETH_NPCAP; c.config.device="live-pid";
                                live.post(c);
                                waitFor(live,[](const auto &s){return s.connected;});
                                c.action=MotorController::Action::Enable; c.target={0,-9,12,3,0,0}; live.post(c);
                                waitFor(live,[](const auto &s){return s.snapshot.axes[0].flags&AVA_ETH_ENABLED;});
                                c.action=MotorController::Action::WritePid; c.pidMask=4; c.pid.values[2]=2.5f; live.post(c);
                                waitFor(live,[](const auto &s){return s.pidPending;});
                                auto sent=waitFor(live,[](const auto &s){return s.pidRevision==1;});
                                check(!sent.pidPending && sent.pidSentMask==4 && sent.pid.values[2]==2.5f,"online PID completion");
                                check((sent.snapshot.axes[0].flags&AVA_ETH_ENABLED) && sent.targets[0].velocity==3,
                                      "PID preserves enable and targets while actor renews heartbeat");
                                c.pid.values[2]=-1; live.post(c);
                                auto rejected=waitFor(live,[](const auto &s){return s.error;});
                                check(rejected.snapshot.axes[0].flags&AVA_ETH_ENABLED,"invalid PID must not stop active motion");
                                c.action=MotorController::Action::Disconnect; live.post(c);
                                auto closed=waitFor(live,[](const auto &s){return !s.connected;});
                                check(!closed.pidPending,"disconnect clears PID progress");
                        }
                        MotorController          fault;
                        MotorController::Command open;
                        open.action         = MotorController::Action::Connect;
                        open.config.library = argv[2];
                        open.config.backend = AVA_ETH_DEVICE_HARDWARE;
                        fault.post(open);
                        const auto failure =
                            waitFor(fault, [](const auto &s) { return s.error && (s.snapshot.axes[0].flags & AVA_ETH_FAULT); });
                        check(!(failure.snapshot.axes[0].flags & AVA_ETH_FEEDBACK_VALID), "fixture feedback invalidated");
                        check(failure.message.find("0x80000001") != std::string::npos &&
                                  failure.message.find("PDO receive timeout") != std::string::npos,
                              "disabled-axis timeout is reported with its cause");
                        check(failure.diagnostics_valid && failure.diagnostics.timeouts == 1 &&
                                  failure.diagnostics.receive_timeout_us == 5000,
                              "failure diagnostics remain readable");
                        MotorController timeout;
                        open.config.backend = AVA_ETH_NPCAP;
                        open.config.device = "enable-timeout";
                        timeout.post(open);
                        waitFor(timeout, [](const auto &s) {
                                return s.connected && (s.snapshot.axes[0].flags & AVA_ETH_FEEDBACK_VALID);
                        });
                        MotorController::Command enable;
                        enable.action = MotorController::Action::Enable;
                        enable.target = {0, -9, 0, 1, 0, 0};
                        timeout.post(enable);
                        const auto rejected = waitFor(timeout, [](const auto &s) {
                                return s.error && !(s.snapshot.axes[0].flags & AVA_ETH_FEEDBACK_VALID);
                        });
                        check(rejected.message.find("使能失败") != std::string::npos &&
                              rejected.message.find("使能确认超时") != std::string::npos &&
                              rejected.message.find("SW=0x0040") != std::string::npos,
                              "Npcap enable failure keeps operation and diagnostic details");
                        check(rejected.diagnostics_valid, "enable failure retains diagnostics after stop");
                        check(!rejected.elevation_required, "PDO timeout must not request administrator rights");
                        for (const auto *device : {"access-denied", "npcap-access-denied"}) {
                                MotorController denied;
                                open.config.device = device;
                                denied.post(open);
                                const auto needsRights = waitFor(denied, [](const auto &s) { return s.error; });
                                check(!needsRights.connected && needsRights.elevation_required &&
                                      needsRights.elevation_revision == 1, "device permission failure not distinguished");
                                // A subsequent ordinary connection clears the permission prompt
                                open.config.device = "enable-timeout";
                                denied.post(open);
                                const auto recovered = waitFor(denied, [](const auto &s) { return s.connected; });
                                check(!recovered.elevation_required, "permission failure persisted after reconnect");
                        }
                }
                std::cout << "Motor controller: connection, per-axis control, UI lease, invalid target, stop, disconnect and "
                             "DLL failure passed\n";
                return 0;
        } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                return 1;
        }
}
