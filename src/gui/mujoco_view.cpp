#include "gui/mujoco_view.hpp"
#include "ckit.h"
#include "gui/choice_control.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "platform/native_dlg.hpp"
#include "sim_stream.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace
{
std::mutex                      simulationRegionsMutex;
std::unordered_set<std::string> simulationRegions;
} // namespace

#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
#include <mujoco/mujoco.h>
#define MJ_FUNCTIONS(X)          \
        X(mj_version)            \
        X(mj_loadXML)            \
        X(mj_makeData)           \
        X(mj_deleteData)         \
        X(mj_deleteModel)        \
        X(mj_step)               \
        X(mj_forward)            \
        X(mj_resetData)          \
        X(mj_name2id)            \
        X(mjv_defaultCamera)     \
        X(mjv_defaultOption)     \
        X(mjv_defaultScene)      \
        X(mjv_makeScene)         \
        X(mjv_freeScene)         \
        X(mjv_updateScene)       \
        X(mjv_moveCamera)        \
        X(mjv_defaultPerturb)    \
        X(mjv_select)            \
        X(mjv_initPerturb)       \
        X(mjv_movePerturb)       \
        X(mjv_applyPerturbForce) \
        X(mjr_defaultContext)    \
        X(mjr_makeContext) X(mjr_freeContext) X(mjr_resizeOffscreen) X(mjr_setBuffer) X(mjr_render) X(mjr_readPixels)
#endif

struct MuJoCoView::Impl {
        std::string                           path, message;
        bool                                  active = false, paused = false;
        std::string                           region;
        ava_sim::Publisher                    publisher;
        ava_sim::Reader                       commands;
        std::uint64_t                         epoch = 0, sequence = 0;
        double                                accumulator = 0;
        ava_mujoco::Control                   control;
        ava_mujoco::Controller                controller;
        unsigned                              commandOverrides = 0;
        float                                 disturbance      = 0;
        ava_actuator::Config                  actuatorConfig, draftConfig;
        bool                                  actuatorModel = false;
        double                                gearRatio     = 1;
        std::vector<ava_actuator::Stage>      stages;
        std::string                           draftSummary, draftError;
        std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        HMODULE dll = nullptr;
#define DECLARE_FN(name) decltype(&::name) name = nullptr;
        MJ_FUNCTIONS(DECLARE_FN)
#undef DECLARE_FN
        mjModel                   *model = nullptr;
        mjData                    *data  = nullptr;
        mjvCamera                  camera{};
        mjvOption                  option{};
        mjvScene                   scene{};
        mjvPerturb                 perturb{};
        int                        dragButton = -1, lastDrawFrame = -1;
        bool                       physicsGesture = false;
        mjrContext                 context{};
        bool                       graphics = false;
        GLuint                     texture  = 0;
        int                        width = 0, height = 0;
        std::vector<unsigned char> pixels;

        double numeric(mjModel *m, const char *name, double fallback)
        {
                int id = mj_name2id(m, mjOBJ_NUMERIC, name);
                return id >= 0 && m->numeric_size[id] == 1 ? m->numeric_data[m->numeric_adr[id]] : fallback;
        }
        void assembly()
        {
                if (!actuatorModel || !data)
                        return;
                auto pose = [&](const std::string &name, double angle, double x, double y, double z) {
                        int body = mj_name2id(model, mjOBJ_BODY, name.c_str());
                        if (body < 0 || model->body_mocapid[body] < 0)
                                return;
                        int id                       = model->body_mocapid[body];
                        data->mocap_pos[3 * id]      = x;
                        data->mocap_pos[3 * id + 1]  = y;
                        data->mocap_pos[3 * id + 2]  = z;
                        data->mocap_quat[4 * id]     = std::cos(angle / 2);
                        data->mocap_quat[4 * id + 1] = data->mocap_quat[4 * id + 2] = 0;
                        data->mocap_quat[4 * id + 3]                                = std::sin(angle / 2);
                };
                double q = data->qpos[0], input = q * gearRatio;
                pose("ava_rotor", input, 0, 0, 0);
                for (size_t i = 0; i < stages.size(); ++i) {
                        auto   st      = stages[i];
                        double carrier = input / st.ratio();
                        double z       = .065 + i * (actuatorConfig.exploded ? .065 : .025);
                        double radius  = .12 / st.ring * (st.sun + st.planet) / 2;
                        auto   prefix  = "ava_stage_" + std::to_string(i);
                        pose(prefix + "_sun", input, 0, 0, z);
                        pose(prefix + "_carrier", carrier, 0, 0, z + .016);
                        for (int p = 0; p < 3; ++p) {
                                double angle = carrier + 2 * ava_actuator::pi * p / 3;
                                double spin  = carrier - double(st.sun) / st.planet * (input - carrier);
                                pose(prefix + "_planet_" + std::to_string(p),
                                     spin,
                                     radius * std::cos(angle),
                                     radius * std::sin(angle),
                                     z);
                        }
                        input = carrier;
                }
                pose("ava_encoder_disk",
                     q,
                     actuatorConfig.eccentricMm * .001 * std::cos(q),
                     actuatorConfig.eccentricMm * .001 * std::sin(q),
                     -.035);
        }

        bool api()
        {
                if (dll)
                        return true;
                dll = LoadLibraryW(L"mujoco.dll");
                if (!dll)
                        dll = LoadLibraryW(L"C:/mujoco/bin/mujoco.dll");
                if (!dll) {
                        message = tr("MuJoCo runtime not found; place mujoco.dll beside AvA Tool",
                                     "未找到 MuJoCo 运行库，请将 mujoco.dll 放在上位机程序旁");
                        return false;
                }
#define LOAD_FN(name)                                                        \
        name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, #name)); \
        if (!name) {                                                         \
                message = "Missing MuJoCo API: " #name;                      \
                FreeLibrary(dll);                                            \
                dll = nullptr;                                               \
                return false;                                                \
        }
                MJ_FUNCTIONS(LOAD_FN)
#undef LOAD_FN
                if (mj_version() != mjVERSION_HEADER) {
                        message = tr("MuJoCo DLL version differs from the build SDK", "MuJoCo DLL 版本与编译时的 SDK 不一致");
                        FreeLibrary(dll);
                        dll = nullptr;
                        return false;
                }
                return true;
        }
        void releaseModel()
        {
                if (graphics) {
                        mjr_freeContext(&context);
                        mjv_freeScene(&scene);
                        graphics = false;
                }
                if (texture) {
                        glDeleteTextures(1, &texture);
                        texture = 0;
                }
                if (data) {
                        mj_deleteData(data);
                        data = nullptr;
                }
                if (model) {
                        mj_deleteModel(model);
                        model = nullptr;
                }
                width = height = 0;
        }
#endif
};

MuJoCoView::MuJoCoView() : impl_(std::make_unique<Impl>()) {}
MuJoCoView::~MuJoCoView()
{
        stop();
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        impl_->releaseModel();
        if (impl_->dll)
                FreeLibrary(impl_->dll);
#endif
}
bool
MuJoCoView::running() const
{
        return impl_->active;
}
const std::string &
MuJoCoView::modelPath() const
{
        return impl_->path;
}
const std::string &
MuJoCoView::error() const
{
        return impl_->message;
}

static std::string
bundledModelPath(const char *filename)
{
        std::filesystem::path beside;
#ifdef _WIN32
        wchar_t executable[32768]{};
        if (GetModuleFileNameW(nullptr, executable, 32768))
                beside = std::filesystem::path(executable).parent_path() / filename;
#endif
        for (const auto &path : {beside, std::filesystem::path("assets") / filename}) {
                if (!path.empty() && std::filesystem::is_regular_file(path)) {
                        const auto utf = std::filesystem::absolute(path).u8string();
                        return std::string(utf.begin(), utf.end());
                }
        }
        return {};
}
std::string
MuJoCoView::defaultModelPath()
{
        return bundledModelPath("mujoco_single_joint.xml");
}

bool
MuJoCoView::loadModel(const std::string &path)
{
        auto &s = *impl_;
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        if (!s.api())
                return false;
        char  error[2048]{};
        auto *model = s.mj_loadXML(path.c_str(), nullptr, error, sizeof(error));
        if (!model) {
                s.message = error;
                return false;
        }
        // The existing telemetry/PD contract describes one scalar hinge joint
        if (model->nq != 1 || model->nv != 1 || model->nu != 1 || !std::isfinite(model->opt.timestep) ||
            model->opt.timestep <= 0) {
                s.mj_deleteModel(model);
                s.message =
                    tr("This MuJoCo dock requires one joint and one actuator", "当前 MuJoCo Dock 支持单关节、单执行器模型");
                return false;
        }
        auto *data = s.mj_makeData(model);
        if (!data) {
                s.mj_deleteModel(model);
                s.message = "mj_makeData failed";
                return false;
        }
        ava_actuator::Config             config;
        std::vector<ava_actuator::Stage> stages;
        const bool                       actuator = s.numeric(model, "ava_actuator_version", 0) == 1;
        double                           ratio    = 1;
        if (actuator) {
                try {
                        auto get = [&](const char *name) {
                                double v = s.numeric(model, name, NAN);
                                if (!std::isfinite(v))
                                        throw std::runtime_error("Invalid actuator metadata / 关节模型元数据无效");
                                return v;
                        };
                        auto integer = [&](const char *name, int lo, int hi) {
                                double v = get(name);
                                if (v < lo || v > hi || v != std::floor(v))
                                        throw std::runtime_error("Invalid actuator integer metadata / 关节整数参数无效");
                                return static_cast<int>(v);
                        };
                        config.polePairs    = integer("ava_poles", 1, 32);
                        config.slots        = integer("ava_slots", 3, 96);
                        config.encoderBits  = integer("ava_bits", 8, 24);
                        config.ratio        = get("ava_requested_ratio");
                        config.gapMm        = get("ava_gap");
                        config.eccentricMm  = get("ava_eccentric");
                        config.axialMm      = get("ava_axial");
                        config.tiltDeg      = get("ava_tilt");
                        config.zeroDeg      = get("ava_zero");
                        config.rotorInertia = get("ava_rotor_inertia");
                        config.loadInertia  = get("ava_load_inertia");
                        config.motorTorque  = get("ava_motor_torque");
                        config.efficiency   = get("ava_efficiency");
                        config.exploded     = get("ava_exploded") != 0;
                        ava_actuator::validate(config);
                        auto expected = ava_actuator::generate(config);
                        stages        = expected.stages;
                        ratio         = expected.ratio;
                        if (std::abs(get("ava_ratio") - ratio) > 1e-6)
                                throw std::runtime_error("Actuator ratio metadata mismatch / 减速比元数据不一致");
                } catch (const std::exception &e) {
                        s.mj_deleteData(data);
                        s.mj_deleteModel(model);
                        s.message = e.what();
                        return false;
                }
        }
        stop();
        s.releaseModel();
        s.model = model;
        s.data  = data;
        s.path  = path;
        s.message.clear();
        s.actuatorModel                 = actuator;
        s.actuatorConfig                = config;
        s.draftConfig                   = config;
        s.stages                        = stages;
        s.gearRatio                     = ratio;
        s.model->vis.quality.offsamples = std::max(4, s.model->vis.quality.offsamples);
        s.mj_forward(model, data);
        s.mjv_defaultCamera(&s.camera);
        s.mjv_defaultOption(&s.option);
        s.mjv_defaultPerturb(&s.perturb);
        s.option.flags[mjVIS_SELECT]    = 1;
        s.option.flags[mjVIS_PERTFORCE] = 1;
        s.option.flags[mjVIS_PERTOBJ]   = 1;
        s.mjv_defaultScene(&s.scene);
        s.mjr_defaultContext(&s.context);
        s.mjv_makeScene(model, &s.scene, 2000);
        s.mjr_makeContext(model, &s.context, mjFONTSCALE_100);
        s.graphics    = true;
        s.camera.type = mjCAMERA_FREE;
        std::copy(model->stat.center, model->stat.center + 3, s.camera.lookat);
        s.camera.distance  = std::max(0.2, model->stat.extent * 1.5);
        s.camera.azimuth   = 140;
        s.camera.elevation = -20;
        reset();
        return true;
#else
        s.message = tr("This build has no MuJoCo renderer; rebuild with MUJOCO_DIR set to your SDK",
                       "此版本未编译 MuJoCo 渲染器，请指定 MUJOCO_DIR 后重新编译");
        return false;
#endif
}

bool
MuJoCoView::createActuator(const std::string &path, const ava_actuator::Config &config)
{
        try {
                const auto    model = ava_actuator::generate(config);
                std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
                if (!file)
                        throw std::runtime_error("Cannot save actuator XML / 无法保存关节模型 XML");
                file << model.xml;
                file.close();
                if (!file)
                        throw std::runtime_error("Cannot write actuator XML / 写入关节模型 XML 失败");
                return loadModel(path);
        } catch (const std::exception &e) {
                impl_->message = e.what();
                return false;
        }
}

bool
MuJoCoView::start(const std::string &region)
{
        auto &s = *impl_;
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        if (!s.model) {
                s.message = tr("Open a model first", "请先打开模型");
                return false;
        }
        if (s.active)
                return true;
        std::lock_guard lock(simulationRegionsMutex);
        if (simulationRegions.count(region) || !s.publisher.open(region)) {
                s.message = tr("A simulation already owns this SHM region; stop it before starting here",
                               "已有仿真占用此共享内存，请先停止它再启动内置仿真");
                return false;
        }
        s.region = region;
        s.active = true;
        s.paused = false;
        simulationRegions.insert(region);
        auto requestedControl = s.control;
        reset();
        s.control     = requestedControl;
        s.lastTick    = std::chrono::steady_clock::now();
        s.accumulator = 0;
        s.message.clear();
        return true;
#else
        return loadModel(s.path);
#endif
}
void
MuJoCoView::stop()
{
        auto           &s = *impl_;
        std::lock_guard lock(simulationRegionsMutex);
        if (s.active)
                simulationRegions.erase(s.region);
        s.active = false;
        s.controller.reset();
        s.commands.close();
        s.publisher.close();
        s.disturbance = 0;
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        s.perturb.active = 0;
        s.dragButton     = -1;
        s.physicsGesture = false;
        if (s.data)
                std::fill_n(s.data->xfrc_applied, 6 * s.model->nbody, 0.0);
#endif
}
void
MuJoCoView::reset()
{
        auto &s = *impl_;
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        if (s.data) {
                s.mj_resetData(s.model, s.data);
                s.assembly();
                s.mj_forward(s.model, s.data);
        }
        s.perturb.active = 0;
        s.dragButton     = -1;
        s.physicsGesture = false;
#endif
        s.epoch                  = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        s.sequence               = 0;
        s.control.positionTarget = s.control.velocityTarget = s.control.velocityFeedforward = s.control.torqueFeedforward = 0;
        s.controller.reset();
        s.commandOverrides = 0;
        s.accumulator      = 0;
        s.disturbance      = 0;
        s.lastTick         = std::chrono::steady_clock::now();
}
void
MuJoCoView::setPaused(bool paused)
{
        impl_->paused = paused;
}
void
MuJoCoView::tick()
{
        auto &s = *impl_;
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        if (!s.active || !s.data)
                return;
        if (ImGui::GetIO().AppFocusLost || s.lastDrawFrame < ImGui::GetFrameCount() - 1 ||
            (s.physicsGesture && (!ImGui::GetIO().KeyCtrl || s.dragButton < 0 || !ImGui::IsMouseDown(s.dragButton)))) {
                s.perturb.active = 0;
                s.dragButton     = -1;
                s.physicsGesture = false;
                std::fill_n(s.data->xfrc_applied, 6 * s.model->nbody, 0.0);
        }
        const auto   now     = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - s.lastTick).count();
        s.lastTick           = now;
        if (!s.commands.isOpen())
                s.commands.open(ava_sim::commandRegion(s.region));
        ava_sim::Command command;
        for (unsigned n = 0; n < 256 && s.commands.read(command); ++n) {
                if (!ava_sim::valid(command) || command.epoch != s.epoch || (s.commandOverrides & (1u << command.field)))
                        continue;
                switch (command.field) {
                        case 0:
                                s.control.positionTarget = command.val;
                                break;
                        case 2:
                                if (s.control.mode == ava_mujoco::Mode::Position)
                                        s.control.velocityFeedforward = command.val;
                                else
                                        s.control.velocityTarget = command.val;
                                break;
                        case 6:
                                if (command.val < 0 || command.val > 1e6)
                                        break;
                                if (s.control.mode == ava_mujoco::Mode::Position)
                                        s.control.positionKp = command.val;
                                else if (s.control.mode == ava_mujoco::Mode::Velocity)
                                        s.control.velocityKp = command.val;
                                else
                                        s.control.pdKp = command.val;
                                break;
                        case 7:
                                if (command.val >= 0 && command.val <= 1e6)
                                        s.control.pdKd = command.val;
                                break;
                }
        }
        s.commandOverrides = 0;
        if (s.paused)
                return;
        s.accumulator += std::min(elapsed, 0.1);
        for (unsigned n = 0; n < 200 && s.accumulator >= s.model->opt.timestep; ++n) {
                const double q  = s.actuatorModel ? ava_actuator::encoder(s.actuatorConfig, s.data->qpos[0]) : s.data->qpos[0];
                const double dq = s.actuatorModel
                                      ? ava_actuator::encoderVelocity(s.actuatorConfig, s.data->qpos[0], s.data->qvel[0])
                                      : s.data->qvel[0];
                double       transmission = s.actuatorModel ? s.gearRatio * s.actuatorConfig.efficiency : 1;
                double       low = -std::numeric_limits<double>::infinity(), high = -low;
                if (s.model->actuator_ctrllimited[0]) {
                        low  = s.model->actuator_ctrlrange[0] * transmission;
                        high = s.model->actuator_ctrlrange[1] * transmission;
                }
                if (s.actuatorModel) {
                        low  = std::max(low, -s.actuatorConfig.motorTorque * transmission);
                        high = std::min(high, s.actuatorConfig.motorTorque * transmission);
                }
                const double torque     = s.controller.step(s.control, q, dq, s.model->opt.timestep, low, high);
                s.data->ctrl[0]         = torque / transmission;
                s.data->qfrc_applied[0] = s.disturbance;
                std::fill_n(s.data->xfrc_applied, 6 * s.model->nbody, 0.0);
                if (s.perturb.active)
                        s.mjv_applyPerturbForce(s.model, s.data, &s.perturb);
                ava_sim::Sample sample;
                sample.epoch    = s.epoch;
                sample.sequence = s.sequence++;
                sample.time     = s.data->time;
                sample.q_ref    = static_cast<float>(s.control.positionTarget);
                sample.q        = static_cast<float>(q);
                sample.dq_ref = static_cast<float>(s.control.mode == ava_mujoco::Mode::Position ? s.control.velocityFeedforward
                                                                                                : s.control.velocityTarget);
                sample.dq     = static_cast<float>(dq);
                sample.tau    = static_cast<float>(torque);
                sample.kp     = static_cast<float>(ava_mujoco::activeKp(s.control));
                sample.reserved = 0x4D430000u | static_cast<unsigned>(s.control.mode);
                sample.kd       = static_cast<float>(s.control.pdKd);
                if (!ava_sim::valid(sample)) {
                        stop();
                        s.message = tr("Simulation produced invalid values; reset and check control parameters",
                                       "仿真出现无效数值，请复位并检查控制参数");
                        break;
                }
                s.publisher.publish(sample);
                s.mj_step(s.model, s.data);
                s.accumulator -= s.model->opt.timestep;
        }
        if (s.actuatorModel) {
                s.assembly();
                s.mj_forward(s.model, s.data);
        }
#endif
}

const ava_mujoco::Control &
MuJoCoView::control() const
{
        return impl_->control;
}
bool
MuJoCoView::setControl(const ava_mujoco::Control &config)
{
        if (!ava_mujoco::valid(config))
                return false;
        auto &s = *impl_;
        if (s.control.mode != config.mode || s.control.velocityKi != config.velocityKi)
                s.controller.reset();
        s.control           = config;
        s.commandOverrides |= (1u << 0) | (1u << 2) | (1u << 6) | (1u << 7);
        return true;
}
unsigned
MuJoCoView::drawControls()
{
        auto    &s     = *impl_;
        unsigned edits = 0;
        int      mode  = static_cast<int>(s.control.mode);
        ImGui::TextUnformatted(tr("Control mode", "控制模式"));
        ImGui::SetNextItemWidth(-1);
        if (ui::Choice("##control_mode", &mode, tr("Position\0Velocity\0PD\0", "位置模式\0速度模式\0PD 模式\0"))) {
                s.control.mode = static_cast<ava_mujoco::Mode>(mode);
                s.controller.reset();
                edits = (1u << 31) | (1u << 0) | (1u << 2) | (1u << 6) | (1u << 7);
        }
        auto input = [&](const char *en, const char *cn, double &val, unsigned field, bool gain = false) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped("%s", tr(en, cn));
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::PushID(en);
                if (ImGui::InputDouble("##value", &val, 0, 0, "%.5g")) {
                        if (!std::isfinite(val))
                                val = 0;
                        val    = std::clamp(val, gain ? 0.0 : -1e6, 1e6);
                        edits |= (1u << 31) | field;
                }
                if (ImGui::IsItemHovered()) {
                        const char *tip =
                            &val == &s.control.positionKp
                                ? tr("Position error times Kp produces the inner-loop velocity target.",
                                     "位置误差乘 Kp，得到内部速度环目标；增大可提高位置响应速度。")
                            : &val == &s.control.velocityKp
                                ? tr("Velocity error times Kp produces torque. Shared by position and velocity modes.",
                                     "速度误差乘 Kp，得到比例力矩；位置与速度模式共用此速度环增益。")
                            : &val == &s.control.velocityKi
                                ? tr("Integrates velocity error to compensate persistent loads. Shared by position and "
                                     "velocity modes.",
                                     "累积速度误差，用于补偿持续负载；位置与速度模式共用。过大可能增加振荡。")
                            : &val == &s.control.pdKp ? tr("Position error times Kp produces torque in PD mode.",
                                                           "PD 模式下，位置误差乘 Kp 得到力矩，影响位置刚度。")
                            : &val == &s.control.pdKd ? tr("Velocity error times Kd produces torque in PD mode.",
                                                           "PD 模式下，速度误差乘 Kd 得到力矩，影响速度反馈与阻尼。")
                            : &val == &s.control.positionTarget
                                ? tr("Desired output-joint angle in radians.", "输出关节目标角度，单位为弧度。")
                            : &val == &s.control.velocityTarget
                                ? tr("Desired output-joint angular velocity in rad/s.", "输出关节目标角速度，单位 rad/s。")
                            : &val == &s.control.velocityFeedforward
                                ? tr("Added to the position-loop velocity demand before velocity PI.",
                                     "加到位置环输出的速度目标上，再进入速度 PI；不会直接作为力矩使用。")
                                : tr("Added to the output-joint torque before torque limiting.",
                                     "加到反馈控制力矩上，再进行力矩限幅；单位为输出关节 Nm。");
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
                        ImGui::TextUnformatted(tip);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                }
                ImGui::PopID();
        };
        if (ImGui::BeginTable("##control_parameters", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, .6f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, .4f);
                if (s.control.mode == ava_mujoco::Mode::Position) {
                        input("Kp (1/s)", "Kp (1/s)", s.control.positionKp, 1u << 6, true);
                        input("Target position (rad)", "目标位置 (rad)", s.control.positionTarget, 1u << 0);
                        input("Velocity feedforward (rad/s)", "前馈速度 (rad/s)", s.control.velocityFeedforward, 1u << 2);
                } else if (s.control.mode == ava_mujoco::Mode::Velocity) {
                        input("Kp (Nm/(rad/s))", "Kp (Nm/(rad/s))", s.control.velocityKp, 1u << 6, true);
                        input("Ki (Nm/rad)", "Ki (Nm/rad)", s.control.velocityKi, 0, true);
                        input("Target velocity (rad/s)", "目标速度 (rad/s)", s.control.velocityTarget, 1u << 2);
                } else {
                        input("Kp (Nm/rad)", "Kp (Nm/rad)", s.control.pdKp, 1u << 6, true);
                        input("Kd (Nm/(rad/s))", "Kd (Nm/(rad/s))", s.control.pdKd, 1u << 7, true);
                        input("Target position (rad)", "目标位置 (rad)", s.control.positionTarget, 1u << 0);
                        input("Target velocity (rad/s)", "目标速度 (rad/s)", s.control.velocityTarget, 1u << 2);
                }
                input("Torque feedforward (Nm)", "前馈力矩 (Nm)", s.control.torqueFeedforward, 0);
                ImGui::EndTable();
        }
        ImGui::TextWrapped("%s",
                           s.control.mode == ava_mujoco::Mode::Position
                               ? tr("Position P -> velocity PI. Inner PI gains are shared with Velocity mode.",
                                    "位置 P → 速度 PI，内部 PI 增益沿用速度模式设置。")
                           : s.control.mode == ava_mujoco::Mode::Velocity
                               ? tr("Velocity PI + torque feedforward.", "速度 PI + 前馈力矩。")
                               : tr("Torque = Kp * position error + Kd * velocity error + feedforward.",
                                    "力矩 = Kp × 位置误差 + Kd × 速度误差 + 前馈力矩。"));
        s.commandOverrides |= edits & ~(1u << 31);
        return edits;
}
void
MuJoCoView::draw(const std::string &region, bool live, double position)
{
        auto &s        = *impl_;
        auto  sameLine = [&](float width) {
                const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right)
                        ImGui::SameLine();
        };
        auto nextButton = [&](const char *label) {
                sameLine(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2);
        };
        if (ImGui::Button(tr("New actuator...", "新建关节模组..."))) {
                s.draftConfig = s.actuatorModel ? s.actuatorConfig : ava_actuator::Config{};
                ImGui::OpenPopup("##actuator_builder");
        }
        ImGui::SetNextWindowSize(ImVec2(620, 660), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("##actuator_builder", nullptr, ImGuiWindowFlags_NoTitleBar)) {
                ImGui::TextUnformatted(tr("PMSM + planetary reducer + output encoder", "PMSM + 行星减速机 + 输出轴编码器"));
                auto &c       = s.draftConfig;
                bool  changed = ImGui::IsWindowAppearing();
                ImGui::PushItemWidth(220);
                ImGui::TextDisabled("%s", tr("Motor geometry", "电机结构"));
                changed |= ImGui::InputInt(tr("Pole pairs", "极对数"), &c.polePairs);
                changed |= ImGui::InputInt(tr("Stator slots", "定子槽数"), &c.slots);
                changed |= ImGui::InputDouble(tr("Rotor inertia (kg*m^2)", "转子惯量 (kg*m^2)"), &c.rotorInertia, 0, 0, "%.7f");
                changed |= ImGui::InputDouble(tr("Motor torque limit (Nm)", "电机转矩上限 (Nm)"), &c.motorTorque, 0, 0, "%.3f");
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Planetary transmission", "行星传动"));
                changed |= ImGui::InputDouble(tr("Requested ratio", "目标减速比"), &c.ratio, 0, 0, "%.5f");
                changed |= ImGui::InputDouble(tr("Total efficiency", "总传动效率"), &c.efficiency, 0, 0, "%.3f");
                changed |= ImGui::InputDouble(
                    tr("Output load inertia (kg*m^2)", "输出负载惯量 (kg*m^2)"), &c.loadInertia, 0, 0, "%.6f");
                changed |= ImGui::Checkbox(tr("Exploded assembly", "展开装配结构"), &c.exploded);
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Output magnetic encoder", "输出轴磁编码器"));
                changed |= ImGui::InputDouble(tr("Nominal air gap (mm)", "标称气隙 (mm)"), &c.gapMm, 0, 0, "%.3f");
                changed |= ImGui::InputDouble(tr("Axial assembly error (mm)", "轴向装配误差 (mm)"), &c.axialMm, 0, 0, "%.3f");
                changed |= ImGui::InputDouble(tr("Radial eccentricity (mm)", "径向偏心 (mm)"), &c.eccentricMm, 0, 0, "%.3f");
                changed |= ImGui::InputDouble(tr("Assembly tilt (deg)", "装配倾斜 (度)"), &c.tiltDeg, 0, 0, "%.3f");
                changed |= ImGui::InputDouble(tr("Zero offset (deg)", "零位误差 (度)"), &c.zeroDeg, 0, 0, "%.3f");
                changed |= ImGui::InputInt(tr("Encoder resolution (bits)", "编码器分辨率 (位)"), &c.encoderBits);
                ImGui::PopItemWidth();
                if (changed) {
                        try {
                                auto               model = ava_actuator::generate(c);
                                std::ostringstream summary;
                                summary << tr("Actual ratio: ", "实际减速比：") << model.ratio << "  "
                                        << tr("Stages: ", "级数：") << model.stages.size();
                                for (size_t i = 0; i < model.stages.size(); ++i) {
                                        auto stage = model.stages[i];
                                        summary << "\n"
                                                << i + 1 << ": "
                                                << tr("Sun / planet / ring teeth: ", "太阳轮 / 行星轮 / 内齿圈齿数：")
                                                << stage.sun << " / " << stage.planet << " / " << stage.ring;
                                }
                                s.draftSummary = summary.str();
                                s.draftError.clear();
                        } catch (const std::exception &e) {
                                s.draftError = e.what();
                        }
                }
                if (!s.draftError.empty())
                        ui::ErrorText("%s", s.draftError.c_str());
                else
                        ImGui::TextUnformatted(s.draftSummary.c_str());
                ImGui::TextWrapped(
                    "%s",
                    tr("Ideal rigid transmission; approximate encoder errors. No electromagnetic field or tooth contact solve.",
                       "理想刚性传动；编码器误差为近似模型。未求解电磁场或齿面接触。"));
                ImGui::BeginDisabled(!s.draftError.empty());
                if (ImGui::Button(tr("Generate, save and load", "生成、保存并加载"))) {
                        auto path = nativeDlgSave(
                            tr("Save actuator model", "保存关节模组模型"), {{"MuJoCo XML", {"xml"}}}, "mujoco_actuator.xml");
                        if (!path.empty() && createActuator(path, c))
                                ImGui::CloseCurrentPopup();
                        else if (!path.empty())
                                s.draftError = s.message;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
        nextButton(tr("Open model...", "打开模型..."));
        if (ImGui::Button(tr("Open model...", "打开模型..."))) {
                const auto path = nativeDlgOpen(tr("MuJoCo model", "MuJoCo 模型"), {{"XML", {"xml"}}});
                if (!path.empty())
                        loadModel(path);
        }
        nextButton(tr("Actuator example", "关节模组示例"));
        if (ImGui::Button(tr("Actuator example", "关节模组示例"))) {
                auto path = bundledModelPath("mujoco_actuator.xml");
                if (!path.empty())
                        loadModel(path);
                else
                        s.message = tr("Actuator example XML was not found", "未找到关节模组示例 XML");
        }
        nextButton(s.active ? tr("Stop simulation", "停止仿真") : tr("Start simulation", "启动仿真"));
        if (ImGui::Button(s.active ? tr("Stop simulation", "停止仿真") : tr("Start simulation", "启动仿真"))) {
                if (s.active)
                        stop();
                else
                        start(region);
        }
        if (s.active) {
                nextButton(s.paused ? tr("Resume", "继续") : tr("Pause", "暂停"));
                if (ImGui::Button(s.paused ? tr("Resume", "继续") : tr("Pause", "暂停")))
                        setPaused(!s.paused);
                nextButton(tr("Reset", "复位"));
                if (ImGui::Button(tr("Reset", "复位")))
                        reset();
                sameLine(130 + ImGui::CalcTextSize(tr("Disturbance (Nm)", "扰动力矩 (Nm)")).x +
                         ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::SetNextItemWidth(130);
                ImGui::SliderFloat(tr("Disturbance (Nm)", "扰动力矩 (Nm)"), &s.disturbance, -10, 10);
        }
        if (!s.message.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
                ImGui::TextWrapped("%s", s.message.c_str());
                ImGui::PopStyleColor();
        }
#if defined(_WIN32) && defined(AVA_HAS_MUJOCO)
        if (!s.model) {
                ImGui::TextUnformatted(tr("Open a single-joint MuJoCo XML model", "打开单关节 MuJoCo XML 模型"));
                return;
        }
        if (s.actuatorModel)
                ImGui::TextDisabled(tr("Actuator: %d pole pairs / %d slots | ratio %.5g | gap %.3f mm",
                                       "关节模组：%d 极对 / %d 槽 | 减速比 %.5g | 气隙 %.3f mm"),
                                    s.actuatorConfig.polePairs,
                                    s.actuatorConfig.slots,
                                    s.gearRatio,
                                    s.actuatorConfig.gapMm + s.actuatorConfig.axialMm);
        ImGui::TextDisabled("%s",
                            tr("Left: rotate | right: pan | wheel: zoom | Ctrl+drag: disturb model",
                               "左键旋转 | 右键平移 | 滚轮缩放 | Ctrl+拖动：扰动模型"));
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 2 || size.y < 2)
                return;
        auto       &io         = ImGui::GetIO();
        const float dpi        = ImGui::GetWindowViewport()->DpiScale;
        float       pixelX     = std::max(dpi, io.DisplayFramebufferScale.x);
        float       pixelY     = std::max(dpi, io.DisplayFramebufferScale.y);
        GLint       maxTexture = 0, maxBuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(0x84E8, &maxBuffer);
        const float  limit  = static_cast<float>(std::min(maxTexture, maxBuffer));
        const float  scale  = std::min({1.0f, limit / (size.x * pixelX), limit / (size.y * pixelY)});
        const int    w      = std::max(2, static_cast<int>(std::ceil(size.x * pixelX * scale)));
        const int    h      = std::max(2, static_cast<int>(std::ceil(size.y * pixelY * scale)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##mujoco_viewport",
                               size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        s.lastDrawFrame = ImGui::GetFrameCount();
        if (!s.active && live && std::isfinite(position)) {
                s.data->qpos[0] = position;
                s.assembly();
                s.mj_forward(s.model, s.data);
        }
        if (s.dragButton >= 0 && (!active || !ImGui::IsMouseDown(s.dragButton) || (s.physicsGesture && !io.KeyCtrl))) {
                s.perturb.active = 0;
                s.dragButton     = -1;
                s.physicsGesture = false;
                std::fill_n(s.data->xfrc_applied, 6 * s.model->nbody, 0.0);
        }
        if (active && s.dragButton < 0) {
                for (int button = 0; button < 3; ++button)
                        if (ImGui::IsMouseClicked(button)) {
                                s.dragButton = button;
                                break;
                        }
                s.physicsGesture = io.KeyCtrl;
                if (s.physicsGesture && s.active && s.dragButton >= 0 && s.dragButton != ImGuiMouseButton_Middle) {
                        s.mjv_updateScene(s.model, s.data, &s.option, nullptr, &s.camera, mjCAT_ALL, &s.scene);
                        mjtNum    point[3]{};
                        int       geom = -1, flex = -1, skin = -1;
                        const int body = s.mjv_select(s.model,
                                                      s.data,
                                                      &s.option,
                                                      double(w) / h,
                                                      (io.MousePos.x - origin.x) / size.x,
                                                      1.0 - (io.MousePos.y - origin.y) / size.y,
                                                      &s.scene,
                                                      point,
                                                      &geom,
                                                      &flex,
                                                      &skin);
                        if (body > 0 && s.model->body_weldid[body] != 0) {
                                s.perturb.select     = body;
                                s.perturb.flexselect = flex;
                                s.perturb.skinselect = skin;
                                for (int i = 0; i < 3; ++i) {
                                        s.perturb.localpos[i] = 0;
                                        for (int j = 0; j < 3; ++j)
                                                s.perturb.localpos[i] += s.data->xmat[9 * body + 3 * j + i] *
                                                                         (point[j] - s.data->xpos[3 * body + j]);
                                }
                                s.mjv_initPerturb(s.model, s.data, &s.scene, &s.perturb);
                                s.perturb.active = s.dragButton == ImGuiMouseButton_Left ? mjPERT_TRANSLATE : mjPERT_ROTATE;
                        }
                }
        }
        if (hovered && io.MouseWheel != 0)
                s.mjv_moveCamera(s.model, mjMOUSE_ZOOM, 0, -0.05 * io.MouseWheel, &s.camera);
        if (active && s.dragButton >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
                if (s.physicsGesture) {
                        if (s.perturb.active) {
                                const int action = s.perturb.active & mjPERT_TRANSLATE
                                                       ? (io.KeyShift ? mjMOUSE_MOVE_H : mjMOUSE_MOVE_V)
                                                       : (io.KeyShift ? mjMOUSE_ROTATE_H : mjMOUSE_ROTATE_V);
                                s.mjv_movePerturb(s.model,
                                                  s.data,
                                                  action,
                                                  io.MouseDelta.x / size.y,
                                                  io.MouseDelta.y / size.y,
                                                  &s.scene,
                                                  &s.perturb);
                        }
                } else {
                        const int action = s.dragButton == ImGuiMouseButton_Right
                                               ? (io.KeyShift ? mjMOUSE_MOVE_H : mjMOUSE_MOVE_V)
                                               : (s.dragButton == ImGuiMouseButton_Middle
                                                      ? mjMOUSE_ZOOM
                                                      : (io.KeyShift ? mjMOUSE_ROTATE_H : mjMOUSE_ROTATE_V));
                        s.mjv_moveCamera(s.model, action, io.MouseDelta.x / size.y, io.MouseDelta.y / size.y, &s.camera);
                }
        }
        GLint program = 0;
        glGetIntegerv(0x8B8D, &program);
        auto useProgram = reinterpret_cast<void(APIENTRY *)(GLuint)>(glfwGetProcAddress("glUseProgram"));
        if (useProgram)
                useProgram(0);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const bool resized = w != s.width || h != s.height;
        if (resized) {
                s.mjr_resizeOffscreen(w, h, &s.context);
                s.width  = w;
                s.height = h;
                s.pixels.resize(static_cast<std::size_t>(w) * h * 3);
        }
        s.mjv_updateScene(s.model, s.data, &s.option, &s.perturb, &s.camera, mjCAT_ALL, &s.scene);
        s.mjr_setBuffer(mjFB_OFFSCREEN, &s.context);
        const mjrRect viewport{0, 0, w, h};
        s.mjr_render(viewport, &s.scene, &s.context);
        s.mjr_readPixels(s.pixels.data(), nullptr, viewport, &s.context);
        s.mjr_setBuffer(mjFB_WINDOW, &s.context);
        if (!s.texture) {
                glGenTextures(1, &s.texture);
        }
        glBindTexture(GL_TEXTURE_2D, s.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (resized)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, s.pixels.data());
        else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, s.pixels.data());
        glPopClientAttrib();
        glPopAttrib();
        if (useProgram)
                useProgram(program);
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(s.texture),
                                             origin,
                                             ImVec2(origin.x + size.x, origin.y + size.y),
                                             ImVec2(0, 1),
                                             ImVec2(1, 0));
#endif
}
