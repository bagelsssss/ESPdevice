// ESPdevice - 3-key keypad firmware for ESP32 (WROOM-32 / CP2102 USB-serial)
// ------------------------------------------------------------------------
// Sends one byte per key event over USB serial at 921600 baud. No WiFi, no
// Bluetooth, no LED code: just debounce + report, so the loop stays fast.
//
// Protocol (what ESPdevice.exe expects):
//   key N press   : byte (N*2 - 1)   -> key1 = 0x01, key2 = 0x03, key3 = 0x05
//   key N release : byte (N*2)       -> key1 = 0x02, key2 = 0x04, key3 = 0x06
//   held key      : the press byte is repeated every KEEPALIVE_MS, so the PC
//                   can tell "still held" from "board unplugged / went quiet".
//   identity query: the PC sends 0xFE, the board replies "3KEY\n". This is how
//                   the PC finds the right COM port without a fixed port number.
//
// Wiring: each key connects its GPIO to GND (internal pull-up is used, so no
// resistors needed).
//
// KEY ORDER: the order of the numbers in KEY_PINS decides which key is "key 1,
// 2, 3" on the PC. If your keys show up in the wrong order in the ESPdevice
// visualizer, swap the numbers here (not the wiring).

const uint8_t NUM_KEYS = 3;
const uint8_t KEY_PINS[NUM_KEYS] = {4, 5, 13};   // key 1 = GPIO4, key 2 = GPIO5, key 3 = GPIO13

const uint32_t DEBOUNCE_MS  = 5;    // a pin must hold a new level this long before it counts
const uint32_t KEEPALIVE_MS = 20;   // resend interval while a key is held

const uint8_t ID_QUERY_BYTE = 0xFE;
const char*   DEVICE_ID     = "3KEY";   // must match DEVICE_ID in ESPdevice.cpp

uint8_t  rawState[NUM_KEYS];      // last raw pin reading
uint8_t  stableState[NUM_KEYS];   // debounced state (LOW = pressed)
uint32_t lastChange[NUM_KEYS];    // when the raw reading last changed
uint32_t lastReport[NUM_KEYS];    // when this key last sent a byte

void setup() {
  Serial.begin(921600);
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_KEYS; i++) {
    pinMode(KEY_PINS[i], INPUT_PULLUP);
    uint8_t state = digitalRead(KEY_PINS[i]);
    rawState[i] = state;
    stableState[i] = state;
    lastChange[i] = now;
    lastReport[i] = now;
  }
}

void loop() {
  // Identity query. Only does work when a byte has actually arrived.
  if (Serial.available() > 0) {
    if (Serial.read() == ID_QUERY_BYTE) {
      Serial.println(DEVICE_ID);
    }
  }

  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_KEYS; i++) {
    uint8_t reading = digitalRead(KEY_PINS[i]);
    if (reading != rawState[i]) {
      rawState[i] = reading;
      lastChange[i] = now;
    }

    // Debounced change -> send press or release.
    if ((uint32_t)(now - lastChange[i]) >= DEBOUNCE_MS && stableState[i] != rawState[i]) {
      stableState[i] = rawState[i];
      bool pressed = (stableState[i] == LOW);
      Serial.write(pressed ? (uint8_t)(0x01 + i * 2) : (uint8_t)(0x02 + i * 2));
      lastReport[i] = now;
    }

    // Held key: keep telling the PC it is still down.
    if (stableState[i] == LOW && (uint32_t)(now - lastReport[i]) >= KEEPALIVE_MS) {
      Serial.write((uint8_t)(0x01 + i * 2));
      lastReport[i] = now;
    }
  }
}
