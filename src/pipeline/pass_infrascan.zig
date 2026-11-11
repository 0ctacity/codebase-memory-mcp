//! pass_infrascan.zig — Infrastructure file detection and parsing helpers.
//!
//! Pure helper functions for detecting infrastructure file types,
//! secret filtering, and parsing Dockerfiles, .env files, shell scripts,
//! and Terraform files.

const std = @import("std");
const c = @import("c");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const Str = [*:0]const u8;
const DockerfileResult = c.cbm_dockerfile_result_t;
const DotenvResult = c.cbm_dotenv_result_t;
const ShellResult = c.cbm_shell_result_t;
const TerraformResult = c.cbm_terraform_result_t;

const IS_ENV_EXT = 4; // strlen(".env")
const IS_CONTENT_LIMIT = 4096;
const IS_MAX_STAGES = 16;
const IS_MAX_PORTS = 16;
const IS_MAX_SOURCES = 16;
const IS_TOKEN_MIN_1 = 16; // min alnum for AWS key
const IS_TOKEN_MIN_2 = 20; // min alnum for GH classic token
const IS_NOT_FOUND = -1;

/// String length constants for keyword matching
const LEN_DOCKERFILE = 11; // strlen("dockerfile.") == strlen(".dockerfile")
const LEN_DOCKER_COMPOSE = 14; // strlen("docker-compose")
const LEN_HEALTHCHECK = 11; // strlen("HEALTHCHECK")

/// Minimum alnum chars after prefix for secret detection
const GITHUB_PAT_MIN_ALNUM = 36; // ghp_ + 36 alnum chars

// ── Internal helpers ────────────────────────────────────────────────

fn toLower(src: CStr, dst: []u8) void {
    var len = c.strlen(src);
    if (len >= dst.len) len = dst.len - 1;
    for (0..len) |i| dst[i] = std.ascii.toLower(src[i]);
    dst[len] = 0;
}

/// strncmp(s, prefix, strlen(prefix)) == 0.
fn hasPrefix(s: [*]const u8, comptime prefix: [:0]const u8) bool {
    return c.strncmp(s, prefix, prefix.len) == 0;
}

/// strncasecmp(s, prefix, strlen(prefix)) == 0.
fn hasPrefixCi(s: [*]const u8, prefix: []const u8) bool {
    for (prefix, 0..) |ch, i| {
        if (std.ascii.toLower(s[i]) != std.ascii.toLower(ch)) return false;
    }
    return true;
}

fn isBlank(ch: u8) bool {
    return ch == ' ' or ch == '\t';
}

/// Case-insensitive substring search
fn ciStrstr(haystack_in: [*:0]const u8, needle: []const u8) ?[*:0]const u8 {
    if (needle.len == 0) return haystack_in;
    var haystack = haystack_in;
    while (haystack[0] != 0) : (haystack += 1) {
        if (hasPrefixCi(haystack, needle)) return haystack;
    }
    return null;
}

/// Count consecutive alphanumeric characters
fn countAlnum(s: [*]const u8) c_int {
    var n: usize = 0;
    while (std.ascii.isAlphanumeric(s[n])) n += 1;
    return @intCast(n);
}

/// Count consecutive alphanumeric or dash characters
fn countAlnumDash(s: [*]const u8) c_int {
    var n: usize = 0;
    while (std.ascii.isAlphanumeric(s[n]) or s[n] == '-') n += 1;
    return @intCast(n);
}

/// Skip whitespace, return pointer past it
fn skipWs(p_in: [*:0]const u8) [*:0]const u8 {
    var p = p_in;
    while (isBlank(p[0])) p += 1;
    return p;
}

/// Extract a word ([a-zA-Z0-9_]+) into buf. Returns length.
fn extractWord(p: [*]const u8, buf: []u8) usize {
    var n: usize = 0;
    while ((std.ascii.isAlphanumeric(p[n]) or p[n] == '_') and n < buf.len - 1) {
        buf[n] = p[n];
        n += 1;
    }
    buf[n] = 0;
    return n;
}

/// Extract a non-space token into buf. Returns length.
fn extractToken(p: [*]const u8, buf: []u8) usize {
    var n: usize = 0;
    while (p[n] != 0 and !isBlank(p[n]) and n < buf.len - 1) {
        buf[n] = p[n];
        n += 1;
    }
    buf[n] = 0;
    return n;
}

/// Trim trailing whitespace in-place
fn rtrim(s: [*:0]u8) void {
    var len = c.strlen(s);
    while (len > 0 and (s[len - 1] == ' ' or s[len - 1] == '\t' or
        s[len - 1] == '\r' or s[len - 1] == '\n'))
    {
        len -= 1;
        s[len] = 0;
    }
}

/// snprintf(dst, sizeof(dst), "%s", src) for a fixed char array.
fn copyStr(dst: []u8, src: CStr) void {
    _ = c.snprintf(dst.ptr, dst.len, "%s", src);
}

// ── File identification ─────────────────────────────────────────────

pub export fn cbm_is_dockerfile(name: CStr) bool {
    if (name == null) return false;
    var lower: [c.CBM_SZ_256:0]u8 = undefined;
    toLower(name, &lower);

    if (streq(&lower, "dockerfile")) return true;
    if (c.strncmp(&lower, "dockerfile.", LEN_DOCKERFILE) == 0) return true;
    const len = c.strlen(&lower);
    if (len > LEN_DOCKERFILE and streq(@as(CStr, &lower) + len - LEN_DOCKERFILE, ".dockerfile")) {
        return true;
    }
    return false;
}

/// True when the lowercased name ends in .yml or .yaml.
fn hasYamlExt(lower: CStr) bool {
    const ext: CStr = c.strrchr(lower, '.');
    if (ext == null) return false;
    return streq(ext, ".yml") or streq(ext, ".yaml");
}

pub export fn cbm_is_compose_file(name: CStr) bool {
    if (name == null) return false;
    var lower: [c.CBM_SZ_256:0]u8 = undefined;
    toLower(name, &lower);

    const prefix_match = c.strncmp(&lower, "docker-compose", LEN_DOCKER_COMPOSE) == 0 or
        streq(&lower, "compose.yml") or streq(&lower, "compose.yaml");
    if (!prefix_match) return false;

    return hasYamlExt(&lower);
}

pub export fn cbm_is_cloudbuild_file(name: CStr) bool {
    if (name == null) return false;
    var lower: [c.CBM_SZ_256:0]u8 = undefined;
    toLower(name, &lower);

    if (!hasPrefix(&lower, "cloudbuild")) return false;
    return hasYamlExt(&lower);
}

pub export fn cbm_is_env_file(name: CStr) bool {
    if (name == null) return false;
    var lower: [c.CBM_SZ_256:0]u8 = undefined;
    toLower(name, &lower);

    if (streq(&lower, ".env")) return true;
    if (hasPrefix(&lower, ".env.")) return true;
    const len = c.strlen(&lower);
    if (len > IS_ENV_EXT and streq(@as(CStr, &lower) + len - IS_ENV_EXT, ".env")) return true;
    return false;
}

pub export fn cbm_is_kustomize_file(name: CStr) bool {
    if (name == null) return false;
    var lower: [c.CBM_SZ_256:0]u8 = undefined;
    toLower(name, &lower);
    if (streq(&lower, "kustomization.yaml")) return true;
    return streq(&lower, "kustomization.yml");
}

pub export fn cbm_is_k8s_manifest(name: CStr, content: CStr) bool {
    if (name == null or content == null or cbm_is_kustomize_file(name)) return false;
    var buf: [IS_CONTENT_LIMIT:0]u8 = undefined;
    // strnlen(content, IS_CONTENT_LIMIT)
    var n: usize = 0;
    while (n < IS_CONTENT_LIMIT and content[n] != 0) n += 1;
    @memcpy(buf[0..n], content[0..n]);
    buf[n] = 0;
    return ciStrstr(&buf, "apiVersion:") != null;
}

pub export fn cbm_is_shell_script(name: CStr, ext: CStr) bool {
    _ = name;
    if (ext == null) return false;
    return streq(ext, ".sh") or streq(ext, ".bash") or streq(ext, ".zsh");
}

// ── Secret detection ────────────────────────────────────────────────

fn keyIsSecret(key: Str) bool {
    const patterns = [_][]const u8{
        "secret",          "password",    "passwd",         "token",      "api_key",
        "apikey",          "private_key", "credential",     "auth_token", "access_key",
        "client_secret",   "signing_key", "encryption_key", "ssh_key",    "deploy_key",
        "service_account", "bearer",      "jwt_secret",
    };
    for (patterns) |pattern| {
        if (ciStrstr(key, pattern) != null) return true;
    }
    return false;
}

pub export fn cbm_is_secret_value(value_c: ?Str) bool {
    const value = value_c orelse return false;
    if (value[0] == 0) return false;

    // -----BEGIN (PEM key)
    if (ciStrstr(value, "-----BEGIN") != null) return true;

    // Substring searches — Go uses regex MatchString which finds anywhere

    // AKIA + 16 alnum (AWS key)
    if (ciStrstr(value, "AKIA")) |p| {
        if (countAlnum(p + 4) >= IS_TOKEN_MIN_1) return true;
    }

    // sk- + 20 alnum (API key)
    if (ciStrstr(value, "sk-")) |p| {
        if (countAlnum(p + 3) >= IS_TOKEN_MIN_2) return true;
    }

    // ghp_ + 36 alnum (GitHub PAT)
    if (ciStrstr(value, "ghp_")) |p| {
        if (countAlnum(p + 4) >= GITHUB_PAT_MIN_ALNUM) return true;
    }

    // glpat- + 20 alnum/dash (GitLab PAT)
    if (ciStrstr(value, "glpat-")) |p| {
        if (countAlnumDash(p + 6) >= IS_TOKEN_MIN_2) return true;
    }

    // xox[bps]- (Slack token)
    if (ciStrstr(value, "xox")) |p| {
        if (p[3] != 0) {
            const kind = std.ascii.toLower(p[3]);
            if ((kind == 'b' or kind == 'p' or kind == 's') and p[4] == '-' and
                countAlnumDash(p + 5) >= 1)
            {
                return true;
            }
        }
    }

    return false;
}

pub export fn cbm_is_secret_binding(key: ?Str, value: ?Str) bool {
    if (key) |k| {
        if (keyIsSecret(k)) return true;
    }
    if (value != null and cbm_is_secret_value(value)) return true;
    return false;
}

// ── Clean JSON brackets ─────────────────────────────────────────────

/// Strip JSON array brackets, quotes, and normalize whitespace.
fn cleanJsonArrayInner(s: [*]const u8, len: usize, out: [*]u8, out_sz: usize) void {
    var pos: usize = 0;
    var in_space = false;
    var i: usize = 1;
    while (i < len - 1 and pos < out_sz - 1) : (i += 1) {
        var ch = s[i];
        if (ch == '"') continue;
        if (ch == ',') ch = ' ';
        if (ch == ' ' or ch == '\t') {
            if (!in_space and pos > 0) {
                out[pos] = ' ';
                pos += 1;
                in_space = true;
            }
        } else {
            out[pos] = ch;
            pos += 1;
            in_space = false;
        }
    }
    while (pos > 0 and out[pos - 1] == ' ') pos -= 1;
    out[pos] = 0;
}

pub export fn cbm_clean_json_brackets(s: CStr, out: MutStr, out_sz: usize) void {
    if (s == null or out == null or out_sz == 0) return;

    const len = c.strlen(s);
    if (len >= 2 and s[0] == '[' and s[len - 1] == ']') {
        cleanJsonArrayInner(s, len, out, out_sz);
    } else {
        _ = c.snprintf(out, out_sz, "%s", s);
    }
}

// ── Dockerfile parser ───────────────────────────────────────────────

fn dfParseFrom(line: Str, r: *DockerfileResult) void {
    if (!hasPrefixCi(line, "FROM")) return;
    var p: Str = line + 4;
    if (!isBlank(p[0])) return;
    p = skipWs(p);

    // Extract image
    if (r.stage_count >= IS_MAX_STAGES) return;
    const idx = ix(r.stage_count);
    _ = extractToken(p, &r.stage_images[idx]);

    // Advance past image
    while (p[0] != 0 and !isBlank(p[0])) p += 1;
    p = skipWs(p);

    // Check for AS <name>
    r.stage_names[idx][0] = 0;
    if (hasPrefixCi(p, "AS") and isBlank(p[2])) {
        p = skipWs(p + 3);
        _ = extractWord(p, &r.stage_names[idx]);
    }
    r.stage_count += 1;
}

fn dfParseExpose(line: Str, r: *DockerfileResult) void {
    if (!hasPrefixCi(line, "EXPOSE")) return;
    var p: Str = line + 6;
    if (!isBlank(p[0])) return;
    p = skipWs(p);

    // Parse space-separated ports
    while (p[0] != 0 and r.port_count < IS_MAX_PORTS) {
        var port: [c.CBM_SZ_32]u8 = undefined;
        const n = extractToken(p, &port);
        if (n == 0) break;

        // Strip protocol suffix (e.g. 8080/tcp)
        const slash: MutStr = c.strchr(&port, '/');
        if (slash != null) slash[0] = 0;

        copyStr(&r.exposed_ports[ix(r.port_count)], &port);
        r.port_count += 1;

        p += n;
        p = skipWs(p);
    }
}

fn dfParseEnv(line: Str, r: *DockerfileResult) void {
    if (!hasPrefixCi(line, "ENV")) return;
    var p: Str = line + 3;
    if (!isBlank(p[0])) return;
    p = skipWs(p);

    if (r.env_count >= c.CBM_SZ_64) return;

    // Extract key
    var key: [c.CBM_SZ_128:0]u8 = undefined;
    const klen = extractWord(p, &key);
    if (klen == 0) return;
    p += klen;

    // Separator: = or space
    if (p[0] != '=' and !isBlank(p[0])) return;
    p += 1;
    // Skip additional whitespace after separator
    while (isBlank(p[0])) p += 1;

    // Value is rest of line (trimmed)
    var value: [c.CBM_SZ_512:0]u8 = undefined;
    copyStr(&value, p);
    rtrim(&value);

    // Filter secrets
    if (cbm_is_secret_binding(&key, &value)) return;

    const env = &r.env_vars[ix(r.env_count)];
    copyStr(&env.key, &key);
    copyStr(&env.value, &value);
    r.env_count += 1;
}

fn dfParseArg(line: Str, r: *DockerfileResult) void {
    if (!hasPrefixCi(line, "ARG")) return;
    var p: Str = line + 3;
    if (!isBlank(p[0])) return;
    p = skipWs(p);

    if (r.build_arg_count >= c.CBM_SZ_32) return;
    const arg = &r.build_args[ix(r.build_arg_count)];
    _ = extractWord(p, arg);
    if (arg[0] != 0) r.build_arg_count += 1;
}

/// Parse a Dockerfile CMD/ENTRYPOINT value: strip brackets, trim.
fn dfParseJsonCmd(line: Str, prefix_len: usize, out: []u8) void {
    const p = skipWs(line + prefix_len);
    var raw: [c.CBM_SZ_512:0]u8 = undefined;
    copyStr(&raw, p);
    rtrim(&raw);
    cbm_clean_json_brackets(&raw, out.ptr, out.len);
}

fn dfParseDirectives(line: Str, r: *DockerfileResult) void {
    if (hasPrefixCi(line, "WORKDIR") and isBlank(line[7])) {
        const p = skipWs(line + 8);
        copyStr(&r.workdir, p);
        rtrim(@ptrCast(&r.workdir));
        return;
    }
    if (hasPrefixCi(line, "CMD") and isBlank(line[3])) {
        dfParseJsonCmd(line, 4, &r.cmd);
        return;
    }
    if (hasPrefixCi(line, "ENTRYPOINT") and isBlank(line[10])) {
        dfParseJsonCmd(line, 11, &r.entrypoint);
        return;
    }
    if (hasPrefixCi(line, "USER") and isBlank(line[4])) {
        const p = skipWs(line + 5);
        _ = extractWord(p, &r.user);
        return;
    }
    if (hasPrefixCi(line, "HEALTHCHECK") and isBlank(line[LEN_HEALTHCHECK])) {
        if (ciStrstr(line + LEN_HEALTHCHECK, "CMD")) |cmd_pos| {
            copyStr(&r.healthcheck, skipWs(cmd_pos + 3));
            rtrim(@ptrCast(&r.healthcheck));
        }
    }
}

/// Iterates `source` line by line, copying each line into a caller buffer
/// (truncated to fit) and returning it without leading blanks or trailing
/// whitespace.
const LineReader = struct {
    p: [*:0]const u8,
    line: [c.CBM_SZ_4K:0]u8 = undefined,

    fn next(self: *LineReader) ?[*:0]u8 {
        if (self.p[0] == 0) return null;
        const eol: CStr = c.strchr(self.p, '\n');
        var line_len: usize = if (eol != null) util.ptrDiff(eol, self.p) else c.strlen(self.p);
        if (line_len >= c.CBM_SZ_4K) line_len = c.CBM_SZ_4K - 1;
        @memcpy(self.line[0..line_len], self.p[0..line_len]);
        self.line[line_len] = 0;
        // Without a newline the C parsers advanced by the truncated length,
        // so the tail of an over-long last line is read as a further line.
        self.p = if (eol != null) @ptrCast(eol + 1) else self.p + line_len;

        var trimmed: [*:0]u8 = &self.line;
        while (isBlank(trimmed[0])) trimmed += 1;
        rtrim(trimmed);
        return trimmed;
    }
};

pub export fn cbm_parse_dockerfile_source(source: ?Str, out_c: ?*DockerfileResult) c_int {
    const out = out_c orelse return c.CBM_NOT_FOUND;
    var lines: LineReader = .{ .p = source orelse return c.CBM_NOT_FOUND };
    out.* = std.mem.zeroes(DockerfileResult);

    // Process line by line
    while (lines.next()) |trimmed| {
        // Skip empty lines and comments
        if (trimmed[0] == 0 or trimmed[0] == '#') continue;

        dfParseFrom(trimmed, out);
        dfParseExpose(trimmed, out);
        dfParseEnv(trimmed, out);
        dfParseArg(trimmed, out);
        dfParseDirectives(trimmed, out);
    }

    // No stages = empty/invalid Dockerfile
    if (out.stage_count == 0) return c.CBM_NOT_FOUND;

    // base_image = last stage's image (use intermediate copy to avoid restrict overlap)
    {
        var tmp: [c.CBM_SZ_256]u8 = undefined;
        _ = c.snprintf(&tmp, tmp.len, "%s", @as(CStr, &out.stage_images[ix(out.stage_count) - 1]));
        out.base_image = tmp;
    }

    return 0;
}

// ── Dotenv parser ───────────────────────────────────────────────────

/// Strip matching surrounding quotes from a value string in-place.
fn stripSurroundingQuotes(value: [*:0]u8) void {
    const vlen = c.strlen(value);
    if (vlen >= 2 and ((value[0] == '"' and value[vlen - 1] == '"') or
        (value[0] == '\'' and value[vlen - 1] == '\'')))
    {
        _ = c.memmove(value, value + 1, vlen - 2);
        value[vlen - 2] = 0;
    }
}

/// Parse a single dotenv line: KEY=VALUE. Returns true if an env var was added.
fn parseDotenvLine(trimmed: Str, out: *DotenvResult) bool {
    var key: [c.CBM_SZ_128:0]u8 = undefined;
    var klen: usize = 0;
    if (std.ascii.isAlphabetic(trimmed[0]) or trimmed[0] == '_') {
        klen = extractWord(trimmed, &key);
    }
    if (klen == 0 or trimmed[klen] != '=') return false;

    var value: [c.CBM_SZ_512:0]u8 = undefined;
    copyStr(&value, trimmed + klen + 1);
    rtrim(&value);
    stripSurroundingQuotes(&value);

    if (cbm_is_secret_binding(&key, &value) or out.env_count >= c.CBM_SZ_64) return false;
    const env = &out.env_vars[ix(out.env_count)];
    copyStr(&env.key, &key);
    copyStr(&env.value, &value);
    out.env_count += 1;
    return true;
}

pub export fn cbm_parse_dotenv_source(source: ?Str, out_c: ?*DotenvResult) c_int {
    const out = out_c orelse return c.CBM_NOT_FOUND;
    var lines: LineReader = .{ .p = source orelse return c.CBM_NOT_FOUND };
    out.* = std.mem.zeroes(DotenvResult);

    while (lines.next()) |trimmed| {
        if (trimmed[0] == 0 or trimmed[0] == '#') continue;
        _ = parseDotenvLine(trimmed, out);
    }

    return if (out.env_count > 0) 0 else IS_NOT_FOUND;
}

// ── Shell script parser ─────────────────────────────────────────────

/// Store VAR=VALUE from `val_start` unless it is a secret or the table is full.
fn shellAddVar(r: *ShellResult, key: *[c.CBM_SZ_128:0]u8, val_start: Str) void {
    var value: [c.CBM_SZ_512:0]u8 = undefined;
    copyStr(&value, val_start);
    rtrim(&value);

    // Strip surrounding quotes
    stripSurroundingQuotes(&value);

    if (cbm_is_secret_binding(key, &value)) return;
    if (r.env_count >= c.CBM_SZ_64) return;
    const env = &r.env_vars[ix(r.env_count)];
    copyStr(&env.key, key);
    copyStr(&env.value, &value);
    r.env_count += 1;
}

fn shellParseExport(line: Str, r: *ShellResult) void {
    // export VAR=VALUE
    if (!hasPrefix(line, "export") or !isBlank(line[6])) return;
    const p = skipWs(line + 7);

    var key: [c.CBM_SZ_128:0]u8 = undefined;
    const klen = extractWord(p, &key);
    if (klen == 0 or p[klen] != '=') return;

    shellAddVar(r, &key, p + klen + 1);
}

fn shellParsePlainVar(line: Str, r: *ShellResult) void {
    // VAR=VALUE (only if no spaces in line — avoids matching commands)
    if (c.strchr(line, ' ') != null or c.strchr(line, '\t') != null) return;

    var key: [c.CBM_SZ_128:0]u8 = undefined;
    const klen = extractWord(line, &key);
    if (klen == 0 or line[klen] != '=') return;

    shellAddVar(r, &key, line + klen + 1);
}

fn shellParseSource(line: Str, r: *ShellResult) void {
    // source <file> or . <file>
    var p: ?Str = null;
    if (hasPrefix(line, "source") and isBlank(line[6])) {
        p = skipWs(line + 7);
    } else if (line[0] == '.' and isBlank(line[1])) {
        p = skipWs(line + 2);
    }
    const file = p orelse return;

    if (r.source_count >= IS_MAX_SOURCES) return;

    // Strip surrounding quotes
    var path: [c.CBM_SZ_256:0]u8 = undefined;
    copyStr(&path, file);
    rtrim(&path);
    stripSurroundingQuotes(&path);

    // Extract just the path token (no trailing args)
    const space: MutStr = c.strchr(&path, ' ');
    if (space != null) space[0] = 0;

    copyStr(&r.sources[ix(r.source_count)], &path);
    r.source_count += 1;
}

fn shellParseDocker(line: Str, r: *ShellResult) void {
    // docker <subcmd> or docker-compose <subcmd>
    var p: ?Str = null;
    var tool: CStr = null;

    if (c.strncmp(line, "docker-compose", LEN_DOCKER_COMPOSE) == 0 and isBlank(line[LEN_DOCKER_COMPOSE])) {
        tool = "docker-compose";
        p = skipWs(line + LEN_DOCKER_COMPOSE + 1);
    } else if (hasPrefix(line, "docker") and isBlank(line[6])) {
        tool = "docker";
        p = skipWs(line + 7);
    }
    const rest = p orelse return;
    if (tool == null or rest[0] == 0) return;

    if (r.docker_cmd_count >= IS_MAX_STAGES) return;

    var subcmd: [c.CBM_SZ_64]u8 = undefined;
    _ = extractWord(rest, &subcmd);
    if (subcmd[0] != 0) {
        const slot = &r.docker_cmds[ix(r.docker_cmd_count)];
        _ = c.snprintf(slot, slot.len, "%s %s", tool, @as(CStr, &subcmd));
        r.docker_cmd_count += 1;
    }
}

/// Check for shebang on first non-empty line. Returns true if line was consumed.
fn shellTryShebang(trimmed: Str, out: *ShellResult, shebang_checked: *bool) bool {
    if (shebang_checked.*) return false;
    if (trimmed[0] == '#' and trimmed[1] == '!') {
        var sb: Str = trimmed + 2;
        while (sb[0] == ' ') sb += 1;
        copyStr(&out.shebang, sb);
        rtrim(@ptrCast(&out.shebang));
        shebang_checked.* = true;
        return true;
    }
    shebang_checked.* = true;
    return false;
}

/// Dispatch a single trimmed shell line to the appropriate parser.
fn shellDispatchLine(trimmed: Str, out: *ShellResult) void {
    if (hasPrefix(trimmed, "export")) {
        shellParseExport(trimmed, out);
        return;
    }
    if (hasPrefix(trimmed, "source") or (trimmed[0] == '.' and trimmed[1] == ' ')) {
        shellParseSource(trimmed, out);
        return;
    }
    if (hasPrefix(trimmed, "docker")) {
        shellParseDocker(trimmed, out);
    }
    shellParsePlainVar(trimmed, out);
}

pub export fn cbm_parse_shell_source(source: ?Str, out_c: ?*ShellResult) c_int {
    const out = out_c orelse return c.CBM_NOT_FOUND;
    var lines: LineReader = .{ .p = source orelse return c.CBM_NOT_FOUND };
    out.* = std.mem.zeroes(ShellResult);

    var shebang_checked = false;

    while (lines.next()) |trimmed| {
        if (trimmed[0] == 0) continue;
        if (shellTryShebang(trimmed, out, &shebang_checked)) continue;
        if (trimmed[0] == '#') continue;
        shellDispatchLine(trimmed, out);
    }

    const has_content = out.shebang[0] != 0 or out.env_count > 0 or out.source_count > 0 or
        out.docker_cmd_count > 0;
    return if (has_content) 0 else IS_NOT_FOUND;
}

// ── Terraform parser ────────────────────────────────────────────────

const TfBlockKind = enum { none, variable, module, terraform };

/// Extract a double-quoted string value after = sign.
/// Handles: key = "value" and key = value
fn tfExtractQuoted(line: Str, prefix: []const u8, out: []u8) void {
    var p = skipWs(line);
    if (c.strncmp(p, prefix.ptr, prefix.len) != 0) return;
    p = skipWs(p + prefix.len);
    if (p[0] != '=') return;
    p = skipWs(p + 1);

    // Strip optional quotes
    if (p[0] == '"') {
        p += 1;
        const end: CStr = c.strchr(p, '"');
        var vlen: usize = if (end != null) util.ptrDiff(end, p) else c.strlen(p);
        if (vlen >= out.len) vlen = out.len - 1;
        @memcpy(out[0..vlen], p[0..vlen]);
        out[vlen] = 0;
    } else {
        _ = extractToken(p, out);
    }
}

/// Count occurrences of a character in a string
fn countChar(s: Str, ch: u8) c_int {
    var n: c_int = 0;
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {
        if (s[i] == ch) n += 1;
    }
    return n;
}

/// Extract double-quoted identifier after keyword.
/// Matches: keyword "identifier" → sets out to "identifier"
/// Returns number of chars consumed from p, or 0 if no match.
fn tfExtractIdent(p: Str, keyword: []const u8, out: []u8) usize {
    if (c.strncmp(p, keyword.ptr, keyword.len) != 0) return 0;
    var after = skipWs(p + keyword.len);
    if (after[0] != '"') return 0;
    after += 1;
    const end: CStr = c.strchr(after, '"');
    if (end == null) return 0;
    var ilen = util.ptrDiff(end, after);
    if (ilen >= out.len) ilen = out.len - 1;
    @memcpy(out[0..ilen], after[0..ilen]);
    out[ilen] = 0;
    return util.ptrDiff(end, p) + 1;
}

/// Parse a second quoted string after the first ident (for resource/data blocks).
fn tfExtractSecondQuoted(trimmed: Str, consumed: usize, out: []u8) void {
    var rest = skipWs(trimmed + consumed);
    out[0] = 0;
    if (rest[0] == '"') {
        rest += 1;
        const end: CStr = c.strchr(rest, '"');
        if (end != null) {
            var len = util.ptrDiff(end, rest);
            if (len >= out.len) len = out.len - 1;
            @memcpy(out[0..len], rest[0..len]);
            out[len] = 0;
        }
    }
}

/// Try to parse a simple "keyword ident" block (variable, output, provider).
/// `names` is the table of fixed-size name slots the identifier is added to.
fn tfTrySimpleBlock(trimmed: Str, keyword: []const u8, names: anytype, count: *c_int) bool {
    var ident: [c.CBM_SZ_128]u8 = undefined;
    const consumed = tfExtractIdent(trimmed, keyword, &ident);
    if (consumed == 0 or count.* >= names.len) return false;
    copyStr(&names[ix(count.*)], &ident);
    count.* += 1;
    return true;
}

/// Try to match terraform/locals keyword blocks.
fn tfTryKeywordBlock(trimmed: Str, cur_block: *TfBlockKind, out: *TerraformResult) bool {
    if (hasPrefix(trimmed, "terraform") and c.strstr(trimmed, "{") != null) {
        cur_block.* = .terraform;
        return true;
    }
    if (hasPrefix(trimmed, "locals") and c.strstr(trimmed, "{") != null) {
        out.has_locals = true;
        return true;
    }
    return false;
}

/// Detect a top-level block header. Returns true if line was consumed as a header.
fn tfDetectBlockHeader(
    trimmed: Str,
    out: *TerraformResult,
    cur_block: *TfBlockKind,
    cur_var_idx: *c_int,
    cur_mod_idx: *c_int,
) bool {
    var ident1: [c.CBM_SZ_128]u8 = undefined;

    // resource "type" "name"
    {
        const rc = tfExtractIdent(trimmed, "resource", &ident1);
        if (rc > 0 and out.resource_count < c.CBM_SZ_32) {
            var ident2: [c.CBM_SZ_128]u8 = undefined;
            tfExtractSecondQuoted(trimmed, rc, &ident2);
            const resource = &out.resources[ix(out.resource_count)];
            copyStr(&resource.type, &ident1);
            copyStr(&resource.name, &ident2);
            out.resource_count += 1;
            return true;
        }
    }

    // variable "name"
    var consumed = tfExtractIdent(trimmed, "variable", &ident1);
    if (consumed > 0 and out.variable_count < c.CBM_SZ_32) {
        cur_block.* = .variable;
        cur_var_idx.* = out.variable_count;
        copyStr(&out.variables[ix(cur_var_idx.*)].name, &ident1);
        out.variable_count += 1;
        return true;
    }

    // output "name"
    if (tfTrySimpleBlock(trimmed, "output", &out.outputs, &out.output_count)) return true;

    // provider "name"
    if (tfTrySimpleBlock(trimmed, "provider", &out.providers, &out.provider_count)) return true;

    // module "name"
    consumed = tfExtractIdent(trimmed, "module", &ident1);
    if (consumed > 0 and out.module_count < IS_MAX_STAGES) {
        cur_block.* = .module;
        cur_mod_idx.* = out.module_count;
        copyStr(&out.modules[ix(cur_mod_idx.*)].tf_name, &ident1);
        out.module_count += 1;
        return true;
    }

    // data "type" "name"
    {
        const dc = tfExtractIdent(trimmed, "data", &ident1);
        if (dc > 0 and out.data_source_count < IS_MAX_STAGES) {
            var ident2: [c.CBM_SZ_128]u8 = undefined;
            tfExtractSecondQuoted(trimmed, dc, &ident2);
            const data_source = &out.data_sources[ix(out.data_source_count)];
            copyStr(&data_source.type, &ident1);
            copyStr(&data_source.name, &ident2);
            out.data_source_count += 1;
            return true;
        }
    }

    return tfTryKeywordBlock(trimmed, cur_block, out);
}

/// Extract attributes from a line inside a variable block.
fn tfParseVariableAttrs(trimmed: Str, v: *c.cbm_tf_variable_t) void {
    var def_val: [c.CBM_SZ_256:0]u8 = @splat(0);
    tfExtractQuoted(trimmed, "default", &def_val);
    if (def_val[0] != 0 and !cbm_is_secret_binding(@ptrCast(&v.name), &def_val)) {
        copyStr(&v.default_val, &def_val);
    }
    var type_val: [c.CBM_SZ_64]u8 = @splat(0);
    tfExtractQuoted(trimmed, "type", &type_val);
    if (type_val[0] != 0) copyStr(&v.type, &type_val);
    var desc_val: [c.CBM_SZ_256]u8 = @splat(0);
    tfExtractQuoted(trimmed, "description", &desc_val);
    if (desc_val[0] != 0) copyStr(&v.description, &desc_val);
}

/// Extract attributes inside a block (variable/module/terraform).
fn tfParseBlockAttrs(
    trimmed: Str,
    out: *TerraformResult,
    cur_block: TfBlockKind,
    cur_var_idx: c_int,
    cur_mod_idx: c_int,
) void {
    switch (cur_block) {
        .variable => {
            if (cur_var_idx >= 0) tfParseVariableAttrs(trimmed, &out.variables[ix(cur_var_idx)]);
        },
        .module => {
            if (cur_mod_idx >= 0) {
                var src_val: [c.CBM_SZ_256]u8 = @splat(0);
                tfExtractQuoted(trimmed, "source", &src_val);
                if (src_val[0] != 0) copyStr(&out.modules[ix(cur_mod_idx)].source, &src_val);
            }
        },
        .terraform => {
            const bp = skipWs(trimmed);
            var backend_name: [c.CBM_SZ_128]u8 = @splat(0);
            _ = tfExtractIdent(bp, "backend", &backend_name);
            if (backend_name[0] != 0) copyStr(&out.backend, &backend_name);
        },
        .none => {},
    }
}

/// Check if any terraform content was parsed.
fn tfResultHasContent(out: *const TerraformResult) bool {
    return out.resource_count > 0 or out.variable_count > 0 or out.output_count > 0 or
        out.provider_count > 0 or out.module_count > 0 or out.data_source_count > 0 or
        out.backend[0] != 0 or out.has_locals;
}

pub export fn cbm_parse_terraform_source(source: ?Str, out_c: ?*TerraformResult) c_int {
    const out = out_c orelse return c.CBM_NOT_FOUND;
    var lines: LineReader = .{ .p = source orelse return c.CBM_NOT_FOUND };
    out.* = std.mem.zeroes(TerraformResult);

    var brace_depth: c_int = 0;
    var cur_block: TfBlockKind = .none;
    var cur_var_idx: c_int = IS_NOT_FOUND;
    var cur_mod_idx: c_int = IS_NOT_FOUND;

    while (lines.next()) |trimmed| {
        // Skip empty lines and # or // comments.
        if (trimmed[0] == 0 or trimmed[0] == '#' or (trimmed[0] == '/' and trimmed[1] == '/')) {
            continue;
        }

        brace_depth += countChar(trimmed, '{') - countChar(trimmed, '}');

        if (brace_depth <= 1 and cur_block == .none) {
            if (tfDetectBlockHeader(trimmed, out, &cur_block, &cur_var_idx, &cur_mod_idx)) continue;
        }

        if (cur_block != .none and brace_depth >= 1 and brace_depth <= 2) {
            tfParseBlockAttrs(trimmed, out, cur_block, cur_var_idx, cur_mod_idx);
        }

        if (brace_depth == 0 and cur_block != .none) {
            cur_block = .none;
            cur_var_idx = IS_NOT_FOUND;
            cur_mod_idx = IS_NOT_FOUND;
        }
    }

    return if (tfResultHasContent(out)) 0 else IS_NOT_FOUND;
}

// ── Infra QN helper ─────────────────────────────────────────────────

pub export fn cbm_infra_qn(project_name: CStr, rel_path: CStr, infra_type: CStr, service_name: CStr) MutStr {
    const base: MutStr = c.cbm_pipeline_fqn_compute(project_name, rel_path, "");
    if (base == null) return null;

    var result: [c.CBM_SZ_1K]u8 = undefined;
    if (service_name != null and service_name[0] != 0 and infra_type != null and
        streq(infra_type, "compose-service"))
    {
        _ = c.snprintf(&result, result.len, "%s::%s", base, service_name);
    } else {
        _ = c.snprintf(&result, result.len, "%s.__infra__", base);
    }

    c.free(base);
    return c.strdup(&result);
}

// ── Helm Chart.yaml parser (#338) ───────────────────────────────────

/// Copy the scalar value following a "key:" up to end-of-line into out,
/// trimming surrounding whitespace, optional quotes, and trailing comments.
fn helmCopyScalar(s_in: [*]const u8, eol: [*]const u8, out: []u8) void {
    var s = s_in;
    while (@intFromPtr(s) < @intFromPtr(eol) and isBlank(s[0])) s += 1;
    var quote: u8 = 0;
    if (@intFromPtr(s) < @intFromPtr(eol) and (s[0] == '"' or s[0] == '\'')) {
        quote = s[0];
        s += 1;
    }
    var e = s;
    while (@intFromPtr(e) < @intFromPtr(eol)) : (e += 1) {
        if (quote != 0) {
            if (e[0] == quote) break;
        } else if (e[0] == ' ' or e[0] == '\t' or e[0] == '#' or e[0] == '\r') {
            break;
        }
    }
    var n = util.ptrDiff(e, s);
    if (n >= out.len) n = out.len - 1;
    @memcpy(out[0..n], s[0..n]);
    out[n] = 0;
}

/// Match "key:" at the start of a content pointer; return value start or NULL.
fn helmMatchKey(content: [*]const u8, eol: [*]const u8, key: []const u8) ?[*]const u8 {
    if (util.ptrDiff(eol, content) < key.len + 1) return null;
    if (c.strncmp(content, key.ptr, key.len) != 0 or content[key.len] != ':') return null;
    return content + key.len + 1;
}

pub export fn cbm_parse_helm_chart(source: ?Str, out_c: ?*c.cbm_helm_chart_t) c_int {
    var p: [*]const u8 = source orelse return -1;
    const out = out_c orelse return -1;
    out.chart_name[0] = 0;
    out.dep_count = 0;

    var in_deps = false;
    while (p[0] != 0) {
        var eol: [*]const u8 = undefined;
        const newline: CStr = c.strchr(p, '\n');
        if (newline != null) {
            eol = newline;
        } else {
            eol = p + c.strlen(p);
        }
        var indent: c_int = 0;
        var content = p;
        while (@intFromPtr(content) < @intFromPtr(eol) and content[0] == ' ') {
            content += 1;
            indent += 1;
        }
        if (@intFromPtr(content) < @intFromPtr(eol) and content[0] != '#') {
            if (indent == 0) {
                // New top-level key ends any dependencies block.
                in_deps = helmMatchKey(content, eol, "dependencies") != null;
                const nv = helmMatchKey(content, eol, "name");
                if (!in_deps and nv != null and out.chart_name[0] == 0) {
                    helmCopyScalar(nv.?, eol, &out.chart_name);
                }
            } else if (in_deps) {
                // List item: optional leading "- ", then look for name:.
                var q = content;
                if (q[0] == '-') {
                    q += 1;
                    while (@intFromPtr(q) < @intFromPtr(eol) and q[0] == ' ') q += 1;
                }
                const nv = helmMatchKey(q, eol, "name");
                if (nv != null and out.dep_count < c.CBM_HELM_MAX_DEPS) {
                    var buf: [c.CBM_HELM_NAME_MAX]u8 = undefined;
                    helmCopyScalar(nv.?, eol, &buf);
                    if (buf[0] != 0) {
                        out.deps[ix(out.dep_count)] = buf;
                        out.dep_count += 1;
                    }
                }
            }
        }
        if (eol[0] == 0) break;
        p = eol + 1;
    }
    return if (out.chart_name[0] != 0 or out.dep_count > 0) 0 else -1;
}
