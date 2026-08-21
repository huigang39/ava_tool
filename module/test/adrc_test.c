#include <assert.h>
#include <math.h>

#include "module.h"

int
main(void)
{
    const struct adrc_cfg cfg = {
        .fs      = 10000.0F,
        .b0      = 20.0F,
        .wc      = 50.0F,
        .wo      = 200.0F,
        .out_min = -100.0F,
        .out_max = 100.0F,
    };
    struct adrc_ctl adrc = {0};
    adrc_init(&adrc, cfg);

    float32_t y = 0.0F;
    for (uint32_t i = 0; i < 20000U; ++i) {
        const float32_t disturbance = i >= 5000U ? -20.0F : 0.0F;
        const float32_t feedforward = i >= 5000U ? 0.4F : 0.0F;
        adrc_exec_in_rt(&adrc, 5.0F, y, feedforward);
        y += (cfg.b0 * adrc.out.u + disturbance) / cfg.fs;
        assert(adrc.out.u >= cfg.out_min && adrc.out.u <= cfg.out_max);
    }

    assert(fabsf(y - 5.0F) < 0.02F);
    /* LESO 估计的是相对 ADRC 分量的总扰动: disturbance+b0*feedforward。 */
    assert(fabsf(adrc.lo.d_hat + 12.0F) < 0.2F);

    adrc_reset(&adrc);
    assert(adrc.lo.initialized == false);
    assert(adrc.out.u == 0.0F);

    struct adrc_cfg high_bandwidth_cfg = cfg;
    FS_INIT(20000.0F, &high_bandwidth_cfg);
    high_bandwidth_cfg.wc = 6283.0F;
    high_bandwidth_cfg.wo = 18849.0F;
    adrc_init(&adrc, high_bandwidth_cfg);
    assert(adrc.cfg.wc <= 0.2F * adrc.cfg.fs);
    assert(adrc.cfg.wo <= 0.3F * adrc.cfg.fs);

    const struct adrc_cfg safe_cfg = {
        .fs           = 1000.0F,
        .b0           = 1.0F,
        .wc           = 10.0F,
        .wo           = 20.0F,
        .out_min      = -10.0F,
        .out_max      = 10.0F,
        .d_comp_max   = 1.0F,
        .out_rate_max = 100.0F,
    };
    adrc_init(&adrc, safe_cfg);
    adrc.lo.initialized = true;
    adrc.lo.d_hat_raw = adrc.lo.d_hat = 100.0F;
    adrc_exec_in_rt(&adrc, 10.0F, 0.0F, 0.0F);
    assert(fabsf(adrc.out.d_comp) <= safe_cfg.d_comp_max);
    assert(fabsf(adrc.out.u) <= safe_cfg.out_rate_max / safe_cfg.fs);
    return 0;
}
