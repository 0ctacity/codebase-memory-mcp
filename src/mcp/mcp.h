/*
 * mcp.h — MCP (Model Context Protocol) server for codebase-memory-mcp.
 *
 * Implements JSON-RPC 2.0 over stdio with the MCP tool calling protocol.
 * Provides 14 graph analysis tools (search, trace, query, index, etc.)
 */
#ifndef CBM_MCP_H
#define CBM_MCP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "zova/cbm_zova_coordination.h"

/* ── Forward declarations ─────────────────────────────────────── */

typedef struct cbm_store cbm_store_t; /* from store/store.h */
struct cbm_watcher;                   /* from watcher/watcher.h */
struct cbm_config;                    /* from cli/cli.h */

/* ── JSON-RPC types ───────────────────────────────────────────── */

typedef struct {
    const char *jsonrpc;    /* "2.0" */
    const char *method;     /* e.g. "initialize", "tools/call" */
    int64_t id;             /* request ID (numeric form; -1 if notification) */
    const char *id_str;     /* non-NULL when id is a JSON string (issue #253) */
    bool has_id;            /* false for notifications */
    const char *params_raw; /* raw JSON string of params */
} cbm_jsonrpc_request_t;

typedef struct {
    int64_t id;
    const char *id_str;      /* non-NULL to echo a string id verbatim (issue #253) */
    const char *result_json; /* JSON string for result (success) */
    const char *error_json;  /* JSON string for error (failure), NULL on success */
    int error_code;          /* JSON-RPC error code */
} cbm_jsonrpc_response_t;

/* ── JSON-RPC parsing / formatting ────────────────────────────── */

/* Parse a JSON-RPC request line. Returns 0 on success, -1 on error.
 * Caller must call cbm_jsonrpc_request_free(). */
int cbm_jsonrpc_parse(const char *line, cbm_jsonrpc_request_t *out);
void cbm_jsonrpc_request_free(cbm_jsonrpc_request_t *r);

/* Format a JSON-RPC response. Returns heap-allocated JSON string. */
char *cbm_jsonrpc_format_response(const cbm_jsonrpc_response_t *resp);

/* Format a JSON-RPC error response. Returns heap-allocated JSON string. */
char *cbm_jsonrpc_format_error(int64_t id, int code, const char *message);

/* ── MCP protocol helpers ─────────────────────────────────────── */

/* Format an MCP tool result with text content. Returns heap-allocated JSON. */
char *cbm_mcp_text_result(const char *text, bool is_error);

/* Return true when notifications/cancelled params target the active request. */
bool cbm_mcp_cancel_request_matches(const char *params_json, int64_t active_id,
                                    const char *active_id_str);

/* Format the tools/list response. Returns heap-allocated JSON. */
char *cbm_mcp_tools_list(void);

/* Return a tool's JSON input_schema string by name (static; do not free), or
 * NULL if the tool is unknown. Backs the CLI flag parser + per-tool --help. */
const char *cbm_mcp_tool_input_schema(const char *tool_name);

/* The tool registry, in tools/list order: count and the i-th tool's name and
 * description (static; do not free; NULL when i is out of range). Agent
 * instructions and --help are generated from these. */
int cbm_mcp_tool_count(void);
const char *cbm_mcp_tool_name(int i);
const char *cbm_mcp_tool_description(int i);

/* Render a JSON object as the compact text form used by format:"compact".
 * Returns heap-allocated text, or NULL when json is not a JSON object. */
char *cbm_mcp_render_compact(const char *json);

/* Format the initialize response. params_json is the raw initialize params
 * (used for protocol version negotiation). Returns heap-allocated JSON. */
char *cbm_mcp_initialize_response(const char *params_json);

/* ── Tool argument helpers ────────────────────────────────────── */

/* Extract a string argument from the tools/call params JSON.
 * Returns heap-allocated copy, or NULL if not found. */
char *cbm_mcp_get_string_arg(const char *args_json, const char *key);

/* Extract an int argument. Returns default_val if not found. */
int cbm_mcp_get_int_arg(const char *args_json, const char *key, int default_val);

/* Extract a bool argument. Returns false if not found. */
bool cbm_mcp_get_bool_arg(const char *args_json, const char *key);

/* Map an internal resolver strategy recorded on an edge to the closed class
 * trace_path publishes with include_evidence: "lsp" | "language_rule" |
 * "heuristic" | "unresolved". NULL only for a NULL/empty strategy. */
const char *cbm_mcp_edge_strategy_class(const char *strategy);

/* Extract the tool name from a tools/call params JSON. Heap-allocated. */
char *cbm_mcp_get_tool_name(const char *params_json);

/* Extract the arguments sub-object from tools/call params. Heap-allocated JSON string. */
char *cbm_mcp_get_arguments(const char *params_json);

/* ── MCP Server ───────────────────────────────────────────────── */

typedef struct cbm_mcp_server cbm_mcp_server_t;

/* Create an MCP server. Projects are read from the cache's cbm.zova. */
cbm_mcp_server_t *cbm_mcp_server_new(void);

/* Free an MCP server. */
void cbm_mcp_server_free(cbm_mcp_server_t *srv);

/* Set external watcher reference (for auto-index registration). Not owned. */
void cbm_mcp_server_set_watcher(cbm_mcp_server_t *srv, struct cbm_watcher *w);

/* Set external config store reference (for auto_index setting). Not owned. */
void cbm_mcp_server_set_config(cbm_mcp_server_t *srv, struct cbm_config *cfg);

/* Run the MCP server event loop on the given streams (typically stdin/stdout).
 * Blocks until EOF on input. Returns 0 on success, -1 on error. */
int cbm_mcp_server_run(cbm_mcp_server_t *srv, FILE *in, FILE *out);

/* Process a single JSON-RPC request line and return the response.
 * Returns heap-allocated JSON response string, or NULL for notifications. */
char *cbm_mcp_server_handle(cbm_mcp_server_t *srv, const char *line);

/* ── Tool handler dispatch (for testing) ──────────────────────── */

/* Handle a tools/call request. Returns MCP tool result JSON. */
char *cbm_mcp_handle_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json);

/* Indexing boundary guard (upstream 7fa5b077, #17) used by the MCP
 * index_repository handler: canonicalize the candidate root, then enforce the
 * CBM_ALLOWED_ROOT containment boundary using path-component semantics (a
 * sibling prefix like /home/alice vs /home/alice-evil must NOT count).
 *
 * repo_path may be NULL. canonical_out (when non-NULL/canonical_sz>0) receives
 * the realpath-resolved path (or the input if it cannot be resolved). Returns
 * 0 when the path may be indexed; otherwise -1 and *out_reason (when
 * non-NULL) is set to a static explanation. CBM_ALLOWED_ROOT unset => every
 * existing path is permitted (no restriction, default behavior unchanged). */
int cbm_mcp_check_index_root(const char *repo_path, char *canonical_out, size_t canonical_sz,
                             const char **out_reason);

/* ── Supervised background index (RSS isolation, #832) ────────── */

/* Run a full index of root_path in a supervised worker SUBPROCESS (the same
 * crash/hang-isolating runner used by handle_index_repository), so the child
 * returns 100% of its RSS to the OS on exit instead of ratcheting the long-lived
 * parent. Builds {"repo_path": root_path} internally. Returns the worker's
 * response string (caller frees) on success, or NULL to signal the caller must
 * degrade to the in-process path (kill switch set, spawn failure, or the process
 * is not a supervisor host). This is the shared entry the watcher re-index
 * (main.zig) and the session auto-index (mcp.c) route through. */
char *cbm_mcp_index_run_supervised_path(const char *root_path);

/* Index hold (ADR-0001) for root_path, canonicalized the way index_repository
 * canonicalizes repo_path: the preparation barrier shared, then the workspace
 * lease. wait_ms 0 tries once (background work); release with
 * cbm_zova_index_hold_release. */
cbm_zova_coord_status_t cbm_mcp_index_hold_acquire(const char *root_path, int64_t wait_ms,
                                                   cbm_zova_index_hold_t *out,
                                                   char *out_workspace_id,
                                                   size_t out_workspace_id_size);
/* Machine-readable reason for a failed hold: workspace_busy,
 * database_replacement_in_progress, cancelled or coordination_unavailable. */
const char *cbm_mcp_index_hold_refusal_reason(cbm_zova_coord_status_t status,
                                              const cbm_zova_index_hold_t *hold);
uint64_t cbm_index_job_queue_waiter_count(const cbm_mcp_server_t *srv);
void cbm_index_job_queue_enter(cbm_mcp_server_t *srv);
void cbm_index_job_queue_leave(cbm_mcp_server_t *srv);

/* ── Idle store eviction ──────────────────────────────────────── */

/* Evict the cached project store if idle for more than timeout_s seconds.
 * Protects initial in-memory stores (those never accessed via a named project).
 * Called automatically by the event loop on poll() timeout. */
void cbm_mcp_server_evict_idle(cbm_mcp_server_t *srv, int timeout_s);

/* Check if the server currently has a cached store open. */
bool cbm_mcp_server_has_cached_store(cbm_mcp_server_t *srv);

/* ── Testing helpers ───────────────────────────────────────────── */

/* Get the store handle from a server (for test setup). */
cbm_store_t *cbm_mcp_server_store(cbm_mcp_server_t *srv);

/* Set the project name associated with the server's current store (for test setup).
 * This prevents resolve_store() from trying to open a .db file when tools specify a project. */
void cbm_mcp_server_set_project(cbm_mcp_server_t *srv, const char *project);

/* ── Cancellation support ─────────────────────────────────────── */

struct cbm_pipeline; /* forward decl */

/* Get the currently active pipeline (for signal handler cancellation).
 * Returns NULL if no pipeline is running. */
struct cbm_pipeline *cbm_mcp_server_active_pipeline(cbm_mcp_server_t *srv);

/* ── URI helpers ───────────────────────────────────────────────── */

/* Parse a file:// URI and extract the filesystem path.
 * Writes to out_path (up to out_size bytes). Returns true on success.
 * On Windows, strips leading / from /C:/path. */
bool cbm_parse_file_uri(const char *uri, char *out_path, int out_size);

/* CBM_INDEX_MAX_RESTARTS as the supervised index path reads it (tests). */
int cbm_index_restart_cap_for_testing(void);

/* Run search_code's in-process scanner over a file list in the scoped format
 * (NUL-separated on POSIX, newline-separated on Windows) and write the hits
 * as "file:line:content\n" into out (tests). Returns 0 on success, 1 when
 * deadline_ms (a cbm_now_ms() value) passed, 2 on failure. */
int cbm_mcp_search_scan_for_testing(const char *filelist_path, const char *root_path,
                                    const char *pattern, bool use_regex, int limit,
                                    uint64_t deadline_ms, char *out, size_t out_size);

#endif /* CBM_MCP_H */
