#!/usr/bin/env python3
"""Compare incremental direct-Zova lifecycle states with fresh full controls."""

import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path


PROBE_CALLER = "zova_lifecycle_probe/caller.go"
PROBE_HELPER = "zova_lifecycle_probe/helper.go"
PROBE_RENAMED_HELPER = "zova_lifecycle_probe/nested/helper.go"


def load_json(path: Path) -> tuple[dict | None, str | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, f"unreadable report: {error}"
    return (value, None) if isinstance(value, dict) else (None, "report root must be an object")


def encode_value(value: object) -> bytes:
    if value is None:
        return b"n"
    if isinstance(value, bytes):
        return b"b" + len(value).to_bytes(8, "big") + value
    text = str(value).encode("utf-8")
    return b"t" + len(text).to_bytes(8, "big") + text


def digest_rows(path: Path, sql: str, params: tuple[object, ...]) -> tuple[str | None, str | None]:
    try:
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        # `properties` is an opaque CBM payload. It is usually JSON but must
        # be compared byte-for-byte even when an extractor produced invalid
        # UTF-8, so do not let Python decode it before encode_value sees it.
        db.text_factory = bytes
        cursor = db.execute(sql, params)
        digest = hashlib.sha256()
        row_count = 0
        for row in cursor:
            digest.update(b"[")
            for value in row:
                digest.update(encode_value(value))
            digest.update(b"]")
            row_count += 1
        db.close()
    except sqlite3.Error as error:
        return None, str(error)
    return f"{row_count}:{digest.hexdigest()}", None


def compact_artifact_summary(digests: dict[str, str]) -> dict[str, object]:
    """Keep row counts beside digests without retaining the source database."""
    counts: dict[str, int] = {}
    for name, digest in digests.items():
        count_text, separator, _ = digest.partition(":")
        if not separator or not count_text.isdigit():
            raise ValueError(f"invalid digest for {name}: {digest!r}")
        counts[name] = int(count_text)
    return {"digests": dict(digests), "counts": counts}


def compact_row_diff(incremental_rows: list[tuple[object, ...]],
                     control_rows: list[tuple[object, ...]], limit: int = 8) -> dict[str, list[list[object]]]:
    """Return bounded, JSON-friendly rows unique to either artifact."""
    def sort_key(row: tuple[object, ...]) -> tuple[str, ...]:
        return tuple(repr(value) for value in row)

    def json_row(row: tuple[object, ...]) -> list[object]:
        result: list[object] = []
        for value in row:
            if isinstance(value, bytes):
                result.append(value.decode("utf-8", errors="replace"))
            else:
                result.append(value)
        return result

    incremental_set = {tuple(row) for row in incremental_rows}
    control_set = {tuple(row) for row in control_rows}
    return {
        "only_incremental": [json_row(row) for row in sorted(incremental_set - control_set, key=sort_key)[:limit]],
        "only_control": [json_row(row) for row in sorted(control_set - incremental_set, key=sort_key)[:limit]],
    }


def _canonical_property_value(value: object) -> object:
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    if isinstance(value, str):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


def canonical_property_text(value: object) -> str:
    """Serialize property JSON canonically so object key order is irrelevant."""
    original = value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value
    if isinstance(original, str):
        try:
            parsed = json.loads(original)
        except json.JSONDecodeError:
            return original
    else:
        parsed = original
    return json.dumps(parsed, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      default=str)


def digest_node_property_rows(path: Path, params: tuple[object, ...]) -> tuple[str | None, str | None]:
    try:
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        db.text_factory = bytes
        cursor = db.execute(
            "SELECT qualified_name,label,name,file_path,start_line,end_line,properties FROM nodes "
            "WHERE project=? ORDER BY qualified_name,label,name,file_path,start_line,end_line",
            params,
        )
        digest = hashlib.sha256()
        row_count = 0
        for row in cursor:
            digest.update(b"[")
            for value in (*row[:-1], canonical_property_text(row[-1])):
                digest.update(encode_value(value))
            digest.update(b"]")
            row_count += 1
        db.close()
    except sqlite3.Error as error:
        return None, str(error)
    return f"{row_count}:{digest.hexdigest()}", None


def _flatten_property_value(value: object, prefix: str = "") -> dict[str, object]:
    value = _canonical_property_value(value)
    if isinstance(value, dict):
        flattened: dict[str, object] = {}
        for key in sorted(value):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            flattened.update(_flatten_property_value(value[key], child_prefix))
        return flattened
    return {prefix or "<value>": value}


def _property_values_equal(left: object, right: object) -> bool:
    return json.dumps(left, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      default=str) == json.dumps(right, sort_keys=True, separators=(",", ":"),
                                                   ensure_ascii=False, default=str)


def _json_safe_property_value(value: object) -> object:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, dict):
        return {str(key): _json_safe_property_value(child) for key, child in value.items()}
    if isinstance(value, list):
        return [_json_safe_property_value(child) for child in value]
    return value


def compact_property_diff(incremental: dict[str, object], control: dict[str, object],
                          limit: int = 64) -> dict[str, list[object]]:
    """Compare parsed properties canonically, ignoring JSON object key order."""
    only_incremental_nodes = sorted(set(incremental) - set(control))
    only_control_nodes = sorted(set(control) - set(incremental))
    changed: list[dict[str, object]] = []
    for node in sorted(set(incremental) & set(control)):
        left = _flatten_property_value(incremental[node])
        right = _flatten_property_value(control[node])
        for property_key in sorted(set(left) | set(right)):
            if property_key not in left or property_key not in right:
                continue
            if _property_values_equal(left[property_key], right[property_key]):
                continue
            changed.append({
                "node": node,
                "property_key": property_key,
                "incremental_value": _json_safe_property_value(left[property_key]),
                "fresh_control_value": _json_safe_property_value(right[property_key]),
            })
            if len(changed) >= limit:
                break
        if len(changed) >= limit:
            break
    return {
        "changed": changed,
        "only_incremental_nodes": only_incremental_nodes[:limit],
        "only_control_nodes": only_control_nodes[:limit],
    }


def artifact_identity_rows(report: dict, kind: str) -> tuple[list[tuple[object, ...]] | None, str | None]:
    """Load only compact node/edge identity rows for mismatch diagnostics."""
    db_path = Path(str(report.get("db_path", "")))
    project = report.get("project")
    if not db_path.is_file() or not isinstance(project, str) or not project:
        return None, "report is missing an accessible db_path or project"
    queries = {
        "nodes": (
            "SELECT label,name,qualified_name,file_path,start_line,end_line FROM nodes "
            "WHERE project=? ORDER BY label,name,qualified_name,file_path,start_line,end_line",
        ),
        "edges": (
            "SELECT e.type,s.qualified_name,t.qualified_name FROM edges e "
            "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
            "WHERE e.project=? ORDER BY e.type,s.qualified_name,t.qualified_name",
        ),
    }
    if kind not in queries:
        return None, f"unsupported identity kind: {kind}"
    try:
        db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        db.text_factory = bytes
        rows = [tuple(row) for row in db.execute(queries[kind][0], (project,))]
        db.close()
        return rows, None
    except sqlite3.Error as error:
        return None, str(error)


def artifact_node_properties(report: dict) -> tuple[dict[str, object] | None, str | None]:
    """Load parsed node properties keyed by qualified name for compact diffs."""
    db_path = Path(str(report.get("db_path", "")))
    project = report.get("project")
    if not db_path.is_file() or not isinstance(project, str) or not project:
        return None, "report is missing an accessible db_path or project"
    try:
        db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        db.text_factory = bytes
        result: dict[str, object] = {}
        for qualified_name, properties in db.execute(
            "SELECT qualified_name,properties FROM nodes WHERE project=? ORDER BY qualified_name",
            (project,),
        ):
            if isinstance(qualified_name, bytes):
                qualified_name = qualified_name.decode("utf-8", errors="replace")
            result[str(qualified_name)] = _canonical_property_value(properties)
        db.close()
        return result, None
    except sqlite3.Error as error:
        return None, str(error)


def scalar(path: Path, sql: str, params: tuple[object, ...] = ()) -> tuple[int | None, str | None]:
    try:
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        row = db.execute(sql, params).fetchone()
        db.close()
    except sqlite3.Error as error:
        return None, str(error)
    return (int(row[0]), None) if row is not None else (None, "query returned no row")


def artifact_digests(report: dict) -> tuple[dict[str, str] | None, list[str]]:
    reasons: list[str] = []
    db_path = Path(str(report.get("db_path", "")))
    zova_path = Path(str(report.get("zova_path", "")))
    project = report.get("project")
    if not db_path.is_file() or not zova_path.is_file() or not isinstance(project, str) or not project:
        return None, ["report is missing an accessible db_path, zova_path, or project"]
    queries = {
        "node_properties": (db_path,
                             "SELECT qualified_name,label,name,file_path,start_line,end_line,properties "
                             "FROM nodes WHERE project=? ORDER BY qualified_name,label,name,file_path,"
                             "start_line,end_line,properties", (project,)),
        "edges": (db_path,
                  "SELECT e.type,e.properties,s.label,s.name,s.qualified_name,s.file_path,"
                  "s.start_line,s.end_line,t.label,t.name,t.qualified_name,t.file_path,"
                  "t.start_line,t.end_line FROM edges e JOIN nodes s ON s.id=e.source_id "
                  "JOIN nodes t ON t.id=e.target_id WHERE e.project=? ORDER BY e.type,e.properties,"
                  "s.label,s.name,s.qualified_name,s.file_path,s.start_line,s.end_line,"
                  "t.label,t.name,t.qualified_name,t.file_path,t.start_line,t.end_line", (project,)),
        "raw_node_vector_payloads": (db_path,
                                      "SELECT n.qualified_name,n.label,n.name,n.file_path,"
                                      "n.start_line,n.end_line,v.vector FROM node_vectors v "
                                      "JOIN nodes n ON n.id=v.node_id WHERE v.project=? "
                                      "ORDER BY n.qualified_name,n.label,n.name,n.file_path,"
                                      "n.start_line,n.end_line,v.vector", (project,)),
        "token_vectors": (db_path,
                          "SELECT token,idf,vector FROM token_vectors WHERE project=? "
                          "ORDER BY token,idf,vector", (project,)),
        "trace_projection": (zova_path,
                             "SELECT node_id,project,name,qualified_name,file_path,label,start_line,end_line "
                             "FROM cbm_zova_trace_nodes_v1 WHERE project=? "
                             "ORDER BY node_id,project,name,qualified_name,file_path,label,start_line,end_line",
                             (project,)),
    }
    result: dict[str, str] = {}
    for name, (path, sql, params) in queries.items():
        if name == "node_properties":
            value, error = digest_node_property_rows(path, params)
        else:
            value, error = digest_rows(path, sql, params)
        if error:
            reasons.append(f"{name} digest failed: {error}")
        else:
            result[name] = value or ""
    return (result if not reasons else None), reasons


def validate_generation(report: dict) -> list[str]:
    reasons: list[str] = []
    db_path = Path(str(report.get("db_path", "")))
    zova_path = Path(str(report.get("zova_path", "")))
    repo_path = str(report.get("repo_path", "")).rstrip("/")
    db_generation, error = scalar(db_path, "SELECT generation FROM cbm_zova_sidecar_generation_v1 WHERE id=1")
    if error:
        return [f"source generation unavailable: {error}"]
    zova_generation, error = scalar(zova_path, "SELECT generation FROM cbm_zova_sidecar_generation_v1 WHERE id=1")
    if error or db_generation != zova_generation:
        reasons.append(f"db/sidecar generation mismatch: {db_generation!r}/{zova_generation!r} ({error or ''})")
    workspace_id = "w1_" + hashlib.sha256(repo_path.encode("utf-8")).hexdigest()[:32]
    registry_path = db_path.parent / "cbm.zova"
    active, error = scalar(registry_path,
                           "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id=?",
                           (workspace_id,))
    if error or active != db_generation:
        reasons.append(f"ready generation mismatch: {active!r}/{db_generation!r} ({error or ''})")
    return reasons


def validate_vector_lifecycle(report: dict) -> list[str]:
    """Require vector correctness here; keep the separate performance gate diagnostic."""
    reasons: list[str] = []
    promotion = report.get("promotion_gate")
    promotion_reasons = promotion.get("reasons") if isinstance(promotion, dict) else None
    if not isinstance(promotion_reasons, dict) or promotion_reasons.get("exact_parity") is not True:
        reasons.append("vector exact parity is not true")
    aggregate = report.get("aggregate")
    if not isinstance(aggregate, dict) or aggregate.get("fallback_count") != 0:
        reasons.append("vector aggregate fallback_count is not zero")
    return reasons


def validate_existing_reports(report_path: Path, report: dict) -> list[str]:
    reasons: list[str] = []
    reasons.extend(validate_vector_lifecycle(report))
    for name, path in (("graph", report_path.with_name("graph-report.json")),
                       ("graph_mcp", report_path.with_name("graph-mcp-report.json"))):
        data, error = load_json(path)
        if error:
            reasons.append(f"{name} report {error}")
            continue
        if name == "graph":
            parity = data.get("parity")
            if data.get("sidecar_topology_source") != "direct_graph_buffer":
                reasons.append("graph report is not direct_graph_buffer")
            if not isinstance(parity, dict) or parity.get("topology_parity_passed") is not True:
                reasons.append("graph topology parity is not true")
        else:
            parity = data.get("trace_parity")
            if not isinstance(parity, dict) or parity.get("mismatches") != 0 or parity.get("fallback_count") != 0:
                reasons.append("graph MCP parity/fallback gate failed")
    return reasons


def validate_probe(report: dict, state: str) -> list[str]:
    expected_path = {"add": PROBE_HELPER, "edit": PROBE_HELPER,
                     "rename": PROBE_RENAMED_HELPER, "delete": None}.get(state)
    if state not in {"add", "edit", "rename", "delete"}:
        return [f"unsupported lifecycle state: {state}"]
    db_path = Path(str(report.get("db_path", "")))
    zova_path = Path(str(report.get("zova_path", "")))
    project = report.get("project")
    if not isinstance(project, str):
        return ["missing project"]
    reasons: list[str] = []
    for path, table, sql in (
        (zova_path, "trace target", "SELECT count(*) FROM cbm_zova_trace_nodes_v1 WHERE project=? AND file_path=?"),
        (db_path, "source vector target", "SELECT count(*) FROM node_vectors v JOIN nodes n ON n.id=v.node_id WHERE v.project=? AND n.file_path=?"),
    ):
        count, error = scalar(path, sql, (project, expected_path or PROBE_HELPER))
        if error:
            reasons.append(f"{table} check failed: {error}")
        elif expected_path is None and count != 0:
            reasons.append(f"{table} remains after delete: {count}")
        elif expected_path is not None and count <= 0:
            reasons.append(f"{table} missing for {expected_path}")
    obsolete_paths = {
        "rename": (PROBE_HELPER,),
        "delete": (PROBE_HELPER, PROBE_RENAMED_HELPER),
    }.get(state, ())
    for obsolete_path in obsolete_paths:
        old_count, error = scalar(
            zova_path,
            "SELECT count(*) FROM cbm_zova_trace_nodes_v1 WHERE project=? AND file_path=?",
            (project, obsolete_path),
        )
        if error:
            reasons.append(f"obsolete helper projection check failed: {error}")
        elif old_count != 0:
            reasons.append(f"obsolete helper projection remains: {obsolete_path} ({old_count})")
    calls, error = scalar(
        db_path,
        "SELECT count(*) FROM edges e JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
        "WHERE e.project=? AND e.type='CALLS' AND s.file_path=? AND t.file_path=?",
        (project, PROBE_CALLER, expected_path or PROBE_HELPER),
    )
    if error:
        reasons.append(f"probe CALLS check failed: {error}")
    elif (expected_path is None and calls != 0) or (expected_path is not None and calls <= 0):
        reasons.append(f"probe CALLS count invalid: {calls}")
    return reasons


def evaluate_state(spec: str) -> dict:
    item: dict = {"name": spec, "passed": False, "reasons": []}
    parts = spec.split("=", 2)
    if len(parts) != 3 or not all(parts):
        item["reasons"].append("state must be NAME=INCREMENTAL_REPORT=CONTROL_REPORT")
        return item
    state, incremental_path, control_path = parts
    item["name"] = state
    incremental, error = load_json(Path(incremental_path))
    if error:
        item["reasons"].append(f"incremental {error}")
        return item
    control, error = load_json(Path(control_path))
    if error:
        item["reasons"].append(f"control {error}")
        return item
    incremental_digests, reasons = artifact_digests(incremental)
    control_digests, control_reasons = artifact_digests(control)
    item["reasons"].extend(reasons)
    item["reasons"].extend(f"control {reason}" for reason in control_reasons)
    if incremental_digests and control_digests:
        incremental_summary = compact_artifact_summary(incremental_digests)
        control_summary = compact_artifact_summary(control_digests)
        item["digests"] = {
            "incremental": incremental_summary["digests"],
            "control": control_summary["digests"],
        }
        item["counts"] = {
            "incremental": incremental_summary["counts"],
            "control": control_summary["counts"],
        }
        for key in incremental_digests:
            if incremental_digests[key] != control_digests.get(key):
                item["reasons"].append(f"{key} digest differs from fresh control")
                if key == "node_properties":
                    incremental_properties, incremental_error = artifact_node_properties(incremental)
                    control_properties, control_error = artifact_node_properties(control)
                    if incremental_error:
                        item["reasons"].append(
                            f"node property diff failed for incremental: {incremental_error}"
                        )
                    elif control_error:
                        item["reasons"].append(
                            f"node property diff failed for control: {control_error}"
                        )
                    elif incremental_properties is not None and control_properties is not None:
                        item["property_differences"] = compact_property_diff(
                            incremental_properties, control_properties
                        )
                if key == "edges":
                    incremental_rows, incremental_error = artifact_identity_rows(incremental, key)
                    control_rows, control_error = artifact_identity_rows(control, key)
                    if incremental_error:
                        item["reasons"].append(f"{key} identity sample failed for incremental: {incremental_error}")
                    elif control_error:
                        item["reasons"].append(f"{key} identity sample failed for control: {control_error}")
                    elif incremental_rows is not None and control_rows is not None:
                        item.setdefault("row_differences", {})[key] = compact_row_diff(
                            incremental_rows, control_rows
                        )
    item["reasons"].extend(validate_generation(incremental))
    item["reasons"].extend(f"control {reason}" for reason in validate_generation(control))
    item["reasons"].extend(validate_existing_reports(Path(incremental_path), incremental))
    item["reasons"].extend(f"control {reason}" for reason in validate_existing_reports(Path(control_path), control))
    item["reasons"].extend(validate_probe(incremental, state))
    item["reasons"].extend(f"control {reason}" for reason in validate_probe(control, state))
    item["passed"] = not item["reasons"]
    return item


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--state", action="append", default=[])
    args = parser.parse_args(argv)
    states = [evaluate_state(spec) for spec in args.state]
    report = {"schema_version": 1, "passed": bool(states) and all(item["passed"] for item in states),
              "states": states}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
