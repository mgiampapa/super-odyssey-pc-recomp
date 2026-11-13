#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cache driver information while the device is healthy; crash reporting never
// queries a lost device. A report contains text only, not process memory.
void diagnostics_init(void);
void diagnostics_gpu(const char *description);
void diagnostics_fault(const char *what, uint64_t address, uint64_t pc, uint64_t lr);
bool diagnostics_write_report(char *path, size_t capacity);
bool diagnostics_capture_crash(char *path, size_t capacity);
bool diagnostics_last_crash(char *path, size_t capacity);
bool diagnostics_copy_path(const char *path);
int diagnostics_copy_status(void); // 1 copied, 0 queued, -1 failed
void diagnostics_fatal_dialog(const char *message);
