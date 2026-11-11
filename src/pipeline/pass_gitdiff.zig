//! pass_gitdiff.zig — Git diff output parsing helpers.
//!
//! Pure string parsers for git diff --name-status and --unified=0 output.
//! No git execution — just parsing pre-captured output strings.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ChangedFile = c.cbm_changed_file_t;
const ChangedHunk = c.cbm_changed_hunk_t;

const GD_STATUS_IDX = 1;
const GD_PLUS_PREFIX = 6;
const DECIMAL_BASE = c.CBM_DECIMAL_BASE;

pub export fn cbm_parse_range(s: CStr, out_start: *c_int, out_count: *c_int) void {
    out_start.* = 0;
    out_count.* = 1;

    const comma: CStr = c.strchr(s, ',');
    if (comma == null) {
        out_start.* = @truncate(c.strtol(s, null, DECIMAL_BASE));
        return;
    }

    // Parse start
    var buf: [c.CBM_SZ_32]u8 = undefined;
    var len = util.ptrDiff(comma, s);
    if (len >= buf.len) len = buf.len - 1;
    @memcpy(buf[0..len], s[0..len]);
    buf[len] = 0;
    out_start.* = @truncate(c.strtol(&buf, null, DECIMAL_BASE));

    // Parse count
    out_count.* = @truncate(c.strtol(comma + 1, null, DECIMAL_BASE));
}

const HUNK_LINE_BUF = 1536;

/// Parse a single name-status line into a cbm_changed_file_t.
/// Returns true if a valid entry was produced.
fn parseOneNameStatus(line: [*]const u8, line_len_in: usize, out_f: *ChangedFile) bool {
    var tmp: [HUNK_LINE_BUF]u8 = undefined;
    var line_len = line_len_in;
    if (line_len >= tmp.len) line_len = tmp.len - 1;
    @memcpy(tmp[0..line_len], line[0..line_len]);
    tmp[line_len] = 0;

    const status_str: [*]u8 = &tmp;
    const tab1: MutStr = c.strchr(&tmp, '\t');
    if (tab1 == null) return false;
    tab1[0] = 0;
    const path1: MutStr = tab1 + 1;

    const tab2: MutStr = c.strchr(path1, '\t');
    var path2: MutStr = null;
    if (tab2 != null) {
        tab2[0] = 0;
        path2 = tab2 + 1;
    }

    out_f.* = std.mem.zeroes(ChangedFile);

    if (status_str[0] == 'R') {
        out_f.status[0] = 'R';
        out_f.status[GD_STATUS_IDX] = 0;
        _ = c.snprintf(&out_f.old_path, out_f.old_path.len, "%s", path1);
        _ = c.snprintf(&out_f.path, out_f.path.len, "%s", if (path2 != null) path2 else path1);
    } else {
        out_f.status[0] = status_str[0];
        out_f.status[GD_STATUS_IDX] = 0;
        _ = c.snprintf(&out_f.path, out_f.path.len, "%s", path1);
        out_f.old_path[0] = 0;
    }

    return c.cbm_is_trackable_file(&out_f.path);
}

pub export fn cbm_parse_name_status(output: CStr, out: [*c]ChangedFile, max_out: c_int) c_int {
    if (output == null or out == null or max_out <= 0) return 0;

    var count: c_int = 0;
    var line: [*]const u8 = output;

    while (line[0] != 0 and count < max_out) {
        const eol: CStr = c.strchr(line, '\n');
        const line_len: usize = if (eol != null) util.ptrDiff(eol, line) else c.strlen(line);

        if (line_len > 0) {
            var f: ChangedFile = undefined;
            if (parseOneNameStatus(line, line_len, &f)) {
                out[util.ix(count)] = f;
                count += 1;
            }
        }

        line = if (eol != null) eol + 1 else line + line_len;
    }
    return count;
}

/// Parse a single @@ hunk header line and emit a hunk entry if valid.
/// Returns true if a hunk was added.
fn parseHunkLine(line: [*]const u8, line_len: usize, current_file: CStr, out_h: *ChangedHunk) bool {
    const plus: CStr = @ptrCast(c.memchr(line, '+', line_len));
    if (plus == null or @intFromPtr(plus) <= @intFromPtr(line)) return false;

    const end_at: CStr = c.strstr(plus, " @@");
    var range_len: usize = undefined;
    if (end_at != null) {
        range_len = util.ptrDiff(end_at, plus) - 1;
    } else {
        range_len = util.ptrDiff(line + line_len, plus) - 1;
    }

    var range_str: [c.CBM_SZ_64]u8 = undefined;
    if (range_len >= range_str.len) range_len = range_str.len - 1;
    @memcpy(range_str[0..range_len], (plus + 1)[0..range_len]);
    range_str[range_len] = 0;

    var start: c_int = undefined;
    var cnt: c_int = undefined;
    cbm_parse_range(&range_str, &start, &cnt);

    if (start <= 0 or !c.cbm_is_trackable_file(current_file)) return false;

    var end = start +% cnt -% 1;
    if (end < start) end = start;

    _ = c.snprintf(&out_h.path, out_h.path.len, "%s", current_file);
    out_h.start_line = start;
    out_h.end_line = end;
    return true;
}

pub export fn cbm_parse_hunks(output: CStr, out: [*c]ChangedHunk, max_out: c_int) c_int {
    if (output == null or out == null or max_out <= 0) return 0;

    var count: c_int = 0;
    var current_file: [c.CBM_SZ_512]u8 = @splat(0);
    var line: [*]const u8 = output;

    while (line[0] != 0 and count < max_out) {
        const eol: CStr = c.strchr(line, '\n');
        const line_len: usize = if (eol != null) util.ptrDiff(eol, line) else c.strlen(line);

        if (line_len == 0) {
            line = if (eol != null) eol + 1 else line + line_len;
            continue;
        }

        if (line_len > GD_PLUS_PREFIX and c.strncmp(line, "+++ b/", "+++ b/".len) == 0) {
            var flen = line_len - GD_PLUS_PREFIX;
            if (flen >= current_file.len) flen = current_file.len - 1;
            @memcpy(current_file[0..flen], (line + 6)[0..flen]);
            current_file[flen] = 0;
        } else if (line_len >= 2 and line[0] == '@' and line[GD_STATUS_IDX] == '@' and
            current_file[0] != 0)
        {
            var h: ChangedHunk = undefined;
            if (parseHunkLine(line, line_len, &current_file, &h)) {
                out[util.ix(count)] = h;
                count += 1;
            }
        }

        line = if (eol != null) eol + 1 else line + line_len;
    }
    return count;
}
