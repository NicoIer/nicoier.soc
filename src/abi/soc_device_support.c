#include <soc/soc.h>

#include "abi/soc_device_support.h"

#if defined(SOC_ABI_REQUIRES_ARM32_NEON_VFPV4)
    #if !defined(__ANDROID__) || !defined(__arm__)
        #error "ARM32 NEON/VFPv4 support probe requires Android AArch32"
    #endif
    #include <sys/auxv.h>
#endif

soc_bool SOC_CALL soc_device_is_supported(void)
{
#if defined(SOC_ABI_REQUIRES_ARM32_NEON_VFPV4)
    return soc_arm32_neon_vfpv4_is_supported(getauxval(AT_HWCAP));
#else
    return SOC_TRUE;
#endif
}
