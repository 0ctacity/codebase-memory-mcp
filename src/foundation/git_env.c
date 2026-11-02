/*
 * git_env.c — child environment for cbm's git spawns. See git_env.h.
 */
#include "foundation/constants.h"
#include "foundation/git_env.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
extern char **environ;
#endif

const char *const cbm_git_repo_env_vars[CBM_GIT_REPO_ENV_VAR_COUNT] = {
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_CONFIG",
    "GIT_CONFIG_PARAMETERS",
    "GIT_CONFIG_COUNT",
    "GIT_OBJECT_DIRECTORY",
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_GRAFT_FILE",
    "GIT_INDEX_FILE",
    "GIT_NO_REPLACE_OBJECTS",
    "GIT_REPLACE_REF_BASE",
    "GIT_PREFIX",
    "GIT_SHALLOW_FILE",
    "GIT_COMMON_DIR",
};

static char git_env_fold(char c) {
#ifdef _WIN32
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
#else
    return c;
#endif
}

/* Do the first `name_len` chars of `name` equal `var`? */
static bool git_env_name_equals(const char *name, size_t name_len, const char *var) {
    size_t var_len = strlen(var);
    if (name_len != var_len) {
        return false;
    }
    for (size_t i = 0; i < var_len; i++) {
        if (git_env_fold(name[i]) != var[i]) {
            return false;
        }
    }
    return true;
}

static bool git_env_name_is_repo_local(const char *name, size_t name_len) {
    for (size_t i = 0; i < CBM_GIT_REPO_ENV_VAR_COUNT; i++) {
        if (git_env_name_equals(name, name_len, cbm_git_repo_env_vars[i])) {
            return true;
        }
    }
    return false;
}

bool cbm_git_env_entry_is_repo_local(const char *entry) {
    if (!entry) {
        return false;
    }
    const char *eq = strchr(entry, '=');
    size_t name_len = eq ? (size_t)(eq - entry) : strlen(entry);
    return git_env_name_is_repo_local(entry, name_len);
}

#ifdef _WIN32

enum { GIT_ENV_ASCII_MAX = 0x7F };

/* Is a wide "NAME=value" entry repository-local? The names we match are
 * ASCII, so any non-ASCII code unit is a mismatch. Windows keeps per-drive
 * working directories as "=C:=C:\\dir"; the separator search starts at index 1
 * so those entries are kept. */
static bool git_env_wide_entry_is_repo_local(const wchar_t *entry) {
    char name[CBM_SZ_64] = {0};
    size_t n = 0;
    for (size_t i = 0; entry[i] && (i == 0 || entry[i] != L'='); i++) {
        if (entry[i] > GIT_ENV_ASCII_MAX || n + SKIP_ONE >= sizeof(name)) {
            return false;
        }
        name[n++] = (char)entry[i];
    }
    return git_env_name_is_repo_local(name, n);
}

wchar_t *cbm_git_child_env_block(void) {
    wchar_t *current = GetEnvironmentStringsW();
    if (!current) {
        return NULL;
    }
    size_t kept = 0;
    for (const wchar_t *e = current; *e; e += wcslen(e) + 1) {
        if (!git_env_wide_entry_is_repo_local(e)) {
            kept += wcslen(e) + SKIP_ONE;
        }
    }
    /* PAIR_LEN: a block that keeps nothing still needs its double terminator. */
    wchar_t *block = (wchar_t *)malloc((kept + PAIR_LEN) * sizeof(wchar_t));
    if (!block) {
        FreeEnvironmentStringsW(current);
        return NULL;
    }
    size_t pos = 0;
    for (const wchar_t *e = current; *e; e += wcslen(e) + 1) {
        if (!git_env_wide_entry_is_repo_local(e)) {
            size_t len = wcslen(e) + SKIP_ONE;
            memcpy(block + pos, e, len * sizeof(wchar_t));
            pos += len;
        }
    }
    block[pos] = L'\0';
    if (pos == 0) {
        block[SKIP_ONE] = L'\0';
    }
    FreeEnvironmentStringsW(current);
    return block;
}

#else

char **cbm_git_child_envp(void) {
    size_t count = 0;
    size_t bytes = 0;
    for (char **e = environ; e && *e; e++) {
        if (!cbm_git_env_entry_is_repo_local(*e)) {
            count++;
            bytes += strlen(*e) + SKIP_ONE;
        }
    }
    size_t table = (count + SKIP_ONE) * sizeof(char *);
    char **envp = (char **)malloc(table + bytes);
    if (!envp) {
        return NULL;
    }
    char *strings = (char *)envp + table;
    size_t slot = 0;
    for (char **e = environ; e && *e && slot < count; e++) {
        if (!cbm_git_env_entry_is_repo_local(*e)) {
            size_t len = strlen(*e) + SKIP_ONE;
            memcpy(strings, *e, len);
            envp[slot++] = strings;
            strings += len;
        }
    }
    envp[slot] = NULL;
    return envp;
}

#endif

void cbm_git_child_env_free(void *env) {
    free(env);
}
