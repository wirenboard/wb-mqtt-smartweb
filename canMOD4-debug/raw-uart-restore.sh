#!/bin/bash
# Restore the original /etc/wb-hardware.conf (CAN driver on mod4). Undo of raw-uart-enable.sh.
set +e
CONF=/etc/wb-hardware.conf
chattr -i "$CONF" 2>/dev/null
if [ -f "$CONF.orig" ]; then cp "$CONF.orig" "$CONF" && rm -f "$CONF.orig"; fi
echo "config-apply (restore):"
wb-hwconf-helper config-apply 2>&1 | tail -3
sleep 2
chattr +i "$CONF" 2>/dev/null                              # WB keeps this conf immutable
echo "module: $(grep -o '"module": "[^"]*"' "$CONF")"
echo "canMOD4: $(ip -br link show canMOD4 2>/dev/null)"
echo "re-enable the app if needed:  systemctl enable --now wb-mqtt-smartweb"
