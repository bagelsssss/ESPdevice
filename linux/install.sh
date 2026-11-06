#!/usr/bin/env bash
# ESPdevice installer for Linux (per user; only the permission files need sudo).
#   ./install.sh                 install the app, the menu entry and the permission files
#   ./install.sh --autostart     also start ESPdevice when you log in (desktop autostart entry)
#   ./install.sh --systemd       also start it with a systemd user service instead
#   ./install.sh --no-system     skip the permission files (you set up /dev/uinput access yourself)
set -e
cd "$(dirname "$0")"

AUTOSTART=0; SYSTEMD=0; SYSTEM=1
for a in "$@"; do
  case "$a" in
    --autostart) AUTOSTART=1 ;;
    --systemd) SYSTEMD=1 ;;
    --no-system) SYSTEM=0 ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "unknown option: $a"; exit 2 ;;
  esac
done

if [ "$(id -u)" = 0 ]; then
  echo "Run this as your normal user, not root (it installs into your home folder and asks for sudo itself)."; exit 1
fi
[ -f espdevice ] || { echo "espdevice binary not found next to install.sh"; exit 1; }

BIN="$HOME/.local/bin/espdevice"
ICON="$HOME/.local/share/icons/hicolor/256x256/apps/espdevice.png"
APPS="$HOME/.local/share/applications"
mkdir -p "$HOME/.local/bin" "$(dirname "$ICON")" "$APPS"

install -m 755 espdevice "$BIN"
install -m 644 espdevice.png "$ICON"
sed -e "s|@BIN@|$BIN|g" -e "s|@ICON@|$ICON|g" espdevice.desktop > "$APPS/espdevice.desktop"
echo "installed: $BIN"
echo "installed: $APPS/espdevice.desktop (menu entry)"

if [ "$SYSTEM" = 1 ]; then
  echo
  echo "Permissions (needs sudo): lets you, as the logged-in desktop user, use /dev/uinput and the keypad's serial port."
  SUDO=sudo; command -v sudo >/dev/null 2>&1 || SUDO=
  if [ -z "$SUDO" ]; then echo "sudo is not available. As root, run:"; fi
  run() { if [ -n "$SUDO" ]; then $SUDO "$@"; else echo "  $*"; fi; }
  run install -m 644 99-espdevice.rules /etc/udev/rules.d/99-espdevice.rules
  if [ -n "$SUDO" ]; then echo uinput | $SUDO tee /etc/modules-load.d/espdevice.conf >/dev/null; else echo "  echo uinput > /etc/modules-load.d/espdevice.conf"; fi
  run modprobe uinput || true
  run udevadm control --reload-rules
  run udevadm trigger --subsystem-match=misc --subsystem-match=tty
  echo "done. If ESPdevice still says it cannot open /dev/uinput, log out and back in once."
fi

if [ "$AUTOSTART" = 1 ]; then
  mkdir -p "$HOME/.config/autostart"
  sed -e "s|@BIN@|$BIN --background|g" -e "s|@ICON@|$ICON|g" espdevice.desktop > "$HOME/.config/autostart/espdevice.desktop"
  echo "autostart: $HOME/.config/autostart/espdevice.desktop"
fi
if [ "$SYSTEMD" = 1 ]; then
  mkdir -p "$HOME/.config/systemd/user"
  sed -e "s|@BIN@|$BIN|g" espdevice.service > "$HOME/.config/systemd/user/espdevice.service"
  if systemctl --user daemon-reload && systemctl --user enable espdevice.service; then
    echo "systemd user service enabled (starts with your next login; start it now with: systemctl --user start espdevice)"
  else
    echo "could not reach the systemd user manager; after logging in to your desktop run: systemctl --user enable espdevice.service"
  fi
fi

case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo; echo "Note: $HOME/.local/bin is not in your PATH; start the app from the menu or run $BIN" ;; esac
echo
echo "Start it from your app menu (ESPdevice) or run: espdevice"
echo "Settings page: http://localhost:9191"
