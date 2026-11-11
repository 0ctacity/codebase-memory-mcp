/*
 * main.c — Entry point for codebase-memory-mcp. The program is cbm_main in
 * main.zig; this file only hands it argv and the version.
 *
 * main() stays C for two reasons:
 *   - The test runners link the same Zig library and define their own main(),
 *     so the library cannot define one.
 *   - The release version is a C define: scripts/build.sh passes
 *     -DCBM_VERSION through CFLAGS_EXTRA, and the Windows release links a Zig
 *     library that CI prebuilds without it.
 */
#include "main.h"

#ifndef CBM_VERSION
#define CBM_VERSION "dev"
#endif

int main(int argc, char **argv) {
    return cbm_main(argc, argv, CBM_VERSION);
}
