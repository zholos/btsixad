#include "sixaxis.h"

#include <dev/usb/usbhid.h>


// Provide a (simplified) descriptor for the Sixaxis to avoid parsing
// unauthenticated and potentially corrupt data received over the air.

// The semantics are altered somewhat.

// All buttons on the controller have force sensors, but only the R2 and L2
// triggers are mapped as sliders instead of buttons. There are also three
// unmapped tilt axes (Y and two X). Unfortunately, SDL doesn't understand
// multiple axes with the same usage, so the usage assignments are semi-random.

// Buttons are reshuffled programmatically so they are more useful wihtout
// additional configuration. All the multibyte values in the report are
// big-endian while USB requires little-endian, so the button order is
// effectively opposite to that intended. Renumbering in the descriptor is not
// enough because SDL ignores the numbering. The reshuffled values are placed in
// padding so reading the report assuming the original descriptor still works.

// Although it is reported as separate buttons, the D-pad can't physically be
// pressed in opposite directions, so it is programmatically converted to a hat.

static unsigned char descr[] = {
    0x05, 0x01,       // Usage Page - Generic Desktop
    0x09, 0x05,       // Usage - Gamepad
    0xa1, 0x01,       // Collection - Application
    0x85, 0x01,       //     Report ID - 1

    0x14,             //     Logical Minimum - 0
    0x25, 0x01,       //     Logical Maximum - 1
    0x75, 0x01,       //     Report Size - 1
    0x95, 0x14,       //     Report Count - 20
    0x81, 0x01,       //     Input (Const, Array, Absolute) [padding]
                      //     - 8 bits original padding
                      //     - 12 shuffled away buttons
    0x05, 0x09,       //     Usage Page - Button
    0x19, 0x01,       //     Usage Mimumum - Button 1
    0x29, 0x04,       //     Usage Maximum - Button 4
    0x95, 0x04,       //     Report Count - 4
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
                      //     - X, O, Square, Triangle reshuffled in place
    0x81, 0x01,       //     Input (Const, Array, Absolute) [padding]
                      //     - 3 shuffled away buttons (1 soldered) and padding
    0x19, 0x05,       //     Usage Mimumum - Button 5
    0x29, 0x0b,       //     Usage Maximum - Button 11
    0x95, 0x07,       //     Report Count - 7
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
                      //     - reshuffled buttons
    0x95, 0x01,       //     Report Count - 1
    0x81, 0x01,       //     Input (Const, Array, Absolute) [padding]

    0x05, 0x01,       //     Usage Page - Generic Desktop
    0x09, 0x39,       //     Usage - Hat switch
    0x14,             //     Logical Minimum - 0
    0x25, 0x07,       //     Logical Maximum - 7
    0x34,             //     Physical Minimum - 0
    0x46, 0x3b, 0x01, //     Physical Maximum - 315
    0x65, 0x14,       //     Unit - Degrees
    0x75, 0x04,       //     Report Size - 4
    0x81, 0x42,       //     Input (Data, Variable, Absolute, Null State)
                      //     - converted D-pad
    0x64,             //     Unit - None

    0x09, 0x01,       //     Usage - Pointer
    0xa1, 0x00,       //     Collection - Physical
    0x09, 0x30,       //         Usage - X
    0x09, 0x31,       //         Usage - Y
    0x26, 0xff, 0x00, //         Logical Maximum - 255
    0x35, 0x80,       //         Physical Minimum - -128
    0x45, 0x7f,       //         Physical Maximum - 127
    0x75, 0x08,       //         Report Size - 8
    0x95, 0x02,       //         Report Count - 2
    0x81, 0x02,       //         Input (Data, Variable, Absolute)
    0xc0,             //     End Collection
    0x09, 0x01,       //     Usage - Pointer
    0xa1, 0x00,       //     Collection - Physical
    0x09, 0x33,       //         Usage - Rx [not X]
    0x09, 0x34,       //         Usage - Ry [not Y]
    0x81, 0x02,       //         Input (Data, Variable, Absolute)
    0xc0,             //     End Collection

    0x95, 0x08,       //     Report Count - 8
    0x81, 0x01,       //     Input (Const, Array, Absolute) [padding]
    0x09, 0x38,       //     Usage - Wheel [not second Slider]
    0x09, 0x36,       //     Usage - Slider
    0x34,             //     Physical Minimum - 0
    0x46, 0xff, 0x00, //     Physical Maximum - 255
    0x95, 0x02,       //     Report Count - 2
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
                      //     - L2, R2
    0x44,             //     Physical Maximum - 0
    0x95, 0x1d,       //     Report Count - 29
    0x81, 0x01,       //     Input (Const, Array, Absolute) [padding]
    0x75, 0x08,       //     Report Size - 8
    0x95, 0x30,       //     Report Count - 48
    0x91, 0x02,       //     Output (Data, Variable, Absolute)
    0xb1, 0x02,       //     Feature (Data, Variable, Absolute)
    0xc0              // End Collection
};

struct descr sixaxis_descr = { descr, sizeof descr, 1 };


void
sixaxis_operational(struct device* d, int operational)
{
    // magic
    unsigned char report[] =
        { 0xf4, 0x42, operational < 0 ? 8 : operational ? 3 : 1, 0, 0 };
    device_set_report(d, UHID_FEATURE_REPORT, report, sizeof report);
}


void
sixaxis_leds(struct device* d, int bitmap, int blink)
{
    // Each LED timer is controlled by 5 bytes:
    // - duration in 20 ms increments (0 = off, 0xff = forever)
    // - 2-byte big-endian tick length in 1 us increments
    // - off time in ticks
    // - on time in ticks

    unsigned char report[36] = { 0x01 };
    report[10] = bitmap << 1;
    if (blink)
        // sync all timers by switching them off
        device_set_report(d, UHID_OUTPUT_REPORT, report, sizeof report);
    for (int i = 0; i < 4; i++)
        if (bitmap & 1 << i) {
            unsigned char* timer = report+(26-5*i);
            timer[0] = 0xff;
            if (blink) {
                timer[1] = 0x27; // 10 ms
                timer[2] = 0x10;
                timer[3] = 99; // 990 ms off
                timer[4] = 1; // 10 ms on
            } else
                timer[4] = timer[1] = 0x80; // continuously on
        }
    device_set_report(d, UHID_OUTPUT_REPORT, report, sizeof report);
}


static char hat[] = { 15, 0, 2, 1, 4, 15, 3, 15, 6, 7, 15, 15, 5, 15, 15, 15 };

void
sixaxis_fixup(struct device* d, int kind, unsigned char* data, size_t size)
{
    if (kind == UHID_INPUT_REPORT && size == 49 && data[0] == 1) {
        data[3] = data[3] & 0xf |       // lower nibble
                  data[3] >> 3 & 0x10 | // Square
                  data[3] >> 1 & 0x20 | // X
                  data[3] << 1 & 0x40 | // O
                  data[3] << 3 & 0x80;  // Triangle
        data[4] = data[4] & 0xf |       // lower nibble
                  data[3] << 1 & 0x10 | // R1
                  data[3] << 3 & 0x20 | // L1
                  data[2] << 4 & 0x40 | // R3
                  data[2] << 6 & 0x80;  // L3
        data[5] = data[2] >> 3 & 1 |            // Start
                  data[2] << 1 & 2 |            // Select
                  data[4] << 2 & 4 |            // PS
                  hat[data[2] >> 4 & 0xf] << 4; // D-pad
    }
}
