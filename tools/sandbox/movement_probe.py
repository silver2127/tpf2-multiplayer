#!/usr/bin/env python3
"""Prepare (never inject) exact-time movement diagnostics, or compare snapshots."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys
import uuid

from people_probe import eval_line, targets


def render_probe(times, request):
    if len(request) != 12 or any(c not in "0123456789abcdef" for c in request):
        raise ValueError("Invalid probe request identifier")
    source = Path(__file__).with_name("movement_snapshot.lua").read_text()
    return (source.replace("@TARGETS@", "{" + ",".join(map(str, times)) + "}")
            .replace("@PROBE_ID@", json.dumps(request))
            .replace("@PREFIX@", json.dumps("movement_probe_" + request)))


def prepare(root, output, times):
    if not times:
        raise ValueError("prepare requires --at, for example --at 660,720")
    if output.exists():
        raise ValueError("Use a new output directory for each diagnostic")
    request = {"schema": 1, "kind": "movement", "id": uuid.uuid4().hex[:12],
               "root": str(root), "targets": times}
    source = render_probe(times, request["id"])
    output.mkdir(parents=True)
    (output / "request.json").write_text(json.dumps(request, indent=2) + "\n")
    (output / "probe.lua").write_text(source)
    (output / "eval.txt").write_text(eval_line(source))
    print(f"Prepared {request['id']} for {times}: {output}")
    print("Not queued. Review probe.lua; inject eval.txt only after the current diagnostic has restored.")


def read_snapshot(path, at, request):
    data = path.read_bytes()
    rows = [json.loads(line) for line in data.splitlines()]
    if not rows or rows[0].get("kind") != "meta" or rows[0].get("schema") not in (1, 2, 3, 4):
        raise ValueError(f"Invalid movement header: {path}")
    meta, *persons = rows
    if meta.get("request") != request or any(meta.get(k) != at for k in ("target", "time", "timeAfter")):
        raise ValueError(f"Snapshot did not capture exact requested time {at}: {path}")
    ids = [row["id"] for row in persons]
    if len(ids) != len(set(ids)) or len(ids) != meta["entityCount"]:
        raise ValueError(f"Incomplete or duplicate person records: {path}")
    if any(row.get("kind") != "person" or not {"moving", "simMoving", "movePath", "worldTransforms"} <= row.keys()
           for row in persons):
        raise ValueError(f"Missing movement fields: {path}")
    if sum(row["moving"] for row in persons) != meta["movingCount"]:
        raise ValueError(f"Moving count disagrees with records: {path}")
    if sum(row["movePath"]["present"] for row in persons) != meta["movePathCount"]:
        raise ValueError(f"Path count disagrees with records: {path}")
    if meta["schema"] >= 2:
        if any("walker" not in row for row in persons) or sum(row["walker"]["present"] for row in persons) != meta["walkerCount"]:
            raise ValueError(f"Walker count disagrees with records: {path}")
    if meta["schema"] >= 3 and meta["simulationCaptureComplete"]:
        if "globalCount" not in meta or any(not {"entity", "lastDestinationUpdate"} <= row.keys() for row in persons):
            raise ValueError(f"Missing person state fields: {path}")
    if meta["schema"] >= 4 and any("atBuilding" not in row for row in persons):
        raise ValueError(f"Missing building waiting state: {path}")
    return meta, dict(zip(ids, persons)), data


def compare_pair(a, b, at):
    ma, pa, _ = a
    mb, pb, _ = b
    differences = Counter()
    samples = []
    for entity in sorted(pa.keys() & pb.keys()):
        for field in ("entity", "lastDestinationUpdate", "atBuilding", "moving", "simMoving", "movePath", "walker", "worldTransforms"):
            if pa[entity].get(field) != pb[entity].get(field):
                differences[field] += 1
                if len(samples) < 10:
                    samples.append({"id": entity, "field": field,
                                    "native": pa[entity].get(field), "proton": pb[entity].get(field)})
    simulation_complete = all(m["simulationCaptureComplete"] and m["errors"]["simulation"] == 0 for m in (ma, mb))
    geometry_complete = all(m["geometryCaptureComplete"] and m["errors"]["geometry"] == 0 for m in (ma, mb))
    ids_equal = pa.keys() == pb.keys()
    global_count_equal = ma.get("globalCount") == mb.get("globalCount")
    if not global_count_equal:
        differences["globalCount"] += 1
    simulation_equal = simulation_complete and ids_equal and global_count_equal and ma["schema"] == mb["schema"] and not any(differences[f] for f in ("entity", "lastDestinationUpdate", "atBuilding", "moving", "simMoving", "movePath", "walker"))
    geometry_equal = geometry_complete and ids_equal and not differences["worldTransforms"]
    return {"target": at, "personIdsEqual": ids_equal, "globalCountEqual": global_count_equal,
            "onlyNativeIds": sorted(pa.keys() - pb.keys()), "onlyProtonIds": sorted(pb.keys() - pa.keys()),
            "simulationCaptureComplete": simulation_complete, "geometryCaptureComplete": geometry_complete,
            "simulationEqual": simulation_equal, "worldTransformsEqual": geometry_equal,
            "allEqual": simulation_equal and geometry_equal, "differentFields": dict(differences),
            "samples": samples, "metadata": {"native": ma, "proton": mb}}


def compare(root, output, times=None):
    request = json.loads((output / "request.json").read_text())
    if request.get("schema") != 1 or request.get("kind") != "movement" or request.get("root") != str(root):
        raise ValueError("Request belongs to a different lab or schema")
    render_probe(request["targets"], request["id"])
    times = times or request["targets"]
    if not set(times) <= set(request["targets"]):
        raise ValueError("Comparison time was not prepared in this request")
    archives, reports = [], []
    for at in times:
        pair = []
        for actor in ("native", "proton"):
            snapshot = read_snapshot(root / actor / "game" / f"movement_probe_{request['id']}_{at}.jsonl", at, request["id"])
            pair.append(snapshot)
            archives.append((output / f"{actor}_{at}.jsonl", snapshot[2]))
        reports.append(compare_pair(*pair, at))
    for path, data in archives:
        if path.exists() and path.read_bytes() != data:
            raise ValueError(f"Archived snapshot differs: {path}")
    for path, data in archives:
        if not path.exists():
            path.write_bytes(data)
    report = {"request": request, "snapshots": reports,
              "allSimulationEqual": all(r["simulationEqual"] for r in reports),
              "allWorldTransformsEqual": all(r["worldTransformsEqual"] for r in reports),
              "sha256": {p.name: hashlib.sha256(data).hexdigest() for p, data in archives}}
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for row in reports:
        print(f"t={row['target']}: movement={row['simulationEqual']}; "
              f"transforms={row['worldTransformsEqual']}; differences={row['differentFields']}; "
              f"complete={row['simulationCaptureComplete']}/{row['geometryCaptureComplete']}")
    return report["allSimulationEqual"] and report["allWorldTransformsEqual"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "compare"))
    parser.add_argument("--root", type=Path, default=Path.home() / ".local/share/tpf2mp-lab")
    parser.add_argument("--at")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        root, output = args.root.expanduser().resolve(), args.output.expanduser().resolve()
        if output == root or output.is_relative_to(root):
            raise ValueError("--output must be outside the lab")
        times = targets(args.at) if args.at else None
        if args.command == "prepare":
            prepare(root, output, times)
        else:
            return 0 if compare(root, output, times) else 1
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        print(f"movement_probe: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
