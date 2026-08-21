#ifndef MODULE_H
#define MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "inc/motor_control/foc.h"
#include "inc/motor_control/focdef.h"
#include "inc/motor_control/observer/flux.h"
#include "inc/motor_control/observer/hfi.h"
#include "inc/motor_control/observer/luenberger.h"
#include "inc/motor_control/observer/smo.h"
#include "inc/motor_control/types.h"

#include "inc/crc.h"
#include "inc/fft.h"
#include "inc/fftw3.h"
#include "inc/lut.h"

#include "inc/fir.h"
#include "inc/iir.h"
#include "inc/motor_control/pll.h"
#include "inc/rls.h"

#include "inc/motor_control/controller/adrc.h"
#include "inc/motor_control/controller/pid.h"
#include "inc/wave.h"

#include "inc/list.h"
#include "inc/mempool.h"
#include "inc/mpsc.h"
#include "inc/rbtree.h"
#include "inc/spsc.h"

#include "inc/log.h"
#include "inc/net.h"
#include "inc/sch.h"
#include "inc/shm.h"

#include "inc/errdef.h"
#include "inc/fastmath.h"
#include "inc/macrodef.h"
#include "inc/mathdef.h"
#include "inc/platdef.h"

#include "inc/benchmark.h"
#include "inc/bitops.h"
#include "inc/printops.h"
#include "inc/timeops.h"

#include "inc/jlink.h"
#include "inc/jlinkport.h"

#define MODULE_VER_MAJOR  0
#define MODULE_VER_MINOR  0
#define MODULE_VER_PATCH  1

#define MODULE_VER        VERSION_PACK(MODULE_VER_MAJOR, MODULE_VER_MINOR, MODULE_VER_PATCH)
#define MODULE_VER_STRING VERSION_STRING(MODULE_VER_MAJOR, MODULE_VER_MINOR, MODULE_VER_PATCH)

static const uint32_t module_ver = MODULE_VER;

#endif // !MODULE_H
