# Changelog

## 1.1.0
- Every section is now a floating window: drag it anywhere (freeform, no screen-edge snapping), resize from the bottom-left corner, and minimize it into a small, draggable, resizable icon window.
- Invisible snap guides (a vertical line down the middle and horizontal lines at 25%, 50% and 75%). Hold ALT while dragging to skip snapping.
- All window changes (position, size, minimized state, icon window position and size, stacking order, snaps) are saved to `config\layout.json` and restored exactly. Snapped edges follow the guides if the browser window changes size.
- New `[ SETTINGS ]` button (top right) with toggles saved to `config\settings.json`: window snapping, show snap guides, and which social links to show. Reset window layout lives here too.
- Social links (GitHub, YouTube, TikTok, Discord) in the bottom-left corner.
- The credit now reads "made by bagelings, assisted by claude" with the version number beside it. The release build writes the version from the git tag into `app/version.h`.
- Icons for minimized windows can be dropped into `config\icons\` (for example `Profile_icon.png`); placeholders are shown until then.
- Debugger log now grows and shrinks with its window.

## 1.0.x
- First public release: 3-key ESP32 keypad driver for Windows with profiles, live visualizer, latency debugger, themes, custom logo, installer and portable build.
