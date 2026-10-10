/*
 * main.c - entry point of the FreeRTOS debug demo.
 *
 * What this application demonstrates (one file per pattern):
 *
 *   queue_demo.c       Producer / consumer through a FreeRTOS queue
 *   reporter.c         Mutex-protected shared data, software timer,
 *                      direct-to-task notification, task introspection
 *   isr_demo.c         Hardware timer ISR -> "deferred interrupt handling"
 *                      task (the ISR stays tiny, a task does the work)
 *   main.c (this file) Event group used as a startup barrier
 *   debug_exercises.c  Deliberately injected bugs (menuconfig) for practice
 *
 * Data flow:
 *
 *   [Producer A]──┐
 *   [Producer B]──┼──> sensor_queue ──> [Consumer] ──(mutex)──> g_app.stats
 *   [GPTimer ISR]─┘                                                 │
 *     (via notify → ISR handler task)                               │
 *                                                                   v
 *   [SW timer "report"] ──notify──> [Reporter] ──(mutex)── reads stats,
 *                                               prints task list
 */
#include <stdio.h>
#include "esp_log.h"
#include "app.h"

static const char *TAG = "main";

/*
 * The one and only global application context. Defined here, declared
 * `extern` in app.h. Zero-initialised because it has static storage.
 */
app_t g_app;

/*
 * Startup barrier.
 *
 * Why bother? Tasks start running the moment xTaskCreate() returns (if
 * their priority is higher than the creator's, even *before* it returns!).
 * Without a barrier, a producer could send to a queue whose consumer is not
 * set up yet, or the ISR could fire before its handler task exists.
 *
 * Pattern: each task sets its own bit, then waits until ALL bits are set.
 * We pass xClearOnExit = pdFALSE so the bits stay set: a task that arrives
 * "late" still passes straight through.
 *
 * (FreeRTOS also has xEventGroupSync() for exactly this rendezvous, but it
 * CLEARS the bits once everyone has arrived - any task that calls it later
 * would wait forever. Set+Wait is the more forgiving variant.)
 */
void app_wait_for_all_ready(EventBits_t my_bit)
{
    xEventGroupSetBits(g_app.startup_events, my_bit);
    xEventGroupWaitBits(g_app.startup_events,
                        EVT_ALL_READY,
                        pdFALSE,          /* xClearOnExit: keep bits set   */
                        pdTRUE,           /* xWaitForAllBits: AND, not OR  */
                        portMAX_DELAY);
}

/*
 * app_main() is NOT the first code that runs. By the time we get here the
 * 2nd-stage bootloader has loaded the app, both CPUs are up, the FreeRTOS
 * scheduler is running, and app_main() is being called from the "main"
 * task (priority 1, stack CONFIG_ESP_MAIN_TASK_STACK_SIZE).
 *
 * It is fine - and common - for app_main() to return. The main task is
 * then deleted and its stack is freed; your other tasks keep running.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "FreeRTOS debug demo starting on core %d", xPortGetCoreID());

    /*
     * Create shared objects BEFORE any task that uses them.
     * Every xxxCreate() allocates from the heap and can return NULL.
     * configASSERT() halts with file:line if that happens - better than a
     * mysterious crash later. (Production code may prefer graceful error
     * handling or the xxxCreateStatic() variants that never fail at run time.)
     */
    g_app.startup_events = xEventGroupCreate();
    configASSERT(g_app.startup_events);

    /*
     * MUTEX vs BINARY SEMAPHORE - a classic interview question:
     *   - A mutex has an OWNER. Only the task that took it may give it back,
     *     and FreeRTOS applies PRIORITY INHERITANCE: if a high-priority task
     *     waits for a mutex held by a low-priority task, the holder is
     *     temporarily boosted so it can finish and release it quickly.
     *     -> Use a mutex to protect a shared RESOURCE.
     *   - A binary semaphore has no owner and no priority inheritance.
     *     -> Use it (or better, a task notification) for SIGNALLING events.
     * Mutexes must never be used from an ISR.
     */
    g_app.stats_mutex = xSemaphoreCreateMutex();
    configASSERT(g_app.stats_mutex);
    /* Name it so the debugger can show it (see CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE). */
    vQueueAddToRegistry(g_app.stats_mutex, "stats_mutex");

    /* Order matters a bit: consumers before producers is a good habit,
     * even though the barrier would save us either way. */
    queue_demo_start();      /* queue + consumer + 2 producers  */
    reporter_start();        /* reporter task + software timer  */
    isr_demo_start();        /* GPTimer ISR + handler task      */
    debug_exercises_start(); /* no-op unless selected in menuconfig */

    /*
     * The main task waits for the barrier too, but WITH A TIMEOUT. If some
     * task never reports ready (crashed, stuck, forgot to set its bit),
     * we find out immediately instead of staring at a silent console.
     */
    EventBits_t bits = xEventGroupWaitBits(g_app.startup_events, EVT_ALL_READY,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    if ((bits & EVT_ALL_READY) != EVT_ALL_READY) {
        ESP_LOGE(TAG, "Startup barrier timed out! Missing bits: 0x%02lx",
                 (unsigned long)(EVT_ALL_READY & ~bits));
    } else {
        ESP_LOGI(TAG, "All tasks ready - app_main() returns, main task is deleted");
    }
}
