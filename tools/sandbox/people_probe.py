#!/usr/bin/env python3
"""Queue read-only person snapshots through existing EVAL, or compare exact times."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys
import uuid

ACTORS = ("native", "proton")
DATA = {
    "native": "share/tpf2mp/data",
    "proton": "compatdata/pfx/drive_c/users/steamuser/AppData/Local/tpf2mp/data",
}


def targets(value):
    result = sorted(set(int(item.strip()) for item in value.split(",")))
    if not result or result[0] <= 0 or result[-1] > 2**31 - 1 or len(result) > 32:
        raise ValueError("Use 1–32 positive integer game times, separated by commas")
    return result


def render_probe(times, request):
    if len(request) != 12 or any(c not in "0123456789abcdef" for c in request):
        raise ValueError("Invalid probe request identifier")
    source = Path(__file__).with_name("people_snapshot.lua").read_text()
    return (source.replace("@TARGETS@", "{" + ",".join(map(str, times)) + "}")
            .replace("@PROBE_ID@", json.dumps(request))
            .replace("@PREFIX@", json.dumps("people_probe_" + request)))


def eval_line(source):
    # JSON's escapes here are Lua-compatible: source is ASCII, with no control
    # characters beyond newlines/tabs. Keep the entire EVAL on one physical line.
    return "EVAL return assert((loadstring or load)(" + json.dumps(source) + "))()\n"


def read_request(output, root):
    request = json.loads((output / "request.json").read_text())
    if request.get("schema") != 1 or request.get("root") != str(root):
        raise ValueError("Report directory belongs to a different lab or schema")
    render_probe(request["targets"], request["id"])
    return request


def arm(root, output, times, dry_run=False):
    if not times:
        raise ValueError("arm requires --at, for example --at 60,72,144,300")
    if (output / "request.json").exists():
        raise ValueError("This report directory already contains a request; use a new --output")
    injects = {}
    for actor in ACTORS:
        data = root / actor / DATA[actor]
        identity = (data / "tpf2_instance.txt").read_text().splitlines()[0].strip()
        if identity not in ("a", "b"):
            raise ValueError(f"Unexpected {actor} identity {identity!r}")
        inject = data / f"lockstep_inject_{identity}.txt"
        if not inject.is_file() or not (root / actor / "game").is_dir():
            raise ValueError(f"Missing initialized {actor} lab/inject file")
        injects[actor] = inject
    request = {"schema": 1, "id": uuid.uuid4().hex[:12], "root": str(root), "targets": times}
    source = render_probe(times, request["id"])
    line = eval_line(source)
    if dry_run:
        print(json.dumps({"request": request, "injects": {k: str(v) for k, v in injects.items()}}, indent=2))
        print(source)
        return
    output.mkdir(parents=True, exist_ok=True)
    with (output / "request.json").open("x") as stream:
        json.dump(request, stream, indent=2)
        stream.write("\n")
    (output / "probe.lua").write_text(source)
    queued = []
    try:
        for actor, inject in injects.items():
            with inject.open("a") as stream:
                stream.write(line)
            queued.append(actor)
    finally:
        (output / "queued.json").write_text(json.dumps(queued) + "\n")
    print(f"Queued {request['id']} on both actors for {times}; confirm PEOPLE_PROBE armed in each stdout.txt.")
    print(f"Request and diagnostic source: {output}")


def read_snapshot(path, at, request):
    data = path.read_bytes()
    rows = [json.loads(line) for line in data.splitlines()]
    if not rows or rows[0].get("kind") != "meta":
        raise ValueError(f"Missing snapshot header: {path}")
    meta, *persons = rows
    if meta.get("request") != request or any(meta.get(key) != at for key in ("target", "time", "timeAfter")):
        raise ValueError(f"Snapshot did not capture exact requested time {at}: {path}")
    ids = [row["id"] for row in persons]
    if len(ids) != len(set(ids)) or len(ids) != meta["entityCount"]:
        raise ValueError(f"Incomplete/duplicate person records: {path}")
    if any(row.get("kind") != "person" or "lastDestinationUpdate" not in row for row in persons):
        raise ValueError(f"Incomplete person fields: {path}")
    if sum(row["spatial"] for row in persons) != meta["spatialCount"]:
        raise ValueError(f"Spatial query and person membership disagree: {path}")
    actual_states = Counter(name for row in persons for name, present in row["states"].items() if present)
    if any(actual_states[name] != count for name, count in meta["stateCounts"].items()):
        raise ValueError(f"State counts disagree with person records: {path}")
    return meta, dict(zip(ids, persons)), data


def compare_pair(a, b, at):
    ma, pa, _ = a
    mb, pb, _ = b
    field_differences, destination_differences = Counter(), Counter()
    samples = []
    for entity in sorted(pa.keys() & pb.keys()):
        x, y = pa[entity], pb[entity]
        xe, ye = x["entity"], y["entity"]
        for field in xe.keys() | ye.keys():
            if xe.get(field) != ye.get(field):
                field_differences[field] += 1
        for index, name in (("1", "residence"), ("2", "shopping"), ("3", "work")):
            if xe.get("destinations", {}).get(index) != ye.get("destinations", {}).get(index):
                destination_differences[name] += 1
        for field in ("lastDestinationUpdate", "spatial", "states"):
            if x.get(field) != y.get(field):
                field_differences[field] += 1
        if x != y and len(samples) < 10:
            samples.append({"id": entity, "native": x, "proton": y})
    counts = {}
    for actor, meta, people in (("native", ma, pa), ("proton", mb, pb)):
        counts[actor] = {key: meta[key] for key in ("globalCount", "entityCount", "spatialCount", "stateCounts")}
        counts[actor]["spatialExactlyMoving"] = all(
            row["spatial"] == bool(row["states"].get("SIM_ENTITY_MOVING")) for row in people.values())
    return {"target": at, "counts": counts,
            "globalCountsEqual": ma["globalCount"] == mb["globalCount"],
            "personIdsEqual": pa.keys() == pb.keys(),
            "onlyNativeIds": sorted(pa.keys() - pb.keys()), "onlyProtonIds": sorted(pb.keys() - pa.keys()),
            "personCapacityCallbackOrderEqual": ma["personCapacityOrder"] == mb["personCapacityOrder"],
            "differentFields": dict(field_differences), "differentDestinations": dict(destination_differences),
            "samples": samples,
            "equal": pa == pb and ma["globalCount"] == mb["globalCount"]}


def compare(root, output, times=None):
    request = read_request(output, root)
    times = times or request["targets"]
    if not set(times) <= set(request["targets"]):
        raise ValueError("Requested comparison time was not armed in this request")
    snapshots, rows = {}, []
    # Validate all requested inputs before publishing any comparison report.
    for at in times:
        for actor in ACTORS:
            name = f"people_probe_{request['id']}_{at}.jsonl"
            snapshots[actor, at] = read_snapshot(root / actor / "game" / name, at, request["id"])
        rows.append(compare_pair(snapshots["native", at], snapshots["proton", at], at))
    archives = []
    for (actor, at), (_, _, data) in snapshots.items():
        path = output / f"{actor}_{at}.jsonl"
        if path.exists() and path.read_bytes() != data:
            raise ValueError(f"Existing archived snapshot differs: {path}")
        archives.append((path, data))
    for path, data in archives:
        if not path.exists():
            path.write_bytes(data)
    report = {"schema": 1, "request": request, "snapshots": rows,
              "allEqual": all(row["equal"] for row in rows),
              "sha256": {path.name: hashlib.sha256(data).hexdigest() for path, data in archives}}
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for row in rows:
        a, b = (row["counts"][actor] for actor in ACTORS)
        print(f"t={row['target']}: global {a['globalCount']}/{b['globalCount']}; "
              f"spatial {a['spatialCount']}/{b['spatialCount']}; "
              f"destination differences {row['differentDestinations']}; equal={row['equal']}")
    print(f"Archived snapshots and report: {output}")
    return report["allEqual"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("arm", "compare"))
    parser.add_argument("--root", type=Path, default=Path.home() / ".local/share/tpf2mp-lab")
    parser.add_argument("--at", help="Comma-separated integer game times; compare defaults to all armed times")
    parser.add_argument("--output", type=Path, required=True, help="Request/report directory outside the lab")
    parser.add_argument("--dry-run", action="store_true", help="Print arm request/source; write nothing")
    args = parser.parse_args()
    try:
        root, output = args.root.expanduser().resolve(), args.output.expanduser().resolve()
        if output == root or output.is_relative_to(root):
            raise ValueError("--output must be outside the lab")
        times = targets(args.at) if args.at else None
        if args.command == "arm":
            arm(root, output, times, args.dry_run)
        elif args.dry_run:
            raise ValueError("--dry-run is only valid with arm")
        else:
            return 0 if compare(root, output, times) else 1
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        print(f"people_probe: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
