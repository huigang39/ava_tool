#include "fsav3_c.h"
#include "fsav3.h"
#include <memory>
#include <string>
#include <unordered_map>

extern "C" int fsav3_c_read_error3(const char *ip, int timeout_ms, uint32_t *error3) {
    static std::unordered_map<std::string, AC3::FSA *> devices;
    if (!ip || !error3 || timeout_ms < 0) return (int)AC3::FSA::ret_e::ARG_ERR;
    AC3::FSA *&fsa = devices[ip];
    if (!fsa) {
        fsa = new AC3::FSA; /* Persistent: avoids unsafe repeated SDK destruction. */
        auto rc = fsa->Init(ip, AC3::FSA::net_recv_mode_e::YIELD_WAIT);
        if (rc != AC3::FSA::ret_e::SUCCESS) { fsa = nullptr; return (int)rc; }
    }
    AC3::FSA::err_code_t code{};
    auto rc = fsa->GetErrCode(code, (double)timeout_ms, 0);
    if (rc == AC3::FSA::ret_e::SUCCESS) *error3 = code.arr[2];
    return (int)rc;
}
