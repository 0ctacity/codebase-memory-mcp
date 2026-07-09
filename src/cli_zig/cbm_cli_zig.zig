const std = @import("std");
const c = @cImport({
    @cInclude("stdlib.h");
});

fn cspan(ptr: [*:0]const u8) []const u8 {
    return std.mem.span(ptr);
}

fn appendFmt(
    list: *std.ArrayList(u8),
    allocator: std.mem.Allocator,
    comptime fmt: []const u8,
    args: anytype,
) !void {
    const s = try std.fmt.allocPrint(allocator, fmt, args);
    defer allocator.free(s);
    try list.appendSlice(allocator, s);
}

fn appendLines(
    list: *std.ArrayList(u8),
    allocator: std.mem.Allocator,
    text: []const u8,
    none_label: []const u8,
) !void {
    if (text.len == 0) {
        try appendFmt(list, allocator, "  {s}\n", .{none_label});
        return;
    }

    var wrote = false;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r\n");
        if (line.len == 0) continue;
        try appendFmt(list, allocator, "  {s}\n", .{line});
        wrote = true;
    }
    if (!wrote) {
        try appendFmt(list, allocator, "  {s}\n", .{none_label});
    }
}

fn toCString(list: *std.ArrayList(u8), allocator: std.mem.Allocator) ?[*:0]u8 {
    const out = allocator.allocSentinel(u8, list.items.len, 0) catch return null;
    @memcpy(out[0..list.items.len], list.items);
    return out.ptr;
}

fn yesNo(v: c_int) []const u8 {
    return if (v != 0) "yes" else "no";
}

export fn cbm_cli_zig_doctor_report(
    home_ptr: [*:0]const u8,
    running_binary_ptr: [*:0]const u8,
    installed_binary_ptr: [*:0]const u8,
    cache_dir_ptr: [*:0]const u8,
    project_dbs_ptr: [*:0]const u8,
    config_paths_ptr: [*:0]const u8,
    detected_agents_ptr: [*:0]const u8,
    path_ready: c_int,
    ui_capable: c_int,
    ui_enabled: c_int,
    ui_port: c_int,
) callconv(.c) ?[*:0]u8 {
    const allocator = std.heap.c_allocator;
    var list: std.ArrayList(u8) = .empty;
    defer list.deinit(allocator);

    appendFmt(&list, allocator, "codebase-memory-mcp doctor\n", .{}) catch return null;
    appendFmt(&list, allocator, "home: {s}\n\n", .{cspan(home_ptr)}) catch return null;

    appendFmt(&list, allocator, "binary\n", .{}) catch return null;
    appendFmt(&list, allocator, "  running: {s}\n", .{cspan(running_binary_ptr)}) catch return null;
    appendFmt(&list, allocator, "  installed: {s}\n", .{cspan(installed_binary_ptr)}) catch return null;
    appendFmt(&list, allocator, "  install dir on PATH: {s}\n\n", .{yesNo(path_ready)}) catch return null;

    appendFmt(&list, allocator, "storage\n", .{}) catch return null;
    appendFmt(&list, allocator, "  cache: {s}\n", .{cspan(cache_dir_ptr)}) catch return null;
    appendFmt(&list, allocator, "  project databases:\n", .{}) catch return null;
    appendLines(&list, allocator, cspan(project_dbs_ptr), "(none found)") catch return null;
    appendFmt(&list, allocator, "\n", .{}) catch return null;

    appendFmt(&list, allocator, "agents\n", .{}) catch return null;
    appendFmt(&list, allocator, "  detected: {s}\n\n", .{cspan(detected_agents_ptr)}) catch return null;

    appendFmt(&list, allocator, "config paths\n", .{}) catch return null;
    appendLines(&list, allocator, cspan(config_paths_ptr), "(none)") catch return null;
    appendFmt(&list, allocator, "\n", .{}) catch return null;

    appendFmt(&list, allocator, "ui\n", .{}) catch return null;
    appendFmt(&list, allocator, "  capable: {s}\n", .{yesNo(ui_capable)}) catch return null;
    appendFmt(&list, allocator, "  enabled: {s}\n", .{yesNo(ui_enabled)}) catch return null;
    appendFmt(&list, allocator, "  port: {d}\n", .{ui_port}) catch return null;

    return toCString(&list, allocator);
}

export fn cbm_cli_zig_where_report(
    running_binary_ptr: [*:0]const u8,
    installed_binary_ptr: [*:0]const u8,
    cache_dir_ptr: [*:0]const u8,
    project_dbs_ptr: [*:0]const u8,
    config_paths_ptr: [*:0]const u8,
) callconv(.c) ?[*:0]u8 {
    const allocator = std.heap.c_allocator;
    var list: std.ArrayList(u8) = .empty;
    defer list.deinit(allocator);

    appendFmt(&list, allocator, "cache: {s}\n", .{cspan(cache_dir_ptr)}) catch return null;
    appendFmt(&list, allocator, "running_binary: {s}\n", .{cspan(running_binary_ptr)}) catch return null;
    appendFmt(&list, allocator, "installed_binary: {s}\n", .{cspan(installed_binary_ptr)}) catch return null;
    appendFmt(&list, allocator, "project_databases:\n", .{}) catch return null;
    appendLines(&list, allocator, cspan(project_dbs_ptr), "(none found)") catch return null;
    appendFmt(&list, allocator, "config_files:\n", .{}) catch return null;
    appendLines(&list, allocator, cspan(config_paths_ptr), "(none)") catch return null;

    return toCString(&list, allocator);
}

export fn cbm_cli_zig_install_plan_overview(
    binary_target_ptr: [*:0]const u8,
    shell_rc_ptr: [*:0]const u8,
    planned_paths_ptr: [*:0]const u8,
) callconv(.c) ?[*:0]u8 {
    const allocator = std.heap.c_allocator;
    var list: std.ArrayList(u8) = .empty;
    defer list.deinit(allocator);

    appendFmt(&list, allocator, "binary target: {s}\n", .{cspan(binary_target_ptr)}) catch return null;
    appendFmt(&list, allocator, "shell rc: {s}\n", .{cspan(shell_rc_ptr)}) catch return null;
    appendFmt(&list, allocator, "planned files:\n", .{}) catch return null;
    appendLines(&list, allocator, cspan(planned_paths_ptr), "(none)") catch return null;

    return toCString(&list, allocator);
}

export fn cbm_cli_zig_free(ptr: ?[*:0]u8) callconv(.c) void {
    if (ptr) |p| {
        c.free(p);
    }
}
