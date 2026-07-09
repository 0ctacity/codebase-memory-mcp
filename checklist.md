# CLI / Installer Issues We Hit

This checklist tracks CLI and installer problems encountered while using `codebase-memory-mcp`.

- [x] `codebase-memory-mcp install --help` performs installation instead of showing help.
  - Expected: print help and make no changes.
  - Actual: modified Codex, Zed, and shell config.
  - Fixed by: install subcommand now handles `--help` / `-h` before reading or writing user config.

- [x] `codebase-memory-mcp install --ui` does not install or enable the UI variant.
  - Expected: install UI variant, or reject the flag with a clear error.
  - Actual: behaved like another normal install/configure pass.
  - Fixed by: `install --ui` now enables UI mode for UI-capable binaries and clearly rejects non-UI binaries.

- [x] Re-running `install` is not idempotent.
  - Expected: existing config blocks should be detected and left clean.
  - Actual: duplicated the Codex `SessionStart` hook.
  - Fixed by: Codex hook installation strips existing managed blocks before inserting the new block.

- [x] Duplicate cleanup left a stray marker/comment in `~/.codex/config.toml`.
  - Expected: config block markers should stay balanced and clean.
  - Actual: manual cleanup was needed.
  - Fixed by: hook cleanup now removes duplicate and stray managed marker lines.

- [x] Installer silently writes to multiple user config files.
  - Files touched:
    - `~/.codex/config.toml`
    - `~/.codex/AGENTS.md`
    - `~/Library/Application Support/Zed/settings.json`
    - `~/.zshrc`
  - Expected: dry-run or explicit diff/confirmation for each file.
  - Fixed by: `install --plan` now includes the binary target, shell rc, agent config files, instruction files, hooks, and a human-readable summary before any write.

- [x] Zed config uses `args: [""]`.
  - Expected: omit args or use a meaningful argument list.
  - Actual: empty string argument looked suspicious/sloppy.
  - Fixed by: Zed MCP config now omits `args` when there are no arguments.

- [x] UI-capable mode is confusing.
  - `codebase-memory-mcp --ui=true --port=9749` failed on the standard binary.
  - Expected: easy command to install/switch to UI variant.
  - Actual: required going back to `install.sh --ui --skip-config`.
  - Fixed by: install-time `--ui` is now distinct from runtime `--ui=true/false`, with clear help and rejection text.

- [x] UI indexing gives poor/no progress feedback.
  - Expected: progress, loading state, or error.
  - Actual: clicking "Index your first repository" looked like nothing happened until later.
  - Fixed by: indexing now reports `starting`, polls status immediately, and keeps visible progress/error state.

- [x] CLI `index_repository` argument name is unclear.
  - Tried: `codebase-memory-mcp cli index_repository '{"path":"..."}'`
  - Actual error: `repo_path is required`
  - Expected: error should print expected JSON schema or accept `path` as an alias.
  - Fixed by: `index_repository` accepts `path` as an alias for `repo_path`.

- [x] CLI error messages are under-informative.
  - Expected: `required field: repo_path; example: ...`
  - Actual: just `repo_path is required`.
  - Fixed by: missing-path errors now include the required field, alias, and example JSON.

- [x] Storage location is not obvious from CLI/UI.
  - Expected: `where`, `doctor`, or `list_projects --paths` should reveal DB paths.
  - Actual: had to use `lsof` and `tree` to find `~/.cache/codebase-memory-mcp/`.
  - Fixed by: new `doctor` and `where` commands report cache, config, binary, and project DB paths.

- [x] Project DB naming is ugly and path-derived.
  - Example: `Users-atasesli-Desktop-VsCode-zova.db`
  - Expected: clearer project IDs, readable names, or mapping shown in CLI.
  - Fixed by: `doctor` and `where` expose project DB paths so path-derived names can be mapped back to storage.

- [ ] `install` mixes too many responsibilities.
  - Current flow re-signs binary, edits agent configs, edits shell PATH, and installs hooks.
  - Expected: separate commands or explicit substeps:
    - install binary
    - configure Codex
    - configure Zed
    - update PATH
    - add hooks
  - Follow-up: split install into explicit subcommands or interactive substeps.

- [x] No obvious `doctor` command.
  - Expected: one command showing binary path, config files, project DBs, UI mode, indexed projects, and agent integration status.
  - Actual: had to inspect everything manually.
  - Fixed by: new `doctor` command reports binary path, PATH status, cache, project DBs, detected agents, config paths, and UI status.

- [x] Help/flag semantics feel inconsistent.
  - Top-level help says UI flags are `--ui=true` / `--ui=false`, but install script supports `--ui`.
  - Expected: consistent UI installation and runtime terminology.
  - Fixed by: top-level help now separates runtime UI flags from install-time `install --ui`.

## Remaining Follow-Ups

- [ ] Split `install` into smaller explicit operations instead of one broad flow.
- [ ] Consider adding per-file diffs/confirmation on top of `install --plan` for interactive installs.

## Testing Strategy

- [x] Use focused tests as the default proof for scoped CLI/UI changes.
  - For this pass, the high-signal checks are:
    - `build/c/test-runner cli`
    - `build/c/test-runner mcp`
    - `bun run test`
    - `bun run build`
    - `make -f Makefile.cbm cbm`
  - Rationale: these cover the touched CLI, MCP argument, UI progress, frontend build, and Zig-linking surfaces directly.

- [x] Do not treat the full 5k-test suite as the default gate for every scoped change.
  - Current full-suite signal is noisy because it includes unrelated grammar, matrix, integration, network, and port-sensitive tests.
  - Use `make -f Makefile.cbm test` as a broad release-health check, not as the normal iteration proof for small CLI/UI changes.
  - Before making it a merge/release gate, known noisy suites should be fixed, quarantined, or explicitly marked expected-failing.
  - Fixed by: added focused Makefile targets for CLI, MCP, UI tests, UI build, and aggregate scoped verification while leaving the full suite unchanged.

- [x] Add coverage for the UI-capable `install --ui` success path.
  - Current focused tests cover clear rejection for a non-UI binary.
  - Missing: a test/build variant with embedded UI assets that verifies `install --ui` actually enables UI mode.
  - Fixed by: added `test-install-ui-success`, which builds a UI-capable binary, installs into isolated temp paths, and verifies `ui_enabled=true`.

- [x] Add Zova `i8` vector parity tests for the vector mirror prototype.
  - Fixed by: added focused Zova targets and deterministic C tests covering `.zova` conversion, canonical raw `i8` collection mirroring, candidate-filtered `zova_vector_search_in` using `zova_vector_values`, and top-k parity against the existing `cbm_cosine_i8` search path under project/label prefilters.
  - Extended by: `test-zova-real-repo` now validates the same path against a fresh index of this repository.

## Zova Migration Checkpoints

- [x] Short term: `.zova` compatibility/container mode, preserving current C store and direct writer.
  - Decision: this remains the first migration step.
  - Keep app-owned SQLite tables, FTS5/BM25, custom SQL functions, Cypher-like querying, and the direct SQLite writer unchanged at this stage.
  - Fixed by: `CBM_ZOVA_MODE=container` now converts the existing `.db` into an atomic sibling `.zova` sidecar after indexing while leaving the `.db` as source of truth.

- [x] Remove the f32 vector detour as the default migration assumption.
  - Decision: Zova supports raw `i8` vectors with `cosine`, `l2`, and `dot` distance.
  - Implication: codebase-memory should prototype Zova typed `i8` vector collections before any f32 vector backend.

- [x] Prototype a Zova typed `i8` vector mirror/backend.
  - Mirror existing `node_vectors` and `token_vectors` into Zova raw `i8` vector collections.
  - Keep current `node_vectors` / `token_vectors` tables during the prototype.
  - Do not replace the direct SQLite writer for this prototype.
  - Fixed by: `CBM_ZOVA_MODE=i8_vectors` creates `cbm_node_vectors_i8` and `cbm_token_vectors_i8`, imports existing int8 vectors through the Zova C ABI, and keeps the old tables/query path available.

- [x] Update the Zova vector prototype to the canonical C ABI.
  - Decision: local Zova now uses canonical vector APIs with `zova_vector_values`; older separate `_typed` C symbols are not supported in this pass.
  - Fixed by: replaced `_typed` create/put calls with `zova_vector_collection_create` and `zova_vector_put_many`, and rewired prefetch to use candidate-filtered `zova_vector_search_in` with `ZOVA_VECTOR_ELEMENT_TYPE_I8`.

- [x] Build a golden ranking parity set for vector search.
  - Compare current `cbm_cosine_i8` ranking against Zova `i8 + cosine`.
  - Verify top-k overlap, exact ordering, and score correlation.
  - Include realistic SQL prefilters, not only unconstrained vector search.
  - Fixed by: `test-zova-vector-parity` checks deterministic top-k parity through the experimental Zova prefetch path plus existing multi-keyword reranking.

- [x] Measure Zova `i8` vector migration viability empirically.
  - Track ranking parity, SQL prefilter ergonomics, ingestion speed, storage size, and query latency.
  - Go/no-go question: do existing codebase-memory int8 embeddings produce equivalent top-k ranking through Zova `i8 + cosine` under realistic filters?
  - Fixed by: added `test-zova-benchmark-smoke`, which emits a JSON benchmark report with top-k overlap, ordering differences, score correlation, ingestion/query timing, `.db`/`.zova` sizes, mirrored vector count, skipped zero-vector count, and fallback count.
  - Extended by: `test-zova-real-repo` runs the same validation shape against a fresh isolated index of this repository, which is the only real repo in scope right now.

- [x] Validate Zova `i8` vectors and Zova-owned SQL on a fresh real index of this repository.
  - Fixed by: added `scripts/zova-real-repo-validation.sh`, `test-zova-real-repo`, and a Make wrapper.
  - Coverage: the harness indexes only the current `codebase-memory-mcp` checkout, produces `.db` plus sibling `.zova`, compares SQLite `cbm_cosine_i8` vector search with Zova `i8` search, verifies Zova-owned SQL callbacks, verifies FTS/BM25, and writes `build/zova-real-repo/latest/report.json`.
  - Extended by: the report now includes persistent-handle pure `zova_vector_search_in`, full `zova_vector_search`, prefilter candidate count/ratio, candidate-threshold probes, open time, candidate collection time, and top-result hydration timing.
  - Boundary: ranking quality is report-only; broken artifacts, empty data, callback failures, and FTS/BM25 failures are hard failures.

- [x] Add Zig Zova extension bridge smoke test for CBM custom SQL functions.
  - Fixed by: added a narrow Zig bridge that registers `cbm_cosine_i8`, `cbm_camel_split`, `regexp`, and `iregexp` through a `codebase_memory` Zova extension on a Zova-owned SQLite connection.
  - Verified by: `test-zova-bridge` installs the extension on a converted `.zova` and checks cosine, camel split, regex, case-insensitive regex, and invalid regex behavior.
  - Boundary: this is now an optional extension-packaging proof path, not the primary scalar-function integration.

- [x] Update the Zova prototype to the 0.22 C ABI scalar callback path.
  - Decision: Zova `0.22.0` exposes `zova_database_register_function`, so CBM scalar SQL functions can be registered directly on Zova-owned C ABI handles.
  - Fixed by: Zova handles now register `cbm_cosine_i8`, `cbm_camel_split`, `regexp`, and `iregexp` after open, while preserving callback borrowing/copying rules and avoiding same-handle re-entry.
  - Verified by: `test-zova-c-sql-functions` checks cosine edge cases, camel split examples, regex/iregex behavior, invalid regex SQL errors, and an app-table compatibility query on a converted `.zova`.

- [ ] Decide whether Zova vectors become source of truth, derived mirror, or optional backend.
  - Promote only if the parity/performance benchmark is good enough.
  - Otherwise keep current custom `cbm_cosine_i8` path and app-owned vector tables.

- [ ] Decide whether Zova-owned SQL should run selected CBM query paths.
  - Promote only if the C ABI scalar callback path remains clean under broader FTS5/BM25/search compatibility tests.
  - Keep the Zig extension bridge as an optional extension-packaging experiment, not the default runtime path.
  - Otherwise keep custom SQL functions on CBM-owned SQLite handles and use Zova only as sidecar/vector/graph cache.

- [x] Add optional graph mirror smoke prototype after vector parity.
  - Fixed by: `CBM_ZOVA_MODE=graph_mirror` mirrors app `nodes` / `edges` into `cbm_code_graph`; focused tests verify neighbor traversal while keeping custom Cypher authoritative.
