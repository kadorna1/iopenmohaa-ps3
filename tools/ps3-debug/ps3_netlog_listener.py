#!/usr/bin/env python3
"""UDP log listener for the OpenMoHAA PS3 port (ps3_log.c, v40).

    python3 ps3_netlog_listener.py            # port 9999, writes ps3-netlog.txt
    python3 ps3_netlog_listener.py 9999 run1.txt

Put "<this PC's IP>:9999" in netlog.txt in the game's USRDIR on the PS3.
Datagrams are written to the file and flushed immediately, so the last line
before a console freeze is on disk here.
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
out_path = sys.argv[2] if len(sys.argv) > 2 else "ps3-netlog.txt"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", port))
print(f"listening on UDP {port}, writing {out_path} (Ctrl+C to stop)")

with open(out_path, "ab") as out:
    while True:
        data, addr = sock.recvfrom(4096)
        stamp = time.strftime("%H:%M:%S")
        text = data.decode("utf-8", "replace")
        if not text.endswith("\n"):
            text += "\n"
        line = f"{stamp} {text}"
        sys.stdout.write(line)
        sys.stdout.flush()
        out.write(line.encode("utf-8"))
        out.flush()
