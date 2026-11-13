// Sampling profiler of the game thread (runtime/sampler.c; Linux, ODYSSEY_SAMPLE_AT=<frame>).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The game thread, at each present (its frame number): starts / stops the sampling
void sampler_frame(uint64_t frame);

#ifdef __cplusplus
}
#endif
