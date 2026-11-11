//! pass_envscan.zig — Environment URL scanner.
//!
//! Walks a project directory, scans config files (Dockerfile, .env, shell,
//! YAML, TOML, Terraform, .properties) for environment variable assignments
//! where the value is a URL. Filters out secrets.
//!
//! Port of internal/pipeline/envscan.go:ScanProjectEnvURLs().

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Regex = c.cbm_regex_t;
const Regmatch = c.cbm_regmatch_t;
const EnvBinding = c.cbm_env_binding_t;

const ENV_REGEX_MAX = 5;
const ENV_EXT_LEN = 4; // strlen(".env")

// ── Regex patterns (compiled lazily) ────────────────────────────────

var dockerfile_re: Regex = undefined; // ENV|ARG KEY=VALUE or KEY VALUE
var yaml_kv_re: Regex = undefined; // key: "https://..."
var yaml_setenv_re: Regex = undefined; // --set-env-vars KEY=VALUE
var terraform_re: Regex = undefined; // default|value = "https://..."
var shell_re: Regex = undefined; // [export] KEY=https://...
var envfile_re: Regex = undefined; // KEY=https://...
var toml_re: Regex = undefined; // key = "https://..."
var properties_re: Regex = undefined; // key=https://...
var patterns_once: util.Once = .{};

// POSIX ERE doesn't support \w or \S — use bracket expressions
const W = "[A-Za-z0-9_]"; // word char
const NW = "[^ \t\"']"; // non-whitespace, non-quote

/// Compiled once per process; concurrent pipelines then only read them.
fn compilePatternsOnce() void {
    const ext = c.CBM_REG_EXTENDED;
    _ = c.cbm_regcomp(&dockerfile_re, "^(ENV|ARG)[[:space:]]+(" ++ W ++ "+)[= ](.*)", ext);
    _ = c.cbm_regcomp(&yaml_kv_re, "(" ++ W ++ "+):[[:space:]]*[\"']?(https?://" ++ NW ++ "+)", ext);
    _ = c.cbm_regcomp(&yaml_setenv_re, "--set-env-vars[[:space:]]+(" ++ W ++ "+)=([^ \t]+)", ext);
    _ = c.cbm_regcomp(&terraform_re, "(default|value)[[:space:]]*=[[:space:]]*\"(https?://[^\"]+)\"", ext);
    _ = c.cbm_regcomp(&shell_re, "(export[[:space:]]+)?(" ++ W ++ "+)=[\"']?(https?://" ++ NW ++ "+)", ext);
    _ = c.cbm_regcomp(&envfile_re, "^(" ++ W ++ "+)=(https?://[^ \t]+)", ext);
    _ = c.cbm_regcomp(&toml_re, "(" ++ W ++ "+)[[:space:]]*=[[:space:]]*\"(https?://[^\"]+)\"", ext);
    _ = c.cbm_regcomp(&properties_re, "(" ++ W ++ "+)[[:space:]]*=[[:space:]]*(https?://[^ \t]+)", ext);
}

fn compilePatterns() void {
    patterns_once.call(compilePatternsOnce);
}

// ── File type detection ─────────────────────────────────────────────

/// name lowercased into buf (with its NUL); null when it does not fit.
fn lowerName(name: CStr, buf: *[c.CBM_SZ_256]u8) ?[:0]const u8 {
    const len = c.strlen(name);
    if (len >= buf.len) return null;
    for (0..len + 1) |i| buf[i] = std.ascii.toLower(name[i]);
    return buf[0..len :0];
}

const DOCKERFILE_SUFFIX_LEN = 11; // strlen("dockerfile.") == strlen(".dockerfile")

fn isDockerfileName(name: CStr) bool {
    // Case-insensitive check
    var buf: [c.CBM_SZ_256]u8 = undefined;
    const lower = lowerName(name, &buf) orelse return false;

    if (streq(lower, "dockerfile")) return true;
    if (c.strncmp(lower, "dockerfile.", DOCKERFILE_SUFFIX_LEN) == 0) return true;
    if (lower.len > DOCKERFILE_SUFFIX_LEN and
        streq(lower.ptr + lower.len - DOCKERFILE_SUFFIX_LEN, ".dockerfile"))
    {
        return true;
    }
    return false;
}

fn isEnvFileName(name: CStr) bool {
    var buf: [c.CBM_SZ_256]u8 = undefined;
    const lower = lowerName(name, &buf) orelse return false;

    if (streq(lower, ".env")) return true;
    if (c.strncmp(lower, ".env.", ".env.".len) == 0) return true;
    if (lower.len > ENV_EXT_LEN and streq(lower.ptr + lower.len - ENV_EXT_LEN, ".env")) return true;
    return false;
}

fn isSecretFile(name: CStr) bool {
    var buf: [c.CBM_SZ_256]u8 = undefined;
    const lower = lowerName(name, &buf) orelse return false;

    const patterns = [_][*:0]const u8{
        "service_account", "credentials", "key.json", "key.pem", "id_rsa",
        "id_ed25519",      ".pem",        ".key",
    };
    for (patterns) |pattern| {
        if (c.strstr(lower, pattern) != null) return true;
    }
    return false;
}

// ── Ignored directories ─────────────────────────────────────────────

fn isIgnoredDir(name: CStr) bool {
    const dirs = [_][*:0]const u8{
        ".git",  "node_modules", ".svn", ".hg",   "__pycache__", "vendor", ".terraform", ".cache",
        ".idea", ".vscode",      "dist", "build", ".next",       ".nuxt",  "target",
    };
    for (dirs) |dir| {
        if (streq(name, dir)) return true;
    }
    return false;
}

// ── File type enum ──────────────────────────────────────────────────

const FileType = enum {
    unknown,
    dockerfile,
    yaml,
    terraform,
    shell,
    envfile,
    toml,
    properties,
};

fn detectFileType(name: CStr) FileType {
    if (isDockerfileName(name)) return .dockerfile;
    if (isEnvFileName(name)) return .envfile;

    const ext: CStr = c.strrchr(name, '.');
    if (ext == null) return .unknown;

    if (streq(ext, ".yaml") or streq(ext, ".yml")) return .yaml;
    if (streq(ext, ".tf") or streq(ext, ".hcl")) return .terraform;
    if (streq(ext, ".sh") or streq(ext, ".bash") or streq(ext, ".zsh")) return .shell;
    if (streq(ext, ".toml")) return .toml;
    if (streq(ext, ".properties") or streq(ext, ".cfg") or streq(ext, ".ini")) return .properties;

    return .unknown;
}

// ── Line scanner ────────────────────────────────────────────────────

/// Extract key/value from a regex match with two capture groups.
/// Returns true on success, false if groups are empty or too large.
fn extractKvGroups(
    trimmed: [*:0]const u8,
    m: *const [ENV_REGEX_MAX]Regmatch,
    key_grp: usize,
    val_grp: usize,
    key_out: []u8,
    val_out: []u8,
) bool {
    const klen: c_int = @intCast(m[key_grp].rm_eo - m[key_grp].rm_so);
    const vlen: c_int = @intCast(m[val_grp].rm_eo - m[val_grp].rm_so);
    if (klen <= 0 or klen >= key_out.len or vlen <= 0 or vlen >= val_out.len) return false;
    const key_start: usize = @intCast(m[key_grp].rm_so);
    const val_start: usize = @intCast(m[val_grp].rm_so);
    @memcpy(key_out[0..ix(klen)], trimmed[key_start..][0..ix(klen)]);
    key_out[ix(klen)] = 0;
    @memcpy(val_out[0..ix(vlen)], trimmed[val_start..][0..ix(vlen)]);
    val_out[ix(vlen)] = 0;
    return true;
}

/// Try to scan a Dockerfile line.
fn scanDockerfileLine(line: [*:0]const u8, key: []u8, val: []u8) bool {
    var m: [ENV_REGEX_MAX]Regmatch = undefined;
    if (c.cbm_regexec(&dockerfile_re, line, 4, &m, 0) != 0) return false;
    if (!extractKvGroups(line, &m, 2, 3, key, val)) return false;
    var vl = c.strlen(val.ptr);
    while (vl > 0 and (val[vl - 1] == '"' or val[vl - 1] == '\'')) {
        vl -= 1;
        val[vl] = 0;
    }
    return true;
}

/// Try to scan a YAML line.
fn scanYamlLine(line: [*:0]const u8, key: []u8, val: []u8) bool {
    var m: [ENV_REGEX_MAX]Regmatch = undefined;
    if (c.cbm_regexec(&yaml_kv_re, line, 3, &m, 0) == 0 and extractKvGroups(line, &m, 1, 2, key, val)) {
        return true;
    }
    if (c.cbm_regexec(&yaml_setenv_re, line, 3, &m, 0) == 0 and extractKvGroups(line, &m, 1, 2, key, val)) {
        return true;
    }
    return false;
}

/// Try to scan a Terraform line.
fn scanTerraformLine(line: [*:0]const u8, key: []u8, val: []u8) bool {
    var m: [ENV_REGEX_MAX]Regmatch = undefined;
    if (c.cbm_regexec(&terraform_re, line, 3, &m, 0) != 0) return false;
    const vlen: c_int = @intCast(m[2].rm_eo - m[2].rm_so);
    if (vlen <= 0 or vlen >= val.len) return false;
    _ = c.strncpy(key.ptr, "_tf_default", key.len - 1);
    key[key.len - 1] = 0;
    const val_start: usize = @intCast(m[2].rm_so);
    @memcpy(val[0..ix(vlen)], line[val_start..][0..ix(vlen)]);
    val[ix(vlen)] = 0;
    return true;
}

/// Try single-regex scan (shell, envfile, toml, properties).
fn scanRegexLine(re: *Regex, line: [*:0]const u8, kg: usize, vg: usize, key: []u8, val: []u8) bool {
    var m: [ENV_REGEX_MAX]Regmatch = undefined;
    return c.cbm_regexec(re, line, 5, &m, 0) == 0 and extractKvGroups(line, &m, kg, vg, key, val);
}

fn scanLine(line: [*:0]const u8, ft: FileType, key_out: []u8, val_out: []u8) bool {
    var trimmed = line;
    while (trimmed[0] == ' ' or trimmed[0] == '\t') trimmed += 1;
    if (trimmed[0] == '#' or (trimmed[0] == '/' and trimmed[1] == '/')) return false;

    return switch (ft) {
        .dockerfile => scanDockerfileLine(trimmed, key_out, val_out),
        .yaml => scanYamlLine(trimmed, key_out, val_out),
        .terraform => scanTerraformLine(trimmed, key_out, val_out),
        .shell => scanRegexLine(&shell_re, trimmed, 2, 3, key_out, val_out),
        .envfile => scanRegexLine(&envfile_re, trimmed, 1, 2, key_out, val_out),
        .toml => scanRegexLine(&toml_re, trimmed, 1, 2, key_out, val_out),
        .properties => scanRegexLine(&properties_re, trimmed, 1, 2, key_out, val_out),
        .unknown => false,
    };
}

// ── Public API ──────────────────────────────────────────────────────

/// strncpy into a fixed buffer, always NUL-terminated.
fn copyField(dst: []u8, src: CStr) void {
    _ = c.strncpy(dst.ptr, src, dst.len - 1);
    dst[dst.len - 1] = 0;
}

/// Scan a single file for env URL bindings. Returns number of bindings added.
fn scanEnvFile(full_path: CStr, rel: CStr, ft: FileType, out: [*]EnvBinding, max_out: c_int) c_int {
    const f = c.cbm_fopen(full_path, "r") orelse return 0;

    var fst: c.struct_stat = undefined;
    if (c.fstat(c.fileno(f), &fst) != 0 or fst.st_size > c.CBM_SZ_1K * c.CBM_SZ_1K) {
        _ = c.fclose(f);
        return 0;
    }

    var count: c_int = 0;
    var line: [c.CBM_SZ_2K:0]u8 = undefined;
    while (count < max_out and c.fgets(&line, c.CBM_SZ_2K, f) != null) {
        var ll = c.strlen(&line);
        while (ll > 0 and (line[ll - 1] == '\n' or line[ll - 1] == '\r')) {
            ll -= 1;
            line[ll] = 0;
        }

        var key: [c.CBM_SZ_128]u8 = undefined;
        var value: [c.CBM_SZ_512]u8 = undefined;
        if (!scanLine(&line, ft, &key, &value)) continue;
        if (c.strncmp(&value, "http://", "http://".len) != 0 and
            c.strncmp(&value, "https://", "https://".len) != 0)
        {
            continue;
        }
        if (c.cbm_is_secret_binding(&key, &value) or c.cbm_is_secret_value(&value)) continue;

        const binding = &out[ix(count)];
        copyField(&binding.key, &key);
        copyField(&binding.value, &value);
        copyField(&binding.file_path, rel);
        count += 1;
    }
    _ = c.fclose(f);
    return count;
}

const PathStack = [c.CBM_SZ_256][c.CBM_SZ_512]u8;

/// Process a single directory entry for env scanning. Returns bindings added.
fn processEnvEntry(
    ent: *c.cbm_dirent_t,
    dir_path: CStr,
    root_path: CStr,
    out: [*]EnvBinding,
    max_out: c_int,
    path_stack: *PathStack,
    stack_top: *usize,
    excluded_dirs: [*c]MutStr,
    excluded_count: c_int,
) c_int {
    var full_path: [c.CBM_SZ_512]u8 = undefined;
    const name: CStr = &ent.name;
    _ = c.snprintf(&full_path, full_path.len, "%s/%s", dir_path, name);
    var rel: [*]const u8 = @as([*]const u8, &full_path) + c.strlen(root_path);
    while (rel[0] == '/') rel += 1;

    var st: c.struct_stat = undefined;
    if (c.stat(&full_path, &st) != 0) return 0;
    if (util.isDirMode(&st)) {
        if (!isIgnoredDir(name) and
            c.cbm_pipeline_relpath_is_excluded(rel, excluded_dirs, excluded_count) == 0 and
            stack_top.* < c.CBM_SZ_256)
        {
            copyField(&path_stack[stack_top.*], &full_path);
            stack_top.* += 1;
        }
        return 0;
    }
    if (isSecretFile(name)) return 0;
    const ft = detectFileType(name);
    if (ft == .unknown) return 0;
    return scanEnvFile(&full_path, rel, ft, out, max_out);
}

pub export fn cbm_scan_project_env_urls_excluded(
    root_path: CStr,
    out: [*c]EnvBinding,
    max_out: c_int,
    excluded_dirs: [*c]MutStr,
    excluded_count: c_int,
) c_int {
    if (root_path == null or out == null or max_out <= 0) return 0;
    compilePatterns();

    var count: c_int = 0;
    var path_stack: PathStack = undefined;
    var stack_top: usize = 1;
    copyField(&path_stack[0], root_path);

    while (stack_top > 0 and count < max_out) {
        stack_top -= 1;
        var dir_path: [c.CBM_SZ_512]u8 = undefined;
        copyField(&dir_path, &path_stack[stack_top]);

        const d = c.cbm_opendir(&dir_path) orelse continue;
        while (true) {
            const ent = c.cbm_readdir(d) orelse break;
            if (count >= max_out) break;
            count += processEnvEntry(ent, &dir_path, root_path, @as([*]EnvBinding, out) + ix(count), max_out - count, &path_stack, &stack_top, excluded_dirs, excluded_count);
        }
        _ = c.cbm_closedir(d);
    }
    return count;
}

pub export fn cbm_scan_project_env_urls(root_path: CStr, out: [*c]EnvBinding, max_out: c_int) c_int {
    return cbm_scan_project_env_urls_excluded(root_path, out, max_out, null, 0);
}
