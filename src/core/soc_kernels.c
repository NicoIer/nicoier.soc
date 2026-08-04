#include "core/soc_kernels.h"

#include "occlusion/soc_hiz.h"
#include "occlusion/soc_visibility.h"

#include <stddef.h>

void soc_kernel_clear_f32_scalar(
    float* destination,
    size_t count,
    float value
)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        destination[index] = value;
    }
}

static const soc_kernel_table scalar_kernels = {
    .backend = SOC_KERNEL_BACKEND_SCALAR,
    .clear_f32 = soc_kernel_clear_f32_scalar,
    .reduce_hiz_level_f32 = soc_hiz_reduce_level_scalar,
    .test_aabbs = soc_occlusion_test_aabbs,
};

const soc_kernel_table* soc_kernel_table_scalar(void)
{
    return &scalar_kernels;
}

const soc_kernel_table* soc_kernel_table_select(
    const soc_cpu_features* features
)
{
    const soc_kernel_table* neon = soc_kernel_table_neon();

    if (features != NULL && neon != NULL &&
        features->architecture == SOC_CPU_ARCHITECTURE_ARM64 &&
        soc_cpu_features_has(features, SOC_CPU_FEATURE_NEON)) {
        return neon;
    }
    return soc_kernel_table_scalar();
}

const soc_kernel_table* soc_kernel_table_for_backend(
    soc_kernel_backend backend
)
{
    if (backend == SOC_KERNEL_BACKEND_SCALAR) {
        return soc_kernel_table_scalar();
    }
    if (backend == SOC_KERNEL_BACKEND_NEON) {
        return soc_kernel_table_neon();
    }
    return NULL;
}
