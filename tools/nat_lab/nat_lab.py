"""NAT lab: prove the lobby's hole punching across two real NATs, on one PC (Docker).

A simulated internet with two home networks, each behind its own Linux NAT
router (iptables MASQUERADE, no UPnP, no forwarded ports, unsolicited inbound
dropped), a STUN server and the master server on the "internet", and one "PC"
per home running the real netpunch/lobby.py:

    home_a 192.168.10.0/24          internet 10.99.0.0/24          home_b 192.168.20.0/24
    pc_a (host) -- router_a  ==  stun 10.99.0.10, master 10.99.0.11  ==  router_b -- pc_b (joiner)

Scenarios (expected result in brackets):
  no_knock        knocks off -- the old lobby: the host's port is closed    [FAIL]
  port_open       the host's router forwards 29471 (what UPnP does)         [CONNECT, no knock]
  punch           knocks on, both routers port-preserving (cone-like)      [CONNECT, knocked]
  punch_password  as punch, with a lobby password                          [CONNECT, knocked]
  symmetric_both  both routers randomise ports per destination             [FAIL]

The routers drop unsolicited WAN packets in INPUT, as a home router's firewall
does. A router that answers them without a firewall keeps Linux conntrack state
for each, which pins the host's public port to a dead flow and defeats punching.
The lab showed this on its second run; real home routers do not behave that way.

    python tools/nat_lab/nat_lab.py [--keep] [--only name,name]

Needs Docker Desktop running. Builds the image tpf2mp-natlab:1 on first use,
mounts netpunch/ read-only, and removes every natlab_* container and network
at the end unless --keep.
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
NP = os.path.join(REPO, "netpunch")
IMAGE = "tpf2mp-natlab:1"
P = "natlab_"
STUN_IP, MASTER_IP = "10.99.0.10", "10.99.0.11"
ROUTERS = {"a": ("10.99.0.20", "192.168.10.2", "192.168.10.10", "192.168.10.0/24"),
           "b": ("10.99.0.30", "192.168.20.2", "192.168.20.10", "192.168.20.0/24")}
MASTER_URL = f"http://{MASTER_IP}:8471"


def run(*args, check=True, quiet=False):
    r = subprocess.run(["docker", *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"docker {' '.join(args)} failed: {r.stderr.strip() or r.stdout.strip()}")
    if not quiet and r.stdout.strip():
        pass
    return r


def sh(container, cmd, check=True):
    return run("exec", P + container, "sh", "-c", cmd, check=check)


def cleanup():
    names = run("ps", "-aq", "--filter", f"name={P}", check=False).stdout.split()
    if names:
        run("rm", "-f", *names, check=False)
    for net in ("inet", "home_a", "home_b"):
        run("network", "rm", P + net, check=False)


def build_image():
    if run("image", "inspect", IMAGE, check=False).returncode == 0:
        return
    print("building the lab image (first run only)...", flush=True)
    r = subprocess.run(["docker", "build", "-t", IMAGE, HERE], text=True)
    if r.returncode != 0:
        raise RuntimeError("docker build failed")


def iface_for(container, ip):
    out = sh(container, "ip -o -4 addr show").stdout
    for line in out.splitlines():
        parts = line.split()
        if any(p.startswith(ip + "/") for p in parts):
            return parts[1].split("@")[0]
    raise RuntimeError(f"{container}: no interface with {ip}")


def set_nat(side, symmetric, forward_port=False):
    inet_ip, home_ip, pc_ip, _ = ROUTERS[side]
    r = "router_" + side
    wan, lan = iface_for(r, inet_ip), iface_for(r, home_ip)
    rnd = " --random-fully" if symmetric else ""
    # forward_port: what UPnP (or a manual forward) does -- the lobby port is open
    fwd = (f"iptables -t nat -A PREROUTING -i {wan} -p udp --dport 29471 -j DNAT --to-destination {pc_ip}:29471 && "
           f"iptables -A FORWARD -i {wan} -o {lan} -p udp -d {pc_ip} --dport 29471 -j ACCEPT && ") if forward_port else ""
    # INPUT drops unsolicited WAN packets like a home router does. Without it Linux
    # keeps a conntrack entry for every unanswered packet addressed to the router
    # itself, and the joiner's early HELLOs pinned the host's public port 29471 to
    # a dead flow: the host's punch was then SNATed to another port and the two
    # never met (second run, 2026-09-11). A packet dropped in filter is never
    # confirmed, so it leaves no state.
    sh(r, "iptables -t nat -F && iptables -F FORWARD && iptables -F INPUT && "
          f"iptables -A INPUT -i {wan} -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT && "
          f"iptables -A INPUT -i {wan} -j DROP && "
          f"iptables -t nat -A POSTROUTING -o {wan} -j MASQUERADE{rnd} && " + fwd +
          f"iptables -A FORWARD -i {lan} -o {wan} -j ACCEPT && "
          f"iptables -A FORWARD -i {wan} -o {lan} -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT && "
          f"iptables -A FORWARD -i {wan} -o {lan} -j DROP && "
          "(conntrack -F >/dev/null 2>&1 || true)")


def setup():
    cleanup()
    build_image()
    # NOT --internal: Docker's rules for an internal network drop every bridged frame
    # whose destination is outside the subnet -- including a PC's traffic to its own
    # router -- so nothing reached the lab's STUN or master (first run, 2026-09-11).
    # Isolation is kept anyway: each PC's default route is its lab router, and Docker
    # isolates different bridge networks from each other.
    # NO DOCKER MASQUERADE: the Docker VM filters bridged frames (bridge-nf-call-
    # iptables=1), so its "-s <subnet> ! -o <bridge> -j MASQUERADE" rule rewrote a
    # port-forwarded packet from the joiner to 192.168.10.1 on its way from router_a
    # to pc_a; the host answered Docker instead of its router and the forward never
    # connected (port_open scenario, 2026-09-11). Nothing in the lab needs the real
    # internet.
    nomasq = ("-o", "com.docker.network.bridge.enable_ip_masquerade=false")
    run("network", "create", "--subnet", "10.99.0.0/24", *nomasq, P + "inet")
    run("network", "create", "--subnet", "192.168.10.0/24", *nomasq, P + "home_a")
    run("network", "create", "--subnet", "192.168.20.0/24", *nomasq, P + "home_b")
    mount = f"type=bind,source={NP},target=/np,readonly"
    labmount = f"type=bind,source={HERE},target=/lab,readonly"
    run("run", "-d", "--name", P + "stun", "--network", P + "inet", "--ip", STUN_IP,
        "--mount", labmount, IMAGE, "python", "/lab/stunserver.py", "3478", "3479")
    run("run", "-d", "--name", P + "master", "--network", P + "inet", "--ip", MASTER_IP,
        "--mount", mount, IMAGE, "python", "/np/masterserver.py", "8471", "--bind", "0.0.0.0")
    for side, (inet_ip, home_ip, pc_ip, subnet) in ROUTERS.items():
        r = P + "router_" + side
        run("run", "-d", "--name", r, "--cap-add", "NET_ADMIN", "--sysctl", "net.ipv4.ip_forward=1",
            "--network", P + "inet", "--ip", inet_ip, IMAGE, "sleep", "infinity")
        run("network", "connect", "--ip", home_ip, P + "home_" + side, r)
        pc = P + "pc_" + side
        run("run", "-d", "--name", pc, "--cap-add", "NET_ADMIN", "--network", P + "home_" + side,
            "--ip", pc_ip, "--mount", mount,
            "-e", f"TPF2MP_STUN={STUN_IP}:3478,{STUN_IP}:3479", IMAGE, "sleep", "infinity")
        sh("pc_" + side, f"ip route replace default via {home_ip}")
    time.sleep(1.0)


def reset_pcs():
    for side in ("a", "b"):
        sh("pc_" + side, "pkill -f lobby.py; rm -rf /tmp/io /tmp/lobby.log; mkdir -p /tmp/io", check=False)


def read(container, path):
    return sh(container, f"cat {path} 2>/dev/null", check=False).stdout


def wait_for(container, path, needles, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = read(container, path)
        for n in needles:
            if n in text:
                return n, text
        time.sleep(0.5)
    return None, read(container, path)


def scenario(name, knock, symmetric=False, password="", forward_port=False):
    for side in ("a", "b"):
        set_nat(side, symmetric, forward_port=forward_port and side == "a")
    reset_pcs()
    rv = MASTER_URL if knock else "off"
    pw = f" --password {password}" if password else ""
    sh("pc_a", f"cd /np && nohup python lobby.py host --name alice --local-port 29471 --io-dir /tmp/io "
               f"--rendezvous {rv}{pw} > /tmp/lobby.log 2>&1 &")
    found, host_log = wait_for("pc_a", "/tmp/lobby.log", ["CODE="], 30)
    if not found:
        return False, "the host printed no CODE", host_log, ""
    code = host_log.split("CODE=", 1)[1].split()[0]
    time.sleep(1.5)                                   # let the host's first poll happen
    sh("pc_b", f"cd /np && nohup python lobby.py join {code} --name bob --local-port 0 --io-dir /tmp/io "
               f"--timeout 25 --rendezvous {rv}{pw} > /tmp/lobby.log 2>&1 &")
    found, join_log = wait_for("pc_b", "/tmp/lobby.log", ["connected to host", "FAILED to reach host"], 45)
    connected = found == "connected to host"
    if connected:
        # the lobby handshake on top of the punched link: the host names the joiner
        f2, host_log = wait_for("pc_a", "/tmp/lobby.log", ["bob"], 15)
        if not f2:
            return False, "the punched link came up but the host never saw bob join", read("pc_a", "/tmp/lobby.log"), join_log
    host_log = read("pc_a", "/tmp/lobby.log")
    return connected, ("connected" if connected else (found or "no result in 45 s")), host_log, join_log


# (name, scenario kwargs, expect a connection, expect the joiner to knock: True/False/None = either)
SCENARIOS = [
    ("no_knock", dict(knock=False), False, False),
    ("port_open", dict(knock=True, forward_port=True), True, False),   # UPnP worked: direct, the master never hears of it
    ("punch", dict(knock=True), True, True),                           # no open port: the fallback punches
    ("punch_password", dict(knock=True, password="hunter2"), True, True),
    ("symmetric_both", dict(knock=True, symmetric=True), False, None),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the lab running afterwards")
    ap.add_argument("--only", default="", help="comma-separated scenario names")
    a = ap.parse_args()
    only = {s for s in a.only.split(",") if s}
    if run("info", check=False).returncode != 0:
        print("Docker is not running -- start Docker Desktop first")
        return 2
    results = []
    try:
        setup()
        for name, kw, expect, expect_knock in SCENARIOS:
            if only and name not in only:
                continue
            print(f"--- {name} ...", flush=True)
            ok, why, host_log, join_log = scenario(name, **kw)
            knocked = "knocked at the master" in join_log
            good = ok == expect and (expect_knock is None or knocked == expect_knock)
            results.append((name, expect, ok, good))
            print(f"{'ok  ' if good else 'FAIL'} {name}: {'connected' if ok else 'no connection'} "
                  f"(expected {'connect' if expect else 'no connection'}), knocked={knocked}"
                  f"{'' if expect_knock is None else ' (expected ' + str(expect_knock) + ')'} -- {why}", flush=True)
            rv_lines = [l for l in (host_log + join_log).splitlines() if "[rendezvous]" in l or "[race]" in l]
            for l in rv_lines[:8]:
                print("       " + l[:180])
            if not good:
                print("       host log tail:\n         " + "\n         ".join(host_log.splitlines()[-12:]))
                print("       join log tail:\n         " + "\n         ".join(join_log.splitlines()[-12:]))
    finally:
        if not a.keep:
            cleanup()
    bad = [r for r in results if not r[3]]
    print("ALL AS EXPECTED" if results and not bad else "UNEXPECTED: " + ", ".join(r[0] for r in bad))
    return 0 if results and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
