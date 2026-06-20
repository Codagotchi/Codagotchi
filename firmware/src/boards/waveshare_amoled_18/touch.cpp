#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal touch reader (FocalTech-style register layout, shared by the v2
// CST816 at 0x15 and the v1 FT3168 at 0x38). Avoids vendoring Waveshare's
// GPLv3 Arduino_DriveBus library.
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read_into_shared_state(void) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)5) != 5) { touch_pressed = false; return; }
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();
    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
}

void touch_hal_init(void) {
    // Read the chip-ID register (CST816: 0xA7, reports 0xB5/0xB6). Logged for
    // diagnostics; we don't fail on a mismatch. Talking to a present device
    // (0x15) is also what keeps the new-core I2C driver from wedging — the old
    // FT3168 address (0x38) is absent on v2 boards and a failed write there
    // throws the whole bus into ESP_ERR_INVALID_STATE.
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0xA7);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(TOUCH_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch ID=0x%02X\n", Wire.read());
    } else {
        Serial.println("Touch ID read failed");
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        touch_read_into_shared_state();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
