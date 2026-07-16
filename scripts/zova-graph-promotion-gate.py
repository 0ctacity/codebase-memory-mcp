#!/usr/bin/env python3
"""Evaluate the experimental Zova graph-read promotion gate."""

import argparse
import json
import math
import sys
from pathlib import Path


THRESHOLD_RATIO = 1.05
REQUIRED_REPOSITORIES = 4


def parse_named_report(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise ValueError("--report must use NAME=PATH")
    name, path = value.split("=", 1)
    if not name or not path:
        raise ValueError("--report must use NAME=PATH")
    return name, Path(path)


def report_name(path: Path) -> str:
    if path.name == "graph-mcp-report.json" and path.parent.name == "latest":
        return path.parent.parent.name
    return path.stem


def finite_number(value: object) -> float | None:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def load_report(path: Path) -> tuple[dict | None, str | None]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, f"unreadable report: {error}"
    if not isinstance(data, dict):
        return None, "report root must be an object"
    return data, None


def evaluate_report(name: str, path: Path) -> dict:
    item: dict = {"repo": name, "path": str(path), "passed": False, "reasons": []}
    data, error = load_report(path)
    if error:
        item["reasons"].append(error)
        return item

    parity = data.get("trace_parity")
    warm = data.get("warm_ms")
    if not isinstance(parity, dict):
        item["reasons"].append("missing trace_parity object")
        return item
    if not isinstance(warm, dict):
        item["reasons"].append("missing warm_ms object")
        return item

    mismatches = parity.get("mismatches")
    fallbacks = parity.get("fallback_count")
    if mismatches != 0:
        item["reasons"].append(f"mismatches={mismatches!r}, expected 0")
    if fallbacks != 0:
        item["reasons"].append(f"fallback_count={fallbacks!r}, expected 0")

    values: dict[str, float] = {}
    for field in ("sqlite_p50", "zova_total_p50", "sqlite_p95", "zova_total_p95"):
        value = finite_number(warm.get(field))
        if value is None or value < 0.0:
            item["reasons"].append(f"warm_ms.{field} must be a non-negative finite number")
        else:
            values[field] = value
    item["warm_ms"] = values
    if len(values) == 4:
        if values["zova_total_p50"] > values["sqlite_p50"] * THRESHOLD_RATIO:
            item["reasons"].append(
                "zova_total_p50 %.6f exceeds sqlite_p50 %.6f × %.2f"
                % (values["zova_total_p50"], values["sqlite_p50"], THRESHOLD_RATIO)
            )
        if values["zova_total_p95"] > values["sqlite_p95"] * THRESHOLD_RATIO:
            item["reasons"].append(
                "zova_total_p95 %.6f exceeds sqlite_p95 %.6f × %.2f"
                % (values["zova_total_p95"], values["sqlite_p95"], THRESHOLD_RATIO)
            )

    item["passed"] = not item["reasons"]
    return item


def evaluate_topology(path: Path) -> tuple[dict, list[str]]:
    data, error = load_report(path)
    if error:
        return {"path": str(path)}, [error]
    reasons: list[str] = []
    if data.get("sidecar_topology_source") != "direct_graph_buffer":
        reasons.append("sidecar_topology_source must be direct_graph_buffer")
    for direct, mirror in (("direct_node_count", "mirror_node_count"),
                           ("direct_edge_count", "mirror_edge_count")):
        if data.get(direct) != data.get(mirror):
            reasons.append(f"{direct} must equal {mirror}")
    parity = data.get("parity")
    if not isinstance(parity, dict):
        reasons.append("missing parity object in topology report")
    else:
        if parity.get("topology_parity_passed") is not True:
            reasons.append("topology_parity_passed is not true")
        for field in ("direct_mirror_degree_mismatches",
                      "direct_mirror_neighbor_mismatches",
                      "direct_mirror_walk_mismatches"):
            if parity.get(field) != 0:
                reasons.append(f"{field}={parity.get(field)!r}, expected 0")
    ingestion = data.get("ingestion_benchmark")
    if not isinstance(ingestion, dict):
        reasons.append("missing ingestion_benchmark object in topology report")
    else:
        for field in ("direct_graph_write_ms", "sqlite_row_mirror_ms"):
            value = finite_number(ingestion.get(field))
            if value is None or value < 0.0:
                reasons.append(f"ingestion_benchmark.{field} must be a non-negative finite number")
        for direct, mirror in (("direct_node_count", "mirror_node_count"),
                               ("direct_edge_count", "mirror_edge_count")):
            if ingestion.get(direct) != ingestion.get(mirror):
                reasons.append(f"ingestion_benchmark.{direct} must equal {mirror}")
    return {"path": str(path)}, reasons


def evaluate_vector(path: Path) -> tuple[dict, list[str]]:
    data, error = load_report(path)
    if error:
        return {"path": str(path)}, [error]
    promotion = data.get("promotion_gate")
    if not isinstance(promotion, dict) or promotion.get("passed") is not True:
        return {"path": str(path)}, ["direct vector promotion_gate.passed is not true"]
    return {"path": str(path)}, []


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--topology-report", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--vector-report", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("reports", nargs="*", type=Path)
    args = parser.parse_args(argv)

    inputs: list[tuple[str, Path]] = []
    try:
        inputs.extend(parse_named_report(value) for value in args.report)
        topology_reports = dict(parse_named_report(value) for value in args.topology_report)
        vector_reports = dict(parse_named_report(value) for value in args.vector_report)
    except ValueError as error:
        parser.error(str(error))
    inputs.extend((report_name(path), path) for path in args.reports)

    evaluated = []
    for name, path in inputs:
        item = evaluate_report(name, path)
        topology_path = topology_reports.get(name)
        vector_path = vector_reports.get(name)
        if topology_path is None:
            item["reasons"].append("missing direct topology report")
        else:
            item["topology"], reasons = evaluate_topology(topology_path)
            item["reasons"].extend(reasons)
        if vector_path is None:
            item["reasons"].append("missing direct vector report")
        else:
            item["vector"], reasons = evaluate_vector(vector_path)
            item["reasons"].extend(reasons)
        item["passed"] = not item["reasons"]
        evaluated.append(item)

    decision = {
        "schema_version": 2,
        "threshold_ratio": THRESHOLD_RATIO,
        "passed": False,
        "reasons": [],
        "reports": evaluated,
    }
    names = [item["repo"] for item in decision["reports"]]
    if len(inputs) != REQUIRED_REPOSITORIES:
        decision["reasons"].append(
            f"expected {REQUIRED_REPOSITORIES} reports, received {len(inputs)}"
        )
    if len(set(names)) != len(names):
        decision["reasons"].append("repository names must be unique")
    if any(not item["passed"] for item in decision["reports"]):
        decision["reasons"].append("at least one repository failed the promotion rule")
    decision["passed"] = not decision["reasons"]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(decision, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if decision["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
