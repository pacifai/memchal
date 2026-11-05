"""Prints "<errors> <challenges>" from a verifier --json summary."""
import json
import sys

with open(sys.argv[1]) as f:
    result = json.load(f)["result"]
print(result["errors"], result["challenges"])
