#!/usr/bin/env bash
# check-build-sources.sh — Fail when build.zig and Makefile.cbm build different
# source files.
#
# build.zig builds the PR CI binaries and test runners; Makefile.cbm builds the
# release binaries and the ASan test runner. Each keeps its own source list, so
# a file added to one and missed in the other only breaks the other build.
#
# Compared groups:
#   prod  C/C++/assembly sources linked into the binary
#   test  test sources linked into the full test runner
#   zig   Zig sources: the files build.zig reaches by @import from its roots,
#         and the files Makefile.cbm lists as Zig library inputs
#
# Usage: bash scripts/check-build-sources.sh   (exit 0 = same sets, 1 = drift)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

# Intentional differences: "<only-in> <group> <path>".
ALLOWED=(
    # Windows regex; build.zig does not target Windows.
    "make prod vendored/tre/tre_all.c"
)

# Quoted strings between "const <name> = ..." and the closing "};".
zig_array() {
    awk -v start="^const $1 = " '$0 ~ start {inb = 1; next} inb && /^};/ {inb = 0} inb' build.zig |
        grep -oE '"[^"]+"' | tr -d '"'
}

# b.path("...") arguments inside "fn <name>(" up to the closing "}".
zig_fn_paths() {
    awk -v start="^fn $1[(]" '$0 ~ start {inb = 1} inb && /^}/ {inb = 0} inb' build.zig |
        grep -oE 'b\.path\("[^"]+"\)' | sed -E 's/^b\.path\("(.*)"\)$/\1/'
}

# Zig files reachable by relative @import from the given roots.
zig_imports() {
    local -a queue=("$@")
    local seen=" " file dir dep path
    while ((${#queue[@]})); do
        file=${queue[0]}
        queue=("${queue[@]:1}")
        [[ $seen == *" $file "* ]] && continue
        seen+="$file "
        printf '%s\n' "$file"
        dir=$(dirname "$file")
        while IFS= read -r dep; do
            path=$(cd "$dir/$(dirname "$dep")" && pwd -P)/$(basename "$dep")
            queue+=("${path#"$ROOT"/}")
        done < <(grep -oE '@import\("[^"]+\.zig"\)' "$file" | sed -E 's/^@import\("(.*)"\)$/\1/')
    done
}

zig_build_sources() {
    {
        zig_fn_paths addCbmExecutable
        zig_fn_paths addNativeSources
        zig_array native_sources
        # grammarSources(): every internal/cbm/grammar_*.c
        ls internal/cbm/grammar_*.c
    } | sed 's/^/prod /'
    zig_array all_test_sources | sed 's/^/test /'
    zig_imports src/cbm_zig.zig "$(zig_fn_paths addZovaBridgeObject | grep '\.zig$')" | sed 's/^/zig /'
}

zig_list=$(zig_build_sources | sort -u)
make_list=$(make -s -f Makefile.cbm print-build-sources | sort -u)
allowed=$(printf '%s\n' "${ALLOWED[@]}")

failures=0
report() { # <only-in> <other build> <lines>
    local side=$1 other=$2 line
    while IFS= read -r line; do
        [[ -z $line ]] && continue
        grep -qxF "$side $line" <<<"$allowed" && continue
        echo "[build-sources] only in $side build, missing from $other: $line"
        failures=$((failures + 1))
    done <<<"$3"
}
report zig "Makefile.cbm" "$(comm -23 <(printf '%s\n' "$zig_list") <(printf '%s\n' "$make_list"))"
report make "build.zig" "$(comm -13 <(printf '%s\n' "$zig_list") <(printf '%s\n' "$make_list"))"

# The focused build.zig runners must only use tests the full runner also has.
all_tests=$(zig_array all_test_sources)
for name in zova_test_sources mcp_test_sources pipeline_test_sources; do
    while IFS= read -r test; do
        grep -qxF "$test" <<<"$all_tests" && continue
        echo "[build-sources] $name in build.zig lists $test, which all_test_sources does not"
        failures=$((failures + 1))
    done < <(zig_array "$name")
done

if ((failures)); then
    echo ""
    echo "[build-sources] FAIL: $failures difference(s). Add each file to both build.zig and Makefile.cbm,"
    echo "  or to ALLOWED in scripts/check-build-sources.sh if the difference is intentional."
    exit 1
fi

echo "[build-sources] OK — build.zig and Makefile.cbm build the same sources"
