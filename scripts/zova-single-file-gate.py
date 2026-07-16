#!/usr/bin/env python3
"""Evaluate compact reports from the experimental single-file Zova gate.

This checker deliberately has no performance threshold.  The single-file
development slice first establishes exact parity and crash atomicity; the
ingestion and storage measurements are retained for a later promotion rule.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


PARITY_FIELDS = (
    "metadata_mismatches",
    "fts_mismatches",
    "graph_mismatches",
    "vector_mismatches",
    "workspace_cross_results",
)
INGESTION_FIELDS = (
    "sqlite_sidecar_ms",
    "single_file_publish_ms",
    "database_bytes",
    "wal_peak_bytes",
    "checkpoint_ms",
    "checkpoint_wal_bytes",
)
FULL_AUTHORITY_ZERO_FIELDS = (
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
)
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
SECTION8_REPOSITORY_ORDER = ("tops", "motive", "rvault", "CBM")

# Fresh Section 8 regression reports use the current schema. Retained Section 7
# reports with target_schema_version=4 remain historical evidence and are not
# re-evaluated as fresh Section 8 runs.
CURRENT_TARGET_SCHEMA_VERSION = 5


def parse_named_report(value: str) -> tuple[str, Path]:
    """Parse NAME=PATH, allowing paths that contain '=' after the first one."""
    if "=" not in value:
        raise ValueError("--report must use NAME=PATH")
    name, path = value.split("=", 1)
    if not name or not path:
        raise ValueError("--report must use NAME=PATH")
    return name, Path(path)


def report_name(path: Path) -> str:
    return path.stem


def load_report(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, f"unreadable report: {error}"
    if not isinstance(value, dict):
        return None, "report root must be an object"
    return value, None


def finite_nonnegative(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    return math.isfinite(float(value)) and float(value) >= 0.0


def nonnegative_integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def validate_operations_fields(data: dict[str, Any], item: dict[str, Any]) -> None:
    for field in OPERATIONS_ZERO_FIELDS:
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value != 0:
            item["reasons"].append(f"{field}={value!r}, expected integer 0")
    for field in OPERATIONS_POSITIVE_FIELDS:
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value <= 0:
            item["reasons"].append(f"{field}={value!r}, expected positive integer")
    for field, expected in (
        ("operations_archive_version", 1),
        ("target_schema_version", CURRENT_TARGET_SCHEMA_VERSION),
    ):
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value != expected:
            item["reasons"].append(f"{field}={value!r}, expected integer {expected}")
    health = data.get("operations_health_state")
    item["operations_health_state"] = health
    if type(health) is not str or health != "healthy":
        item["reasons"].append(
            f"operations_health_state={health!r}, expected string 'healthy'"
        )
    compact_verified = data.get("operations_compaction_verified_count")
    compact_noop = data.get("operations_compaction_policy_noop_count")
    item["operations_compaction_verified_count"] = compact_verified
    item["operations_compaction_policy_noop_count"] = compact_noop
    if (type(compact_verified) is not int or compact_verified < 0 or
            type(compact_noop) is not int or compact_noop < 0 or
            compact_verified + compact_noop != 1):
        item["reasons"].append(
            "operations compaction evidence must be exactly one verified run or policy no-op"
        )


def validate_report(
    name: str, path: Path, *, require_section8_operations: bool = False,
    allow_confirmation_only: bool = False,
) -> dict[str, Any]:
    """Validate one compact report and return a serializable decision item."""
    item: dict[str, Any] = {
        "repo": name,
        "path": str(path),
        "passed": False,
        "reasons": [],
    }
    data, error = load_report(path)
    if error:
        item["reasons"].append(error)
        return item

    if data.get("section8_confirmation_only") is True:
        item["section8_confirmation_only"] = True
        if not allow_confirmation_only or name != "CBM":
            item["reasons"].append(
                "section8_confirmation_only is allowed only for the final ordered CBM report"
            )
        if data.get("flagged_full_authority") is not True:
            item["reasons"].append("flagged_full_authority must be true")
        item["flagged_full_authority"] = data.get("flagged_full_authority")
        for field in (
            "compatibility_artifact_count",
            "unexpected_fallback_count",
            "public_confirmation_failures",
        ):
            value = data.get(field)
            item[field] = value
            if type(value) is not int or value != 0:
                item["reasons"].append(f"{field}={value!r}, expected integer 0")
        if data.get("target_schema_version") != CURRENT_TARGET_SCHEMA_VERSION:
            item["reasons"].append(
                f"target_schema_version={data.get('target_schema_version')!r}, "
                f"expected integer {CURRENT_TARGET_SCHEMA_VERSION}"
            )
        validate_operations_fields(data, item)
        generation = data.get("generation")
        ingestion = data.get("ingestion")
        if not isinstance(generation, dict):
            item["reasons"].append("missing generation object")
        else:
            item["generation"] = {
                "workspace_id": generation.get("workspace_id"),
                "active": generation.get("active"),
                "integrity_matches": generation.get("integrity_matches"),
            }
            if not isinstance(generation.get("workspace_id"), str) or not generation.get(
                "workspace_id"
            ):
                item["reasons"].append("generation.workspace_id must be a non-empty string")
            if not nonnegative_integer(generation.get("active")):
                item["reasons"].append("generation.active must be a non-negative integer")
            if generation.get("integrity_matches") is not True:
                item["reasons"].append("generation.integrity_matches must be true")
        if not isinstance(ingestion, dict):
            item["reasons"].append("missing ingestion object")
        else:
            item["ingestion"] = {field: ingestion.get(field) for field in INGESTION_FIELDS}
            for field in INGESTION_FIELDS:
                if not finite_nonnegative(ingestion.get(field)):
                    item["reasons"].append(
                        f"ingestion.{field} must be a non-negative finite number"
                    )
        item["passed"] = not item["reasons"]
        return item

    if data.get("flagged_full_authority") is not True:
        item["reasons"].append("flagged_full_authority must be true")
    item["flagged_full_authority"] = data.get("flagged_full_authority")
    for field in FULL_AUTHORITY_ZERO_FIELDS:
        value = data.get(field)
        item[field] = value
        if not isinstance(value, int) or isinstance(value, bool) or value != 0:
            item["reasons"].append(f"{field}={value!r}, expected integer 0")
    for field in ("cypher_native_route_count", "cypher_compat_route_count"):
        value = data.get(field)
        item[field] = value
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            item["reasons"].append(f"{field}={value!r}, expected positive integer")

    for field in MIGRATION_ZERO_FIELDS:
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value != 0:
            item["reasons"].append(f"{field}={value!r}, expected integer 0")
    for field, expected in (
        ("migration_version", 1),
        ("target_schema_version", CURRENT_TARGET_SCHEMA_VERSION),
    ):
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value != expected:
            item["reasons"].append(f"{field}={value!r}, expected integer {expected}")
    state = data.get("migration_state")
    item["migration_state"] = state
    if type(state) is not str or state != "retired":
        item["reasons"].append(
            f"migration_state={state!r}, expected string 'retired'"
        )
    for field in ("migration_noop_count", "migration_rollback_route_count"):
        value = data.get(field)
        item[field] = value
        if type(value) is not int or value <= 0:
            item["reasons"].append(f"{field}={value!r}, expected positive integer")

    if require_section8_operations:
        validate_operations_fields(data, item)

    parity = data.get("parity")
    generation = data.get("generation")
    ingestion = data.get("ingestion")
    atomicity = data.get("atomicity")
    for section_name, section in (
        ("parity", parity),
        ("generation", generation),
        ("ingestion", ingestion),
        ("atomicity", atomicity),
    ):
        if not isinstance(section, dict):
            item["reasons"].append(f"missing {section_name} object")

    if isinstance(parity, dict):
        item["parity"] = {field: parity.get(field) for field in PARITY_FIELDS}
        for field in PARITY_FIELDS:
            value = parity.get(field)
            if not isinstance(value, int) or isinstance(value, bool):
                item["reasons"].append(f"parity.{field} must be an integer equal to 0")
            elif value != 0:
                item["reasons"].append(f"parity.{field}={value!r}, expected 0")

    if isinstance(generation, dict):
        item["generation"] = {
            "workspace_id": generation.get("workspace_id"),
            "active": generation.get("active"),
            "integrity_matches": generation.get("integrity_matches"),
        }
        workspace_id = generation.get("workspace_id")
        if not isinstance(workspace_id, str) or not workspace_id:
            item["reasons"].append("generation.workspace_id must be a non-empty string")
        active = generation.get("active")
        if not nonnegative_integer(active):
            item["reasons"].append("generation.active must be a non-negative integer")
        if generation.get("integrity_matches") is not True:
            item["reasons"].append("generation.integrity_matches must be true")

    if isinstance(ingestion, dict):
        item["ingestion"] = {field: ingestion.get(field) for field in INGESTION_FIELDS}
        for field in INGESTION_FIELDS:
            if not finite_nonnegative(ingestion.get(field)):
                item["reasons"].append(
                    f"ingestion.{field} must be a non-negative finite number"
                )

    if isinstance(atomicity, dict):
        item["atomicity"] = {
            "fault_phases_passed": atomicity.get("fault_phases_passed"),
            "partial_visibility_failures": atomicity.get("partial_visibility_failures"),
        }
        fault_phases = atomicity.get("fault_phases_passed")
        if fault_phases != 8:
            item["reasons"].append(
                f"atomicity.fault_phases_passed={fault_phases!r}, expected 8"
            )
        partial_failures = atomicity.get("partial_visibility_failures")
        if partial_failures != 0:
            item["reasons"].append(
                f"atomicity.partial_visibility_failures={partial_failures!r}, expected 0"
            )

    # Preserve all compact operational measurements, but do not copy arbitrary
    # report payloads or source artifact paths into the gate result.
    item["passed"] = not item["reasons"]
    return item


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--require-section8-order", action="store_true")
    parser.add_argument("--require-section8-operations", action="store_true")
    parser.add_argument("--allow-section8-confirmation-only", action="store_true")
    parser.add_argument("reports", nargs="*", type=Path)
    args = parser.parse_args(argv)

    inputs: list[tuple[str, Path]] = []
    try:
        inputs.extend(parse_named_report(value) for value in args.report)
    except ValueError as error:
        parser.error(str(error))
    inputs.extend((report_name(path), path) for path in args.reports)

    decision: dict[str, Any] = {
        "schema_version": 1,
        "passed": False,
        "reasons": [],
        "reports": [],
    }
    if not inputs:
        decision["reasons"].append("at least one report is required")
    names = [name for name, _ in inputs]
    if len(set(names)) != len(names):
        decision["reasons"].append("report names must be unique")
    if args.require_section8_order and tuple(names) != SECTION8_REPOSITORY_ORDER:
        decision["reasons"].append(
            "Section 8 reports must be present in order: tops,motive,rvault,CBM"
        )

    require_operations = args.require_section8_operations or args.require_section8_order
    evaluated = [
        validate_report(
            name,
            path,
            require_section8_operations=require_operations,
            allow_confirmation_only=(args.require_section8_order and index == 3)
            or args.allow_section8_confirmation_only,
        )
        for index, (name, path) in enumerate(inputs)
    ]
    decision["reports"] = evaluated
    if any(not item["passed"] for item in evaluated):
        decision["reasons"].append("at least one report failed the single-file gate")
    decision["passed"] = not decision["reasons"]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(decision, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if decision["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
