//! cbm_zova_bridge_stub.zig — src/zova/cbm_zova_bridge.h for builds without
//! Zova. cbm_zig.zig imports this file only when CBM_WITH_ZOVA is 0; with
//! Zova, cbm_zova_bridge.zig provides the same symbols.

pub export fn cbm_zova_bridge_build_enabled() c_int {
    return 0;
}

pub export fn cbm_zova_bridge_install_extension(zova_path: ?[*:0]const u8) c_int {
    _ = zova_path;
    return -1;
}

pub export fn cbm_zova_bridge_cosine_smoke(zova_path: ?[*:0]const u8, a: ?[*]const i8, b: ?[*]const i8, len: usize, out_score: ?*f64) c_int {
    _ = zova_path;
    _ = a;
    _ = b;
    _ = len;
    _ = out_score;
    return -1;
}

pub export fn cbm_zova_bridge_camel_smoke(zova_path: ?[*:0]const u8, input: ?[*:0]const u8, out: ?[*]u8, out_len: usize) c_int {
    _ = zova_path;
    _ = input;
    _ = out;
    _ = out_len;
    return -1;
}

pub export fn cbm_zova_bridge_regex_smoke(zova_path: ?[*:0]const u8, pattern: ?[*:0]const u8, text: ?[*:0]const u8, case_insensitive: c_int, out_match: ?*c_int) c_int {
    _ = zova_path;
    _ = pattern;
    _ = text;
    _ = case_insensitive;
    _ = out_match;
    return -1;
}

pub export fn cbm_zova_bridge_invalid_regex_smoke(zova_path: ?[*:0]const u8, out_error: ?[*]u8, out_error_len: usize) c_int {
    _ = zova_path;
    _ = out_error;
    _ = out_error_len;
    return -1;
}
