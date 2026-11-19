#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# ///
"""Downloads a Hugging Face model's safetensors files and writes their weights manifest.

The files are pinned to the repository's current commit, which the script
prints and records in the folder as `revision`. A file already present with
the right size is kept. Works only for models that need no login.

Usage: python3 scripts/fetch_model.py Qwen/Qwen2.5-7B-Instruct models/qwen
Writes: <dir>/*.safetensors, <dir>/revision, and the manifest <dir>/model.weights
"""
import argparse
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

HUB = "https://huggingface.co"
CHUNK = 1 << 24
MANIFEST = "model.weights"


def files(repo):
    """The commit and (name, size) of each safetensors file at the repository's head."""
    with urllib.request.urlopen(f"{HUB}/api/models/{repo}?blobs=true") as r:
        info = json.load(r)
    found = [(s["rfilename"], s["size"]) for s in info["siblings"] if s["rfilename"].endswith(".safetensors")]
    if not found:
        raise ValueError(f"{repo} has no safetensors files")
    return info["sha"], sorted(found)


def download(repo, sha, name, size, dest):
    if dest.exists() and dest.stat().st_size == size:
        print(f"have {name}")
        return
    print(f"downloading {name} ({size / 2**30:.2f} GiB)", flush=True)
    part = dest.with_suffix(".part")
    with urllib.request.urlopen(f"{HUB}/{repo}/resolve/{sha}/{name}") as r, part.open("wb") as f:
        while chunk := r.read(CHUNK):
            f.write(chunk)
    if part.stat().st_size != size:
        raise ValueError(f"{name}: got {part.stat().st_size} bytes, expected {size}")
    part.rename(dest)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("repo", help="the model's repository, such as Qwen/Qwen2.5-7B-Instruct")
    p.add_argument("dir", type=Path, help="the folder to download into")
    args = p.parse_args()

    try:
        sha, found = files(args.repo)
        args.dir.mkdir(parents=True, exist_ok=True)
        print(f"{args.repo} at commit {sha}")
        for name, size in found:
            download(args.repo, sha, name, size, args.dir / name)
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f"fetch_model: {e}")
    (args.dir / "revision").write_text(f"{args.repo} {sha}\n")
    manifest_script = Path(__file__).resolve().parent / "weights_manifest.py"
    made = subprocess.run([sys.executable, str(manifest_script), str(args.dir / MANIFEST),
                           *(str(args.dir / name) for name, _ in found)])
    if made.returncode:
        sys.exit("fetch_model: could not write the weights manifest")


if __name__ == "__main__":
    main()
