#include "clarkepark.h"
#include "mathdef.h"

f32_ab_t
clarke(const f32_uvw_t f32_abc, const f32 mi)
{
        f32_ab_t f32_ab;
        f32_ab.a = mi * (f32_abc.u - 0.5f * (f32_abc.v + f32_abc.w));
        f32_ab.b = mi * (f32_abc.v - f32_abc.w) * DIV_SQRT_3_BY_2;
        return f32_ab;
}

f32_uvw_t
inv_clarke(const f32_ab_t f32_ab)
{
        f32_uvw_t f32_uvw;
        f32       f32_a = -(f32_ab.a * 0.5f);
        f32       f32_b = f32_ab.b * DIV_SQRT_3_BY_2;
        f32_uvw.u       = f32_ab.a;
        f32_uvw.v       = f32_a + f32_b;
        f32_uvw.w       = f32_a - f32_b;
        return f32_uvw;
}

f32_dq_t
park(const f32_ab_t f32_ab, const f32 theta)
{
        f32_dq_t f32_dq;
        f32_dq.d = f32_ab.b * SIN(theta) + f32_ab.a * COS(theta);
        f32_dq.q = f32_ab.b * COS(theta) - f32_ab.a * SIN(theta);
        return f32_dq;
}

f32_ab_t
inv_park(const f32_dq_t f32_dq, const f32 theta)
{
        f32_ab_t f32_ab;
        f32_ab.a = f32_dq.d * COS(theta) - f32_dq.q * SIN(theta);
        f32_ab.b = f32_dq.d * SIN(theta) + f32_dq.q * COS(theta);
        return f32_ab;
}
