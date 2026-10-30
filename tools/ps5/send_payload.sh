#!/usr/bin/env bash
# Sends a payload ELF to elfldr (PS5_ELFLDR_PORT), which runs it in its own process.
#   tools/ps5/send_payload.sh build-ps5/probe/oh2probe.elf
# If elfldr isn't listening (the console rebooted), it is started first through
# pldmgr's web API from PS5_ELFLDR_PLDMGR (elfldr v0.27, installed 2026-10-30).
set -euo pipefail
source "$(dirname "$0")/lib.sh"
elf=${1:?usage: send_payload.sh FILE.elf}
[[ -f $elf ]] || { echo "no such file: $elf" >&2; exit 2; }
ps5_need_console
if ! ps5_port_open "$PS5_ELFLDR_PORT"; then
  echo "elfldr not listening; starting it through pldmgr"
  curl -sf -m 15 -X POST -H 'X-PLDMGR-Request: 1' \
    "http://$PS5_IP:$PS5_PLDMGR_PORT/loadpayload:$PS5_ELFLDR_PLDMGR" > /dev/null \
    || { echo "pldmgr couldn't start $PS5_ELFLDR_PLDMGR" >&2; exit 1; }
  for _ in $(seq 1 20); do ps5_port_open "$PS5_ELFLDR_PORT" && break; sleep 1; done
  ps5_port_open "$PS5_ELFLDR_PORT" || { echo "elfldr didn't come up on $PS5_ELFLDR_PORT" >&2; exit 1; }
fi
python3 -I - "$PS5_IP" "$PS5_ELFLDR_PORT" "$elf" <<'PY'
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=10)
s.sendall(open(sys.argv[3], "rb").read())
s.shutdown(socket.SHUT_WR)
s.close()
PY
echo "sent $elf to elfldr $PS5_IP:$PS5_ELFLDR_PORT"
