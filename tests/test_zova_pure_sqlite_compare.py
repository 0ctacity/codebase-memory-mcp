import json
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "zova-pure-sqlite-compare.py"
REPOS = ("tops", "motive", "rvault", "CBM")


def state(name, pure, sidecar=0):
    base = 100.0 if name == "full" else 40.0
    return {
        "name": name,
        "baseline_route": "pure_sqlite" if pure else "transitional_sidecar",
        "state_workload": "full_pipeline" if name == "full" else "changed_state_full_pipeline",
        "passed": True,
        "metadata_mismatches": 0,
        "fts_mismatches": 0,
        "graph_mismatches": 0,
        "vector_mismatches": 0,
        "public_mcp_mismatches": 0,
        "cypher_mismatches": 0,
        "unexpected_fallback_count": 0,
        "cross_workspace_results": 0,
        "compatibility_artifact_count": 0,
        "fresh_full_mismatches": 0,
        "ingestion": {"compat_ms": base, "single_ms": base * 0.8},
        "storage": {
            "compat_db_bytes": 1000,
            "compat_zova_bytes": sidecar,
            "single_bytes": 3000,
        },
        "performance": {
            "graph": {
                "compat_p50_ms": 2.0,
                "compat_p95_ms": 3.0,
                "single_p50_ms": 1.0,
                "single_p95_ms": 1.5,
            },
            "vector": {
                "compat_p50_ms": 4.0,
                "compat_p95_ms": 5.0,
                "single_p50_ms": 2.0,
                "single_p95_ms": 2.5,
            },
        },
    }


class PureSqliteCompareTest(unittest.TestCase):
    def fixtures(self, root):
        pure_args = []
        attempts = []
        for attempt in range(1, 4):
            reports = []
            for repo in REPOS:
                pure_path = root / f"pure-{repo}-{attempt}.json"
                pure_path.write_text(json.dumps({
                    "schema_version": 1,
                    "baseline_route": "pure_sqlite",
                    "repository": repo,
                    "attempt": attempt,
                    "passed": True,
                    "states": [state("full", True), state("incremental", True)],
                }))
                pure_args.extend(["--pure-report", f"{repo}={pure_path}"])
                retained_path = root / f"zova-{repo}-{attempt}.json"
                retained_path.write_text(json.dumps({
                    "repository": repo,
                    "attempt": attempt,
                    "passed": True,
                    "states": [state("full", False, 2000), state("incremental", False, 2000)],
                }))
                reports.append({"repo": repo, "path": str(retained_path)})
            attempts.append({"attempt": attempt, "passed": True, "reports": reports})
        aggregate = root / "section9.json"
        aggregate.write_text(json.dumps({"passed": True, "attempts": attempts}))
        return aggregate, pure_args

    def test_generates_three_attempt_comparison(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            aggregate, pure_args = self.fixtures(root)
            output = root / "comparison.json"
            markdown = root / "comparison.md"
            result = subprocess.run([
                "python3", str(SCRIPT), "--section9", str(aggregate),
                "--output", str(output), "--markdown", str(markdown), *pure_args,
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(output.read_text())
            self.assertTrue(data["passed"])
            self.assertEqual(data["attempt_count"], 3)
            self.assertEqual(data["repositories"]["tops"]["full"]["ingestion"]["pure_sqlite_ms"], 100.0)
            self.assertEqual(data["repositories"]["tops"]["full"]["storage"]["single_file_to_pure_ratio"], 3.0)
            self.assertIn("changed_state_full_pipeline", markdown.read_text())

    def test_rejects_pure_report_with_sidecar_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            aggregate, pure_args = self.fixtures(root)
            path = pathlib.Path(pure_args[1].split("=", 1)[1])
            data = json.loads(path.read_text())
            data["states"][0]["storage"]["compat_zova_bytes"] = 1
            path.write_text(json.dumps(data))
            result = subprocess.run([
                "python3", str(SCRIPT), "--section9", str(aggregate),
                "--output", str(root / "out.json"),
                "--markdown", str(root / "out.md"), *pure_args,
            ], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("sidecar", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
