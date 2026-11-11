//! pass_enrichment.zig — Decorator tokenization, camelCase splitting,
//! and decorator_tags pass (post-flush enrichment).
//!
//! Pure helper functions + a store-level pass that classifies decorators
//! into semantic tags via auto-discovery (words on 2+ nodes become tags).

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const Node = c.cbm_gbuf_node_t;
const HashTable = c.CBMHashTable;

const ENRICH_ATTR_SKIP = 2;
const ENRICH_MAX_CAMEL = 16;

/// cbm_intptr_to_ptr: a small integer stored as a hash table value.
fn intToPtr(v: isize) ?*anyopaque {
    return @ptrFromInt(@as(usize, @bitCast(v)));
}

fn ptrToInt(p: ?*anyopaque) isize {
    return @bitCast(@intFromPtr(p));
}

fn isDecoratorStopword(w: CStr) bool {
    const stopwords = [_][*:0]const u8{
        "get",   "set",  "new",   "class",  "method", "function",
        "value", "type", "param", "return", "public", "private",
        "for",   "if",   "the",   "and",    "or",     "not",
        "with",  "from", "app",   "router",
    };
    for (stopwords) |stopword| {
        if (util.streq(w, stopword)) return true;
    }
    return false;
}

pub export fn cbm_split_camel_case(s: CStr, out: [*c]MutStr, max_out: c_int) c_int {
    if (s == null or out == null or max_out <= 0) return 0;
    const len = c.strlen(s);
    if (len == 0) return 0;

    var count: c_int = 0;
    var start: usize = 0;

    var i: usize = 1;
    while (i < len) : (i += 1) {
        if (s[i] >= 'A' and s[i] <= 'Z' and s[i - 1] >= 'a' and s[i - 1] <= 'z') {
            if (count < max_out) {
                out[ix(count)] = c.cbm_strndup(s + start, i - start);
                count += 1;
            }
            start = i;
        }
    }
    // Emit remaining
    if (count < max_out) {
        out[ix(count)] = c.cbm_strndup(s + start, len - start);
        count += 1;
    }
    return count;
}

/// Strip decorator syntax: leading @, #[...], and arguments (...).
/// Operates in-place on buf. Returns pointer to the cleaned token start.
fn stripDecoratorSyntax(buf: [*:0]u8) [*:0]u8 {
    var p = buf;
    if (p[0] == '@') p += 1;
    if (p[0] == '#' and p[1] == '[') {
        p += ENRICH_ATTR_SKIP;
        const plen = c.strlen(p);
        if (plen > 0 and p[plen - 1] == ']') p[plen - 1] = 0;
    }
    const paren: MutStr = c.strchr(p, '(');
    if (paren != null) paren[0] = 0;
    var ch = p;
    while (ch[0] != 0) : (ch += 1) {
        if (ch[0] == '.' or ch[0] == '_' or ch[0] == '-' or ch[0] == ':' or ch[0] == '/') {
            ch[0] = ' ';
        }
    }
    return p;
}

pub export fn cbm_tokenize_decorator(dec: CStr, out: [*c]MutStr, max_out: c_int) c_int {
    if (dec == null or out == null or max_out <= 0) return 0;

    var buf: [c.CBM_SZ_256:0]u8 = undefined;
    var len = c.strlen(dec);
    if (len >= c.CBM_SZ_256) len = c.CBM_SZ_256 - 1;
    @memcpy(buf[0..len], dec[0..len]);
    buf[len] = 0;

    const p = stripDecoratorSyntax(&buf);

    var count: c_int = 0;
    var saveptr: MutStr = null;
    var part: MutStr = c.strtok_r(p, " ", &saveptr);

    while (part != null and count < max_out) {
        var camel_parts: [ENRICH_MAX_CAMEL]MutStr = undefined;
        const camel_count = cbm_split_camel_case(part, &camel_parts, ENRICH_MAX_CAMEL);

        var i: usize = 0;
        while (i < camel_count and count < max_out) : (i += 1) {
            const word = camel_parts[i];
            const word_len = c.strlen(word);
            for (word[0..word_len]) |*ch| ch.* = std.ascii.toLower(ch.*);
            if (word_len >= 2 and !isDecoratorStopword(word)) {
                out[ix(count)] = word;
                count += 1;
            } else {
                c.free(word);
            }
        }
        part = c.strtok_r(null, " ", &saveptr);
    }

    return count;
}

// ════════════════════════════════════════════════════════════════════
//  Decorator Tags Pass (post-flush, operates on store)
//
//  Algorithm:
//  1. Load all Function/Method/Class nodes from the store
//  2. For each node with "decorators" property, tokenize decorators
//  3. Count word frequency across all nodes
//  4. Words on 2+ distinct nodes become candidates
//  5. Update each node's properties_json with "decorator_tags" array
// ════════════════════════════════════════════════════════════════════

/// Per-node tokenization state
const TaggedNode = extern struct {
    node_id: i64,
    qualified_name: MutStr,
    words: [*c]MutStr,
    word_count: c_int,
};

/// Extract the "decorators" array from a properties_json string.
/// Returns a NULL-terminated array of strings. Caller must free array and strings.
fn extractDecoratorsFromJson(json: CStr) [*c]MutStr {
    @setEvalBranchQuota(10000);
    if (json == null) return null;

    const doc = c.yyjson_read(json, c.strlen(json), 0);
    if (doc == null) return null;

    const root = c.yyjson_doc_get_root(doc);
    const decs = c.yyjson_obj_get(root, "decorators");
    if (decs == null or !c.yyjson_is_arr(decs)) {
        c.yyjson_doc_free(doc);
        return null;
    }

    const cnt = c.yyjson_arr_size(decs);
    if (cnt == 0) {
        c.yyjson_doc_free(doc);
        return null;
    }

    const out = interop.callocN(MutStr, cnt + 1);
    var idx: usize = 0;
    var iter: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(decs, &iter);
    while (true) {
        const item = c.yyjson_arr_iter_next(&iter);
        if (item == null) break;
        if (c.yyjson_is_str(item)) {
            out[idx] = c.strdup(c.yyjson_get_str(item));
            idx += 1;
        }
    }
    out[idx] = null;

    c.yyjson_doc_free(doc);
    if (idx > 0) return out;
    c.free(@ptrCast(out));
    return null;
}

/// Tokenize all decorators on a node into unique words.
/// Returns heap-allocated array, caller frees each word and the array.
fn extractDecoratorWords(json: CStr, out_words: *[*c]MutStr) c_int {
    const decorators = extractDecoratorsFromJson(json);
    if (decorators == null) {
        out_words.* = null;
        return 0;
    }

    // Collect unique words from all decorators
    var all_words: [c.CBM_SZ_256]MutStr = undefined;
    var total: usize = 0;
    const seen = c.cbm_ht_create(c.CBM_SZ_32);

    var i: usize = 0;
    while (decorators[i] != null) : (i += 1) {
        var tokens: [c.CBM_SZ_32]MutStr = undefined;
        const tc = cbm_tokenize_decorator(decorators[i], &tokens, c.CBM_SZ_32);
        for (tokens[0..ix(tc)]) |token| {
            if (c.cbm_ht_get(seen, token) == null and total < all_words.len) {
                _ = c.cbm_ht_set(seen, token, intToPtr(1));
                all_words[total] = token;
                total += 1;
            } else {
                c.free(token);
            }
        }
        c.free(decorators[i]);
    }
    c.free(@ptrCast(decorators));
    c.cbm_ht_free(seen);

    if (total == 0) {
        out_words.* = null;
        return 0;
    }

    out_words.* = interop.mallocN(MutStr, total);
    @memcpy(out_words.*[0..total], all_words[0..total]);
    return @intCast(total);
}

/// Insert "decorator_tags" into a properties_json string.
/// Returns a newly allocated JSON string. Caller must free().
fn injectDecoratorTags(json: CStr, tags: []const MutStr) MutStr {
    @setEvalBranchQuota(10000);
    const doc = c.yyjson_read(json, c.strlen(json), 0);
    const root = if (doc != null) c.yyjson_doc_get_root(doc) else null;

    const mdoc = c.yyjson_mut_doc_new(null);
    const mroot = if (root != null and c.yyjson_is_obj(root))
        c.yyjson_val_mut_copy(mdoc, root)
    else
        c.yyjson_mut_obj(mdoc);
    c.yyjson_mut_doc_set_root(mdoc, mroot);

    // Remove existing decorator_tags if any
    _ = c.yyjson_mut_obj_remove_key(mroot, "decorator_tags");

    // Add sorted tag array
    const arr = c.yyjson_mut_arr(mdoc);
    for (tags) |tag| {
        _ = c.yyjson_mut_arr_add_str(mdoc, arr, tag);
    }
    _ = c.yyjson_mut_obj_add_val(mdoc, mroot, "decorator_tags", arr);

    const result = c.yyjson_mut_write(mdoc, 0, null);
    c.yyjson_mut_doc_free(mdoc);
    c.yyjson_doc_free(doc);
    return result;
}

/// Simple string comparison for qsort
fn cmpStr(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const sa: *const CStr = @ptrCast(@alignCast(a));
    const sb: *const CStr = @ptrCast(@alignCast(b));
    return c.strcmp(sa.*, sb.*);
}

/// Free TaggedNode array.
fn freeTaggedNodes(nodes: [*c]TaggedNode, count: c_int) void {
    for (0..ix(count)) |n| {
        c.free(nodes[n].qualified_name);
        for (0..ix(nodes[n].word_count)) |w| {
            c.free(nodes[n].words[w]);
        }
        c.free(@ptrCast(nodes[n].words));
    }
    c.free(nodes);
}

/// Phase 1: Collect decorated nodes and count word frequency.
fn collectDecoratedNodes(gbuf: *c.cbm_gbuf_t, out_nodes: *[*c]TaggedNode, word_counts: ?*HashTable) c_int {
    // "Struct" alongside "Class" so Go/Rust/Swift/D struct names keep
    // contributing to / receiving auto-tags as they did when structs were
    // labelled "Class".
    const labels = [_][*:0]const u8{ "Function", "Method", "Class", "Struct" };
    var nodes: [*c]TaggedNode = null;
    var node_count: c_int = 0;
    var node_cap: c_int = 0;

    for (labels) |label| {
        var found: [*c][*c]const Node = null;
        var fc: c_int = 0;
        _ = c.cbm_gbuf_find_by_label(gbuf, label, &found, &fc);
        if (found == null or fc <= 0) continue;

        for (0..ix(fc)) |i| {
            var words: [*c]MutStr = null;
            const wc = extractDecoratorWords(found[i].*.properties_json, &words);
            if (wc <= 0) continue;
            if (node_count >= node_cap) {
                node_cap = if (node_cap != 0) node_cap * 2 else c.CBM_SZ_64;
                nodes = interop.safeReallocN(TaggedNode, nodes, ix(node_cap));
            }
            const tn = util.at(nodes, ix(node_count));
            node_count += 1;
            tn.node_id = found[i].*.id;
            tn.qualified_name = c.strdup(found[i].*.qualified_name);
            tn.words = words;
            tn.word_count = wc;
            for (0..ix(wc)) |w| {
                const cnt = ptrToInt(c.cbm_ht_get(word_counts, words[w]));
                _ = c.cbm_ht_set(word_counts, words[w], intToPtr(cnt + 1));
            }
        }
    }

    out_nodes.* = nodes;
    return node_count;
}

/// Phase 3: Apply candidate tags to nodes in the gbuf.
fn applyDecoratorTags(gbuf: *c.cbm_gbuf_t, nodes: [*c]TaggedNode, node_count: c_int, candidates: ?*HashTable) c_int {
    var tagged: c_int = 0;
    for (0..ix(node_count)) |n| {
        var tag_words: [c.CBM_SZ_256]MutStr = undefined;
        var tag_count: usize = 0;
        for (0..ix(nodes[n].word_count)) |w| {
            if (c.cbm_ht_get(candidates, nodes[n].words[w]) != null and tag_count < tag_words.len) {
                tag_words[tag_count] = nodes[n].words[w];
                tag_count += 1;
            }
        }
        if (tag_count == 0) continue;
        if (tag_count > 1) {
            c.qsort(&tag_words, tag_count, @sizeOf(MutStr), cmpStr);
        }

        const gn: *const Node = @as(?*const Node, c.cbm_gbuf_find_by_qn(gbuf, nodes[n].qualified_name)) orelse continue;

        const props: CStr = if (gn.properties_json != null) gn.properties_json else "{}";
        const new_props = injectDecoratorTags(props, tag_words[0..tag_count]);
        if (new_props != null) {
            _ = c.cbm_gbuf_upsert_node(gbuf, gn.label, gn.name, gn.qualified_name, gn.file_path, gn.start_line, gn.end_line, new_props);
            c.free(new_props);
            tagged += 1;
        }
    }
    return tagged;
}

pub export fn cbm_pipeline_pass_decorator_tags(gbuf_c: ?*c.cbm_gbuf_t, project: CStr) c_int {
    const gbuf = gbuf_c orelse return 0;
    if (project == null) return 0;

    const word_counts = c.cbm_ht_create(c.CBM_SZ_128);
    var nodes: [*c]TaggedNode = null;
    const node_count = collectDecoratedNodes(gbuf, &nodes, word_counts);
    if (node_count == 0) {
        c.cbm_ht_free(word_counts);
        c.free(nodes);
        return 0;
    }

    // Phase 2: Determine candidates (words on 2+ nodes)
    const candidates = c.cbm_ht_create(c.CBM_SZ_64);
    var candidate_count: c_int = 0;
    for (0..ix(node_count)) |n| {
        for (0..ix(nodes[n].word_count)) |w| {
            const word: CStr = nodes[n].words[w];
            const cnt = ptrToInt(c.cbm_ht_get(word_counts, word));
            if (cnt >= 2 and c.cbm_ht_get(candidates, word) == null) {
                _ = c.cbm_ht_set(candidates, word, intToPtr(1));
                candidate_count += 1;
            }
        }
    }

    var tagged: c_int = 0;
    if (candidate_count > 0) {
        tagged = applyDecoratorTags(gbuf, nodes, node_count, candidates);
    }

    freeTaggedNodes(nodes, node_count);
    c.cbm_ht_free(word_counts);
    c.cbm_ht_free(candidates);

    interop.logInfo("pass.decorator_tags", .{
        @as(CStr, "candidates"), @as(CStr, if (candidate_count > 0) "yes" else "0"),
        @as(CStr, "tagged"),     @as(CStr, if (tagged > 0) "yes" else "0"),
    });
    return tagged;
}
