#ifndef CLARKEPARK_H
#define CLARKEPARK_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

// clang-format off
/**
 * @brief 克拉克变换
 * 
 * @details $\begin{bmatrix}i_\alpha \\i_\beta\end{bmatrix}=\sqrt{\frac{2}{3}}\begin{bmatrix}1 & -\frac{1}{2} & -\frac{1}{2} \\0 & \frac{\sqrt{3}}{2} & -\frac{\sqrt{3}}{2}\end{bmatrix}\begin{bmatrix}i_u \\i_v \\i_w\end{bmatrix}$
 * 
 * @param f32_abc 
 * @param mi 调制比 
 * @return f32_ab_t 
 */
// clang-format on
f32_ab_t  clarke(f32_uvw_t f32_abc, f32 mi);
f32_uvw_t inv_clarke(f32_ab_t f32_ab);
f32_dq_t  park(f32_ab_t f32_ab, f32 theta);
f32_ab_t  inv_park(f32_dq_t f32_dq, f32 theta);

#ifdef __cplusplus
}
#endif

#endif // !CLARKEPARK_H
