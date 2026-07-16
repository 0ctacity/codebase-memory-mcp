#!/usr/bin/env python3
"""Regression tests for the Section 9 full-authority promotion gate."""

from __future__ import annotations

import copy
import hashlib
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts" / "zova-full-authority-promotion-gate.py"
BUILD_SHA = "a" * 64
RUNNER_SHA = "b" * 64
LOG_SHA = "c" * 64
SOURCE_SHA = "d" * 40
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
PROOF_KEYS = (
    "cancellation",
    "publication_crash_recovery",
    "migration",
    "backup_restore",
    "workspace_deletion",
    "corruption_recovery",
)


def digest_object(value: dict) -> str:
    payload = dict(value)
    payload.pop("digest", None)
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def valid_focused(attempt: int) -> dict:
    value = {
        "schema_version": 1,
        "attempt": attempt,
        "passed": True,
        "build_sha256": BUILD_SHA,
        "test_runner_sha256": RUNNER_SHA,
        "suite_log_sha256": LOG_SHA,
        "proofs": {key: True for key in PROOF_KEYS},
    }
    value["digest"] = digest_object(value)
    return value


def valid_state(name: str) -> dict:
    return {
        "name": name,
        "passed": True,
        **{field: 0 for field in ZERO_FIELDS},
        "fts_query_count": 20,
        "graph_sample_count": 10,
        "vector_query_count": 4,
        "public_mcp_case_count": 8,
        "cypher_native_route_count": 5,
        "cypher_compat_route_count": 9,
        "generation": {"active": 1, "integrity_matches": True},
        "performance": {
            "sample_count": 20,
            "graph": {
                "compat_p50_ms": 10.0,
                "compat_p95_ms": 20.0,
                "single_p50_ms": 10.5,
                "single_p95_ms": 21.0,
            },
            "vector": {
                "compat_p50_ms": 10.0,
                "compat_p95_ms": 20.0,
                "single_p50_ms": 10.5,
                "single_p95_ms": 21.0,
            },
        },
        "ingestion": {"compat_ms": 100.0, "single_ms": 105.0, "ratio": 1.05},
        "storage": {
            "compat_db_bytes": 1000,
            "compat_zova_bytes": 1000,
            "single_bytes": 2100,
            "ratio": 1.05,
            "page_count": 1,
            "freelist_count": 0,
            "wal_bytes": 0,
        },
    }


def calibration_candidate(ingestion_ratio: float = 1.041, sample: int = 1) -> dict:
    full = valid_state("full")
    incremental = valid_state("incremental")
    for state in (full, incremental):
        state["ingestion"] = {
            "compat_ms": 1000.0,
            "single_ms": 1000.0 * ingestion_ratio,
            "ratio": ingestion_ratio,
        }
    return {
        "schema_version": 1,
        "repository": "tops",
        "source_commit": SOURCE_SHA,
        "run_id": f"calibration-run-{sample}",
        "attempt": 0,
        "calibration_mode": True,
        "build_sha256": BUILD_SHA,
        "flagged_full_authority": True,
        "passed": True,
        "states": [full, incremental],
    }


def valid_calibration() -> dict:
    ratios = {key: 1.05 for key in RATIO_KEYS}
    value = {
        "schema_version": 1,
        "repository": "tops",
        "source_commit": SOURCE_SHA,
        "build_sha256": BUILD_SHA,
        "sample_count": 5,
        "samples": [
            {"run_id": f"calibration-run-{sample}", "report_sha256": f"{sample}" * 64,
             "ratios": dict(ratios)}
            for sample in range(1, 6)
        ],
        "observed": ratios,
        "limits": {key: 1.10 for key in RATIO_KEYS},
    }
    value["digest"] = digest_object(value)
    return value


def valid_report(name: str, attempt: int, calibration: dict, focused: dict) -> dict:
    return {
        "schema_version": 1,
        "repository": name,
        "source_commit": SOURCE_SHA,
        "run_id": f"attempt-{attempt}-{name}",
        "attempt": attempt,
        "calibration_mode": False,
        "build_sha256": BUILD_SHA,
        "calibration_sha256": calibration["digest"],
        "focused_evidence_sha256": focused["digest"],
        "flagged_full_authority": True,
        "passed": True,
        "states": [valid_state("full"), valid_state("incremental")],
    }


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def run_gate(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(GATE), *args],
        check=False,
        capture_output=True,
        text=True,
    )


def invoke_calibrate(reports: list[dict]) -> tuple[subprocess.CompletedProcess[str], dict | None]:
    with tempfile.TemporaryDirectory(prefix="cbm-section9-calibrate-") as td:
        root = Path(td)
        output = root / "calibration.json"
        args = [
            "--calibrate", "--build-sha256", BUILD_SHA,
            "--output", str(output),
        ]
        for sample, report in enumerate(reports, 1):
            report_path = root / f"tops-report-{sample}.json"
            write_json(report_path, report)
            args.extend(["--report", f"tops={report_path}"])
        result = run_gate(args)
        value = json.loads(output.read_text()) if output.exists() else None
        return result, value


def invoke_repository(
    report: dict,
    calibration: dict | None = None,
    focused: dict | None = None,
    name: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], dict]:
    calibration = calibration or valid_calibration()
    focused = focused or valid_focused(int(report.get("attempt", 1)))
    name = name or str(report.get("repository", "tops"))
    with tempfile.TemporaryDirectory(prefix="cbm-section9-repository-") as td:
        root = Path(td)
        report_path = root / "runs" / str(report.get("run_id", "run")) / "report.json"
        calibration_path = root / "calibration.json"
        focused_path = root / "focused.json"
        output = root / "decision.json"
        write_json(report_path, report)
        write_json(calibration_path, calibration)
        write_json(focused_path, focused)
        result = run_gate([
            "--repository-only", "--attempt", str(report.get("attempt", 1)),
            "--calibration", str(calibration_path),
            "--focused-evidence", str(focused_path),
            "--output", str(output), "--report", f"{name}={report_path}",
        ])
        assert output.exists(), result.stderr
        return result, json.loads(output.read_text())


def invoke_attempt(
    reports: list[tuple[str, dict]], attempt: int,
    calibration: dict | None = None, focused: dict | None = None,
    use_latest: bool = False,
) -> tuple[subprocess.CompletedProcess[str], dict]:
    calibration = calibration or valid_calibration()
    focused = focused or valid_focused(attempt)
    with tempfile.TemporaryDirectory(prefix="cbm-section9-attempt-") as td:
        root = Path(td)
        calibration_path = root / "calibration.json"
        focused_path = root / "focused.json"
        output = root / "attempt-gate.json"
        write_json(calibration_path, calibration)
        write_json(focused_path, focused)
        args = [
            "--attempt", str(attempt), "--calibration", str(calibration_path),
            "--focused-evidence", str(focused_path), "--output", str(output),
        ]
        for name, report in reports:
            middle = "latest" if use_latest else str(report["run_id"])
            path = root / "runs" / middle / f"{name}.json"
            write_json(path, report)
            args.extend(["--report", f"{name}={path}"])
        result = run_gate(args)
        assert output.exists(), result.stderr
        return result, json.loads(output.read_text())


def valid_attempt_gate(attempt: int, calibration: dict) -> dict:
    focused = valid_focused(attempt)
    reports = [
        (name, valid_report(name, attempt, calibration, focused))
        for name in ("tops", "motive", "rvault", "CBM")
    ]
    result, decision = invoke_attempt(reports, attempt, calibration, focused)
    assert result.returncode == 0, decision
    return decision


def invoke_aggregate(
    gates: list[tuple[int, dict]], duplicate_path: bool = False,
) -> tuple[subprocess.CompletedProcess[str], dict]:
    with tempfile.TemporaryDirectory(prefix="cbm-section9-aggregate-") as td:
        root = Path(td)
        output = root / "aggregate.json"
        args = ["--aggregate", "--output", str(output)]
        shared = root / "attempt-gate.json"
        for attempt, gate in gates:
            path = shared if duplicate_path else root / f"attempt-{attempt}" / "gate.json"
            write_json(path, gate)
            args.extend(["--attempt-report", f"{attempt}={path}"])
        result = run_gate(args)
        assert output.exists(), result.stderr
        return result, json.loads(output.read_text())


def test_calibration_formula_and_hard_cap() -> None:
    result, value = invoke_calibrate([
        calibration_candidate(0.70, sample) for sample in range(1, 6)
    ])
    assert result.returncode == 0, result.stderr
    assert value is not None
    assert value["observed"]["full_ingestion"] == 0.70
    assert value["limits"]["full_ingestion"] == 1.00
    assert value["limits"]["incremental_ingestion"] == 1.00

    result, value = invoke_calibrate([
        calibration_candidate(1.041, 1),
        calibration_candidate(1.08, 2),
        calibration_candidate(1.06, 3),
        calibration_candidate(1.09, 4),
        calibration_candidate(1.07, 5),
    ])
    assert result.returncode == 0, result.stderr
    assert value is not None
    assert value["sample_count"] == 5
    assert len(value["samples"]) == 5
    assert value["observed"]["full_ingestion"] == 1.09
    assert value["limits"]["full_ingestion"] == 1.14
    assert value["limits"]["incremental_ingestion"] == 1.14
    assert value["digest"] == digest_object(value)

    result, value = invoke_calibrate([
        calibration_candidate(1.10, 1),
        calibration_candidate(1.201, 2),
        calibration_candidate(1.15, 3),
        calibration_candidate(1.18, 4),
        calibration_candidate(1.19, 5),
    ])
    assert result.returncode == 1
    assert value is not None and value["passed"] is False
    assert any("1.25" in reason for reason in value["reasons"])


def test_calibration_rejects_wrong_identity_and_overwrite() -> None:
    reports = [calibration_candidate(1.041, sample) for sample in range(1, 6)]
    reports[1]["repository"] = "motive"
    result, _ = invoke_calibrate(reports)
    assert result.returncode == 1

    result, value = invoke_calibrate(reports[:4])
    assert result.returncode == 1
    assert value is not None and any("exactly 5" in reason for reason in value["reasons"])

    with tempfile.TemporaryDirectory(prefix="cbm-section9-calibrate-overwrite-") as td:
        root = Path(td)
        report_path = root / "report.json"
        output = root / "calibration.json"
        write_json(report_path, calibration_candidate())
        output.write_text("sentinel", encoding="utf-8")
        result = run_gate([
            "--calibrate", "--build-sha256", BUILD_SHA,
            "--output", str(output),
            "--report", f"tops={report_path}",
            "--report", f"tops={report_path}",
            "--report", f"tops={report_path}",
            "--report", f"tops={report_path}",
            "--report", f"tops={report_path}",
        ])
        assert result.returncode == 1
        assert output.read_text(encoding="utf-8") == "sentinel"


def test_repository_accepts_valid_report() -> None:
    calibration = valid_calibration()
    focused = valid_focused(1)
    report = valid_report("tops", 1, calibration, focused)
    result, decision = invoke_repository(report, calibration, focused)
    assert result.returncode == 0, decision
    assert decision["passed"] is True


def test_repository_rejects_every_zero_field_violation() -> None:
    calibration = valid_calibration()
    focused = valid_focused(1)
    for field in ZERO_FIELDS:
        for value in (None, False, 0.0, "0", -1, 1):
            report = valid_report("tops", 1, calibration, focused)
            if value is None:
                del report["states"][0][field]
            else:
                report["states"][0][field] = value
            result, decision = invoke_repository(report, calibration, focused)
            assert result.returncode == 1, (field, value, decision)
            assert any(field in reason for reason in decision["reports"][0]["reasons"])


def test_repository_rejects_counts_identity_and_digest_errors() -> None:
    calibration = valid_calibration()
    focused = valid_focused(1)
    mutations = (
        ("fts_query_count", 19),
        ("graph_sample_count", 0),
        ("vector_query_count", False),
        ("public_mcp_case_count", "8"),
        ("cypher_native_route_count", 0),
        ("cypher_compat_route_count", 0),
    )
    for field, value in mutations:
        report = valid_report("tops", 1, calibration, focused)
        report["states"][0][field] = value
        result, _ = invoke_repository(report, calibration, focused)
        assert result.returncode == 1, (field, value)

    for field, value in (
        ("source_commit", "not-a-sha"),
        ("run_id", "bad run id"),
        ("build_sha256", "f" * 63),
        ("calibration_sha256", "0" * 64),
        ("focused_evidence_sha256", "0" * 64),
        ("flagged_full_authority", False),
        ("passed", False),
    ):
        report = valid_report("tops", 1, calibration, focused)
        report[field] = value
        result, _ = invoke_repository(report, calibration, focused)
        assert result.returncode == 1, field

    report = valid_report("tops", 1, calibration, focused)
    report["states"][0]["generation"]["integrity_matches"] = False
    result, _ = invoke_repository(report, calibration, focused)
    assert result.returncode == 1


def test_repository_rejects_invalid_performance_and_overhead() -> None:
    calibration = valid_calibration()
    focused = valid_focused(1)
    for section in ("graph", "vector"):
        for field, value in (
            ("compat_p50_ms", 0.0),
            ("compat_p95_ms", False),
            ("single_p50_ms", float("nan")),
            ("single_p95_ms", 21.01),
        ):
            report = valid_report("tops", 1, calibration, focused)
            report["states"][0]["performance"][section][field] = value
            result, _ = invoke_repository(report, calibration, focused)
            assert result.returncode == 1, (section, field, value)

    report = valid_report("tops", 1, calibration, focused)
    report["states"][0]["ingestion"]["ratio"] = 1.11
    report["states"][0]["ingestion"]["single_ms"] = 111.0
    result, _ = invoke_repository(report, calibration, focused)
    assert result.returncode == 1

    report = valid_report("tops", 1, calibration, focused)
    report["states"][0]["storage"]["ratio"] = 1.11
    report["states"][0]["storage"]["single_bytes"] = 2220
    result, _ = invoke_repository(report, calibration, focused)
    assert result.returncode == 1

    report = valid_report("tops", 1, calibration, focused)
    report["states"][0]["ingestion"]["ratio"] = 1.04
    result, _ = invoke_repository(report, calibration, focused)
    assert result.returncode == 1


def test_attempt_requires_exact_order_fresh_paths_and_unique_ids() -> None:
    calibration = valid_calibration()
    focused = valid_focused(1)
    ordered = [
        (name, valid_report(name, 1, calibration, focused))
        for name in ("tops", "motive", "rvault", "CBM")
    ]
    result, decision = invoke_attempt(ordered, 1, calibration, focused)
    assert result.returncode == 0, decision

    wrong_order = [ordered[1], ordered[0], ordered[2], ordered[3]]
    result, _ = invoke_attempt(wrong_order, 1, calibration, focused)
    assert result.returncode == 1

    result, _ = invoke_attempt(ordered, 1, calibration, focused, use_latest=True)
    assert result.returncode == 1

    duplicate = copy.deepcopy(ordered)
    duplicate[1][1]["run_id"] = duplicate[0][1]["run_id"]
    result, _ = invoke_attempt(duplicate, 1, calibration, focused)
    assert result.returncode == 1


def test_aggregate_requires_three_fresh_consistent_attempts() -> None:
    calibration = valid_calibration()
    gates = [(attempt, valid_attempt_gate(attempt, calibration)) for attempt in (1, 2, 3)]
    result, decision = invoke_aggregate(gates)
    assert result.returncode == 0, decision
    assert decision["passed"] is True

    result, _ = invoke_aggregate(gates[:2])
    assert result.returncode == 1

    result, _ = invoke_aggregate([gates[1], gates[0], gates[2]])
    assert result.returncode == 1

    result, _ = invoke_aggregate(gates, duplicate_path=True)
    assert result.returncode == 1

    inconsistent = copy.deepcopy(gates)
    inconsistent[2][1]["build_sha256"] = "e" * 64
    result, _ = invoke_aggregate(inconsistent)
    assert result.returncode == 1


def main() -> None:
    test_calibration_formula_and_hard_cap()
    test_calibration_rejects_wrong_identity_and_overwrite()
    test_repository_accepts_valid_report()
    test_repository_rejects_every_zero_field_violation()
    test_repository_rejects_counts_identity_and_digest_errors()
    test_repository_rejects_invalid_performance_and_overhead()
    test_attempt_requires_exact_order_fresh_paths_and_unique_ids()
    test_aggregate_requires_three_fresh_consistent_attempts()
    print("PASS: Zova Section 9 full-authority promotion gate")


if __name__ == "__main__":
    main()
