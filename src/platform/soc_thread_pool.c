#if defined(__ANDROID__)
    #define SOC_THREAD_POOL_USE_ANDROID_AFFINITY 1
    /* Bionic exposes cpu_set_t and sched_setaffinity under __USE_GNU. */
    #if !defined(_GNU_SOURCE)
        #define _GNU_SOURCE 1
    #endif
#else
    #define SOC_THREAD_POOL_USE_ANDROID_AFFINITY 0
#endif

#include "platform/soc_thread_pool.h"

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
    #include <fcntl.h>
    #include <sched.h>
    #include <unistd.h>
#endif

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

#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
    cpu_set_t performance_cpu_set;
    cpu_set_t caller_original_cpu_set;
    soc_bool performance_affinity_enabled;
    soc_bool caller_affinity_changed;
#endif
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

uint32_t soc_thread_pool_select_performance_cpus(
    const uint64_t* scores,
    uint32_t cpu_count,
    uint32_t worker_count,
    soc_bool* out_selected
)
{
    uint32_t selected_count = 0u;
    uint32_t cpu_index;

    if (scores == NULL || out_selected == NULL || cpu_count == 0u ||
        worker_count == 0u) {
        return 0u;
    }
    for (cpu_index = 0u; cpu_index < cpu_count; ++cpu_index) {
        out_selected[cpu_index] = SOC_FALSE;
    }

    while (selected_count < worker_count && selected_count < cpu_count) {
        uint64_t highest_score = 0u;
        uint64_t lowest_score = UINT64_MAX;

        for (cpu_index = 0u; cpu_index < cpu_count; ++cpu_index) {
            if (out_selected[cpu_index] != SOC_TRUE &&
                scores[cpu_index] > highest_score) {
                highest_score = scores[cpu_index];
            }
            if (out_selected[cpu_index] != SOC_TRUE &&
                scores[cpu_index] < lowest_score) {
                lowest_score = scores[cpu_index];
            }
        }
        if (selected_count == 0u && highest_score == lowest_score) {
            for (cpu_index = 0u; cpu_index < cpu_count; ++cpu_index) {
                out_selected[cpu_index] = SOC_TRUE;
            }
            return cpu_count;
        }
        for (cpu_index = 0u; cpu_index < cpu_count; ++cpu_index) {
            if (out_selected[cpu_index] != SOC_TRUE &&
                scores[cpu_index] == highest_score) {
                out_selected[cpu_index] = SOC_TRUE;
                ++selected_count;
                if (selected_count == worker_count) {
                    break;
                }
            }
        }
    }
    return selected_count;
}

#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY

static soc_bool read_positive_uint64_file(
    const char* path,
    uint64_t* out_value
)
{
    char buffer[32];
    uint64_t value = 0u;
    ssize_t length;
    size_t index = 0u;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return SOC_FALSE;
    }
    length = read(descriptor, buffer, sizeof(buffer));
    (void)close(descriptor);
    if (length <= 0) {
        return SOC_FALSE;
    }
    while (index < (size_t)length &&
        buffer[index] >= '0' && buffer[index] <= '9') {
        value = value * UINT64_C(10) +
            (uint64_t)(buffer[index] - '0');
        ++index;
    }
    if (index == 0u || value == 0u) {
        return SOC_FALSE;
    }
    *out_value = value;
    return SOC_TRUE;
}

static soc_bool read_cpu_scores(
    const uint32_t* cpu_indices,
    uint32_t cpu_count,
    const char* attribute,
    uint64_t* out_scores
)
{
    char path[128];
    uint32_t index;

    for (index = 0u; index < cpu_count; ++index) {
        const int length = snprintf(
            path,
            sizeof(path),
            "/sys/devices/system/cpu/cpu%" PRIu32 "/%s",
            cpu_indices[index],
            attribute
        );

        if (length <= 0 || (size_t)length >= sizeof(path) ||
            read_positive_uint64_file(path, &out_scores[index]) != SOC_TRUE) {
            return SOC_FALSE;
        }
    }
    return SOC_TRUE;
}

static void discover_performance_cpu_set(
    soc_thread_pool_implementation* implementation,
    uint32_t worker_count
)
{
    static const char capacity_attribute[] = "cpu_capacity";
    static const char frequency_attribute[] =
        "cpufreq/cpuinfo_max_freq";
    cpu_set_t allowed_cpu_set;
    uint32_t cpu_indices[CPU_SETSIZE];
    uint64_t scores[CPU_SETSIZE];
    soc_bool selected[CPU_SETSIZE];
    uint32_t allowed_count = 0u;
    uint32_t selected_count;
    uint32_t cpu_index;

    if (sched_getaffinity(0, sizeof(allowed_cpu_set), &allowed_cpu_set) != 0) {
        return;
    }
    for (cpu_index = 0u; cpu_index < CPU_SETSIZE; ++cpu_index) {
        if (CPU_ISSET(cpu_index, &allowed_cpu_set)) {
            cpu_indices[allowed_count++] = cpu_index;
        }
    }
    if (allowed_count <= worker_count) {
        return;
    }

    if (read_cpu_scores(
            cpu_indices,
            allowed_count,
            capacity_attribute,
            scores
        ) != SOC_TRUE &&
        read_cpu_scores(
            cpu_indices,
            allowed_count,
            frequency_attribute,
            scores
        ) != SOC_TRUE) {
        return;
    }
    selected_count = soc_thread_pool_select_performance_cpus(
        scores,
        allowed_count,
        worker_count,
        selected
    );
    if (selected_count < worker_count || selected_count >= allowed_count) {
        return;
    }

    CPU_ZERO(&implementation->performance_cpu_set);
    for (cpu_index = 0u; cpu_index < allowed_count; ++cpu_index) {
        if (selected[cpu_index] == SOC_TRUE) {
            CPU_SET(
                cpu_indices[cpu_index],
                &implementation->performance_cpu_set
            );
        }
    }
    implementation->performance_affinity_enabled = SOC_TRUE;
}

static soc_bool pin_current_thread_to_performance_cpus(
    const soc_thread_pool_implementation* implementation
)
{
    return sched_setaffinity(
        0,
        sizeof(implementation->performance_cpu_set),
        &implementation->performance_cpu_set
    ) == 0
        ? SOC_TRUE
        : SOC_FALSE;
}

#endif

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
    soc_thread_pool_worker* worker = (soc_thread_pool_worker*)user_data;

#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
    if (worker->implementation->performance_affinity_enabled) {
        /* One attempt during worker initialization; never retry in a run. */
        (void)pin_current_thread_to_performance_cpus(
            worker->implementation
        );
    }
#endif

    worker_run(worker);
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
    uint32_t worker_count,
    soc_bool prefer_performance_cpus
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
#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
    if (prefer_performance_cpus == SOC_TRUE) {
        discover_performance_cpu_set(implementation, worker_count);
    }
#else
    (void)prefer_performance_cpus;
#endif
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

#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
    if (implementation->performance_affinity_enabled == SOC_TRUE &&
        sched_getaffinity(
            0,
            sizeof(implementation->caller_original_cpu_set),
            &implementation->caller_original_cpu_set
        ) == 0 &&
        !CPU_EQUAL(
            &implementation->caller_original_cpu_set,
            &implementation->performance_cpu_set
        )) {
        implementation->caller_affinity_changed =
            pin_current_thread_to_performance_cpus(implementation);
    }
#endif

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
#if SOC_THREAD_POOL_USE_ANDROID_AFFINITY
        if (implementation->caller_affinity_changed == SOC_TRUE) {
            (void)sched_setaffinity(
                0,
                sizeof(implementation->caller_original_cpu_set),
                &implementation->caller_original_cpu_set
            );
            implementation->caller_affinity_changed = SOC_FALSE;
        }
#endif
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
