/*
 * Adafruit Gamepad QT - an Adafruit seesaw device on I2C.
 *
 * Two entry points, both called from the input backend on core 0. Nothing here
 * touches SDL; turning a reading into an SDL event is psdl_gamecontroller.c's job.
 */
#ifndef PSDL_SEESAW_GAMEPAD_H
#define PSDL_SEESAW_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

/* One poll's worth of the pad. Axes are the raw 10-bit ADC readings, 0..1023,
 * centred near 512; buttons are true when pressed, the active-low already undone. */
typedef struct {
    uint16_t x, y;
    bool a, b, x_btn, y_btn, select, start;
} seesaw_gamepad_state_t;

/* Bring up the bus and look for the pad. True if one answered and identified
 * itself; false leaves the rest of the system to carry on without it. */
bool seesaw_gamepad_init(void);

/* True once init has succeeded. */
bool seesaw_gamepad_present(void);

/* Read the pad. False if a transfer failed, in which case `out` is untouched. */
bool seesaw_gamepad_read(seesaw_gamepad_state_t *out);

#endif /* PSDL_SEESAW_GAMEPAD_H */
