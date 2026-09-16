#!/usr/bin/env python3
"""Compare native/Proton hashes and spatial people counts at equal game times."""
import argparse
import json
from pathlib import Path
import re


def samples(data):
    identity = (data / "tpf2_instance.txt").read_text().splitlines()[0].strip()
    if identity not in ("a", "b"):
        raise ValueError(f"Unexpected bridge identity {identity!r} in {data}")
    capture = data / f"tpf2_capture_{identity}.txt"
    result = {}
    last_stamp = None
    for line in capture.read_text(errors="replace").splitlines():
        match = re.match(r"LSHASH t=([0-9.]+) h=(\S+) d=(.*?)(?: o=\S+)?$", line)
        if match:
            stamp = float(match[1])
            # Capture files append across world loads. Keep only the latest
            # world, including saves whose first stamp is already nonzero.
            if last_stamp is not None and (stamp == 0 or stamp < last_stamp):
                result.clear()
            last_stamp = stamp
            people = re.search(r"(?:^|,)n:(\d+)(?:,|$)", match[3])
            sampled_at = re.search(r"(?:^|,)p\d+@([0-9.]+):", match[3])
            result[stamp] = {"hash": match[2], "detail": match[3],
                             "simulation_time": float(sampled_at[1]) if sampled_at else None,
                             "people": int(people[1]) if people else None}
    return identity, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.home() / ".local/share/tpf2mp-lab")
    parser.add_argument("--last", type=int, default=12)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    native_id, native = samples(root / "native/share/tpf2mp/data")
    proton_id, proton = samples(root / "proton/compatdata/pfx/drive_c/users/steamuser/AppData/Local/tpf2mp/data")
    common = sorted(native.keys() & proton.keys())
    rows = []
    for stamp in common:
        a, b = native[stamp], proton[stamp]
        same_time = a["simulation_time"] is not None and a["simulation_time"] == b["simulation_time"]
        rows.append({"stamp": stamp, "hash_equal": a["hash"] == b["hash"],
                     "sample_times_equal": same_time,
                     "people_equal": same_time and a["people"] is not None and a["people"] == b["people"],
                     "native": a, "proton": b})
    report = {"native_identity": native_id, "proton_identity": proton_id, "samples": rows,
              "all_hashes_equal": bool(rows) and all(row["hash_equal"] for row in rows),
              "all_people_equal": bool(rows) and all(row["people_equal"] for row in rows)}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Common samples: {len(rows)}; verdict hashes equal: {report['all_hashes_equal']}; spatial people counts equal at equal times: {report['all_people_equal']}")
    for row in rows[-max(1, args.last):]:
        timing = "" if row['sample_times_equal'] else f" (sample times differ or missing: {row['native']['simulation_time']}/{row['proton']['simulation_time']})"
        print(f"t={row['stamp']:g}: hash {'match' if row['hash_equal'] else 'DIFF'}; spatial people native={row['native']['people']} proton={row['proton']['people']}{timing}")
    if not rows:
        print("No common published samples yet; this is not a passing comparison.")


if __name__ == "__main__":
    main()
