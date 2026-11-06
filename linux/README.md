# ESPdevice for Linux (1.2.0_linux)

The same keypad driver and settings page as the Windows app, as one program: it listens to the ESP32 over serial, types
real keystrokes through a virtual keyboard (`/dev/uinput`) and serves the settings page at http://localhost:9191.
Profiles, the visualizer, click stats, themes, advanced theming and 8-key mode work as described in the main README.

- **Platform:** x86_64 Linux, built on Ubuntu 22.04 (needs glibc 2.35 or newer), no extra libraries.
- **Keys are typed on X11, Wayland and the text console**, because they come from a virtual keyboard device, not from a display-server API.
- **Hardware:** the same ESP32 keypad and firmware as on Windows (see the main README). The board appears as `/dev/ttyUSB*` or `/dev/ttyACM*`.

## Install

```
tar xzf ESPdevice-linux-x86_64.tar.gz
cd ESPdevice-linux-x86_64
./install.sh                 # add --autostart to also start it when you log in
```

`install.sh` runs as your normal user. It copies the program to `~/.local/bin/espdevice`, adds an ESPdevice entry to your
app menu, and (with `sudo`) installs two small permission files:

- `/etc/udev/rules.d/99-espdevice.rules` lets the logged-in desktop user open `/dev/uinput` and the keypad's serial port, and
  keeps ModemManager from probing the keypad.
- `/etc/modules-load.d/espdevice.conf` loads the `uinput` kernel module at boot.

Then **unplug and replug the keypad** (or log out and in once) and start ESPdevice from the menu or with `espdevice`.
It opens the settings page in your browser. Other options: `./install.sh --systemd` (user service instead of the autostart entry),
`./install.sh --no-system` (skip the permission files). Remove everything with `./uninstall.sh` (`--purge` also deletes your settings).

## Running it

```
espdevice [--background] [--no-browser] [--device /dev/ttyUSB0] [--dry-run] [--version]
```

| Option | Meaning |
|--------|---------|
| `--background` | Used by autostart / systemd: do not open the browser. |
| `--no-browser` | Never open the browser. |
| `--device PATH` | Use this serial device instead of scanning `/dev/ttyACM*` and `/dev/ttyUSB*`. |
| `--dry-run` | Print the keys (`KEY 30 down`...) instead of typing them. Good for checking a setup. |

- There is **no tray icon** on Linux (tray support differs between desktops). The app runs in the background; pause, resume and
  terminate are in **SETTINGS** on the page. Starting `espdevice` again just opens the page. `Ctrl+C` or `SIGTERM` quits cleanly.
- **Your files** are in `~/.config/espdevice/` (or `$XDG_CONFIG_HOME/espdevice`): `profiles.json`, `settings.json`, `layout.json`, `theme.json`,
  `theme_advanced.json`, `counters.json`, `clickstats.json` and `espdevice.log`. They are the same files and formats as the Windows `config\` folder,
  so you can copy them between the two.
- Only one copy can run at a time (a lock file in the config folder).

## Flashing the ESP32 from Linux

Use the Arduino IDE as in the main README. To upload you need access to the serial port: either the `99-espdevice.rules` file above
(works while you are logged in at the desktop) or `sudo usermod -aG dialout $USER` and log in again. Close the Arduino serial monitor
before starting ESPdevice: only one program can have the port.

## Troubleshooting

- **"keys NOT typed" / "cannot open /dev/uinput"** (in the DEBUGGER log and `~/.config/espdevice/espdevice.log`): the permission files are missing
  or not active yet. Run `./install.sh` again, then log out and in. Check with `ls -l /dev/uinput` (needs `modprobe uinput` once if the file is missing).
  Without the udev rule you can also run `sudo setfacl -m u:$USER:rw /dev/uinput` for the current boot.
- **The keypad is not found** (status stays "not connected"): check that `ls /dev/ttyUSB* /dev/ttyACM*` shows the board and that you can open it
  (`dialout` group or the udev rule). If a CH340 board disappears right after plugging in, the `brltty` package is claiming it: remove `brltty`.
  If the board answers slowly, ESPdevice tries three times; `--device` skips the scan.
- **Keys are typed twice**: two copies are running (for example the autostart entry and a manual start). ESPdevice prevents that, but an old `espdevice`
  from another install location may still be there: `pkill -x espdevice` and start once.
- **Games**: the keypad is a normal virtual keyboard to the system, so native, Proton and Wine games should see it like any other keyboard
  (not tested with real games yet; please report problems).

## Differences from the Windows app

- No tray icon, no installer: a tar.gz with a small install script.
- Key repeat uses a fixed 250 ms delay and 30 repeats per second (Linux has no single place to read the desktop's setting).
- Key names still assume a US layout: a binding like `A` sends the key in the "A" position, and your desktop's keyboard layout decides what appears.
- The Windows key is the Super key.
- x86_64 only. No ARM build.
- Built and tested with a simulated keypad (the project's CI runs `linux/selftest.py` on every build). Real-hardware reports are welcome.

## Building it yourself

```
g++ -std=c++17 -O2 -pthread -static-libstdc++ -static-libgcc app/ESPdevice.cpp -o espdevice
```

(`app/default_logo.h` and `app/version.h` must be next to it.) Then run `python3 linux/selftest.py ./espdevice` to check it.
