#!/usr/bin/env bash
# Removes ESPdevice for the current user. Your settings (~/.config/espdevice) are kept unless you add --purge.
#   ./uninstall.sh [--purge] [--system]      --system also removes the udev rule and the uinput module entry (sudo)
set -e
PURGE=0; SYSTEM=0
for a in "$@"; do case "$a" in --purge) PURGE=1 ;; --system) SYSTEM=1 ;; *) echo "unknown option: $a"; exit 2 ;; esac; done
systemctl --user disable --now espdevice.service 2>/dev/null || true
pkill -x espdevice 2>/dev/null || true
rm -f "$HOME/.local/bin/espdevice" "$HOME/.local/share/applications/espdevice.desktop" \
      "$HOME/.local/share/icons/hicolor/256x256/apps/espdevice.png" \
      "$HOME/.config/autostart/espdevice.desktop" "$HOME/.config/systemd/user/espdevice.service"
[ "$PURGE" = 1 ] && rm -rf "$HOME/.config/espdevice" && echo "removed ~/.config/espdevice"
if [ "$SYSTEM" = 1 ]; then
  sudo rm -f /etc/udev/rules.d/99-espdevice.rules /etc/modules-load.d/espdevice.conf
  sudo udevadm control --reload-rules
fi
echo "ESPdevice removed."
