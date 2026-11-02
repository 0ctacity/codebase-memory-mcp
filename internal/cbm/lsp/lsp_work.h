/*
 * lsp_work.h — deterministic work counter for LSP complexity tests.
 *
 * Every name comparison a resolver primitive performs while searching (scope
 * frame probes, registry chains, enclosing-definition candidates) adds to a
 * thread-local counter, so a test can assert that the work grows linearly
 * with input size without reading a clock. Like ts_lsp.c's eval counter it is
 * always compiled in: one thread-local add per probe.
 */
#ifndef CBM_LSP_WORK_H
#define CBM_LSP_WORK_H

#include <stdint.h>

extern _Thread_local uint64_t cbm_lsp_work_steps;
#define CBM_LSP_WORK(n) (cbm_lsp_work_steps += (uint64_t)(n))
/* Read and reset the calling thread's counter. */
uint64_t cbm_lsp_work_take(void);

#endif /* CBM_LSP_WORK_H */
