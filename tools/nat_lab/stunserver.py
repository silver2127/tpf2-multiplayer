"""Minimal STUN Binding responder for the NAT lab (tools/nat_lab).

Answers an RFC 3489/5389 Binding Request with a Binding Response carrying the
MAPPED-ADDRESS the request arrived from -- exactly what pystun3's stun_test (the
lobby's STUN client, netpunch/observe.py) parses. Listens on every port given
(default 3478 and 3479), so one server gives the two answers observe() compares.

    python stunserver.py [port ...]
"""
import socket, struct, sys, threading


def serve(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    while True:
        data, (ip, p) = s.recvfrom(2048)
        if len(data) < 20 or data[0:2] != b"\x00\x01":
            continue
        tran = data[4:20]
        attr = struct.pack("!HHBBH", 0x0001, 8, 0, 1, p) + socket.inet_aton(ip)
        s.sendto(struct.pack("!HH", 0x0101, len(attr)) + tran + attr, (ip, p))


ports = [int(a) for a in sys.argv[1:]] or [3478, 3479]
for port in ports:
    threading.Thread(target=serve, args=(port,), daemon=True).start()
print("stun on udp " + ", ".join(map(str, ports)), flush=True)
threading.Event().wait()
