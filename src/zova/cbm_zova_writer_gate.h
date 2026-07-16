#ifndef CBM_ZOVA_WRITER_GATE_H
#define CBM_ZOVA_WRITER_GATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    intptr_t handle;
    bool mutex_held;
    char lock_path[1024];
} cbm_zova_writer_guard_t;

int cbm_zova_writer_guard_acquire(const char *zova_path,
                                  cbm_zova_writer_guard_t *guard);
int cbm_zova_writer_guard_try_acquire(const char *zova_path,
                                      cbm_zova_writer_guard_t *guard);
void cbm_zova_writer_guard_release(cbm_zova_writer_guard_t *guard);
uint64_t cbm_zova_writer_gate_waiter_count(void);

#endif
