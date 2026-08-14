#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
#endif

#include "platform/soc_thread_pool.h"

#include <stdatomic.h>
#include <stdint.h>

#if defined(__ANDROID__)
    #include <pthread.h>
    #include <sched.h>
    #include <string.h>
#endif

#define TEST_WORKER_COUNT 4u
#define TEST_RUN_COUNT 128u

typedef struct callback_state {
    atomic_uint calls[TEST_WORKER_COUNT];
    atomic_uint failures;
    uint32_t expected_worker_count;
} callback_state;

static void count_callback(
    void* user_data,
    uint32_t worker_index,
    uint32_t worker_count
)
{
    callback_state* state = (callback_state*)user_data;

    if (worker_count != state->expected_worker_count ||
        worker_index >= worker_count ||
        worker_index >= TEST_WORKER_COUNT) {
        (void)atomic_fetch_add_explicit(
            &state->failures,
            1u,
            memory_order_relaxed
        );
        return;
    }
    (void)atomic_fetch_add_explicit(
        &state->calls[worker_index],
        1u,
        memory_order_relaxed
    );
}

static void reset_state(callback_state* state, uint32_t worker_count)
{
    uint32_t worker_index;

    for (worker_index = 0u;
         worker_index < TEST_WORKER_COUNT;
         ++worker_index) {
        atomic_init(&state->calls[worker_index], 0u);
    }
    atomic_init(&state->failures, 0u);
    state->expected_worker_count = worker_count;
}

static void clear_state(callback_state* state, uint32_t worker_count)
{
    uint32_t worker_index;

    for (worker_index = 0u;
         worker_index < TEST_WORKER_COUNT;
         ++worker_index) {
        atomic_store_explicit(
            &state->calls[worker_index],
            0u,
            memory_order_relaxed
        );
    }
    atomic_store_explicit(
        &state->failures,
        0u,
        memory_order_relaxed
    );
    state->expected_worker_count = worker_count;
}

static int run_pool_test(uint32_t worker_count, uint32_t run_count)
{
    soc_thread_pool thread_pool = {0};
    callback_state state;
    uint32_t run_index;
    uint32_t worker_index;

    reset_state(&state, worker_count);
    if (soc_thread_pool_initialize(
            &thread_pool,
            worker_count,
            SOC_FALSE
        ) !=
            SOC_RESULT_OK ||
        soc_thread_pool_worker_count(&thread_pool) != worker_count) {
        soc_thread_pool_shutdown(&thread_pool);
        return 1;
    }

    for (run_index = 0u; run_index < run_count; ++run_index) {
        if ((run_index & 1u) == 0u) {
            soc_thread_pool_run(&thread_pool, count_callback, &state);
        } else {
            soc_thread_pool_run_active(
                &thread_pool,
                worker_count,
                count_callback,
                &state
            );
        }
    }

    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0u) {
        soc_thread_pool_shutdown(&thread_pool);
        return 1;
    }
    for (worker_index = 0u; worker_index < worker_count; ++worker_index) {
        if (atomic_load_explicit(
                &state.calls[worker_index],
                memory_order_relaxed
            ) != run_count) {
            soc_thread_pool_shutdown(&thread_pool);
            return 1;
        }
    }

    soc_thread_pool_shutdown(&thread_pool);
    if (soc_thread_pool_worker_count(&thread_pool) != 0u) {
        return 1;
    }
    soc_thread_pool_shutdown(&thread_pool);
    return 0;
}

static int run_active_sequence_test(void)
{
    static const uint32_t active_worker_counts[] = {
        2u, 1u, 3u, TEST_WORKER_COUNT,
        1u, TEST_WORKER_COUNT, 2u, TEST_WORKER_COUNT, 3u,
    };
    soc_thread_pool thread_pool = {0};
    callback_state state;
    uint32_t run_index;
    uint32_t sequence_index;
    uint32_t worker_index;

    reset_state(&state, 1u);
    if (soc_thread_pool_initialize(
            &thread_pool,
            TEST_WORKER_COUNT,
            SOC_FALSE
        ) !=
        SOC_RESULT_OK) {
        return 1;
    }

    for (run_index = 0u; run_index < TEST_RUN_COUNT; ++run_index) {
        for (sequence_index = 0u;
             sequence_index < sizeof(active_worker_counts) /
                sizeof(active_worker_counts[0]);
             ++sequence_index) {
            const uint32_t active_worker_count =
                active_worker_counts[sequence_index];

            clear_state(&state, active_worker_count);
            if (active_worker_count == TEST_WORKER_COUNT &&
                (sequence_index & 1u) != 0u) {
                soc_thread_pool_run(
                    &thread_pool,
                    count_callback,
                    &state
                );
            } else {
                soc_thread_pool_run_active(
                    &thread_pool,
                    active_worker_count,
                    count_callback,
                    &state
                );
            }
            if (atomic_load_explicit(
                    &state.failures,
                    memory_order_relaxed
                ) != 0u) {
                soc_thread_pool_shutdown(&thread_pool);
                return 1;
            }
            for (worker_index = 0u;
                 worker_index < TEST_WORKER_COUNT;
                 ++worker_index) {
                const unsigned int expected =
                    worker_index < active_worker_count ? 1u : 0u;

                if (atomic_load_explicit(
                        &state.calls[worker_index],
                        memory_order_relaxed
                    ) != expected) {
                    soc_thread_pool_shutdown(&thread_pool);
                    return 1;
                }
            }
        }
    }

    soc_thread_pool_shutdown(&thread_pool);
    return 0;
}

static int selection_matches(
    const uint64_t* scores,
    uint32_t cpu_count,
    uint32_t worker_count,
    const soc_bool* expected,
    uint32_t expected_count
)
{
    soc_bool selected[8];
    uint32_t selected_count;
    uint32_t cpu_index;

    if (cpu_count > (uint32_t)(sizeof(selected) / sizeof(selected[0]))) {
        return 0;
    }
    selected_count = soc_thread_pool_select_performance_cpus(
        scores,
        cpu_count,
        worker_count,
        selected
    );
    if (selected_count != expected_count) {
        return 0;
    }
    for (cpu_index = 0u; cpu_index < cpu_count; ++cpu_index) {
        if (selected[cpu_index] != expected[cpu_index]) {
            return 0;
        }
    }
    return 1;
}

static int test_performance_cpu_selection(void)
{
    static const soc_bool top_four[] = {
        SOC_TRUE, SOC_TRUE, SOC_TRUE, SOC_TRUE,
        SOC_FALSE, SOC_FALSE, SOC_FALSE, SOC_FALSE,
    };
    static const soc_bool all_eight[] = {
        SOC_TRUE, SOC_TRUE, SOC_TRUE, SOC_TRUE,
        SOC_TRUE, SOC_TRUE, SOC_TRUE, SOC_TRUE,
    };
    static const soc_bool top_two_plus_first_two[] = {
        SOC_TRUE, SOC_TRUE, SOC_TRUE, SOC_TRUE,
        SOC_FALSE, SOC_FALSE, SOC_FALSE, SOC_FALSE,
    };
    static const uint64_t four_plus_four[] = {
        UINT64_C(2301000), UINT64_C(2301000),
        UINT64_C(2301000), UINT64_C(2301000),
        UINT64_C(1800000), UINT64_C(1800000),
        UINT64_C(1800000), UINT64_C(1800000),
    };
    static const uint64_t one_plus_three_plus_four[] = {
        UINT64_C(3000000),
        UINT64_C(2400000), UINT64_C(2400000), UINT64_C(2400000),
        UINT64_C(1800000), UINT64_C(1800000),
        UINT64_C(1800000), UINT64_C(1800000),
    };
    static const uint64_t homogeneous[] = {
        UINT64_C(2000000), UINT64_C(2000000),
        UINT64_C(2000000), UINT64_C(2000000),
        UINT64_C(2000000), UINT64_C(2000000),
        UINT64_C(2000000), UINT64_C(2000000),
    };
    static const uint64_t two_plus_six[] = {
        UINT64_C(2800000), UINT64_C(2800000),
        UINT64_C(1900000), UINT64_C(1900000),
        UINT64_C(1900000), UINT64_C(1900000),
        UINT64_C(1900000), UINT64_C(1900000),
    };

    return
        selection_matches(
            four_plus_four,
            8u,
            4u,
            top_four,
            4u
        ) &&
        selection_matches(
            one_plus_three_plus_four,
            8u,
            4u,
            top_four,
            4u
        ) &&
        selection_matches(
            homogeneous,
            8u,
            4u,
            all_eight,
            8u
        ) &&
        selection_matches(
            two_plus_six,
            8u,
            4u,
            top_two_plus_first_two,
            4u
        ) &&
        selection_matches(
            four_plus_four,
            8u,
            8u,
            all_eight,
            8u
        ) &&
        selection_matches(
            four_plus_four,
            8u,
            9u,
            all_eight,
            8u
        ) ? 0 : 1;
}

#if defined(__ANDROID__)

typedef struct affinity_shutdown_state {
    int succeeded;
} affinity_shutdown_state;

static void* affinity_shutdown_entry(void* user_data)
{
    affinity_shutdown_state* state = (affinity_shutdown_state*)user_data;
    soc_thread_pool thread_pool = {0};
    cpu_set_t affinity_before;
    cpu_set_t affinity_after;

    state->succeeded = 0;
    (void)memset(&affinity_before, 0, sizeof(affinity_before));
    if (sched_getaffinity(0, sizeof(affinity_before), &affinity_before) != 0) {
        return NULL;
    }
    if (soc_thread_pool_initialize(
            &thread_pool,
            TEST_WORKER_COUNT,
            SOC_TRUE
        ) != SOC_RESULT_OK) {
        return NULL;
    }
    soc_thread_pool_shutdown(&thread_pool);

    (void)memset(&affinity_after, 0, sizeof(affinity_after));
    if (sched_getaffinity(0, sizeof(affinity_after), &affinity_after) != 0) {
        return NULL;
    }
    if (memcmp(
            &affinity_before,
            &affinity_after,
            sizeof(affinity_before)
        ) == 0 &&
        soc_thread_pool_worker_count(&thread_pool) == 0u) {
        state->succeeded = 1;
    }
    return NULL;
}

static int test_android_affinity_shutdown_restore(void)
{
    affinity_shutdown_state state = {
        .succeeded = 0,
    };
    pthread_t caller_thread;

    if (pthread_create(
            &caller_thread,
            NULL,
            affinity_shutdown_entry,
            &state
        ) != 0) {
        return 1;
    }
    if (pthread_join(caller_thread, NULL) != 0) {
        return 1;
    }
    return state.succeeded != 0 ? 0 : 1;
}

#endif

int main(void)
{
    soc_thread_pool invalid_pool = {0};

    if (soc_thread_pool_initialize(NULL, 1u, SOC_FALSE) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_thread_pool_initialize(&invalid_pool, 0u, SOC_FALSE) !=
            SOC_RESULT_INVALID_ARGUMENT ||
        soc_thread_pool_worker_count(NULL) != 0u ||
        run_pool_test(1u, TEST_RUN_COUNT) != 0 ||
        run_pool_test(TEST_WORKER_COUNT, TEST_RUN_COUNT) != 0 ||
        run_active_sequence_test() != 0 ||
        test_performance_cpu_selection() != 0) {
        return 1;
    }
#if defined(__ANDROID__)
    if (test_android_affinity_shutdown_restore() != 0) {
        return 1;
    }
#endif

    return 0;
}
