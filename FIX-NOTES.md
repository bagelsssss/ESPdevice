# Fixes

## Round 1: keypad not detected
- linux/99-espdevice.rules -> 70-espdevice.rules (uaccess + ModemManager-ignore only work before udev rule 73/77).
- Every unusable serial port is logged with the reason + fix hint; probe retries 6x over ~4 s.

## Round 2: keypad detected but no keystrokes (/dev/uinput)
- Rule now also gives /dev/uinput to group "uinput" (install.sh creates it and adds you) besides uaccess.
- App retries opening /dev/uinput every 3 s, so fixing permissions needs no restart.
- Log lines: exact fix command for permission denied / module not loaded, "Virtual keyboard created: /sys/devices/virtual/input/inputN",
  and the first failed write.

# Apply
    g++ -std=c++17 -O2 -pthread -static-libstdc++ -static-libgcc app/ESPdevice.cpp -o linux/espdevice
    cd linux && ./install.sh        # then log out and in once
    espdevice
    tail -20 ~/.config/espdevice/espdevice.log
    ls -l /dev/uinput               # want: group uinput, crw-rw----+
    sudo evtest                     # pick "ESPdevice keypad", press a key on the pad
