const std = @import("std");

const Sanitizer = enum {
    none,
    address,
    thread,
};

const BuildKind = enum {
    prod,
    tests,
};

const Config = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    with_zova: bool,
    with_ui: bool,
    zova_root: []const u8,
    sanitize: Sanitizer,
    static_link: bool,
    target_macos_version: []const u8,
};

pub fn build(b: *std.Build) void {
    const cfg = Config{
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
        .with_zova = b.option(bool, "with-zova", "Build with Zova C ABI support") orelse false,
        .with_ui = b.option(bool, "with-ui", "Use embedded UI assets instead of the stub") orelse false,
        .zova_root = b.option([]const u8, "zova-root", "Path to the local Zova checkout") orelse "../zova",
        .sanitize = b.option(Sanitizer, "sanitize", "Sanitizer mode: none, address, thread") orelse .none,
        .static_link = b.option(bool, "static", "Request static system library linkage where supported") orelse false,
        .target_macos_version = b.option([]const u8, "target-macos-version", "Minimum macOS deployment target") orelse "15.0",
    };

    if (cfg.with_zova) {
        checkZovaInputs(b, cfg);
    }

    const cbm_exe = addCbmExecutable(b, cfg, .prod, cfg.with_ui);
    const cbm_copy = copyArtifact(b, cbm_exe, "build/c/codebase-memory-mcp");
    const cbm_step = b.step("cbm", "Build build/c/codebase-memory-mcp");
    cbm_step.dependOn(&cbm_copy.step);

    const test_runner = addTestRunner(b, cfg);
    const test_runner_copy = copyArtifact(b, test_runner, "build/c/test-runner");
    const test_runner_step = b.step("test-runner", "Build build/c/test-runner");
    test_runner_step.dependOn(&test_runner_copy.step);

    _ = addRunSuiteStep(b, "test", "Run the full C test runner", test_runner, null, null);
    const test_cli_step = addRunSuiteStep(b, "test-cli", "Run focused CLI tests", test_runner, "cli", null);
    const test_mcp_step = addRunSuiteStep(b, "test-mcp", "Run focused MCP tests", test_runner, "mcp", null);

    const test_ui_step = addUiStep(b, "test-ui", "Run graph-ui tests with Bun", &.{ "bun", "run", "test" });
    const test_ui_build_step = addUiStep(b, "test-ui-build", "Build graph-ui with Bun", &.{ "bun", "run", "build" });

    const focused = b.step("test-focused-cli-ui", "Run focused CLI/MCP/UI checks and build cbm");
    focused.dependOn(test_cli_step);
    focused.dependOn(test_mcp_step);
    focused.dependOn(test_ui_step);
    focused.dependOn(test_ui_build_step);
    focused.dependOn(cbm_step);

    addZovaSuiteStep(b, cfg, test_runner, "test-zova-link", "off", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-container", "container", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-i8", "i8_vectors", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-vector-parity", "i8_vectors", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-benchmark-smoke", "i8_vectors", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-c-sql-functions", "container", "zova_c_sql_functions");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-bridge", "container", "zova_bridge");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-graph-mirror", "graph_mirror", "zova");
    addZovaRealRepoStep(b, cfg, cbm_copy, test_runner_copy);

    const cbm_with_ui = b.step("cbm-with-ui", "Build UI-capable binary through the current embed flow");
    const ui_build = b.addSystemCommand(&.{ "make", "-f", "Makefile.cbm", "cbm-with-ui" });
    cbm_with_ui.dependOn(&ui_build.step);

    const test_install_ui = b.step("test-install-ui-success", "Verify UI-capable install --ui succeeds");
    const install_ui = b.addSystemCommand(&.{ "bash", "scripts/test-install-ui-success.sh" });
    install_ui.step.dependOn(cbm_with_ui);
    test_install_ui.dependOn(&install_ui.step);

    const test_install_safety =
        b.step("test-install-safety", "Verify installer replacement and idempotency safeguards");
    const install_safety =
        b.addSystemCommand(&.{ "bash", "tests/test_install_script_safety.sh" });
    test_install_safety.dependOn(&install_safety.step);
}

fn checkZovaInputs(b: *std.Build, cfg: Config) void {
    const include_path = b.pathJoin(&.{ cfg.zova_root, "include" });
    const lib_path = b.pathJoin(&.{ cfg.zova_root, "zig-out/lib/libzova_c.a" });
    const root_path = b.pathJoin(&.{ cfg.zova_root, "src/root.zig" });
    b.build_root.handle.access(b.graph.io, include_path, .{}) catch {
        std.debug.panic("error: ZOVA_ROOT not found or incomplete: {s}", .{cfg.zova_root});
    };
    b.build_root.handle.access(b.graph.io, lib_path, .{}) catch {
        std.debug.panic("error: Zova C static library not found: {s}\n       Build Zova first with: cd {s} && zig build", .{ lib_path, cfg.zova_root });
    };
    b.build_root.handle.access(b.graph.io, root_path, .{}) catch {
        std.debug.panic("error: Zova Zig root not found: {s}", .{root_path});
    };
    const header_path = b.pathJoin(&.{ cfg.zova_root, "include/zova.h" });
    const header = b.build_root.handle.readFileAlloc(b.graph.io, header_path, b.allocator, .limited(1024 * 1024)) catch {
        std.debug.panic("error: unable to read Zova C ABI header: {s}", .{header_path});
    };
    if (!std.mem.containsAtLeast(u8, header, 1, "Zova C ABI, v0.") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_database_register_function") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_search_in") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_search_by_id_in") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_delete_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_delete_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_keyed") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_prepared_keyed") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_prepared_keyed_with_payloads") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_payload_get_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_payload_replace_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_fresh_build_begin"))
    {
        std.debug.panic("error: a pinned format-9 Zova C ABI header with SQL callbacks, candidate vector search, batch deletion, prepared payload graph builds, edge payload access, and fresh-build sessions is required: {s}", .{header_path});
    }
}

fn addCbmExecutable(b: *std.Build, cfg: Config, kind: BuildKind, with_ui_assets: bool) *std.Build.Step.Compile {
    const module = createCModule(b, cfg, kind);
    addNativeSources(b, module, cfg, kind, false, with_ui_assets);
    module.addCSourceFile(.{
        .file = b.path("src/main.c"),
        .flags = flags(b, cfg, kind, .normal),
    });
    const exe = b.addExecutable(.{
        .name = "codebase-memory-mcp",
        .root_module = module,
    });
    return exe;
}

fn addTestRunner(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    const module = createCModule(b, cfg, .tests);
    addNativeSources(b, module, cfg, .tests, true, false);
    module.addCSourceFiles(.{
        .files = &all_test_sources,
        .flags = flags(b, cfg, .tests, .normal),
    });
    const exe = b.addExecutable(.{
        .name = "test-runner",
        .root_module = module,
    });
    return exe;
}

fn createCModule(b: *std.Build, cfg: Config, kind: BuildKind) *std.Build.Module {
    const sanitize_c: ?std.zig.SanitizeC = switch (cfg.sanitize) {
        .none, .thread => null,
        .address => .full,
    };
    const sanitize_thread = if (cfg.sanitize == .thread) true else null;
    const module = b.createModule(.{
        .target = cfg.target,
        .optimize = cfg.optimize,
        .link_libc = true,
        .link_libcpp = true,
        .sanitize_c = sanitize_c,
        .sanitize_thread = sanitize_thread,
    });
    _ = kind;
    addIncludePaths(b, module, cfg);
    module.linkSystemLibrary("m", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    module.linkSystemLibrary("pthread", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    module.linkSystemLibrary("z", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    return module;
}

fn addIncludePaths(b: *std.Build, module: *std.Build.Module, cfg: Config) void {
    module.addIncludePath(b.path("src"));
    module.addIncludePath(b.path("vendored"));
    module.addIncludePath(b.path("vendored/sqlite3"));
    module.addIncludePath(b.path("vendored/mimalloc/include"));
    module.addIncludePath(b.path("internal/cbm"));
    module.addIncludePath(b.path("internal/cbm/vendored/ts_runtime/include"));
    module.addIncludePath(b.path("internal/cbm/vendored/ts_runtime/src"));
    module.addIncludePath(b.path("tests"));
    module.addIncludePath(b.path("tests/repro"));
    if (cfg.with_zova) {
        module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "include" }) });
    }
}

const SourceFlavor = enum {
    normal,
    grammar,
    cxx,
    mimalloc,
    sqlite,
    lz4,
    zstd,
};

fn flags(b: *std.Build, cfg: Config, kind: BuildKind, flavor: SourceFlavor) []const []const u8 {
    var list = std.ArrayList([]const u8).empty;
    const a = b.allocator;
    switch (flavor) {
        .normal => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare" }) catch @panic("OOM");
            if (kind == .prod) {
                list.append(a, "-O2") catch @panic("OOM");
                list.append(a, "-DCBM_BIND_TS_ALLOCATOR=1") catch @panic("OOM");
            } else {
                list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
                list.append(a, "-Wno-unused-function") catch @panic("OOM");
            }
        },
        .grammar => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm", "-Iinternal/cbm/vendored/ts_runtime/include", "-Iinternal/cbm/vendored/ts_runtime/src" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .cxx => {
            list.appendSlice(a, &.{ "-std=c++14", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-w", "-Iinternal/cbm/vendored" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .mimalloc => {
            list.appendSlice(a, &.{ "-std=c11", "-w", "-Ivendored/mimalloc/include", "-Ivendored/mimalloc/src" }) catch @panic("OOM");
            if (kind == .prod) {
                list.appendSlice(a, &.{ "-O2", "-DMI_OVERRIDE=1" }) catch @panic("OOM");
            } else {
                list.appendSlice(a, &.{ "-g", "-O1", "-DMI_OVERRIDE=0" }) catch @panic("OOM");
            }
        },
        .sqlite => {
            list.appendSlice(a, &.{ "-std=c11", "-w", "-DSQLITE_DQS=0", "-DSQLITE_THREADSAFE=1", "-DSQLITE_ENABLE_FTS5" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .lz4 => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .zstd => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm/vendored/zstd" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
    }
    list.append(a, b.fmt("-DCBM_WITH_ZOVA={d}", .{@intFromBool(cfg.with_zova)})) catch @panic("OOM");
    if (cfg.target.result.os.tag == .macos) {
        list.append(a, b.fmt("-mmacosx-version-min={s}", .{cfg.target_macos_version})) catch @panic("OOM");
    }
    switch (cfg.sanitize) {
        .none => {},
        .address => list.appendSlice(a, &.{ "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }) catch @panic("OOM"),
        .thread => list.appendSlice(a, &.{ "-fsanitize=thread", "-fno-omit-frame-pointer" }) catch @panic("OOM"),
    }
    return list.toOwnedSlice(a) catch @panic("OOM");
}

fn addNativeSources(
    b: *std.Build,
    module: *std.Build.Module,
    cfg: Config,
    kind: BuildKind,
    include_tests: bool,
    with_ui_assets: bool,
) void {
    module.addCSourceFile(.{ .file = b.path("vendored/mimalloc/src/static.c"), .flags = flags(b, cfg, kind, .mimalloc) });

    if (!cfg.with_zova) {
        module.addCSourceFile(.{ .file = b.path("vendored/sqlite3/sqlite3.c"), .flags = flags(b, cfg, kind, .sqlite) });
    }

    module.addCSourceFiles(.{ .files = grammarSources(b), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/ts_runtime.c"), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/lsp_all.c"), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/preprocessor.cpp"), .flags = flags(b, cfg, kind, .cxx), .language = .cpp });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/lz4/lz4.c"), .flags = flags(b, cfg, kind, .lz4) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/lz4/lz4hc.c"), .flags = flags(b, cfg, kind, .lz4) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/zstd/zstd.c"), .flags = flags(b, cfg, kind, .zstd) });
    module.addAssemblyFile(b.path("vendored/nomic/code_vectors_blob.S"));

    module.addCSourceFiles(.{ .files = &native_sources, .flags = flags(b, cfg, kind, .normal) });
    module.addCSourceFiles(.{ .files = uiSources(with_ui_assets), .flags = flags(b, cfg, kind, .normal) });

    if (cfg.with_zova) {
        module.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "zig-out/lib/libzova_c.a" }) });
        const bridge = addZovaBridgeObject(b, cfg);
        module.addObject(bridge);
    }

    const cli_zig = addCliZigLibrary(b, cfg);
    module.linkLibrary(cli_zig);

    if (include_tests) {
        module.addCSourceFiles(.{ .files = &.{}, .flags = &.{} });
    }
}

fn addCliZigLibrary(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    const module = b.createModule(.{
        .root_source_file = b.path("src/cli_zig/cbm_cli_zig.zig"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
        .link_libc = true,
    });
    const lib = b.addLibrary(.{
        .name = "cbm_cli_zig",
        .linkage = .static,
        .root_module = module,
    });
    return lib;
}

fn addZovaBridgeObject(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    const zova_module = b.createModule(.{
        .root_source_file = .{ .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "src/root.zig" }) },
        .target = cfg.target,
        .optimize = .ReleaseFast,
    });
    zova_module.addIncludePath(b.path("vendored/sqlite3"));
    const bridge_module = b.createModule(.{
        .root_source_file = b.path("src/zova/cbm_zova_bridge.zig"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
        .link_libc = true,
    });
    bridge_module.addImport("zova", zova_module);
    bridge_module.addIncludePath(b.path("src"));
    bridge_module.addIncludePath(b.path("vendored/sqlite3"));
    return b.addObject(.{
        .name = "cbm_zova_bridge",
        .root_module = bridge_module,
    });
}

fn copyArtifact(b: *std.Build, artifact: *std.Build.Step.Compile, dest: []const u8) *std.Build.Step.Run {
    const dir = std.fs.path.dirname(dest).?;
    const mkdir = b.addSystemCommand(&.{ "mkdir", "-p", dir });
    const cp = b.addSystemCommand(&.{"cp"});
    cp.step.dependOn(&mkdir.step);
    cp.addArtifactArg(artifact);
    cp.addArg(dest);
    return cp;
}

fn addRunSuiteStep(
    b: *std.Build,
    name: []const u8,
    desc: []const u8,
    runner: *std.Build.Step.Compile,
    suite: ?[]const u8,
    zova_mode: ?[]const u8,
) *std.Build.Step {
    const run = b.addRunArtifact(runner);
    if (suite) |s| run.addArg(s);
    if (zova_mode) |mode| run.setEnvironmentVariable("CBM_ZOVA_MODE", mode);
    const step = b.step(name, desc);
    step.dependOn(&run.step);
    return step;
}

fn addZovaSuiteStep(
    b: *std.Build,
    cfg: Config,
    runner: *std.Build.Step.Compile,
    name: []const u8,
    mode: []const u8,
    suite: []const u8,
) void {
    const step = b.step(name, "Run focused Zova test suite");
    if (!cfg.with_zova) {
        const fail = b.addFail(b.fmt("{s} requires -Dwith-zova=true", .{name}));
        step.dependOn(&fail.step);
        return;
    }
    const run = b.addRunArtifact(runner);
    run.addArg(suite);
    run.setEnvironmentVariable("CBM_ZOVA_MODE", mode);
    step.dependOn(&run.step);
}

fn addZovaRealRepoStep(
    b: *std.Build,
    cfg: Config,
    cbm_copy: *std.Build.Step.Run,
    test_runner_copy: *std.Build.Step.Run,
) void {
    const step = b.step("test-zova-real-repo", "Run Zova validation against this repository");
    if (!cfg.with_zova) {
        const fail = b.addFail("test-zova-real-repo requires -Dwith-zova=true");
        step.dependOn(&fail.step);
        return;
    }
    const run = b.addSystemCommand(&.{ "bash", "scripts/zova-real-repo-validation.sh" });
    run.step.dependOn(&cbm_copy.step);
    run.step.dependOn(&test_runner_copy.step);
    run.setEnvironmentVariable("CBM_ZOVA_VALIDATION_SKIP_BUILD", "1");
    run.setEnvironmentVariable("ZOVA_ROOT", cfg.zova_root);
    step.dependOn(&run.step);
}

fn addUiStep(b: *std.Build, name: []const u8, desc: []const u8, argv: []const []const u8) *std.Build.Step {
    const run = b.addSystemCommand(argv);
    run.setCwd(b.path("graph-ui"));
    const step = b.step(name, desc);
    step.dependOn(&run.step);
    return step;
}

fn grammarSources(b: *std.Build) []const []const u8 {
    var dir = b.build_root.handle.openDir(b.graph.io, "internal/cbm", .{ .iterate = true }) catch @panic("unable to open internal/cbm");
    defer dir.close(b.graph.io);
    var list = std.ArrayList([]const u8).empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch @panic("unable to iterate internal/cbm")) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.startsWith(u8, entry.name, "grammar_")) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        list.append(b.allocator, b.pathJoin(&.{ "internal/cbm", entry.name })) catch @panic("OOM");
    }
    return list.toOwnedSlice(b.allocator) catch @panic("OOM");
}

fn uiSources(with_assets: bool) []const []const u8 {
    return if (with_assets) &.{
        "src/ui/config.c",
        "src/ui/http_server.c",
        "src/ui/layout3d.c",
        "src/ui/httpd.c",
        "src/ui/embedded_assets.c",
    } else &.{
        "src/ui/config.c",
        "src/ui/http_server.c",
        "src/ui/layout3d.c",
        "src/ui/httpd.c",
        "src/ui/embedded_stub.c",
    };
}

const native_sources = [_][]const u8{
    "src/foundation/arena.c",
    "src/foundation/hash_table.c",
    "src/foundation/str_intern.c",
    "src/foundation/log.c",
    "src/foundation/str_util.c",
    "src/foundation/platform.c",
    "src/foundation/system_info.c",
    "src/foundation/slab_alloc.c",
    "src/foundation/yaml.c",
    "src/foundation/compat.c",
    "src/foundation/compat_thread.c",
    "src/foundation/compat_fs.c",
    "src/foundation/compat_regex.c",
    "src/foundation/mem.c",
    "src/foundation/diagnostics.c",
    "src/foundation/profile.c",
    "src/foundation/dump_verify.c",
    "src/foundation/limits.c",
    "src/foundation/subprocess.c",
    "src/foundation/sha256.c",
    "src/store/store.c",
    "src/zova/cbm_zova.c",
    "src/zova/cbm_zova_publish_model.c",
    "src/zova/cbm_zova_edge_payload.c",
    "src/zova/cbm_zova_delta.c",
    "src/zova/cbm_zova_operations.c",
    "src/zova/cbm_zova_migration.c",
    "src/zova/cbm_zova_legacy_snapshot.c",
    "src/zova/cbm_zova_v5_snapshot.c",
    "src/zova/cbm_zova_writer_gate.c",
    "src/zova/cbm_zova_route.c",
    "src/zova/cbm_zova_repository.c",
    "src/zova/cbm_zova_bridge_stub.c",
    "src/cypher/cypher.c",
    "src/mcp/mcp.c",
    "src/mcp/index_supervisor.c",
    "src/discover/language.c",
    "src/discover/userconfig.c",
    "src/discover/gitignore.c",
    "src/discover/discover.c",
    "src/graph_buffer/graph_buffer.c",
    "src/pipeline/fqn.c",
    "src/pipeline/path_alias.c",
    "src/pipeline/registry.c",
    "src/pipeline/pipeline.c",
    "src/pipeline/pipeline_incremental.c",
    "src/pipeline/worker_pool.c",
    "src/pipeline/pass_parallel.c",
    "src/pipeline/pass_definitions.c",
    "src/pipeline/pass_calls.c",
    "src/pipeline/pass_lsp_cross.c",
    "src/pipeline/pass_usages.c",
    "src/pipeline/pass_semantic.c",
    "src/pipeline/pass_tests.c",
    "src/pipeline/pass_githistory.c",
    "src/pipeline/pass_gitdiff.c",
    "src/pipeline/pass_configures.c",
    "src/pipeline/pass_configlink.c",
    "src/pipeline/pass_route_nodes.c",
    "src/pipeline/pass_enrichment.c",
    "src/pipeline/pass_envscan.c",
    "src/pipeline/pass_compile_commands.c",
    "src/pipeline/pass_infrascan.c",
    "src/pipeline/pass_k8s.c",
    "src/pipeline/pass_similarity.c",
    "src/pipeline/pass_semantic_edges.c",
    "src/pipeline/pass_complexity.c",
    "src/pipeline/pass_cross_repo.c",
    "src/pipeline/artifact.c",
    "src/pipeline/pass_pkgmap.c",
    "src/simhash/minhash.c",
    "src/semantic/semantic.c",
    "src/semantic/ast_profile.c",
    "src/semantic/rotsq.c",
    "src/traces/traces.c",
    "src/watcher/watcher.c",
    "src/git/git_context.c",
    "src/cli/cli.c",
    "src/cli/progress_sink.c",
    "src/cli/hook_augment.c",
    "vendored/yyjson/yyjson.c",
    "internal/cbm/cbm.c",
    "internal/cbm/extract_defs.c",
    "internal/cbm/extract_calls.c",
    "internal/cbm/extract_imports.c",
    "internal/cbm/extract_usages.c",
    "internal/cbm/extract_unified.c",
    "internal/cbm/extract_semantic.c",
    "internal/cbm/extract_type_refs.c",
    "internal/cbm/extract_type_assigns.c",
    "internal/cbm/extract_env_accesses.c",
    "internal/cbm/extract_channels.c",
    "internal/cbm/extract_k8s.c",
    "internal/cbm/helpers.c",
    "internal/cbm/lang_specs.c",
    "internal/cbm/service_patterns.c",
    "internal/cbm/ac.c",
    "internal/cbm/lz4_store.c",
    "internal/cbm/zstd_store.c",
    "internal/cbm/sqlite_writer.c",
};

const all_test_sources = [_][]const u8{
    "tests/test_main.c",
    "tests/test_arena.c",
    "tests/test_hash_table.c",
    "tests/test_dyn_array.c",
    "tests/test_str_intern.c",
    "tests/test_log.c",
    "tests/test_str_util.c",
    "tests/test_platform.c",
    "tests/test_dump_verify.c",
    "tests/test_subprocess.c",
    "tests/test_extraction.c",
    "tests/test_extraction_inheritance.c",
    "tests/test_extraction_imports.c",
    "tests/test_grammar_regression.c",
    "tests/test_grammar_labels.c",
    "tests/test_grammar_imports.c",
    "tests/test_ac.c",
    "tests/test_store_nodes.c",
    "tests/test_store_edges.c",
    "tests/test_store_search.c",
    "tests/test_store_arch.c",
    "tests/test_store_bulk.c",
    "tests/test_store_pragmas.c",
    "tests/test_store_checkpoint.c",
    "tests/test_dump_verify_io.c",
    "tests/test_cypher.c",
    "tests/test_mcp.c",
    "tests/test_language.c",
    "tests/test_userconfig.c",
    "tests/test_gitignore.c",
    "tests/test_git_context.c",
    "tests/test_discover.c",
    "tests/test_graph_buffer.c",
    "tests/test_registry.c",
    "tests/test_pipeline.c",
    "tests/test_fqn.c",
    "tests/test_route_canon.c",
    "tests/test_path_alias.c",
    "tests/test_configlink.c",
    "tests/test_infrascan.c",
    "tests/test_worker_pool.c",
    "tests/test_parallel.c",
    "tests/test_index_resilience.c",
    "tests/test_watcher.c",
    "tests/test_lz4.c",
    "tests/test_zstd.c",
    "tests/test_artifact.c",
    "tests/test_sqlite_writer.c",
    "tests/test_go_lsp.c",
    "tests/test_c_lsp.c",
    "tests/test_php_lsp.c",
    "tests/test_cs_lsp.c",
    "tests/test_cs_lsp_bench.c",
    "tests/test_scope.c",
    "tests/test_type_rep.c",
    "tests/test_py_lsp.c",
    "tests/test_py_lsp_bench.c",
    "tests/test_py_lsp_stress.c",
    "tests/test_py_lsp_scale.c",
    "tests/test_ts_lsp.c",
    "tests/test_java_lsp.c",
    "tests/test_java_lsp_coverage.c",
    "tests/test_kotlin_lsp.c",
    "tests/test_rust_lsp.c",
    "tests/test_traces.c",
    "tests/test_cli.c",
    "tests/test_mem.c",
    "tests/test_ui.c",
    "tests/test_httpd.c",
    "tests/test_security.c",
    "tests/test_yaml.c",
    "tests/test_semantic.c",
    "tests/test_ast_profile.c",
    "tests/test_slab_alloc.c",
    "tests/test_simhash.c",
    "tests/test_stack_overflow.c",
    "tests/test_zova.c",
    "tests/test_zova_operations.c",
    "tests/test_zova_migration.c",
    "tests/test_zova_c_sql_functions.c",
    "tests/test_zova_bridge.c",
    "tests/test_zova_real_repo.c",
    "tests/test_zova_graph_real_repo.c",
    "tests/test_zova_single_file_real_repo.c",
    "tests/test_integration.c",
    "tests/test_incremental.c",
    "tests/test_lang_contract.c",
    "tests/test_edge_imports.c",
    "tests/test_edge_structural.c",
    "tests/test_lsp_resolution_probe.c",
    "tests/test_node_creation_probe.c",
    "tests/test_edge_types_probe.c",
    "tests/test_convergence_probe.c",
    "tests/test_matrix_known_classes.c",
    "tests/test_matrix_new_constructs.c",
    "tests/test_grammar_probe_a.c",
    "tests/test_grammar_probe_b.c",
    "tests/test_grammar_probe_c.c",
    "tests/test_grammar_probe_d.c",
    "tests/test_grammar_probe_e.c",
    "tests/test_grammar_probe_f.c",
    "tests/test_grammar_probe_g.c",
};
