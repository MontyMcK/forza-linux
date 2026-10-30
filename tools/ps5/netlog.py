#!/usr/bin/env python3
"""Receives log lines the PS5 title sends over TCP (PS5_LOG_HOST/PORT in
tools/ps5/ps5.env) and writes each connection to
research/private/ps5-logs/net-<time>.txt (gitignored).

  tools/ps5/netlog.py            listen until stopped
  tools/ps5/netlog.py --once     exit after the first connection closes
"""
import os
import socket
import sys
import time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    env = {}
    for line in open(os.path.join(REPO, "tools", "ps5", "ps5.env")):
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    port = int(env.get("PS5_LOG_PORT", 9599))
    out_dir = os.path.join(REPO, "research", "private", "ps5-logs")
    os.makedirs(out_dir, exist_ok=True)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"netlog: listening on :{port}", flush=True)
    while True:
        conn, addr = srv.accept()
        path = os.path.join(out_dir, time.strftime("net-%Y%m%d-%H%M%S.txt"))
        print(f"netlog: {addr[0]} -> {path}", flush=True)
        with conn, open(path, "wb") as f:
            while data := conn.recv(65536):
                f.write(data)
                f.flush()
        print(f"netlog: {addr[0]} closed", flush=True)
        if "--once" in sys.argv:
            return


if __name__ == "__main__":
    main()
