#!/usr/bin/env bash
# Lista as DWM1001-DEV conectadas: porta serial (console) e serial do J-Link
# (para "west flash --dev-id <serial>").
#
# Uso: scripts/list-boards.sh

shopt -s nullglob
links=(/dev/serial/by-id/usb-SEGGER_J-Link_*)

if [[ ${#links[@]} -eq 0 ]]; then
	echo "Nenhuma J-Link encontrada."
	exit 0
fi

printf '%-14s %s\n' "PORTA" "SERIAL_JLINK"
for link in "${links[@]}"; do
	serial="$(basename "$link" | sed -E 's/^usb-SEGGER_J-Link_0*([0-9]+)-if.*/\1/')"
	printf '%-14s %s\n' "$(readlink -f "$link")" "$serial"
done
