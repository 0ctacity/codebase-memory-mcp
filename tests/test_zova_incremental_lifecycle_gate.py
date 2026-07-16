#!/usr/bin/env python3
"""Regression tests for the real-repository incremental lifecycle gate."""

import json
import importlib.util
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts" / "zova-incremental-lifecycle-gate.py"


def load_gate_module():
    spec = importlib.util.spec_from_file_location("zova_incremental_lifecycle_gate", GATE)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_gate_rejects_malformed_state_and_writes_report() -> None:
    with tempfile.TemporaryDirectory(prefix="cbm-zova-lifecycle-gate-") as td:
        output = Path(td) / "report.json"
        result = subprocess.run(
            [sys.executable, str(GATE), "--output", str(output), "--state", "broken"],
            check=False,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 1
        assert output.exists(), result.stderr
        report = json.loads(output.read_text(encoding="utf-8"))
        assert report["passed"] is False
        assert report["states"][0]["name"] == "broken"


def test_digest_accepts_non_utf8_properties_blob() -> None:
    gate = load_gate_module()
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "fixture.db"
        db = sqlite3.connect(path)
        db.execute("CREATE TABLE sample(value TEXT)")
        db.execute("INSERT INTO sample(value) VALUES(CAST(x'FF' AS TEXT))")
        db.commit()
        db.close()

        digest, error = gate.digest_rows(path, "SELECT value FROM sample", ())
        assert error is None
        assert digest and digest.startswith("1:")


def test_compact_artifact_summary_retains_digest_and_count() -> None:
    gate = load_gate_module()
    summary = gate.compact_artifact_summary({
        "nodes": "12:abc",
        "edges": "0:def",
    })
    assert summary == {
        "digests": {"nodes": "12:abc", "edges": "0:def"},
        "counts": {"nodes": 12, "edges": 0},
    }


def test_compact_row_diff_is_bounded_and_directional() -> None:
    gate = load_gate_module()
    diff = gate.compact_row_diff(
        [("keep",), ("incremental-only",)],
        [("keep",), ("control-only",)],
        limit=1,
    )
    assert diff == {
        "only_incremental": [["incremental-only"]],
        "only_control": [["control-only"]],
    }


def test_vector_lifecycle_check_does_not_require_performance_promotion() -> None:
    gate = load_gate_module()
    report = {
        "promotion_gate": {
            "passed": False,
            "reasons": {
                "exact_parity": True,
                "warm_p50_not_slower": False,
                "warm_p95_not_slower": False,
            },
        },
        "aggregate": {"fallback_count": 0},
    }
    assert gate.validate_vector_lifecycle(report) == []


def test_property_diff_ignores_json_key_order_and_reports_changed_keys() -> None:
    gate = load_gate_module()
    diff = gate.compact_property_diff(
        {"pkg.Node": {"nested": {"a": 1, "b": 2}, "same": True}},
        {"pkg.Node": {"nested": {"b": 3, "a": 1}, "same": True}},
    )
    assert diff == {
        "changed": [{
            "node": "pkg.Node",
            "property_key": "nested.b",
            "incremental_value": 2,
            "fresh_control_value": 3,
        }],
        "only_incremental_nodes": [],
        "only_control_nodes": [],
    }


def test_canonical_property_text_ignores_json_key_order() -> None:
    gate = load_gate_module()
    assert gate.canonical_property_text('{"b":2,"a":1}') == '{"a":1,"b":2}'


if __name__ == "__main__":
    test_gate_rejects_malformed_state_and_writes_report()
    test_digest_accepts_non_utf8_properties_blob()
    test_compact_artifact_summary_retains_digest_and_count()
    test_compact_row_diff_is_bounded_and_directional()
    test_vector_lifecycle_check_does_not_require_performance_promotion()
    test_property_diff_ignores_json_key_order_and_reports_changed_keys()
    test_canonical_property_text_ignores_json_key_order()
    print("PASS: zova incremental lifecycle gate")
