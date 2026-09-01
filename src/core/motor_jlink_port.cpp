#include "core/jlink_port.hpp"
#include "core/motor_jlink.hpp"

motor_jlink::Io
motor_jlink::portIo()
{
        return {[]() -> uint64_t {
                        auto &p = JLinkPort::instance();
                        return p.isConnected() && !p.isBusy() ? p.targetEpoch() : 0;
                },
                [](uint64_t epoch, uint32_t address, uint32_t size, void *out) {
                        return JLinkPort::instance().readMemAtEpoch(epoch, address, size, out);
                },
                [](uint64_t epoch, uint32_t address, uint32_t size, const void *in) {
                        return JLinkPort::instance().writeMemAtEpoch(epoch, address, size, in);
                }};
}
