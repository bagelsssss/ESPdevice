# ESPdevice (v4)

Background driver for the 3-key ESP32 keypad: keyboard-style input, profiles,
live visualizer, debugger, themes. Local website at http://localhost:9191.

Compile-checked with a MinGW cross-compiler (no warnings). It has NOT been built
with MSVC or run on a real keypad here - your build is the real test.

## Files (keep all four in ONE folder)
- `ESPdevice.cpp`
- `default_logo.h`  (the built-in logo, embedded as bytes; included by ESPdevice.cpp)
- `espdevice.rc`   (`101 ICON "espdevice.ico"`)
- `espdevice.ico`

## Build (Developer Command Prompt for VS)

    rc /nologo espdevice.rc
    cl /EHsc /std:c++17 /O2 /MT /DUNICODE /D_UNICODE ESPdevice.cpp espdevice.res /link /SUBSYSTEM:WINDOWS /OUT:ESPdevice.exe

Close any older copy first (the previous build under its old name) or both
will type every key. Your `config\` folder (profiles, theme, art) is reused as is.
The log file is now `config\espdevice.log`.

## Use
Run `ESPdevice.exe` -> tray icon -> right-click > Open Settings (or browse to
http://localhost:9191). Only one copy can run at a time.

## Features
- Input: scan-code SendInput, tap + hold, typematic repeat from your Windows
  keyboard delay/rate, stale-key release after 1.5 s of silence. Every key is
  bindable (symbols, F1-F24, numpad, L/R modifiers, media keys, combos like
  CTRL+SHIFT+A). Type a binding and press ENTER, or use CAPTURE.
- Delay: one high-priority thread blocks on the serial port and wakes the instant
  a byte arrives; the key path has no locks, files or network.
- Profiles: up to 99, saved atomically to `config\profiles.json`.
- Visualizer: presses are pushed to the page. Debugger: reports/s, event log,
  byte-to-SendInput latency.
- Themes: Light (default), Dark, Terminal, Bagel + your own (COPY, then edit ~36
  colours). Saved in `config\theme.json`.
- Title art: put plain-text art in `config\art.txt` (fitted automatically).
- Sections: click any `[ TITLE ]` bar to fold / unfold its box (remembered per browser).
- Logo: the built-in default logo is recolored to match the theme. To use a different
  default, put a PNG at `config\default_logo.png`. Clicking the logo on the page uploads
  a custom one (shown as-is); the click-to-change hint only shows while hovering the logo.
- Safety: the site only answers to localhost and needs a custom header for
  changes, so other websites can't remap your keys.

## If it misbehaves
`config\espdevice.log` - look for `FATAL:` / `ERROR in` lines.
