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
    checkExports(@import("git/git_context.zig"));
    checkExports(@import("mcp/code_tools.zig"));
    checkExports(@import("mcp/graph_tools.zig"));
    checkExports(@import("mcp/index_supervisor.zig"));
    checkExports(@import("mcp/index_tools.zig"));
    checkExports(@import("mcp/mcp.zig"));
    checkExports(@import("mcp/project_tools.zig"));
    checkExports(@import("mcp/search_tools.zig"));
    checkExports(@import("mcp/server.zig"));
    checkExports(@import("simhash/minhash.zig"));
    checkExports(@import("watcher/watcher.zig"));
}

/// Checks every pub `cbm_*` function of `module` against the C declaration
/// of the same name in the translated headers.
fn checkExports(comptime module: type) void {
    @setEvalBranchQuota(10000);
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
