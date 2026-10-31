# Changelog

## 1.1.0
- Every section is now a floating window: drag it anywhere (freeform, nothing snaps), resize from either bottom corner, and minimize it into a small, draggable, resizable icon window.
- The default arrangement matches 1.0: two columns, with THEME and SETTINGS closed until you open them. Snapping windows is planned for a future build.
- Minimizing and restoring follow each other: the icon window appears where the window was, and the window opens where the icon window is.
- All window changes (position, size, minimized state, icon window position and size, stacking order) are saved to `config\layout.json` and restored exactly.
- New `[ SETTINGS ]` button (top right) with toggles saved to `config\settings.json`: keep the top buttons above the windows and which social links to show. Reset window layout and reset counters live here too.
- New `[ THEMES ]` button next to it opens and closes the THEME window.
- Social links (GitHub, YouTube, TikTok, Discord) in the bottom-left corner.
- The connection status moved to the bottom-right corner, above the credit, which now reads "made by bagelings, assisted by claude" with the version number beside it. The release build writes the version from the git tag into `app/version.h`.
- Icons for minimized windows can be dropped into `config\icons\` (for example `Profile_icon.png`); placeholders are shown until then.
- A thin divider line (50% opacity) above the links/status strip.
- A second resize handle in the bottom-right corner of every window and icon window.
- Each window's icon is shown at the right end of its title bar when the icon file exists.
- The settings and themes buttons become icons (`Settings_icon.png`, `Theme_icon.png`, text until they exist) and stay on top of the windows; this can be turned off in SETTINGS.
- Social links use `Github_icon.png`, `Youtube_icon.png`, `Tiktok_icon.png`, `Discord_icon.png` when present.
- Visualizer counters: **CPS** and **Total**, larger, centred beside the key boxes, and flashing on every key press. The total is counted by the app (works with the page closed), saved in `config\counters.json`, and can be reset in SETTINGS.
- Debugger log now grows and shrinks with its window.

## 1.0.x
- First public release: 3-key ESP32 keypad driver for Windows with profiles, live visualizer, latency debugger, themes, custom logo, installer and portable build.
