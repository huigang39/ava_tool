#include "core/firmware_match.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace firmware_match;

static void
put(Bytes &bytes, size_t offset, uint32_t val)
{
        for (size_t i = 0; i < 4; ++i)
                bytes[offset + i] = uint8_t(val >> (i * 8));
}

int
main(int argc, char **argv)
{
        assert(hex(sha256({}).data(), 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert(hex(sha256({'a', 'b', 'c'}).data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert(hex(sha256(std::vector<uint8_t>(1000000, 'a')).data(), 32) ==
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        Image expected;
        expected.address = 0x08001000;
        expected.path    = "correct.axf";
        put(expected.info, 0, magic);
        put(expected.info, 4, 1);
        put(expected.info, 8, infoSize);
        put(expected.info, 12, expected.address);
        put(expected.info, 56, 0x08000401U);
        expected.info[16] = 1;
        Bytes runtime     = expected.info;
        put(runtime, 48, 1);
        put(runtime, 52, ~1U);
        Bytes    flash       = expected.info;
        bool     supported   = true;
        bool     readable    = true;
        uint32_t activeReset = 0x08000401U;
        auto     read        = [&](uint32_t address, uint32_t size, uint8_t *out) {
                if (!readable)
                        return false;
                if (address == 0xE000ED08U && size == 4) {
                        std::memset(out, 0, 4);
                        return true;
                }
                if (address == 4 && size == 4) {
                        for (size_t i = 0; i < 4; ++i)
                                out[i] = uint8_t(activeReset >> (i * 8));
                        return true;
                }
                if (size != infoSize)
                        return false;
                const auto &bytes = address == mailboxAddress ? runtime : flash;
                if (address == mailboxAddress && !supported) {
                        std::memset(out, 0, size);
                        return true;
                }
                std::copy(bytes.begin(), bytes.end(), out);
                return true;
        };
        Gate gate;
        gate.setSource(1, expected);
        assert(gate.check(read).state == State::Matched);
        auto wrong      = expected;
        wrong.info[16] ^= 1;
        wrong.path      = "wrong.axf";
        gate.setSource(1, wrong);
        assert(gate.check(read).state == State::Mismatch);
        assert(!gate.status().allowsAccess());
        gate.setSource(1, expected);
        assert(gate.check(read).state == State::Matched);
        supported = false;
        assert(gate.check(read).state == State::Unavailable);
        assert(!gate.status().allowsAccess());

        // 重新连接到没有标识的固件时保持可用, 不沿用上一台设备的匹配结果
        gate.resetConnection();
        assert(gate.check(read).state == State::Unavailable);
        flash.fill(0xFF);
        assert(gate.check(read).state == State::Unverified);
        assert(gate.status().allowsAccess());
        readable = false;
        assert(gate.check(read).allowsAccess());
        readable = supported = true;
        flash = expected.info;
        gate.resetConnection();
        put(runtime, 4, 2);
        assert(gate.check(read).state == State::Unverified);
        assert(gate.status().allowsAccess());
        put(runtime, 4, 1);
        gate.setSource(1, std::nullopt);
        assert(gate.check(read).allowsAccess());
        supported = readable = true;
        gate.setSource(1, expected, true);
        assert(gate.check(read).state == State::Loading);
        assert(!gate.status().allowsAccess());
        gate.setSource(1, expected);
        gate.setSource(2, wrong);
        assert(gate.check(read).state == State::Mismatch);
        gate.removeSource(2);
        assert(gate.check(read).state == State::Matched);
        flash[16] ^= 1;
        assert(gate.check(read).state == State::Unavailable);
        flash[16] ^= 1;
        put(runtime, 52, 0);
        assert(gate.check(read).state == State::Unavailable);
        put(runtime, 52, ~1U);
        // 旧固件启动或烧录后留下 RAM 标识时, 重新连接仍应进入兼容模式
        flash.fill(0xFF);
        gate.resetConnection();
        assert(gate.check(read).state == State::Unverified);
        assert(gate.status().allowsAccess());
        flash       = expected.info;
        activeReset = 0x08080401U;
        assert(gate.check(read).state == State::Unverified);
        activeReset = 0x08000401U;
        assert(gate.check(read).state == State::Matched);
        gate.setSource(2, std::nullopt);
        assert(gate.check(read).state == State::Matched);

        Gate overrideGate;
        assert(!overrideGate.setMismatchOverride(true));
        overrideGate.setSource(1, expected);
        assert(overrideGate.check(read).state == State::Matched);
        assert(!overrideGate.setMismatchOverride(true));
        overrideGate.setSource(1, wrong);
        assert(overrideGate.check(read).state == State::Mismatch);
        const auto mismatch = overrideGate.status();
        assert(!mismatch.forced && !mismatch.allowsAccess());
        assert(overrideGate.setMismatchOverride(true));
        assert(overrideGate.status().state == State::Mismatch);
        assert(overrideGate.status().forced && overrideGate.status().allowsAccess());
        assert(overrideGate.status().deviceId == mismatch.deviceId);
        assert(overrideGate.status().expectedId == mismatch.expectedId);
        assert(overrideGate.status().detail == mismatch.detail);
        assert(overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(false));
        assert(!overrideGate.status().forced && !overrideGate.status().allowsAccess());
        assert(!overrideGate.check(read).allowsAccess());

        // 设备构建 ID 相同但身份地址变化时, 不能沿用上次强制访问
        assert(overrideGate.setMismatchOverride(true));
        put(runtime, 12, expected.address + 0x100);
        put(flash, 12, expected.address + 0x100);
        assert(overrideGate.check(read).state == State::Mismatch);
        assert(!overrideGate.status().forced && !overrideGate.status().allowsAccess());
        put(runtime, 12, expected.address);
        put(flash, 12, expected.address);
        assert(!overrideGate.check(read).forced);

        // 设备更换构建或重新启动后需要重新选择强制访问
        assert(overrideGate.setMismatchOverride(true));
        runtime[17] ^= 1;
        flash[17] ^= 1;
        assert(overrideGate.check(read).state == State::Mismatch);
        assert(!overrideGate.status().forced);
        runtime[17] ^= 1;
        flash[17] ^= 1;
        assert(!overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(true));
        put(runtime, 48, 2);
        put(runtime, 52, ~2U);
        assert(overrideGate.check(read).state == State::Mismatch);
        assert(!overrideGate.status().forced);
        put(runtime, 48, 1);
        put(runtime, 52, ~1U);
        assert(!overrideGate.check(read).forced);

        // 身份暂时无法读取后取消强制访问, 恢复读取也不会自动重开
        assert(overrideGate.setMismatchOverride(true));
        readable = false;
        assert(overrideGate.check(read).state == State::Unavailable);
        assert(!overrideGate.status().forced && !overrideGate.status().allowsAccess());
        assert(!overrideGate.setMismatchOverride(true));
        readable = true;
        assert(overrideGate.check(read).state == State::Mismatch);
        assert(!overrideGate.status().forced);

        // 替换或增删 AXF, 加载中及重新连接均清除授权
        assert(overrideGate.setMismatchOverride(true));
        overrideGate.setSource(1, wrong);
        assert(!overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(true));
        overrideGate.setSource(2, std::nullopt);
        assert(!overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(true));
        overrideGate.removeSource(2);
        assert(!overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(true));
        overrideGate.setSource(1, wrong, true);
        assert(overrideGate.check(read).state == State::Loading);
        assert(!overrideGate.status().forced && !overrideGate.status().allowsAccess());
        assert(!overrideGate.setMismatchOverride(true));
        overrideGate.setSource(1, wrong);
        assert(!overrideGate.check(read).forced);
        assert(overrideGate.setMismatchOverride(true));
        overrideGate.resetConnection();
        assert(!overrideGate.check(read).forced);

        // 设备变成匹配构建后清除授权, 再次不匹配时重新阻止访问
        assert(overrideGate.setMismatchOverride(true));
        runtime[16] = wrong.info[16];
        flash[16]   = wrong.info[16];
        assert(overrideGate.check(read).state == State::Matched);
        assert(!overrideGate.status().forced && overrideGate.status().allowsAccess());
        runtime[16] = expected.info[16];
        flash[16]   = expected.info[16];
        assert(overrideGate.check(read).state == State::Mismatch);
        assert(!overrideGate.status().forced && !overrideGate.status().allowsAccess());

        // PSC3 secure code/data aliases and relocated startup mailbox.
        Image psc3 = expected;
        psc3.address = 0x12001000U;
        put(psc3.info, 12, psc3.address);
        put(psc3.info, 56, 0x12000401U);
        Bytes pscRuntime = psc3.info, pscFlash = psc3.info;
        put(pscRuntime, 48, 7U);
        put(pscRuntime, 52, ~7U);
        uint32_t pscVector = 0x34000000U;
        bool mailboxRead = false;
        auto pscRead = [&](uint32_t address, uint32_t size, uint8_t *out) {
                if (address == 0x3400FFC0U && size == infoSize) {
                        mailboxRead = true;
                        std::copy(pscRuntime.begin(), pscRuntime.end(), out);
                        return true;
                }
                if (address == psc3.address && size == infoSize) {
                        std::copy(pscFlash.begin(), pscFlash.end(), out);
                        return true;
                }
                if (size == 4 && (address == 0xE000ED08U || address == pscVector + 4)) {
                        const uint32_t value = address == 0xE000ED08U ? pscVector : 0x12000401U;
                        for (unsigned k = 0; k < 4; ++k) out[k] = uint8_t(value >> (8*k));
                        return true;
                }
                return false;
        };
        Gate pscGate;
        pscGate.setSource(1, psc3);
        assert(pscGate.check(pscRead).state == State::Matched && mailboxRead);
        pscVector = 0x12000000U;
        assert(pscGate.check(pscRead).state == State::Matched);
        pscVector = 0x34010000U; // outside 64 KiB SRAM
        assert(pscGate.check(pscRead).state == State::Unavailable);
        pscVector = 0x34000000U;
        pscFlash[16] ^= 1;
        assert(pscGate.check(pscRead).state == State::Unavailable);
        pscFlash[16] ^= 1;
        auto pscWrong = psc3;
        pscWrong.info[16] ^= 1;
        pscGate.setSource(1, pscWrong);
        assert(pscGate.check(pscRead).state == State::Mismatch);
        pscGate.setSource(1, psc3);
        pscGate.setSource(2, expected);
        assert(pscGate.check(pscRead).state == State::Unavailable);
        assert(!psc3Flash(0x32040000U - infoSize + 1));
        assert(!psc3Flash(0x34000000U));
        assert(codeAddress(0x32001000U) == 0x12001000U);

        if (argc == 4) {
                const auto image = Image::load(argv[1], uint32_t(std::stoul(argv[2], nullptr, 0)));
                assert(image.id() == argv[3]);
                std::filesystem::path copied;
                {
                        FileSnapshot snapshot(argv[1]);
                        copied = snapshot.path;
                        assert(Image::load(copied, image.address).id() == image.id());
                }
                assert(!std::filesystem::exists(copied));
                const auto changed = std::filesystem::temp_directory_path() / "ava-build-check-corrupt.axf";
                {
                        std::ifstream     input(argv[1], std::ios::binary);
                        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
                        bytes.back() ^= 1;
                        std::ofstream output(changed, std::ios::binary);
                        output.write(bytes.data(), bytes.size());
                }
                bool rejected = false;
                try {
                        (void)Image::load(changed, image.address);
                } catch (const std::runtime_error &) {
                        rejected = true;
                }
                std::filesystem::remove(changed);
                assert(rejected);
        }
        std::cout << "Firmware match, mismatch override lifecycle, legacy compatibility and SHA-256 tests passed.\n";
}
