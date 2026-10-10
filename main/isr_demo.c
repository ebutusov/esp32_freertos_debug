/*
 * isr_demo.c - DEFERRED INTERRUPT HANDLING.
 *
 * A hardware timer (GPTimer) raises an interrupt every 500 ms. The rule
 * for interrupt service routines (ISRs) is: do the bare minimum and get
 * out. While an ISR runs, the tasks on that core (and lower-priority
 * interrupts) cannot run.
 *
 * So the ISR only:
 *   1. records that the event happened (a counter), and
 *   2. notifies a high-priority "handler" task.
 * The handler task then does the real work (here: builds a message and
 * sends it to the same queue the producers use - so the ISR effectively
 * becomes a third producer).
 *
 * ISR rules worth memorising:
 *   - Only call FreeRTOS functions ending in ...FromISR().
 *   - Never block: no mutexes, no vTaskDelay, no waiting on queues.
 *   - No ESP_LOGx / printf (they take locks). If you MUST log, use
 *     ESP_DRAM_LOGx / ESP_EARLY_LOGx, and only while debugging.
 *   - Code and data used by the ISR should be in IRAM/DRAM (IRAM_ATTR,
 *     DRAM_ATTR) if the ISR must keep working while flash cache is
 *     disabled (e.g. during flash writes). See CONFIG_GPTIMER_ISR_CACHE_SAFE.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gptimer.h"
#include "app.h"

static const char *TAG = "isr";

#define TIMER_RESOLUTION_HZ  (1 * 1000 * 1000)  /* 1 MHz -> 1 tick = 1 us */
#define TIMER_PERIOD_US      (500 * 1000)       /* 500 ms                 */

static gptimer_handle_t s_timer;

/*
 * GPTimer alarm callback - runs IN INTERRUPT CONTEXT.
 *
 * The return value tells the driver whether a higher-priority task was
 * woken by this ISR. If so, the driver triggers a context switch right
 * at the end of the interrupt (portYIELD_FROM_ISR), so the handler task
 * runs IMMEDIATELY instead of at the next tick. Forgetting this is a
 * common cause of "my ISR is slow to be processed" (up to 1 tick = 10 ms).
 */
static bool IRAM_ATTR timer_alarm_isr(gptimer_handle_t timer,
                                      const gptimer_alarm_event_data_t *edata,
                                      void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    BaseType_t higher_prio_task_woken = pdFALSE;

    g_app.isr_count++;
    vTaskNotifyGiveFromISR(g_app.isr_handler_task, &higher_prio_task_woken);

    return higher_prio_task_woken == pdTRUE;
}

static void isr_handler_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t seq = 0;

    app_wait_for_all_ready(EVT_ISR_HANDLER_READY);

    /* Start the hardware timer only once everybody is ready. */
    ESP_ERROR_CHECK(gptimer_start(s_timer));
    ESP_LOGI(TAG, "ISR handler started on core %d, timer period %d ms",
             xPortGetCoreID(), TIMER_PERIOD_US / 1000);

    for (;;) {
        /*
         * ulTaskNotifyTake(pdTRUE, ...) returns the notification count
         * BEFORE clearing it. If the ISR fired twice before we got to run,
         * we get 2 - the events were coalesced, not lost. Knowing that
         * matters when the count itself has meaning.
         *
         * Timeout = 2 periods: if the interrupt stops arriving we notice.
         */
        uint32_t events = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2 * TIMER_PERIOD_US / 1000));
        if (events == 0) {
            ESP_LOGW(TAG, "Timer interrupt did not fire for %d ms!", 2 * TIMER_PERIOD_US / 1000);
            continue;
        }
        if (events > 1) {
            ESP_LOGW(TAG, "%lu timer events coalesced - handler too slow?",
                     (unsigned long)events);
        }

        sensor_msg_t msg = {
            .source = SRC_TIMER_ISR,
            .seq = seq,
            .value = (int32_t)g_app.isr_count,
            .timestamp_us = esp_timer_get_time(),
        };
        seq++;

        /* This handler runs at high priority: never block here for long. A
         * timeout of 0 means "if the queue is full, drop and move on". */
        if (xQueueSend(g_app.sensor_queue, &msg, 0) != pdPASS) {
            if (xSemaphoreTake(g_app.stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_app.stats.dropped[SRC_TIMER_ISR]++;
                xSemaphoreGive(g_app.stats_mutex);
            }
        }
    }
}

void isr_demo_start(void)
{
    /*
     * ORDER MATTERS - a real bug I hit while writing this very demo:
     *
     * The first version created the handler task first and the timer
     * afterwards. But a new task can start running BEFORE xTaskCreate()
     * even returns (here: priority 8 on the other core). It set its ready
     * bit, the barrier opened (all other tasks were already waiting) and it
     * called gptimer_start(s_timer) while s_timer was still NULL:
     *   ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)
     *
     * Lesson: set up EVERYTHING a task needs before you create the task.
     * (Safe here because the ISR can only fire after gptimer_start(),
     * which the handler task calls - so its handle exists by then.)
     */

    /* 1. Allocate a timer. ESP-IDF drivers return esp_err_t; ESP_ERROR_CHECK
     *    aborts with a readable message (error name + file:line) on failure.
     *    Good for "this can only fail if the code is wrong" situations. */
    const gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_cfg, &s_timer));

    /* 2. Register the ISR callback. */
    const gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_alarm_isr,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));

    /* 3. Periodic alarm: when the counter hits alarm_count, reload to 0. */
    const gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = TIMER_PERIOD_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &alarm_cfg));

    /* 4. Enable (installs the interrupt) - gptimer_start() comes later,
     *    from the handler task, after the startup barrier. */
    ESP_ERROR_CHECK(gptimer_enable(s_timer));

    /* 5. Only now create the handler task. */
    BaseType_t ok = xTaskCreatePinnedToCore(isr_handler_task, "isr_handler", STACK_DEFAULT,
                                            NULL, PRIO_ISR_HANDLER,
                                            &g_app.isr_handler_task, 1);
    configASSERT(ok == pdPASS);
}
