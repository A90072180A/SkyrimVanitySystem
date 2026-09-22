#!/usr/bin/env python3
"""Replay exported calibration geometry through the C++ height kernel.

Usage: python replay_height.py input.zip ./replay-height output-directory
Requires only Python's standard library. Never installs or applies a profile.
"""
from __future__ import annotations
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import zipfile


def main() -> None:
    archive, binary, outdir = Path(sys.argv[1]), Path(sys.argv[2]).resolve(), Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        docs = [json.loads(z.read(n)) for n in z.namelist()
                if n.startswith("geometry/") and n.endswith(".json")]
    donor = next(d for d in docs if d.get("geometryRole") == "stocking"
                 and d.get("stockingLocalCalibration", {}).get("status") == "source-reconstructed-reference")
    anchor = next(d for d in docs if d["identity"]["armor"] == "[AFxII] Converse AS.esp|0000080A")
    def context(d: dict) -> list:
        i = d["identity"]
        return [i["race"], i["sexIndex"], i["actorWeight"], d["actorMorphValues"], d["localTransform"], d["rootParentToSkin"]]
    assert context(anchor) == context(donor), "donor context differs"
    count = len(donor["positions"])
    deltas: dict[str, list[list[float]]] = {}
    for name in ("NoHeel", "Heel"):
        m = next(x for x in donor["nativeMorphMeasurements"] if x["morph"] == name and x["status"] == "present")
        values = [[0.0]*3 for _ in range(count)]
        seen: set[int] = set()
        for row in m["offsets"]:
            i = row["index"]
            assert isinstance(i, int) and 0 <= i < count and i not in seen
            assert len(row["delta"]) == 3 and all(math.isfinite(v) for v in row["delta"])
            seen.add(i)
            values[i] = row["delta"]
        deltas[name] = values
    qn = donor["stockingLocalCalibration"]["rawObservedNoHeel"]
    qh = donor["stockingLocalCalibration"].get("rawObservedHeel", 0.0)
    stock = [[v + (1-qn)*n - qh*h for v, n, h in zip(p, dn, dh)]
             for p, dn, dh in zip(donor["positions"], deltas["NoHeel"], deltas["Heel"])]
    results = []
    for d in docs:
        if d.get("geometryRole") != "foot":
            continue
        name = d["identity"]["armor"]
        if context(d) != context(anchor) :
            results.append({"armor": name, "status": "context-or-topology-rejected"})
            continue
        path = outdir / (hashlib.sha256(name.encode()).hexdigest()[:12] + ".bin")
        with path.open("wb") as f:
            f.write(struct.pack("<I", 0x48544732))
            arrays = [(anchor["positions"], "f"), (d["positions"], "f"), (anchor["triangles"], "I"), (d["triangles"], "I"),
                      (stock, "f"), (deltas["NoHeel"], "f"), (deltas["Heel"], "f")]
            for values, kind in arrays:
                assert 0 < len(values) <= 65535
                f.write(struct.pack("<I", len(values)))
                for row in values:
                    f.write(struct.pack("<3"+kind, *row))
        run = subprocess.run([str(binary), str(path)], check=True, capture_output=True, text=True, timeout=30)
        results.append({"armor": name, **json.loads(run.stdout)})
    report = {"inputSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
              "scope": "offline kernel replay, not an in-game transaction test", "results": results}
    (outdir / "replay-results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main()
