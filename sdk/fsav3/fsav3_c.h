#ifndef FSAV3_C_H
#define FSAV3_C_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 0=success, -302=timeout, other values are fsav3 return codes. */
int fsav3_c_read_error3(const char *ip, int timeout_ms, uint32_t *error3);
#ifdef __cplusplus
}
#endif
#endif
