/* @title: Writer to Reader Boosting */
#pragma once
#include <thread/thread_types.h>

/*
 * Implementing writer to reader boosting for rwlocks is quite tricky,
 * especially for turnstile-backed ones where you don't get extra data
 * to store lists of threads.
 *
 *
 *
 *
 *
 */

#define WROOST_BUCKET_COUNT 256
#define WROOST_THREAD_COUNTERS 64

static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) +
                  BITS_NEEDED((WROOST_BUCKET_COUNT / WROOST_THREAD_COUNTERS) -
                              1) ==
              8);

typedef BITMAP_DECLARE(wroost_mask_t, WROOST_THREAD_COUNTERS);
typedef uint8_t wroost_id_t;
static_assert(BITS_PER_BYTE * sizeof(wroost_id_t) >=
                  BITS_NEEDED(WROOST_THREAD_COUNTERS),
              "wroost_id_t needs to be wider");
