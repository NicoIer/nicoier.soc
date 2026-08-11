#ifndef SOC_DEVICE_SUPPORT_H_INCLUDED
#define SOC_DEVICE_SUPPORT_H_INCLUDED

#include <soc/soc.h>

#define SOC_ARM32_HWCAP_NEON (1ul << 12u)
#define SOC_ARM32_HWCAP_VFPV4 (1ul << 16u)

static inline soc_bool soc_arm32_neon_vfpv4_is_supported(
    unsigned long hwcap
)
{
    const unsigned long required =
        SOC_ARM32_HWCAP_NEON | SOC_ARM32_HWCAP_VFPV4;

    return (hwcap & required) == required ? SOC_TRUE : SOC_FALSE;
}

#endif
