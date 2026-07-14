# Pure SQLite versus single-file Zova comparison implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the Section 9 workload against a true SQLite-only baseline and compare its fresh measurements with the retained single-file Zova evidence.

**Architecture:** Add an opt-in baseline selector to the existing real-repository harness while preserving its default transitional behavior. Drive it with separate staged scripts and produce standalone JSON/Markdown comparison evidence; never rewrite Section 9 artifacts.

**Tech Stack:** C test harness, Bash orchestration, Python 3 evidence validation/reporting, SQLite, Zova C SDK.

## Global constraints

- Keep `build/zova-section9-promotion/` immutable.
- Pure SQLite means `CBM_ZOVA_MODE=off`, the single-file flag unset, one `project.db`, and no sidecar.
- Use the same current build for both routes to avoid source/build drift.
- Preserve the retained `incremental` state name for joins, but describe it as `changed_state_full_pipeline` because it uses `CBM_MODE_FULL`.
- Run TOPS and inspect it before motive, rvault, or CBM.
- Do not apply Section 9 promotion thresholds to this diagnostic comparison.

---

### Task 1: Pure baseline selection in the real-repository harness

**Files:**
- Modify: `tests/test_zova_single_file_real_repo.c`

**Interfaces:**
- Consumes: `CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE=1`.
- Produces: state JSON with `baseline_route`, `state_workload`, zero compatibility sidecar bytes, and parity-only `passed` behavior in pure mode.

- [ ] Add a test that toggles `CBM_ZOVA_SECTION9_PURE_SQLITE_BASELINE` and expects the selector to be false/unset and true/`1`.
- [ ] Run `scripts/zova-run-tests.sh zova_single_file_promotion_real_repo` and confirm the new test fails because the selector does not exist.
- [ ] Add the selector and make the unflagged route set `CBM_ZOVA_MODE=off`, unset the experimental flag, reject a sidecar, and report zero sidecar bytes only when selected.
- [ ] Make public baseline capture and graph timing explicitly select `off` for pure SQLite and the flagged route for single-file, restoring the process environment after every pair.
- [ ] Add route/workload fields to state JSON and exclude read-ratio thresholds from the pure diagnostic `passed` calculation while retaining all parity/count requirements.
- [ ] Re-run the focused suite and confirm it passes.

### Task 2: Repository runner for pure SQLite evidence

**Files:**
- Create: `scripts/zova-pure-sqlite-repository-validation.sh`
- Create: `tests/test_zova_pure_sqlite_repository_validation.sh`

**Interfaces:**
- Consumes: repository path/name, attempt number, built binary/test runner, and fresh run root.
- Produces: `repository-report.json` with full and changed-state reports and provenance.

- [ ] Write a fake-runner shell test requiring two ordered states, the pure-baseline environment variable, retained cache reuse, honest workload labels, and no Section 9 gate invocation.
- [ ] Run the shell test and confirm failure because the repository runner is missing.
- [ ] Implement cloning, probe mutation, two state executions, report assembly, locks, progress output, disk guard, provenance hashes, and success cleanup using the existing Section 9 repository script pattern.
- [ ] Run the shell test and confirm it passes.

### Task 3: Comparison generator

**Files:**
- Create: `scripts/zova-pure-sqlite-compare.py`
- Create: `tests/test_zova_pure_sqlite_compare.py`

**Interfaces:**
- Consumes: three pure reports per repository plus `section9-latest-gate.json` and its referenced retained repository reports.
- Produces: `comparison.json` and `comparison.md` with averages, ratios, deltas, hashes, and caveats.

- [ ] Write fixtures covering valid aggregation, missing repository/attempt, failed parity, nonzero sidecar bytes, incorrect route/workload labels, and malformed retained evidence.
- [ ] Run `python3 -m unittest tests.test_zova_pure_sqlite_compare` and confirm failure because the generator is missing.
- [ ] Implement strict schema/provenance validation, per-state averages, pure/Zova ratios, Markdown tables, and atomic output replacement.
- [ ] Re-run the Python test and confirm it passes.

### Task 4: Staged multi-repository orchestration

**Files:**
- Create: `scripts/zova-pure-sqlite-multi-repo-validation.sh`
- Create: `tests/test_zova_pure_sqlite_multi_repo_validation.sh`

**Interfaces:**
- Consumes: TOPS, motive, rvault, and CBM repository paths plus the retained Section 9 aggregate.
- Produces: three attempts per repository and final comparison outputs.

- [ ] Write a fake-runner test proving build-once, all three TOPS attempts, TOPS validation, then motive, rvault, CBM, and final comparison order; prove fail-fast prevents larger runs.
- [ ] Run the shell test and confirm failure because the orchestrator is missing.
- [ ] Implement the exact staged order, disk checks, fresh-root/lock enforcement, report validation checkpoints, and comparison invocation.
- [ ] Re-run the shell test and confirm it passes.

### Task 5: Real staged evidence

**Files:**
- Generate: `build/zova-pure-sqlite-comparison/**`

- [ ] Build once with `scripts/zova-build-once.sh` and record progress.
- [ ] Run focused C, shell, and Python tests.
- [ ] Run all three TOPS attempts only.
- [ ] Validate TOPS: no sidecar, route/workload labels correct, every parity counter zero, all measurements finite, and comparison input hashes correct.
- [ ] Run motive and rvault attempts.
- [ ] Run CBM attempts last.
- [ ] Generate and validate `comparison.json` and `comparison.md`.

### Task 6: Correct the architecture report

**Files:**
- Modify: `docs/cbm-zova-architecture.md`

- [ ] Replace “original CBM” dual-file wording with a three-route model: pure SQLite, transitional sidecar, and single-file Zova.
- [ ] Replace unsupported ingestion/storage claims with the pure comparison tables.
- [ ] Keep transitional comparisons in a separately labeled table.
- [ ] State that the retained `incremental` measurement is a changed-state full-pipeline rerun.
- [ ] Run link, digest, placeholder, Markdown-fence, and `git diff --check` validation.

### Task 7: Final verification

- [ ] Run the focused C suite, all new shell tests, and all new Python tests from a clean temporary cache.
- [ ] Validate every generated report hash and ensure the retained Section 9 aggregate hash is unchanged.
- [ ] Run `git diff --check` and inspect only files in this plan's scope.
- [ ] Report the actual pure SQLite versus single-file Zova results, separating measured facts from inference.
