//! pass_tests.zig — Create TESTS edges from test functions to production functions.
//!
//! Scans CALLS edges in the graph buffer: if the source function has
//! is_test=true and the target does not, creates a TESTS edge.
//!
//! Also creates TESTS_FILE edges from test File nodes to the production
//! File nodes they correspond to (naming convention: _test.go → .go, etc.)
//!
//! Depends on: the call resolver having populated CALLS edges

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const streq = util.streq;
const Node = c.cbm_gbuf_node_t;
const Edge = c.cbm_gbuf_edge_t;

const PT_TEST_LEN = 4; // strlen("Test")
const PT_BENCHMARK_LEN = 9; // strlen("Benchmark")
const PT_EXAMPLE_LEN = 7; // strlen("Example")
const PT_SUFFIX_LEN = 8; // strlen("_test.go")
const PT_EXT_PY = 3; // strlen(".py") / strlen(".go")
const PT_PYTEST_PREFIX = 5; // strlen("test_")
const PT_FOUND_SKIP = 5; // strlen(".test") or strlen(".spec")

fn hasPrefix(s: CStr, comptime prefix: [:0]const u8) bool {
    return c.strncmp(s, prefix, prefix.len) == 0;
}

/// Check if a node has is_test:true in its properties JSON.
fn nodeIsTest(n: *const Node) bool {
    if (n.properties_json == null) return false;
    return c.strstr(n.properties_json, "\"is_test\":true") != null;
}

/// Helper to check suffix.
fn strEndsWith(s: CStr, slen: usize, suffix: [*:0]const u8) bool {
    const sflen = c.strlen(suffix);
    return slen >= sflen and streq(s + slen - sflen, suffix);
}

/// Check if a file path looks like a test file (language-agnostic).
pub export fn cbm_is_test_path(path: CStr) bool {
    if (path == null) return false;
    var base: CStr = c.strrchr(path, '/');
    base = if (base != null) base + 1 else path;
    const len = c.strlen(path);

    // Prefix-based: Python test_*.py
    if (hasPrefix(base, "test_")) return true;

    // Suffix-based: _test.<ext> pattern (Go, Python, Rust, C++, Lua)
    if (strEndsWith(path, len, "_test.go") or strEndsWith(path, len, "_test.py") or
        strEndsWith(path, len, "_test.rs") or strEndsWith(path, len, "_test.cpp") or
        strEndsWith(path, len, "_test.lua"))
    {
        return true;
    }

    // .test.<ext> / .spec.<ext> pattern (JS/TS/TSX)
    if (c.strstr(path, ".test.ts") != null or c.strstr(path, ".spec.ts") != null or
        c.strstr(path, ".test.js") != null or c.strstr(path, ".spec.js") != null or
        c.strstr(path, ".test.tsx") != null or c.strstr(path, ".spec.tsx") != null)
    {
        return true;
    }

    // Name ends with "Test" or "Spec" before extension (Java, Kotlin, C#, PHP, Scala)
    if (strEndsWith(path, len, "Test.java") or strEndsWith(path, len, "Test.kt") or
        strEndsWith(path, len, "Test.cs") or strEndsWith(path, len, "Test.php") or
        strEndsWith(path, len, "Spec.scala"))
    {
        return true;
    }

    // Directory-based: __tests__/, tests/, test/, spec/
    if (c.strstr(path, "__tests__/") != null or c.strstr(path, "/tests/") != null or
        c.strstr(path, "/test/") != null or c.strstr(path, "/spec/") != null)
    {
        return true;
    }
    // Also match if path STARTS with these directories
    if (hasPrefix(path, "tests/") or hasPrefix(path, "test/") or hasPrefix(path, "spec/") or
        hasPrefix(path, "__tests__/"))
    {
        return true;
    }

    // Ruby: _spec.rb suffix
    if (strEndsWith(path, len, "_spec.rb")) return true;

    return false;
}

fn isUpper(ch: u8) bool {
    return ch >= 'A' and ch <= 'Z';
}

/// Check if a function name looks like a test function (language-agnostic).
pub export fn cbm_is_test_func_name(name: CStr) bool {
    if (name == null) return false;
    // Go: Test/Benchmark/Example + uppercase letter or end-of-string.
    // "TestFoo" = test, "Testable" = not test (lowercase after prefix).
    if (hasPrefix(name, "Test") and (name[PT_TEST_LEN] == 0 or isUpper(name[PT_TEST_LEN]))) {
        return true;
    }
    if (hasPrefix(name, "Benchmark") and
        (name[PT_BENCHMARK_LEN] == 0 or isUpper(name[PT_BENCHMARK_LEN])))
    {
        return true;
    }
    if (hasPrefix(name, "Example") and
        (name[PT_EXAMPLE_LEN] == 0 or isUpper(name[PT_EXAMPLE_LEN])))
    {
        return true;
    }
    // Python/Rust/C++/Lua/Java: test_ or test prefix (lowercase)
    if (hasPrefix(name, "test_")) return true;
    if (hasPrefix(name, "test") and isUpper(name[PT_TEST_LEN])) return true;
    // JS/TS: test/it/describe + lifecycle hooks
    if (streq(name, "test") or streq(name, "it") or streq(name, "describe") or
        streq(name, "beforeAll") or streq(name, "afterAll") or
        streq(name, "beforeEach") or streq(name, "afterEach"))
    {
        return true;
    }
    // Julia: @testset, @test
    if (streq(name, "@testset") or streq(name, "@test")) return true;
    return false;
}

/// Assemble dir/name+ext into a heap-allocated path.
fn buildPath(test_path: CStr, dir_len: usize, name: CStr, name_len: usize, ext: CStr, ext_len: usize) MutStr {
    const result: MutStr = @ptrCast(c.malloc(dir_len + 1 + name_len + ext_len + 1));
    if (dir_len > 0) {
        @memcpy(result[0..dir_len], test_path[0..dir_len]);
        result[dir_len] = '/';
        @memcpy(result[dir_len + 1 ..][0..name_len], name[0..name_len]);
        @memcpy(result[dir_len + 1 + name_len ..][0 .. ext_len + 1], ext[0 .. ext_len + 1]);
    } else {
        @memcpy(result[0..name_len], name[0..name_len]);
        @memcpy(result[name_len..][0 .. ext_len + 1], ext[0 .. ext_len + 1]);
    }
    return result;
}

/// Try to derive the production file path from a test file path.
/// Returns heap-allocated string or NULL. Caller must free().
fn testToProdPath(test_path: CStr) MutStr {
    if (test_path == null) return null;

    var base: CStr = c.strrchr(test_path, '/');
    const dir_len: usize = if (base != null) util.ptrDiff(base, test_path) else 0;
    base = if (base != null) base + 1 else test_path;

    // Go: foo_test.go → foo.go
    const suffix: CStr = c.strstr(base, "_test.go");
    if (suffix != null and suffix[PT_SUFFIX_LEN] == 0) {
        return buildPath(test_path, dir_len, base, util.ptrDiff(suffix, base), ".go", PT_EXT_PY);
    }

    // Python: test_foo.py → foo.py
    if (hasPrefix(base, "test_")) {
        const ext: CStr = c.strstr(base, ".py");
        if (ext != null and ext[PT_EXT_PY] == 0) {
            return buildPath(test_path, dir_len, base + PT_PYTEST_PREFIX, util.ptrDiff(ext, base) -% PT_PYTEST_PREFIX, ".py", PT_EXT_PY);
        }
    }

    // JS/TS: foo.test.ts → foo.ts, foo.spec.ts → foo.ts
    for ([_][*:0]const u8{ ".test.", ".spec." }) |pat| {
        const found: CStr = c.strstr(base, pat);
        if (found != null) {
            const ext: CStr = found + PT_FOUND_SKIP;
            return buildPath(test_path, dir_len, base, util.ptrDiff(found, base), ext, c.strlen(ext));
        }
    }

    return null;
}

fn nodeLooksLikeTest(n: *const Node) bool {
    return nodeIsTest(n) or (n.file_path != null and cbm_is_test_path(n.file_path));
}

/// Create TESTS edges from test functions to production functions via CALLS edges.
fn createTestsEdges(ctx: *c.cbm_pipeline_ctx_t) c_int {
    var count: c_int = 0;
    var call_edges: [*c][*c]const Edge = null;
    var call_count: c_int = 0;
    const rc = c.cbm_gbuf_find_edges_by_type(ctx.gbuf, "CALLS", &call_edges, &call_count);
    if (rc != 0 or call_count == 0) return 0;

    var i: usize = 0;
    while (i < call_count) : (i += 1) {
        const e: *const Edge = call_edges[i];
        const src: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(ctx.gbuf, e.source_id)) orelse continue;
        const tgt: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_id(ctx.gbuf, e.target_id)) orelse continue;

        if (!nodeLooksLikeTest(src)) continue;
        if (nodeLooksLikeTest(tgt)) continue;
        if (!cbm_is_test_func_name(src.name)) continue;

        _ = c.cbm_gbuf_insert_edge(ctx.gbuf, src.id, tgt.id, "TESTS", "{}");
        count += 1;
    }
    return count;
}

/// Create TESTS_FILE edges from test File nodes to production File nodes.
fn createTestsFileEdges(ctx: *c.cbm_pipeline_ctx_t) c_int {
    var count: c_int = 0;
    var file_nodes: [*c][*c]const Node = null;
    var file_node_count: c_int = 0;
    const rc = c.cbm_gbuf_find_by_label(ctx.gbuf, "File", &file_nodes, &file_node_count);
    if (rc != 0) return 0;

    var i: usize = 0;
    while (i < file_node_count) : (i += 1) {
        const fnode: *const Node = file_nodes[i];
        if (fnode.file_path == null or !cbm_is_test_path(fnode.file_path)) continue;

        const prod_path = testToProdPath(fnode.file_path);
        if (prod_path == null) continue;

        const prod_qn = c.cbm_pipeline_fqn_compute(ctx.project_name, prod_path, "__file__");
        const prod_node: ?*const Node = c.cbm_gbuf_find_by_qn(ctx.gbuf, prod_qn);
        c.free(prod_qn);
        c.free(prod_path);

        if (prod_node) |pn| {
            if (fnode.id != pn.id) {
                _ = c.cbm_gbuf_insert_edge(ctx.gbuf, fnode.id, pn.id, "TESTS_FILE", "{}");
                count += 1;
            }
        }
    }
    return count;
}

pub export fn cbm_pipeline_pass_tests(ctx: *c.cbm_pipeline_ctx_t, files: ?*const c.cbm_file_info_t, file_count: c_int) c_int {
    interop.logInfo("pass.start", .{ @as(CStr, "pass"), @as(CStr, "tests") });
    _ = files;
    _ = file_count;

    const tests_count = createTestsEdges(ctx);
    const tests_file_count = createTestsFileEdges(ctx);

    interop.logInfo("pass.done", .{
        @as(CStr, "pass"),       @as(CStr, "tests"),
        @as(CStr, "tests"),      @as(CStr, c.cbm_log_itoa(tests_count)),
        @as(CStr, "tests_file"), @as(CStr, c.cbm_log_itoa(tests_file_count)),
    });
    return 0;
}
