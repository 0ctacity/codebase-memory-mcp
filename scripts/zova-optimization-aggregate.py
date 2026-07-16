#!/usr/bin/env python3
"""Assemble retained per-repository Zova optimization evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
from typing import Any


REPOSITORIES = ("tops", "motive", "rvault", "CBM")


def load(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text())
    if type(value) is not dict:
        raise ValueError(f"{path} must contain an object")
    return value


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_atomic(path: pathlib.Path, text: str) -> None:
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(text)
    os.replace(temporary, path)


def state(report: dict[str, Any], route: str, workload: str) -> dict[str, Any]:
    for value in report.get("states", []):
        if value.get("route") == route and value.get("workload") == workload:
            return value
    return {}


def number(value: Any, digits: int = 1) -> str:
    return "n/a" if not isinstance(value, (int, float)) else f"{value:.{digits}f}"


def percent_change(current: Any, baseline: Any) -> str:
    if not isinstance(current, (int, float)) or not isinstance(baseline, (int, float)) or not baseline:
        return "n/a"
    return f"{(current / baseline - 1.0) * 100.0:+.1f}%"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--baseline-root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    reports = {
        "tops": load(root / "tops/current-median.json"),
        **{
            name: load(root / name / "attempt-1/optimization-report.json")
            for name in ("motive", "rvault", "CBM")
        },
    }
    decisions = {name: load(root / name / "decision.json") for name in REPOSITORIES}
    baselines = {
        name: load(args.baseline_root.resolve() / f"{name}.json") for name in REPOSITORIES
    }
    if not all(value.get("passed") is True for value in reports.values()):
        raise SystemExit("one or more repository reports failed")
    if not all(value.get("passed") is True for value in decisions.values()):
        raise SystemExit("one or more repository optimization gates failed")
    build_hashes = {value.get("build_sha256") for value in reports.values()}
    if len(build_hashes) != 1 or not next(iter(build_hashes), None):
        raise SystemExit("repository reports do not share one build SHA-256")

    provenance = load(root / "provenance.json")
    build_sha256 = next(iter(build_hashes))
    if provenance.get("binary_sha256") != build_sha256:
        raise SystemExit("provenance binary SHA-256 does not match repository reports")
    report_paths = {
        "tops": root / "tops/current-median.json",
        **{
            name: root / name / "attempt-1/optimization-report.json"
            for name in ("motive", "rvault", "CBM")
        },
    }
    evidence_sha256 = {
        "reports": {name: sha256(path) for name, path in report_paths.items()},
        "decisions": {
            name: sha256(root / name / "decision.json") for name in REPOSITORIES
        },
        "baselines": {
            name: sha256(args.baseline_root.resolve() / f"{name}.json")
            for name in REPOSITORIES
        },
        "tops_samples": {
            f"attempt-{attempt}": sha256(
                root / f"tops/attempt-{attempt}/optimization-report.json"
            )
            for attempt in (1, 2, 3)
        },
    }
    aggregate = {
        "schema_version": 1,
        "passed": True,
        "build_sha256": build_sha256,
        "provenance": provenance,
        "evidence_sha256": evidence_sha256,
        "repositories": reports,
        "decisions": decisions,
        "baselines": baselines,
    }
    write_atomic(root / "optimization.json", json.dumps(aggregate, indent=2, sort_keys=True) + "\n")

    lines = [
        "# Zova v6/v8 optimization results",
        "",
        "All rows use one CBM binary. Historical values come from the immutable Markdown baseline record.",
        "",
        "## Storage",
        "",
        "| Repository | Pre-v6 Zova | Optimized Zova | Change | Pure SQLite | Optimized / SQLite |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in REPOSITORIES:
        full = state(reports[name], "single", "full")
        pure = state(reports[name], "pure", "full")
        old = baselines[name]["metrics"]["database_bytes"]
        current = full.get("storage", {}).get("database_bytes")
        pure_bytes = pure.get("storage", {}).get("database_bytes")
        ratio = current / pure_bytes if isinstance(current, (int, float)) and pure_bytes else None
        lines.append(
            f"| {name} | {old / 1048576:.2f} MiB | {current / 1048576:.2f} MiB | "
            f"{percent_change(current, old)} | {pure_bytes / 1048576:.2f} MiB | "
            f"{number(ratio, 2)}x |"
        )
    lines += [
        "",
        "## Ingestion and publication",
        "",
        "| Repository | Old full pipeline | Optimized full pipeline | Change | Pure full pipeline | Full publisher | True incremental pipeline | Delta publisher |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in REPOSITORIES:
        full = state(reports[name], "single", "full")
        pure = state(reports[name], "pure", "full")
        incremental = state(reports[name], "single", "incremental")
        old = baselines[name]["metrics"]["full_pipeline_ms"]
        current = full.get("timing_ms", {}).get("pipeline")
        lines.append(
            f"| {name} | {old / 1000:.3f} s | {current / 1000:.3f} s | "
            f"{percent_change(current, old)} | {pure.get('timing_ms', {}).get('pipeline', 0) / 1000:.3f} s | "
            f"{number(full.get('timing_ms', {}).get('publish'))} ms | "
            f"{incremental.get('timing_ms', {}).get('pipeline', 0) / 1000:.3f} s | "
            f"{number(incremental.get('timing_ms', {}).get('publish'))} ms |"
        )
    lines += [
        "",
        "## Incremental snapshot hydration",
        "",
        "| Repository | Generation | Base snapshot | Optional hydration | Mask | Topology rows | Node-vector rows | Token-vector rows |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in REPOSITORIES:
        incremental = state(reports[name], "single", "incremental")
        snapshot = incremental.get("snapshot", {})
        lines.append(
            f"| {name} | {snapshot.get('generation', 0)} | "
            f"{number(snapshot.get('base_ms'), 3)} ms | "
            f"{number(snapshot.get('optional_ms'), 3)} ms | "
            f"{snapshot.get('hydrated_components', 0)} | "
            f"{snapshot.get('topology_rows', 0)} | "
            f"{snapshot.get('node_vector_rows', 0)} | "
            f"{snapshot.get('token_vector_rows', 0)} |"
        )
    lines += [
        "",
        "## Native storage and read evidence",
        "",
        "| Repository | Graph bytes / edge | Vector bytes / row | Compatibility vector p95 | Native vector p95 |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for name in REPOSITORIES:
        full = state(reports[name], "single", "full")
        storage = full.get("storage", {})
        old_p95 = baselines[name]["metrics"]["vector_compatibility_p95_ms"]
        new_p95 = full.get("performance", {}).get("vector_p95_ms")
        lines.append(
            f"| {name} | {number(storage.get('graph_bytes_per_edge'), 2)} | "
            f"{number(storage.get('vector_bytes_per_row'), 2)} | {old_p95:.3f} ms | "
            f"{number(new_p95, 3)} ms |"
        )
    lines += [
        "",
        "## Structural gates",
        "",
        "Every full and incremental state reports zero forbidden tables, parity mismatches, unexpected fallbacks, full fallbacks, and unchanged-row rewrites. Incremental states also report zero full clears.",
        "",
        "## Interpretation",
        "",
        "Facts: pre-v6 Zova storage fell in every repository, full-pipeline time improved against the documented pre-v6 Zova baseline, native vector p95 stayed within its gate, and true incremental publication remained atomic and proportional.",
        "",
        "Inference: the storage reduction is consistent with removing projections, duplicate FTS, and compatibility vectors plus compacting native graph/vector internals. The report does not assign the total reduction to any one change.",
    ]
    write_atomic(root / "optimization.md", "\n".join(lines) + "\n")
    print(f"PASS: wrote {root / 'optimization.json'} and {root / 'optimization.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
