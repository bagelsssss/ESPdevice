# Changelog

## 1.2.0_linux
- **Linux port (x86_64)**: the same app and settings page as 1.2.0 on Linux, released as `ESPdevice-linux-x86_64.tar.gz`. Serial port through `/dev/ttyUSB*` / `/dev/ttyACM*` (921600 baud, retries while the board reboots after opening), keys typed through a `/dev/uinput` virtual keyboard (X11, Wayland, console), config in `~/.config/espdevice/` with the same files as Windows, single-instance lock, clean quit on Ctrl+C / SIGTERM.
- No tray icon on Linux: pause / resume / terminate are in SETTINGS, the app never opens a browser by itself (so autostart is quiet); `--open` (used by the menu entry) opens the settings page. Other options: `--device PATH`, `--dry-run`; `--background` and `--no-browser` are still accepted.
- `install.sh` / `uninstall.sh`, a udev rule (permission for `/dev/uinput` and the serial port, ModemManager kept away), an app menu entry, optional autostart or systemd user service.
- `.github/workflows/build.yml` now builds both: a `windows` job and a `linux` job (builds the binary, runs `linux/selftest.py` against a simulated keypad, packages the tar.gz); a `release` job attaches the files to releases (Windows files for `v*` tags, the Linux file for `*_linux` tags).
- Linux fixes after first real-hardware tests: the udev rule is `70-espdevice.rules` (the old 99- name was ignored by udev), `/dev/uinput` access via a `uinput` group as well as uaccess, the app retries `/dev/uinput` every 3 s, and the log says exactly why a serial port or `/dev/uinput` could not be used.
- The Windows build is unchanged (the Linux code is behind `#ifdef`); the only shared change is that the two socket-timeout calls now go through one helper with identical behaviour on Windows.

## 1.2.0
- **Icons**: PROFILE, VISUALIZER and KEYBINDS have built-in 16x16 pixel icons inside the exe (title bar and icon window), centred, in the theme's text colour. Loading icon files from `config\icons\` was removed (the top-right buttons and social links are text); the icon system will be revamped in the next update. `ICONS.md` is gone.
- **Advanced theming** (SETTINGS > THEMING, off by default): in THEMES, edit individual elements (text, backgrounds, window lines, buttons, inputs, fonts, sizes) of any custom theme, with PICK AN ELEMENT to click the thing you want. Edits are per theme, saved in the new `config\theme_advanced.json` (older builds ignore it); switching it off shows the plain theme and keeps the edits. Built-in themes need COPY first.
- **Visualizer**: keys centred, **CPS** at the bottom left and **Total** (same size as CPS) at the bottom right, each in its own row so they can't wrap or get cut off any more (this was the "counter disappears" bug). The Total no longer pops; CPS still does. The Total is shortened from 100,000 (123K, 1.25M, 12.3M, 123M, 1.23B); hover for the exact value.
- **Per-key totals** (SETTINGS > per-key totals): a counter under each key, clicks since the app started.
- **Click stats**: bar-chart button at the top right of the VISUALIZER window. SESSION graph (clicks per minute since the app started) and ALL TIME graph (clicks per day), for all keys or a single key, with hover values. ALL TIME needs SETTINGS > **log click stats** (off by default, greyed out while off) and is saved in the new, versioned `config\clickstats.json`; `counters.json` is unchanged. SETTINGS > CLEAR CLICK STATS wipes it.
- Fixed a scrollbar that appeared in the VISUALIZER: the CPS number grows for a moment on every press and pushed past the window edge (now there is room for it), and a default layout saved in 3-key mode was not re-fitted when the app started in 8-key mode.
- The open SETTINGS / THEMES panel now covers the DEBUGGER button instead of being drawn under it.
- **Profiles per mode**: 3-key and 8-key have separate lists. 3-key keeps `profiles.json` in the same format, 8-key uses `profiles8.json` (starts as a copy of the 3-key list). The old file is backed up once as `profiles.pre-1.2.bak`.
- **test profile** at the end of every list: can't be renamed or removed, no bindings, never saved. The keys light up and count, but nothing is typed.
- **SERVICE window removed**: pause / resume / terminate are in SETTINGS.
- **DEBUGGER** is now a pull-down button under SETTINGS (uses `Debugger_icon.png` when present); its window is gone.
- New default layout: PROFILE and VISUALIZER on the left, KEYBINDS on the right. It re-fits automatically when the browser window changes size (square, small, fullscreen) until you arrange windows yourself; arranged windows stay inside the browser window. In a small window the social links hide and `[8-key Mode Active]` moves to the bottom left.
- **Animations**: windows now expand then roll out (a thin bar spreads along x, then grows down) and minimize the same way backwards, at 30 fps. SETTINGS > old animations brings back zoom and fade; disable animations still turns everything off.
- **8-key hot-plug**: the firmware ignores extension pins until they have been HIGH for 50 ms (so a pin that starts LOW or glitches while plugging in is ignored) and re-initialises them regularly. The DEBUGGER log now says when an extension key sends its first signal, when signals are ignored because 8-key support is off, and when the mode changes. Please re-flash the firmware.
- **Compatibility**: no file fields were renamed or removed. Settings and layout entries written by other versions are kept when saving.

## 1.1.0
- PROFILE, VISUALIZER, KEYBINDS, SERVICE and DEBUGGER are floating windows: drag anywhere (freeform, nothing snaps), resize from either bottom corner, and minimize into a small, draggable, resizable icon window. Minimizing and restoring zoom and fade between the window and its icon (30 fps).
- The default arrangement matches 1.0 (two columns). Windows start cut to their content, in width and height, with no empty space. Snapping windows is planned for a future build.
- Minimizing and restoring follow each other: the icon window appears where the window was, and the window opens where the icon window is.
- All window changes (position, size, minimized state, icon window position and size, stacking order) are saved to `config\layout.json` and restored exactly.
- New `[ SETTINGS ]` pull-down (top right), saved to `config\settings.json`: reset window layout, keep the top buttons above the windows, reset total / CPS, disable CPS counter, colored cps, and which social links to show.
- New `[ THEMES ]` pull-down next to it holds the theme picker and editor (the THEME window is gone). Both menus hang from their button. Opening spreads a bar out from the button and grows the panel downward; closing shrinks and fades it. Animations run at 30 fps and can be turned off in SETTINGS ("disable animations").
- Experimental **8-key support** with one ESP32: extra keys on GPIO 18, 19, 21, 22 and 23 (keys 4 to 8, default bindings F G H J K). The firmware now scans 8 pins (unused ones are harmless) and identifies as `8KEY`; the app accepts `3KEY` and `8KEY`. Turn it on in SETTINGS > 8-key support (experimental); `[8-key Mode Active]` is shown at the bottom centre while it is on. Off by default, and with it off nothing changes for a 3-key keypad.
- Social links (GitHub, YouTube, TikTok, Discord) in the bottom-left corner.
- The connection status moved to the bottom-right corner, above the credit, which now reads "made by bagelings, assisted by claude" with the version number beside it. The release build writes the version from the git tag into `app/version.h`.
- Icons for minimized windows can be dropped into `config\icons\` (for example `Profile_icon.png`); placeholders are shown until then.
- A thin divider line (50% opacity) above the links/status strip.
- A second resize handle in the bottom-right corner of every window and icon window.
- Each window's icon is shown at the right end of its title bar when the icon file exists.
- The settings and themes buttons become icons (`Settings_icon.png`, `Theme_icon.png`, text until they exist) and stay on top of the windows; this can be turned off in SETTINGS.
- Social links use `Github_icon.png`, `Youtube_icon.png`, `Tiktok_icon.png`, `Discord_icon.png` when present.
- Visualizer counters: **CPS** and **Total**, larger, centred beside the key boxes. Only the numbers pop on every press (size only, no fading), and CPS is colour coded (orange from 5, red from 10, purple from 15). The total is counted by the app (works with the page closed) and saved in `config\counters.json`.
- Debugger log now grows and shrinks with its window.

## 1.0.x
- First public release: 3-key ESP32 keypad driver for Windows with profiles, live visualizer, latency debugger, themes, custom logo, installer and portable build.
