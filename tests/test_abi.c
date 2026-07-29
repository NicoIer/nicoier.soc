#include <soc/soc.h>

#include <stddef.h>

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
    const soc_config invalid_config = {
        .struct_size = sizeof(soc_config),
        .width = 0u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 0u,
    };
    const soc_config unsupported_config = {
        .struct_size = sizeof(soc_config),
        .width = 320u,
        .height = 180u,
        .worker_count = 0u,
        .flags = 1u,
    };
    soc_context* context = NULL;

    if (soc_context_create(NULL, &context) != SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    if (soc_context_create(&invalid_config, &context) !=
        SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    if (soc_context_create(&invalid_config, NULL) !=
        SOC_RESULT_INVALID_ARGUMENT) {
        return 1;
    }
    if (soc_context_create(&unsupported_config, &context) !=
        SOC_RESULT_UNSUPPORTED) {
        return 1;
    }

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
