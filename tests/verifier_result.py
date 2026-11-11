"""Prints "<errors> <checked>" from a verifier --json summary."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import mc_stats as ms

result = ms.load_json(sys.argv[1], ms.VERIFIER_SCHEMA)["result"]
print(result["errors"], result["checked"])
