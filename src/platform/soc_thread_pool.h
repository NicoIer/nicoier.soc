#ifndef SOC_THREAD_POOL_H_INCLUDED
#define SOC_THREAD_POOL_H_INCLUDED

#include <soc/soc_types.h>

#include <stdint.h>

typedef void (*soc_thread_pool_callback)(
    void* user_data,
    uint32_t worker_index,
    uint32_t worker_count
);

/*
 * A persistent synchronous thread team. worker_count includes the calling
 * thread, which always executes lane zero. The implementation pointer is
 * private to soc_thread_pool.c so contexts can embed this type without
 * exposing platform thread primitives throughout the core.
 */
typedef struct soc_thread_pool {
    void* implementation;
    uint32_t worker_count;
} soc_thread_pool;

soc_result soc_thread_pool_initialize(
    soc_thread_pool* thread_pool,
    uint32_t worker_count
);

void soc_thread_pool_shutdown(soc_thread_pool* thread_pool);

/*
 * Invokes callback exactly once for every lane and returns after all lanes
 * finish. Concurrent or recursive calls on the same pool are unsupported.
 */
void soc_thread_pool_run(
    soc_thread_pool* thread_pool,
    soc_thread_pool_callback callback,
    void* user_data
);

uint32_t soc_thread_pool_worker_count(const soc_thread_pool* thread_pool);

#endif
