# gdb/qemu.gdb - connect to QEMU and add a few demo helper commands.
#
# Usage (terminal 2, while terminal 1 runs `idf.py qemu --gdb monitor`):
#     idf.py gdb -x gdb/qemu.gdb
#
# How this fits together: `idf.py gdb` normally runs the generated files in
# build/gdbinit/ (symbols, py_extensions, connect). Passing our own -x file
# REPLACES the "connect" step with this file and keeps the rest - so the
# ROM/bootloader/app symbols and the freertos-gdb extension are still loaded.
#
# Plain `idf.py gdb` works with QEMU too; this file just adds the helpers.

set pagination off
set print pretty on

# QEMU's gdbstub listens on the same port OpenOCD would (3333).
set remotetimeout 10
target remote :3333

# ---------------------------------------------------------------------------
# Helper commands - type `help user-defined` in GDB to list them.
# ---------------------------------------------------------------------------
define app
    print g_app
end
document app
Print the whole application context: every RTOS handle plus the statistics.
end

define stats
    print g_app.stats
    printf "queue fill: %d/%d\n", ((Queue_t *)g_app.sensor_queue)->uxMessagesWaiting, ((Queue_t *)g_app.sensor_queue)->uxLength
end
document stats
Print the producer/consumer statistics and how full the sensor queue is.
end

define rtos
    freertos task
    freertos queue
    freertos semaphore
end
document rtos
Show all FreeRTOS tasks, queues and mutexes/semaphores (needs freertos-gdb).
end

# Break where a consumed message is processed. We break on a LINE inside the
# function, not on the function itself: on Xtensa a breakpoint on the
# function name can stop before the register-window prologue has run, and
# arguments then show garbage (e.g. msg=0x0). Alternatively `break
# consumer_process_msg` followed by one `next`.
define bconsumer
    break queue_demo.c:136
end
document bconsumer
Set a breakpoint inside consumer_process_msg() where `msg` is valid.
end

# The CPU is halted at the reset vector (QEMU was started with -S).
# Run up to app_main so the user starts somewhere familiar.
thbreak app_main
continue
