# What was fixed (keypad not detected on Linux)

1. linux/99-espdevice.rules -> linux/70-espdevice.rules
   The uaccess permission and ID_MM_DEVICE_IGNORE must be set BEFORE udev rule 73 / ModemManager's 77-80 rules.
   At "99-" they were silently ignored: /dev/ttyUSB0 stayed root:dialout (permission denied) and ModemManager could probe the port.
2. install.sh removes the old 99- file, installs the 70- rule, and adds you to the dialout (or uucp) group as a fallback.
3. app/ESPdevice.cpp (Linux section only): every port that cannot be used is now logged with the reason and a fix hint
   (permission denied / busy / no reply + the bytes it sent) in ~/.config/espdevice/espdevice.log and the DEBUGGER;
   the identity probe now retries 6 times over ~4 s (ESP32 reboots when the port opens).

# How to apply
    g++ -std=c++17 -O2 -pthread -static-libstdc++ -static-libgcc app/ESPdevice.cpp -o linux/espdevice
    cd linux && ./install.sh
    # then LOG OUT AND IN once (group + ACLs), unplug/replug the keypad, run: espdevice
    # if it still says not connected:  cat ~/.config/espdevice/espdevice.log   (it now says why)
