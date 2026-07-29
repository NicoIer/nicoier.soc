#ifndef SOC_H_INCLUDED
#define SOC_H_INCLUDED

#include <stdint.h>

#if defined(_WIN32)
    #if defined(SOC_STATIC)
        #define SOC_API
    #elif defined(SOC_BUILDING_LIBRARY)
        #define SOC_API __declspec(dllexport)
    #else
        #define SOC_API __declspec(dllimport)
    #endif
    #define SOC_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
    #define SOC_API __attribute__((visibility("default")))
    #define SOC_CALL
#else
    #define SOC_API
    #define SOC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SOC_ABI_VERSION_MAJOR 1u
#define SOC_ABI_VERSION_MINOR 0u
#define SOC_ABI_VERSION \
    ((SOC_ABI_VERSION_MAJOR << 16u) | SOC_ABI_VERSION_MINOR)

typedef int32_t soc_result;

#define SOC_RESULT_OK ((soc_result)0)
#define SOC_RESULT_INVALID_ARGUMENT ((soc_result)-1)
#define SOC_RESULT_OUT_OF_MEMORY ((soc_result)-2)
#define SOC_RESULT_UNSUPPORTED ((soc_result)-3)
#define SOC_RESULT_INTERNAL_ERROR ((soc_result)-4)

typedef struct soc_context soc_context;

typedef struct soc_config {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} soc_config;

SOC_API uint32_t SOC_CALL soc_get_abi_version(void);

SOC_API soc_result SOC_CALL soc_context_create(
    const soc_config* config,
    soc_context** out_context
);

SOC_API void SOC_CALL soc_context_destroy(soc_context* context);

#ifdef __cplusplus
}
#endif

#endif
