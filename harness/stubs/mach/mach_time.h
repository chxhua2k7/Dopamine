#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer; uint32_t denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *i) { i->numer = 1; i->denom = 1; return 0; }
static inline uint64_t mach_continuous_time(void) { struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
