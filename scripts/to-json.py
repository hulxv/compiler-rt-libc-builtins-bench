#!/usr/bin/env python3
"""Convert run-bench measurements.tsv into github-action-benchmark JSON.

Input rows (tab-separated): metric  builtin  category  mode  unit  value
Output: a JSON array of {name, unit, value} objects for the
        "customSmallerIsBetter" tool (lower = better for all our metrics).
"""
import json
import sys


def main(path):
    points = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            metric, builtin, category, mode, unit, value = line.split("\t")
            if metric == "size":
                name = f"{builtin} size [{mode}]"
            elif metric == "time":
                name = f"{builtin} ns/op {category} [{mode}]"
            elif metric == "instr":
                name = f"{builtin} instr/op {category} [{mode}]"
            else:
                continue
            points.append({"name": name, "unit": unit, "value": float(value)})
    json.dump(points, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "measurements.tsv")
