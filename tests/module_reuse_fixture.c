#include "mempool.h"

size_t
module_reuse_c_alignment(void)
{
        return MEMPOOL_ALIGN;
}

size_t
module_reuse_c_mempool_size(void)
{
        return sizeof(struct mempool);
}
