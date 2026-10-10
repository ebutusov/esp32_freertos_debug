/*
 * reporter.c - three patterns working together:
 *
 *   1. SOFTWARE TIMER  - a periodic FreeRTOS timer fires every N ms.
 *   2. TASK NOTIFICATION - the timer callback does NOT do the work itself;
 *      it just "pokes" the reporter task (deferred work).
 *   3. MUTEX-PROTECTED SNAPSHOT - the reporter copies the shared stats under
 *      the mutex, releases it, and only THEN does the slow printing.
 *
 * Plus some introspection APIs that are gold when debugging on real HW:
 * vTaskList(), vTaskGetRunTimeStats(), uxTaskGetStackHighWaterMark(),
 * heap statistics.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app.h"

static const char *TAG = "report";

/*
 * Software timer callback.
 *
 * Software timers do NOT run in an interrupt. All timer callbacks run one
 * after another in the "Tmr Svc" (timer service / daemon) task. Rules:
 *   - keep callbacks short; a slow callback delays ALL other timers
 *   - never block (no vTaskDelay, no waiting on a queue with a timeout)
 * Hence: notify a task and return.
 */
static void report_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    /*
     * Direct-to-task notification: every task has a built-in 32-bit
     * notification value. xTaskNotifyGive() increments it and unblocks the
     * task if it waits in ulTaskNotifyTake().
     * It is faster and uses less RAM than a binary semaphore, but it only
     * works when there is exactly ONE known receiving task.
     */
    xTaskNotifyGive(g_app.reporter_task);
}

static const char *exercise_name(void)
{
#if CONFIG_DEMO_DEBUG_EXERCISE_NULL_DEREF
    return "NULL dereference";
#elif CONFIG_DEMO_DEBUG_EXERCISE_STACK_OVERFLOW
    return "stack overflow";
#elif CONFIG_DEMO_DEBUG_EXERCISE_DEADLOCK
    return "deadlock";
#elif CONFIG_DEMO_DEBUG_EXERCISE_SLOW_CONSUMER
    return "slow consumer";
#elif CONFIG_DEMO_DEBUG_EXERCISE_CPU_HOG
    return "CPU hog / task watchdog";
#else
    return "none";
#endif
}

static void print_report(void)
{
    /*
     * Take a SNAPSHOT under the lock, then release it immediately.
     * printf() over UART is slow (milliseconds!). Holding the mutex while
     * printing would block the consumer for all that time.
     */
    app_stats_t snap;
    if (xSemaphoreTake(g_app.stats_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Could not lock stats_mutex - possible deadlock?");
        return;
    }
    memcpy(&snap, &g_app.stats, sizeof(snap));
    xSemaphoreGive(g_app.stats_mutex);

    static const char *const src_names[SRC_COUNT] = {"producer_A", "producer_B", "timer_ISR"};

    printf("\n===================== REPORT (exercise: %s) =====================\n",
           exercise_name());
    printf("%-12s %9s %8s %9s %10s\n", "source", "received", "dropped", "seq_gaps", "last_val");
    for (int i = 0; i < SRC_COUNT; i++) {
        printf("%-12s %9lu %8lu %9lu %10ld\n", src_names[i],
               (unsigned long)snap.received[i], (unsigned long)snap.dropped[i],
               (unsigned long)snap.seq_gaps[i], (long)snap.last_value[i]);
    }
    /* uxQueueMessagesWaiting(): how full is the queue right now? A queue
     * that is permanently near-full means the consumer cannot keep up. */
    printf("queue fill: %u/%d   max latency: %lld us   ISR count: %lu\n",
           (unsigned)uxQueueMessagesWaiting(g_app.sensor_queue), CONFIG_DEMO_QUEUE_LENGTH,
           snap.max_latency_us, (unsigned long)g_app.isr_count);

    /*
     * vTaskList(): one line per task.
     * Columns: Name, State (X=running B=blocked R=ready S=suspended
     * D=deleted), Priority, minimum free stack ever (bytes on ESP-IDF),
     * task number, core affinity (-1 = any core).
     *
     * Needs CONFIG_FREERTOS_USE_TRACE_FACILITY and
     * CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS (see sdkconfig.defaults).
     * Roughly 40 bytes per task; it does NOT check the buffer size, so be
     * generous. That is also why this buffer is static, not on the stack.
     */
    static char buf[1024];
    vTaskList(buf);
    printf("\n%-15s\t%s\t%s\t%s\t%s\t%s\n%s",
           "Task", "State", "Prio", "Stack", "Num", "Core", buf);

    /*
     * Run-time stats: absolute time and percentage of CPU each task used
     * since boot. "IDLE0/IDLE1" near 100% = system mostly idle (good).
     * A task you did not expect near the top = your performance problem.
     */
    vTaskGetRunTimeStats(buf);
    printf("\n%-15s\t%s\t\t%s\n%s", "Task", "Abs time", "CPU %", buf);

    /* Heap: current free and the lowest it ever was ("low water mark").
     * A steadily decreasing free heap across reports = memory leak. */
    printf("\nheap free: %u bytes, minimum ever: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    printf("==================================================================\n\n");
}

static void reporter_task(void *pvParameters)
{
    (void)pvParameters;
    app_wait_for_all_ready(EVT_REPORTER_READY);

    /* Start the timer only now, once everyone is running. The last argument
     * is how long to wait if the timer command queue is full. */
    xTimerStart(g_app.report_timer, portMAX_DELAY);

    for (;;) {
        /*
         * Wait for the timer callback to notify us.
         * pdTRUE = clear the notification count to 0 on exit (it behaves
         * like a binary semaphore). pdFALSE would decrement it by one
         * (counting semaphore behaviour).
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        print_report();

        /* Our own stack usage: the minimum free stack EVER, in bytes.
         * If this gets close to 0, increase STACK_REPORTER. */
        ESP_LOGI(TAG, "reporter stack high-water mark: %u bytes free",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

void reporter_start(void)
{
    g_app.report_timer = xTimerCreate("report",
                                      pdMS_TO_TICKS(CONFIG_DEMO_REPORT_PERIOD_MS),
                                      pdTRUE,   /* auto-reload = periodic */
                                      NULL,     /* timer ID, unused here   */
                                      report_timer_cb);
    configASSERT(g_app.report_timer);

    /* tskNO_AFFINITY: let the scheduler pick a core - the reporter is not
     * time critical. */
    BaseType_t ok = xTaskCreatePinnedToCore(reporter_task, "reporter", STACK_REPORTER,
                                            NULL, PRIO_REPORTER, &g_app.reporter_task,
                                            tskNO_AFFINITY);
    configASSERT(ok == pdPASS);
}
