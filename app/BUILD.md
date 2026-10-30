# Building ESPdevice by hand

Most people should download the installer from the Releases page instead.
GitHub Actions builds the same thing on every push (`.github/workflows/build.yml`).

## Files (keep all of these in ONE folder)
- `ESPdevice.cpp`
- `default_logo.h`  (the built-in logo, embedded as bytes; included by ESPdevice.cpp)
- `espdevice.rc`   (`101 ICON "espdevice.ico"`)
- `espdevice.ico`
- `version.h`  (`#define ESP_VERSION "1.1.0"`, the version shown on the page; optional, 1.1.0 is built in if it is missing)

## Build (Developer Command Prompt for VS)

    rc /nologo espdevice.rc
    cl /EHsc /std:c++17 /O2 /MT /DUNICODE /D_UNICODE ESPdevice.cpp espdevice.res /link /SUBSYSTEM:WINDOWS /OUT:ESPdevice.exe

Close any older copy first or both will type every key. Your `config\` folder
(profiles, theme, art) sits next to the exe and is reused as is. The log file is
`config\espdevice.log`.

## Use
Run `ESPdevice.exe`, then tray icon > right-click > Open Settings (or browse to
http://localhost:9191). Only one copy can run at a time.
