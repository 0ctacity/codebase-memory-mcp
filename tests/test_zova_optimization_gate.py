#!/usr/bin/env python3
"""Contract tests for the Zova storage/ingestion optimization gate."""

from __future__ import annotations

import copy
import importlib.util
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "zova-optimization-gate.py"
BASELINE_SCRIPT = ROOT / "scripts" / "zova-optimization-baseline.py"


def load_gate():
    spec = importlib.util.spec_from_file_location("zova_optimization_gate", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def state(route: str, workload: str) -> dict:
    incremental = workload == "incremental"
    return {
        "route": route,
        "workload": workload,
        "pipeline_mode": "CBM_MODE_INCREMENTAL" if incremental else "CBM_MODE_FULL",
        "incremental_fallback_reason": None,
        "full_fallback_count": 0,
        "full_clear_count": 0,
        "unchanged_rewrite_count": 0,
        "rows": {
            "nodes_total": 1000,
            "nodes_inserted": 2 if incremental else 1000,
            "nodes_updated": 1 if incremental else 0,
            "nodes_deleted": 0,
            "edges_total": 4000,
            "edges_inserted": 3 if incremental else 4000,
            "edges_deleted": 1 if incremental else 0,
            "node_vectors_total": 800,
            "node_vectors_upserted": 2 if incremental else 800,
            "node_vectors_deleted": 0,
            "token_vectors_total": 200,
            "token_vectors_upserted": 1 if incremental else 200,
            "token_vectors_deleted": 0,
        },
        "timing_ms": {
            "normalize": 10.0,
            "canonical_nodes": 20.0,
            "canonical_edges": 30.0,
            "fts": 10.0,
            "native_graph": 40.0,
            "native_vectors": 20.0,
            "digests": 10.0,
            "verify": 10.0,
            "publish": 200.0 if not incremental else 50.0,
            "pipeline": 500.0 if not incremental else 120.0,
        },
        "snapshot": {
            "completed": route == "single" and incremental,
            "generation": 1 if route == "single" and incremental else 0,
            "base_ms": 15.0 if route == "single" and incremental else 0.0,
            "optional_ms": 0.0,
            "hydrated_components": 0,
            "topology_rows": 0,
            "node_vector_rows": 0,
            "token_vector_rows": 0,
        },
        "storage": {
            "database_bytes": 800_000,
            "wal_bytes": 0,
            "freelist_bytes": 0,
            "canonical_bytes": 200_000,
            "fts_bytes": 50_000,
            "native_graph_bytes": 300_000,
            "native_vector_bytes": 200_000,
            "other_bytes": 50_000,
            "graph_bytes_per_edge": 75.0,
            "vector_bytes_per_row": 200.0,
        },
        "performance": {
            "vector_p95_ms": 10.4,
        },
        "forbidden_table_count": 0,
        "parity_mismatch_count": 0,
        "unexpected_fallback_count": 0,
        "passed": True,
    }


def report(*, baseline: bool = False) -> dict:
    states = [
        state("pure", "full"),
        state("single", "full"),
        state("pure", "incremental"),
        state("single", "incremental"),
    ]
    if baseline:
        states[1]["storage"].update(
            database_bytes=1_000_000,
            native_graph_bytes=400_000,
            native_vector_bytes=250_000,
            graph_bytes_per_edge=100.0,
            vector_bytes_per_row=250.0,
        )
        states[1]["timing_ms"]["publish"] = 250.0
        states[1]["performance"]["vector_p95_ms"] = 10.0
        states[3]["timing_ms"]["publish"] = 250.0
        # The old publisher is allowed to expose the structural problem in the
        # immutable baseline; the final report is not.
        states[3]["full_clear_count"] = 1
        states[3]["unchanged_rewrite_count"] = 5_000
    return {
        "schema_version": 1,
        "repository": "tops",
        "source_commit": "a" * 40 if baseline else "b" * 40,
        "build_sha256": "c" * 64 if baseline else "d" * 64,
        "run_id": "tops-baseline" if baseline else "tops-final",
        "mutation": "source-change",
        "passed": True,
        "states": states,
    }


def lazy_report(*, baseline: bool = False, mutation: str = "digest-stable") -> dict:
    value = report(baseline=baseline)
    value["benchmark_kind"] = "lazy-hydration"
    value["mutation"] = mutation
    incremental = value["states"][3]
    incremental["full_clear_count"] = 0
    incremental["unchanged_rewrite_count"] = 0
    incremental["timing_ms"]["pipeline"] = 200.0 if baseline else 160.0
    value["states"][2]["timing_ms"]["pipeline"] = 150.0
    return value


class OptimizationGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(SCRIPT.is_file(), f"missing implementation: {SCRIPT}")
        self.gate = load_gate()

    def test_valid_final_report_passes(self) -> None:
        decision = self.gate.validate(report(baseline=True), report())
        self.assertTrue(decision["passed"], decision)
        self.assertEqual(decision["failures"], [])

    def test_lazy_hydration_thresholds_and_digest_stable_contract(self) -> None:
        decision = self.gate.validate(
            lazy_report(baseline=True), lazy_report()
        )
        self.assertTrue(decision["passed"], decision)

        final = lazy_report()
        final["states"][3]["snapshot"]["hydrated_components"] = 1
        decision = self.gate.validate(lazy_report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("hydrate zero" in failure for failure in decision["failures"]))

        final = lazy_report()
        final["states"][3]["timing_ms"]["pipeline"] = 171.0
        decision = self.gate.validate(lazy_report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("at least 15%" in failure for failure in decision["failures"]))

        source_baseline = lazy_report(baseline=True, mutation="source-change")
        source_final = lazy_report(mutation="source-change")
        source_final["states"][3]["timing_ms"]["pipeline"] = 211.0
        decision = self.gate.validate(source_baseline, source_final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("regression exceeds 5%" in failure for failure in decision["failures"]))

    def test_exact_state_order_and_pipeline_modes_are_required(self) -> None:
        final = report()
        final["states"][2]["pipeline_mode"] = "CBM_MODE_FULL"
        with self.assertRaisesRegex(ValueError, "pipeline_mode"):
            self.gate.validate(report(baseline=True), final)

        final = report()
        final["states"][2]["workload"] = "changed_state_full_pipeline"
        with self.assertRaisesRegex(ValueError, "workload"):
            self.gate.validate(report(baseline=True), final)

    def test_incremental_snapshot_contract_is_strict(self) -> None:
        final = report()
        del final["states"][3]["snapshot"]
        with self.assertRaisesRegex((TypeError, ValueError), "snapshot"):
            self.gate.validate(report(baseline=True), final)

        final = report()
        final["states"][3]["snapshot"]["completed"] = False
        with self.assertRaisesRegex(ValueError, "snapshot.completed"):
            self.gate.validate(report(baseline=True), final)

        final = report()
        final["states"][3]["snapshot"]["topology_rows"] = 1
        with self.assertRaisesRegex(ValueError, "topology_rows"):
            self.gate.validate(report(baseline=True), final)

    def test_final_incremental_must_not_clear_fallback_or_rewrite_unchanged(self) -> None:
        for field in (
            "full_clear_count",
            "full_fallback_count",
            "unchanged_rewrite_count",
            "unexpected_fallback_count",
        ):
            final = report()
            final["states"][3][field] = 1
            decision = self.gate.validate(report(baseline=True), final)
            self.assertFalse(decision["passed"])
            self.assertTrue(any(field in item for item in decision["failures"]))

        final = report()
        final["states"][3]["incremental_fallback_reason"] = "full rebuild"
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("fallback_reason" in item for item in decision["failures"]))

    def test_delta_structural_gate_applies_to_single_route_only(self) -> None:
        final = report()
        pure_incremental = final["states"][2]
        pure_incremental["full_clear_count"] = 1
        pure_incremental["unchanged_rewrite_count"] = 1000
        pure_incremental["rows"]["nodes_inserted"] = 1000
        decision = self.gate.validate(report(baseline=True), final)
        self.assertTrue(decision["passed"], decision)

    def test_final_rejects_forbidden_tables_and_parity_mismatches(self) -> None:
        for field in (
            "forbidden_table_count",
            "parity_mismatch_count",
            "unexpected_fallback_count",
        ):
            final = report()
            final["states"][1][field] = 1
            decision = self.gate.validate(report(baseline=True), final)
            self.assertFalse(decision["passed"])
            self.assertTrue(any(field in item for item in decision["failures"]))

    def test_storage_and_publisher_thresholds_are_enforced(self) -> None:
        final = report()
        final["states"][1]["storage"]["database_bytes"] = 800_001
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("storage reduction" in item for item in decision["failures"]))

        final = report()
        final["states"][1]["timing_ms"]["publish"] = 212.501
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("publisher reduction" in item for item in decision["failures"]))

    def test_native_categories_and_vector_p95_must_improve(self) -> None:
        final = report()
        final["states"][1]["storage"]["graph_bytes_per_edge"] = 100.0
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("graph bytes/edge" in item for item in decision["failures"]))

        final = report()
        final["states"][1]["performance"]["vector_p95_ms"] = 10.501
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("vector p95" in item for item in decision["failures"]))

    def test_delta_size_and_time_thresholds_are_enforced(self) -> None:
        final = report()
        incremental = final["states"][3]
        incremental["rows"]["nodes_inserted"] = 100
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("changed-row ratio" in item for item in decision["failures"]))

        final = report()
        final["states"][3]["timing_ms"]["publish"] = 75.001
        decision = self.gate.validate(report(baseline=True), final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("delta publisher" in item for item in decision["failures"]))

    def test_numeric_contract_rejects_bool_nan_inf_negative_and_strings(self) -> None:
        mutations = (
            ("rows", "nodes_total", True),
            ("rows", "nodes_total", "1000"),
            ("timing_ms", "publish", math.nan),
            ("timing_ms", "publish", math.inf),
            ("timing_ms", "publish", -1.0),
            ("storage", "database_bytes", -1),
        )
        for section, field, value in mutations:
            final = report()
            final["states"][1][section][field] = value
            with self.subTest(section=section, field=field, value=value):
                with self.assertRaises((TypeError, ValueError)):
                    self.gate.validate(report(baseline=True), final)

    def test_provenance_and_passed_are_strict(self) -> None:
        final = report()
        final["build_sha256"] = "bad"
        with self.assertRaisesRegex(ValueError, "build_sha256"):
            self.gate.validate(report(baseline=True), final)
        final = report()
        final["passed"] = 1
        with self.assertRaisesRegex(TypeError, "passed"):
            self.gate.validate(report(baseline=True), final)

    def test_cli_always_writes_a_decision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            baseline_path = root / "baseline.json"
            final_path = root / "final.json"
            output_path = root / "decision.json"
            baseline_path.write_text(json.dumps(report(baseline=True)))
            broken = report()
            broken["states"][3]["full_clear_count"] = 1
            final_path.write_text(json.dumps(broken))
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--baseline",
                    str(baseline_path),
                    "--report",
                    str(final_path),
                    "--output",
                    str(output_path),
                ],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            decision = json.loads(output_path.read_text())
            self.assertFalse(decision["passed"])
            self.assertTrue(decision["failures"])

    def test_baseline_aggregation_uses_three_provenance_matched_medians(self) -> None:
        samples = [report(baseline=True) for _ in range(3)]
        samples[0]["run_id"] = "sample-1"
        samples[1]["run_id"] = "sample-2"
        samples[2]["run_id"] = "sample-3"
        samples[0]["states"][1]["timing_ms"]["publish"] = 260.0
        samples[1]["states"][1]["timing_ms"]["publish"] = 240.0
        samples[2]["states"][1]["timing_ms"]["publish"] = 250.0
        aggregate = self.gate.aggregate_baseline(samples)
        self.assertEqual(aggregate["states"][1]["timing_ms"]["publish"], 250.0)
        self.assertEqual(aggregate["sample_run_ids"], ["sample-1", "sample-2", "sample-3"])
        self.assertEqual(len(aggregate["sample_sha256"]), 3)

        broken = copy.deepcopy(samples)
        broken[2]["build_sha256"] = "e" * 64
        with self.assertRaisesRegex(ValueError, "build_sha256"):
            self.gate.aggregate_baseline(broken)

    def test_documented_baseline_is_extracted_from_retained_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [
                    sys.executable,
                    str(BASELINE_SCRIPT),
                    "--root",
                    str(ROOT),
                    "--output",
                    directory,
                ],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            tops = json.loads((pathlib.Path(directory) / "tops.json").read_text())
            self.assertEqual(tops["baseline_kind"], "documented_pre_v6")
            self.assertEqual(tops["metrics"]["database_bytes"], 51_118_080)
            self.assertEqual(tops["metrics"]["full_publish_ms"], 1189.0)
            self.assertEqual(tops["metrics"]["vector_compatibility_p95_ms"], 5.59)
            self.assertEqual(len(tops["source_documents"]), 1)
            for source in tops["source_documents"]:
                self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")

            cbm = json.loads((pathlib.Path(directory) / "CBM.json").read_text())
            self.assertEqual(cbm["metrics"]["database_bytes"], 382_048_666)
            self.assertEqual(cbm["metrics"]["full_pipeline_ms"], 93_974.0)

    def test_documented_tops_baseline_drives_strict_gate(self) -> None:
        baseline = {
            "schema_version": 1,
            "baseline_kind": "documented_pre_v6",
            "repository": "tops",
            "run_id": "documented-tops",
            "source_documents": [
                {"path": "docs/source.md", "sha256": "a" * 64}
            ],
            "metrics": {
                "database_bytes": 1_000_000,
                "full_pipeline_ms": 500.0,
                "full_publish_ms": 250.0,
                "vector_compatibility_p95_ms": 10.0,
            },
        }
        decision = self.gate.validate(baseline, report())
        self.assertTrue(decision["passed"], decision)
        self.assertEqual(decision["baseline_kind"], "documented_pre_v6")

        final = report()
        final["states"][1]["timing_ms"]["publish"] = 212.501
        decision = self.gate.validate(baseline, final)
        self.assertFalse(decision["passed"])
        self.assertTrue(any("publisher reduction" in item for item in decision["failures"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
