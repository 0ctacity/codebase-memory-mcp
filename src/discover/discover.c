/*
 * discover.c — Recursive directory walk with filtering.
 *
 * Walks a repository directory tree, applying:
 *   1. Hardcoded directory skip patterns (60+ dirs like .git, node_modules)
 *   2. Hardcoded suffix filters (.pyc, .png, .wasm, etc.)
 *   3. Fast-mode additional filters (docs, examples, lock files, etc.)
 *   4. Gitignore-style pattern matching
 *   5. Language detection for accepted files
 */
#include "discover/discover.h"
#include "cbm.h" // CBMLanguage, CBM_LANG_COUNT, CBM_LANG_JSON

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/hash_table.h"
#include "foundation/platform.h"
#include "git/git_context.h" // cbm_git_ls_files
#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif
#include <ctype.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strdup
#include <sys/stat.h>

int cbm_gitignore_match_result(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir);

/* ── Hardcoded always-skip directories ──────────────────────────── */

static const char *ALWAYS_SKIP_DIRS[] = {
    /* VCS */
    ".git", ".hg", ".svn", ".worktrees",
    /* IDE */
    ".idea", ".vs", ".vscode", ".eclipse", ".claude", ".claude-worktrees", "Antigravity",
    /* Python */
    ".cache", ".eggs", ".env", ".mypy_cache", ".nox", ".pytest_cache", ".ruff_cache", ".tox",
    ".venv", "__pycache__", "env", "htmlcov", "site-packages", "venv",
    /* JS/TS */
    ".npm", ".nyc_output", ".pnpm-store", ".yarn", "bower_components", "coverage", "node_modules",
    ".next", ".nuxt", ".svelte-kit", ".angular", ".turbo", ".parcel-cache", ".docusaurus", ".expo",
    /* Build artifacts */
    "dist", "obj", "Pods", "target", "temp", "tmp", ".terraform", ".serverless", "bazel-bin",
    "bazel-out", "bazel-testlogs",
    /* Language caches */
    ".cargo", ".stack-work", ".dart_tool", "zig-cache", "zig-out", ".metals", ".bloop", ".bsp",
    ".ccls-cache", ".clangd", "elm-stuff", "_opam", ".cpcache", ".shadow-cljs",
    /* Deploy */
    ".vercel", ".netlify", "deploy", "deployed",
    /* Misc */
    ".qdrant_code_embeddings", ".tmp", "vendor", "vendored", NULL};

static const char *FAST_SKIP_DIRS[] = {
    "generated", "gen",           "auto-generated", "fixtures",     "testdata",    "test_data",
    "__tests__", "__mocks__",     "__snapshots__",  "__fixtures__", "__test__",    "docs",
    "doc",       "documentation", "examples",       "example",      "samples",     "sample",
    "assets",    "static",        "public",         "media",        "third_party", "thirdparty",
    "3rdparty",  "external",      "migrations",     "seeds",        "e2e",         "integration",
    "locale",    "locales",       "i18n",           "l10n",         "scripts",     "tools",
    "hack",      "bin",           "build",          "out",          NULL};

/* ── Ignored suffixes ───────────────────────────────── */

static const char *ALWAYS_IGNORED_SUFFIXES[] = {
    ".tmp",    "~",        ".pyc",  ".pyo",   ".o",   ".a",   ".so",  ".dll",
    ".class",  ".png",     ".jpg",  ".jpeg",  ".gif", ".ico", ".bmp", ".tiff",
    ".webp",   ".svg",     ".wasm", ".node",  ".exe", ".bin", ".dat", ".db",
    ".sqlite", ".sqlite3", ".woff", ".woff2", ".ttf", ".eot", ".otf", NULL};

static const char *FAST_IGNORED_SUFFIXES[] = {
    ".zip", ".tar",  ".gz",       ".bz2",  ".xz",  ".rar",    ".7z",      ".jar",
    ".war", ".ear",  ".mp3",      ".mp4",  ".avi", ".mov",    ".wav",     ".flac",
    ".ogg", ".mkv",  ".webm",     ".pdf",  ".doc", ".docx",   ".xls",     ".xlsx",
    ".ppt", ".pptx", ".odt",      ".ods",  ".map", ".min.js", ".min.css", ".pem",
    ".crt", ".key",  ".cer",      ".p12",  ".pb",  ".avro",   ".parquet", ".beam",
    ".elc", ".rlib", ".coverage", ".prof", ".out", ".patch",  ".diff",    NULL};

/* ── Fast-mode skip filenames ─────────────────────── */

static const char *FAST_SKIP_FILENAMES[] = {
    "LICENSE",        "LICENSE.txt",     "LICENSE.md",   "LICENSE-MIT",   "LICENSE-APACHE",
    "LICENCE",        "LICENCE.txt",     "LICENCE.md",   "CHANGELOG",     "CHANGELOG.md",
    "CHANGES.md",     "HISTORY",         "HISTORY.md",   "AUTHORS",       "AUTHORS.md",
    "CONTRIBUTORS",   "CONTRIBUTORS.md", "CODEOWNERS",   "go.sum",        "yarn.lock",
    "pnpm-lock.yaml", "Pipfile.lock",    "poetry.lock",  "Gemfile.lock",  "Cargo.lock",
    "mix.lock",       "flake.lock",      "pubspec.lock", "composer.lock", "package-lock.json",
    "configure",      "Makefile.in",     "config.guess", "config.sub",    NULL};

/* ── Fast-mode substring patterns ───────────────────── */

static const char *FAST_PATTERNS[] = {".d.ts",      ".bundle.", ".chunk.", ".generated.",
                                      ".pb.go",     "_pb2.py",  ".pb2.py", "_grpc.pb.go",
                                      "_string.go", "mock_",    "_mock.",  "_test_helpers.",
                                      ".stories.",  ".spec.",   ".test.",  NULL};

/* ── Ignored JSON filenames ──────────────────────── */

static const char *IGNORED_JSON_FILES[] = {
    "package.json",       "package-lock.json", "tsconfig.json",
    "jsconfig.json",      "composer.json",     "composer.lock",
    "yarn.lock",          "openapi.json",      "swagger.json",
    "jest.config.json",   ".eslintrc.json",    ".prettierrc.json",
    ".babelrc.json",      "tslint.json",       "angular.json",
    "firebase.json",      "renovate.json",     "lerna.json",
    "turbo.json",         ".stylelintrc.json", "pnpm-lock.json",
    "deno.json",          "biome.json",        "devcontainer.json",
    ".devcontainer.json", "launch.json",       "settings.json",
    "extensions.json",    "tasks.json",        NULL};

/* ── Helper: check if string is in NULL-terminated array ─────────── */

static bool str_in_list(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Helper: check if string ends with suffix ────────────── */

static bool ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) {
        return false;
    }
    return strcmp(s + slen - sufflen, suffix) == 0;
}

/* ── Helper: check if string contains substring ───────────── */

static bool str_contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

/* ── Git global excludes resolution ───────────────────────────── */

enum { GIT_TILDE_PREFIX_LEN = 2 }; /* "~/". */

static bool ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void strip_inline_comment(char *s) {
    bool in_quote = false;
    char quote = '\0';
    for (char *p = s; *p; p++) {
        if ((*p == '"' || *p == '\'') && (p == s || p[-1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (*p == quote) {
                in_quote = false;
            }
            continue;
        }
        if (!in_quote && (*p == '#' || *p == ';') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static char *strip_matching_quotes(char *s) {
    size_t len = strlen(s);
    if (len >= CBM_QUOTE_PAIR && ((s[0] == '"' && s[len - SKIP_ONE] == '"') ||
                                  (s[0] == '\'' && s[len - SKIP_ONE] == '\''))) {
        s[len - SKIP_ONE] = '\0';
        return s + SKIP_ONE;
    }
    return s;
}

static bool has_trailing_sep(const char *path) {
    size_t len = strlen(path);
    return len > 0 && (path[len - SKIP_ONE] == '/' || path[len - SKIP_ONE] == '\\');
}

static void path_join(char *out, size_t out_sz, const char *base, const char *rel) {
    if (!out || out_sz == 0) {
        return;
    }
    if (!base || base[0] == '\0') {
        snprintf(out, out_sz, "%s", rel ? rel : "");
    } else if (!rel || rel[0] == '\0') {
        snprintf(out, out_sz, "%s", base);
    } else if (has_trailing_sep(base)) {
        snprintf(out, out_sz, "%s%s", base, rel);
    } else {
        snprintf(out, out_sz, "%s/%s", base, rel);
    }
    cbm_normalize_path_sep(out);
}

static bool expand_git_path(const char *path, char *out, size_t out_sz) {
    if (!path || !path[0] || !out || out_sz == 0) {
        return false;
    }
    char normalized[CBM_SZ_4K];
    snprintf(normalized, sizeof(normalized), "%s", path);
    cbm_normalize_path_sep(normalized);

    if (normalized[0] != '~') {
        snprintf(out, out_sz, "%s", normalized);
        cbm_normalize_path_sep(out);
        return out[0] != '\0';
    }

    if (normalized[1] != '\0' && normalized[1] != '/') {
        return false; /* ~user expansion is intentionally not supported. */
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    if (normalized[1] == '\0') {
        snprintf(out, out_sz, "%s", home);
        cbm_normalize_path_sep(out);
    } else {
        path_join(out, out_sz, home, normalized + GIT_TILDE_PREFIX_LEN);
    }
    return out[0] != '\0';
}

static bool read_core_excludes_file(const char *config_path, char *out, size_t out_sz) {
    FILE *f = cbm_fopen(config_path, "r");
    if (!f) {
        return false;
    }

    bool in_core = false;
    bool found = false;
    char line[CBM_SZ_4K];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_ws(line);
        if (s[0] == '\0' || s[0] == '#' || s[0] == ';') {
            continue;
        }

        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                in_core = false;
                continue;
            }
            *end = '\0';
            in_core = ascii_ieq(trim_ws(s + SKIP_ONE), "core");
            continue;
        }

        if (!in_core) {
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_ws(s);
        char *value = trim_ws(eq + SKIP_ONE);
        strip_inline_comment(value);
        value = strip_matching_quotes(trim_ws(value));

        if (ascii_ieq(key, "excludesfile") && value[0] != '\0' &&
            expand_git_path(value, out, out_sz)) {
            found = true;
        }
    }

    fclose(f);
    return found;
}

static bool resolve_xdg_git_config_dir(char *out, size_t out_sz) {
    char env[CBM_SZ_4K];
    if (cbm_safe_getenv("XDG_CONFIG_HOME", env, sizeof(env), NULL) && env[0] != '\0') {
        snprintf(out, out_sz, "%s", env);
        cbm_normalize_path_sep(out);
        return true;
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    path_join(out, out_sz, home, ".config");
    return out[0] != '\0';
}

static bool resolve_global_excludes_path(char *out, size_t out_sz) {
    char config_path[CBM_SZ_4K];

    const char *home = cbm_get_home_dir();
    if (home && home[0] != '\0') {
        path_join(config_path, sizeof(config_path), home, ".gitconfig");
        if (read_core_excludes_file(config_path, out, out_sz)) {
            return true;
        }
    }

    char xdg_config[CBM_SZ_4K];
    if (resolve_xdg_git_config_dir(xdg_config, sizeof(xdg_config))) {
        path_join(config_path, sizeof(config_path), xdg_config, "git/config");
        if (read_core_excludes_file(config_path, out, out_sz)) {
            return true;
        }
        path_join(out, out_sz, xdg_config, "git/ignore");
        return out[0] != '\0';
    }

    return false;
}

/* ── Public filter functions ─────────────────────── */

bool cbm_should_skip_dir(const char *dirname, cbm_index_mode_t mode) {
    if (!dirname) {
        return false;
    }

    if (str_in_list(dirname, ALWAYS_SKIP_DIRS)) {
        return true;
    }

    /* Fast discovery applies to both MODERATE and FAST — only FULL keeps everything. */
    if (mode != CBM_MODE_FULL) {
        if (str_in_list(dirname, FAST_SKIP_DIRS)) {
            return true;
        }
    }

    return false;
}

bool cbm_has_ignored_suffix(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    for (int i = 0; ALWAYS_IGNORED_SUFFIXES[i]; i++) {
        if (ends_with(filename, ALWAYS_IGNORED_SUFFIXES[i])) {
            return true;
        }
    }

    if (mode != CBM_MODE_FULL) {
        for (int i = 0; FAST_IGNORED_SUFFIXES[i]; i++) {
            if (ends_with(filename, FAST_IGNORED_SUFFIXES[i])) {
                return true;
            }
        }
    }

    return false;
}

bool cbm_should_skip_filename(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    if (mode != CBM_MODE_FULL) {
        if (str_in_list(filename, FAST_SKIP_FILENAMES)) {
            return true;
        }
    }

    return false;
}

bool cbm_matches_fast_pattern(const char *filename, cbm_index_mode_t mode) {
    if (!filename || mode == CBM_MODE_FULL) {
        return false;
    }

    for (int i = 0; FAST_PATTERNS[i]; i++) {
        if (str_contains(filename, FAST_PATTERNS[i])) {
            return true;
        }
    }

    return false;
}

/* ── Dynamic file list ────────────────────────── */

/* Paths git tracks under the discovery root, and every directory above one.
 * git applies ignore rules to untracked paths only, so .gitignore,
 * info/exclude and the global excludes file never hide a tracked path. Both
 * tables stay NULL when the root is not in a git repository or git cannot
 * run, and the ignore rules then apply exactly as before. */
typedef struct {
    char *paths;         /* `git ls-files -z` output; holds the keys of `files` */
    CBMHashTable *files; /* tracked path */
    CBMHashTable *dirs;  /* directory above a tracked path (owned key) */
} discover_tracked_t;

static bool tracked_has_file(const discover_tracked_t *tracked, const char *rel_path) {
    return tracked && tracked->files && cbm_ht_has(tracked->files, rel_path);
}

static bool tracked_has_dir(const discover_tracked_t *tracked, const char *rel_path) {
    return tracked && tracked->dirs && cbm_ht_has(tracked->dirs, rel_path);
}

typedef struct {
    cbm_file_info_t *files;
    int count;
    int capacity;
    /* Directories skipped during the walk (rel paths), so callers can surface
     * which subtrees were dropped (#411). strdup'd; freed by the caller via
     * cbm_discover_free_excluded or internally when not requested. */
    char **excluded;
    int excluded_count;
    int excluded_cap;
    char cache_tree[CBM_SZ_4K];
    /* Set when the walk could not fully materialize (e.g. a walk-stack grow
     * failed on allocation). Callers keep whatever was discovered; the flag
     * distinguishes a complete walk from a partially-degraded one (upstream
     * 03dc9c91, #17). */
    bool failed;
    const discover_tracked_t *tracked;
} file_list_t;

/* Test fault injection for the OOM paths below (same convention as
 * CBM_ZOVA_TEST_FAIL_PHASE): when CBM_DISCOVER_TEST_FAIL_PHASE is set, the
 * NTH call to a checkpoint with that phase name returns failure. The hit
 * counter is tracked PER PHASE (reset whenever the active phase name
 * changes), so a fault aimed at one checkpoint cannot be consumed by hits
 * from another. */
static int discover_test_fail_hit(const char *phase) {
    const char *fault = getenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    if (!fault || strcmp(fault, phase) != 0) {
        return 0;
    }
    static char active_phase[64];
    static long hits;
    if (strcmp(active_phase, phase) != 0) {
        snprintf(active_phase, sizeof(active_phase), "%s", phase);
        hits = 0;
    }
    const char *nth_raw = getenv("CBM_DISCOVER_TEST_FAIL_NTH");
    long nth = nth_raw ? strtol(nth_raw, NULL, 10) : 1;
    if (nth <= 0) {
        nth = 1;
    }
    return ++hits == nth;
}

static void file_list_add_excluded(file_list_t *fl, const char *rel_path) {
    if (!rel_path || rel_path[0] == '\0') {
        return;
    }
    if (fl->excluded_count >= fl->excluded_cap) {
        int new_cap = fl->excluded_cap ? fl->excluded_cap * PAIR_LEN : CBM_SZ_64;
        if (discover_test_fail_hit("excluded_grow")) {
            fl->failed = true;
            return;
        }
        char **grown = realloc(fl->excluded, new_cap * sizeof(char *));
        if (!grown) {
            /* Dropped skip-point record — the walk is no longer complete (#17). */
            fl->failed = true;
            return;
        }
        fl->excluded = grown;
        fl->excluded_cap = new_cap;
    }
    char *copy = strdup(rel_path);
    if (!copy || discover_test_fail_hit("excluded_strdup")) {
        fl->failed = true;
        return;
    }
    fl->excluded[fl->excluded_count++] = copy;
}

static void fl_add(file_list_t *fl, const char *abs_path, const char *rel_path, CBMLanguage lang,
                   int64_t size) {
    if (fl->count >= fl->capacity) {
        int new_cap = fl->capacity ? fl->capacity * PAIR_LEN : CBM_SZ_256;
        if (discover_test_fail_hit("files_grow")) {
            fl->failed = true;
            return;
        }
        cbm_file_info_t *new_files = realloc(fl->files, new_cap * sizeof(cbm_file_info_t));
        if (!new_files) {
            /* Checked bounds growth (#17): on allocation failure the file is
             * dropped and the walk is flagged incomplete instead of silently
             * indexing a partial view as if it were the whole tree. */
            fl->failed = true;
            return;
        }
        fl->files = new_files;
        fl->capacity = new_cap;
    }

    /* Copy BOTH strings BEFORE the slot becomes visible: on a strdup failure
     * the entry is rolled back, so a partially initialized record with a NULL
     * path can never be passed downstream (#17). */
    char *path_copy = strdup(abs_path);
    char *rel_copy = strdup(rel_path);
    if (!path_copy || !rel_copy || discover_test_fail_hit("file_strdup")) {
        free(path_copy);
        free(rel_copy);
        fl->failed = true;
        return;
    }
    cbm_file_info_t *fi = &fl->files[fl->count++];
    fi->path = path_copy;
    fi->rel_path = rel_copy;
    fi->language = lang;
    fi->size = size;
}

/* ── Recursive walk ─────────────────────────────── */

/* Compute path relative to a nested .gitignore's directory.
 * "webapp/src/foo.js" with prefix "webapp" → "src/foo.js". */
static const char *local_rel_path(const char *rel_path, const char *local_prefix) {
    if (!local_prefix || local_prefix[0] == '\0') {
        return rel_path;
    }
    size_t prefix_len = strlen(local_prefix);
    if (strncmp(rel_path, local_prefix, prefix_len) == 0 && rel_path[prefix_len] == '/') {
        return rel_path + prefix_len + SKIP_ONE;
    }
    return rel_path;
}

/* One ignore file on the path from the repository root down to a walk frame.
 * Links form a root->leaf chain; every frame below the directory that owns a
 * matcher borrows a pointer to the deepest link governing it, so a directory
 * without a .gitignore of its own simply shares its parent's link. Links live
 * on the heap (the frame stack is realloc'd and popped) and are owned through
 * the `owned_next` list.
 *
 * Every pattern is anchored to the directory its file came from, so each link
 * carries the offset between that directory and the walk-relative rel_path it
 * is asked about. The two offsets are mirror images and at most one is ever
 * non-empty:
 *   `prefix` — the matcher's directory is at or BELOW the walk root
 *              ("" for the root itself): strip it from rel_path.
 *   `base`   — the matcher's directory is ABOVE the walk root (an enclosing
 *              repository's ignore files, when a git-less subfolder is
 *              indexed): prepend the walk root's path relative to it.
 * Both strings live in the flexible tail, `base` right after `prefix`.
 * (Upstream d3eb7e98 and 92910147.) */
typedef struct gitignore_link {
    const cbm_gitignore_t *gi;
    const struct gitignore_link *parent;
    struct gitignore_link *owned_next;
    const char *base;
    char prefix[];
} gitignore_link_t;

static gitignore_link_t *gitignore_link_new(const cbm_gitignore_t *gi, const char *prefix,
                                            const char *base, const gitignore_link_t *parent,
                                            gitignore_link_t **owned_links) {
    size_t prefix_len = strlen(prefix);
    size_t base_len = strlen(base);
    gitignore_link_t *link = malloc(sizeof(*link) + prefix_len + base_len + PAIR_LEN);
    if (!link) {
        return NULL;
    }
    link->gi = gi;
    link->parent = parent;
    memcpy(link->prefix, prefix, prefix_len);
    link->prefix[prefix_len] = '\0';
    char *base_copy = link->prefix + prefix_len + SKIP_ONE;
    memcpy(base_copy, base, base_len);
    base_copy[base_len] = '\0';
    link->base = base_copy;
    link->owned_next = *owned_links;
    *owned_links = link;
    return link;
}

static void gitignore_links_free(gitignore_link_t *link) {
    while (link) {
        gitignore_link_t *next = link->owned_next;
        free(link);
        link = next;
    }
}

/* Verdict of every ignore file between the repository root and the directory
 * being walked, with git's precedence: the deepest file that has an opinion
 * wins (a lower-level file takes precedence over every higher-level one), and
 * within one file the last matching pattern wins. Returns >0 ignored, <0
 * re-included by a negation, 0 when no file mentions the path. Cost is one
 * match per ignore file on the path. */
static int gitignore_chain_result(const gitignore_link_t *link, const char *rel_path, bool is_dir) {
    char based[CBM_SZ_4K];
    for (; link; link = link->parent) {
        const char *probe = local_rel_path(rel_path, link->prefix);
        if (link->base[0]) {
            /* Matcher from a directory above the walk root: ask it about the
             * path it would see, the one its own rooted patterns are anchored
             * against. */
            int written = snprintf(based, sizeof(based), "%s/%s", link->base, probe);
            if (written < 0 || (size_t)written >= sizeof(based)) {
                continue; /* longer than this matcher can address: no opinion */
            }
            probe = based;
        }
        int verdict = cbm_gitignore_match_result(link->gi, probe, is_dir);
        if (verdict != 0) {
            return verdict;
        }
    }
    return 0;
}

/* Non-negatable safety core: built-in skip dirs that a .cbmignore negation
 * can NEVER un-skip. A repo-committed .cbmignore must not be able to defeat
 * OOM/safety skips: .git holds VCS internals (and the info/exclude sources,
 * #489), node_modules explodes discovery, and the worktree-internal dirs
 * (.worktrees / .claude-worktrees, the worktree entries in ALWAYS_SKIP_DIRS)
 * contain parallel checkouts of the same repo whose indexing would duplicate
 * the whole codebase (#802). */
static bool is_safety_core_dir(const char *name) {
    static const char *const SAFETY_CORE_DIRS[] = {".git", "node_modules", ".worktrees",
                                                   ".claude-worktrees", NULL};
    return str_in_list(name, SAFETY_CORE_DIRS);
}

/* Check if a directory entry should be skipped (hardcoded dirs + gitignore). */

/* The shared cache directory holds every indexed workspace's database and the
 * CBM internal files. A custom CBM_CACHE_DIR may legitimately sit inside a
 * repository — tests do it routinely, and nothing forbids it in production —
 * and walking into it would pull other workspaces' databases and cache state
 * into this project's file list. "cache" is deliberately not on the built-in
 * skip list (it is a common source directory name), so the walk prunes the
 * configured cache tree by absolute path instead.
 *
 * This is the narrow remedy for the boundary concern in #17 ("never traverse
 * the shared CBM cache/database as repository input"): refusing any root that
 * contains the cache was considered and rejected as too blunt — not walking
 * the cache is what the boundary actually asks for. (Upstream 7814dacf.) */
static bool dir_is_cache_tree(const char *abs_path, const char *cache) {
    if (!cache || !cache[0] || !abs_path || !abs_path[0]) {
        return false;
    }
    size_t n = strlen(cache);
    while (n > 1 && (cache[n - 1] == '/' || cache[n - 1] == '\\')) {
        n--;
    }
    if (n == 0 || strncmp(abs_path, cache, n) != 0) {
        return false;
    }
    /* Boundary-aware so "<cache>x" is not treated as living under "<cache>". */
    return abs_path[n] == '\0' || abs_path[n] == '/' || abs_path[n] == '\\';
}

/* What the ignore rules say about a directory. DIR_GIT_IGNORED comes from
 * git's own rules (.gitignore files, info/exclude, the global excludes file),
 * which yield to tracked content; built-in skips and .cbmignore never do. */
typedef enum { DIR_KEEP, DIR_SKIP, DIR_GIT_IGNORED } dir_verdict_t;

static dir_verdict_t directory_verdict(const char *entry_name, const char *rel_path,
                                       const cbm_discover_opts_t *opts,
                                       const gitignore_link_t *ignore_chain,
                                       const cbm_gitignore_t *global_gi,
                                       const cbm_gitignore_t *cbmignore) {
    if (cbm_should_skip_dir(entry_name, opts ? opts->mode : CBM_MODE_FULL)) {
        /* #500: a .cbmignore negation (e.g. "!obj/") whose rule is the last
         * match for this dir un-skips a built-in skip-list dir — except the
         * non-negatable safety core. Fall through so .gitignore/global/local
         * rules still apply to the un-skipped dir. */
        bool unskipped = cbmignore && !is_safety_core_dir(entry_name) &&
                         cbm_gitignore_match_result(cbmignore, rel_path, true) < 0;
        if (!unskipped) {
            return DIR_SKIP;
        }
    }
    bool git_ignored = gitignore_chain_result(ignore_chain, rel_path, true) > 0;
    bool global_ignored = global_gi && cbm_gitignore_matches(global_gi, rel_path, true);
    if (cbmignore) {
        int cbm_result = cbm_gitignore_match_result(cbmignore, rel_path, true);
        if (cbm_result > 0) {
            return DIR_SKIP;
        }
        if (cbm_result < 0) {
            global_ignored = false; /* a .cbmignore negation un-ignores a global match */
        }
    }
    return git_ignored || global_ignored ? DIR_GIT_IGNORED : DIR_KEEP;
}

/* Check if a regular file should be skipped (filters + gitignore + size).
 * git's own rules never hide a tracked file; inside a git-ignored directory
 * (entered only for its tracked content) every untracked file is ignored, as
 * in git. */
static bool should_skip_file(const char *entry_name, const char *rel_path,
                             const cbm_discover_opts_t *opts, const gitignore_link_t *ignore_chain,
                             const cbm_gitignore_t *global_gi, const cbm_gitignore_t *cbmignore,
                             off_t file_size, bool tracked, bool in_ignored_dir) {
    cbm_index_mode_t mode = opts ? opts->mode : CBM_MODE_FULL;
    if (cbm_has_ignored_suffix(entry_name, mode)) {
        return true;
    }
    if (cbm_should_skip_filename(entry_name, mode)) {
        return true;
    }
    if (cbm_matches_fast_pattern(entry_name, mode)) {
        return true;
    }
    if (!tracked && (in_ignored_dir || gitignore_chain_result(ignore_chain, rel_path, false) > 0)) {
        return true;
    }
    bool global_ignored =
        !tracked && global_gi && cbm_gitignore_matches(global_gi, rel_path, false);
    if (cbmignore) {
        int cbm_result = cbm_gitignore_match_result(cbmignore, rel_path, false);
        if (cbm_result > 0) {
            return true;
        }
        if (cbm_result < 0 && global_ignored) {
            global_ignored = false;
        }
    }
    if (opts && opts->max_file_size > 0 && file_size > opts->max_file_size) {
        return true;
    }
    return global_ignored;
}

/* Detect language for a file, handling .m/.cls/.frm disambiguation and JSON filtering. */
static CBMLanguage detect_file_language(const char *entry_name, const char *abs_path) {
    CBMLanguage lang = cbm_language_for_filename(entry_name);
    if (lang == CBM_LANG_COUNT) {
        /* Filename/extension detection failed: fall back to a conservative
         * shebang probe so extensionless scripts get indexed (#1199). Filename
         * detection stays authoritative — this runs only when it returns
         * unknown. */
        return cbm_language_from_shebang(abs_path);
    }
    /* Special: .m files need content-based disambiguation */
    const char *dot = strrchr(entry_name, '.');
    if (dot && strcmp(dot, ".m") == 0) {
        lang = cbm_disambiguate_m(abs_path);
    }
    /* Special: .cls / .frm are shared with VB6 class modules and forms */
    if (dot && strcmp(dot, ".cls") == 0) {
        lang = cbm_disambiguate_cls(abs_path);
    }
    if (dot && strcmp(dot, ".frm") == 0) {
        lang = cbm_disambiguate_frm(abs_path);
    }
    /* Check ignored JSON files */
    if (lang == CBM_LANG_JSON && str_in_list(entry_name, IGNORED_JSON_FILES)) {
        return CBM_LANG_COUNT;
    }
    return lang;
}

/* UTF-8-safe stat: wide API on Windows, regular stat on POSIX. */
static int wide_stat(const char *path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    struct _stat64 wst;
    int ret = _wstat64(wpath, &wst);
    free(wpath);
    if (ret != 0) {
        return CBM_NOT_FOUND;
    }
    st->st_mode = wst.st_mode;
    st->st_size = wst.st_size;
    st->st_mtime = wst.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

/* Stat a path, skipping symlinks (POSIX) and junctions / reparse points
 * (Windows). Returns 0 on success, -1 to skip. Skipping reparse points keeps
 * discovery from walking through a junction that points outside the project
 * root, mirroring the POSIX S_ISLNK skip. */
static int safe_stat(const char *abs_path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(abs_path);
    if (wpath) {
        DWORD attr = GetFileAttributesW(wpath);
        free(wpath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return CBM_NOT_FOUND;
        }
    }
    return wide_stat(abs_path, st);
#else
    if (lstat(abs_path, st) != 0) {
        return CBM_NOT_FOUND;
    }
    if (S_ISLNK(st->st_mode)) {
        return CBM_NOT_FOUND;
    }
    return 0;
#endif
}

/* Process a single regular file entry during directory walk. */
static void walk_dir_process_file(const char *abs_path, const char *rel_path, const char *name,
                                  const cbm_discover_opts_t *opts,
                                  const gitignore_link_t *ignore_chain,
                                  const cbm_gitignore_t *global_gi,
                                  const cbm_gitignore_t *cbmignore, bool in_ignored_dir, off_t size,
                                  file_list_t *out) {
    if (should_skip_file(name, rel_path, opts, ignore_chain, global_gi, cbmignore, size,
                         tracked_has_file(out->tracked, rel_path), in_ignored_dir)) {
        return;
    }
    CBMLanguage lang = detect_file_language(name, abs_path);
    if (lang == CBM_LANG_COUNT) {
        return;
    }
    fl_add(out, abs_path, rel_path, lang, size);
}

typedef struct {
    char dir[CBM_SZ_4K];
    char prefix[CBM_SZ_4K];
    const gitignore_link_t *ignore_chain; /* deepest .gitignore governing this dir */
    bool git_ignored; /* ignored by git's rules, entered only for its tracked content */
} walk_frame_t;
/* Initial capacity only — the stack grows on demand. A single directory can
 * hold more pending sibling frames than any fixed cap (dotnet/runtime has 855
 * subdirs in one JIT regression dir), so a hard cap here means whole-repo
 * discovery failure, not a depth guard (upstream 03dc9c91, #17). */
#define WALK_STACK_CAP 512

typedef struct {
    walk_frame_t *frames;
    int top;
    int cap;
} walk_stack_t;
/* Build abs/rel paths and process one directory entry. */
/* Try to load a nested .gitignore from this directory. Returns owned pointer or
 * NULL. Every directory below the root is probed, whether or not an ancestor
 * already contributed a matcher: git stacks ALL .gitignore files on a path.
 * The root's own .gitignore is loaded by cbm_discover_ex (merged with
 * info/exclude). */
static cbm_gitignore_t *try_load_nested_gitignore(const walk_frame_t *frame) {
    if (frame->prefix[0] == '\0') {
        return NULL;
    }
    char gi_path[CBM_SZ_4K];
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", frame->dir);
    struct stat gi_st;
    if (wide_stat(gi_path, &gi_st) == 0 && S_ISREG(gi_st.st_mode)) {
        return cbm_gitignore_load(gi_path);
    }
    return NULL;
}

/* Push a subdirectory onto the walk stack, inheriting local gitignore context.
 * Grows the stack geometrically; the caller's `parent` must not point into the
 * stack array (walk_dir pops into a local copy). On growth failure the subtree
 * is dropped and the walk is flagged incomplete (#17, upstream 03dc9c91). */
static void walk_push_subdir(walk_stack_t *ws, const char *abs_path, const char *rel_path,
                             const walk_frame_t *parent, bool git_ignored, file_list_t *out) {
    if (ws->top >= ws->cap) {
        int new_cap = ws->cap * 2;
        if (discover_test_fail_hit("stack_grow")) {
            out->failed = true;
            return;
        }
        walk_frame_t *grown = realloc(ws->frames, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            out->failed = true;
            return;
        }
        ws->frames = grown;
        ws->cap = new_cap;
    }
    walk_frame_t *slot = &ws->frames[ws->top];
    int directory_length = snprintf(slot->dir, CBM_SZ_4K, "%s", abs_path);
    int prefix_length = snprintf(slot->prefix, CBM_SZ_4K, "%s", rel_path);
    if (directory_length <= 0 || directory_length >= CBM_SZ_4K || prefix_length < 0 ||
        prefix_length >= CBM_SZ_4K) {
        out->failed = true;
        return;
    }
    slot->ignore_chain = parent->ignore_chain;
    slot->git_ignored = git_ignored;
    ws->top++;
}

static void walk_dir_process_entry(cbm_dirent_t *entry, const walk_frame_t *frame,
                                   const cbm_discover_opts_t *opts,
                                   const cbm_gitignore_t *global_gi,
                                   const cbm_gitignore_t *cbmignore, walk_stack_t *ws,
                                   file_list_t *out) {
    char abs_path[CBM_SZ_4K];
    char rel_path[CBM_SZ_4K];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", frame->dir, entry->name);
    if (frame->prefix[0] != '\0') {
        snprintf(rel_path, sizeof(rel_path), "%s/%s", frame->prefix, entry->name);
    } else {
        snprintf(rel_path, sizeof(rel_path), "%s", entry->name);
    }

    struct stat st;
    if (safe_stat(abs_path, &st) != 0) {
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        dir_verdict_t verdict = dir_is_cache_tree(abs_path, out->cache_tree)
                                    ? DIR_SKIP
                                    : directory_verdict(entry->name, rel_path, opts,
                                                        frame->ignore_chain, global_gi, cbmignore);
        if (verdict == DIR_KEEP && frame->git_ignored) {
            verdict = DIR_GIT_IGNORED; /* below a git-ignored directory, as in git */
        }
        if (verdict == DIR_KEEP ||
            (verdict == DIR_GIT_IGNORED && tracked_has_dir(out->tracked, rel_path))) {
            walk_push_subdir(ws, abs_path, rel_path, frame, verdict == DIR_GIT_IGNORED, out);
        } else {
            /* Record the excluded subtree root so callers can report it (#411). */
            file_list_add_excluded(out, rel_path);
        }
    } else if (S_ISREG(st.st_mode)) {
        walk_dir_process_file(abs_path, rel_path, entry->name, opts, frame->ignore_chain, global_gi,
                              cbmignore, frame->git_ignored, st.st_size, out);
    }
}

/* Append a matcher to an owner array; false (array unchanged) when it cannot
 * grow. Shared by the walk and by the enclosing-repository chain. */
static bool owned_gitignore_append(cbm_gitignore_t ***owned, int *count, int *cap,
                                   cbm_gitignore_t *gi) {
    if (*count == *cap) {
        int new_cap = *cap ? *cap * PAIR_LEN : CBM_SZ_16;
        cbm_gitignore_t **grown = discover_test_fail_hit("gi_owned_grow")
                                      ? NULL
                                      : realloc(*owned, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        *owned = grown;
        *cap = new_cap;
    }
    (*owned)[(*count)++] = gi;
    return true;
}

static void owned_gitignore_free(cbm_gitignore_t **owned, int count) {
    for (int i = 0; i < count; i++) {
        cbm_gitignore_free(owned[i]);
    }
    free(owned);
}

static void walk_dir(const char *dir_path, const char *rel_prefix, const cbm_discover_opts_t *opts,
                     const cbm_gitignore_t *gitignore, const gitignore_link_t *ancestors,
                     const cbm_gitignore_t *global_gi, const cbm_gitignore_t *cbmignore,
                     file_list_t *out) {
    walk_stack_t ws = {
        .frames = discover_test_fail_hit("stack_calloc")
                      ? NULL
                      : calloc(WALK_STACK_CAP, sizeof(walk_frame_t)),
        .top = 0,
        .cap = WALK_STACK_CAP};
    if (!ws.frames) {
        /* Nothing was walked at all — flag so the caller does not mistake an
         * empty result for an empty tree (#17). */
        out->failed = true;
        return;
    }
    /* Collect all owned gitignores and chain links — freed at the end because
     * child frames on the stack hold borrowed pointers to them. */
    cbm_gitignore_t **owned_gis = NULL;
    int owned_count = 0;
    int owned_cap = 0;
    gitignore_link_t *owned_links = NULL;

    int initial_directory_length = snprintf(ws.frames[0].dir, CBM_SZ_4K, "%s", dir_path);
    int initial_prefix_length = snprintf(ws.frames[0].prefix, CBM_SZ_4K, "%s", rel_prefix);
    if (initial_directory_length <= 0 || initial_directory_length >= CBM_SZ_4K ||
        initial_prefix_length < 0 || initial_prefix_length >= CBM_SZ_4K) {
        out->failed = true;
        free(ws.frames);
        return;
    }
    /* `ancestors` is the deepest link of the enclosing repository's chain (or
     * NULL); the walk root's own matcher sits below it so it still wins. */
    ws.frames[0].ignore_chain = ancestors;
    if (gitignore) {
        ws.frames[0].ignore_chain =
            gitignore_link_new(gitignore, rel_prefix, "", ancestors, &owned_links);
        if (!ws.frames[0].ignore_chain) {
            out->failed = true;
            free(ws.frames);
            return;
        }
    }
    ws.top++;

    while (ws.top > 0) {
        walk_frame_t frame = ws.frames[--ws.top];

        cbm_gitignore_t *loaded = try_load_nested_gitignore(&frame);
        if (loaded) {
            if (!owned_gitignore_append(&owned_gis, &owned_count, &owned_cap, loaded)) {
                /* Walking on without these rules would index files they
                 * exclude, so drop the subtree and flag the walk (#17). */
                cbm_gitignore_free(loaded);
                out->failed = true;
                continue;
            }
            /* owned_gis owns `loaded` from here on, even if the link fails. */
            const gitignore_link_t *link =
                gitignore_link_new(loaded, frame.prefix, "", frame.ignore_chain, &owned_links);
            if (!link) {
                out->failed = true;
                continue;
            }
            frame.ignore_chain = link;
        }

        cbm_dir_t *d = cbm_opendir(frame.dir);
        if (!d) {
            continue;
        }

        cbm_dirent_t *entry;
        while ((entry = cbm_readdir(d)) != NULL) {
            walk_dir_process_entry(entry, &frame, opts, global_gi, cbmignore, &ws, out);
        }
        cbm_closedir(d);
    }
    owned_gitignore_free(owned_gis, owned_count);
    gitignore_links_free(owned_links);
    free(ws.frames);
}

/* ── Public API ───────────────────────────────── */

static bool discover_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

/* Resolve the shared "common" git directory for repo_path.
 * Handles three layouts:
 *   1. <repo>/.git is a directory    - ordinary repo; common_dir == <repo>/.git
 *   2. <repo>/.git is a regular file - linked worktree gitlink "gitdir: <path>";
 *      the common dir is read from <git_dir>/commondir (git stores info/exclude +
 *      config there, shared across worktrees). Falls back to git_dir when no
 *      commondir file exists.
 *   3. neither - not a git repo.
 * Returns true when a git dir was resolved. Fixes the worktree case where
 * .git/info/exclude and core.excludesfile were silently dropped because the old
 * check required .git to be a directory (issue #489 only covered ordinary repos). */
static bool resolve_git_common_dir(const char *repo_path, char *common_dir, size_t cd_sz) {
    char dot_git[CBM_SZ_4K];
    snprintf(dot_git, sizeof(dot_git), "%s/.git", repo_path);
    struct stat st;
    if (wide_stat(dot_git, &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(common_dir, cd_sz, "%s", dot_git);
        cbm_normalize_path_sep(common_dir);
        return true;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }

    /* Linked worktree: parse "gitdir: <path>" from the gitlink file.
     * cbm_fopen (not raw fopen) so non-ASCII repo paths open on Windows. */
    FILE *f = cbm_fopen(dot_git, "r");
    if (!f) {
        return false;
    }
    char git_dir[CBM_SZ_4K];
    bool got_git_dir = false;
    char line[CBM_SZ_4K];
    while (fgets(line, sizeof(line), f)) {
        char *gs = trim_ws(line);
        if (strncmp(gs, "gitdir:", 7) == 0) {
            char *val = trim_ws(gs + 7);
            if (val[0] != '\0') {
                if (discover_path_is_absolute(val)) {
                    snprintf(git_dir, sizeof(git_dir), "%s", val);
                    cbm_normalize_path_sep(git_dir);
                } else {
                    path_join(git_dir, sizeof(git_dir), repo_path, val);
                }
                got_git_dir = true;
            }
            break;
        }
    }
    fclose(f);
    if (!got_git_dir) {
        return false;
    }

    /* The shared dir holding info/exclude + config is named in <git_dir>/commondir
     * (typically a relative path like "../.."). Absent in single-worktree gitdirs. */
    char commondir_path[CBM_SZ_4K];
    path_join(commondir_path, sizeof(commondir_path), git_dir, "commondir");
    FILE *cf = cbm_fopen(commondir_path, "r");
    if (cf) {
        char cbuf[CBM_SZ_4K];
        bool resolved = false;
        if (fgets(cbuf, sizeof(cbuf), cf)) {
            char *cs = trim_ws(cbuf);
            if (cs[0] != '\0') {
                if (discover_path_is_absolute(cs)) {
                    snprintf(common_dir, cd_sz, "%s", cs);
                    cbm_normalize_path_sep(common_dir);
                } else {
                    path_join(common_dir, cd_sz, git_dir, cs);
                }
                resolved = true;
            }
        }
        fclose(cf);
        if (resolved) {
            return true;
        }
    }

    snprintf(common_dir, cd_sz, "%s", git_dir);
    cbm_normalize_path_sep(common_dir);
    return true;
}

/* When repo_path itself carries no .git (resolve_git_common_dir already
 * returned false for it), walk upward looking for an enclosing repository, as
 * `git` itself does when run from a subfolder. Bounded by the filesystem or
 * drive root: dir is truncated at each iteration, so the loop cannot run more
 * times than repo_path is characters long. Returns true and fills
 * ancestor_root (for the enclosing repository's own .gitignore files) plus
 * common_dir (for its info/exclude and config). (Upstream 92910147.) */
static bool resolve_enclosing_git_root(const char *repo_path, char *ancestor_root, size_t ar_sz,
                                       char *common_dir, size_t cd_sz) {
    char dir[CBM_SZ_4K];
    (void)snprintf(dir, sizeof(dir), "%s", repo_path);
    cbm_normalize_path_sep(dir);

    for (;;) {
        char *slash = strrchr(dir, '/');
        /* No separator left, or only the root separator (POSIX "/") or a
         * bare drive prefix (Windows "C:/"): nothing above this to check. */
        if (!slash || slash == dir || (slash > dir && *(slash - SKIP_ONE) == ':')) {
            return false;
        }
        *slash = '\0';
        if (resolve_git_common_dir(dir, common_dir, cd_sz)) {
            (void)snprintf(ancestor_root, ar_sz, "%s", dir);
            return true;
        }
    }
}

/* The enclosing repository's ignore files, kept OUT of the indexed directory's
 * own matcher. Merging them in would re-anchor every rooted pattern onto the
 * indexed subfolder: "/secret.py" at the enclosing root would start hiding
 * <subfolder>/secret.py, and "pkg/scratch/" would stop hiding pkg/scratch.
 * Each matcher instead keeps its own directory as its anchor, via the link's
 * `base`. */
typedef struct {
    cbm_gitignore_t **owned;
    int count;
    int cap;
    const gitignore_link_t *chain; /* deepest ancestor link, or NULL */
    gitignore_link_t *links;       /* owner list backing `chain` */
} ancestor_ignores_t;

static void ancestor_ignores_free(ancestor_ignores_t *anc) {
    owned_gitignore_free(anc->owned, anc->count);
    gitignore_links_free(anc->links);
}

/* Add one ancestor directory's matcher to the chain, deepest last so that the
 * deepest file with an opinion wins (git's shallow-to-deep rule, negations
 * included). Takes ownership of `gi`. */
static bool ancestor_ignores_push(ancestor_ignores_t *anc, cbm_gitignore_t *gi, const char *base) {
    if (!owned_gitignore_append(&anc->owned, &anc->count, &anc->cap, gi)) {
        cbm_gitignore_free(gi);
        return false;
    }
    /* owned[] owns `gi` from here on, even if the link fails. */
    const gitignore_link_t *link = gitignore_link_new(gi, "", base, anc->chain, &anc->links);
    if (!link) {
        return false;
    }
    anc->chain = link;
    return true;
}

/* Load the .gitignore of EVERY directory from the enclosing repository root
 * (inclusive) down to the indexed directory (exclusive): git consults all of
 * them, not just the root's. `root_extra` is that repository's
 * <common>/info/exclude, which git anchors at its root like the root
 * .gitignore; ownership is taken. `leaf` must be a canonical path under
 * `root`, both with '/' separators. Returns false only on allocation
 * failure. */
static bool ancestor_ignores_build(ancestor_ignores_t *anc, const char *root, const char *leaf,
                                   cbm_gitignore_t *root_extra) {
    size_t root_len = strlen(root);
    if (strncmp(leaf, root, root_len) != 0) {
        cbm_gitignore_free(root_extra);
        return true;
    }
    const char *base = leaf + root_len;
    while (*base == '/') {
        base++;
    }

    char dir[CBM_SZ_4K];
    int dir_len = snprintf(dir, sizeof(dir), "%s", root);
    if (dir_len < 0 || (size_t)dir_len >= sizeof(dir) || !*base) {
        cbm_gitignore_free(root_extra);
        return true;
    }

    for (;;) {
        char gi_path[CBM_SZ_4K];
        path_join(gi_path, sizeof(gi_path), dir, ".gitignore");
        cbm_gitignore_t *gi = cbm_gitignore_load(gi_path);
        if (root_extra) {
            /* First iteration: `dir` is the enclosing repository root, the one
             * directory info/exclude shares an anchor with. Merged after the
             * .gitignore patterns so it overrides them, the same precedence
             * the two already have for an ordinary repository root. */
            if (!gi) {
                gi = root_extra;
            } else {
                (void)cbm_gitignore_merge(gi, root_extra);
                cbm_gitignore_free(root_extra);
            }
            root_extra = NULL;
        }
        if (gi && !ancestor_ignores_push(anc, gi, base)) {
            return false;
        }
        const char *slash = strchr(base, '/');
        if (!slash) {
            return true; /* the next directory down IS the indexed one */
        }
        int written = snprintf(dir + dir_len, sizeof(dir) - (size_t)dir_len, "/%.*s",
                               (int)(slash - base), base);
        if (written < 0 || (size_t)written >= sizeof(dir) - (size_t)dir_len) {
            return true; /* deeper directories are unaddressable: stop here */
        }
        dir_len += written;
        base = slash + SKIP_ONE;
    }
}

static void tracked_free_key(const char *key, void *value, void *userdata) {
    (void)value;
    (void)userdata;
    free((void *)key);
}

static void tracked_free(discover_tracked_t *tracked) {
    if (tracked->dirs) {
        cbm_ht_foreach(tracked->dirs, tracked_free_key, NULL);
    }
    cbm_ht_free(tracked->dirs);
    cbm_ht_free(tracked->files);
    free(tracked->paths);
    memset(tracked, 0, sizeof(*tracked));
}

/* Record every directory above `path` (its '/'-separated prefixes). */
static bool tracked_add_dirs(CBMHashTable *dirs, const char *path) {
    char prefix[CBM_SZ_4K];
    for (const char *slash = strchr(path, '/'); slash; slash = strchr(slash + SKIP_ONE, '/')) {
        size_t n = (size_t)(slash - path);
        if (n == 0 || n >= sizeof(prefix)) {
            continue;
        }
        memcpy(prefix, path, n);
        prefix[n] = '\0';
        if (cbm_ht_has(dirs, prefix)) {
            continue;
        }
        char *key = strdup(prefix);
        if (!key) {
            return false;
        }
        cbm_ht_set(dirs, key, key);
    }
    return true;
}

/* Load what git tracks under repo_path (`git ls-files -z`, paths relative to
 * it); leaves `tracked` empty on any failure, so the ignore rules then apply
 * exactly as they did without it. (Upstream a90076a3.) */
static void tracked_load(discover_tracked_t *tracked, const char *repo_path) {
    memset(tracked, 0, sizeof(*tracked));
    size_t length = 0;
    tracked->paths = cbm_git_ls_files(repo_path, &length);
    if (!tracked->paths || length == 0) {
        tracked_free(tracked);
        return;
    }
    tracked->files = cbm_ht_create(CBM_SZ_1K);
    tracked->dirs = cbm_ht_create(CBM_SZ_256);
    if (!tracked->files || !tracked->dirs) {
        tracked_free(tracked);
        return;
    }
    for (size_t at = 0; at < length;) {
        const char *path = tracked->paths + at;
        size_t n = strlen(path);
        if (n > 0) {
            cbm_ht_set(tracked->files, path, (void *)path);
            if (!tracked_add_dirs(tracked->dirs, path)) {
                tracked_free(tracked);
                return;
            }
        }
        at += n + SKIP_ONE;
    }
}

int cbm_discover(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                 int *count) {
    return cbm_discover_ex(repo_path, opts, out, count, NULL, NULL, NULL);
}

int cbm_discover_ex(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out,
                    bool *degraded_out) {
    if (excluded_out) {
        *excluded_out = NULL;
    }
    if (excluded_count_out) {
        *excluded_count_out = 0;
    }
    if (degraded_out) {
        *degraded_out = false;
    }
    if (!repo_path || !out || !count) {
        return CBM_NOT_FOUND;
    }

    *out = NULL;
    *count = 0;

    /* Verify directory exists */
    struct stat st;
    if (wide_stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return CBM_NOT_FOUND;
    }

    /* Load gitignore sources for ordinary repos AND linked worktrees.
     * Sources merged in order (later patterns win on conflict):
     *   1. <repo>/.gitignore     — committed exclusions
     *   2. <common>/info/exclude — per-clone exclusions, not committed
     * <common> is the git common dir, resolved via resolve_git_common_dir() so a
     * worktree (where .git is a gitlink file) reads the shared info/exclude/config
     * just like a normal checkout. Both are folded into a single matcher so all
     * downstream call paths remain unchanged. Fixes issue #489: OOM on repos whose
     * worktrees are excluded only via .git/info/exclude (e.g. Sandcastle).
     *
     * An ENCLOSING repository's ignore files (indexing a git-less subfolder)
     * are deliberately NOT folded in here: their patterns are anchored to
     * their own directories, so they stay separate matchers on the chain with
     * a `base` offset. See ancestor_ignores_build(). */
    cbm_gitignore_t *gitignore = NULL;
    char gi_path[CBM_SZ_4K];
    struct stat gi_stat;
    /* Resolve the git common dir, transparently following a worktree gitlink so the
     * .git/info/exclude and core.excludesfile sources are honoured inside linked
     * worktrees too (where .git is a file pointing at the shared dir, not a directory). */
    char git_common_dir[CBM_SZ_4K];
    bool is_git_repo = resolve_git_common_dir(repo_path, git_common_dir, sizeof(git_common_dir));
    bool has_git_config = false;
    /* repo_path itself is not a git repo root: walk upward for an enclosing
     * one, as `git` does when run from a subfolder (upstream 92910147). The
     * walk is lexical, so repo_path is canonicalized first; otherwise a
     * relative and an absolute spelling of one directory would discover
     * different files. */
    ancestor_ignores_t ancestors = {0};
    char enclosing_root[CBM_SZ_4K];
    char canonical_repo[CBM_SZ_4K];
    bool has_enclosing_repo = false;
    if (!is_git_repo) {
#ifdef _WIN32
        if (!_fullpath(canonical_repo, repo_path, sizeof(canonical_repo))) {
#else
        if (!realpath(repo_path, canonical_repo)) {
#endif
            (void)snprintf(canonical_repo, sizeof(canonical_repo), "%s", repo_path);
        }
        cbm_normalize_path_sep(canonical_repo);
        has_enclosing_repo =
            resolve_enclosing_git_root(canonical_repo, enclosing_root, sizeof(enclosing_root),
                                       git_common_dir, sizeof(git_common_dir));
        is_git_repo = has_enclosing_repo;
    }
    /* Always honour the .gitignore at the indexed-directory root, even when the
     * directory is not a git repo root (e.g. indexing a sub-package directly).
     * Fixes issue #510: a root .gitignore was silently ignored without .git/. */
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", repo_path);
    gitignore = cbm_gitignore_load(gi_path);
    if (is_git_repo) {
        path_join(gi_path, sizeof(gi_path), git_common_dir, "config");
        has_git_config = wide_stat(gi_path, &gi_stat) == 0 && S_ISREG(gi_stat.st_mode);

        char exc_path[CBM_SZ_4K];
        path_join(exc_path, sizeof(exc_path), git_common_dir, "info/exclude");
        cbm_gitignore_t *git_exclude = cbm_gitignore_load(exc_path);
        if (has_enclosing_repo) {
            /* git_common_dir belongs to the ENCLOSING repository, so its
             * info/exclude is anchored at that repository's root, not at the
             * indexed subfolder. Ownership passes to the ancestor chain. */
            if (!ancestor_ignores_build(&ancestors, enclosing_root, canonical_repo, git_exclude)) {
                ancestor_ignores_free(&ancestors);
                cbm_gitignore_free(gitignore);
                return CBM_NOT_FOUND;
            }
        } else if (git_exclude) {
            if (!gitignore) {
                gitignore = git_exclude;
            } else {
                /* On allocation failure the merge is atomic (dst unchanged), so
                 * the .gitignore patterns still apply; the exclude patterns are
                 * simply skipped — same as if .git/info/exclude were absent. */
                (void)cbm_gitignore_merge(gitignore, git_exclude);
                cbm_gitignore_free(git_exclude);
            }
        }
    }

    cbm_gitignore_t *global_gi = NULL;
    if (has_git_config && resolve_global_excludes_path(gi_path, sizeof(gi_path))) {
        global_gi = cbm_gitignore_load(gi_path);
    }

    /* Load cbmignore if specified or exists at repo root */
    cbm_gitignore_t *cbmignore = NULL;
    if (opts && opts->ignore_file) {
        cbmignore = cbm_gitignore_load(opts->ignore_file);
    } else {
        snprintf(gi_path, sizeof(gi_path), "%s/.cbmignore", repo_path);
        cbmignore = cbm_gitignore_load(gi_path);
    }

    /* Walk */
    file_list_t fl = {0};
    const char *cache = cbm_resolve_cache_dir();
    if (cache && cache[0]) {
#ifdef _WIN32
        if (!_fullpath(fl.cache_tree, cache, sizeof(fl.cache_tree))) {
            snprintf(fl.cache_tree, sizeof(fl.cache_tree), "%s", cache);
        }
#else
        if (!realpath(cache, fl.cache_tree)) {
            snprintf(fl.cache_tree, sizeof(fl.cache_tree), "%s", cache);
        }
        /* The walk builds paths from repo_path as given. When that path goes
         * through a symlink (macOS /tmp -> /private/tmp), re-express a cache
         * that lives inside the repository in the walk's own spelling. */
        char root_real[CBM_SZ_4K];
        if (realpath(repo_path, root_real) && dir_is_cache_tree(fl.cache_tree, root_real)) {
            char inside[CBM_SZ_4K];
            snprintf(inside, sizeof(inside), "%s", fl.cache_tree + strlen(root_real));
            snprintf(fl.cache_tree, sizeof(fl.cache_tree), "%s%s", repo_path, inside);
        }
#endif
    }
    discover_tracked_t tracked = {0};
    if (is_git_repo) {
        tracked_load(&tracked, repo_path);
        fl.tracked = &tracked;
    }
    walk_dir(repo_path, "", opts, gitignore, ancestors.chain, global_gi, cbmignore, &fl);

    /* Cleanup */
    tracked_free(&tracked);
    ancestor_ignores_free(&ancestors);
    cbm_gitignore_free(gitignore);
    cbm_gitignore_free(global_gi);
    cbm_gitignore_free(cbmignore);

    *out = fl.files;
    *count = fl.count;
    if (degraded_out) {
        *degraded_out = fl.failed;
    }

    /* Hand the excluded-dir list to the caller, or free it if not requested. */
    if (excluded_out) {
        *excluded_out = fl.excluded;
        if (excluded_count_out) {
            *excluded_count_out = fl.excluded_count;
        }
    } else {
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
    }
    return 0;
}

void cbm_discover_free(cbm_file_info_t *files, int count) {
    if (!files) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].rel_path);
    }
    free(files);
}

void cbm_discover_free_excluded(char **excluded, int count) {
    if (!excluded) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(excluded[i]);
    }
    free(excluded);
}
