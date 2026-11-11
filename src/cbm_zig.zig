//! cbm_zig.zig — root of libcbm_zig, the static library holding every Zig
//! module linked into the C build. Each module exports the functions its C
//! header declares; importing it here pulls those exports into the library.
//!
//! Modules that implement a C header declare their exports `pub export fn`,
//! so the comptime check below can compare each one with the header's
//! translated declaration. A signature drift then fails the build instead of
//! linking and breaking at runtime.

const std = @import("std");
const c = @import("c");

comptime {
    _ = @import("cli_zig/cbm_cli_zig.zig");
    checkExports(@import("cli/archive.zig"));
    checkExports(@import("cli/cli.zig"));
    checkExports(@import("cli/commands.zig"));
    checkExports(@import("cli/hook_augment.zig"));
    checkExports(@import("cli/progress_sink.zig"));
    checkExports(@import("cli/store.zig"));
    checkExports(@import("cli/tool_args.zig"));
    checkExports(@import("cypher/parser.zig"));
    checkExports(@import("cypher/cypher.zig"));
    checkExports(@import("discover/discover.zig"));
    checkExports(@import("discover/gitignore.zig"));
    checkExports(@import("discover/language.zig"));
    checkExports(@import("discover/userconfig.zig"));
    checkExports(@import("foundation/arena.zig"));
    checkExports(@import("foundation/compat.zig"));
    checkExports(@import("foundation/compat.zig").windows);
    checkExports(@import("foundation/compat_fs.zig"));
    checkExports(@import("foundation/compat_fs.zig").impl);
    checkExports(@import("foundation/compat_regex.zig"));
    checkExports(@import("foundation/compat_thread.zig"));
    checkExports(@import("foundation/diagnostics.zig"));
    checkExports(@import("foundation/dump_verify.zig"));
    checkExports(@import("foundation/file_lock.zig"));
    checkExports(@import("foundation/git_env.zig"));
    checkExports(@import("foundation/git_env.zig").windows);
    checkExports(@import("foundation/git_env.zig").posix);
    checkExports(@import("foundation/limits.zig"));
    checkExports(@import("foundation/log.zig"));
    checkExports(@import("foundation/mem.zig"));
    checkExports(@import("foundation/platform.zig"));
    checkExports(@import("foundation/profile.zig"));
    checkExports(@import("foundation/sha256.zig"));
    checkExports(@import("foundation/slab_alloc.zig"));
    checkExports(@import("foundation/str_intern.zig"));
    checkExports(@import("foundation/str_util.zig"));
    checkExports(@import("foundation/subprocess.zig"));
    checkExports(@import("foundation/system_info.zig"));
    checkExports(@import("foundation/system_info.zig").linux);
    checkExports(@import("foundation/yaml.zig"));
    checkExports(@import("git/git_context.zig"));
    checkExports(@import("graph_buffer/graph_buffer.zig"));
    checkExports(@import("mcp/code_tools.zig"));
    checkExports(@import("mcp/graph_tools.zig"));
    checkExports(@import("mcp/index_supervisor.zig"));
    checkExports(@import("mcp/index_tools.zig"));
    checkExports(@import("mcp/mcp.zig"));
    checkExports(@import("mcp/project_tools.zig"));
    checkExports(@import("mcp/search_tools.zig"));
    checkExports(@import("mcp/server.zig"));
    checkExports(@import("pipeline/fqn.zig"));
    checkExports(@import("pipeline/laravel_routing.zig"));
    checkExports(@import("pipeline/pass_compile_commands.zig"));
    checkExports(@import("pipeline/pass_complexity.zig"));
    checkExports(@import("pipeline/pass_configlink.zig"));
    checkExports(@import("pipeline/pass_configures.zig"));
    checkExports(@import("pipeline/pass_enrichment.zig"));
    checkExports(@import("pipeline/pass_envscan.zig"));
    checkExports(@import("pipeline/pass_gitdiff.zig"));
    checkExports(@import("pipeline/pass_githistory.zig"));
    checkExports(@import("pipeline/pass_importance.zig"));
    checkExports(@import("pipeline/pass_semantic.zig"));
    checkExports(@import("pipeline/pass_semantic_edges.zig"));
    checkExports(@import("pipeline/pass_similarity.zig"));
    checkExports(@import("pipeline/pass_tests.zig"));
    checkExports(@import("pipeline/path_alias.zig"));
    checkExports(@import("pipeline/registry.zig"));
    checkExports(@import("pipeline/result_spill.zig"));
    checkExports(@import("pipeline/service_bindings.zig"));
    checkExports(@import("pipeline/worker_pool.zig"));
    checkExports(@import("semantic/ast_profile.zig"));
    checkExports(@import("semantic/rotsq.zig"));
    checkExports(@import("semantic/semantic.zig"));
    checkExports(@import("simhash/minhash.zig"));
    checkExports(@import("store/store.zig"));
    checkExports(@import("watcher/watcher.zig"));
    checkExports(@import("zova/cbm_zova.zig"));
    checkExports(@import("zova/cbm_zova.zig").zova_only);
    if (c.CBM_WITH_ZOVA == 0) checkExports(@import("zova/cbm_zova_bridge_stub.zig"));
    checkExports(@import("zova/cbm_zova_coordination.zig"));
    checkExports(@import("zova/cbm_zova_delta.zig"));
    checkExports(@import("zova/cbm_zova_edge_payload.zig"));
    checkExports(@import("zova/cbm_zova_operations.zig"));
    checkExports(@import("zova/cbm_zova_plugin.zig"));
    checkExports(@import("zova/cbm_zova_publish_model.zig"));
    checkExports(@import("zova/cbm_zova_repository.zig"));
    checkExports(@import("zova/cbm_zova_route.zig"));
    checkExports(@import("zova/cbm_zova_v5_snapshot.zig"));
    checkExports(@import("zova/cbm_zova_writer_gate.zig"));
}

/// Checks every pub `cbm_*` function of `module` against the C declaration
/// of the same name in the translated headers.
fn checkExports(comptime module: type) void {
    @setEvalBranchQuota(50000);
    for (@typeInfo(module).@"struct".decl_names) |name| {
        if (!std.mem.startsWith(u8, name, "cbm_")) continue;
        const Impl = @TypeOf(@field(module, name));
        if (@typeInfo(Impl) != .@"fn") continue;
        if (!@hasDecl(c, name)) {
            @compileError(name ++ " is exported from Zig but no C header declares it");
        }
        checkSignature(name, @TypeOf(@field(c, name)), Impl);
    }
}

fn checkSignature(comptime name: []const u8, comptime Header: type, comptime Impl: type) void {
    const h = @typeInfo(Header).@"fn";
    const z = @typeInfo(Impl).@"fn";
    if (h.param_types.len != z.param_types.len or h.attrs.varargs != z.attrs.varargs) {
        @compileError(name ++ ": parameter count differs from its C header");
    }
    for (h.param_types, z.param_types, 0..) |hp, zp, i| {
        if (!sameAbi(hp.?, zp.?)) {
            @compileError(std.fmt.comptimePrint("{s}: parameter {d} is {s} in Zig but {s} in its C header", .{
                name, i, @typeName(zp.?), @typeName(hp.?),
            }));
        }
    }
    if (!sameAbi(h.return_type.?, z.return_type.?)) {
        @compileError(std.fmt.comptimePrint("{s}: returns {s} in Zig but {s} in its C header", .{
            name, @typeName(z.return_type.?), @typeName(h.return_type.?),
        }));
    }
}

/// Same C ABI type. Pointer kind and optionality are ignored (translate-c
/// emits `[*c]T` where Zig code uses `?*T` or `?[*:0]T`), but constness and
/// pointee types must match. A pointer to an opaque C type is a handle whose
/// struct the Zig module defines itself, so any pointee matches it.
fn sameAbi(comptime Header: type, comptime Impl: type) bool {
    const H = unwrapOptional(Header);
    const Z = unwrapOptional(Impl);
    if (H == Z) return true;
    const hp = switch (@typeInfo(H)) {
        .pointer => |p| p,
        else => return false,
    };
    const zp = switch (@typeInfo(Z)) {
        .pointer => |p| p,
        else => return false,
    };
    if (hp.attrs.@"const" != zp.attrs.@"const") return false;
    if (@typeInfo(hp.child) == .@"opaque") return true;
    return sameAbi(hp.child, zp.child);
}

fn unwrapOptional(comptime T: type) type {
    return switch (@typeInfo(T)) {
        .optional => |o| o.child,
        else => T,
    };
}
