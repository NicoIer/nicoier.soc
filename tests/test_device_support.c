#include <soc/soc.h>

#include "abi/soc_device_support.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return 1; \
        } \
    } while (0)

int main(void)
{
    CHECK(soc_arm32_neon_vfpv4_is_supported(0ul) == SOC_FALSE);
    CHECK(soc_arm32_neon_vfpv4_is_supported(
        SOC_ARM32_HWCAP_NEON
    ) == SOC_FALSE);
    CHECK(soc_arm32_neon_vfpv4_is_supported(
        SOC_ARM32_HWCAP_VFPV4
    ) == SOC_FALSE);
    CHECK(soc_arm32_neon_vfpv4_is_supported(
        SOC_ARM32_HWCAP_NEON | SOC_ARM32_HWCAP_VFPV4
    ) == SOC_TRUE);
    CHECK(soc_arm32_neon_vfpv4_is_supported(~0ul) == SOC_TRUE);

    const soc_bool supported = soc_device_is_supported();
    CHECK(supported == SOC_FALSE || supported == SOC_TRUE);
    return 0;
}
