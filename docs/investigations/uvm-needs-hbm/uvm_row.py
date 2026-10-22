#!/usr/bin/env python3
"""Print one row of run_uvm_sweep.sh's table.

Usage: uvm_row.py VERIFIER_JSON HOG_MIB FREE_MIB HONEST_P50_US
"""
import json
import sys

path, hog, free, base = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
d = json.load(open(path))["result"]
p = d["percentiles_us"]["p50"]["value"]
print("%10s %10s %12.1f %9.2fx %8d" % (hog, free, p, p / base, d["errors"]))
