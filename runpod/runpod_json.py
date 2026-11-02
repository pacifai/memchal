#!/usr/bin/env python3
"""JSON handling for gpu_demo_up.sh and gpu_demo_down.sh.

The shell scripts talk to the RunPod API with curl; this does the parts that
need a JSON parser.

  get KEY.PATH            print a dotted field of the JSON on stdin, or nothing
  catalog GPU_ID CLOUD    from the GPU catalog on stdin, print NAME|PRICE|STOCK,
                          or ERR|reason when the card is not listed
  pod-body IMAGE GPU_ID DISK_GB CLOUD BOOT_CMD
                          print the JSON body that rents one pod
"""
import json
import sys


def get(path):
    try:
        d = json.load(sys.stdin)
    except ValueError:
        return
    for k in path.split("."):
        if d is None:
            break
        d = d.get(k) if isinstance(d, dict) else None
    print("" if d is None else d)


def catalog(gpu, cloud):
    try:
        gpus = json.load(sys.stdin).get("gpus", [])
    except ValueError:
        print("ERR|the catalog did not answer with JSON")
        return
    g = next((x for x in gpus if x.get("id") == gpu), None)
    if g is None:
        # The catalog ignores a gpuTypeIds filter and returns every card, so an
        # id missing here is a typo in --gpu. Suggest ids sharing its last word.
        word = gpu.split()[-1].lower()
        near = [x["id"] for x in gpus if word in x.get("id", "").lower()][:5]
        hint = (" - did you mean: " + ", ".join(near)) if near else ""
        print("ERR|no card with id " + repr(gpu) + hint)
        return
    price = (g.get("price") or {}).get(cloud.lower())
    print("|".join([g.get("name", gpu),
                    "?" if price is None else str(price),
                    g.get("availability", "?")]))


def pod_body(image, gpu, disk, cloud, boot):
    print(json.dumps({"name": "memchal-demo", "image": image, "gpu": {"id": gpu},
                      "disk": int(disk), "ports": ["22/tcp"], "cloud": cloud,
                      "startSsh": True, "entrypoint": ["bash", "-lc"], "cmd": [boot]}))


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    {"get": get, "catalog": catalog, "pod-body": pod_body}[cmd](*args)
