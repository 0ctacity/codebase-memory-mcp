#!/usr/bin/env python3
"""Validate Section 9 flagged single-file promotion evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
REPOSITORY_ORDER = ("tops", "motive", "rvault", "CBM")
ATTEMPT_ORDER = (1, 2, 3)
READ_THRESHOLD_RATIO = 1.05
OVERHEAD_HARD_CAP = 1.25
OVERHEAD_NEUTRAL_FLOOR = 1.00
CALIBRATION_SAMPLE_COUNT = 5
STATE_ORDER = ("full", "incremental")
RATIO_KEYS = (
    "full_ingestion",
    "incremental_ingestion",
    "full_storage",
    "incremental_storage",
)
ZERO_FIELDS = (
    "metadata_mismatches",
    "fts_mismatches",
    "graph_mismatches",
    "vector_mismatches",
    "public_mcp_mismatches",
    "cypher_mismatches",
    "unexpected_fallback_count",
    "cross_workspace_results",
    "compatibility_artifact_count",
    "fresh_full_mismatches",
)
POSITIVE_COUNTS = {
    "fts_query_count": 20,
    "graph_sample_count": 10,
    "vector_query_count": 4,
    "public_mcp_case_count": 8,
    "cypher_native_route_count": 1,
    "cypher_compat_route_count": 1,
}
PROOF_KEYS = (
    "cancellation",
    "publication_crash_recovery",
    "migration",
    "backup_restore",
    "workspace_deletion",
    "corruption_recovery",
)
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
RUN_ID = re.compile(r"^[A-Za-z0-9._-]+$")


def parse_named(value: str, option: str) -> tuple[str, Path]:
    if "=" not in value:
        raise ValueError(f"{option} must use NAME=PATH")
    name, path = value.split("=", 1)
    if not name or not path:
        raise ValueError(f"{option} must use NAME=PATH")
    return name, Path(path)


def load_json(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, f"unreadable report: {error}"
    return (value, None) if isinstance(value, dict) else (None, "report root must be an object")


def canonical_digest(value: dict[str, Any]) -> str:
    payload = dict(value)
    payload.pop("digest", None)
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def finite_number(value: object, *, positive: bool = False) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    if not math.isfinite(result) or result < 0.0 or (positive and result <= 0.0):
        return None
    return result


def exact_nonnegative_int(value: object, *, positive: bool = False) -> int | None:
    if type(value) is not int or value < 0 or (positive and value <= 0):
        return None
    return value


def calibrated_limit(ratio: float) -> float | None:
    if finite_number(ratio, positive=True) is None:
        return None
    rounded = math.ceil((ratio - 1e-12) * 100.0) / 100.0
    # A ratio below one is a speedup, not negative overhead. Small-repository
    # fixed costs must not turn that speedup into a mandatory acceleration for
    # larger repositories whose shared parsing work dominates elapsed time.
    limit = max(OVERHEAD_NEUTRAL_FLOOR, round(rounded + 0.05, 2))
    return limit if limit <= OVERHEAD_HARD_CAP else None


def path_is_fresh(path: Path) -> bool:
    return all(part != "latest" for part in path.parts)


def validate_digest_object(data: dict[str, Any], label: str) -> list[str]:
    digest = data.get("digest")
    if not isinstance(digest, str) or not HEX64.fullmatch(digest):
        return [f"{label}.digest must be 64 lowercase hex"]
    expected = canonical_digest(data)
    return [] if digest == expected else [f"{label}.digest does not match canonical content"]


def validate_calibration(data: dict[str, Any]) -> list[str]:
    reasons: list[str] = []
    if data.get("schema_version") != SCHEMA_VERSION:
        reasons.append(f"calibration.schema_version must be integer {SCHEMA_VERSION}")
    if data.get("repository") != "tops":
        reasons.append("calibration.repository must be tops")
    if not isinstance(data.get("source_commit"), str) or not HEX40.fullmatch(data["source_commit"]):
        reasons.append("calibration.source_commit must be 40 lowercase hex")
    if not isinstance(data.get("build_sha256"), str) or not HEX64.fullmatch(data["build_sha256"]):
        reasons.append("calibration.build_sha256 must be 64 lowercase hex")
    if data.get("sample_count") != CALIBRATION_SAMPLE_COUNT:
        reasons.append(f"calibration.sample_count must equal integer {CALIBRATION_SAMPLE_COUNT}")
    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) != CALIBRATION_SAMPLE_COUNT:
        reasons.append(
            f"calibration.samples must contain exactly {CALIBRATION_SAMPLE_COUNT} samples"
        )
        samples = []
    sample_ratios: dict[str, list[float]] = {key: [] for key in RATIO_KEYS}
    run_ids: set[str] = set()
    report_hashes: set[str] = set()
    for index, sample in enumerate(samples):
        prefix = f"calibration.samples[{index}]"
        if not isinstance(sample, dict) or set(sample) != {"run_id", "report_sha256", "ratios"}:
            reasons.append(f"{prefix} must contain run_id, report_sha256, and ratios")
            continue
        run_id = sample.get("run_id")
        report_hash = sample.get("report_sha256")
        ratios = sample.get("ratios")
        if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
            reasons.append(f"{prefix}.run_id is invalid")
        elif run_id in run_ids:
            reasons.append("calibration sample run IDs must be distinct")
        else:
            run_ids.add(run_id)
        if not isinstance(report_hash, str) or not HEX64.fullmatch(report_hash):
            reasons.append(f"{prefix}.report_sha256 must be 64 lowercase hex")
        elif report_hash in report_hashes:
            reasons.append("calibration sample report hashes must be distinct")
        else:
            report_hashes.add(report_hash)
        if not isinstance(ratios, dict) or set(ratios) != set(RATIO_KEYS):
            reasons.append(f"{prefix}.ratios must contain exactly the four ratio keys")
            continue
        for key in RATIO_KEYS:
            ratio = finite_number(ratios.get(key), positive=True)
            if ratio is None:
                reasons.append(f"{prefix}.ratios.{key} must be positive and finite")
            else:
                sample_ratios[key].append(ratio)
    observed = data.get("observed")
    limits = data.get("limits")
    if not isinstance(observed, dict):
        reasons.append("calibration.observed must be an object")
    if not isinstance(limits, dict):
        reasons.append("calibration.limits must be an object")
    if isinstance(observed, dict) and isinstance(limits, dict):
        if set(observed) != set(RATIO_KEYS):
            reasons.append("calibration.observed must contain exactly the four ratio keys")
        if set(limits) != set(RATIO_KEYS):
            reasons.append("calibration.limits must contain exactly the four ratio keys")
        for key in RATIO_KEYS:
            ratio = finite_number(observed.get(key), positive=True)
            limit = finite_number(limits.get(key), positive=True)
            if ratio is None:
                reasons.append(f"calibration.observed.{key} must be positive and finite")
                continue
            if len(sample_ratios[key]) == CALIBRATION_SAMPLE_COUNT and not math.isclose(
                ratio, max(sample_ratios[key]), rel_tol=0.0, abs_tol=1e-9
            ):
                reasons.append(f"calibration.observed.{key} must equal the maximum sample")
            expected = calibrated_limit(ratio)
            if expected is None:
                reasons.append(f"calibration.observed.{key} exceeds the {OVERHEAD_HARD_CAP:.2f} hard cap")
            elif limit is None or not math.isclose(limit, expected, rel_tol=0.0, abs_tol=1e-9):
                reasons.append(f"calibration.limits.{key} must equal {expected:.2f}")
    reasons.extend(validate_digest_object(data, "calibration"))
    return reasons


def validate_focused(data: dict[str, Any], attempt: int, build_sha: str) -> list[str]:
    reasons: list[str] = []
    if data.get("schema_version") != SCHEMA_VERSION:
        reasons.append(f"focused.schema_version must be integer {SCHEMA_VERSION}")
    if type(data.get("attempt")) is not int or data.get("attempt") != attempt:
        reasons.append(f"focused.attempt must equal integer {attempt}")
    if data.get("passed") is not True:
        reasons.append("focused.passed must be true")
    if data.get("build_sha256") != build_sha:
        reasons.append("focused.build_sha256 must match calibration")
    for field in ("test_runner_sha256", "suite_log_sha256"):
        value = data.get(field)
        if not isinstance(value, str) or not HEX64.fullmatch(value):
            reasons.append(f"focused.{field} must be 64 lowercase hex")
    proofs = data.get("proofs")
    if not isinstance(proofs, dict) or set(proofs) != set(PROOF_KEYS):
        reasons.append("focused.proofs must contain exactly the six required proofs")
    elif any(proofs.get(key) is not True for key in PROOF_KEYS):
        reasons.append("every focused proof must be true")
    reasons.extend(validate_digest_object(data, "focused"))
    return reasons


def validate_performance(state: dict[str, Any], reasons: list[str], prefix: str) -> None:
    performance = state.get("performance")
    if not isinstance(performance, dict):
        reasons.append(f"{prefix}.performance must be an object")
        return
    if exact_nonnegative_int(performance.get("sample_count"), positive=True) != 20:
        reasons.append(f"{prefix}.performance.sample_count must equal integer 20")
    for section_name in ("graph", "vector"):
        section = performance.get(section_name)
        if not isinstance(section, dict):
            reasons.append(f"{prefix}.performance.{section_name} must be an object")
            continue
        values: dict[str, float] = {}
        for field in ("compat_p50_ms", "compat_p95_ms"):
            value = finite_number(section.get(field), positive=True)
            if value is None:
                reasons.append(f"{prefix}.performance.{section_name}.{field} must be positive and finite")
            else:
                values[field] = value
        for field in ("single_p50_ms", "single_p95_ms"):
            value = finite_number(section.get(field))
            if value is None:
                reasons.append(f"{prefix}.performance.{section_name}.{field} must be non-negative and finite")
            else:
                values[field] = value
        for percentile in ("p50", "p95"):
            compat = values.get(f"compat_{percentile}_ms")
            single = values.get(f"single_{percentile}_ms")
            if compat is not None and single is not None and (
                single > compat * READ_THRESHOLD_RATIO + 1e-9
            ):
                reasons.append(
                    f"{prefix}.performance.{section_name}.single_{percentile}_ms "
                    f"exceeds compatibility × {READ_THRESHOLD_RATIO:.2f}"
                )


def validate_overhead(
    state: dict[str, Any], state_name: str, limits: dict[str, Any] | None,
    reasons: list[str], prefix: str,
) -> None:
    ingestion = state.get("ingestion")
    if not isinstance(ingestion, dict):
        reasons.append(f"{prefix}.ingestion must be an object")
    else:
        compat = finite_number(ingestion.get("compat_ms"), positive=True)
        single = finite_number(ingestion.get("single_ms"))
        ratio = finite_number(ingestion.get("ratio"), positive=True)
        if compat is None:
            reasons.append(f"{prefix}.ingestion.compat_ms must be positive and finite")
        if single is None:
            reasons.append(f"{prefix}.ingestion.single_ms must be non-negative and finite")
        if ratio is None:
            reasons.append(f"{prefix}.ingestion.ratio must be positive and finite")
        if compat is not None and single is not None and ratio is not None:
            calculated = single / compat
            if not math.isclose(ratio, calculated, rel_tol=1e-6, abs_tol=1e-6):
                reasons.append(f"{prefix}.ingestion.ratio does not match measured bytes/time")
            if limits is not None and ratio > float(limits[f"{state_name}_ingestion"]) + 1e-9:
                reasons.append(f"{prefix}.ingestion.ratio exceeds frozen calibration limit")

    storage = state.get("storage")
    if not isinstance(storage, dict):
        reasons.append(f"{prefix}.storage must be an object")
        return
    db_bytes = exact_nonnegative_int(storage.get("compat_db_bytes"))
    zova_bytes = exact_nonnegative_int(storage.get("compat_zova_bytes"))
    single_bytes = exact_nonnegative_int(storage.get("single_bytes"))
    page_count = exact_nonnegative_int(storage.get("page_count"), positive=True)
    freelist = exact_nonnegative_int(storage.get("freelist_count"))
    wal_bytes = exact_nonnegative_int(storage.get("wal_bytes"))
    ratio = finite_number(storage.get("ratio"), positive=True)
    for field, value in (
        ("compat_db_bytes", db_bytes), ("compat_zova_bytes", zova_bytes),
        ("single_bytes", single_bytes), ("page_count", page_count),
        ("freelist_count", freelist), ("wal_bytes", wal_bytes),
    ):
        if value is None:
            reasons.append(f"{prefix}.storage.{field} has an invalid integer value")
    if ratio is None:
        reasons.append(f"{prefix}.storage.ratio must be positive and finite")
    if db_bytes is not None and zova_bytes is not None and single_bytes is not None and ratio is not None:
        denominator = db_bytes + zova_bytes
        if denominator <= 0:
            reasons.append(f"{prefix}.storage compatibility bytes must be positive")
        else:
            calculated = single_bytes / denominator
            if not math.isclose(ratio, calculated, rel_tol=1e-6, abs_tol=1e-6):
                reasons.append(f"{prefix}.storage.ratio does not match measured bytes/time")
            if limits is not None and ratio > float(limits[f"{state_name}_storage"]) + 1e-9:
                reasons.append(f"{prefix}.storage.ratio exceeds frozen calibration limit")


def validate_state(
    state: object, expected_name: str, limits: dict[str, Any] | None,
    index: int,
) -> list[str]:
    prefix = f"states[{index}]"
    reasons: list[str] = []
    if not isinstance(state, dict):
        return [f"{prefix} must be an object"]
    if state.get("name") != expected_name:
        reasons.append(f"{prefix}.name must be {expected_name}")
    if state.get("passed") is not True:
        reasons.append(f"{prefix}.passed must be true")
    for field in ZERO_FIELDS:
        if type(state.get(field)) is not int or state.get(field) != 0:
            reasons.append(f"{prefix}.{field}={state.get(field)!r}, expected integer 0")
    for field, minimum in POSITIVE_COUNTS.items():
        value = exact_nonnegative_int(state.get(field), positive=True)
        if value is None or value < minimum:
            reasons.append(f"{prefix}.{field} must be an integer >= {minimum}")
    generation = state.get("generation")
    if not isinstance(generation, dict):
        reasons.append(f"{prefix}.generation must be an object")
    else:
        if exact_nonnegative_int(generation.get("active"), positive=True) is None:
            reasons.append(f"{prefix}.generation.active must be a positive integer")
        if generation.get("integrity_matches") is not True:
            reasons.append(f"{prefix}.generation.integrity_matches must be true")
    validate_performance(state, reasons, prefix)
    validate_overhead(state, expected_name, limits, reasons, prefix)
    return reasons


def validate_candidate_report(name: str, data: dict[str, Any], build_sha: str) -> list[str]:
    reasons: list[str] = []
    if name != "tops" or data.get("repository") != "tops":
        reasons.append("calibration candidate must be named tops and report repository tops")
    if data.get("schema_version") != SCHEMA_VERSION:
        reasons.append(f"schema_version must be integer {SCHEMA_VERSION}")
    if type(data.get("attempt")) is not int or data.get("attempt") != 0:
        reasons.append("calibration candidate attempt must equal integer 0")
    if data.get("calibration_mode") is not True:
        reasons.append("calibration candidate calibration_mode must be true")
    if data.get("build_sha256") != build_sha or not HEX64.fullmatch(build_sha):
        reasons.append("calibration candidate build_sha256 mismatch")
    if not isinstance(data.get("source_commit"), str) or not HEX40.fullmatch(data["source_commit"]):
        reasons.append("calibration candidate source_commit must be 40 lowercase hex")
    if not isinstance(data.get("run_id"), str) or not RUN_ID.fullmatch(data["run_id"]):
        reasons.append("calibration candidate run_id is invalid")
    if data.get("flagged_full_authority") is not True or data.get("passed") is not True:
        reasons.append("calibration candidate must be passing flagged full authority")
    states = data.get("states")
    if not isinstance(states, list) or len(states) != 2:
        reasons.append("calibration candidate states must contain full and incremental")
    else:
        for index, expected in enumerate(STATE_ORDER):
            reasons.extend(validate_state(states[index], expected, None, index))
    return reasons


def validate_repository(
    name: str, path: Path, calibration: dict[str, Any], focused: dict[str, Any], attempt: int,
) -> dict[str, Any]:
    item: dict[str, Any] = {
        "repo": name, "path": str(path), "passed": False, "reasons": []
    }
    data, error = load_json(path)
    if error:
        item["reasons"].append(error)
        return item
    assert data is not None
    if not path_is_fresh(path):
        item["reasons"].append("report path must not reuse latest")
    if data.get("schema_version") != SCHEMA_VERSION:
        item["reasons"].append(f"schema_version must be integer {SCHEMA_VERSION}")
    if data.get("repository") != name:
        item["reasons"].append("repository name does not match report input")
    source_commit = data.get("source_commit")
    if not isinstance(source_commit, str) or not HEX40.fullmatch(source_commit):
        item["reasons"].append("source_commit must be 40 lowercase hex")
    run_id = data.get("run_id")
    if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
        item["reasons"].append("run_id must match [A-Za-z0-9._-]+")
    if type(data.get("attempt")) is not int or data.get("attempt") != attempt:
        item["reasons"].append(f"attempt must equal integer {attempt}")
    if data.get("calibration_mode") is not False:
        item["reasons"].append("official report calibration_mode must be false")
    if data.get("build_sha256") != calibration.get("build_sha256"):
        item["reasons"].append("build_sha256 must match calibration")
    if data.get("calibration_sha256") != calibration.get("digest"):
        item["reasons"].append("calibration_sha256 must match calibration digest")
    if data.get("focused_evidence_sha256") != focused.get("digest"):
        item["reasons"].append("focused_evidence_sha256 must match focused digest")
    if data.get("flagged_full_authority") is not True:
        item["reasons"].append("flagged_full_authority must be true")
    if data.get("passed") is not True:
        item["reasons"].append("report passed must be true")
    states = data.get("states")
    if not isinstance(states, list) or len(states) != 2:
        item["reasons"].append("states must contain exactly full and incremental")
    else:
        for index, expected in enumerate(STATE_ORDER):
            item["reasons"].extend(
                validate_state(states[index], expected, calibration.get("limits"), index)
            )
    item.update({
        "run_id": run_id,
        "source_commit": source_commit,
        "attempt": data.get("attempt"),
        "build_sha256": data.get("build_sha256"),
        "calibration_sha256": data.get("calibration_sha256"),
        "focused_evidence_sha256": data.get("focused_evidence_sha256"),
        "report_sha256": file_digest(path),
    })
    item["passed"] = not item["reasons"]
    return item


def write_decision(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def calibration_mode(args: argparse.Namespace, inputs: list[tuple[str, Path]]) -> int:
    if args.output.exists():
        print(f"error: calibration output already exists: {args.output}", file=sys.stderr)
        return 1
    decision: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION, "passed": False, "reasons": []
    }
    if len(inputs) != CALIBRATION_SAMPLE_COUNT:
        decision["reasons"].append(
            f"calibration requires exactly {CALIBRATION_SAMPLE_COUNT} tops reports"
        )
    loaded: list[tuple[Path, dict[str, Any]]] = []
    paths: set[Path] = set()
    for name, path in inputs:
        resolved = path.resolve()
        if resolved in paths:
            decision["reasons"].append("calibration report paths must be distinct")
        paths.add(resolved)
        if not path_is_fresh(path):
            decision["reasons"].append("calibration report path must not reuse latest")
        data, error = load_json(path)
        if error:
            decision["reasons"].append(error)
            continue
        assert data is not None
        decision["reasons"].extend(
            validate_candidate_report(name, data, args.build_sha256 or "")
        )
        loaded.append((path, data))
    if len(loaded) == CALIBRATION_SAMPLE_COUNT:
        source_commits = {data.get("source_commit") for _, data in loaded}
        run_ids = {data.get("run_id") for _, data in loaded}
        if len(source_commits) != 1:
            decision["reasons"].append("calibration samples must use one source commit")
        if len(run_ids) != CALIBRATION_SAMPLE_COUNT:
            decision["reasons"].append("calibration sample run IDs must be distinct")
    if len(loaded) == CALIBRATION_SAMPLE_COUNT and not decision["reasons"]:
        samples: list[dict[str, Any]] = []
        for path, data in loaded:
            states = data["states"]
            ratios = {
                "full_ingestion": float(states[0]["ingestion"]["ratio"]),
                "incremental_ingestion": float(states[1]["ingestion"]["ratio"]),
                "full_storage": float(states[0]["storage"]["ratio"]),
                "incremental_storage": float(states[1]["storage"]["ratio"]),
            }
            samples.append({
                "run_id": data["run_id"],
                "report_sha256": file_digest(path),
                "ratios": ratios,
            })
        observed = {key: max(sample["ratios"][key] for sample in samples)
                    for key in RATIO_KEYS}
        limits: dict[str, float] = {}
        for key, ratio in observed.items():
            limit = calibrated_limit(ratio)
            if limit is None:
                decision["reasons"].append(
                    f"{key} calibration requires a limit above the {OVERHEAD_HARD_CAP:.2f} hard cap"
                )
            else:
                limits[key] = limit
        if not decision["reasons"]:
            decision.update({
                "repository": "tops",
                "source_commit": loaded[0][1]["source_commit"],
                "build_sha256": loaded[0][1]["build_sha256"],
                "sample_count": CALIBRATION_SAMPLE_COUNT,
                "samples": samples,
                "observed": observed,
                "limits": limits,
                "passed": True,
            })
    decision["digest"] = canonical_digest(decision)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as target:
        json.dump(decision, target, indent=2, sort_keys=True)
        target.write("\n")
    return 0 if decision["passed"] else 1


def load_official_inputs(
    args: argparse.Namespace,
) -> tuple[dict[str, Any] | None, dict[str, Any] | None, list[str]]:
    reasons: list[str] = []
    if args.calibration is None:
        reasons.append("--calibration is required")
        calibration = None
    else:
        calibration, error = load_json(args.calibration)
        if error:
            reasons.append(f"calibration {error}")
        elif calibration is not None:
            reasons.extend(validate_calibration(calibration))
    if args.focused_evidence is None:
        reasons.append("--focused-evidence is required")
        focused = None
    else:
        focused, error = load_json(args.focused_evidence)
        if error:
            reasons.append(f"focused evidence {error}")
    if calibration is not None and focused is not None:
        reasons.extend(validate_focused(focused, args.attempt, str(calibration.get("build_sha256", ""))))
    return calibration, focused, reasons


def attempt_mode(
    args: argparse.Namespace, inputs: list[tuple[str, Path]], repository_only: bool,
) -> int:
    calibration, focused, reasons = load_official_inputs(args)
    decision: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "type": "repository" if repository_only else "attempt",
        "attempt": args.attempt,
        "passed": False,
        "reasons": list(reasons),
        "reports": [],
    }
    if type(args.attempt) is not int or args.attempt not in ATTEMPT_ORDER:
        decision["reasons"].append("attempt must be one of 1,2,3")
    expected_count = 1 if repository_only else len(REPOSITORY_ORDER)
    if len(inputs) != expected_count:
        decision["reasons"].append(f"expected {expected_count} report inputs")
    if not repository_only and tuple(name for name, _ in inputs) != REPOSITORY_ORDER:
        decision["reasons"].append("reports must be ordered tops,motive,rvault,CBM")
    if calibration is not None and focused is not None and args.attempt in ATTEMPT_ORDER:
        decision["reports"] = [
            validate_repository(name, path, calibration, focused, args.attempt)
            for name, path in inputs
        ]
        if any(not report["passed"] for report in decision["reports"]):
            decision["reasons"].append("at least one repository report failed")
        run_ids = [report.get("run_id") for report in decision["reports"]]
        if len(set(run_ids)) != len(run_ids):
            decision["reasons"].append("repository run IDs must be unique")
        paths = [report.get("path") for report in decision["reports"]]
        if len(set(paths)) != len(paths):
            decision["reasons"].append("repository report paths must be unique")
        decision.update({
            "build_sha256": calibration.get("build_sha256"),
            "calibration_sha256": calibration.get("digest"),
            "focused_evidence_sha256": focused.get("digest"),
        })
    decision["passed"] = not decision["reasons"]
    write_decision(args.output, decision)
    return 0 if decision["passed"] else 1


def aggregate_mode(args: argparse.Namespace, attempts: list[tuple[str, Path]]) -> int:
    decision: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "type": "aggregate",
        "passed": False,
        "reasons": [],
        "attempts": [],
    }
    names: list[int] = []
    for name, _ in attempts:
        try:
            names.append(int(name))
        except ValueError:
            decision["reasons"].append(f"invalid attempt name: {name}")
    if tuple(names) != ATTEMPT_ORDER:
        decision["reasons"].append("attempt reports must be ordered 1,2,3")
    paths = [str(path) for _, path in attempts]
    if len(set(paths)) != len(paths):
        decision["reasons"].append("attempt report paths must be unique")
    if any(not path_is_fresh(path) for _, path in attempts):
        decision["reasons"].append("attempt report paths must not reuse latest")
    for expected, (name, path) in enumerate(attempts, start=1):
        data, error = load_json(path)
        if error:
            decision["attempts"].append({"attempt": name, "path": str(path), "passed": False,
                                         "reasons": [error]})
            continue
        assert data is not None
        item = dict(data)
        item["path"] = str(path)
        if data.get("type") != "attempt" or data.get("passed") is not True:
            decision["reasons"].append(f"attempt {name} is not a passing attempt gate")
        if type(data.get("attempt")) is not int or data.get("attempt") != expected:
            decision["reasons"].append(f"attempt {name} identity mismatch")
        decision["attempts"].append(item)
    if len(decision["attempts"]) != 3:
        decision["reasons"].append("aggregate requires exactly three attempt reports")
    if len(decision["attempts"]) == 3:
        builds = {item.get("build_sha256") for item in decision["attempts"]}
        calibrations = {item.get("calibration_sha256") for item in decision["attempts"]}
        focused = {item.get("focused_evidence_sha256") for item in decision["attempts"]}
        if len(builds) != 1:
            decision["reasons"].append("all attempts must use one build SHA-256")
        if len(calibrations) != 1:
            decision["reasons"].append("all attempts must use one calibration SHA-256")
        if len(focused) != 3:
            decision["reasons"].append("every attempt must use fresh focused evidence")
        all_reports = [
            report for item in decision["attempts"]
            for report in item.get("reports", []) if isinstance(report, dict)
        ]
        run_ids = [report.get("run_id") for report in all_reports]
        report_paths = [report.get("path") for report in all_reports]
        if len(all_reports) != 12:
            decision["reasons"].append("aggregate requires twelve repository reports")
        if len(set(run_ids)) != len(run_ids):
            decision["reasons"].append("all twelve repository run IDs must be unique")
        if len(set(report_paths)) != len(report_paths):
            decision["reasons"].append("all twelve repository report paths must be unique")
        if len(builds) == 1:
            decision["build_sha256"] = next(iter(builds))
        if len(calibrations) == 1:
            decision["calibration_sha256"] = next(iter(calibrations))
    decision["passed"] = not decision["reasons"]
    write_decision(args.output, decision)
    return 0 if decision["passed"] else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--calibrate", action="store_true")
    mode.add_argument("--repository-only", action="store_true")
    mode.add_argument("--aggregate", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--attempt", type=int, default=0)
    parser.add_argument("--build-sha256")
    parser.add_argument("--calibration", type=Path)
    parser.add_argument("--focused-evidence", type=Path)
    parser.add_argument("--report", action="append", default=[])
    parser.add_argument("--attempt-report", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        reports = [parse_named(value, "--report") for value in args.report]
        attempts = [parse_named(value, "--attempt-report") for value in args.attempt_report]
    except ValueError as error:
        parser.error(str(error))
    if args.calibrate:
        return calibration_mode(args, reports)
    if args.aggregate:
        return aggregate_mode(args, attempts)
    return attempt_mode(args, reports, args.repository_only)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
