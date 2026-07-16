#!/usr/bin/env python3
"""Compare fresh pure-SQLite evidence with retained Section 9 Zova reports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import statistics
import sys
from typing import Any


REPOSITORIES = ("tops", "motive", "rvault", "CBM")
STATES = ("full", "incremental")
ZERO_FIELDS = (
    "metadata_mismatches", "fts_mismatches", "graph_mismatches",
    "vector_mismatches", "public_mcp_mismatches", "cypher_mismatches",
    "unexpected_fallback_count", "cross_workspace_results",
    "compatibility_artifact_count", "fresh_full_mismatches",
)


def fail(message: str) -> None:
    raise ValueError(message)


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {path}: {error}")
    if not isinstance(value, dict):
        fail(f"report root must be an object: {path}")
    return value


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def number(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{label} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        fail(f"{label} must be finite and nonnegative")
    return result


def state_map(report: dict[str, Any], label: str) -> dict[str, dict[str, Any]]:
    states = report.get("states")
    if not isinstance(states, list) or len(states) != 2:
        fail(f"{label}.states must contain full and incremental")
    result = {state.get("name"): state for state in states if isinstance(state, dict)}
    if tuple(result) != STATES:
        fail(f"{label}.states must be ordered full, incremental")
    return result


def validate_pure(report: dict[str, Any], repo: str, attempt: int, label: str) -> None:
    if report.get("schema_version") != 1 or report.get("baseline_route") != "pure_sqlite":
        fail(f"{label} is not a schema-1 pure_sqlite report")
    if report.get("repository") != repo or report.get("attempt") != attempt:
        fail(f"{label} repository/attempt mismatch")
    if report.get("passed") is not True:
        fail(f"{label} did not pass")
    for name, state in state_map(report, label).items():
        workload = "full_pipeline" if name == "full" else "changed_state_full_pipeline"
        if state.get("baseline_route") != "pure_sqlite" or state.get("state_workload") != workload:
            fail(f"{label}.{name} route/workload mismatch")
        if state.get("passed") is not True:
            fail(f"{label}.{name} did not pass parity")
        for field in ZERO_FIELDS:
            if state.get(field, 0) != 0:
                fail(f"{label}.{name}.{field} must be zero")
        storage = state.get("storage")
        if not isinstance(storage, dict) or storage.get("compat_zova_bytes") != 0:
            fail(f"{label}.{name} pure SQLite baseline produced sidecar bytes")


def parse_named(values: list[str]) -> dict[str, list[pathlib.Path]]:
    result = {repo: [] for repo in REPOSITORIES}
    for value in values:
        if "=" not in value:
            fail("--pure-report must use REPOSITORY=PATH")
        repo, raw = value.split("=", 1)
        if repo not in result:
            fail(f"unknown pure repository: {repo}")
        result[repo].append(pathlib.Path(raw))
    for repo, paths in result.items():
        if len(paths) != 3:
            fail(f"{repo} requires exactly three pure reports")
    return result


def average(states: list[dict[str, Any]], path: tuple[str, ...], label: str) -> float:
    values = []
    for state in states:
        value: Any = state
        for key in path:
            if not isinstance(value, dict) or key not in value:
                fail(f"{label} missing {'.'.join(path)}")
            value = value[key]
        values.append(number(value, f"{label}.{'.'.join(path)}"))
    return statistics.mean(values)


def metric_block(pure: list[dict[str, Any]], retained: list[dict[str, Any]],
                 section: str, percentile: str, label: str) -> dict[str, float]:
    pure_value = average(pure, ("performance", section, f"compat_{percentile}_ms"), label)
    transitional = average(retained, ("performance", section, f"compat_{percentile}_ms"), label)
    single = average(retained, ("performance", section, f"single_{percentile}_ms"), label)
    return {
        "pure_sqlite_ms": pure_value,
        "transitional_ms": transitional,
        "single_file_ms": single,
        "single_file_to_pure_ratio": single / pure_value if pure_value else 0.0,
    }


def compare_state(pure: list[dict[str, Any]], retained: list[dict[str, Any]],
                  label: str) -> dict[str, Any]:
    pure_ingestion = average(pure, ("ingestion", "compat_ms"), label)
    transitional_ingestion = average(retained, ("ingestion", "compat_ms"), label)
    single_ingestion = average(retained, ("ingestion", "single_ms"), label)
    pure_storage = average(pure, ("storage", "compat_db_bytes"), label)
    transitional_storage = statistics.mean(
        number(s["storage"]["compat_db_bytes"], label) +
        number(s["storage"]["compat_zova_bytes"], label) for s in retained
    )
    single_storage = average(retained, ("storage", "single_bytes"), label)
    return {
        "workload": "full_pipeline" if pure[0]["name"] == "full" else
                    "changed_state_full_pipeline",
        "ingestion": {
            "pure_sqlite_ms": pure_ingestion,
            "transitional_ms": transitional_ingestion,
            "single_file_ms": single_ingestion,
            "single_file_to_pure_ratio": single_ingestion / pure_ingestion,
        },
        "storage": {
            "pure_sqlite_bytes": pure_storage,
            "transitional_bytes": transitional_storage,
            "single_file_bytes": single_storage,
            "single_file_to_pure_ratio": single_storage / pure_storage,
        },
        "graph": {
            percentile: metric_block(pure, retained, "graph", percentile, label)
            for percentile in ("p50", "p95")
        },
        "vector": {
            percentile: metric_block(pure, retained, "vector", percentile, label)
            for percentile in ("p50", "p95")
        },
    }


def markdown(data: dict[str, Any]) -> str:
    lines = [
        "# Pure SQLite versus single-file Zova measurements", "",
        "The `incremental` join key below is the retained Section 9 name. Its actual workload is",
        "`changed_state_full_pipeline`, not the real incremental pipeline.", "",
    ]
    for state_name in STATES:
        title = "Full pipeline" if state_name == "full" else "Changed-state full pipeline"
        lines += [f"## {title}", "",
                  "| Repository | SQLite ingest | Zova ingest | Ratio | SQLite storage | Zova storage | Ratio | Graph p95 ratio | Vector p95 ratio |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for repo in REPOSITORIES:
            row = data["repositories"][repo][state_name]
            i, s = row["ingestion"], row["storage"]
            lines.append(
                f"| {repo} | {i['pure_sqlite_ms']/1000:.3f} s | {i['single_file_ms']/1000:.3f} s | "
                f"{i['single_file_to_pure_ratio']:.3f} | {s['pure_sqlite_bytes']/1048576:.2f} MiB | "
                f"{s['single_file_bytes']/1048576:.2f} MiB | {s['single_file_to_pure_ratio']:.3f} | "
                f"{row['graph']['p95']['single_file_to_pure_ratio']:.3f} | "
                f"{row['vector']['p95']['single_file_to_pure_ratio']:.3f} |"
            )
        lines.append("")
    return "\n".join(lines)


def write_atomic(path: pathlib.Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--section9", required=True, type=pathlib.Path)
    parser.add_argument("--pure-report", action="append", default=[])
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--markdown", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        pure_paths = parse_named(args.pure_report)
        aggregate = load(args.section9)
        if aggregate.get("passed") is not True:
            fail("retained Section 9 aggregate did not pass")
        attempts = aggregate.get("attempts")
        if not isinstance(attempts, list) or [a.get("attempt") for a in attempts] != [1, 2, 3]:
            fail("retained Section 9 aggregate must contain attempts 1, 2, 3")
        retained_paths = {repo: [] for repo in REPOSITORIES}
        for attempt in attempts:
            reports = attempt.get("reports")
            if not isinstance(reports, list):
                fail("retained attempt reports are missing")
            by_repo = {r.get("repo"): r for r in reports if isinstance(r, dict)}
            if set(by_repo) != set(REPOSITORIES):
                fail("retained attempt must contain all four repositories")
            for repo in REPOSITORIES:
                retained_paths[repo].append(pathlib.Path(by_repo[repo]["path"]))

        provenance: dict[str, Any] = {
            "section9": {"path": str(args.section9), "sha256": digest(args.section9)},
            "pure_reports": {}, "retained_reports": {},
        }
        repositories: dict[str, Any] = {}
        for repo in REPOSITORIES:
            pure_reports = [load(path) for path in pure_paths[repo]]
            retained_reports = [load(path) for path in retained_paths[repo]]
            for index, report in enumerate(pure_reports, 1):
                validate_pure(report, repo, index, f"pure.{repo}.attempt{index}")
            for index, report in enumerate(retained_reports, 1):
                if report.get("repository") != repo or report.get("attempt") != index or report.get("passed") is not True:
                    fail(f"retained.{repo}.attempt{index} identity/pass mismatch")
            provenance["pure_reports"][repo] = [
                {"path": str(path), "sha256": digest(path)} for path in pure_paths[repo]
            ]
            provenance["retained_reports"][repo] = [
                {"path": str(path), "sha256": digest(path)} for path in retained_paths[repo]
            ]
            pure_states = [state_map(report, f"pure.{repo}") for report in pure_reports]
            retained_states = [state_map(report, f"retained.{repo}") for report in retained_reports]
            repositories[repo] = {
                state: compare_state(
                    [item[state] for item in pure_states],
                    [item[state] for item in retained_states], f"{repo}.{state}")
                for state in STATES
            }
        result = {
            "schema_version": 1,
            "passed": True,
            "attempt_count": 3,
            "comparison": "pure_sqlite_vs_retained_single_file_zova",
            "repositories": repositories,
            "provenance": provenance,
        }
        write_atomic(args.output, json.dumps(result, indent=2, sort_keys=True) + "\n")
        write_atomic(args.markdown, markdown(result))
        return 0
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
