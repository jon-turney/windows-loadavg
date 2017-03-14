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

  A per-process load average estimation is maintained.

  This estimation is only updated at most every 5 seconds (similar to linux)

  See linux kernel implementation of loadavg, particularly in the tickless
  kernel case.

  We count running and runnable processes, but unlike linux we don't count
  processes in uninterruptible sleep (blocked on I/O).

  See the MSDN article "Accessing WMI Preinstalled Performance Classes"
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* Emulate MSVCRT-style _(m|)alloca used by comip.h */
#include <alloca.h>
#define _alloca alloca
#define _malloca alloca
#define _freea(x)

#include <comdef.h>
#include <wbemidl.h>

static double _loadavg[3] = { 0.0, 0.0, 0.0 };

class ProcessorQueueLength
{
public:
  static bool init(void);
  static void fini(void);
  static DWORD read(void);

private:
  static bool try_init(void);

  // data
  static IWbemLocator *pLoc;
  static IWbemServices *pNameSpace;
  static IWbemRefresher *pRefresher;
  static IWbemConfigureRefresher *pConfig;
  static IWbemObjectAccess *pAcc;
  static LONG lProcessorQueueLengthHandle;
};

IWbemLocator *ProcessorQueueLength::pLoc = NULL;
IWbemServices *ProcessorQueueLength::pNameSpace = NULL;
IWbemRefresher *ProcessorQueueLength::pRefresher = NULL;
IWbemConfigureRefresher *ProcessorQueueLength::pConfig = NULL;
IWbemObjectAccess *ProcessorQueueLength::pAcc = NULL;
LONG ProcessorQueueLength::lProcessorQueueLengthHandle = 0;

bool ProcessorQueueLength::init(void)
{
  static bool tried = false;
  static bool initialized = false;

  if (!tried)
    {
      tried = true;
      initialized = try_init();
    }

  return initialized;
}

bool ProcessorQueueLength::try_init(void)
{
  HRESULT hres;

  // Step 1: --------------------------------------------------
  // Initialize COM. ------------------------------------------

  hres =  CoInitializeEx(0, COINIT_MULTITHREADED);
  if (FAILED(hres))
    {
      printf("Failed to initialize COM library. Error code = 0x%x\n",
		   hres);
      return false;
    }

  // Step 2: --------------------------------------------------
  // Set general COM security levels --------------------------

  hres =  CoInitializeSecurity(
			       NULL,
			       -1,			      // COM authentication
			       NULL,			      // Authentication services
			       NULL,			      // Reserved
			       RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication
			       RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation
			       NULL,			      // Authentication info
			       EOAC_NONE,		      // Additional capabilities
			       NULL			      // Reserved
			       );

  if (FAILED(hres))
    {
      printf("Failed to initialize security. Error code = 0x%x\n", hres);
      CoUninitialize();
      return false;
    }

  // Step 3: ---------------------------------------------------
  // Obtain the initial locator to WMI -------------------------

  hres = CoCreateInstance(
			  CLSID_WbemLocator,
			  0,
			  CLSCTX_INPROC_SERVER,
			  IID_IWbemLocator, (LPVOID *) &pLoc);

  if (FAILED(hres))
    {
      printf("Failed to create IWbemLocator object. Error code = 0x%x\n",
		   hres);
      CoUninitialize();
      return false;
    }

  // Step 4: -----------------------------------------------------
  // Connect to WMI through the IWbemLocator::ConnectServer method

  // Connect to the root\cimv2 namespace with
  // the current user and obtain pointer pSvc
  // to make IWbemServices calls.
  hres = pLoc->ConnectServer(
			     _bstr_t(L"ROOT\\CIMV2"), // Object path of WMI namespace
			     NULL,			// User name. NULL = current user
			     NULL,			// User password. NULL = current
			     0,			// Locale. NULL indicates current
			     0,			// Security flags.
			     NULL,			// Authority (for example, Kerberos)
			     0,			// Context object
			     &pNameSpace		// pointer to IWbemServices proxy
			     );

  if (FAILED(hres))
    {
      printf("Could not connect. Error code = 0x%x\n", hres);
      pLoc->Release();
      CoUninitialize();
      return false;
    }

  // Step 5: --------------------------------------------------
  // Set security levels on the proxy -------------------------

  hres = CoSetProxyBlanket(
			   pNameSpace,		  // Indicates the proxy to set
			   RPC_C_AUTHN_WINNT,		  // RPC_C_AUTHN_xxx
			   RPC_C_AUTHZ_NONE,		  // RPC_C_AUTHZ_xxx
			   NULL,			  // Server principal name
			   RPC_C_AUTHN_LEVEL_CALL,	  // RPC_C_AUTHN_LEVEL_xxx
			   RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
			   NULL,			  // client identity
			   EOAC_NONE			  // proxy capabilities
			   );

  if (FAILED(hres))
    {
      printf("Could not set proxy blanket. Error code = 0x%x\n", hres);
      pNameSpace->Release();
      pLoc->Release();
      CoUninitialize();
      return false;
    }

  // Step 6: --------------------------------------------------

  // Create a WMI Refresher and get a pointer to the
  // IWbemConfigureRefresher interface.
  CoCreateInstance(CLSID_WbemRefresher,
		   NULL,
		   CLSCTX_INPROC_SERVER,
		   IID_IWbemRefresher,
		   (void**) &pRefresher);

  pRefresher->QueryInterface(IID_IWbemConfigureRefresher,
			     (void**) &pConfig);

  IWbemClassObject* pObj = NULL;

  // Add the instance to be refreshed.
  hres = pConfig->AddObjectByPath(
				  pNameSpace,
				  L"Win32_PerfRawData_PerfOS_System=@",
				  0L,
				  NULL,
				  &pObj,
				  NULL);

  if (FAILED(hres))
    {
      printf("Cannot add object. Error code: 0x%x\n", hres);
      pNameSpace->Release();
      pLoc->Release();
      CoUninitialize();

      return false;
    }

  // For quick property retrieval, use IWbemObjectAccess.
  pObj->QueryInterface(IID_IWbemObjectAccess, (void**) &pAcc);

  // This is no longer required.
  pObj->Release();

  // Get a property handle for the ProcessorQueueLength property.
  CIMTYPE variant;

  pAcc->GetPropertyHandle(L"ProcessorQueueLength",
			  &variant,
			  &lProcessorQueueLengthHandle);

  return true;
}

void ProcessorQueueLength::fini(void)
{
  // Clean up all the objects.
  pAcc->Release();
  pConfig->Release();
  pRefresher->Release();
  pNameSpace->Release();
  pLoc->Release();

  CoUninitialize();
}

DWORD ProcessorQueueLength::read(void)
{
  if (!init())
    return 0;

  DWORD dwProcessorQueueLength = 0;
  pRefresher->Refresh(0L);
  pAcc->ReadDWORD(lProcessorQueueLengthHandle, &dwProcessorQueueLength);

  return dwProcessorQueueLength;
}

/* XXX: exp is computed more than once */
#define CALC_LOAD(load, exp, n) \
  load *= exp; \
  load += n*(1.0-(exp));

#define EXP(decay_time) \
  1.0/exp(delta_time/decay_time)

static void calculate_loadavg(int delta_time)
{
  /* Estimate the number of runnable processes using ProcessorQueueLength */
  DWORD rql = ProcessorQueueLength::read();

  /* Estimate the number of running processes as (NumberOfProcessors) * (% Processor Time) */

  unsigned int active_tasks = rql; /* + XXX: */
  printf("active_tasks: %u, delta_time %d\n", active_tasks, delta_time);

  /* Compute the exponentially weighted moving average over ... */
  CALC_LOAD(_loadavg[0], EXP(60.0),  active_tasks); /* ... 1 min */
  CALC_LOAD(_loadavg[1], EXP(300.0), active_tasks); /* ... 5 min */
  CALC_LOAD(_loadavg[2], EXP(900.0), active_tasks); /* ... 15 min */
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
      calculate_loadavg(delta_time);
    }

  /* The maximum number of samples is 3 */
  if (nelem > 3)
    nelem = 3;

  /* Return the samples and number of samples retrieved */
  memcpy(loadavg, _loadavg, nelem * sizeof(double));
  return nelem;
}


//
// g++ -g -O0 wmi-loadavg.c -lole32 -loleaut32 -lwbemuuid -o wmi-loadavg
//

int main(int argc, char **argv)
{
  printf("Working...\n");
  while (1)
    {
      double la[3];
      getloadavg(la, 3);
      printf("%f %f %f\n", la[0], la[1], la[2]);
      sleep(1);
    }
}
