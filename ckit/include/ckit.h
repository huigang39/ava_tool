#ifndef CKIT_H
#define CKIT_H

#include <stddef.h>
#include <stdint.h>

#include "crc.h"
#include "fft.h"
#include "fftw3.h"
#include "lut.h"

#include "fir.h"
#include "iir.h"
#include "rls.h"

#include "wave.h"

#include "list.h"
#include "mempool.h"
#include "mpsc.h"
#include "param_list.h"
#include "safety.h"
#include "rbtree.h"
#include "spsc.h"

#include "log.h"
#include "net.h"
#include "task_scheduler.h"
#include "shm.h"

#include "errdef.h"
#include "fastmath.h"
#include "macrodef.h"
#include "mathdef.h"
#include "platdef.h"

#include "benchmark.h"
#include "bitops.h"
#include "printops.h"
#include "timeops.h"

#include "jlink.h"
#include "jlink_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CKIT_VER_MAJOR (0)
#define CKIT_VER_MINOR (0)
#define CKIT_VER_PATCH (1)

#define CKIT_VER VER_PACK(CKIT_VER_MAJOR, CKIT_VER_MINOR, CKIT_VER_PATCH)
#define CKIT_VER_STR VER_STR(CKIT_VER_MAJOR, CKIT_VER_MINOR, CKIT_VER_PATCH)

static const uint32_t ckit_ver = CKIT_VER;

#ifdef __cplusplus
}
#endif

#endif
