#!/usr/bin/env python3
"""Regression tests for the single-file Zova gate checker."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts" / "zova-single-file-gate.py"
MIGRATION_ZERO_FIELDS = (
    "migration_workspace_count_mismatches",
    "migration_stable_id_mismatches",
    "migration_graph_checksum_mismatches",
    "migration_vector_inventory_mismatches",
    "migration_metadata_digest_mismatches",
    "migration_fts_result_mismatches",
    "migration_restart_idempotence_failures",
    "migration_source_preservation_failures",
    "migration_rollback_failures",
    "migration_schema_interrupt_failures",
    "migration_cleanup_safety_failures",
)
OPERATIONS_ZERO_FIELDS = (
    "operations_queue_order_failures",
    "operations_reader_visibility_failures",
    "operations_backup_snapshot_mismatches",
    "operations_restore_mismatches",
    "operations_workspace_export_import_mismatches",
    "operations_database_export_import_mismatches",
    "operations_workspace_delete_mismatches",
    "operations_compaction_mismatches",
    "operations_disk_report_failures",
    "operations_workspace_corruption_detection_failures",
    "operations_workspace_rebuild_failures",
    "operations_whole_file_recovery_failures",
    "operations_forward_migration_failures",
    "operations_blast_radius_cross_workspace_failures",
)
OPERATIONS_POSITIVE_FIELDS = (
    "operations_backup_count",
    "operations_restore_count",
    "operations_workspace_export_count",
    "operations_workspace_import_count",
    "operations_database_export_count",
    "operations_database_import_count",
    "operations_confirmed_delete_count",
    "operations_workspace_rebuild_count",
    "operations_whole_file_restore_count",
)


def load_gate_module():
    spec = importlib.util.spec_from_file_location("zova_single_file_gate", GATE)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def valid_report() -> dict:
    return {
        "flagged_full_authority": True,
        "compatibility_artifact_count": 0,
        "unexpected_fallback_count": 0,
        "metadata_hydration_mismatches": 0,
        "fts_bm25_mismatches": 0,
        "public_mcp_search_mismatches": 0,
        "full_incremental_mismatches": 0,
        "workspace_operation_cross_results": 0,
        "workspace_b_digest_mismatches": 0,
        "workspace_delete_mismatches": 0,
        "workspace_id_validation_failures": 0,
        "cypher_result_mismatches": 0,
        "cypher_ordering_mismatches": 0,
        "cypher_unexpected_unsupported": 0,
        "cypher_project_db_fallbacks": 0,
        "cypher_mixed_generation_results": 0,
        "cypher_native_route_count": 1,
        "cypher_compat_route_count": 1,
        "migration_workspace_count_mismatches": 0,
        "migration_stable_id_mismatches": 0,
        "migration_graph_checksum_mismatches": 0,
        "migration_vector_inventory_mismatches": 0,
        "migration_metadata_digest_mismatches": 0,
        "migration_fts_result_mismatches": 0,
        "migration_restart_idempotence_failures": 0,
        "migration_source_preservation_failures": 0,
        "migration_rollback_failures": 0,
        "migration_schema_interrupt_failures": 0,
        "migration_cleanup_safety_failures": 0,
        "migration_version": 1,
        "target_schema_version": 5,
        "migration_state": "retired",
        "migration_noop_count": 1,
        "migration_rollback_route_count": 1,
        **{field: 0 for field in OPERATIONS_ZERO_FIELDS},
        **{field: 1 for field in OPERATIONS_POSITIVE_FIELDS},
        "operations_archive_version": 1,
        "operations_health_state": "healthy",
        "operations_compaction_verified_count": 0,
        "operations_compaction_policy_noop_count": 1,
        "parity": {
            "metadata_mismatches": 0,
            "fts_mismatches": 0,
            "graph_mismatches": 0,
            "vector_mismatches": 0,
            "workspace_cross_results": 0,
        },
        "generation": {
            "workspace_id": "w1_example",
            "active": 1,
            "integrity_matches": True,
        },
        "ingestion": {
            "sqlite_sidecar_ms": 10.0,
            "single_file_publish_ms": 12.0,
            "database_bytes": 1024,
            "wal_peak_bytes": 2048,
            "checkpoint_ms": 0.5,
            "checkpoint_wal_bytes": 0,
        },
        "atomicity": {
            "fault_phases_passed": 8,
            "partial_visibility_failures": 0,
        },
    }


def valid_confirmation_report() -> dict:
    report = {
        "section8_confirmation_only": True,
        "flagged_full_authority": True,
        "compatibility_artifact_count": 0,
        "unexpected_fallback_count": 0,
        "public_confirmation_failures": 0,
        "target_schema_version": 5,
        **{field: 0 for field in OPERATIONS_ZERO_FIELDS},
        **{field: 1 for field in OPERATIONS_POSITIVE_FIELDS},
        "operations_archive_version": 1,
        "operations_health_state": "healthy",
        "operations_compaction_verified_count": 0,
        "operations_compaction_policy_noop_count": 1,
        "generation": {
            "workspace_id": "w1_confirmation",
            "active": 1,
            "integrity_matches": True,
        },
        "ingestion": {
            "sqlite_sidecar_ms": 0.0,
            "single_file_publish_ms": 100.0,
            "database_bytes": 4096,
            "wal_peak_bytes": 0,
            "checkpoint_ms": 0.0,
            "checkpoint_wal_bytes": 0,
        },
    }
    return report


def test_gate_rejects_report_without_flagged_full_authority() -> None:
    report = valid_report()
    report["flagged_full_authority"] = False
    result, decision = invoke({"cbm": report})
    assert result.returncode == 1
    assert decision["passed"] is False


def test_gate_rejects_full_authority_mismatches() -> None:
    for key in (
        "compatibility_artifact_count",
        "unexpected_fallback_count",
        "metadata_hydration_mismatches",
        "fts_bm25_mismatches",
        "public_mcp_search_mismatches",
        "full_incremental_mismatches",
        "workspace_operation_cross_results",
        "workspace_b_digest_mismatches",
        "workspace_delete_mismatches",
        "workspace_id_validation_failures",
        "cypher_result_mismatches",
        "cypher_ordering_mismatches",
        "cypher_unexpected_unsupported",
        "cypher_project_db_fallbacks",
        "cypher_mixed_generation_results",
    ):
        report = valid_report()
        report[key] = 1
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, key
        assert decision["passed"] is False, key


def invoke(
    reports: dict[str, dict], *, require_section8_order: bool = False,
    require_section8_operations: bool = False,
    allow_section8_confirmation_only: bool = False,
) -> tuple[subprocess.CompletedProcess[str], dict]:
    with tempfile.TemporaryDirectory(prefix="cbm-zova-single-file-gate-") as td:
        root = Path(td)
        paths = []
        for name, data in reports.items():
            path = root / f"{name}.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            paths.extend(["--report", f"{name}={path}"])
        output = root / "gate.json"
        order_flag = ["--require-section8-order"] if require_section8_order else []
        operations_flag = (
            ["--require-section8-operations"] if require_section8_operations else []
        )
        confirmation_flag = (
            ["--allow-section8-confirmation-only"]
            if allow_section8_confirmation_only else []
        )
        result = subprocess.run(
            [sys.executable, str(GATE), "--output", str(output), *order_flag,
             *operations_flag, *confirmation_flag, *paths],
            check=False,
            capture_output=True,
            text=True,
        )
        assert output.exists(), result.stderr
        return result, json.loads(output.read_text(encoding="utf-8"))


def test_gate_accepts_valid_compact_report() -> None:
    result, decision = invoke({"cbm": valid_report()})
    assert result.returncode == 0, result.stderr
    assert decision["passed"] is True
    assert decision["reports"][0]["passed"] is True
    assert decision["reports"][0]["ingestion"]["database_bytes"] == 1024


def test_gate_rejects_missing_required_fields() -> None:
    report = valid_report()
    del report["generation"]["integrity_matches"]
    del report["parity"]["fts_mismatches"]
    result, decision = invoke({"cbm": report})
    assert result.returncode == 1
    assert decision["passed"] is False
    reasons = decision["reports"][0]["reasons"]
    assert any("parity.fts_mismatches" in reason for reason in reasons)
    assert any("integrity_matches" in reason for reason in reasons)


def test_gate_rejects_missing_cypher_route_evidence() -> None:
    for key in ("cypher_native_route_count", "cypher_compat_route_count"):
        report = valid_report()
        report[key] = 0
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, key
        assert decision["passed"] is False


def test_gate_rejects_invalid_migration_zero_fields() -> None:
    invalid_values = (False, True, 0.0, "0", -1, 1)
    for key in MIGRATION_ZERO_FIELDS:
        report = valid_report()
        del report[key]
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, f"missing {key}"
        assert any(key in reason for reason in decision["reports"][0]["reasons"]), key

        for value in invalid_values:
            report = valid_report()
            report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key


def test_gate_requires_exact_migration_versions_and_state() -> None:
    invalid_versions = {
        "migration_version": (False, True, 1.0, "1", -1, 0, 2),
        "target_schema_version": (False, True, 5.0, "5", -1, 0, 3, 4, 6),
    }
    for key, values in invalid_versions.items():
        report = valid_report()
        del report[key]
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, f"missing {key}"
        assert any(key in reason for reason in decision["reports"][0]["reasons"]), key

        for value in values:
            report = valid_report()
            report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key

    for value in (None, 1, "active", "rolled_back", "Retired"):
        report = valid_report()
        report["migration_state"] = value
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, f"migration_state accepted {value!r}"
        assert any(
            "migration_state" in reason for reason in decision["reports"][0]["reasons"]
        )
    report = valid_report()
    del report["migration_state"]
    result, decision = invoke({"cbm": report})
    assert result.returncode == 1, "missing migration_state"
    assert any("migration_state" in reason for reason in decision["reports"][0]["reasons"])


def test_gate_requires_positive_migration_lifecycle_evidence() -> None:
    for key in ("migration_noop_count", "migration_rollback_route_count"):
        report = valid_report()
        del report[key]
        result, decision = invoke({"cbm": report})
        assert result.returncode == 1, f"missing {key}"
        assert any(key in reason for reason in decision["reports"][0]["reasons"]), key

        for value in (False, True, 0, -1, 1.0, "1"):
            report = valid_report()
            report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key


def test_gate_rejects_invalid_operations_zero_fields() -> None:
    for key in OPERATIONS_ZERO_FIELDS:
        for value in (None, False, True, 0.0, "0", -1, 1):
            report = valid_report()
            if value is None:
                del report[key]
            else:
                report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key


def test_gate_requires_operations_versions_health_and_positive_counts() -> None:
    for key in OPERATIONS_POSITIVE_FIELDS:
        for value in (None, False, True, 0, -1, 1.0, "1"):
            report = valid_report()
            if value is None:
                del report[key]
            else:
                report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key
    for key, invalid_values in {
        "operations_archive_version": (None, False, True, 0, 1.0, "1", 2),
        "operations_health_state": (None, False, 0, "Healthy", "rebuild_required"),
    }.items():
        for value in invalid_values:
            report = valid_report()
            if value is None:
                del report[key]
            else:
                report[key] = value
            result, decision = invoke({"cbm": report}, require_section8_operations=True)
            assert result.returncode == 1, f"{key} accepted {value!r}"
            assert any(key in reason for reason in decision["reports"][0]["reasons"]), key


def test_gate_requires_exactly_one_valid_compaction_outcome() -> None:
    for verified, noop in ((0, 0), (1, 1), (2, 0), (0, 2), (-1, 2), (False, 1), (1, 1.0)):
        report = valid_report()
        report["operations_compaction_verified_count"] = verified
        report["operations_compaction_policy_noop_count"] = noop
        result, decision = invoke({"cbm": report}, require_section8_operations=True)
        assert result.returncode == 1, (verified, noop)
        assert any("compaction evidence" in reason for reason in decision["reports"][0]["reasons"])


def test_gate_enforces_section8_repository_presence_and_order() -> None:
    ordered = {name: valid_report() for name in ("tops", "motive", "rvault", "CBM")}
    result, decision = invoke(ordered, require_section8_order=True)
    assert result.returncode == 0, decision
    for names in (
        ("tops", "motive", "rvault"),
        ("motive", "tops", "rvault", "CBM"),
        ("tops", "motive", "CBM", "rvault"),
    ):
        reports = {name: valid_report() for name in names}
        result, decision = invoke(reports, require_section8_order=True)
        assert result.returncode == 1, names
        assert any("tops,motive,rvault,CBM" in reason for reason in decision["reasons"])


def test_gate_allows_bounded_confirmation_only_for_final_cbm_report() -> None:
    ordered = {name: valid_report() for name in ("tops", "motive", "rvault")}
    ordered["CBM"] = valid_confirmation_report()
    result, decision = invoke(ordered, require_section8_order=True)
    assert result.returncode == 0, decision
    assert decision["reports"][-1]["section8_confirmation_only"] is True

    for name in ("tops", "motive", "rvault"):
        invalid = {item: valid_report() for item in ("tops", "motive", "rvault", "CBM")}
        invalid[name] = valid_confirmation_report()
        result, decision = invoke(invalid, require_section8_order=True)
        assert result.returncode == 1, name

    result, decision = invoke(
        {"CBM": valid_confirmation_report()}, require_section8_operations=True
    )
    assert result.returncode == 1, decision
    result, decision = invoke(
        {"CBM": valid_confirmation_report()},
        require_section8_operations=True,
        allow_section8_confirmation_only=True,
    )
    assert result.returncode == 0, decision

    for key in (
        "compatibility_artifact_count",
        "unexpected_fallback_count",
        "public_confirmation_failures",
    ):
        invalid = {name: valid_report() for name in ("tops", "motive", "rvault")}
        report = valid_confirmation_report()
        report[key] = 1
        invalid["CBM"] = report
        result, decision = invoke(invalid, require_section8_order=True)
        assert result.returncode == 1, key


def test_gate_rejects_parity_and_atomicity_mismatches() -> None:
    report = valid_report()
    report["parity"]["graph_mismatches"] = 2
    report["atomicity"]["partial_visibility_failures"] = 1
    result, decision = invoke({"cbm": report})
    assert result.returncode == 1
    reasons = decision["reports"][0]["reasons"]
    assert any("graph_mismatches=2" in reason for reason in reasons)
    assert any("partial_visibility_failures=1" in reason for reason in reasons)


def test_gate_rejects_non_finite_ingestion_metric() -> None:
    report = valid_report()
    report["ingestion"]["checkpoint_ms"] = float("inf")
    # JSON encoders emit Infinity by default; the checker must still reject it.
    result, decision = invoke({"cbm": report})
    assert result.returncode == 1
    assert any(
        "ingestion.checkpoint_ms" in reason for reason in decision["reports"][0]["reasons"]
    )


def test_gate_rejects_duplicate_report_names() -> None:
    with tempfile.TemporaryDirectory(prefix="cbm-zova-single-file-gate-") as td:
        root = Path(td)
        first = root / "first.json"
        second = root / "second.json"
        first.write_text(json.dumps(valid_report()), encoding="utf-8")
        second.write_text(json.dumps(valid_report()), encoding="utf-8")
        output = root / "gate.json"
        result = subprocess.run(
            [
                sys.executable,
                str(GATE),
                "--output",
                str(output),
                "--report",
                f"cbm={first}",
                "--report",
                f"cbm={second}",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 1
        decision = json.loads(output.read_text(encoding="utf-8"))
        assert "report names must be unique" in decision["reasons"]


class ZovaSingleFileGateTests(unittest.TestCase):
    def test_accepts_valid_compact_report(self) -> None:
        test_gate_accepts_valid_compact_report()

    def test_rejects_full_authority_contract_violations(self) -> None:
        test_gate_rejects_report_without_flagged_full_authority()
        test_gate_rejects_full_authority_mismatches()
        test_gate_rejects_missing_cypher_route_evidence()

    def test_rejects_migration_contract_violations(self) -> None:
        test_gate_rejects_invalid_migration_zero_fields()
        test_gate_requires_exact_migration_versions_and_state()
        test_gate_requires_positive_migration_lifecycle_evidence()

    def test_rejects_operations_contract_violations(self) -> None:
        test_gate_rejects_invalid_operations_zero_fields()
        test_gate_requires_operations_versions_health_and_positive_counts()
        test_gate_requires_exactly_one_valid_compaction_outcome()
        test_gate_enforces_section8_repository_presence_and_order()
        test_gate_allows_bounded_confirmation_only_for_final_cbm_report()

    def test_rejects_existing_invalid_reports(self) -> None:
        test_gate_rejects_missing_required_fields()
        test_gate_rejects_parity_and_atomicity_mismatches()
        test_gate_rejects_non_finite_ingestion_metric()
        test_gate_rejects_duplicate_report_names()


if __name__ == "__main__":
    test_gate_accepts_valid_compact_report()
    test_gate_rejects_report_without_flagged_full_authority()
    test_gate_rejects_full_authority_mismatches()
    test_gate_rejects_missing_cypher_route_evidence()
    test_gate_rejects_invalid_migration_zero_fields()
    test_gate_requires_exact_migration_versions_and_state()
    test_gate_requires_positive_migration_lifecycle_evidence()
    test_gate_rejects_invalid_operations_zero_fields()
    test_gate_requires_operations_versions_health_and_positive_counts()
    test_gate_requires_exactly_one_valid_compaction_outcome()
    test_gate_enforces_section8_repository_presence_and_order()
    test_gate_allows_bounded_confirmation_only_for_final_cbm_report()
    test_gate_rejects_missing_required_fields()
    test_gate_rejects_parity_and_atomicity_mismatches()
    test_gate_rejects_non_finite_ingestion_metric()
    test_gate_rejects_duplicate_report_names()
    print("PASS: zova single-file gate")
