# Changelog

## 1.1.0
- PROFILE, VISUALIZER, KEYBINDS, SERVICE and DEBUGGER are floating windows: drag anywhere (freeform, nothing snaps), resize from either bottom corner, and minimize into a small, draggable, resizable icon window. Minimizing and restoring zoom and fade between the window and its icon (10 fps).
- The default arrangement matches 1.0 (two columns). Windows start cut to their content with no empty space. Snapping windows is planned for a future build.
- Minimizing and restoring follow each other: the icon window appears where the window was, and the window opens where the icon window is.
- All window changes (position, size, minimized state, icon window position and size, stacking order) are saved to `config\layout.json` and restored exactly.
- New `[ SETTINGS ]` pull-down (top right), saved to `config\settings.json`: reset window layout, keep the top buttons above the windows, reset total / CPS, disable CPS counter, colored cps, and which social links to show.
- New `[ THEMES ]` pull-down next to it holds the theme picker and editor (the THEME window is gone). Both menus hang from their button and animate open and closed at 10 fps.
- Social links (GitHub, YouTube, TikTok, Discord) in the bottom-left corner.
- The connection status moved to the bottom-right corner, above the credit, which now reads "made by bagelings, assisted by claude" with the version number beside it. The release build writes the version from the git tag into `app/version.h`.
- Icons for minimized windows can be dropped into `config\icons\` (for example `Profile_icon.png`); placeholders are shown until then.
- A thin divider line (50% opacity) above the links/status strip.
- A second resize handle in the bottom-right corner of every window and icon window.
- Each window's icon is shown at the right end of its title bar when the icon file exists.
- The settings and themes buttons become icons (`Settings_icon.png`, `Theme_icon.png`, text until they exist) and stay on top of the windows; this can be turned off in SETTINGS.
- Social links use `Github_icon.png`, `Youtube_icon.png`, `Tiktok_icon.png`, `Discord_icon.png` when present.
- Visualizer counters: **CPS** and **Total**, larger, centred beside the key boxes. Only the numbers pop on every press, and CPS is colour coded (orange from 5, red from 10, purple from 15). The total is counted by the app (works with the page closed) and saved in `config\counters.json`.
- Debugger log now grows and shrinks with its window.

## 1.0.x
- First public release: 3-key ESP32 keypad driver for Windows with profiles, live visualizer, latency debugger, themes, custom logo, installer and portable build.
