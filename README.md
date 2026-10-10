# FreeRTOS patterns + debugging on QEMU (ESP32, ESP-IDF v6.1)

A small ESP-IDF application that shows the FreeRTOS patterns you will use in
almost every ESP32 project. It runs **entirely in the QEMU emulator**: no board
needed. It also doubles as a **debugging lab**, with deliberately injected
bugs you can switch on in `menuconfig`.

> Read the source in the order listed below. Every file is heavily commented;
> the comments explain *why*, not just *what*.

## What is inside

| File | Pattern | Key APIs |
|---|---|---|
| `main/queue_demo.c` | **Producer / consumer** with a queue, tasks pinned to cores, periodic tasks without drift | `xQueueCreate`, `xQueueSend`, `xQueueReceive`, `xTaskCreatePinnedToCore`, `xTaskDelayUntil` |
| `main/main.c` | **Event group** as a startup barrier; mutex vs. binary semaphore | `xEventGroupSetBits`, `xEventGroupWaitBits`, `xSemaphoreCreateMutex` |
| `main/reporter.c` | **Software timer**, **direct-to-task notification**, **mutex-protected snapshot**, run-time introspection | `xTimerCreate`, `xTaskNotifyGive`, `ulTaskNotifyTake`, `vTaskList`, `vTaskGetRunTimeStats`, `uxTaskGetStackHighWaterMark` |
| `main/isr_demo.c` | **Deferred interrupt handling**: tiny ISR, the real work happens in a task | GPTimer driver, `vTaskNotifyGiveFromISR`, `IRAM_ATTR` |
| `main/debug_exercises.c` | **Injected bugs** for practice | – |
| `main/app.h` | Shared types, priorities, stack sizes | – |
| `main/Kconfig.projbuild` | Project options in `menuconfig` (periods, queue length, exercise) | – |
| `sdkconfig.defaults` | Debug-friendly config (`-Og`, stack canaries, trace facility, halt on panic) | – |
| `gdb/qemu.gdb` | GDB connect script + helper commands (`app`, `stats`, `rtos`, `bconsumer`) | – |
| `.vscode/` | Build/QEMU tasks and a launch config for VS Code (paths for the author's machine) | – |
| `templates/` | Placeholder versions of the machine-specific files, for use on other machines | – |

Data flow:

```
[Producer A] (core 0, 300 ms) ──┐
[Producer B] (core 0, 700 ms) ──┼──> sensor_queue (8 items) ──> [Consumer] (core 1)
[GPTimer ISR] ─notify─> [isr_handler] (core 1, 500 ms) ─┘             │
                                                               mutex  v
[SW timer "report", 5 s] ─notify─> [Reporter] ──────────────> g_app.stats
```

## 1. Build and run in QEMU

Open a PowerShell terminal and activate ESP-IDF (the EIM installer created this
script; the paths are the author's, see
[Setting up on another machine](#setting-up-on-another-machine)):

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\home\code\Projects\esp32\debug_emu

idf.py set-target esp32     # only once; creates sdkconfig from sdkconfig.defaults
idf.py build
idf.py qemu monitor         # build + emulate + serial monitor. Exit: Ctrl+]
```

Every 5 s the reporter prints something like this:

```
===================== REPORT (exercise: none) =====================
source        received  dropped  seq_gaps   last_val
producer_A          16        0         0         15
producer_B           7        0         0       1006
timer_ISR            9        0         0          9
queue fill: 0/8   max latency: 1019 us   ISR count: 10

Task            State  Prio  Stack  Num  Core     <- vTaskList()
consumer        B      5     2368   7    1        <- B=blocked: waiting on the queue, 0% CPU
...
Task            Abs time   CPU %                  <- vTaskGetRunTimeStats()
IDLE0           4995918    98%                    <- idle ≈ 100% = healthy system
...
heap free: 286876 bytes, minimum ever: 282660 bytes
```

How to read it:

- **`dropped` / `seq_gaps` stay 0.** Every message produced is consumed.
- **`queue fill` stays near 0.** The consumer keeps up easily.
- **`Stack` is the minimum free stack ever, in bytes.** Use it to trim stack sizes. Leave a margin, because the worst case may not have happened yet.

## 2. Debug with GDB from the command line

You need two terminals, both with ESP-IDF activated.

```powershell
# Terminal 1: start QEMU halted at the reset vector, waiting for GDB, with the serial monitor
idf.py qemu --gdb monitor

# Terminal 2: attach GDB (QEMU's gdbstub listens on port 3333, same as OpenOCD)
idf.py gdb -x gdb/qemu.gdb      # or plain `idf.py gdb`
```

GDB stops in `app_main`. Then try this session:

```gdb
(gdb) bconsumer                 # breakpoint inside consumer_process_msg()
(gdb) continue
(gdb) p *msg                    # the message that was just received
(gdb) bt                        # consumer_process_msg <- consumer_task <- vPortTaskWrapper
(gdb) stats                     # g_app.stats + queue fill level
(gdb) rtos                      # all tasks, queues and mutexes (freertos-gdb)
(gdb) delete
(gdb) break queue_demo.c:136 if msg->source == SRC_TIMER_ISR   # conditional breakpoint
(gdb) continue
```

Hardware watchpoints stop the CPU at the exact instruction that *writes* a
variable. Nothing is dropped in the healthy app, so try this one with the
**Slow consumer** exercise (section 4):

```gdb
(gdb) watch g_app.stats.dropped[0]
(gdb) continue
Thread 1 hit Hardware watchpoint 1: g_app.stats.dropped[0]
Old value = 0
New value = 1
producer_task (...) at main/queue_demo.c:112      # the writer, caught in the act
```

Useful commands:

| Command | What it shows |
|---|---|
| `freertos task` | Every task: state, priority, base priority (≠ priority means priority inheritance is active), mutexes held, stack used and free |
| `freertos queue` | Queues by **name**, which works because the code calls `vQueueAddToRegistry()` |
| `freertos semaphore` | Mutexes and semaphores: who **holds** each one and who **waits** for it |
| `freertos timer` | Software timers |
| `info threads` | **CPU cores, not tasks** (see the gotchas below) |
| `thread 1` / `thread 2` | Switch to the task currently running on core 0 or core 1 |

## 3. Debug from VS Code

You need the **C/C++** extension (`ms-vscode.cpptools`).

1. Open the project folder in VS Code.
2. Press **F5** and pick the **"QEMU: debug app (cppdbg)"** configuration.
3. The pre-launch task builds the app and starts `idf.py qemu --gdb monitor` in a terminal panel, where the app's serial output appears. VS Code then attaches, stops once in `app_main`, and continues.
4. Use breakpoints, Watch, Call Stack and so on as usual. For FreeRTOS commands, type `-exec freertos task` in the **Debug Console**.

**IntelliSense (no red squiggles under `#include "esp_log.h"`):**
`.vscode/c_cpp_properties.json` points the C/C++ extension at
`build/compile_commands.json`, which `idf.py build` generates with every
include path and define. Build once after cloning, before opening the sources.
If the squiggles stay, run **C/C++: Reset IntelliSense Database** from the
Command Palette.

The paths in `.vscode/*.json` and `gdb/vscode_setup.gdb` are specific to the
author's machine. On another machine, see
[Setting up on another machine](#setting-up-on-another-machine).
`gdb/vscode_setup.gdb` adds the ESP-IDF Python venv to GDB's `sys.path`.
Without it, `freertos-gdb` cannot be imported when VS Code starts GDB outside
an activated shell.

The official ESP-IDF VS Code extension also has its own QEMU debug commands.
The config here deliberately uses plain `cppdbg`, so you can see every moving part.

## 4. Debugging exercises

Pick one in `idf.py menuconfig` → **FreeRTOS debug demo → Debug exercise**,
then run `idf.py qemu monitor` (or the GDB setup above). Try to diagnose
each bug *before* reading the comments in `main/debug_exercises.c`.

| Exercise | Symptom you will see | How to diagnose it |
|---|---|---|
| **NULL dereference** | `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`, `EXCVADDR: 0x00000004`, CPU halted | `idf.py monitor` decodes the `Backtrace:` line to file:line. Manual decoding: `xtensa-esp32-elf-addr2line -pfiaC -e build/freertos_debug_demo.elf <addresses>`. A **small EXCVADDR** (like `0x4`) means "NULL pointer + struct member offset". |
| **Stack overflow** | `free stack` counts down 1572 → 164 bytes, then `***ERROR*** A stack overflow in task parser has been detected.` and a backtrace ending in `\|<-CORRUPTED` | The canary is checked only at a context switch, so the stack may already be corrupted when it fires, hence the broken backtrace. For an exact hit, enable `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK`. |
| **Deadlock** | No crash. The `transfer` log lines never appear, and the run-time counters of `transfer_AB`/`transfer_BA` freeze. | Run `freertos semaphore`: `lock_A` is held by `transfer_AB` and awaited by `transfer_BA`, and `lock_B` is the other way round. That cycle is the deadlock. |
| **Slow consumer** | `queue full, dropped seq N` warnings, `queue fill: 8/8`, latency of seconds | Watch `stats` over time, or put a watchpoint on `g_app.stats.dropped[0]`. |
| **CPU hog** | The IDLE0 CPU% keeps falling, the producers on core 0 stop, and the hog's own counter is frozen | In QEMU you must re-enable the watchdog first (see the gotchas). Then the Task WDT reports `IDLE0 (CPU 0)` and `CPU 0: hog`. |

Switch back to **None** when you are done.

## 5. Gotchas found while building this demo

Each of these happened for real during development and was verified in QEMU.
None of them is QEMU-specific unless noted.

1. **A new task may run before `xTaskCreate()` returns.** In the first
   version, `isr_demo_start()` created the handler task *before* the GPTimer.
   That task has priority 8 on the other core, so it ran immediately, passed the
   startup barrier and called `gptimer_start(NULL)`:
   `ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)`.
   **Rule: set up everything a task needs *before* creating the task.**

2. **`ESP_LOGx` lines can silently disappear.** The logger looks up the
   tag's level under a mutex with a **10 ms timeout**
   (`components/log/src/os/log_lock.c`). When several tasks log at the same
   moment, as in a startup burst over a slow UART, a log call that cannot get
   the lock **drops the whole line**. Two runs of this demo lost different
   startup lines. Don't treat "the log line is missing" as proof that the code
   didn't run; use a breakpoint or a counter instead.

3. **Breaking on a function name can show garbage arguments (Xtensa).**
   `break consumer_process_msg` stopped *before* the register-window
   prologue, and `msg` showed as `0x0`. Break on a line inside the function
   (`bconsumer`), or do one `next` first.

4. **`info threads` shows CPU cores, not FreeRTOS tasks (QEMU).** QEMU's
   gdbstub knows nothing about FreeRTOS. Use `freertos task` instead. To get
   a backtrace of a task that is *not* currently running, you need OpenOCD
   with RTOS awareness on real hardware. The other option is a core dump: enable
   `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` (or `..._TO_UART`) and run
   `idf.py coredump-info` after a crash.

5. **No Task Watchdog in QEMU by default.** `idf.py qemu` passes
   `-global driver=timer.esp32.timg,property=wdt_disable,value=true`
   (emulation is not real-time), and the TWDT runs on top of that
   hardware. Re-enable it with:
   ```powershell
   idf.py qemu --qemu-extra-args="-global driver=timer.esp32.timg,property=wdt_disable,value=false" monitor
   ```

6. **Tasks waiting with `portMAX_DELAY` show as "suspended", not "blocked".**
   FreeRTOS puts them on the suspended list because they have no wake-up time.

7. **GCC can catch some bugs at compile time.** The first stack-overflow
   exercise had unbounded recursion, and the ESP-IDF build (`-Werror`) rejected
   it with `-Winfinite-recursion`. Read your warnings!

8. **Odd symbols next to heap addresses** in `freertos task` (such as
   `0x3ffb9494 <lb_env+112>`) are harmless. Symbols from the ROM ELF overlap
   RAM that the app's heap reuses.

## Setting up on another machine

The C sources, CMake files, `sdkconfig.defaults` and `gdb/qemu.gdb` are
portable. Four files contain paths from the author's Windows machine
(ESP-IDF v6.1 installed with EIM under `C:\Espressif`, project in
`D:\home\code\Projects\esp32\debug_emu`):

| File | Machine-specific content |
|---|---|
| `.vscode/tasks.json` | Path to the ESP-IDF activation script; uses PowerShell as the shell |
| `.vscode/launch.json` | Path to `xtensa-esp32-elf-gdb.exe` |
| `gdb/vscode_setup.gdb` | Fallback path to the ESP-IDF Python venv (used when `IDF_PYTHON_ENV_PATH` is not set) |
| `README.md` | The activation and `cd` commands in the examples |

If you only use the command line (sections 1, 2 and 4), you need **none**
of these files. Activate ESP-IDF your usual way, `cd` into the clone, and
run the `idf.py` commands.

For VS Code, `templates/` holds copies of the three config files with
placeholders. Copy them over the originals:

```powershell
# PowerShell (from the project root)
Copy-Item -Recurse -Force templates\.vscode, templates\gdb .
```
```bash
# bash/zsh (from the project root)
cp -r templates/.vscode templates/gdb .
```

Then replace the placeholders. Run these commands **in a shell where ESP-IDF
is activated** to find the right values:

| Placeholder | File | What to put there | How to find it |
|---|---|---|---|
| `<IDF_ACTIVATE>` (3×) | `.vscode/tasks.json` | The command that activates ESP-IDF | Windows/EIM: `. C:/Espressif/tools/Microsoft.v<ver>.PowerShell_profile.ps1`. Linux/macOS: `. <path-to-esp-idf>/export.sh` (or the activation script EIM created) |
| `<XTENSA_GDB>` | `.vscode/launch.json` | Full path to the Xtensa GDB | PowerShell: `(Get-Command xtensa-esp32-elf-gdb).Source`. bash: `which xtensa-esp32-elf-gdb` |
| `<IDF_PYTHON_ENV>` | `gdb/vscode_setup.gdb` | The ESP-IDF Python venv, **with forward slashes** | PowerShell: `$env:IDF_PYTHON_ENV_PATH`. bash: `echo $IDF_PYTHON_ENV_PATH` |

Further notes:

- **Linux/macOS:** in `.vscode/tasks.json`, also replace the `options.shell`
  block with `"shell": { "executable": "/bin/bash", "args": ["-c"] }`. The
  template's `vscode_setup.gdb` already handles both venv layouts
  (`Lib/site-packages` on Windows, `lib/pythonX.Y/site-packages` elsewhere).
- **Required tools:** ESP-IDF v6.1 (other v5.x/v6.x versions will probably work
  but are untested), plus the `qemu-xtensa` tool. If `idf.py qemu` says QEMU is
  not installed, run `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`
  in an activated shell.
- **`sdkconfig` is committed.** If `idf.py` complains after a clone, delete
  `sdkconfig` and run `idf.py set-target esp32`. It is regenerated from
  `sdkconfig.defaults`.
- **Your local changes:** to keep your edited `.vscode/` files out of
  `git status`, run `git update-index --skip-worktree .vscode/tasks.json
  .vscode/launch.json gdb/vscode_setup.gdb`.

## QEMU limitations to keep in mind

- **Not emulated:** Wi-Fi, Bluetooth, most analog peripherals and GPIO interrupts from the outside world. UART, timers, flash, efuse and (open_eth) Ethernet work.
- **Timing is not cycle-accurate.** Don't use QEMU to measure performance or to tune timeouts.
- **Halting is global.** A breakpoint halts **both** emulated CPUs, so time stands still for every task while you look around. Remember that the outside world (sensors, network peers) does *not* stop on real hardware.

## Config cheat sheet (`sdkconfig.defaults`)

| Option | Why |
|---|---|
| `CONFIG_COMPILER_OPTIMIZATION_DEBUG` | `-Og`: variables stay visible and stepping follows the source |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY`, `..._STATS_FORMATTING_FUNCTIONS`, `..._GENERATE_RUN_TIME_STATS` | `vTaskList()`, `vTaskGetRunTimeStats()` |
| `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` | Stack overflow detection |
| `CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE` | Queues and mutexes appear by name in the debugger |
| `CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT` | Halt on panic instead of rebooting, so the crime scene stays intact for GDB |

After editing `sdkconfig.defaults`, delete `sdkconfig` (or run
`idf.py fullclean`) so the new defaults are picked up. Changes made in
`menuconfig` go into `sdkconfig`. Copy the ones you want to keep into
`sdkconfig.defaults`.

## License

[MIT](LICENSE)
