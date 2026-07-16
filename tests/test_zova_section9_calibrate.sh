#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT/scripts/zova-section9-calibrate.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbm-zova-section9-calibrate.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
expect_fail() { if "$@" >/dev/null 2>&1; then fail "unexpected success: $*"; fi; }

mkdir -p "$TMP/source" "$TMP/bin"
git -C "$TMP/source" init -q
printf 'fixture\n' >"$TMP/source/file.txt"
git -C "$TMP/source" add file.txt
git -C "$TMP/source" -c user.name=Codex -c user.email=codex@example.invalid commit -qm fixture
printf 'build\n' >"$TMP/bin/build"
chmod +x "$TMP/bin/build"

cat >"$TMP/bin/repository-runner" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$CBM_ZOVA_SECTION9_REPOSITORY" == tops ]]
[[ "$CBM_ZOVA_SECTION9_ATTEMPT" == 0 ]]
[[ "$CBM_ZOVA_SECTION9_CALIBRATION_MODE" == 1 ]]
[[ "$CBM_ZOVA_SECTION9_CALIBRATION_SAMPLE" =~ ^[1-5]$ ]]
mkdir -p "$CBM_ZOVA_SECTION9_RUN_ROOT"
python3 - "$CBM_ZOVA_SECTION9_RUN_ROOT/repository-report.json" \
  "$CBM_ZOVA_SECTION9_BUILD_BINARY" <<'PY'
import hashlib, json, os, pathlib, sys
output, build = map(pathlib.Path, sys.argv[1:])
build_sha = hashlib.sha256(build.read_bytes()).hexdigest()
if os.environ.get("CBM_FAKE_BUILD_MISMATCH") == "1": build_sha = "b" * 64
sample = int(os.environ["CBM_ZOVA_SECTION9_CALIBRATION_SAMPLE"])
defaults = {
    "full_ingestion": (1.041, 1.08, 1.06, 1.09, 1.07),
    "incremental_ingestion": (1.10, 1.12, 1.11, 1.14, 1.13),
    "full_storage": (1.05, 1.07, 1.06, 1.08, 1.07),
    "incremental_storage": (1.18, 1.20, 1.19, 1.17, 1.16),
}
ratios = {
    key: float(os.environ.get(f"CBM_FAKE_{key.upper()}", values[sample - 1]))
    for key, values in defaults.items()
}
zero = {key: 0 for key in (
    "metadata_mismatches", "fts_mismatches", "graph_mismatches", "vector_mismatches",
    "public_mcp_mismatches", "cypher_mismatches", "unexpected_fallback_count",
    "cross_workspace_results", "compatibility_artifact_count", "fresh_full_mismatches",
)}
def state(name):
    ingest = ratios[f"{name}_ingestion"]; storage = ratios[f"{name}_storage"]
    return {
        "name": name, "passed": True, **zero,
        "fts_query_count": 20, "graph_sample_count": 10, "vector_query_count": 4,
        "public_mcp_case_count": 8, "cypher_native_route_count": 5,
        "cypher_compat_route_count": 9,
        "generation": {"active": 1, "integrity_matches": True},
        "performance": {"sample_count": 20,
            "graph": {"compat_p50_ms": 10, "compat_p95_ms": 20,
                      "single_p50_ms": 10, "single_p95_ms": 20},
            "vector": {"compat_p50_ms": 10, "compat_p95_ms": 20,
                       "single_p50_ms": 10, "single_p95_ms": 20}},
        "ingestion": {"compat_ms": 100, "single_ms": ingest * 100, "ratio": ingest},
        "storage": {"compat_db_bytes": 500, "compat_zova_bytes": 500,
                    "single_bytes": int(storage * 1000), "ratio": storage,
                    "page_count": 1, "freelist_count": 0, "wal_bytes": 0},
    }
report = {
    "schema_version": 1, "repository": "tops", "source_commit": "a" * 40,
    "run_id": f"tops-calibration-{sample}", "attempt": 0, "calibration_mode": True,
    "build_sha256": build_sha, "calibration_sha256": None,
    "focused_evidence_sha256": None, "flagged_full_authority": True,
    "passed": True, "states": [state("full"), state("incremental")],
}
output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
PY
SH
chmod +x "$TMP/bin/repository-runner"

invoke() {
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$1" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository-runner" "$SCRIPT"
}

invoke "$TMP/success"
python3 - "$TMP/success/calibration.json" <<'PY'
import hashlib, json, sys
d=json.load(open(sys.argv[1]))
assert d["passed"] is True and d["repository"] == "tops"
assert d["sample_count"] == 5 and len(d["samples"]) == 5
assert d["observed"] == {
    "full_ingestion": 1.09, "incremental_ingestion": 1.14,
    "full_storage": 1.08, "incremental_storage": 1.20,
}
assert d["limits"] == {
    "full_ingestion": 1.14, "incremental_ingestion": 1.19,
    "full_storage": 1.13, "incremental_storage": 1.25,
}
payload=dict(d); actual=payload.pop("digest")
canonical=json.dumps(payload,sort_keys=True,separators=(",", ":")).encode()
assert actual == hashlib.sha256(canonical).hexdigest()
PY
expect_fail invoke "$TMP/success"

expect_fail env CBM_FAKE_FULL_INGESTION=1.201 \
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$TMP/over-cap" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository-runner" "$SCRIPT"
[[ -s "$TMP/over-cap/candidate-1/repository-report.json" ]] ||
  fail "over-cap diagnostics missing"

expect_fail env CBM_FAKE_BUILD_MISMATCH=1 \
  CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$TMP/build-mismatch" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository-runner" "$SCRIPT"

expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_REPOSITORY=motive \
  CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$TMP/non-tops" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository-runner" "$SCRIPT"

expect_fail env CBM_ZOVA_VALIDATION_REPO="$TMP/source" \
  CBM_ZOVA_SECTION9_CALIBRATION_ROOT="$TMP/latest/calibration" \
  CBM_ZOVA_SECTION9_BUILD_BINARY="$TMP/bin/build" \
  CBM_ZOVA_SECTION9_REPOSITORY_RUNNER="$TMP/bin/repository-runner" "$SCRIPT"

echo "PASS: Zova Section 9 calibration workflow"
