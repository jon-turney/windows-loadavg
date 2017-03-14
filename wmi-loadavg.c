/* loadavg.cc: load average support.

  This file is part of Cygwin.

  This software is a copyrighted work licensed under the terms of the
  Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
  details. */

/*
  Emulate load average

  There's a fair amount of approximation done here, so don't try to use this to
  actually measure anything, but it should be good enough for programs to
  throttle their activity based on load.

  A per-process load average estimate is maintained.

  We attempt to count running and runnable processes, but unlike linux we don't
  count processes in uninterruptible sleep (blocked on I/O).

  The number of running processes is estimated as (NumberOfProcessors) * (%
  Processor Time).  The number of runnable processes is estimated as
  ProcessorQueueLength.

  This estimate is only updated at most every 5 seconds.

  Note that PDH will only return data for '% Processor Time' afer the second
  call to PdhCollectQueryData(), as it's computed over an interval, so the first
  load estimate will always be 0.

  See also the linux kernel implementation of loadavg, particularly in the
  tickless kernel case.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define _WIN32_WINNT 0x0600
#include <pdh.h>

static double _loadavg[3] = { 0.0, 0.0, 0.0 };
static PDH_HQUERY query;
static PDH_HCOUNTER counter1;
static PDH_HCOUNTER counter2;

static bool load_init(void)
{
  static bool tried = false;
  static bool initialized = false;

  if (!tried)
    {
      tried = true;

      PDH_STATUS ret = PdhOpenQueryA(NULL, 0, &query);
      if (ret != ERROR_SUCCESS)
        return false;

      ret = PdhAddEnglishCounterA(query, "\\Processor(_Total)\\% Processor Time",
                                  0, &counter1);
      if (ret != ERROR_SUCCESS)
        return false;

      ret = PdhAddEnglishCounterA(query, "\\System\\Processor Queue Length",
                                  0, &counter2);
      if (ret != ERROR_SUCCESS)
        return false;

      initialized = true;
    }

  return initialized;
}

/* estimate the current load */
static double load(void)
{
  PDH_STATUS ret = PdhCollectQueryData(query);
  if (ret != ERROR_SUCCESS)
    return 0.0;

  /* Estimate the number of running processes as (NumberOfProcessors) * (%
     Processor Time) */
  PDH_FMT_COUNTERVALUE fmtvalue1;
  ret = PdhGetFormattedCounterValue(counter1, PDH_FMT_DOUBLE, NULL, &fmtvalue1);
  if (ret != ERROR_SUCCESS)
    return 0.0;

  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);

  printf("processor time: %f, num processors: %d\n", fmtvalue1.doubleValue,
         sysinfo.dwNumberOfProcessors);
  double running = fmtvalue1.doubleValue * sysinfo.dwNumberOfProcessors / 100;

  /* Estimate the number of runnable processes using ProcessorQueueLength */
  PDH_FMT_COUNTERVALUE fmtvalue2;
  ret = PdhGetFormattedCounterValue(counter2, PDH_FMT_LONG, NULL, &fmtvalue2);
  if (ret != ERROR_SUCCESS)
    return 0.0;

  LONG rql = fmtvalue2.longValue;

  double active_tasks = rql + running;
  printf("running: %f, rql: %d, active_tasks: %f\n", running, rql, active_tasks);

  return active_tasks;
}

static void calc_load(int index, int delta_time, int decay_time, double n)
{
  double df = 1.0/exp((double)delta_time/decay_time);
  double load = _loadavg[index];
  load *= df;
  load += n*(1.0-df);
  _loadavg[index] = load;
}

static void update_loadavg(int delta_time)
{
  if (!load_init())
    return;

  printf("delta_time: %d\n", delta_time);

  double active_tasks = load();

  /* Compute the exponentially weighted moving average over ... */
  calc_load(0, delta_time, 60,  active_tasks); /* ... 1 min */
  calc_load(1, delta_time, 300, active_tasks); /* ... 5 min */
  calc_load(2, delta_time, 900, active_tasks); /* ... 15 min */
}

/* getloadavg: BSD */
extern "C" int
getloadavg (double loadavg[], int nelem)
{
  /* Don't recalculate the load average if less than 5 seconds has elapsed since
     the last time it was calculated */
  static time_t last_time = 0;
  time_t curr_time = time(NULL);
  int delta_time = curr_time - last_time;
  if (delta_time >= 5)
    {
      last_time = curr_time;
      update_loadavg(delta_time);
    }

  /* The maximum number of samples is 3 */
  if (nelem > 3)
    nelem = 3;

  /* Return the samples and number of samples retrieved */
  memcpy(loadavg, _loadavg, nelem * sizeof(double));
  return nelem;
}

//
// g++ -g -O0 wmi-loadavg.c -o wmi-loadavg -lpdh
//

int main(int argc, char **argv)
{
  while (1)
    {
      double la[3];
      getloadavg(la, 3);
      printf("%f %f %f\n", la[0], la[1], la[2]);
      sleep(1);
    }
}
