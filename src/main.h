/*
 * main.h — The codebase-memory-mcp program, implemented in main.zig.
 */
#ifndef CBM_MAIN_H
#define CBM_MAIN_H

/* Runs the program for argv: a subcommand, a single CLI tool call, or the MCP
 * server on stdin/stdout. version is printed by --version and --help and
 * logged at server start. Returns the process exit code. */
int cbm_main(int argc, char **argv, const char *version);

#endif /* CBM_MAIN_H */
