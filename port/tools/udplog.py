"""Show log output from the Wii U (WHBLogUdp broadcasts on UDP port 4405).

Usage:  python udplog.py
Allow Python through Windows Firewall (private networks) when asked.
"""
import socket

PORT = 4405

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", PORT))
print(f"Listening for Wii U logs on UDP {PORT}... (Ctrl+C to stop)")
try:
    while True:
        data, addr = s.recvfrom(4096)
        print(data.decode("utf-8", "replace"), end="" if data.endswith(b"\n") else "\n", flush=True)
except KeyboardInterrupt:
    pass
