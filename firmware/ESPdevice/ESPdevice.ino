// ESPdevice - keypad firmware for ESP32 (WROOM-32 / CP2102 USB-serial): 3 keys, or 8 keys (experimental)
// -----------------------------------------------------------------------------------------------------
// Sends one byte per key event over USB serial at 921600 baud. No WiFi, no
// Bluetooth, no LED code: just debounce + report, so the loop stays fast.
//
// The same firmware works with 3 keys or 8 keys: it always scans all 8 pins. A pin with nothing
// connected is pulled up internally and simply never reports a press. Which keys the PC uses is
// chosen in the ESPdevice page (SETTINGS > 8-key support).
//
// Protocol (what ESPdevice.exe expects):
//   key N press   : byte (N*2 - 1)   -> key1 = 0x01, key2 = 0x03 ... key8 = 0x0F
//   key N release : byte (N*2)       -> key1 = 0x02, key2 = 0x04 ... key8 = 0x10
//   held key      : the press byte is repeated every KEEPALIVE_MS, so the PC
//                   can tell "still held" from "board unplugged / went quiet".
//   identity query: the PC sends 0xFE, the board replies "8KEY\n". This is how
//                   the PC finds the right COM port without a fixed port number.
//                   (ESPdevice.exe also still accepts "3KEY" from the older 3-key firmware.)
//
// Wiring: each key connects its GPIO to GND (internal pull-up is used, so no
// resistors needed).
//   keys 1-3 (the original keypad): GPIO 4, 5, 13
//   keys 4-8 (the extension):       GPIO 18, 19, 21, 22, 23
// Do not use GPIO 0, 2, 12, 15 (boot pins), 6-11 (flash), 1 and 3 (USB serial), 34-39 (no pull-up).
//
// KEY ORDER: the order of the numbers in KEY_PINS decides which key is "key 1,
// 2, 3 ... 8" on the PC. If your keys show up in the wrong order in the ESPdevice
// visualizer, swap the numbers here (not the wiring).

const uint8_t NUM_KEYS = 8;
const uint8_t KEY_PINS[NUM_KEYS] = {4, 5, 13, 18, 19, 21, 22, 23};

const uint32_t DEBOUNCE_MS  = 5;    // a pin must hold a new level this long before it counts
const uint32_t KEEPALIVE_MS = 20;   // resend interval while a key is held

// Hot-plugging the extension: while it is being plugged in its pins can read LOW for a moment (contact bounce, static,
// ground connecting first). An extension pin (keys 4-8) therefore only counts once it has been seen HIGH for ARM_MS
// since power-up, so a pin that starts out LOW is ignored until it goes HIGH, and a glitch can never leave a key
// "held" or the extension "dead". While no key is down the extension pins are also re-initialised every REINIT_MS.
const uint8_t  FIRST_EXT_KEY = 3;       // index of key 4
const uint32_t ARM_MS        = 50;
const uint32_t REINIT_MS     = 500;

const uint8_t ID_QUERY_BYTE = 0xFE;
const char*   DEVICE_ID     = "8KEY";   // ESPdevice.exe accepts "3KEY" and "8KEY"

uint8_t  rawState[NUM_KEYS];      // last raw pin reading
uint8_t  stableState[NUM_KEYS];   // debounced state (LOW = pressed)
uint32_t lastChange[NUM_KEYS];    // when the raw reading last changed
uint32_t lastReport[NUM_KEYS];    // when this key last sent a byte
bool     armed[NUM_KEYS];         // extension pins: true once the pin has been HIGH for ARM_MS
uint32_t highSince[NUM_KEYS];     // when the pin last became HIGH
uint32_t lastReinit = 0;

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
    armed[i] = (i < FIRST_EXT_KEY);
    highSince[i] = now;
    if (state == LOW) { stableState[i] = HIGH; rawState[i] = HIGH; }   // a pin that starts LOW is ignored until it goes HIGH
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
    if (!armed[i]) {                       // extension pin not trusted yet
      if (reading == HIGH) {
        if ((uint32_t)(now - highSince[i]) >= ARM_MS) armed[i] = true;
      } else {
        highSince[i] = now;                // LOW again: start waiting over
      }
      if (!armed[i]) continue;
    }
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

  // Hot-plug recovery: with no key down, re-initialise the extension pins now and then.
  if ((uint32_t)(now - lastReinit) >= REINIT_MS) {
    lastReinit = now;
    bool anyDown = false;
    for (uint8_t i = 0; i < NUM_KEYS; i++) if (stableState[i] == LOW) anyDown = true;
    if (!anyDown) for (uint8_t i = FIRST_EXT_KEY; i < NUM_KEYS; i++) pinMode(KEY_PINS[i], INPUT_PULLUP);
  }
}
