"""Local UI progress uses receiver-confirmed bytes, never queued socket bytes."""


class TransferMeter:
    def __init__(self):
        self.at = None
        self.done = 0
        self.path = None
        self.hint_sent = False

    def event(self, now, done, total, transport, tcp_status, role, peer="", host_port=None):
        done = max(0, min(done, total))
        path = (transport, tcp_status)
        finished = done == total and done != self.done
        if self.at is not None and now - self.at < 1 and path == self.path and not finished:
            return None
        elapsed = now - self.at if self.at is not None else 0
        rate = max(0, done - self.done) / elapsed if elapsed > 0 else 0
        self.at, self.done, self.path = now, done, path
        route = transport
        if transport != "TCP":
            route += " (TCP " + tcp_status + ")"
        who = ("To " + peer[:24]) if role == "send" and peer else "Receiving"
        detail = f"{who} | {route} | {done / 1e6:.1f} / {total / 1e6:.1f} MB | {rate / 1e6:.2f} MB/s"
        hint = ""
        if (transport != "TCP" and tcp_status in ("failed", "unavailable", "interrupted")
                and isinstance(host_port, int) and not isinstance(host_port, bool) and 0 < host_port < 65536):
            hint = (f"Check host router/firewall: allow TCP {host_port}. "
                    "IPv4 may need port forwarding to the host PC.")
        show_hint = bool(hint) and not self.hint_sent
        self.hint_sent |= show_hint
        return {"type": "transfer", "role": role, "peer": peer,
                "pct": int(done * 100 // total) if total else 100,
                "bytes_done": done, "bytes_total": total, "bytes_per_second": rate,
                "transport": transport, "tcp_status": tcp_status, "detail": detail,
                "hint": hint, "show_hint": show_hint}
