#!/usr/bin/env python3
"""Print one row of run_fold_ab.sh's table: the verifier's p50 and errors, and
the prover's median kernel, copy, and fold times from its trace.

Usage: fold_row.py VERIFIER_JSON PROVER_LOG FOLD LANES K
"""
import json,sys,statistics,re
d=json.load(open(sys.argv[1]))['result']
p50=d['percentiles_us']['p50']['value']; err=d['errors']
ks=[];cs=[];fs=[]
for line in open(sys.argv[2]):
    if '"t":"g"' in line:
        e=json.loads(line[1:])
        ks.append(e['kernel_ms']); cs.append(e['copy_ms']); fs.append(e['fold_ms'])
med=lambda a: statistics.median(a) if a else float('nan')
print('%-8s %-10s %10s %10.1f %10.4f %10.4f %10.4f   errors=%d' %
      (sys.argv[3],sys.argv[4],sys.argv[5],p50,med(ks),med(cs),med(fs),err))
