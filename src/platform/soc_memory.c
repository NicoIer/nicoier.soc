#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "platform/soc_memory.h"

#if defined(_WIN32)
#include <malloc.h>
#else
#include <stdlib.h>
#endif

void* soc_aligned_alloc(size_t alignment, size_t size)
{
    if (alignment < sizeof(void*) ||
        (alignment & (alignment - 1u)) != 0u) {
        return NULL;
    }

#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    {
        void* allocation = NULL;

        if (posix_memalign(&allocation, alignment, size) != 0) {
            return NULL;
        }
        return allocation;
    }
#endif
}

void soc_aligned_free(void* allocation)
{
    if (allocation == NULL) {
        return;
    }

#if defined(_WIN32)
    _aligned_free(allocation);
#else
    free(allocation);
#endif
}
