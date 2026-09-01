#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef TEST_OFFSET
#define TEST_OFFSET 0
#endif
#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXPORT int32_t sdk_add(int32_t a, int32_t b) { return a + b + TEST_OFFSET; }
EXPORT float sdk_float(float val) { return val * 2.0f; }
EXPORT double sdk_double(double val) { return val * 2.0; }
EXPORT uint64_t sdk_wide(uint64_t val) { return val; }
EXPORT int32_t sdk_write(int32_t *val) { *val += 7; return *val; }
EXPORT int32_t sdk_member(int32_t *self, int32_t val) { return *self + val; }
EXPORT int32_t sdk_static(int32_t val) { return val + 5; }
EXPORT int32_t sdk_tick(uint32_t *val) { ++*val; return 0; }
EXPORT int32_t sdk_fail(uint32_t *) { return -1; }
struct FixtureSync {
        uint32_t write_command, command_value;
        uint64_t feedback;
        double torque;
        uint32_t values[2];
};
static uint32_t syncWrites, syncCommand = 7, syncValues[2] = {11, 22};
static uint64_t syncFeedback = 0xfedcba9876543200ULL;
EXPORT uint32_t sdk_sync_writes() { return syncWrites; }
EXPORT int32_t sdk_sync_read_rt(FixtureSync *p) {
        p->command_value = syncCommand; p->feedback = ++syncFeedback; p->torque = 8.3;
        p->values[0] = syncValues[0]; p->values[1] = syncValues[1]; return 0;
}
EXPORT int32_t sdk_sync_write_rt(FixtureSync *p) {
        ++syncWrites;
        if(p->write_command) syncCommand=p->command_value;
        p->write_command=0;syncValues[0]=p->values[0];syncValues[1]=p->values[1];return 0;
}
EXPORT int64_t sdk_many(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                        int64_t f, int64_t g, int64_t h, int64_t i) { return a+b+c+d+e+f+g+h+i; }

// DYN200-compatible discovery fixture. No serial device is opened by these tests
static uint32_t portScenario, portConnections;
EXPORT void sdk_port_scenario(uint32_t scenario) { portScenario = scenario; }
EXPORT uint32_t sdk_port_connections() { return portConnections; }
EXPORT uint32_t dyn200_port_count() { return portScenario == 2 ? 0 : portScenario == 1 ? 2 : 3; }
EXPORT int32_t dyn200_port_name(uint32_t index, char *name, size_t nameSize, char *desc, size_t descSize) {
        static const char *names[] = {"COM3", "COM11", "COM42"};
        static const char *descs[] = {"USB 串口甲", "USB 串口乙", "扭矩传感器 USB"};
        if (index >= dyn200_port_count() || portScenario == 3) return -1;
        const uint32_t entry = portScenario == 1 ? (index == 0 ? 2 : 0) : index;
        snprintf(name, nameSize, "%s", names[entry]); snprintf(desc, descSize, "%s", descs[entry]);
        return 0;
}
struct FixturePortParam {
        uint32_t index, count;
        char name[64], desc[128], port[64];
        uint32_t protocol, baud_rate, device_address, stop_bits, timeout_ms, action, connected;
        char active_port[64];
};
EXPORT int32_t dyn200_read_port_param(FixturePortParam *p) {
        p->count = dyn200_port_count();
        if (p->index < p->count)
                dyn200_port_name(p->index, p->name, sizeof(p->name), p->desc, sizeof(p->desc));
        if (!p->port[0]) snprintf(p->port, sizeof(p->port), "%s", p->name);
        if (p->action == 1) {
                ++portConnections; p->connected = 1;
                snprintf(p->active_port, sizeof(p->active_port), "%s", p->port);
        }
        p->action = 0;
        return 0;
}
