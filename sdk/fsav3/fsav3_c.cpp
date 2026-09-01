#include "fsav3_c.h"

#include "fsav3.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_devices_mutex;
std::unordered_map<std::string, AC3::FSA *> g_devices;
std::string g_active_ip;
AC3::FSA::pid_param_t g_pid_cache{};
AC3::FSA::pd_param_t g_pd_cache{};
bool g_pid_cache_valid = false;

int get_device(const std::string &ip, AC3::FSA *&device)
{
    AC3::FSA *&stored = g_devices[ip];
    if (!stored) {
        stored = new AC3::FSA;
        const auto rc = stored->Init(ip.c_str(), AC3::FSA::net_recv_mode_e::YIELD_WAIT);
        if (rc != AC3::FSA::ret_e::SUCCESS) {
            // Some vendor SDK versions are unsafe to destroy after a failed Init
            stored = nullptr;
            return static_cast<int>(rc);
        }
    }
    device = stored;
    return static_cast<int>(AC3::FSA::ret_e::SUCCESS);
}

double timeout_or_default(uint32_t timeout_ms)
{
    return timeout_ms == 0 ? 100.0 : static_cast<double>(timeout_ms);
}

int checked_ip(const char *text, size_t capacity, bool allow_active, std::string &ip)
{
    if (!text || capacity == 0)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    size_t length = 0;
    while (length < capacity && text[length] != '\0')
        ++length;
    if (length == capacity)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    ip.assign(text, length);
    if (ip.empty() && allow_active)
        ip = g_active_ip;
    if (ip.empty())
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    return static_cast<int>(AC3::FSA::ret_e::SUCCESS);
}

void set_active_ip(const std::string &ip)
{
    g_active_ip = ip;
}

} // namespace

extern "C" int fsav3_read_param(fsav3_param *param)
{
    if (!param)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    std::string ip;
    int rc = checked_ip(param->ip, sizeof(param->ip), false, ip);
    if (rc != 0)
        return rc;
    AC3::FSA *device = nullptr;
    rc = get_device(ip, device);
    if (rc != 0)
        return rc;
    set_active_ip(ip);

    AC3::FSA::pid_param_t pid{};
    auto sdk_rc = device->GetPIDParams(pid, timeout_or_default(param->timeout_ms), static_cast<int>(param->max_retry));
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);
    param->pos_kp = pid.pos_kp;
    param->vel_kp = pid.vel_kp;
    param->vel_ki = pid.vel_ki;

    AC3::FSA::pd_param_t pd{};
    sdk_rc = device->GetPDParams(pd, timeout_or_default(param->timeout_ms), static_cast<int>(param->max_retry));
    if (sdk_rc == AC3::FSA::ret_e::SUCCESS) {
        param->pd_kp = pd.pd_kp;
        param->pd_kd = pd.pd_kd;
        g_pid_cache = pid;
        g_pd_cache = pd;
        g_pid_cache_valid = true;
        // A successfully read table is safe to edit and write back
        if (param->write_pid == 0 && param->write_pd == 0) {
            param->write_pid = 1;
            param->write_pd = 1;
        }
    }
    return static_cast<int>(sdk_rc);
}

extern "C" int fsav3_write_param(fsav3_param *param)
{
    if (!param)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    std::string ip;
    int rc = checked_ip(param->ip, sizeof(param->ip), false, ip);
    if (rc != 0)
        return rc;
    AC3::FSA *device = nullptr;
    rc = get_device(ip, device);
    if (rc != 0)
        return rc;
    set_active_ip(ip);

    const double timeout = timeout_or_default(param->timeout_ms);
    const int retry = static_cast<int>(param->max_retry);
    AC3::FSA::ret_e sdk_rc = AC3::FSA::ret_e::SUCCESS;
    if (param->control_action == FSAV3_CONTROL_ENABLE && param->control_mode != 1 && param->control_mode != 4 &&
        param->control_mode != 5 && param->control_mode != 6 && param->control_mode != 7)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    if (param->write_pid) {
        sdk_rc = device->SetPIDParams(param->pos_kp, param->vel_kp, param->vel_ki, timeout, retry);
        if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
            return static_cast<int>(sdk_rc);
    }
    if (param->write_pd) {
        sdk_rc = device->SetPDParams(param->pd_kp, param->pd_kd, timeout, retry);
        if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
            return static_cast<int>(sdk_rc);
    }
    g_pid_cache.pos_kp = param->pos_kp;
    g_pid_cache.vel_kp = param->vel_kp;
    g_pid_cache.vel_ki = param->vel_ki;
    g_pd_cache.pd_kp = param->pd_kp;
    g_pd_cache.pd_kd = param->pd_kd;
    g_pid_cache_valid = true;
    if (param->control_action == FSAV3_CONTROL_ENABLE) {
        sdk_rc = device->EnableControl(static_cast<AC3::FSA::ctrl_mode_e>(param->control_mode), timeout, retry);
    } else if (param->control_action == FSAV3_CONTROL_DISABLE) {
        sdk_rc = device->DisableControl(timeout, retry);
    }
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);

    switch (param->command_action) {
        case FSAV3_COMMAND_POSITION:
            sdk_rc = device->SetPosition(param->position_rad, param->velocity_radps, param->torque_nm, timeout, retry);
            break;
        case FSAV3_COMMAND_VELOCITY:
            sdk_rc = device->SetVelocity(param->velocity_radps, param->torque_nm, timeout, retry);
            break;
        case FSAV3_COMMAND_TORQUE:
            sdk_rc = device->SetTorque(param->torque_nm, timeout, retry);
            break;
        case FSAV3_COMMAND_CURRENT:
            sdk_rc = device->SetCurrent(param->current_a, timeout, retry);
            break;
        case FSAV3_COMMAND_PD:
            sdk_rc = device->SetPDPositionVelocity(param->position_rad, param->velocity_radps, param->torque_nm, timeout, retry);
            break;
        default:
            break;
    }
    return static_cast<int>(sdk_rc);
}

extern "C" int fsav3_read_rt(fsav3_rt *rt)
{
    if (!rt)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    std::string ip;
    int rc = checked_ip(rt->ip, sizeof(rt->ip), true, ip);
    if (rc != 0)
        return rc;
    if (rt->ip[0] == '\0')
        std::snprintf(rt->ip, sizeof(rt->ip), "%s", ip.c_str());
    AC3::FSA *device = nullptr;
    rc = get_device(ip, device);
    if (rc != 0)
        return rc;
    set_active_ip(ip);

    const double timeout = timeout_or_default(rt->timeout_ms);
    const int retry = static_cast<int>(rt->max_retry);
    AC3::FSA::pvctte_t pvct{};
    auto sdk_rc = device->GetPVCTTe(pvct, timeout, retry);
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);
    rt->get_pvct.pos = static_cast<float>(pvct.pos);
    rt->get_pvct.vel = static_cast<float>(pvct.vel);
    rt->get_pvct.cur = static_cast<float>(pvct.cur);
    rt->get_pvct.load_tor = static_cast<float>(pvct.tor);
    rt->get_pvct.elec_tor = static_cast<float>(pvct.tor_e);

    AC3::FSA::temp_vbus_t temperatures{};
    sdk_rc = device->GetTempVbus(temperatures, timeout, retry);
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);
    rt->inverter.mos = static_cast<float>(temperatures.mos);
    rt->inverter.coil = static_cast<float>(temperatures.coil);
    rt->inverter.coil_2 = static_cast<float>(temperatures.coil);
    rt->inverter.vbus = static_cast<float>(temperatures.vbus);

    AC3::FSA::err_code_t errors{};
    sdk_rc = device->GetErrCode(errors, timeout, retry);
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);
    for (size_t i = 0; i < errors.arr.size(); ++i)
        rt->err_code[i] = errors.arr[i];

    /* PID belongs to the V2.5 realtime layout but is slow-changing RMA data.
       It is maintained by the parameter read/write path.  Do not add optional
       PID/PD network requests to every realtime frame: some firmware versions
       reject those requests with ARG_ERR and would otherwise discard valid
       PVCT/temperature/error data. */
    if (g_pid_cache_valid) {
        rt->pid.tor_pos_kp = static_cast<float>(g_pid_cache.pos_kp);
        rt->pid.tor_vel_kp = static_cast<float>(g_pid_cache.vel_kp);
        rt->pid.tor_vel_ki = static_cast<float>(g_pid_cache.vel_ki);
        rt->pid.pd_kp = static_cast<float>(g_pd_cache.pd_kp);
        rt->pid.pd_kd = static_cast<float>(g_pd_cache.pd_kd);
    }
    ++rt->sample_count;
    return static_cast<int>(sdk_rc);
}

extern "C" int fsav3_write_rt(fsav3_rt *rt)
{
    if (!rt)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    std::string ip;
    int rc = checked_ip(rt->ip, sizeof(rt->ip), true, ip);
    if (rc != 0)
        return rc;
    if (rt->ip[0] == '\0')
        std::snprintf(rt->ip, sizeof(rt->ip), "%s", ip.c_str());
    AC3::FSA *device = nullptr;
    rc = get_device(ip, device);
    if (rc != 0)
        return rc;
    set_active_ip(ip);

    const double timeout = timeout_or_default(rt->timeout_ms);
    const int retry = static_cast<int>(rt->max_retry);
    AC3::FSA::ret_e sdk_rc = AC3::FSA::ret_e::SUCCESS;

    if (rt->write_pid) {
        sdk_rc = device->SetPIDParams(rt->pid.tor_pos_kp, rt->pid.tor_vel_kp, rt->pid.tor_vel_ki, timeout, retry);
        if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
            return static_cast<int>(sdk_rc);
        sdk_rc = device->SetPDParams(rt->pid.pd_kp, rt->pid.pd_kd, timeout, retry);
        if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
            return static_cast<int>(sdk_rc);
        g_pid_cache.pos_kp = rt->pid.tor_pos_kp;
        g_pid_cache.vel_kp = rt->pid.tor_vel_kp;
        g_pid_cache.vel_ki = rt->pid.tor_vel_ki;
        g_pd_cache.pd_kp = rt->pid.pd_kp;
        g_pd_cache.pd_kd = rt->pid.pd_kd;
        g_pid_cache_valid = true;
        rt->write_pid = 0;
    }

    if (rt->write_comm_timeout) {
        AC3::FSA::comm_timeout_protect_config_t config{};
        config.config_timeout_ms = true;
        config.set_timeout_ms = rt->comm_timeout.timeout_ms;
        config.config_protect_pos_kp = true;
        config.config_protect_vel_kp = true;
        config.config_protect_vel_ki = true;
        config.set_protect_pid.pos_kp = rt->comm_timeout.tor_pos_kp;
        config.set_protect_pid.vel_kp = rt->comm_timeout.tor_vel_kp;
        config.set_protect_pid.vel_ki = rt->comm_timeout.tor_vel_ki;
        sdk_rc = device->CommTimeoutProtect(config, timeout, retry);
        if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
            return static_cast<int>(sdk_rc);
        rt->comm_timeout.timeout_ms = config.get_timeout_ms;
        rt->comm_timeout.tor_pos_kp = static_cast<float>(config.get_protect_pid.pos_kp);
        rt->comm_timeout.tor_vel_kp = static_cast<float>(config.get_protect_pid.vel_kp);
        rt->comm_timeout.tor_vel_ki = static_cast<float>(config.get_protect_pid.vel_ki);
        rt->write_comm_timeout = 0;
    }

    auto sdk_mode = rt->control.set_control_mode;
    if (sdk_mode == COMM_SHM_V25_MODE_VELOCITY)
        sdk_mode = COMM_SHM_V25_MODE_TORQUE_VELOCITY;
    else if (sdk_mode == COMM_SHM_V25_MODE_POSITION)
        sdk_mode = COMM_SHM_V25_MODE_TORQUE_POSITION;

    switch (rt->control.set_control_word) {
        case COMM_SHM_V25_CTRL_SERVO_ON:
            if (sdk_mode != 1 && sdk_mode != 4 && sdk_mode != 5 && sdk_mode != 6 && sdk_mode != 7)
                return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
            sdk_rc = device->EnableControl(static_cast<AC3::FSA::ctrl_mode_e>(sdk_mode), timeout, retry);
            break;
        case COMM_SHM_V25_CTRL_SERVO_OFF:
            sdk_rc = device->DisableControl(timeout, retry);
            break;
        case COMM_SHM_V25_CTRL_POSITION_SET_ZERO:
            sdk_rc = device->SetAbsEncoderZero(timeout, retry);
            break;
        case COMM_SHM_V25_CTRL_NONE:
            break;
        default:
            return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    }
    if (sdk_rc != AC3::FSA::ret_e::SUCCESS)
        return static_cast<int>(sdk_rc);
    rt->control.set_control_word = COMM_SHM_V25_CTRL_NONE;

    switch (rt->control.set_control_mode) {
        case COMM_SHM_V25_MODE_CURRENT:
            sdk_rc = device->SetCurrent(rt->set_pvct.cur, timeout, retry);
            break;
        case COMM_SHM_V25_MODE_VELOCITY:
        case COMM_SHM_V25_MODE_TORQUE_VELOCITY:
            sdk_rc = device->SetVelocity(rt->set_pvct.vel, rt->set_pvct.tor_ff, timeout, retry);
            break;
        case COMM_SHM_V25_MODE_POSITION:
        case COMM_SHM_V25_MODE_TORQUE_POSITION:
            sdk_rc = device->SetPosition(rt->set_pvct.pos, rt->set_pvct.vel_ff, rt->set_pvct.tor_ff, timeout, retry);
            break;
        case COMM_SHM_V25_MODE_TORQUE:
            sdk_rc = device->SetTorque(rt->set_pvct.tor, timeout, retry);
            break;
        case COMM_SHM_V25_MODE_TORQUE_PD:
            sdk_rc = device->SetPDPositionVelocity(rt->set_pvct.pos, rt->set_pvct.vel, rt->set_pvct.tor_ff, timeout, retry);
            break;
        default:
            return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    }
    return static_cast<int>(sdk_rc);
}

extern "C" int fsav3_c_read_error3(const char *ip, int timeout_ms, uint32_t *error3)
{
    if (!ip || !error3 || timeout_ms < 0)
        return static_cast<int>(AC3::FSA::ret_e::ARG_ERR);
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    AC3::FSA *device = nullptr;
    int rc = get_device(ip, device);
    if (rc != 0)
        return rc;
    AC3::FSA::err_code_t code{};
    const auto sdk_rc = device->GetErrCode(code, static_cast<double>(timeout_ms), 0);
    if (sdk_rc == AC3::FSA::ret_e::SUCCESS)
        *error3 = code.arr[2];
    return static_cast<int>(sdk_rc);
}
