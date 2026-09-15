#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

// Off by default: normal use is a keyboard that works, and what you want to see
// then is the text you typed. Press 'd' when a keyboard misbehaves.
bool pico_test_bt_keyboard_verbose = false;

void dbg(const char *fmt, ...) {
    if (!pico_test_bt_keyboard_verbose) return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

#define DBG_BYTES_MAX 12

void dbg_bytes(const char *label, const uint8_t *data, uint16_t len) {
    if (!pico_test_bt_keyboard_verbose) return;

    printf("%s len=%u", label, len);
    if (data == NULL) {
        printf(" <null>\n");
        fflush(stdout);
        return;
    }

    uint16_t shown = (len > DBG_BYTES_MAX) ? DBG_BYTES_MAX : len;
    for (uint16_t i = 0; i < shown; i++) {
        printf(" %02x", data[i]);
    }
    if (shown < len) printf(" ...");
    printf("\n");
    fflush(stdout);
}
