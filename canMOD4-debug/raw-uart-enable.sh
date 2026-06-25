#!/bin/bash
# Switch mod4 to a plain UART using the OFFICIAL wb-hwconf-manager module `wbe2x-generic-uart`,
# via /etc/wb-hardware.conf. Result: /dev/ttyS5 talks directly to the HT42B416 (115200 8N1).
# Run ON the WB controller as root. Undo with raw-uart-restore.sh.
set +e
CONF=/etc/wb-hardware.conf
P=/dev/ttyS5

systemctl stop wb-mqtt-smartweb 2>/dev/null
# back up the original conf once (it is immutable by default -> chattr -i to edit)
chattr -i "$CONF" 2>/dev/null
[ -f "$CONF.orig" ] || cp "$CONF" "$CONF.orig"
cat > "$CONF" <<'JSON'
{
    "mod4": { "module": "wbe2x-generic-uart", "options": {} }
}
JSON
echo "config-apply (mod4 -> wbe2x-generic-uart):"
wb-hwconf-helper config-apply 2>&1 | tail -3
sleep 2

[ -e $P ] && [ ! -c $P ] && rm -f $P                       # clear any stale node
[ ! -c $P ] && { MM=$(cat /sys/class/tty/ttyS5/dev 2>/dev/null); mknod $P c ${MM%%:*} ${MM##*:} 2>/dev/null; }
stty -F $P 115200 raw -echo clocal -crtscts min 0 time 3 2>/dev/null

# verify chip answers (read with dd; printf to write)
: >/tmp/r.bin; timeout 2 dd if=$P of=/tmp/r.bin bs=1 2>/dev/null & rp=$!; sleep 0.15; printf 'V\r' >$P; wait $rp 2>/dev/null
V=$(od -An -tx1 /tmp/r.bin 2>/dev/null | tr -d '\n' | tr -s ' ')
echo "ttyS5: $(ls -l $P 2>&1)"
echo "V=[$V]"
echo "$V" | grep -qi '56 04 d9' && echo "OK: raw UART up, chip VID 0x04D9 answers." \
                                 || echo "no V — sometimes flaky; 'reboot' then rerun."
