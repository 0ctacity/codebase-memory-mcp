#!/usr/bin/env python3
"""Strict before/after gate for Zova storage and ingestion optimization."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import pathlib
import re
import statistics
import sys
from typing import Any


SCHEMA_VERSION = 1
STATE_ORDER = (
    ("pure", "full", "CBM_MODE_FULL"),
    ("single", "full", "CBM_MODE_FULL"),
    ("pure", "incremental", "CBM_MODE_INCREMENTAL"),
    ("single", "incremental", "CBM_MODE_INCREMENTAL"),
)
ROW_FIELDS = (
    "nodes_total",
    "nodes_inserted",
    "nodes_updated",
    "nodes_deleted",
    "edges_total",
    "edges_inserted",
    "edges_deleted",
    "node_vectors_total",
    "node_vectors_upserted",
    "node_vectors_deleted",
    "token_vectors_total",
    "token_vectors_upserted",
    "token_vectors_deleted",
)
TIMING_FIELDS = (
    "normalize",
    "canonical_nodes",
    "canonical_edges",
    "fts",
    "native_graph",
    "native_vectors",
    "digests",
    "verify",
    "publish",
    "pipeline",
)
STORAGE_INTEGER_FIELDS = (
    "database_bytes",
    "wal_bytes",
    "freelist_bytes",
    "canonical_bytes",
    "fts_bytes",
    "native_graph_bytes",
    "native_vector_bytes",
    "other_bytes",
)
STORAGE_NUMBER_FIELDS = ("graph_bytes_per_edge", "vector_bytes_per_row")
ZERO_FIELDS = (
    "forbidden_table_count",
    "parity_mismatch_count",
    "unexpected_fallback_count",
)
COUNT_FIELDS = ("full_fallback_count", "full_clear_count", "unchanged_rewrite_count")
SNAPSHOT_ROW_FIELDS = ("topology_rows", "node_vector_rows", "token_vector_rows")


def _mapping(value: Any, path: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise TypeError(f"{path} must be an object")
    return value


def _list(value: Any, path: str) -> list[Any]:
    if type(value) is not list:
        raise TypeError(f"{path} must be an array")
    return value


def _boolean(value: Any, path: str) -> bool:
    if type(value) is not bool:
        raise TypeError(f"{path} must be a boolean")
    return value


def _integer(value: Any, path: str) -> int:
    if type(value) is not int:
        raise TypeError(f"{path} must be an integer")
    if value < 0:
        raise ValueError(f"{path} must be nonnegative")
    return value


def _number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"{path} must be a number")
    result = float(value)
    if not math.isfinite(result) or result < 0.0:
        raise ValueError(f"{path} must be finite and nonnegative")
    return result


def _string(value: Any, path: str) -> str:
    if type(value) is not str or not value:
        raise TypeError(f"{path} must be a nonempty string")
    return value


def _digest(value: Any, path: str, length: int) -> str:
    result = _string(value, path)
    if re.fullmatch(rf"[0-9a-f]{{{length}}}", result) is None:
        raise ValueError(f"{path} must be {length} lowercase hexadecimal characters")
    return result


def _state(value: Any, index: int) -> dict[str, Any]:
    state = _mapping(value, f"states[{index}]")
    expected_route, expected_workload, expected_mode = STATE_ORDER[index]
    route = _string(state.get("route"), f"states[{index}].route")
    workload = _string(state.get("workload"), f"states[{index}].workload")
    pipeline_mode = _string(state.get("pipeline_mode"), f"states[{index}].pipeline_mode")
    if route != expected_route:
        raise ValueError(f"states[{index}].route must be {expected_route}")
    if workload != expected_workload:
        raise ValueError(f"states[{index}].workload must be {expected_workload}")
    if pipeline_mode != expected_mode:
        raise ValueError(f"states[{index}].pipeline_mode must be {expected_mode}")
    reason = state.get("incremental_fallback_reason")
    if reason is not None and type(reason) is not str:
        raise TypeError(f"states[{index}].incremental_fallback_reason must be null or string")
    _boolean(state.get("passed"), f"states[{index}].passed")
    for field in COUNT_FIELDS + ZERO_FIELDS:
        _integer(state.get(field), f"states[{index}].{field}")

    rows = _mapping(state.get("rows"), f"states[{index}].rows")
    for field in ROW_FIELDS:
        _integer(rows.get(field), f"states[{index}].rows.{field}")
    timing = _mapping(state.get("timing_ms"), f"states[{index}].timing_ms")
    for field in TIMING_FIELDS:
        _number(timing.get(field), f"states[{index}].timing_ms.{field}")
    snapshot = _mapping(state.get("snapshot"), f"states[{index}].snapshot")
    snapshot_completed = _boolean(snapshot.get("completed"),
                                  f"states[{index}].snapshot.completed")
    generation = _integer(snapshot.get("generation"),
                          f"states[{index}].snapshot.generation")
    _number(snapshot.get("base_ms"), f"states[{index}].snapshot.base_ms")
    _number(snapshot.get("optional_ms"), f"states[{index}].snapshot.optional_ms")
    components = _integer(snapshot.get("hydrated_components"),
                          f"states[{index}].snapshot.hydrated_components")
    if components & ~0x7:
        raise ValueError(f"states[{index}].snapshot.hydrated_components contains invalid bits")
    snapshot_rows = {
        field: _integer(snapshot.get(field), f"states[{index}].snapshot.{field}")
        for field in SNAPSHOT_ROW_FIELDS
    }
    expected_snapshot = expected_route == "single" and expected_workload == "incremental"
    if snapshot_completed != expected_snapshot:
        raise ValueError(f"states[{index}].snapshot.completed must be {str(expected_snapshot).lower()}")
    if expected_snapshot and generation <= 0:
        raise ValueError(f"states[{index}].snapshot.generation must be positive")
    if not expected_snapshot and (generation != 0 or components != 0 or
                                  any(snapshot_rows.values())):
        raise ValueError(f"states[{index}].snapshot must be empty outside single incremental")
    bit_rows = ((0x1, "topology_rows"), (0x2, "node_vector_rows"),
                (0x4, "token_vector_rows"))
    for bit, field in bit_rows:
        if snapshot_rows[field] > 0 and not components & bit:
            raise ValueError(f"states[{index}].snapshot.{field} requires component bit {bit}")
    storage = _mapping(state.get("storage"), f"states[{index}].storage")
    for field in STORAGE_INTEGER_FIELDS:
        _integer(storage.get(field), f"states[{index}].storage.{field}")
    for field in STORAGE_NUMBER_FIELDS:
        _number(storage.get(field), f"states[{index}].storage.{field}")
    performance = _mapping(state.get("performance"), f"states[{index}].performance")
    _number(performance.get("vector_p95_ms"), f"states[{index}].performance.vector_p95_ms")
    return state


def _report(value: Any, label: str) -> dict[str, Any]:
    report = _mapping(value, label)
    if _integer(report.get("schema_version"), f"{label}.schema_version") != SCHEMA_VERSION:
        raise ValueError(f"{label}.schema_version must be {SCHEMA_VERSION}")
    repository = _string(report.get("repository"), f"{label}.repository")
    if repository not in ("tops", "motive", "rvault", "CBM"):
        raise ValueError(f"{label}.repository is invalid")
    _digest(report.get("source_commit"), f"{label}.source_commit", 40)
    _digest(report.get("build_sha256"), f"{label}.build_sha256", 64)
    _string(report.get("run_id"), f"{label}.run_id")
    mutation = _string(report.get("mutation"), f"{label}.mutation")
    if mutation not in ("digest-stable", "source-change"):
        raise ValueError(f"{label}.mutation is invalid")
    benchmark_kind = report.get("benchmark_kind", "storage-ingestion")
    if benchmark_kind not in ("storage-ingestion", "lazy-hydration"):
        raise ValueError(f"{label}.benchmark_kind is invalid")
    _boolean(report.get("passed"), f"{label}.passed")
    states = _list(report.get("states"), f"{label}.states")
    if len(states) != len(STATE_ORDER):
        raise ValueError(f"{label}.states must contain exactly {len(STATE_ORDER)} states")
    for index, item in enumerate(states):
        _state(item, index)
    return report


def _documented_baseline(value: Any) -> dict[str, Any]:
    baseline = _mapping(value, "baseline")
    if _integer(baseline.get("schema_version"), "baseline.schema_version") != SCHEMA_VERSION:
        raise ValueError(f"baseline.schema_version must be {SCHEMA_VERSION}")
    if _string(baseline.get("baseline_kind"), "baseline.baseline_kind") != "documented_pre_v6":
        raise ValueError("baseline.baseline_kind must be documented_pre_v6")
    repository = _string(baseline.get("repository"), "baseline.repository")
    if repository not in ("tops", "motive", "rvault", "CBM"):
        raise ValueError("baseline.repository is invalid")
    _string(baseline.get("run_id"), "baseline.run_id")
    sources = _list(baseline.get("source_documents"), "baseline.source_documents")
    if not sources:
        raise ValueError("baseline.source_documents must not be empty")
    for index, value in enumerate(sources):
        source = _mapping(value, f"baseline.source_documents[{index}]")
        _string(source.get("path"), f"baseline.source_documents[{index}].path")
        _digest(source.get("sha256"), f"baseline.source_documents[{index}].sha256", 64)
    metrics = _mapping(baseline.get("metrics"), "baseline.metrics")
    _integer(metrics.get("database_bytes"), "baseline.metrics.database_bytes")
    _number(metrics.get("full_pipeline_ms"), "baseline.metrics.full_pipeline_ms")
    _number(metrics.get("vector_compatibility_p95_ms"), "baseline.metrics.vector_compatibility_p95_ms")
    if repository == "tops":
        _number(metrics.get("full_publish_ms"), "baseline.metrics.full_publish_ms")
    return baseline


def _changed_ratio(rows: dict[str, Any], prefix: str) -> float:
    total = _integer(rows[f"{prefix}_total"], f"rows.{prefix}_total")
    fields = {
        "nodes": ("nodes_inserted", "nodes_updated", "nodes_deleted"),
        "edges": ("edges_inserted", "edges_deleted"),
        "node_vectors": ("node_vectors_upserted", "node_vectors_deleted"),
        "token_vectors": ("token_vectors_upserted", "token_vectors_deleted"),
    }[prefix]
    changed = sum(_integer(rows[field], f"rows.{field}") for field in fields)
    if total == 0:
        return 0.0 if changed == 0 else math.inf
    return changed / total


def validate(baseline_value: Any, report_value: Any) -> dict[str, Any]:
    if type(baseline_value) is dict and baseline_value.get("baseline_kind") == "documented_pre_v6":
        return _validate_documented(baseline_value, report_value)
    baseline = _report(baseline_value, "baseline")
    report = _report(report_value, "report")
    if baseline["repository"] != report["repository"]:
        raise ValueError("baseline and report repository must match")
    if baseline.get("benchmark_kind", "storage-ingestion") != report.get(
        "benchmark_kind", "storage-ingestion"
    ):
        raise ValueError("baseline and report benchmark_kind must match")
    if report.get("benchmark_kind") == "lazy-hydration":
        return _validate_lazy_hydration(baseline, report)
    failures: list[str] = []
    if report["passed"] is not True:
        failures.append("report.passed is false")

    for index, state in enumerate(report["states"]):
        label = f"{state['route']} {state['workload']}"
        if state["passed"] is not True:
            failures.append(f"{label}: state passed is false")
        for field in ZERO_FIELDS:
            if state[field] != 0:
                failures.append(f"{label}: {field} must be zero")
        if state["route"] == "single" and state["workload"] == "incremental":
            for field in COUNT_FIELDS:
                if state[field] != 0:
                    failures.append(f"{label}: {field} must be zero")
            if state["incremental_fallback_reason"] not in (None, ""):
                failures.append(f"{label}: incremental_fallback_reason must be null")
            rows = state["rows"]
            ratios = (
                _changed_ratio(rows, "nodes"),
                _changed_ratio(rows, "edges"),
                _changed_ratio(rows, "node_vectors"),
                _changed_ratio(rows, "token_vectors"),
            )
            if max(ratios) >= 0.10:
                failures.append(f"{label}: changed-row ratio must be below 0.10")

    old_single_full = baseline["states"][1]
    new_single_full = report["states"][1]
    new_single_incremental = report["states"][3]
    old_storage = float(old_single_full["storage"]["database_bytes"])
    new_storage = float(new_single_full["storage"]["database_bytes"])
    if old_storage <= 0.0 or new_storage > old_storage * 0.80:
        failures.append("single full: storage reduction must be at least 20%")

    old_publish = _number(old_single_full["timing_ms"]["publish"], "baseline publish")
    new_publish = _number(new_single_full["timing_ms"]["publish"], "report publish")
    if old_publish <= 0.0 or new_publish > old_publish * 0.85:
        failures.append("single full: publisher reduction must be at least 15%")

    for field, description in (
        ("graph_bytes_per_edge", "graph bytes/edge"),
        ("vector_bytes_per_row", "vector bytes/vector"),
    ):
        old_value = _number(old_single_full["storage"][field], f"baseline {field}")
        new_value = _number(new_single_full["storage"][field], f"report {field}")
        if old_value <= 0.0 or new_value >= old_value:
            failures.append(f"single full: {description} must improve")

    old_p95 = _number(
        old_single_full["performance"]["vector_p95_ms"], "baseline vector p95"
    )
    new_p95 = _number(
        new_single_full["performance"]["vector_p95_ms"], "report vector p95"
    )
    if old_p95 <= 0.0 or new_p95 > old_p95 * 1.05:
        failures.append("single full: vector p95 exceeds 1.05x baseline")

    delta_publish = _number(
        new_single_incremental["timing_ms"]["publish"], "delta publisher"
    )
    if old_publish <= 0.0 or delta_publish > old_publish * 0.30:
        failures.append("single incremental: delta publisher exceeds 30% of old full publisher")

    return {
        "schema_version": SCHEMA_VERSION,
        "repository": report["repository"],
        "baseline_run_id": baseline["run_id"],
        "report_run_id": report["run_id"],
        "baseline_build_sha256": baseline["build_sha256"],
        "report_build_sha256": report["build_sha256"],
        "passed": not failures,
        "failures": failures,
    }


def _validate_lazy_hydration(baseline: dict[str, Any],
                             report: dict[str, Any]) -> dict[str, Any]:
    if baseline["mutation"] != report["mutation"]:
        raise ValueError("baseline and report mutation must match")
    failures: list[str] = []
    if report["passed"] is not True:
        failures.append("report.passed is false")
    for state in report["states"]:
        label = f"{state['route']} {state['workload']}"
        if state["passed"] is not True:
            failures.append(f"{label}: state passed is false")
        for field in ZERO_FIELDS:
            if state[field] != 0:
                failures.append(f"{label}: {field} must be zero")
    incremental = report["states"][3]
    for field in COUNT_FIELDS:
        if incremental[field] != 0:
            failures.append(f"single incremental: {field} must be zero")
    if incremental["incremental_fallback_reason"] not in (None, ""):
        failures.append("single incremental: incremental_fallback_reason must be null")
    snapshot = incremental["snapshot"]
    if report["mutation"] == "digest-stable":
        if snapshot["hydrated_components"] != 0 or any(
            snapshot[field] != 0 for field in SNAPSHOT_ROW_FIELDS
        ):
            failures.append("single incremental: digest-stable snapshot must hydrate zero components")
    old_ms = _number(baseline["states"][3]["timing_ms"]["pipeline"],
                     "baseline incremental pipeline")
    new_ms = _number(incremental["timing_ms"]["pipeline"],
                     "report incremental pipeline")
    pure_ms = _number(report["states"][2]["timing_ms"]["pipeline"],
                      "report pure incremental pipeline")
    if report["mutation"] == "digest-stable":
        if old_ms <= 0.0 or new_ms > old_ms * 0.85:
            failures.append("single incremental: digest-stable improvement must be at least 15%")
        if pure_ms <= 0.0 or new_ms > pure_ms * 1.15:
            failures.append("single incremental: digest-stable ratio exceeds 1.15x pure SQLite")
    elif old_ms <= 0.0 or new_ms > old_ms * 1.05:
        failures.append("single incremental: source-change regression exceeds 5%")
    return {
        "schema_version": SCHEMA_VERSION,
        "repository": report["repository"],
        "benchmark_kind": "lazy-hydration",
        "mutation": report["mutation"],
        "baseline_run_id": baseline["run_id"],
        "report_run_id": report["run_id"],
        "baseline_build_sha256": baseline["build_sha256"],
        "report_build_sha256": report["build_sha256"],
        "passed": not failures,
        "failures": failures,
    }


def _validate_documented(baseline_value: Any, report_value: Any) -> dict[str, Any]:
    baseline = _documented_baseline(baseline_value)
    report = _report(report_value, "report")
    if baseline["repository"] != report["repository"]:
        raise ValueError("baseline and report repository must match")
    failures: list[str] = []
    if report["passed"] is not True:
        failures.append("report.passed is false")
    for state in report["states"]:
        label = f"{state['route']} {state['workload']}"
        if state["passed"] is not True:
            failures.append(f"{label}: state passed is false")
        for field in ZERO_FIELDS:
            if state[field] != 0:
                failures.append(f"{label}: {field} must be zero")
        if state["route"] == "single" and state["workload"] == "incremental":
            for field in COUNT_FIELDS:
                if state[field] != 0:
                    failures.append(f"{label}: {field} must be zero")
            if state["incremental_fallback_reason"] not in (None, ""):
                failures.append(f"{label}: incremental_fallback_reason must be null")
            ratios = tuple(
                _changed_ratio(state["rows"], prefix)
                for prefix in ("nodes", "edges", "node_vectors", "token_vectors")
            )
            if max(ratios) >= 0.10:
                failures.append(f"{label}: changed-row ratio must be below 0.10")

    metrics = baseline["metrics"]
    full = report["states"][1]
    incremental = report["states"][3]
    old_storage = _number(metrics["database_bytes"], "baseline database_bytes")
    new_storage = _number(full["storage"]["database_bytes"], "report database_bytes")
    storage_factor = 0.80 if report["repository"] == "tops" else 1.0
    if old_storage <= 0.0 or new_storage > old_storage * storage_factor:
        requirement = "at least 20%" if report["repository"] == "tops" else "positive"
        failures.append(f"single full: storage reduction must be {requirement}")

    vector_p95 = _number(full["performance"]["vector_p95_ms"], "report vector p95")
    compatibility_p95 = _number(metrics["vector_compatibility_p95_ms"], "baseline compatibility vector p95")
    if compatibility_p95 <= 0.0 or vector_p95 > compatibility_p95 * 1.05:
        failures.append("single full: vector p95 exceeds 1.05x compatibility baseline")

    if _number(full["storage"]["graph_bytes_per_edge"], "report graph bytes/edge") <= 0.0:
        failures.append("single full: graph bytes/edge must be positive")
    if _number(full["storage"]["vector_bytes_per_row"], "report vector bytes/vector") <= 0.0:
        failures.append("single full: vector bytes/vector must be positive")

    if report["repository"] == "tops":
        old_publish = _number(metrics["full_publish_ms"], "baseline full publisher")
        new_publish = _number(full["timing_ms"]["publish"], "report full publisher")
        if old_publish <= 0.0 or new_publish > old_publish * 0.85:
            failures.append("single full: publisher reduction must be at least 15%")
        delta_publish = _number(incremental["timing_ms"]["publish"], "delta publisher")
        if delta_publish > old_publish * 0.30:
            failures.append("single incremental: delta publisher exceeds 30% of old full publisher")

    return {
        "schema_version": SCHEMA_VERSION,
        "repository": report["repository"],
        "baseline_kind": baseline["baseline_kind"],
        "baseline_run_id": baseline["run_id"],
        "report_run_id": report["run_id"],
        "baseline_source_documents": baseline["source_documents"],
        "report_build_sha256": report["build_sha256"],
        "passed": not failures,
        "failures": failures,
    }


def _canonical_sha256(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def aggregate_baseline(sample_values: list[Any]) -> dict[str, Any]:
    if len(sample_values) != 3:
        raise ValueError("baseline aggregation requires exactly three samples")
    samples = [_report(value, f"samples[{index}]") for index, value in enumerate(sample_values)]
    first = samples[0]
    for index, sample in enumerate(samples[1:], 1):
        for field in ("repository", "source_commit", "build_sha256", "mutation"):
            if sample[field] != first[field]:
                raise ValueError(f"samples[{index}].{field} does not match sample 0")
        if sample.get("benchmark_kind", "storage-ingestion") != first.get(
            "benchmark_kind", "storage-ingestion"
        ):
            raise ValueError(
                f"samples[{index}].benchmark_kind does not match sample 0"
            )
    aggregate = copy.deepcopy(first)
    sample_sha256 = [_canonical_sha256(sample) for sample in samples]
    aggregate["run_id"] = "baseline-" + hashlib.sha256(
        "".join(sample_sha256).encode()
    ).hexdigest()[:20]
    aggregate["sample_run_ids"] = [sample["run_id"] for sample in samples]
    aggregate["sample_sha256"] = sample_sha256
    aggregate["passed"] = all(sample["passed"] is True for sample in samples)
    for state_index, state in enumerate(aggregate["states"]):
        source_states = [sample["states"][state_index] for sample in samples]
        state["passed"] = all(item["passed"] is True for item in source_states)
        for field in COUNT_FIELDS + ZERO_FIELDS:
            state[field] = int(statistics.median(item[field] for item in source_states))
        for field in ROW_FIELDS:
            state["rows"][field] = int(
                statistics.median(item["rows"][field] for item in source_states)
            )
        for field in TIMING_FIELDS:
            state["timing_ms"][field] = float(
                statistics.median(item["timing_ms"][field] for item in source_states)
            )
        snapshot_states = [item["snapshot"] for item in source_states]
        state["snapshot"]["completed"] = all(item["completed"] for item in snapshot_states)
        for field in ("generation", "hydrated_components") + SNAPSHOT_ROW_FIELDS:
            state["snapshot"][field] = int(
                statistics.median(item[field] for item in snapshot_states)
            )
        for field in ("base_ms", "optional_ms"):
            state["snapshot"][field] = float(
                statistics.median(item[field] for item in snapshot_states)
            )
        for field in STORAGE_INTEGER_FIELDS:
            state["storage"][field] = int(
                statistics.median(item["storage"][field] for item in source_states)
            )
        for field in STORAGE_NUMBER_FIELDS:
            state["storage"][field] = float(
                statistics.median(item["storage"][field] for item in source_states)
            )
        state["performance"]["vector_p95_ms"] = float(
            statistics.median(
                item["performance"]["vector_p95_ms"] for item in source_states
            )
        )
    aggregate["digest"] = _canonical_sha256(
        {key: value for key, value in aggregate.items() if key != "digest"}
    )
    return aggregate


def _read_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text())


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--aggregate-baseline", action="store_true")
    parser.add_argument("--sample", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    if args.aggregate_baseline:
        try:
            aggregate = aggregate_baseline([_read_json(path) for path in args.sample])
            _write_json(args.output, aggregate)
            print(f"PASS: Zova optimization baseline ({aggregate['repository']})")
            return 0
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as error:
            decision = {"schema_version": SCHEMA_VERSION, "passed": False,
                        "failures": [str(error)]}
            _write_json(args.output, decision)
            print(f"FAIL: {error}", file=sys.stderr)
            return 1
    if args.baseline is None or args.report is None:
        parser.error("--baseline and --report are required unless --aggregate-baseline is used")
    try:
        decision = validate(_read_json(args.baseline), _read_json(args.report))
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as error:
        decision = {
            "schema_version": SCHEMA_VERSION,
            "passed": False,
            "failures": [str(error)],
        }
    _write_json(args.output, decision)
    if decision["passed"]:
        print(f"PASS: Zova optimization gate ({decision['repository']})")
        return 0
    for failure in decision["failures"]:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
