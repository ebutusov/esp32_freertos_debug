/*
 * app.h - shared types and handles for the FreeRTOS debug demo.
 *
 * Design note for juniors:
 * All RTOS objects (queues, mutexes, event groups...) live in ONE global
 * struct, `g_app`. In a production code base you would usually hide them
 * behind module APIs, but for a *debugging* demo this has a big advantage:
 * in GDB you can type `p g_app` and see every handle at once.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

/* ------------------------------------------------------------------------ */
/* Task configuration                                                       */
/* ------------------------------------------------------------------------ */
/*
 * Priorities: higher number = more urgent. The ESP-IDF timer service task
 * runs at priority 1 by default and IDLE at 0. Keep application tasks
 * well below configMAX_PRIORITIES (25) - Wi-Fi/BT tasks use ~18-23 on real
 * projects, and you do not want to starve them.
 *
 * Rule of thumb: the task that must react FASTEST gets the highest
 * priority (here: the deferred ISR handler), and the "bulk" work (reporting)
 * gets the lowest.
 */
#define PRIO_REPORTER        2
#define PRIO_PRODUCER        4
#define PRIO_CONSUMER        5   /* above producers: drains the queue promptly */
#define PRIO_ISR_HANDLER     8

/*
 * Stack sizes are in BYTES on ESP-IDF (vanilla FreeRTOS uses WORDS - a
 * classic porting bug!). Start generous, then trim using
 * uxTaskGetStackHighWaterMark() - the reporter task prints it for you.
 * Anything that calls printf/ESP_LOG with formatting needs ~2-3 KB.
 */
#define STACK_DEFAULT        3072
#define STACK_REPORTER       4096   /* vTaskList() + big printf */

/* ------------------------------------------------------------------------ */
/* Message passed through the queue (producer -> consumer)                  */
/* ------------------------------------------------------------------------ */
typedef enum {
    SRC_PRODUCER_A = 0,
    SRC_PRODUCER_B,
    SRC_TIMER_ISR,      /* produced by the deferred interrupt handler task */
    SRC_COUNT
} msg_source_t;

/*
 * FreeRTOS queues COPY the item into the queue storage (by value).
 * That means:
 *   + no lifetime problems: the sender can reuse its local variable at once
 *   - big items cost RAM (length * item_size) and copy time
 * For large payloads, send a pointer instead and define clear ownership
 * ("whoever receives the pointer must free it").
 */
typedef struct {
    msg_source_t source;
    uint32_t     seq;          /* per-source sequence number - gaps = drops */
    int32_t      value;        /* fake "sensor reading" */
    int64_t      timestamp_us; /* when it was produced (esp_timer_get_time) */
} sensor_msg_t;

/* ------------------------------------------------------------------------ */
/* Statistics shared between consumer (writer) and reporter (reader)        */
/* Protected by g_app.stats_mutex.                                          */
/* ------------------------------------------------------------------------ */
typedef struct {
    uint32_t received[SRC_COUNT];
    uint32_t dropped[SRC_COUNT];   /* producer could not enqueue in time   */
    uint32_t seq_gaps[SRC_COUNT];  /* consumer noticed missing seq numbers */
    int32_t  last_value[SRC_COUNT];
    int64_t  max_latency_us;       /* produce -> consume latency           */
} app_stats_t;

/* ------------------------------------------------------------------------ */
/* Event group bits                                                         */
/* ------------------------------------------------------------------------ */
/*
 * An event group is a set of up to 24 flag bits (on ESP32) that many tasks
 * can wait on. We use it as a startup BARRIER: every worker task signals
 * "I am initialised" and nobody starts real work until ALL bits are set.
 */
#define EVT_CONSUMER_READY    BIT0
#define EVT_PRODUCER_A_READY  BIT1
#define EVT_PRODUCER_B_READY  BIT2
#define EVT_ISR_HANDLER_READY BIT3
#define EVT_REPORTER_READY    BIT4
#define EVT_ALL_READY         (EVT_CONSUMER_READY | EVT_PRODUCER_A_READY | \
                               EVT_PRODUCER_B_READY | EVT_ISR_HANDLER_READY | \
                               EVT_REPORTER_READY)

/* ------------------------------------------------------------------------ */
/* All shared RTOS objects                                                  */
/* ------------------------------------------------------------------------ */
typedef struct {
    QueueHandle_t      sensor_queue;
    SemaphoreHandle_t  stats_mutex;
    EventGroupHandle_t startup_events;
    TimerHandle_t      report_timer;

    TaskHandle_t       consumer_task;
    TaskHandle_t       producer_a_task;
    TaskHandle_t       producer_b_task;
    TaskHandle_t       isr_handler_task;
    TaskHandle_t       reporter_task;

    app_stats_t        stats;          /* guarded by stats_mutex */
    volatile uint32_t  isr_count;      /* written only by the ISR */
} app_t;

extern app_t g_app;

/* Module entry points - each one creates its RTOS objects and tasks. */
void queue_demo_start(void);      /* queue_demo.c   - producers + consumer   */
void reporter_start(void);        /* reporter.c     - reporter + sw timer    */
void isr_demo_start(void);        /* isr_demo.c     - gptimer ISR -> task    */
void debug_exercises_start(void); /* debug_exercises.c - injected bugs       */

/* Helper used by every worker task: signal "ready" and wait for everybody. */
void app_wait_for_all_ready(EventBits_t my_bit);
