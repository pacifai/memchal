"""Print "<errors> <samples>" from a verifier --json summary, for run_tests.sh."""
import json
import sys

with open(sys.argv[1]) as f:
    result = json.load(f)["result"]
print(result["errors"], result["samples"])
