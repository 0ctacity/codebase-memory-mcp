#!/usr/bin/env python3
"""Extract the retained pre-v6 optimization baseline from project Markdown."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re


REPOSITORIES = ("tops", "motive", "rvault", "CBM")


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mib_bytes(value: str) -> int:
    return round(float(value) * 1024 * 1024)


def extract(root: pathlib.Path) -> dict[str, dict]:
    baseline_path = root / "docs/benchmarks/zova-pre-v6-baseline.md"
    baseline_text = baseline_path.read_text()
    baseline_source = {
        "path": str(baseline_path.relative_to(root)),
        "sha256": sha256(baseline_path),
    }

    full_pattern = re.compile(
        r"^\| (TOPS|motive|rvault|CBM) \| [0-9.]+ s \| ([0-9.]+) s \|.*?"
        r"\| [0-9.]+ MiB \| ([0-9.]+) MiB \|",
        re.MULTILINE,
    )
    full_rows = full_pattern.findall(baseline_text)
    full = {
        ("tops" if name == "TOPS" else name): (ingest, storage)
        for name, ingest, storage in full_rows[:4]
    }
    latency_pattern = re.compile(
        r"^\| (TOPS|motive|rvault|CBM) \| ([0-9.]+) ms \| ([0-9.]+) ms \|.*?"
        r"\| ([0-9.]+) ms \| ([0-9.]+) ms \|",
        re.MULTILINE,
    )
    latency = {
        ("tops" if name == "TOPS" else name): (graph_compat, graph_single, vector_compat, vector_single)
        for name, graph_compat, graph_single, vector_compat, vector_single
        in latency_pattern.findall(baseline_text)
    }
    if set(full) != set(REPOSITORIES) or set(latency) != set(REPOSITORIES):
        raise ValueError("retained architecture benchmark tables are incomplete or ambiguous")

    isolated = re.search(
        r"\| Database size \| [0-9.]+ MiB \| ([0-9.]+) MiB.*?"
        r"\| Zova publisher \| n/a \| ([0-9.]+) s \|",
        baseline_text,
        re.DOTALL,
    )
    if isolated is None:
        raise ValueError("retained isolated TOPS baseline table is missing")

    result: dict[str, dict] = {}
    for repository in REPOSITORIES:
        ingest, storage = full[repository]
        graph_compat, graph_single, vector_compat, vector_single = latency[repository]
        metrics = {
            "database_bytes": mib_bytes(storage),
            "full_pipeline_ms": float(ingest) * 1000.0,
            "graph_compatibility_p95_ms": float(graph_compat),
            "graph_single_p95_ms": float(graph_single),
            "vector_compatibility_p95_ms": float(vector_compat),
            "vector_single_p95_ms": float(vector_single),
        }
        sources = [baseline_source]
        if repository == "tops":
            metrics.update(
                database_bytes=mib_bytes(isolated.group(1)),
                full_publish_ms=float(isolated.group(2)) * 1000.0,
            )
        result[repository] = {
            "schema_version": 1,
            "baseline_kind": "documented_pre_v6",
            "repository": repository,
            "run_id": f"documented-pre-v6-{repository}",
            "source_documents": sources,
            "metrics": metrics,
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    for repository, baseline in extract(args.root.resolve()).items():
        target = output / f"{repository}.json"
        temporary = target.with_name(f".{target.name}.tmp-{os.getpid()}")
        temporary.write_text(json.dumps(baseline, indent=2, sort_keys=True) + "\n")
        os.replace(temporary, target)
    print(f"PASS: extracted documented Zova baselines to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
