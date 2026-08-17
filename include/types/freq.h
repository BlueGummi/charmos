/* @title: Frequency */
#pragma once
#include <types/types.h>

#define HZ_PER_KHZ 1000ULL
#define HZ_PER_MHZ 1000000ULL
#define HZ_PER_GHZ 1000000000ULL

#define KHZ_PER_MHZ 1000ULL
#define KHZ_PER_GHZ 1000000ULL

#define MHZ_PER_GHZ 1000ULL

#define HZ_TO_KHZ(hz) ((hz) / HZ_PER_KHZ)
#define HZ_TO_MHZ(hz) ((hz) / HZ_PER_MHZ)
#define HZ_TO_GHZ(hz) ((hz) / HZ_PER_GHZ)

#define KHZ_TO_HZ(khz) ((khz) * HZ_PER_KHZ)
#define KHZ_TO_MHZ(khz) ((khz) / KHZ_PER_MHZ)
#define KHZ_TO_GHZ(khz) ((khz) / KHZ_PER_GHZ)

#define MHZ_TO_HZ(mhz) ((mhz) * HZ_PER_MHZ)
#define MHZ_TO_KHZ(mhz) ((mhz) * KHZ_PER_MHZ)
#define MHZ_TO_GHZ(mhz) ((mhz) / MHZ_PER_GHZ)

#define GHZ_TO_HZ(ghz) ((ghz) * HZ_PER_GHZ)
#define GHZ_TO_KHZ(ghz) ((ghz) * KHZ_PER_GHZ)
#define GHZ_TO_MHZ(ghz) ((ghz) * MHZ_PER_GHZ)
