#include "../ckit/include/log.h"
#include "../ckit/include/mempool.h"
#include "../ckit/include/sim_stream.hpp"
#include "core/flash_array.hpp"
#include "core/parameter_codec.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>

extern "C" size_t module_reuse_c_alignment(void);
extern "C" size_t module_reuse_c_mempool_size(void);

static void
require(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

template <class Operation>
static void
expect_failure(Operation operation)
{
        bool failed = false;
        try {
                operation();
        } catch (const std::runtime_error &) {
                failed = true;
        }
        require(failed, "Invalid parameter input must be rejected");
}

static void
check_parameter_records()
{
        const parameter_codec::Layout layout(20,
                                             {{{7, 1, PARAM_LIST_U32, 1, 5, 3}, "flags"},
                                              {{9, 7, PARAM_LIST_F32, 2, 0, 32}, "gain"},
                                              {{12, 13, PARAM_LIST_ENUM32, 1, 0, 32}, "mode"}});
        const std::array<uint32_t, 3> values  = {5, std::bit_cast<uint32_t>(2.5f), UINT32_MAX};
        const auto                    records = layout.encode_values(values);
        require(records ==
                    std::vector<uint8_t>({7, 0, 1, 1, 5, 0, 0, 0, 9, 0, 2, 2, 0, 0, 32, 64, 12, 0, 3, 1, 255, 255, 255, 255}),
                "Records must retain the firmware's byte format");
        std::vector<uint8_t> config(20, 0xA5);
        auto                 expected = config;
        uint32_t             flags;
        std::memcpy(&flags, expected.data() + 1, sizeof(flags));
        flags = (flags & ~(7u << 5)) | (5u << 5);
        std::memcpy(expected.data() + 1, &flags, sizeof(flags));
        std::memcpy(expected.data() + 7, &values[1], sizeof(uint32_t));
        std::memcpy(expected.data() + 13, &values[2], sizeof(uint32_t));
        layout.decode(config, records);
        require(config == expected, "Unaligned fields and neighboring bits must be preserved");
        require(layout.encode(config) == records, "Configuration must encode to the same records");
        require(layout.read_values(config) == std::vector<uint32_t>(values.begin(), values.end()),
                "Field extraction must preserve float and signed enum bits");

        auto reordered = records;
        std::rotate(reordered.begin(), reordered.begin() + PARAM_LIST_RECORD_SIZE, reordered.end());
        layout.decode(config, reordered);
        require(config == expected, "Record order must not affect parameter mapping");
        const std::array<uint8_t, 8> unknown = {99, 0, 1, 1, 42, 0, 0, 0};
        reordered.insert(reordered.end(), unknown.begin(), unknown.end());
        layout.decode(config, reordered);
        require(config == expected, "Unknown parameter IDs must remain compatible");
        layout.decode(config, {});
        require(config == expected, "Missing records must preserve current fields");

        for (unsigned corruption = 0; corruption < 5; ++corruption) {
                auto invalid = records;
                if (corruption == 0)
                        invalid[10] = PARAM_LIST_U32;
                else if (corruption == 1)
                        invalid[11] = 3;
                else if (corruption == 2)
                        invalid[4] = 8;
                else if (corruption == 3)
                        invalid.push_back(0);
                else {
                        const auto duplicate = std::vector<uint8_t>(records.begin(), records.begin() + 8);
                        invalid.insert(invalid.end(), duplicate.begin(), duplicate.end());
                }
                expect_failure([&] { layout.decode(config, invalid); });
                require(config == expected, "Failed decoding must not partially change configuration");
        }
        for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                auto invalidValues = values;
                invalidValues[1]   = std::bit_cast<uint32_t>(invalid);
                expect_failure([&] { layout.encode_values(invalidValues); });
        }
        const parameter_codec::Layout moved(24,
                                            {{{12, 0, PARAM_LIST_ENUM32, 1, 0, 32}, "mode"},
                                             {{7, 8, PARAM_LIST_U32, 1, 0, 3}, "flags"},
                                             {{9, 16, PARAM_LIST_F32, 2, 0, 32}, "gain"}});
        std::vector<uint8_t>          destination(24);
        moved.decode(destination, records);
        require(moved.read_values(destination) == std::vector<uint32_t>({values[2], values[0], values[1]}),
                "Permanent IDs must transfer values between different AXF layouts");
        for (const auto &fields : std::vector<std::vector<parameter_codec::Field>>{
                 {{{1, 18, PARAM_LIST_U32, 1, 0, 32}, "outside"}},
                 {{{1, 0, PARAM_LIST_U32, 1, 0, 32}, "one"}, {{1, 4, PARAM_LIST_U32, 1, 0, 32}, "duplicate"}},
                 {{{1, 0, PARAM_LIST_U32, 0, 0, 32}, "revision"}},
                 {{{1, 0, PARAM_LIST_U32, 1, 31, 2}, "bits"}}}) {
                const parameter_codec::Layout invalid(20, fields);
                expect_failure([&] { invalid.decode(config, {}); });
                require(config == expected, "Invalid layouts must not change configuration");
        }
        std::vector<uint8_t> wrongSize(19);
        expect_failure([&] { layout.encode(wrongSize); });
        expect_failure([&] { layout.decode(wrongSize, records); });
        expect_failure([&] { layout.encode_values(std::span(values).first(2)); });
        std::cout << "PASS: firmware parameter records, layout changes, compatibility and atomic failure\n";
}

static void
check_half_precision()
{
        for (uint32_t bits = 0; bits <= UINT16_MAX; ++bits) {
                const uint8_t  little[] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8)};
                const uint8_t  big[]    = {little[1], little[0]};
                const unsigned exponent = (bits >> 10) & 31, fraction = bits & 1023;
                double         expected = exponent == 31  ? (fraction ? std::numeric_limits<double>::quiet_NaN()
                                                                      : std::numeric_limits<double>::infinity())
                                          : exponent == 0 ? std::ldexp(double(fraction), -24)
                                                          : std::ldexp(double(1024 + fraction), int(exponent) - 25);
                if (bits & 0x8000)
                        expected = -expected;
                for (double actual : {flasharray::val(little, flasharray::Type::F16, false),
                                      flasharray::val(big, flasharray::Type::F16, true)})
                        require(std::isnan(expected) ? std::isnan(actual)
                                                     : actual == expected && std::signbit(actual) == std::signbit(expected),
                                "Module half conversion must retain all binary16 values and both byte orders");
        }
        std::cout << "PASS: all 65536 half precision bit patterns in both byte orders\n";
}

static void
check_memory_pool()
{
        require(module_reuse_c_alignment() == MEMPOOL_ALIGN && module_reuse_c_mempool_size() == sizeof(mempool),
                "C and C++ memory pool layouts must agree");
        alignas(std::max_align_t) std::array<uint8_t, 4096> storage{};
        mempool                                             pool{};
        pool.buf = storage.data();
        pool.cap = storage.size();
        require(mempool_init(&pool) == 0, "Updated module memory pool must initialize");
        void *first  = mempool_alloc(&pool, 17);
        void *second = mempool_alloc(&pool, 39);
        require(first && second && first != second, "Memory pool must allocate distinct blocks");
        require(reinterpret_cast<uintptr_t>(first) % MEMPOOL_ALIGN == 0 &&
                    reinterpret_cast<uintptr_t>(second) % MEMPOOL_ALIGN == 0,
                "Module allocation must meet C and C++ alignment");
        require(mempool_free(&pool, first) == 0 && mempool_free(&pool, second) == 0, "Allocated blocks must be releasable");
        std::cout << "PASS: C/C++ memory pool layout, alignment and allocation\n";
}

static int
collect_log(void *context, const void *data, size_t size)
{
        static_cast<std::string *>(context)->append(static_cast<const char *>(data), size);
        return static_cast<int>(size);
}

static uint64_t
log_time()
{
        return 1234567;
}

static void
check_async_log()
{
        alignas(std::max_align_t) std::array<uint8_t, 16384> storage{};
        mempool                                              pool{};
        pool.buf = storage.data();
        pool.cap = storage.size();
        require(mempool_init(&pool) == 0, "Log memory pool must initialize");
        std::string received;
        log_cfg     config{};
        config.mode       = LOG_MODE_ASYNC;
        config.level      = LOG_LEVEL_INFO;
        config.fmt        = LOG_FMT_TXT;
        config.mempool      = &pool;
        config.fd           = &received;
        config.chunk_size   = 256;
        config.flush_cap    = 512;
        config.producer_cnt = 2;
        config.get_ts     = log_time;
        config.flush      = collect_log;
        struct log logger{};
        require(log_init(&logger, config) == 0, "Updated asynchronous logger must initialize");
        require(log_info(&logger, 0, "information\n") == 0 && log_err(&logger, 1, "error\n") == 0 &&
                    log_debug(&logger, 0, "hidden\n") == 0,
                "Log producers and level filtering must accept valid input");
        require(log_flush(&logger) == 0, "Asynchronous log queue must drain");
        require(received.find("information") != std::string::npos && received.find("error") != std::string::npos &&
                    received.find("hidden") == std::string::npos,
                "Updated log API must retain messages and filter debug output");
        require(log_deinit(&logger) == 0, "Log shutdown must flush and release module storage");
        std::cout << "PASS: asynchronous log producers, filtering, flush and shutdown\n";
}

static void
check_simulation_frames()
{
        struct SharedRegion {
                shm val{};
                ~SharedRegion()
                {
                        if (val.lo.addr)
                                shm_deinit(&val);
                }
        } writer;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if OS(WIN)
        const std::string name = "Local\\AvA.ModuleReuse." + std::to_string(stamp);
#else
        const std::string name = "/ava_module_reuse_" + std::to_string(stamp);
#endif
        const shm_cfg config{name.c_str(), SHM_READWRITE, ava_sim::regionBytes};
        require(shm_init(&writer.val, config) == 0, "Shared memory fixture must initialize");
        ava_sim::Reader reader;
        require(reader.open(name), "Simulation reader must open the module queue");
        auto *queue = writer.val.lo.spsc;
        ATOMIC_STORE(&queue->wp, queue->cap - 4);
        ATOMIC_STORE(&queue->rp, queue->cap - 4);
        ava_sim::Sample source;
        source.epoch = 123;
        source.kp    = 345;
        source.kd    = 67;
        require(shm_write(&writer.val, &source, 64) == 64, "Partial frame prefix must be queued");
        ava_sim::Sample result;
        result.epoch = 999;
        require(!reader.read(result) && result.epoch == 999 && spsc_avail(queue) == 64,
                "Incomplete frame must retain both caller output and queue read position");
        require(shm_write(&writer.val, &source.kp, 8) == 8, "Frame extension must be queued");
        require(reader.read(result) && result.epoch == source.epoch && result.kp == source.kp && result.kd == source.kd,
                "Peek and read must handle a complete v2 frame across ring wrap");
        source.version = 1;
        source.bytes   = 64;
        require(shm_write(&writer.val, &source, 64) == 64 && reader.read(result) && result.version == 1 && result.kp == 100 &&
                    result.kd == 10,
                "Legacy v1 frames must retain default extension values");
        require(!reader.read(result), "An empty simulation queue must report no frame");
        std::cout << "PASS: module queue peek, partial frames, ring wrap and legacy simulation frames\n";
}

int
main()
{
        try {
                check_parameter_records();
                check_half_precision();
                check_memory_pool();
                check_async_log();
                check_simulation_frames();
        } catch (const std::exception &error) {
                std::cerr << "FAIL: " << error.what() << '\n';
                return 1;
        }
}
