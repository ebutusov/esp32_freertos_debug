# The "main" component. In ESP-IDF *everything* is a component, including
# your application code. List the sources and the components you depend on.
#
# Tip: declare dependencies explicitly with REQUIRES / PRIV_REQUIRES. With
# MINIMAL_BUILD ON (top-level CMakeLists.txt) a missing entry here shows up as
# "fatal error: driver/gptimer.h: No such file or directory".
idf_component_register(
    SRCS
        "main.c"
        "queue_demo.c"
        "reporter.c"
        "isr_demo.c"
        "debug_exercises.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gptimer   # general-purpose hardware timer (our "interrupt source")
        esp_timer            # esp_timer_get_time() for timestamps
)
