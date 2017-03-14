#include <stdio.h>

#define _WIN32_DCOM
#include <iostream>
using namespace std;

/* comip.h uses MSVCRT-style _(m|)alloca */
#include <alloca.h>
#define _alloca alloca
#define _malloca alloca
#define _freea(x)

#include <comdef.h>
#include <wbemidl.h>

//
// g++ -g -O0 wmi-loadavg.c -lole32 -loleaut32 -lwbemuuid -o wmi-loadavg
//
// See MSDN article "Accessing WMI Preinstalled Performance Classes"
//

int main(int argc, char **argv)
{
    HRESULT hres;

    // Step 1: --------------------------------------------------
    // Initialize COM. ------------------------------------------

    hres =  CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres))
    {
        cout << "Failed to initialize COM library. Error code = 0x"
            << hex << hres << endl;
        return 1;                  // Program has failed.
    }

    // Step 2: --------------------------------------------------
    // Set general COM security levels --------------------------

    hres =  CoInitializeSecurity(
        NULL,
        -1,                          // COM authentication
        NULL,                        // Authentication services
        NULL,                        // Reserved
        RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication
        RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation
        NULL,                        // Authentication info
        EOAC_NONE,                   // Additional capabilities
        NULL                         // Reserved
        );

    if (FAILED(hres))
    {
        cout << "Failed to initialize security. Error code = 0x"
            << hex << hres << endl;
        CoUninitialize();
        return 1;                    // Program has failed.
    }

    // Step 3: ---------------------------------------------------
    // Obtain the initial locator to WMI -------------------------

    IWbemLocator *pLoc = NULL;

    hres = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID *) &pLoc);

    if (FAILED(hres))
    {
        cout << "Failed to create IWbemLocator object."
            << " Err code = 0x"
            << hex << hres << endl;
        CoUninitialize();
        return 1;                 // Program has failed.
    }

    // Step 4: -----------------------------------------------------
    // Connect to WMI through the IWbemLocator::ConnectServer method

    IWbemServices *pNameSpace = NULL;

    // Connect to the root\cimv2 namespace with
    // the current user and obtain pointer pSvc
    // to make IWbemServices calls.
    hres = pLoc->ConnectServer(
         _bstr_t(L"ROOT\\CIMV2"), // Object path of WMI namespace
         NULL,                    // User name. NULL = current user
         NULL,                    // User password. NULL = current
         0,                       // Locale. NULL indicates current
         0,                       // Security flags.
         NULL,                    // Authority (for example, Kerberos)
         0,                       // Context object
         &pNameSpace              // pointer to IWbemServices proxy
         );

    if (FAILED(hres))
    {
        cout << "Could not connect. Error code = 0x"
             << hex << hres << endl;
        pLoc->Release();
        CoUninitialize();
        return 1;                // Program has failed.
    }

    cout << "Connected to ROOT\\CIMV2 WMI namespace" << endl;

    // Step 5: --------------------------------------------------
    // Set security levels on the proxy -------------------------

    hres = CoSetProxyBlanket(
       pNameSpace,                  // Indicates the proxy to set
       RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
       RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
       NULL,                        // Server principal name
       RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx
       RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
       NULL,                        // client identity
       EOAC_NONE                    // proxy capabilities
    );

    if (FAILED(hres))
    {
        cout << "Could not set proxy blanket. Error code = 0x"
            << hex << hres << endl;
        pNameSpace->Release();
        pLoc->Release();
        CoUninitialize();
        return 1;               // Program has failed.
    }

    // Step 6: --------------------------------------------------

    IWbemRefresher* pRefresher = NULL;
    IWbemConfigureRefresher* pConfig = NULL;

    // Create a WMI Refresher and get a pointer to the
    // IWbemConfigureRefresher interface.
    CoCreateInstance(CLSID_WbemRefresher,
                     NULL,
                     CLSCTX_INPROC_SERVER,
                     IID_IWbemRefresher,
                     (void**) &pRefresher
    );

    pRefresher->QueryInterface(IID_IWbemConfigureRefresher,
                               (void**) &pConfig );

    IWbemClassObject* pObj = NULL;

    // Add the instance to be refreshed.
    hres = pConfig->AddObjectByPath(
       pNameSpace,
       L"Win32_PerfRawData_PerfOS_System=@",
       0L,
       NULL,
       &pObj,
       NULL
    );

    if (FAILED(hres))
    {
       cout << "Cannot add object. Error code: 0x"
            << hex << hres << endl;
       pNameSpace->Release();

       return hres;
    }

    // For quick property retrieval, use IWbemObjectAccess.
    IWbemObjectAccess* pAcc = NULL;
    pObj->QueryInterface(IID_IWbemObjectAccess,
                         (void**) &pAcc );

    // This is not required.
    pObj->Release();

    // Get a property handle for the ProcessorQueueLength property.
    LONG lProcessorQueueLengthHandle = 0;
    DWORD dwProcessorQueueLength = 0;
    CIMTYPE variant;

    pAcc->GetPropertyHandle(L"ProcessorQueueLength",
                            &variant,
                            &lProcessorQueueLengthHandle );

    // Refresh the object ten times and retrieve the value.
    for( int x = 0; x < 10; x++ )
    {
        pRefresher->Refresh( 0L );
        pAcc->ReadDWORD( lProcessorQueueLengthHandle, &dwProcessorQueueLength );
        printf( "Processor Queue Length is %lu\n", dwProcessorQueueLength );
    }

    // Clean up all the objects.
    pAcc->Release();

    // Done with these too.
    pConfig->Release();
    pRefresher->Release();
    pNameSpace->Release();

    // Cleanup
    // ========
    pNameSpace->Release();
    pLoc->Release();

    CoUninitialize();

    return 0;   // Program successfully completed.
}
