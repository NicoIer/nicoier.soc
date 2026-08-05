#ifndef SOC_PLATFORM_MEMORY_H
#define SOC_PLATFORM_MEMORY_H

#include <stddef.h>

void* soc_aligned_alloc(size_t alignment, size_t size);
void soc_aligned_free(void* allocation);

#endif
