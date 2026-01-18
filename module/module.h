#ifndef MODULE_H
#define MODULE_H

#include "inc/foc.h"
#include "inc/focdef.h"
#include "inc/linerhall.h"

#include "inc/clarkepark.h"
#include "inc/crc.h"
#include "inc/fft.h"
#include "inc/fftw3.h"

#include "inc/fir.h"
#include "inc/iir.h"
#include "inc/maf.h"
#include "inc/pll.h"
#include "inc/rls.h"

#include "inc/adrc.h"
#include "inc/pid.h"
#include "inc/wavegen.h"

#include "inc/list.h"
#include "inc/mempool.h"
#include "inc/mpsc.h"
#include "inc/rbtree.h"
#include "inc/spsc.h"

#include "inc/hfi.h"
#include "inc/lbg.h"
#include "inc/smo.h"

#include "inc/log.h"
#include "inc/net.h"
#include "inc/sch.h"
#include "inc/shm.h"

#include "inc/errdef.h"
#include "inc/fastmath.h"
#include "inc/macrodef.h"
#include "inc/mathdef.h"
#include "inc/typedef.h"

#include "inc/benchmark.h"
#include "inc/bitops.h"
#include "inc/printops.h"
#include "inc/timeops.h"

#include "inc/jlink.h"

#define MODULE_VER \
        (((u32)MODULE_VER_MAJOR << 24) | ((u32)MODULE_VER_MINOR << 16) | ((u32)MODULE_VER_PATCH << 8) | ((u32)MODULE_VER_BUILD))

#define MODULE_VER_MAJOR (0x00)
#define MODULE_VER_MINOR (0x00)
#define MODULE_VER_PATCH (0x00)
#define MODULE_VER_BUILD (0x01)

static const u32 module_ver = MODULE_VER;

#endif // !MODULE_H
