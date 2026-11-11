#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Run on the voice task, never on the Noise callback task. */
void muse_vm_probe_run(char *result, size_t cap, bool (*cancelled)(void));
