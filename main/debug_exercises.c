/*
 * debug_exercises.c - DELIBERATELY BROKEN CODE for debugging practice.
 *
 * Select one exercise in `idf.py menuconfig`:
 *   FreeRTOS debug demo -> Debug exercise (inject a bug)
 * then rebuild and run in QEMU. README.md explains how to hunt each one.
 *
 * Every bug here is one I have seen in real projects more than once.
 * (The "slow consumer" exercise lives in queue_demo.c, in the consumer.)
 */
#include <string.h>
#include "esp_log.h"
#include "app.h"

static const char *TAG = "exercise";

/* ======================================================================== */
#if CONFIG_DEMO_DEBUG_EXERCISE_NULL_DEREF
/*
 * BUG: a lookup function returns NULL for "not found", and the caller does
 * not check it. On ESP32 (Xtensa) reading address 0x0 raises a
 * "LoadProhibited" exception -> "Guru Meditation Error" panic.
 *
 * What to look at in the panic output:
 *   EXCVADDR = the bad address that was accessed (0x00000004 here: the
 *              offset of `gain` inside the struct - a small EXCVADDR is the
 *              fingerprint of a NULL-pointer-plus-member access)
 *   PC       = the instruction that crashed
 *   Backtrace: the call chain. `idf.py monitor` decodes it to file:line.
 */
typedef struct {
    int     sensor_id;
    int32_t gain;
} calibration_t;

static const calibration_t s_calibration_table[] = {
    {.sensor_id = 1, .gain = 100},
    {.sensor_id = 2, .gain = 250},
};

__attribute__((noinline))
static const calibration_t *calibration_find(int sensor_id)
{
    for (size_t i = 0; i < sizeof(s_calibration_table) / sizeof(s_calibration_table[0]); i++) {
        if (s_calibration_table[i].sensor_id == sensor_id) {
            return &s_calibration_table[i];
        }
    }
    return NULL;   /* not found */
}

__attribute__((noinline))
static int32_t apply_calibration(int sensor_id, int32_t raw)
{
    const calibration_t *cal = calibration_find(sensor_id);
    return raw * cal->gain / 100;      /* <-- BUG: cal may be NULL */
}

static void null_deref_task(void *arg)
{
    (void)arg;
    /* volatile: prevents the compiler from proving at build time that the
     * lookup fails (it could then replace the crash with a trap). */
    static volatile int sensor_ids[] = {1, 2, 3};

    for (int i = 0; ; i = (i + 1) % 3) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        int id = sensor_ids[i];
        ESP_LOGI(TAG, "Calibrating sensor %d ...", id);
        int32_t v = apply_calibration(id, 1234);
        ESP_LOGI(TAG, "sensor %d calibrated value %ld", id, (long)v);
    }
}

static void start_exercise(void)
{
    xTaskCreate(null_deref_task, "calibrator", STACK_DEFAULT, NULL, 3, NULL);
}

/* ======================================================================== */
#elif CONFIG_DEMO_DEBUG_EXERCISE_STACK_OVERFLOW
/*
 * BUG: a recursive parser with a large local buffer in each frame, running
 * in a task whose stack was sized "by feel". Each recursion level eats
 * ~300 bytes of stack. Eventually it runs past the end of the stack.
 *
 * With CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY the kernel checks a
 * known byte pattern at the end of the stack on every context switch,
 * and panics with:  ***ERROR*** A stack overflow in task parser has been detected.
 *
 * Caveat: detection happens at the NEXT context switch, so memory BELOW
 * the stack may already be corrupted. For an exact hit, enable
 * CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK: it puts a hardware watchpoint
 * on the last bytes of the stack and stops on the guilty instruction.
 */
/* Nesting depth of the "document" - imagine it comes from network input.
 * volatile so the compiler cannot reason about it. */
static volatile int s_document_depth = 50;

__attribute__((noinline))
static int parse_nested(int depth)
{
    if (depth >= s_document_depth) {
        return 0;      /* the parser IS bounded... just not by the stack size */
    }

    char token[256];   /* <-- BUG: big buffer per recursion level */
    memset(token, 'a' + (depth % 26), sizeof(token));

    ESP_LOGI(TAG, "parse depth %d, free stack %u bytes", depth,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(200));   /* context switch -> canary check */

    /* "Nested structure" that is deeper than anyone expected. */
    return token[depth % sizeof(token)] + parse_nested(depth + 1);
}

static void parser_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    int r = parse_nested(0);
    ESP_LOGI(TAG, "parse result %d", r);   /* never reached */
    vTaskDelete(NULL);
}

static void start_exercise(void)
{
    xTaskCreate(parser_task, "parser", 2048, NULL, 3, NULL);
}

/* ======================================================================== */
#elif CONFIG_DEMO_DEBUG_EXERCISE_DEADLOCK
/*
 * BUG: LOCK-ORDER INVERSION.
 *   task "transfer_AB" takes lock_A then lock_B
 *   task "transfer_BA" takes lock_B then lock_A
 * If both grab their first lock before either grabs its second, each waits
 * forever for the other. No crash, no message - the tasks just stop.
 * (We use portMAX_DELAY here on purpose, which is why it hangs silently.)
 *
 * The fix: define a global lock ORDER and always take locks in that order.
 *
 * How to find it: the "transfer" log lines never appear, and the
 * run-time counters of both tasks stop growing. In GDB:
 *   freertos semaphore  -> lock_A held by transfer_AB, transfer_BA waiting;
 *                          lock_B held by transfer_BA, transfer_AB waiting.
 *                          That cycle IS the deadlock.
 *   freertos task       -> both tasks "suspended" with MUTEXES_HELD = 1.
 * ("suspended", not "blocked": FreeRTOS parks a task that waits with
 * portMAX_DELAY on the suspended list, because it has no wake-up time.)
 */
static SemaphoreHandle_t s_lock_a, s_lock_b;
static volatile uint32_t s_transfers;

static void transfer_task(void *arg)
{
    const bool a_first = (arg != NULL);
    SemaphoreHandle_t first = a_first ? s_lock_a : s_lock_b;
    SemaphoreHandle_t second = a_first ? s_lock_b : s_lock_a;

    for (;;) {
        xSemaphoreTake(first, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));      /* "work" - widens the race window */
        xSemaphoreTake(second, portMAX_DELAY);

        s_transfers++;
        ESP_LOGI(TAG, "%s: transfer #%lu done", pcTaskGetName(NULL),
                 (unsigned long)s_transfers);

        xSemaphoreGive(second);
        xSemaphoreGive(first);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void start_exercise(void)
{
    s_lock_a = xSemaphoreCreateMutex();
    s_lock_b = xSemaphoreCreateMutex();
    vQueueAddToRegistry(s_lock_a, "lock_A");
    vQueueAddToRegistry(s_lock_b, "lock_B");
    xTaskCreatePinnedToCore(transfer_task, "transfer_AB", STACK_DEFAULT, (void *)1, 3, NULL, 0);
    xTaskCreatePinnedToCore(transfer_task, "transfer_BA", STACK_DEFAULT, NULL, 3, NULL, 1);
}

/* ======================================================================== */
#elif CONFIG_DEMO_DEBUG_EXERCISE_CPU_HOG
/*
 * BUG: a high-priority task "polls" for a condition in a busy loop instead
 * of blocking on a queue/semaphore/notification.
 *
 * FreeRTOS never preempts a running task for a LOWER-priority one. Our hog
 * runs at priority 10 on core 0, so everything below priority 10 on core 0
 * starves - including IDLE0. The Task Watchdog (TWDT) watches the IDLE
 * tasks and after CONFIG_ESP_TASK_WDT_TIMEOUT_S prints:
 *   E (...) task_wdt: Task watchdog got triggered. The following tasks/users
 *   did not reset the watchdog in time:  - IDLE0 (CPU 0)
 * Notice also that producer_A/producer_B (core 0) stop producing.
 *
 * QEMU NOTE: `idf.py qemu` disables the emulated timer-group watchdogs
 * (emulation timing is not real-time), and the TWDT is built on them, so
 * by default you will NOT see the message above. Re-enable them with:
 *   idf.py qemu --qemu-extra-args="-global driver=timer.esp32.timg,property=wdt_disable,value=false" monitor
 *
 * The fix: block instead of polling (or, if you really must poll, yield
 * with vTaskDelay(1) in the loop).
 */
static volatile bool s_data_ready;   /* nobody ever sets this... */

static void hog_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "hog: busy-waiting for data_ready (this starves core 0)");
    uint32_t spins = 0;
    while (!s_data_ready) {
        spins++;      /* <-- BUG: busy wait, never blocks */
    }
    ESP_LOGI(TAG, "spins %lu", (unsigned long)spins);
    vTaskDelete(NULL);
}

static void start_exercise(void)
{
    xTaskCreatePinnedToCore(hog_task, "hog", STACK_DEFAULT, NULL, 10, NULL, 0);
}

/* ======================================================================== */
#else  /* NONE or SLOW_CONSUMER (that one is in queue_demo.c) */
static void start_exercise(void) {}
#endif

void debug_exercises_start(void)
{
#if CONFIG_DEMO_DEBUG_EXERCISE_NONE
    ESP_LOGI(TAG, "No debug exercise selected (see menuconfig)");
#else
    ESP_LOGW(TAG, "*** A debug exercise is enabled - expect misbehaviour! ***");
#endif
    start_exercise();
}
