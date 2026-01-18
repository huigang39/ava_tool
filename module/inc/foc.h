#include "focdef.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_init(foc_t *foc, foc_cfg_t foc_cfg);
void foc_exec(foc_t *foc);

void           foc_set_ref(foc_t *foc, foc_ref_pvct_t ref_pvct);
foc_fdb_pvct_t foc_get_fdb(foc_t *foc);

#ifdef __cplusplus
}
#endif
