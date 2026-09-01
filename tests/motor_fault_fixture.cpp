// Synthetic C ABI fixture: valid feedback followed by a stopped PDO timeout
#define AVA_ETH_SDK_BUILD
#include "../sdk/ava_eth/include/ava/sdk.h"
#include <cstring>
#include <cmath>
struct AvaEthHandleImpl {
        unsigned reads = 0, budget = 5000;
        bool enableTimeout = false, stopped = false;
        bool livePid = false, enabled = false;
        bool liveMode = false;
        unsigned heartbeats = 0, pidStartHeartbeats = 0, pidPolls = 0;
        AvaEthTarget target{};
        AvaEthPidStatus pid{};
};
static thread_local int accessFailure = 0;
extern "C" {
int
ava_eth_enumerate(AvaEthDeviceInfo *, uint32_t, uint32_t *n)
{
        *n = 0;
        return 0;
}
int
ava_eth_open(const AvaEthOpenOptions *o, AvaEthHandle *h)
{
        accessFailure = 0;
        if (o->device_path && std::strcmp(o->device_path, "access-denied") == 0)
                accessFailure = 1;
        if (o->device_path && std::strcmp(o->device_path, "npcap-access-denied") == 0)
                accessFailure = 2;
        if (accessFailure) {
                *h = nullptr;
                return accessFailure == 1 ? AVA_ETH_ACCESS : AVA_ETH_IO;
        }
        *h           = new AvaEthHandleImpl;
        (*h)->budget = (o->flags & AVA_ETH_RELAXED_RECEIVE) ? 20000 : 5000;
        (*h)->enableTimeout = o->device_path && std::strcmp(o->device_path, "enable-timeout") == 0;
        (*h)->livePid = o->device_path && std::strcmp(o->device_path, "live-pid") == 0;
        (*h)->liveMode = o->device_path && std::strcmp(o->device_path, "live-mode") == 0;
        if ((*h)->liveMode) { (*h)->livePid=true; (*h)->target={0,-9,73.125f,0,0,0}; }
        return 0;
}
void
ava_eth_close(AvaEthHandle h)
{
        delete h;
}
int
ava_eth_get_info(AvaEthHandle h, AvaEthInfo *i)
{
        *i = {AVA_ETH_ABI, sizeof(*i), AVA_ETH_CAP_HARDWARE, 1, 1000, 300, 1, 0};
        if (h->liveMode) i->capabilities |= AVA_ETH_CAP_INTERPOLATION;
        return 0;
}
int
ava_eth_get_snapshot(AvaEthHandle h, AvaEthSnapshot *s)
{
        *s             = {};
        s->version     = AVA_ETH_ABI;
        s->size        = sizeof(*s);
        s->count       = 1;
        auto &f        = s->axes[0];
        f.flags        = AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID;
        f.timestamp_ns = ++h->reads * 1000000;
        if (h->livePid) {
                f.mode = h->target.mode;
                f.position = h->target.position;
                f.velocity = h->target.velocity;
                if (h->enabled) f.flags |= AVA_ETH_ENABLED;
        }
        if (!h->enableTimeout && !h->livePid && h->reads >= 4) {
                f.flags      = AVA_ETH_FAULT;
                f.error_code = AVA_ETH_ERR_RX_TIMEOUT;
        }
        if (h->stopped)
                f.flags &= ~(AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID);
        return 0;
}
int
ava_eth_get_diagnostics(AvaEthHandle h, AvaEthDiagnostics *d)
{
        *d                    = {};
        d->version            = 1;
        d->size               = sizeof(*d);
        d->receive_timeout_us = h->budget;
        d->timeouts           = h->reads >= 4 ? 1 : 0;
        d->max_rtt_ns         = 6200000;
        d->wkc                = 3;
        d->expected_wkc       = 3;
        return 0;
}
int
ava_eth_set_targets(AvaEthHandle h, const AvaEthTarget *t, uint32_t, uint64_t *)
{
        if (h->livePid) { h->target = *t; return 0; }
        return h->enableTimeout ? AVA_ETH_OK : AVA_ETH_UNSUPPORTED;
}
int
ava_eth_enable(AvaEthHandle h, uint32_t)
{
        if (h->livePid) { h->enabled = true; return 0; }
        return h->enableTimeout ? AVA_ETH_IO : AVA_ETH_UNSUPPORTED;
}
int ava_eth_switch_mode(AvaEthHandle h, AvaEthTarget *target, const AvaEthInterpolation *)
{
        if (!h->liveMode) return AVA_ETH_UNSUPPORTED;
        // Simulate a newer raw sample taken inside the SDK after the actor's snapshot
        target->position=h->target.position+1.25f;
        target->velocity=target->current=target->torque=0;
        h->target=*target;
        return AVA_ETH_OK;
}
int ava_eth_set_interpolation(AvaEthHandle h, uint32_t, uint32_t, float, float)
{
        return h->liveMode ? AVA_ETH_OK : AVA_ETH_UNSUPPORTED;
}
int
ava_eth_disable(AvaEthHandle, uint32_t)
{
        return 0;
}
int
ava_eth_reset_fault(AvaEthHandle, uint32_t)
{
        return AVA_ETH_UNSUPPORTED;
}
int
ava_eth_keepalive(AvaEthHandle h, uint32_t)
{
        ++h->heartbeats;
        return 0;
}
int ava_eth_set_pid(AvaEthHandle h, uint32_t, uint32_t mask, const AvaEthPidParameters *v, uint64_t *sequence)
{
        if (!h->livePid) return AVA_ETH_UNSUPPORTED;
        if (h->pid.state == AVA_ETH_PID_PENDING) return AVA_ETH_STATE;
        for (unsigned i=0;i<8;++i) if ((mask&(1u<<i)) && (!std::isfinite(v->values[i]) || v->values[i]<0))
                return AVA_ETH_INVALID;
        h->pid.size=sizeof(h->pid); h->pid.state=AVA_ETH_PID_PENDING; h->pid.mask=mask;
        h->pid.values=*v; *sequence=++h->pid.sequence;
        h->pidStartHeartbeats=h->heartbeats; h->pidPolls=0;
        return 0;
}
int ava_eth_get_pid_status(AvaEthHandle h, AvaEthPidStatus *s)
{
        if (h->pid.state==AVA_ETH_PID_PENDING && ++h->pidPolls>=15) {
                h->pid.state = h->heartbeats-h->pidStartHeartbeats>=3 && h->enabled && !h->stopped
                               ? AVA_ETH_PID_SENT : AVA_ETH_PID_FAILED;
                std::strcpy(h->pid.message,"PID async heartbeat fixture completed");
        }
        *s=h->pid; return 0;
}
int
ava_eth_stop(AvaEthHandle h)
{
        h->stopped = true;
        h->enabled = false;
        return 0;
}
const char *
ava_eth_result_string(int)
{
        return "fixture result";
}
uint32_t
ava_eth_last_os_error()
{
        return accessFailure == 1 ? 5 : 0;
}
const char *
ava_eth_last_error_message()
{
        if (accessFailure)
                return "Error opening adapter: Access is denied.";
        return "Npcap 使能确认超时（200 ms）；CW=0x0006 SW=0x0040";
}
}
