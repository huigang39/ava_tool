#ifndef AVA_TOOL_MODULE_COMPAT_H
#define AVA_TOOL_MODULE_COMPAT_H

// ava_tool compatibility facade for the public module API. The updated module
// uses standard-width integer names and struct/enum tags directly; keep the
// application's compact aliases local instead of putting them back into module.
#include "../module/module.h"

#include <cstddef>
#include <cstdint>

using u8  = std::uint8_t;
using i8  = std::int8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using fft_cfg_t   = struct fft_cfg;
using fft_t       = struct fft;
using log_cfg_t   = struct log_cfg;
using log_t       = struct log;
using mempool_t   = struct mempool;
using shm_cfg_t   = struct shm_cfg;
using shm_t       = struct shm;
using wave_cfg_t  = struct wave_cfg;
using wave_t      = struct wave;
using wave_type_t = enum wave_type;

#endif // AVA_TOOL_MODULE_COMPAT_H
