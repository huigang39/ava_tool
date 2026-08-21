#ifndef JLINK_DEMO_H
#define JLINK_DEMO_H

#include "module.h"

#include "../user/inc/comm_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 写电机状态到 state_addr,state 为 foc_state_e 枚举值 */
int jlink_set_word(comm_shm_word_e word, uint32_t addr);

/** 写控制模式到 mode_addr,mode 为 foc_mode_e 枚举值 */
int jlink_set_mode(comm_shm_mode_e mode, uint32_t addr);

/** 写 PVCT 参考(位置/速度/电流/力矩等)到 pvct_addr */
int jlink_set_pvct(const struct foc_ref_pvct *pvct, uint32_t pvct_addr);

#ifdef __cplusplus
}
#endif

#endif
