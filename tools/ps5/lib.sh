# Sourced by the tools/ps5 scripts: loads ps5.env and a few helpers.
ps5_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ ! -f "$ps5_root/tools/ps5/ps5.env" ]]; then
  echo "tools/ps5/ps5.env is missing: copy ps5.env.example and fill it in" >&2
  exit 2
fi
set -a
# shellcheck source=/dev/null
source "$ps5_root/tools/ps5/ps5.env"
set +a
ps5ftp() { python3 -I "$ps5_root/tools/ps5/ps5ftp.py" "$@"; }
ps5_port_open() { timeout 2 bash -c "</dev/tcp/$PS5_IP/$1" 2>/dev/null; }
ps5_need_console() {
  if ! ps5_port_open "$PS5_FTP_PORT"; then
    echo "FTP $PS5_IP:$PS5_FTP_PORT is not answering: the console has probably rebooted." >&2
    echo "Re-run the jailbreak, then try again." >&2
    exit 3
  fi
}
