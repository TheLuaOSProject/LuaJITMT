/*
** Shared sleep helper for C fixtures.
**
** Many concurrency tests intentionally wait through interrupted nanosleep()
** calls instead of treating EINTR as progress. Keeping that retry loop here
** avoids slightly different local spellings in each fixture.
*/

#ifndef LJ_TEST_SLEEP_H
#define LJ_TEST_SLEEP_H

#include <time.h>

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

#endif
