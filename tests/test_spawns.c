/*
 * test_spawns.c — SPAWNS edges: a call that starts another program, to the
 * Process node of that program (internal/cbm/spawn_patterns.h,
 * src/pipeline/pass_spawns.c).
 *
 * Unit tests pin how a call is recognised (spellings, the file's imports,
 * bare names only when nothing in the project answers) and which program it
 * starts (a literal, a collection's first element, never an argument after a
 * variable program). Pipeline tests pin the edges in an indexed repo and that
 * the sequential and parallel resolvers emit the same ones.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <foundation/compat_fs.h>
#include <mcp/mcp.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <sqlite3.h>
#include "spawn_patterns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Recognition ─────────────────────────────────────────────────── */

static const char *sp_api(CBMLanguage lang, const char *callee, const CBMImport *imports,
                          int count) {
    const cbm_spawn_api_t *api = cbm_spawn_match(lang, callee, imports, count);
    return api ? api->api : NULL;
}

TEST(spawn_match_through_python_imports) {
    const CBMImport imports[] = {{.local_name = "sp", .module_path = "subprocess"},
                                 {.local_name = "run", .module_path = "subprocess.run"},
                                 {.local_name = "os", .module_path = "os"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_PYTHON, "sp.check_output", imports, 3),
                  "subprocess.check_output");
    ASSERT_STR_EQ(sp_api(CBM_LANG_PYTHON, "run", imports, 3), "subprocess.run");
    ASSERT_STR_EQ(sp_api(CBM_LANG_PYTHON, "subprocess.Popen", NULL, 0), "subprocess.Popen");
    ASSERT_STR_EQ(sp_api(CBM_LANG_PYTHON, "os.spawnv", imports, 3), "os.spawnv");
    /* A bare `run` is not subprocess.run without the import that says so. */
    ASSERT_NULL(sp_api(CBM_LANG_PYTHON, "run", NULL, 0));
    ASSERT_NULL(sp_api(CBM_LANG_PYTHON, "system", NULL, 0));
    PASS();
}

TEST(spawn_match_through_go_rust_js_imports) {
    const CBMImport go[] = {{.local_name = "exec", .module_path = "os/exec"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_GO, "exec.Command", go, 1), "os/exec.Command");
    ASSERT_STR_EQ(sp_api(CBM_LANG_GO, "exec.CommandContext", go, 1), "os/exec.CommandContext");
    ASSERT_NULL(sp_api(CBM_LANG_GO, "exec.Command", NULL, 0));

    const CBMImport rust[] = {{.local_name = "Command", .module_path = "std::process::Command"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_RUST, "Command::new", rust, 1), "std::process::Command::new");
    ASSERT_NULL(sp_api(CBM_LANG_RUST, "Command::new", NULL, 0));

    /* `import { spawn } from "node:child_process"`, `const cp = require(..)`,
     * and a destructured require that records only the module. */
    const CBMImport named[] = {{.local_name = "spawn", .module_path = "node:child_process"},
                               {.local_name = "cp", .module_path = "child_process"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_TYPESCRIPT, "spawn", named, 2), "child_process.spawn");
    ASSERT_STR_EQ(sp_api(CBM_LANG_JAVASCRIPT, "cp.execSync", named, 2), "child_process.execSync");
    const CBMImport destructured[] = {
        {.local_name = "child_process", .module_path = "child_process"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_JAVASCRIPT, "execFile", destructured, 1), "execFile");
    ASSERT_NULL(sp_api(CBM_LANG_JAVASCRIPT, "execFile", NULL, 0));
    PASS();
}

/* Bare names: libc-style ones by spelling (a definition of the name in the
 * calling module still wins, see spawns_in_the_graph), import-gated ones only
 * with their import. */
TEST(spawn_match_bare_names) {
    ASSERT_STR_EQ(sp_api(CBM_LANG_C, "system", NULL, 0), "system");
    ASSERT_STR_EQ(sp_api(CBM_LANG_CPP, "std::system", NULL, 0), "std::system");
    ASSERT_STR_EQ(sp_api(CBM_LANG_RUBY, "spawn", NULL, 0), "spawn");
    const CBMImport hs[] = {{.local_name = "Process", .module_path = "System.Process"}};
    ASSERT_STR_EQ(sp_api(CBM_LANG_HASKELL, "callProcess", hs, 1), "callProcess");
    ASSERT_NULL(sp_api(CBM_LANG_HASKELL, "callProcess", NULL, 0));
    ASSERT_NULL(sp_api(CBM_LANG_GO, "system", NULL, 0));
    ASSERT_NULL(sp_api(CBM_LANG_C, "systemd_notify", NULL, 0));
    PASS();
}

/* ── The program ─────────────────────────────────────────────────── */

/* Program of `callee(arg0, arg1)` as the extractor records it. */
static const char *sp_program(CBMLanguage lang, const char *callee, const char *arg0,
                              const char *arg1, const CBMImport *imports, int count, char *buf) {
    CBMCall call = {0};
    call.callee_name = callee;
    if (arg0) {
        call.args[0].expr = arg0;
        call.args[0].index = 0;
        call.arg_count = 1;
    }
    if (arg1) {
        call.args[1].expr = arg1;
        call.args[1].index = 1;
        call.arg_count = 2;
    }
    const cbm_spawn_api_t *api = cbm_spawn_match(lang, callee, imports, count);
    if (!api) {
        return "(no match)";
    }
    cbm_spawn_program(api, lang, &call, buf, CBM_SPAWN_PROGRAM_MAX);
    return buf;
}

TEST(spawn_program_rules) {
    char b[CBM_SPAWN_PROGRAM_MAX];
    const CBMLanguage py = CBM_LANG_PYTHON;
    ASSERT_STR_EQ(sp_program(py, "subprocess.run", "[\"git\", \"status\"]", NULL, NULL, 0, b),
                  "git");
    /* The program is the variable; the arguments after it are not. */
    ASSERT_STR_EQ(sp_program(py, "subprocess.run", "[binary, \"--version\"]", NULL, NULL, 0, b),
                  CBM_SPAWN_DYNAMIC);
    ASSERT_STR_EQ(sp_program(py, "subprocess.Popen", "record[\"argv\"]", NULL, NULL, 0, b),
                  CBM_SPAWN_DYNAMIC);
    ASSERT_STR_EQ(sp_program(py, "subprocess.run", "os.path.join(\"bin\", tool)", NULL, NULL, 0, b),
                  CBM_SPAWN_DYNAMIC);
    ASSERT_STR_EQ(sp_program(py, "subprocess.run", "f\"{tool} --x\"", NULL, NULL, 0, b),
                  CBM_SPAWN_DYNAMIC);
    ASSERT_STR_EQ(sp_program(py, "subprocess.run", "args=[\"make\", \"all\"]", NULL, NULL, 0, b),
                  "make");
    ASSERT_STR_EQ(sp_program(py, "os.system", "\"rm -rf build\"", NULL, NULL, 0, b), "rm");
    /* Environment prefixes and directories are not the program. */
    ASSERT_STR_EQ(sp_program(CBM_LANG_C, "popen", "\"LC_ALL=C ps -eo pid\"", "\"r\"", NULL, 0, b),
                  "ps");
    ASSERT_STR_EQ(
        sp_program(CBM_LANG_C, "system", "\"/usr/bin/env FOO=1 make all\"", NULL, NULL, 0, b),
        "make");
    /* posix_spawnp names the program second; CreateProcess in its command
     * line when the application name is NULL. */
    ASSERT_STR_EQ(sp_program(CBM_LANG_C, "posix_spawnp", "&pid", "\"git\"", NULL, 0, b), "git");
    ASSERT_STR_EQ(sp_program(CBM_LANG_C, "CreateProcessW", "NULL", "L\"cmd /c dir\"", NULL, 0, b),
                  "cmd");
    ASSERT_STR_EQ(sp_program(CBM_LANG_C, "execvp", "argv[0]", "argv", NULL, 0, b),
                  CBM_SPAWN_DYNAMIC);
    ASSERT_STR_EQ(
        sp_program(CBM_LANG_KOTLIN, "ProcessBuilder", "listOf(\"git\", \"log\")", NULL, NULL, 0, b),
        "git");
    ASSERT_STR_EQ(sp_program(CBM_LANG_SWIFT, "Process.launchedProcess", "launchPath: \"/bin/ls\"",
                             "arguments: []", NULL, 0, b),
                  "ls");
    /* Options tuples / structs: the program is the literal inside. */
    ASSERT_STR_EQ(sp_program(CBM_LANG_ELIXIR, "Port.open", "{:spawn, \"cat\"}", "[]", NULL, 0, b),
                  "cat");
    /* Groovy: the receiver is the command line. */
    ASSERT_STR_EQ(sp_program(CBM_LANG_GROOVY, "\"git status\".execute", NULL, NULL, NULL, 0, b),
                  "git");
    PASS();
}

/* ── In the graph ────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *content;
} sp_file_t;

typedef struct {
    char tmpdir[256];
    char *project;
    cbm_mcp_server_t *srv;
    cbm_store_t *store;
} sp_proj_t;

static bool sp_write(const char *root, const char *name, const char *content) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    char *slash = strrchr(path, '/');
    if (slash && slash > path + strlen(root)) {
        *slash = '\0';
        cbm_mkdir_p(path, 0755);
        *slash = '/';
    }
    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fputs(content, f) >= 0;
    return fclose(f) == 0 && ok;
}

/* Index `files`, plus `pad` trivial Python files (enough of them moves the
 * index onto the parallel resolver, MIN_FILES_FOR_PARALLEL). */
static bool sp_index(sp_proj_t *p, const sp_file_t *files, int count, int pad) {
    memset(p, 0, sizeof(*p));
    snprintf(p->tmpdir, sizeof(p->tmpdir), "%s", th_mktempdir("cbm-sp"));
    if (!p->tmpdir[0]) {
        return false;
    }
    for (char *c = p->tmpdir; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    for (int i = 0; i < count; i++) {
        if (!sp_write(p->tmpdir, files[i].name, files[i].content)) {
            return false;
        }
    }
    for (int i = 0; i < pad; i++) {
        char name[64];
        char body[96];
        snprintf(name, sizeof(name), "pad/p%d.py", i);
        snprintf(body, sizeof(body), "def pad_%d():\n    return %d\n", i, i);
        if (!sp_write(p->tmpdir, name, body)) {
            return false;
        }
    }
    p->project = cbm_project_name_from_path(p->tmpdir);
    if (!p->project) {
        return false;
    }
    th_remove_project_workspace(p->project);
    p->srv = cbm_mcp_server_new();
    if (!p->srv) {
        return false;
    }
    char args[800];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", p->tmpdir);
    char *resp = cbm_mcp_handle_tool(p->srv, "index_repository", args);
    if (resp) {
        free(resp);
    }
    p->store = th_open_project_store(p->project);
    return p->store != NULL;
}

static void sp_cleanup(sp_proj_t *p) {
    if (p->store) {
        cbm_store_close(p->store);
    }
    if (p->srv) {
        cbm_mcp_server_free(p->srv);
    }
    if (p->project) {
        th_remove_project_workspace(p->project);
        free(p->project);
    }
    if (p->tmpdir[0]) {
        th_rmtree(p->tmpdir);
    }
    memset(p, 0, sizeof(*p));
}

/* SPAWNS edges from the definition named `caller` to the Process `program`. */
static int sp_spawns(sp_proj_t *p, const char *caller, const char *program) {
    if (!p || !p->store) return -1;
    cbm_node_t *callers = NULL;
    int caller_count = 0;
    if (cbm_store_find_nodes_by_name(p->store, p->project, caller, &callers, &caller_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < caller_count; i++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (cbm_store_find_edges_by_source_type(p->store, callers[i].id, "SPAWNS", &edges, &edge_count) == CBM_STORE_OK) {
            for (int j = 0; j < edge_count; j++) {
                cbm_node_t target = {0};
                if (cbm_store_find_node_by_id(p->store, edges[j].target_id, &target) == CBM_STORE_OK) {
                    if (target.name && strcmp(target.name, program) == 0 &&
                        target.label && strcmp(target.label, "Process") == 0) {
                        matches++;
                    }
                    cbm_node_free_fields(&target);
                }
            }
            cbm_store_free_edges(edges, edge_count);
        }
    }
    cbm_store_free_nodes(callers, caller_count);
    return matches;
}

static int sp_calls(sp_proj_t *p, const char *caller, const char *callee) {
    if (!p || !p->store) return -1;
    cbm_node_t *callers = NULL;
    int caller_count = 0;
    if (cbm_store_find_nodes_by_name(p->store, p->project, caller, &callers, &caller_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < caller_count; i++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (cbm_store_find_edges_by_source_type(p->store, callers[i].id, "CALLS", &edges, &edge_count) == CBM_STORE_OK) {
            for (int j = 0; j < edge_count; j++) {
                cbm_node_t target = {0};
                if (cbm_store_find_node_by_id(p->store, edges[j].target_id, &target) == CBM_STORE_OK) {
                    if (target.name && strcmp(target.name, callee) == 0) {
                        matches++;
                    }
                    cbm_node_free_fields(&target);
                }
            }
            cbm_store_free_edges(edges, edge_count);
        }
    }
    cbm_store_free_nodes(callers, caller_count);
    return matches;
}

static int sp_spawns_from(sp_proj_t *p, const char *caller) {
    if (!p || !p->store) return -1;
    cbm_node_t *callers = NULL;
    int caller_count = 0;
    if (cbm_store_find_nodes_by_name(p->store, p->project, caller, &callers, &caller_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < caller_count; i++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (cbm_store_find_edges_by_source_type(p->store, callers[i].id, "SPAWNS", &edges, &edge_count) == CBM_STORE_OK) {
            matches += edge_count;
            cbm_store_free_edges(edges, edge_count);
        }
    }
    cbm_store_free_nodes(callers, caller_count);
    return matches;
}

static bool sp_has_process_node(sp_proj_t *p, const char *name, const char *qualified_name) {
    if (!p || !p->store) return false;
    cbm_node_t node = {0};
    if (cbm_store_find_node_by_qn(p->store, p->project, qualified_name, &node) != CBM_STORE_OK) {
        return false;
    }
    bool ok = (node.label && strcmp(node.label, "Process") == 0) &&
              (node.name && strcmp(node.name, name) == 0);
    cbm_node_free_fields(&node);
    return ok;
}

static int sp_edge_cmp(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

/* Every SPAWNS edge as "caller>program|api" lines, sorted, into `out`. */
static bool sp_edge_set(sp_proj_t *p, char *out, size_t n) {
    if (!p || !p->store) return false;
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(p->store, p->project, "SPAWNS", &edges, &edge_count) != CBM_STORE_OK) {
        return false;
    }
    char **lines = calloc((size_t)(edge_count > 0 ? edge_count : 1), sizeof(char *));
    int valid_lines = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t src = {0};
        cbm_node_t tgt = {0};
        if (cbm_store_find_node_by_id(p->store, edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(p->store, edges[i].target_id, &tgt) == CBM_STORE_OK) {
            char api[128] = "";
            if (edges[i].properties_json) {
                const char *pos = strstr(edges[i].properties_json, "\"api\":\"");
                if (pos) {
                    pos += 7;
                    const char *end = strchr(pos, '"');
                    if (end && (size_t)(end - pos) < sizeof(api)) {
                        memcpy(api, pos, (size_t)(end - pos));
                        api[end - pos] = '\0';
                    }
                }
            }
            char line[512];
            snprintf(line, sizeof(line), "%s>%s|%s", src.name ? src.name : "?", tgt.name ? tgt.name : "?", api);
            lines[valid_lines++] = strdup(line);
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    cbm_store_free_edges(edges, edge_count);

    qsort(lines, (size_t)valid_lines, sizeof(char *), sp_edge_cmp);

    out[0] = '\0';
    for (int i = 0; i < valid_lines; i++) {
        size_t used = strlen(out);
        snprintf(out + used, n - used, "%s\n", lines[i]);
        free(lines[i]);
    }
    free(lines);
    return true;
}

static const sp_file_t SP_FILES[] = {
    /* subprocess.run inside the module's own `def run`: the resolver's
     * suffix guess (same_module) must not make it a call to `run`. */
    {"tools.py", "import subprocess\n\n\ndef run(cmd):\n    return subprocess.run(cmd)\n\n\n"
                 "def build():\n    subprocess.check_output([\"make\", \"all\"])\n"},
    {"native.c", "#include <stdlib.h>\nint clean(void) { return system(\"rm -rf build\"); }\n"},
    /* A project-defined `system` answers the bare name: a call, no spawn. */
    {"own.c", "static int system(const char *c) { return c != 0; }\n"
              "int use_own(void) { return system(\"x\"); }\n"},
    {"deploy.go", "package main\n\nimport \"os/exec\"\n\n"
                  "func deploy() { exec.Command(\"kubectl\", \"apply\").Run() }\n"},
    {"launch.js", "const { spawn } = require(\"child_process\");\n"
                  "function start() { spawn(\"node\", [\"server.js\"]); }\n"},
};
#define SP_FILE_COUNT ((int)(sizeof(SP_FILES) / sizeof(SP_FILES[0])))

TEST(spawns_in_the_graph) {
    sp_proj_t p;
    ASSERT_TRUE(sp_index(&p, SP_FILES, SP_FILE_COUNT, 0));
    ASSERT_EQ(sp_spawns(&p, "build", "make"), 1);
    ASSERT_EQ(sp_spawns(&p, "run", CBM_SPAWN_DYNAMIC), 1);
    ASSERT_EQ(sp_spawns(&p, "clean", "rm"), 1);
    ASSERT_EQ(sp_spawns(&p, "deploy", "kubectl"), 1);
    ASSERT_EQ(sp_spawns(&p, "start", "node"), 1);
    /* The spawn replaces the CALLS edge a guess would have made. */
    ASSERT_EQ(sp_calls(&p, "build", "run"), 0);
    /* A project-defined system() is code, not a process. */
    ASSERT_EQ(sp_calls(&p, "use_own", "system"), 1);
    ASSERT_EQ(sp_spawns_from(&p, "use_own"), 0);
    /* One Process node per program, named and qualified by it. */
    ASSERT_TRUE(sp_has_process_node(&p, "make", "__process__make"));
    ASSERT_TRUE(sp_has_process_node(&p, CBM_SPAWN_DYNAMIC, "__process__<dynamic>"));
    ASSERT_TRUE(sp_has_process_node(&p, "rm", "__process__rm"));
    ASSERT_TRUE(sp_has_process_node(&p, "kubectl", "__process__kubectl"));
    ASSERT_TRUE(sp_has_process_node(&p, "node", "__process__node"));
    sp_cleanup(&p);
    PASS();
}

/* The sequential (few files) and parallel (past MIN_FILES_FOR_PARALLEL)
 * resolvers ask at the same point and emit the same SPAWNS edges. */
TEST(spawns_parallel_matches_sequential) {
    static char seq[4096];
    static char par[4096];
    sp_proj_t p;
    ASSERT_TRUE(sp_index(&p, SP_FILES, SP_FILE_COUNT, 0));
    ASSERT_TRUE(sp_edge_set(&p, seq, sizeof(seq)));
    sp_cleanup(&p);
    ASSERT_TRUE(sp_index(&p, SP_FILES, SP_FILE_COUNT, 60));
    ASSERT_TRUE(sp_edge_set(&p, par, sizeof(par)));
    sp_cleanup(&p);
    ASSERT_TRUE(seq[0] != '\0');
    if (strcmp(seq, par) != 0) {
        fprintf(stderr, "  sequential:\n%s  parallel:\n%s", seq, par);
    }
    ASSERT_STR_EQ(seq, par);
    PASS();
}

SUITE(spawns) {
    RUN_TEST(spawn_match_through_python_imports);
    RUN_TEST(spawn_match_through_go_rust_js_imports);
    RUN_TEST(spawn_match_bare_names);
    RUN_TEST(spawn_program_rules);
    RUN_TEST(spawns_in_the_graph);
    RUN_TEST(spawns_parallel_matches_sequential);
}
