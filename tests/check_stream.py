#!/usr/bin/env python3
"""check_stream.py — validate the live demo's event stream against itself.

The browser demo (demo/bridge.py) draws a page map from two independent streams:
the verifier's probed-page bitmap and the prover's mincore residency bitmap. Both
are base64 bitstreams, and a silent encoding slip would not crash anything — it
would just draw a convincing, wrong picture. So the invariants get a test:

  * every event is valid JSON with the fields the page relies on
  * bitmap lengths match the page count they claim to describe
  * the prover's residency bitmap population equals its own res_pages count
  * the prover counted exactly the probes the verifier asked for
  * warmup + measured challenge counts match the run header

Usage: check_stream.py <verifier-stream.jsonl> <prover-log>
"""
import base64
import json
import sys


def events(path):
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith("@"):
                out.append(json.loads(line[1:]))
    return out


def popcount(b64, npages, fail, what):
    raw = base64.b64decode(b64)
    want = (npages + 7) // 8
    if len(raw) != want:
        fail.append(f"{what}: bitmap is {len(raw)} B, expected {want} B for {npages} pages")
    return sum(bin(x).count("1") for x in raw)


def main():
    vs, pv = events(sys.argv[1]), events(sys.argv[2])
    fail = []

    run = next((e for e in vs if e["t"] == "run"), None)
    end = next((e for e in vs if e["t"] == "end"), None)
    if not run or not end:
        print("FAIL: stream is missing its run header or end summary")
        return 1

    chal = [e for e in vs if e["t"] == "c"]
    for field in ("i", "ph", "us", "nonce", "hash", "exp", "ok"):
        if any(field not in e for e in chal):
            fail.append(f"challenge events missing field {field!r}")

    if len(chal) != run["warmup"] + run["n"]:
        fail.append(f"{len(chal)} challenge events, expected warmup+n = {run['warmup'] + run['n']}")
    measured = sum(1 for e in chal if e["ph"] == "r")
    if measured != end["samples"]:
        fail.append(f"{measured} measured events but end summary says {end['samples']}")

    maps = [e for e in chal if "map" in e]
    if run["map_every"] and not maps:
        fail.append("probe maps were requested but none were emitted")
    for e in maps:
        n = popcount(e["map"], run["pages"], fail, f"probe map i={e['i']}")
        if not 0 < n <= run["pages"]:
            fail.append(f"probe map i={e['i']} marks {n} pages of {run['pages']}")
        if n > run["k"]:
            fail.append(f"probe map i={e['i']} marks {n} pages from only {run['k']} probes")

    if not pv:
        fail.append("prover emitted no trace events")
    for e in pv:
        if "res" in e:
            n = popcount(e["res"], e["pages"], fail, f"residency served={e['served']}")
            if n != e["res_pages"]:
                fail.append(f"served={e['served']}: residency bitmap has {n} bits "
                            f"but res_pages says {e['res_pages']}")
        if e["res_pages"] > e["pages_total"]:
            fail.append(f"served={e['served']}: res_pages {e['res_pages']} exceeds "
                        f"pages_total {e['pages_total']}")

    # The prover must have been asked for exactly what the verifier issued.
    issued = (run["warmup"] + run["n"]) * run["k"]
    if pv and pv[-1]["probes"] != issued:
        fail.append(f"prover counted {pv[-1]['probes']} probes, verifier issued {issued}")

    print(f"  run: {run['pages']} pages, k={run['k']}, {len(chal)} challenges "
          f"({measured} measured), {len(maps)} probe maps, {len(pv)} prover traces")
    if fail:
        for f in fail:
            print(f"  {f}")
        print("FAIL: stream invariants violated")
        return 1
    print("  stream is well-formed and self-consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
