//! compat_regex.zig — Portable regular expression implementation.
//!
//! Implements src/foundation/compat_regex.h.
//! POSIX: direct wrappers around <regex.h>.
//! Windows: vendored TRE regex library (BSD-licensed).

const builtin = @import("builtin");
const c = @import("c");

const is_windows = builtin.os.tag == .windows;

const regcomp = if (is_windows) c.tre_regcomp else c.regcomp;
const regexec = if (is_windows) c.tre_regexec else c.regexec;
const regfree = if (is_windows) c.tre_regfree else c.regfree;

const MAX_MATCHES = 32;

comptime {
    // Our opaque buffer must be large enough for the platform's regex_t.
    // If this fires, increase cbm_regex_t.opaque size. glibc's regex_t has
    // bitfields, so translate-c gives an opaque type with no size here; the
    // same check in c_shim.c covers every platform.
    if (@typeInfo(c.regex_t) != .@"opaque" and @sizeOf(c.regex_t) > @sizeOf(c.cbm_regex_t)) {
        @compileError("cbm_regex_t opaque buffer too small for regex_t");
    }
}

/// The regex_t stored in the opaque buffer. cbm_regex_t is a char array, so
/// its alignment is 1; like the C cast, this does not check alignment.
fn regexOf(r: *const c.cbm_regex_t) *c.regex_t {
    @setRuntimeSafety(false);
    return @ptrCast(@alignCast(@constCast(&r.@"opaque")));
}

fn translateFlags(flags: c_int) c_int {
    var out: c_int = 0;
    if (flags & c.CBM_REG_EXTENDED != 0) out |= c.REG_EXTENDED;
    if (flags & c.CBM_REG_ICASE != 0) out |= c.REG_ICASE;
    if (flags & c.CBM_REG_NOSUB != 0) out |= c.REG_NOSUB;
    if (flags & c.CBM_REG_NEWLINE != 0) out |= c.REG_NEWLINE;
    if (!is_windows and @hasDecl(c, "REG_ENHANCED")) {
        if (flags & c.CBM_REG_ENHANCED != 0) out |= c.REG_ENHANCED;
    }
    return out;
}

pub export fn cbm_regcomp(r: *c.cbm_regex_t, pattern: ?[*:0]const u8, flags: c_int) c_int {
    const rc = regcomp(regexOf(r), pattern, translateFlags(flags));
    return if (rc == 0) c.CBM_REG_OK else rc;
}

pub export fn cbm_regexec(r: *const c.cbm_regex_t, str: ?[*:0]const u8, nmatch: c_int, matches: ?[*]c.cbm_regmatch_t, eflags: c_int) c_int {
    const re = regexOf(r);
    const out = matches orelse {
        const rc = regexec(re, str, 0, null, eflags);
        return if (rc == 0) c.CBM_REG_OK else c.CBM_REG_NOMATCH;
    };
    if (nmatch <= 0) {
        const rc = regexec(re, str, 0, null, eflags);
        return if (rc == 0) c.CBM_REG_OK else c.CBM_REG_NOMATCH;
    }

    // Map through the platform regmatch_t
    var pmatch: [MAX_MATCHES]c.regmatch_t = undefined;
    const n: usize = @intCast(@min(nmatch, MAX_MATCHES));
    const rc = regexec(re, str, n, &pmatch, eflags);
    if (rc != 0) return c.CBM_REG_NOMATCH;
    for (out[0..n], pmatch[0..n]) |*dst, src| {
        dst.rm_so = @truncate(src.rm_so);
        dst.rm_eo = @truncate(src.rm_eo);
    }
    return c.CBM_REG_OK;
}

pub export fn cbm_regfree(r: *c.cbm_regex_t) void {
    regfree(regexOf(r));
}
