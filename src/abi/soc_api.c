#include <soc/soc.h>

#include "core/soc_context.h"

uint32_t SOC_CALL soc_get_abi_version(void)
{
    return SOC_ABI_VERSION;
}

soc_result SOC_CALL soc_context_create(
    const soc_config* config,
    soc_context** out_context
)
{
    return soc_context_create_internal(config, out_context);
}

void SOC_CALL soc_context_destroy(soc_context* context)
{
    soc_context_destroy_internal(context);
}
