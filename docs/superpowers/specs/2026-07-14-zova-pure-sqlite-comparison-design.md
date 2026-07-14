# Pure SQLite versus single-file Zova comparison design

## Goal

Measure pure SQLite with the same Section 9 repository workload and read
queries, then compare the fresh SQLite results with the retained single-file
Zova results without modifying the immutable Section 9 evidence.

## Comparison boundary

The new baseline uses the current binary and source tree so parsing, semantic
extraction, vector generation, compiler settings, and test machinery remain
constant. Pure SQLite is selected with:

```text
CBM_ZOVA_MODE=off
CBM_ZOVA_SINGLE_FILE_EXPERIMENTAL unset
```

The baseline must create `project.db`, must not create a Zova sidecar, and must
report sidecar bytes as zero. This isolates the storage route more cleanly than
building upstream CBM at another commit, which would introduce unrelated code
drift.

The retained Section 9 state called `incremental` changes a probe file between
runs but invokes `cbm_pipeline_new(..., CBM_MODE_FULL)`. The comparison keeps
this exact workload for comparability and records both its retained name and
the honest description `changed_state_full_pipeline`. It does not treat that
number as evidence for the real incremental pipeline.

## Architecture

The existing promotion real-repository test gains an opt-in pure-SQLite
baseline switch. With the switch absent, all existing behavior remains
unchanged. With it enabled:

1. the unflagged route runs with Zova disabled;
2. artifact validation rejects any sidecar;
3. public MCP baseline calls run with Zova disabled;
4. graph measurements explicitly alternate pure SQLite and the retained
   single-file workspace query, preventing process-global mode leakage;
5. vector measurements retain their existing SQLite-off versus single-file
   selection;
6. state reports identify the baseline as `pure_sqlite`.

A separate runner produces fresh pure-SQLite reports under
`build/zova-pure-sqlite-comparison/`. A comparison tool reads those reports and
the immutable
`build/zova-section9-promotion/section9-latest-gate.json` report graph. It emits
per-repository full/changed-state tables for ingestion, storage, graph p50/p95,
and vector p50/p95, plus parity counters and provenance hashes.

## Staged execution

Execution is deliberately staged:

1. build once;
2. run focused unit tests for pure artifact selection and report labeling;
3. run TOPS only;
4. inspect the TOPS report and comparison for zero sidecars, zero parity
   mismatches, and valid measurements;
5. run motive and rvault;
6. run CBM last;
7. generate the aggregate comparison.

No larger repository runs before TOPS passes.

## Evidence and output

The new evidence is separate from Section 9:

```text
build/zova-pure-sqlite-comparison/
  tops/repository-report.json
  motive/repository-report.json
  rvault/repository-report.json
  CBM/repository-report.json
  comparison.json
  comparison.md
```

Each repository report records the source commit, binary SHA-256, route,
artifact counts, measurements, and parity results. `comparison.json` records
the SHA-256 of every input report and of the retained Section 9 aggregate.

The report must not apply the old Section 9 promotion thresholds to the pure
SQLite comparison. It reports measured ratios and differences; it does not
convert them into a cutover decision.

## Testing

Unit/fixture tests prove:

- pure mode selects `off` and leaves the experimental flag unset;
- a pure run fails if a sidecar exists;
- pure storage uses only project DB bytes;
- legacy Section 9 behavior is unchanged when the switch is absent;
- comparison rejects missing repositories, mismatched states, failed reports,
  non-pure baselines, or changed retained-input hashes;
- the changed state is described as `changed_state_full_pipeline`.

Real-repository validation proves the complete workload on TOPS before larger
repositories. Final verification includes focused suites, report-schema
validation, digest validation, and `git diff --check`.

## Documentation correction

After evidence generation, `docs/cbm-zova-architecture.md` will distinguish:

- upstream/pure SQLite CBM;
- the transitional SQLite-plus-Zova-sidecar route;
- flagged single-file Zova.

Only conclusions supported by the corresponding baseline will remain. The
document will explicitly distinguish exact measurements from architectural
inference and will not call the changed-state full-pipeline run incremental
indexing.
