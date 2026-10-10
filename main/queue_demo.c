/*
 * queue_demo.c - the classic PRODUCER / CONSUMER pattern.
 *
 * Two producer tasks generate fake sensor readings at different rates and
 * push them into one FreeRTOS queue. A single consumer task pulls them out
 * and updates shared statistics.
 *
 * Why a queue instead of a shared variable + flag?
 *   - It is thread-safe out of the box (no extra locking).
 *   - It BUFFERS: short bursts from producers do not lose data as long as
 *     the queue does not fill up.
 *   - It BLOCKS efficiently: a consumer waiting on an empty queue uses zero
 *     CPU; the scheduler wakes it exactly when data arrives.
 *   - It decouples timing: producers and consumer run at their own pace,
 *     even on different CPU cores.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "app.h"

static const char *TAG = "queue";

/*
 * One task function, two task instances. The per-instance configuration is
 * passed through the `pvParameters` pointer of xTaskCreate().
 *
 * IMPORTANT: whatever pvParameters points to must still be alive when the
 * task runs! Passing the address of a local variable of the creating
 * function is a classic bug (the task starts later and reads garbage).
 * `static const` storage is always safe.
 */
typedef struct {
    msg_source_t source;
    uint32_t     period_ms;
    EventBits_t  ready_bit;
} producer_cfg_t;

static const producer_cfg_t s_producer_a_cfg = {
    .source = SRC_PRODUCER_A,
    .period_ms = CONFIG_DEMO_PRODUCER_A_PERIOD_MS,
    .ready_bit = EVT_PRODUCER_A_READY,
};

static const producer_cfg_t s_producer_b_cfg = {
    .source = SRC_PRODUCER_B,
    .period_ms = CONFIG_DEMO_PRODUCER_B_PERIOD_MS,
    .ready_bit = EVT_PRODUCER_B_READY,
};

/* Consumer-private state: the next sequence number we expect per source. */
static uint32_t s_expected_seq[SRC_COUNT];

/* ======================================================================== */
/* Producer                                                                 */
/* ======================================================================== */

/* A fake sensor: a sawtooth with a per-source offset. Deterministic values
 * make it easy to check in the debugger whether what arrived is right. */
static int32_t read_fake_sensor(msg_source_t src, uint32_t seq)
{
    return (int32_t)(src * 1000 + (seq % 100));
}

static void producer_task(void *pvParameters)
{
    const producer_cfg_t *cfg = (const producer_cfg_t *)pvParameters;
    uint32_t seq = 0;

    app_wait_for_all_ready(cfg->ready_bit);
    ESP_LOGI(TAG, "Producer %c started on core %d, period %lu ms",
             'A' + cfg->source, xPortGetCoreID(), (unsigned long)cfg->period_ms);

    /*
     * vTaskDelay() vs xTaskDelayUntil():
     *   vTaskDelay(100) sleeps 100 ticks *from now*, so the time spent doing
     *   work before it adds up and the period DRIFTS.
     *   xTaskDelayUntil() wakes at fixed absolute times: last_wake + period.
     *   Use it for anything periodic (sampling, control loops).
     */
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(cfg->period_ms));

        /* Note: do NOT write `.seq = seq++, .value = f(seq)` in one
         * initializer - the evaluation order of initializers is unspecified
         * in C. Side effects go in their own statement. */
        sensor_msg_t msg = {
            .source = cfg->source,
            .seq = seq,
            .value = read_fake_sensor(cfg->source, seq),
            .timestamp_us = esp_timer_get_time(),
        };
        seq++;  /* increment even if the send fails: consumer sees a gap */

        /*
         * xQueueSend() copies `msg` into the queue. The 3rd argument is how
         * long to BLOCK if the queue is full:
         *   0               - never block, fail immediately
         *   pdMS_TO_TICKS() - wait up to that long
         *   portMAX_DELAY   - wait forever (only if losing data is worse
         *                     than this task stalling!)
         * A sensor sampled periodically should not stall its own timing, so
         * we wait only a little and count a drop on failure.
         */
        if (xQueueSend(g_app.sensor_queue, &msg, pdMS_TO_TICKS(10)) != pdPASS) {
            ESP_LOGW(TAG, "Producer %c: queue full, dropped seq %lu",
                     'A' + cfg->source, (unsigned long)msg.seq);
            if (xSemaphoreTake(g_app.stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_app.stats.dropped[cfg->source]++;
                xSemaphoreGive(g_app.stats_mutex);
            }
        }
    }
    /* A FreeRTOS task function must NEVER return. If a task is done, it has
     * to delete itself with vTaskDelete(NULL). Returning = abort(). */
}

/* ======================================================================== */
/* Consumer                                                                 */
/* ======================================================================== */

/*
 * The "business logic" of the consumer is in its own function on purpose:
 * it is the perfect breakpoint location (`break consumer_process_msg`).
 * noinline stops the compiler from folding it into the task loop, which
 * would make the breakpoint unreliable.
 */
__attribute__((noinline))
static void consumer_process_msg(const sensor_msg_t *msg)
{
    const int64_t latency_us = esp_timer_get_time() - msg->timestamp_us;

    /* Sequence check: gaps mean messages were lost somewhere. */
    uint32_t gap = 0;
    if (msg->seq != s_expected_seq[msg->source]) {
        gap = msg->seq - s_expected_seq[msg->source];
    }
    s_expected_seq[msg->source] = msg->seq + 1;

#if CONFIG_DEMO_DEBUG_EXERCISE_SLOW_CONSUMER
    /* BUG (exercise): heavy processing that takes far longer than the
     * producers' period. The queue fills up, producers start dropping. */
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif

    /*
     * Critical section with a mutex. Keep it SHORT: only touch the shared
     * data inside, never log, never block on other things while holding a
     * lock (that is how deadlocks and priority problems are born).
     *
     * Always use a timeout when taking a lock in real code. A failure to
     * get the mutex within 100 ms in a system like this is a BUG, and an
     * error message is far easier to debug than a silently stuck task.
     */
    if (xSemaphoreTake(g_app.stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_app.stats.received[msg->source]++;
        g_app.stats.seq_gaps[msg->source] += gap;
        g_app.stats.last_value[msg->source] = msg->value;
        if (latency_us > g_app.stats.max_latency_us) {
            g_app.stats.max_latency_us = latency_us;
        }
        xSemaphoreGive(g_app.stats_mutex);
    } else {
        ESP_LOGE(TAG, "Could not lock stats_mutex - possible deadlock?");
    }

    ESP_LOGD(TAG, "rx src=%d seq=%lu val=%ld latency=%lld us",
             msg->source, (unsigned long)msg->seq, (long)msg->value, latency_us);
}

static void consumer_task(void *pvParameters)
{
    (void)pvParameters;
    app_wait_for_all_ready(EVT_CONSUMER_READY);
    ESP_LOGI(TAG, "Consumer started on core %d", xPortGetCoreID());

    sensor_msg_t msg;
    for (;;) {
        /*
         * Block until a message arrives - but not forever. With a timeout,
         * the task can notice "nothing has arrived for 2 s" and report it.
         * That is a cheap health check: if producers die, we see it.
         */
        if (xQueueReceive(g_app.sensor_queue, &msg, pdMS_TO_TICKS(2000)) == pdTRUE) {
            consumer_process_msg(&msg);
        } else {
            ESP_LOGW(TAG, "No data for 2 s - are the producers alive?");
        }
    }
}

/* ======================================================================== */
/* Setup                                                                    */
/* ======================================================================== */

void queue_demo_start(void)
{
    /* Storage for LENGTH items of sizeof(sensor_msg_t) bytes, allocated now. */
    g_app.sensor_queue = xQueueCreate(CONFIG_DEMO_QUEUE_LENGTH, sizeof(sensor_msg_t));
    configASSERT(g_app.sensor_queue);
    vQueueAddToRegistry(g_app.sensor_queue, "sensor_queue");

    /*
     * The ESP32 has two cores: core 0 ("PRO_CPU") and core 1 ("APP_CPU").
     * xTaskCreatePinnedToCore() fixes a task to one core;
     * tskNO_AFFINITY lets the scheduler run it on either.
     *
     * Here: producers on core 0, consumer on core 1. The queue works
     * across cores just the same - FreeRTOS SMP handles the locking.
     * In real projects, Wi-Fi/BT stacks live on core 0 by default, so
     * time-critical application work often goes to core 1.
     *
     * BaseType_t return value: pdPASS or errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY.
     */
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(consumer_task, "consumer", STACK_DEFAULT, NULL,
                                 PRIO_CONSUMER, &g_app.consumer_task, 1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(producer_task, "producer_A", STACK_DEFAULT,
                                 (void *)&s_producer_a_cfg, PRIO_PRODUCER,
                                 &g_app.producer_a_task, 0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(producer_task, "producer_B", STACK_DEFAULT,
                                 (void *)&s_producer_b_cfg, PRIO_PRODUCER,
                                 &g_app.producer_b_task, 0);
    configASSERT(ok == pdPASS);
}
