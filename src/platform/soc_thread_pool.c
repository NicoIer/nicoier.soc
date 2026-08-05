#include "platform/soc_thread_pool.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(_WIN32_WINNT)
        #define _WIN32_WINNT 0x0601
    #endif
    #include <process.h>
    #include <windows.h>
#else
    #include <pthread.h>
#endif

typedef struct soc_thread_pool_implementation
    soc_thread_pool_implementation;

typedef struct soc_thread_pool_worker {
    soc_thread_pool_implementation* implementation;
} soc_thread_pool_worker;

struct soc_thread_pool_implementation {
    uint32_t helper_count;
    uint32_t created_helper_count;

    soc_thread_pool_callback callback;
    void* user_data;
    uint64_t generation;
    uint32_t active_worker_count;
    uint32_t next_worker_index;
    uint32_t remaining_helpers;
    soc_bool stopping;

    soc_thread_pool_worker* workers;
#if defined(_WIN32)
    HANDLE* threads;
    SRWLOCK run_lock;
    SRWLOCK state_lock;
    CONDITION_VARIABLE work_available;
    CONDITION_VARIABLE work_complete;
#else
    pthread_t* threads;
    pthread_mutex_t run_lock;
    pthread_mutex_t state_lock;
    pthread_cond_t work_available;
    pthread_cond_t work_complete;
#endif
};

#if defined(_WIN32)

static void state_lock(soc_thread_pool_implementation* implementation)
{
    AcquireSRWLockExclusive(&implementation->state_lock);
}

static void state_unlock(soc_thread_pool_implementation* implementation)
{
    ReleaseSRWLockExclusive(&implementation->state_lock);
}

static void work_available_wait(
    soc_thread_pool_implementation* implementation
)
{
    (void)SleepConditionVariableSRW(
        &implementation->work_available,
        &implementation->state_lock,
        INFINITE,
        0u
    );
}

static void work_complete_wait(
    soc_thread_pool_implementation* implementation
)
{
    (void)SleepConditionVariableSRW(
        &implementation->work_complete,
        &implementation->state_lock,
        INFINITE,
        0u
    );
}

static void work_available_broadcast(
    soc_thread_pool_implementation* implementation
)
{
    WakeAllConditionVariable(&implementation->work_available);
}

static void work_available_signal(
    soc_thread_pool_implementation* implementation
)
{
    WakeConditionVariable(&implementation->work_available);
}

static void work_complete_signal(
    soc_thread_pool_implementation* implementation
)
{
    WakeConditionVariable(&implementation->work_complete);
}

#else

static void state_lock(soc_thread_pool_implementation* implementation)
{
    const int result = pthread_mutex_lock(&implementation->state_lock);
    (void)result;
    assert(result == 0);
}

static void state_unlock(soc_thread_pool_implementation* implementation)
{
    const int result = pthread_mutex_unlock(&implementation->state_lock);
    (void)result;
    assert(result == 0);
}

static void work_available_wait(
    soc_thread_pool_implementation* implementation
)
{
    const int result = pthread_cond_wait(
        &implementation->work_available,
        &implementation->state_lock
    );
    (void)result;
    assert(result == 0);
}

static void work_complete_wait(
    soc_thread_pool_implementation* implementation
)
{
    const int result = pthread_cond_wait(
        &implementation->work_complete,
        &implementation->state_lock
    );
    (void)result;
    assert(result == 0);
}

static void work_available_broadcast(
    soc_thread_pool_implementation* implementation
)
{
    const int result = pthread_cond_broadcast(
        &implementation->work_available
    );
    (void)result;
    assert(result == 0);
}

static void work_available_signal(
    soc_thread_pool_implementation* implementation
)
{
    const int result = pthread_cond_signal(
        &implementation->work_available
    );
    (void)result;
    assert(result == 0);
}

static void work_complete_signal(
    soc_thread_pool_implementation* implementation
)
{
    const int result = pthread_cond_signal(&implementation->work_complete);
    (void)result;
    assert(result == 0);
}

#endif

static void worker_run(soc_thread_pool_worker* worker)
{
    soc_thread_pool_implementation* implementation = worker->implementation;
    uint64_t observed_generation = 0u;

    state_lock(implementation);
    for (;;) {
        soc_thread_pool_callback callback;
        void* user_data;
        uint32_t worker_index;
        uint32_t active_worker_count;

        while (!implementation->stopping &&
            (observed_generation == implementation->generation ||
                implementation->next_worker_index >=
                    implementation->active_worker_count)) {
            work_available_wait(implementation);
        }
        if (implementation->stopping) {
            state_unlock(implementation);
            return;
        }

        observed_generation = implementation->generation;
        callback = implementation->callback;
        user_data = implementation->user_data;
        worker_index = implementation->next_worker_index++;
        active_worker_count = implementation->active_worker_count;
        state_unlock(implementation);

        assert(callback != NULL);
        callback(
            user_data,
            worker_index,
            active_worker_count
        );

        state_lock(implementation);
        assert(implementation->remaining_helpers > 0u);
        --implementation->remaining_helpers;
        if (implementation->remaining_helpers == 0u) {
            work_complete_signal(implementation);
        }
    }
}

#if defined(_WIN32)

static unsigned __stdcall worker_entry(void* user_data)
{
    worker_run((soc_thread_pool_worker*)user_data);
    return 0u;
}

static soc_bool create_worker_thread(
    soc_thread_pool_implementation* implementation,
    uint32_t worker_offset
)
{
    const uintptr_t thread_handle = _beginthreadex(
        NULL,
        0u,
        worker_entry,
        &implementation->workers[worker_offset],
        0u,
        NULL
    );
    implementation->threads[worker_offset] = (HANDLE)thread_handle;
    return implementation->threads[worker_offset] != NULL
        ? SOC_TRUE
        : SOC_FALSE;
}

static void join_worker_thread(
    soc_thread_pool_implementation* implementation,
    uint32_t worker_offset
)
{
    const DWORD wait_result = WaitForSingleObject(
        implementation->threads[worker_offset],
        INFINITE
    );
    assert(wait_result == WAIT_OBJECT_0);
    (void)wait_result;
    (void)CloseHandle(implementation->threads[worker_offset]);
}

#else

static void* worker_entry(void* user_data)
{
    worker_run((soc_thread_pool_worker*)user_data);
    return NULL;
}

static soc_bool create_worker_thread(
    soc_thread_pool_implementation* implementation,
    uint32_t worker_offset
)
{
    return pthread_create(
        &implementation->threads[worker_offset],
        NULL,
        worker_entry,
        &implementation->workers[worker_offset]
    ) == 0
        ? SOC_TRUE
        : SOC_FALSE;
}

static void join_worker_thread(
    soc_thread_pool_implementation* implementation,
    uint32_t worker_offset
)
{
    const int result = pthread_join(
        implementation->threads[worker_offset],
        NULL
    );
    (void)result;
    assert(result == 0);
}

#endif

static void stop_and_join_created_workers(
    soc_thread_pool_implementation* implementation
)
{
    uint32_t worker_offset;

    state_lock(implementation);
    implementation->stopping = SOC_TRUE;
    work_available_broadcast(implementation);
    state_unlock(implementation);

    for (worker_offset = 0u;
         worker_offset < implementation->created_helper_count;
         ++worker_offset) {
        join_worker_thread(implementation, worker_offset);
    }
}

soc_result soc_thread_pool_initialize(
    soc_thread_pool* thread_pool,
    uint32_t worker_count
)
{
    soc_thread_pool_implementation* implementation;
    uint32_t worker_offset;
#if !defined(_WIN32)
    soc_bool run_lock_initialized = SOC_FALSE;
    soc_bool state_lock_initialized = SOC_FALSE;
    soc_bool work_available_initialized = SOC_FALSE;
    soc_bool work_complete_initialized = SOC_FALSE;
#endif

    if (thread_pool == NULL || worker_count == 0u) {
        return SOC_RESULT_INVALID_ARGUMENT;
    }

    thread_pool->implementation = NULL;
    thread_pool->worker_count = 0u;
    if (worker_count == 1u) {
        thread_pool->worker_count = 1u;
        return SOC_RESULT_OK;
    }

    implementation = calloc(1u, sizeof(*implementation));
    if (implementation == NULL) {
        return SOC_RESULT_OUT_OF_MEMORY;
    }
    implementation->helper_count = worker_count - 1u;
    implementation->workers = calloc(
        implementation->helper_count,
        sizeof(*implementation->workers)
    );
    implementation->threads = calloc(
        implementation->helper_count,
        sizeof(*implementation->threads)
    );
    if (implementation->workers == NULL || implementation->threads == NULL) {
        free(implementation->threads);
        free(implementation->workers);
        free(implementation);
        return SOC_RESULT_OUT_OF_MEMORY;
    }

#if defined(_WIN32)
    InitializeSRWLock(&implementation->run_lock);
    InitializeSRWLock(&implementation->state_lock);
    InitializeConditionVariable(&implementation->work_available);
    InitializeConditionVariable(&implementation->work_complete);
#else
    if (pthread_mutex_init(&implementation->run_lock, NULL) != 0) {
        goto initialization_failed;
    }
    run_lock_initialized = SOC_TRUE;
    if (pthread_mutex_init(&implementation->state_lock, NULL) != 0) {
        goto initialization_failed;
    }
    state_lock_initialized = SOC_TRUE;
    if (pthread_cond_init(&implementation->work_available, NULL) != 0) {
        goto initialization_failed;
    }
    work_available_initialized = SOC_TRUE;
    if (pthread_cond_init(&implementation->work_complete, NULL) != 0) {
        goto initialization_failed;
    }
    work_complete_initialized = SOC_TRUE;
#endif

    for (worker_offset = 0u;
         worker_offset < implementation->helper_count;
         ++worker_offset) {
        implementation->workers[worker_offset].implementation =
            implementation;
        if (!create_worker_thread(implementation, worker_offset)) {
            stop_and_join_created_workers(implementation);
            goto thread_creation_failed;
        }
        ++implementation->created_helper_count;
    }

    thread_pool->implementation = implementation;
    thread_pool->worker_count = worker_count;
    return SOC_RESULT_OK;

#if !defined(_WIN32)
initialization_failed:
    if (work_complete_initialized) {
        (void)pthread_cond_destroy(&implementation->work_complete);
    }
    if (work_available_initialized) {
        (void)pthread_cond_destroy(&implementation->work_available);
    }
    if (state_lock_initialized) {
        (void)pthread_mutex_destroy(&implementation->state_lock);
    }
    if (run_lock_initialized) {
        (void)pthread_mutex_destroy(&implementation->run_lock);
    }
    free(implementation->threads);
    free(implementation->workers);
    free(implementation);
    return SOC_RESULT_INTERNAL_ERROR;
#endif

thread_creation_failed:
#if !defined(_WIN32)
    (void)pthread_cond_destroy(&implementation->work_complete);
    (void)pthread_cond_destroy(&implementation->work_available);
    (void)pthread_mutex_destroy(&implementation->state_lock);
    (void)pthread_mutex_destroy(&implementation->run_lock);
#endif
    free(implementation->threads);
    free(implementation->workers);
    free(implementation);
    return SOC_RESULT_INTERNAL_ERROR;
}

void soc_thread_pool_shutdown(soc_thread_pool* thread_pool)
{
    soc_thread_pool_implementation* implementation;

    if (thread_pool == NULL || thread_pool->worker_count == 0u) {
        return;
    }

    implementation = thread_pool->implementation;
    if (implementation != NULL) {
#if defined(_WIN32)
        AcquireSRWLockExclusive(&implementation->run_lock);
#else
        {
            const int result = pthread_mutex_lock(&implementation->run_lock);
            (void)result;
            assert(result == 0);
        }
#endif
        stop_and_join_created_workers(implementation);
#if defined(_WIN32)
        ReleaseSRWLockExclusive(&implementation->run_lock);
#else
        {
            const int result = pthread_mutex_unlock(
                &implementation->run_lock
            );
            (void)result;
            assert(result == 0);
        }
        (void)pthread_cond_destroy(&implementation->work_complete);
        (void)pthread_cond_destroy(&implementation->work_available);
        (void)pthread_mutex_destroy(&implementation->state_lock);
        (void)pthread_mutex_destroy(&implementation->run_lock);
#endif
        free(implementation->threads);
        free(implementation->workers);
        free(implementation);
    }

    thread_pool->implementation = NULL;
    thread_pool->worker_count = 0u;
}

void soc_thread_pool_run(
    soc_thread_pool* thread_pool,
    soc_thread_pool_callback callback,
    void* user_data
)
{
    assert(thread_pool != NULL);
    assert(callback != NULL);
    assert(thread_pool == NULL || thread_pool->worker_count > 0u);
    if (thread_pool == NULL ||
        callback == NULL ||
        thread_pool->worker_count == 0u) {
        return;
    }

    soc_thread_pool_run_active(
        thread_pool,
        thread_pool->worker_count,
        callback,
        user_data
    );
}

void soc_thread_pool_run_active(
    soc_thread_pool* thread_pool,
    uint32_t active_worker_count,
    soc_thread_pool_callback callback,
    void* user_data
)
{
    soc_thread_pool_implementation* implementation;
    uint32_t helper_index;

    assert(thread_pool != NULL);
    assert(callback != NULL);
    assert(thread_pool == NULL || thread_pool->worker_count > 0u);
    assert(active_worker_count > 0u);
    assert(thread_pool == NULL ||
        active_worker_count <= thread_pool->worker_count);
    if (thread_pool == NULL ||
        callback == NULL ||
        thread_pool->worker_count == 0u ||
        active_worker_count == 0u ||
        active_worker_count > thread_pool->worker_count) {
        return;
    }

    if (active_worker_count == 1u) {
        callback(user_data, 0u, 1u);
        return;
    }

    implementation = thread_pool->implementation;
    assert(implementation != NULL);
    if (implementation == NULL) {
        return;
    }

#if defined(_WIN32)
    AcquireSRWLockExclusive(&implementation->run_lock);
#else
    {
        const int result = pthread_mutex_lock(&implementation->run_lock);
        (void)result;
        assert(result == 0);
    }
#endif

    state_lock(implementation);
    implementation->callback = callback;
    implementation->user_data = user_data;
    implementation->active_worker_count = active_worker_count;
    implementation->next_worker_index = 1u;
    implementation->remaining_helpers = active_worker_count - 1u;
    ++implementation->generation;
    for (helper_index = 1u;
         helper_index < active_worker_count;
         ++helper_index) {
        work_available_signal(implementation);
    }
    state_unlock(implementation);

    callback(user_data, 0u, active_worker_count);

    state_lock(implementation);
    while (implementation->remaining_helpers != 0u) {
        work_complete_wait(implementation);
    }
    implementation->callback = NULL;
    implementation->user_data = NULL;
    implementation->active_worker_count = 0u;
    implementation->next_worker_index = 0u;
    state_unlock(implementation);

#if defined(_WIN32)
    ReleaseSRWLockExclusive(&implementation->run_lock);
#else
    {
        const int result = pthread_mutex_unlock(&implementation->run_lock);
        (void)result;
        assert(result == 0);
    }
#endif
}

uint32_t soc_thread_pool_worker_count(const soc_thread_pool* thread_pool)
{
    return thread_pool != NULL ? thread_pool->worker_count : 0u;
}
