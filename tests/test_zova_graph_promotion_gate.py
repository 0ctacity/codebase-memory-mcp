#!/usr/bin/env python3
"""Regression tests for the graph-read promotion gate."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts" / "zova-graph-promotion-gate.py"
REPOS = ("codebase-memory-mcp", "motive", "rvault", "tops")


def report(zova_p95: float = 21.0) -> dict:
    return {
        "trace_parity": {"mismatches": 0, "fallback_count": 0},
        "warm_ms": {
            "sqlite_p50": 10.0,
            "zova_total_p50": 10.5,
            "sqlite_p95": 20.0,
            "zova_total_p95": zova_p95,
        },
    }


def topology_report() -> dict:
    return {
        "sidecar_topology_source": "direct_graph_buffer",
        "direct_node_count": 10,
        "mirror_node_count": 10,
        "direct_edge_count": 15,
        "mirror_edge_count": 15,
        "parity": {
            "topology_parity_passed": True,
            "direct_mirror_degree_mismatches": 0,
            "direct_mirror_neighbor_mismatches": 0,
            "direct_mirror_walk_mismatches": 0,
        },
        "ingestion_benchmark": {
            "direct_graph_write_ms": 1.0,
            "sqlite_row_mirror_ms": 2.0,
            "direct_node_count": 10,
            "direct_edge_count": 15,
            "mirror_node_count": 10,
            "mirror_edge_count": 15,
        },
    }


def vector_report() -> dict:
    return {"promotion_gate": {"passed": True}}


def invoke(reports: dict[str, dict], topology_source: str = "direct_graph_buffer",
           include_ingestion: bool = True) -> tuple[
    subprocess.CompletedProcess[str], dict
]:
    with tempfile.TemporaryDirectory(prefix="cbm-zova-gate-") as td:
        root = Path(td)
        inputs = []
        topology_inputs = []
        vector_inputs = []
        for name, data in reports.items():
            path = root / f"{name}.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            inputs.append(str(path))
            topology_path = root / f"{name}-topology.json"
            topology = topology_report()
            topology["sidecar_topology_source"] = topology_source
            if not include_ingestion:
                topology.pop("ingestion_benchmark")
            topology_path.write_text(json.dumps(topology), encoding="utf-8")
            topology_inputs.extend(["--topology-report", f"{name}={topology_path}"])
            vector_path = root / f"{name}-vector.json"
            vector_path.write_text(json.dumps(vector_report()), encoding="utf-8")
            vector_inputs.extend(["--vector-report", f"{name}={vector_path}"])
        output = root / "promotion-gate.json"
        result = subprocess.run(
            [sys.executable, str(GATE), "--output", str(output), *topology_inputs, *vector_inputs,
             *inputs],
            check=False,
            capture_output=True,
            text=True,
        )
        assert output.exists(), result.stderr
        return result, json.loads(output.read_text(encoding="utf-8"))


def test_gate_accepts_exact_five_percent_limit() -> None:
    result, decision = invoke({name: report() for name in REPOS})
    assert result.returncode == 0, result.stderr
    assert decision["passed"] is True
    assert all(item["passed"] is True for item in decision["reports"])


def test_gate_rejects_p95_above_limit_with_reason() -> None:
    reports = {name: report() for name in REPOS}
    reports["tops"] = report(zova_p95=21.01)
    result, decision = invoke(reports)
    assert result.returncode == 1
    assert decision["passed"] is False
    tops = next(item for item in decision["reports"] if item["repo"] == "tops")
    assert tops["passed"] is False
    assert any("zova_total_p95" in reason for reason in tops["reasons"])


def test_gate_rejects_non_direct_topology_control() -> None:
    reports = {name: report() for name in REPOS}
    # The gate must not accept a mirror-backed MCP report as direct-write evidence.
    result, decision = invoke(reports, topology_source="sqlite_row_mirror")
    assert result.returncode == 1
    assert decision["passed"] is False
    assert all(any("sidecar_topology_source" in reason for reason in item["reasons"])
               for item in decision["reports"])


def test_gate_rejects_missing_ingestion_benchmark() -> None:
    result, decision = invoke({name: report() for name in REPOS}, include_ingestion=False)
    assert result.returncode == 1
    assert decision["passed"] is False
    assert all(any("ingestion_benchmark" in reason for reason in item["reasons"])
               for item in decision["reports"])


if __name__ == "__main__":
    test_gate_accepts_exact_five_percent_limit()
    test_gate_rejects_p95_above_limit_with_reason()
    test_gate_rejects_non_direct_topology_control()
    test_gate_rejects_missing_ingestion_benchmark()
    print("PASS: zova graph promotion gate")
