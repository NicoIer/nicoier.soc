#include <soc/soc.h>

#include <stddef.h>
#include <stdint.h>

static int test_version(void)
{
    return soc_get_abi_version() == SOC_ABI_VERSION ? 0 : 1;
}

static int test_context_lifetime(void)
{
    const soc_config config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 0u,
    };
    soc_context* context = NULL;

    if (soc_context_create(&config, &context) != SOC_RESULT_OK) {
        return 1;
    }
    if (context == NULL) {
        return 1;
    }

    soc_context_destroy(context);
    return 0;
}

static int test_invalid_arguments(void)
{
    const soc_config zero_width_config = {
        .struct_size = sizeof(soc_config),
        .width = 0u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 0u,
    };
    const soc_config zero_height_config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 0u,
        .worker_count = 0u,
        .flags = 0u,
    };
    const soc_config oversized_config = {
        .struct_size = sizeof(soc_config),
        .width = SOC_MAX_RASTER_DIMENSION + 1u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 0u,
    };
    const soc_config undersized_config = {
        .struct_size = SOC_CONFIG_SIZE_V1 - 1u,
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 0u,
    };
    const soc_config unsupported_workers_config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 2u,
        .flags = 0u,
    };
    const soc_config unsupported_flags_config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 1u,
    };
    const soc_config single_worker_config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 1u,
        .flags = 0u,
    };
    soc_context* context = (soc_context*)(uintptr_t)1u;

    if (soc_context_create(NULL, &context) != SOC_RESULT_INVALID_ARGUMENT ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&zero_width_config, &context) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&zero_height_config, &context) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&oversized_config, &context) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&undersized_config, &context) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&unsupported_workers_config, &context) !=
            SOC_RESULT_UNSUPPORTED ||
        context != NULL) {
        return 1;
    }
    context = (soc_context*)(uintptr_t)1u;
    if (soc_context_create(&unsupported_flags_config, &context) !=
            SOC_RESULT_UNSUPPORTED ||
        context != NULL) {
        return 1;
    }
    if (soc_context_create(&zero_width_config, NULL) !=
        SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    if (soc_context_create(&single_worker_config, &context) != SOC_RESULT_OK ||
        context == NULL) {
        return 1;
    }

    soc_context_destroy(context);
    soc_context_destroy(NULL);
    return 0;
}

int main(void)
{
    if (test_version() != 0) {
        return 1;
    }
    if (test_context_lifetime() != 0) {
        return 1;
    }
    if (test_invalid_arguments() != 0) {
        return 1;
    }

    return 0;
}
