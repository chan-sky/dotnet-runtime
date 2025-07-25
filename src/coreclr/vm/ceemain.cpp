// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// ===========================================================================
// File: CEEMAIN.CPP
// ===========================================================================
//

//
//
// The CLR code base uses a hyperlink feature of the HyperAddin plugin for Visual Studio. If you don't see
// 'HyperAddin' in your Visual Studio menu bar you don't have this support. To get it type
//
//     \\clrmain\tools\installCLRAddins
//
//  After installing HyperAddin, your first run of VS should be as an administrator so HyperAddin can update
//  some registry information.
//
//  At this point the code: prefixes become hyperlinks in Visual Studio and life is good. See
//  http://mswikis/clr/dev/Pages/CLR%20Team%20Commenting.aspx for more information
//
//  There is a bug associated with Visual Studio where it does not recognise the hyperlink if there is a ::
//  preceding it on the same line. Since C++ uses :: as a namespace separator, this can often mean that the
//  second hyperlink on a line does not work. To work around this it is better to use '.' instead of :: as
//  the namespace separators in code: hyperlinks.
//
// #StartHere
// #TableOfContents The .NET Runtime Table of contents
//
// This comment is mean to be a nexus that allows you to jump quickly to various interesting parts of the
// runtime.
//
// You can refer to product studio bugs using urls like the following
//     * http://bugcheck/bugs/DevDivBugs/2320.asp
//     * http://bugcheck/bugs/VSWhidbey/601210.asp
//
//  Dev10 Bugs can be added with URLs like the following (for Dev10 bug 671409)
//     * http://tkbgitvstfat01:8090/wi.aspx?id=671409
//
//*************************************************************************************************
//
// * Introduction to the runtime file:../../Documentation/botr/botr-faq.md
//
// #MajorDataStructures. The major data structures associated with the runtime are
//     * code:Thread (see file:threads.h#ThreadClass) - the additional thread state the runtime needs.
//     * code:AppDomain - The managed version of a process
//     * code:Assembly - The unit of deployment and versioning (may be several DLLs but often is only one).
//     * code:Module - represents a Module (DLL or EXE).
//     * code:MethodTable - represents the 'hot' part of a type (needed during normal execution)
//     * code:EEClass - represents the 'cold' part of a type (used during compilation, interop, ...)
//     * code:MethodDesc - represents a Method
//     * code:FieldDesc - represents a Field.
//     * code:Object - represents a object on the GC heap allocated with code:Alloc
//
// * ECMA specifications
//     * Partition I Concepts
//         http://download.microsoft.com/download/D/C/1/DC1B219F-3B11-4A05-9DA3-2D0F98B20917/Partition%20I%20Architecture.doc
//     * Partition II Meta Data
//         http://download.microsoft.com/download/D/C/1/DC1B219F-3B11-4A05-9DA3-2D0F98B20917/Partition%20II%20Metadata.doc
//     * Partition III IL
//         http://download.microsoft.com/download/D/C/1/DC1B219F-3B11-4A05-9DA3-2D0F98B20917/Partition%20III%20CIL.doc
//
//  * Serge Liden (worked on the CLR and owned ILASM / ILDASM for a long time wrote a good book on IL
//     * Expert .NET 2.0 IL Assembler  http://www.amazon.com/Expert-NET-2-0-IL-Assembler/dp/1590596463
//
// * This is also a pretty nice overview of what the CLR is at
//     http://msdn2.microsoft.com/en-us/netframework/aa497266.aspx
//
// * code:EEStartup - This routine must be called before any interesting runtime services are used. It is
//     invoked as part of mscorwks's DllMain logic.
// * code:#EEShutDown - Code called before we shut down the EE.
//
// * file:..\inc\corhdr.h#ManagedHeader - From a data structure point of view, this is the entry point into
//     the runtime. This is how all other data in the EXE are found.
//
// * code:ICorJitCompiler#EEToJitInterface - This is the interface from the EE to the Just in time (JIT)
//     compiler. The interface to the JIT is relatively simple (compileMethod), however the EE provides a
//     rich set of callbacks so the JIT can get all the information it needs. See also
//     file:../../Documentation/botr/ryujit-overview.md for general information on the JIT.
//
// * code:VirtualCallStubManager - This is the main class that implements interface dispatch
//
// * Precode - Every method needs entry point for other code to call even if that native code does not
//     actually exist yet. To support this methods can have code:Precode that is an entry point that exists
//     and will call the JIT compiler if the code does not yet exist.
//
//  * NGEN - NGen stands for Native code GENeration and it is the runtime way of precompiling IL and IL
//      Meta-data into native code and runtime data structures. At compilation time the most
//      fundamental data structures is the code:ZapNode which represents something that needs to go into the
//      NGEN image.
//
//   * What is cooperative / preemtive mode ? file:threads.h#CooperativeMode and
//       file:threads.h#SuspendingTheRuntime and file:../../Documentation/botr/threading.md
//   * Garbage collection - file:gc.cpp#Overview and file:../../Documentation/botr/garbage-collection.md
//   * code:AppDomain - The managed version of a process.
//   * Calling Into the runtime (FCALLs QCalls) file:../../Documentation/botr/corelib.md
//   * Exceptions - file:../../Documentation/botr/exceptions.md. The most important routine to start
//       with is code:COMPlusFrameHandler which is the routine that we hook up to get called when an unmanaged
//       exception happens.
//   * Assembly Loading file:../../Documentation/botr/type-loader.md
//   * Profiling file:../../Documentation/botr/profiling.md and file:../../Documentation/botr/profilability.md
//   * FCALLS QCALLS (calling into the runtime from managed code)
//       file:../../Documentation/botr/corelib.md
//   * Event Tracing for Windows
//     * file:../inc/eventtrace.h#EventTracing -
//     * This is the main file dealing with event tracing in CLR
//     * The implementation of this class is available in file:eventtrace.cpp
//     * file:../inc/eventtrace.h#CEtwTracer - This is the main class dealing with event tracing in CLR.
//         Follow the link for more information on how this feature has been implemented
//     * http://mswikis/clr/dev/Pages/CLR%20ETW%20Events%20Wiki.aspx - Follow the link for more information on how to
//         use this instrumentation feature.

// ----------------------------------------------------------------------------------------------------
// Features in the runtime that have been given hyperlinks
//
// * code:Nullable#NullableFeature - the Nullable<T> type has special runtime semantics associated with
//     boxing this describes this feature.

#include "common.h"

#include "vars.hpp"
#include "log.h"
#include "ceemain.h"
#include "clsload.hpp"
#include "object.h"
#include "hash.h"
#include "ecall.h"
#include "ceemain.h"
#include "dllimport.h"
#include "syncblk.h"
#include "eeconfig.h"
#include "stublink.h"
#include "method.hpp"
#include "codeman.h"
#include "frames.h"
#include "threads.h"
#include "stackwalk.h"
#include "gcheaputilities.h"
#include "interoputil.h"
#include "fieldmarshaler.h"
#include "dbginterface.h"
#include "eedbginterfaceimpl.h"
#include "debugdebugger.h"
#include "cordbpriv.h"
#include "comdelegate.h"
#include "appdomain.hpp"
#include "eventtrace.h"
#include "corhost.h"
#include "binder.h"
#include "olevariant.h"
#include "comcallablewrapper.h"
#include "../dlls/mscorrc/resource.h"
#include "util.hpp"
#include "posterror.h"
#include "virtualcallstub.h"
#include "strongnameinternal.h"
#include "syncclean.hpp"
#include "debuginfostore.h"
#include "finalizerthread.h"
#include "threadsuspend.h"
#include "disassembler.h"
#include "jithost.h"
#include "pgo.h"
#include "pendingload.h"
#include "cdacplatformmetadata.hpp"
#include "minipal/time.h"

#ifdef FEATURE_INTERPRETER
#include "callstubgenerator.h"
#endif

#ifndef TARGET_UNIX
#include "dwreport.h"
#endif // !TARGET_UNIX

#include "stringarraylist.h"
#include "stubhelpers.h"

#ifdef FEATURE_COMINTEROP
#include "runtimecallablewrapper.h"
#include "interoplibinterface.h"
#endif // FEATURE_COMINTEROP

#ifdef FEATURE_COMINTEROP_APARTMENT_SUPPORT
#include "olecontexthelpers.h"
#endif // FEATURE_COMINTEROP_APARTMENT_SUPPORT

#ifdef PROFILING_SUPPORTED
#include "proftoeeinterfaceimpl.h"
#include "profilinghelper.h"
#endif // PROFILING_SUPPORTED

#ifdef FEATURE_PERFMAP
#include "perfmap.h"
#endif

#include "diagnosticserveradapter.h"
#include "eventpipeadapter.h"

#if defined(FEATURE_PERFTRACING) && defined(TARGET_LINUX)
#include "user_events.h"
#endif // defined(FEATURE_PERFTRACING) && defined(TARGET_LINUX)

#ifndef TARGET_UNIX
// Included for referencing __security_cookie
#include "process.h"
#endif // !TARGET_UNIX

#ifdef FEATURE_GDBJIT
#include "gdbjit.h"
#endif // FEATURE_GDBJIT

#include "genanalysis.h"

static int GetThreadUICultureId(_Out_ LocaleIDValue* pLocale);  // TODO: This shouldn't use the LCID.  We should rely on name instead

static HRESULT GetThreadUICultureNames(__inout StringArrayList* pCultureNames);

HRESULT EEStartup();


static void InitializeGarbageCollector();

#ifdef DEBUGGING_SUPPORTED
static void InitializeDebugger(void);
static void TerminateDebugger(void);
extern "C" HRESULT __cdecl CorDBGetInterface(DebugInterface** rcInterface);
#endif // DEBUGGING_SUPPORTED

// g_coreclr_embedded indicates that coreclr is linked directly into the program
// g_hostpolicy_embedded indicates that the hostpolicy library is linked directly into the executable
#ifdef CORECLR_EMBEDDED
bool g_coreclr_embedded = true;
bool g_hostpolicy_embedded = true; // We always embed hostpolicy if coreclr is also embedded
#else
bool g_coreclr_embedded = false;
bool g_hostpolicy_embedded = false;
#endif

// Remember how the last startup of EE went.
HRESULT g_EEStartupStatus = S_OK;

// Flag indicating if the EE has been started.  This is set prior to initializing the default AppDomain, and so does not indicate that
// the EE is fully able to execute arbitrary managed code.  To ensure the EE is fully started, call EnsureEEStarted rather than just
// checking this flag.
Volatile<BOOL> g_fEEStarted = FALSE;

// The OS thread ID of the thread currently performing EE startup, or 0 if there is no such thread.
DWORD   g_dwStartupThreadId = 0;

// Event to synchronize EE shutdown.
static CLREvent * g_pEEShutDownEvent;

static DangerousNonHostedSpinLock g_EEStartupLock;

// ---------------------------------------------------------------------------
// %%Function: EnsureEEStarted()
//
// Description: Ensure the CLR is started.
// ---------------------------------------------------------------------------
HRESULT EnsureEEStarted()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
        ENTRY_POINT;
    }
    CONTRACTL_END;

    if (g_fEEShutDown)
        return E_FAIL;

    HRESULT hr = E_FAIL;

    // On non x86 platforms, when we load CoreLib during EEStartup, we will
    // re-enter _CorDllMain with a DLL_PROCESS_ATTACH for CoreLib. We are
    // far enough in startup that this is allowed, however we don't want to
    // re-start the startup code so we need to check to see if startup has
    // been initiated or completed before we call EEStartup.
    //
    // We do however want to make sure other threads block until the EE is started,
    // which we will do further down.
    if (!g_fEEStarted)
    {
        // Initialize our configuration.
        CLRConfig::Initialize();

        BOOL bStarted=FALSE;

        {
            DangerousNonHostedSpinLockHolder lockHolder(&g_EEStartupLock);

            // Now that we've acquired the lock, check again to make sure we aren't in
            // the process of starting the CLR or that it hasn't already been fully started.
            // At this point, if startup has been inited we don't have anything more to do.
            // And if EEStartup already failed before, we don't do it again.
            if (!g_fEEStarted && !g_fEEInit && SUCCEEDED (g_EEStartupStatus))
            {
                g_dwStartupThreadId = GetCurrentThreadId();

                EEStartup();
                bStarted=g_fEEStarted;
                hr = g_EEStartupStatus;

                g_dwStartupThreadId = 0;
            }
            else
            {
                hr = g_EEStartupStatus;
                if (SUCCEEDED(g_EEStartupStatus))
                {
                    hr = S_FALSE;
                }
            }
        }
    }
    else
    {
        //
        // g_fEEStarted is TRUE, but startup may not be complete since we initialize the default AppDomain
        // *after* setting that flag.  g_fEEStarted is set inside of g_EEStartupLock, and that lock is
        // not released until the EE is really started - so we can quickly check whether the EE is definitely
        // started by checking if that lock is currently held.  If it is not, then we know the other thread
        // (that is actually doing the startup) has finished startup.  If it is currently held, then we
        // need to wait for the other thread to release it, which we do by simply acquiring the lock ourselves.
        //
        // We do not want to do this blocking if we are the thread currently performing EE startup.  So we check
        // that first.
        //
        // Note that the call to IsHeld here is an "acquire" barrier, as is acquiring the lock.  And the release of
        // the lock by the other thread is a "release" barrier, due to the volatile semantics in the lock's
        // implementation.  This assures us that once we observe the lock having been released, we are guaranteed
        // to observe a fully-initialized EE.
        //
        // A note about thread affinity here: we're using the OS thread ID of the current thread without
        // asking the host to pin us to this thread, as we did above.  We can get away with this, because we are
        // only interested in a particular thread ID (that of the "startup" thread) and *that* particular thread
        // is already affinitized by the code above.  So if we get that particular OS thread ID, we know for sure
        // we are really the startup thread.
        //
        if (g_EEStartupLock.IsHeld() && g_dwStartupThreadId != GetCurrentThreadId())
        {
            DangerousNonHostedSpinLockHolder lockHolder(&g_EEStartupLock);
        }

        hr = g_EEStartupStatus;
        if (SUCCEEDED(g_EEStartupStatus))
        {
            hr = S_FALSE;
        }
    }

    return hr;
}



#ifndef TARGET_UNIX
// This is our Ctrl-C, Ctrl-Break, etc. handler.
static BOOL WINAPI DbgCtrlCHandler(DWORD dwCtrlType)
{
    WRAPPER_NO_CONTRACT;

#if defined(DEBUGGING_SUPPORTED)
    // Note that if a managed-debugger is attached, it's actually attached with the native
    // debugging pipeline and it will get a control-c notifications via native debug events.
    // However, if we let the native debugging pipeline handle the event and send the notification
    // to the debugger, then we break pre-V4 behaviour because we intercept handlers registered
    // in-process.  See Dev10 Bug 846455 for more information.
    if (CORDebuggerAttached() &&
        (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT))
    {
        return g_pDebugInterface->SendCtrlCToDebugger(dwCtrlType);
    }
    else
#endif // DEBUGGING_SUPPORTED
    {
        return FALSE;             // keep looking for a real handler.
    }
}
#endif

// A host can specify that it only wants one version of hosting interface to be used.
BOOL g_singleVersionHosting;

#ifdef TARGET_WINDOWS
typedef BOOL(WINAPI* PINITIALIZECONTEXT2)(PVOID Buffer, DWORD ContextFlags, PCONTEXT* Context, PDWORD ContextLength, ULONG64 XStateCompactionMask);
PINITIALIZECONTEXT2 g_pfnInitializeContext2 = NULL;

#ifdef TARGET_ARM64
typedef DWORD64(WINAPI* PGETENABLEDXSTATEFEATURES)();
PGETENABLEDXSTATEFEATURES g_pfnGetEnabledXStateFeatures = NULL;

typedef BOOL(WINAPI* PGETXSTATEFEATURESMASK)(PCONTEXT Context, PDWORD64 FeatureMask);
PGETXSTATEFEATURESMASK g_pfnGetXStateFeaturesMask = NULL;

typedef BOOL(WINAPI* PSETXSTATEFEATURESMASK)(PCONTEXT Context, DWORD64 FeatureMask);
PSETXSTATEFEATURESMASK g_pfnSetXStateFeaturesMask = NULL;
#endif // TARGET_ARM64

static BOOLEAN WINAPI RtlDllShutdownInProgressFallback()
{
    return g_fProcessDetach;
}
PRTLDLLSHUTDOWNINPROGRESS g_pfnRtlDllShutdownInProgress = &RtlDllShutdownInProgressFallback;

#ifdef TARGET_X86
typedef VOID(__cdecl* PRTLRESTORECONTEXT)(PCONTEXT ContextRecord, struct _EXCEPTION_RECORD* ExceptionRecord);
PRTLRESTORECONTEXT g_pfnRtlRestoreContext = NULL;
#endif // TARGET_X86

void InitializeOptionalWindowsAPIPointers()
{
    HMODULE hm = GetModuleHandleW(_T("kernel32.dll"));
    g_pfnInitializeContext2 = (PINITIALIZECONTEXT2)GetProcAddress(hm, "InitializeContext2");

#ifdef TARGET_ARM64
    g_pfnGetEnabledXStateFeatures = (PGETENABLEDXSTATEFEATURES)GetProcAddress(hm, "GetEnabledXStateFeatures");
    g_pfnGetXStateFeaturesMask = (PGETXSTATEFEATURESMASK)GetProcAddress(hm, "GetXStateFeaturesMask");
    g_pfnSetXStateFeaturesMask = (PSETXSTATEFEATURESMASK)GetProcAddress(hm, "SetXStateFeaturesMask");
#endif // TARGET_ARM64

    hm = GetModuleHandleW(_T("ntdll.dll"));
    PRTLDLLSHUTDOWNINPROGRESS pfn = (PRTLDLLSHUTDOWNINPROGRESS)GetProcAddress(hm, "RtlDllShutdownInProgress");
    if (pfn != NULL)
        g_pfnRtlDllShutdownInProgress = pfn;

#ifdef TARGET_X86
    g_pfnRtlRestoreContext = (PRTLRESTORECONTEXT)GetProcAddress(hm, "RtlRestoreContext");
#endif //TARGET_X86
}
#endif // TARGET_WINDOWS

void InitializeStartupFlags()
{
    CONTRACTL {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    } CONTRACTL_END;

    STARTUP_FLAGS flags = CorHost2::GetStartupFlags();


    if (flags & STARTUP_CONCURRENT_GC)
        g_IGCconcurrent = 1;
    else
        g_IGCconcurrent = 0;


    g_heap_type = ((flags & STARTUP_SERVER_GC) && GetCurrentProcessCpuCount() > 1) ? GC_HEAP_SVR : GC_HEAP_WKS;
    g_IGCHoardVM = (flags & STARTUP_HOARD_GC_VM) == 0 ? 0 : 1;
}

//-----------------------------------------------------------------------------

void InitGSCookie()
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    volatile GSCookie * pGSCookiePtr = GetProcessGSCookiePtr();

    // The GS cookie is stored in a read only data segment
    DWORD oldProtection;
    if(!ClrVirtualProtect((LPVOID)pGSCookiePtr, sizeof(GSCookie), PAGE_READWRITE, &oldProtection))
    {
        ThrowLastError();
    }

#ifdef TARGET_UNIX
    // PAL layer is unable to extract old protection for regions that were not allocated using VirtualAlloc
    oldProtection = PAGE_READONLY;
#endif // TARGET_UNIX

#ifndef TARGET_UNIX
    // The GSCookie cannot be in a writeable page
    assert(((oldProtection & (PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|
                              PAGE_EXECUTE_WRITECOPY|PAGE_WRITECOMBINE)) == 0));

    // Forces VC cookie to be initialized.
    void * pf = &__security_check_cookie;
    pf = NULL;

    GSCookie val = (GSCookie)(__security_cookie ^ minipal_lowres_ticks());
#else // !TARGET_UNIX
    // REVIEW: Need something better for PAL...
    GSCookie val = (GSCookie)minipal_lowres_ticks();
#endif // !TARGET_UNIX

#ifdef _DEBUG
    // In _DEBUG, always use the same value to make it easier to search for the cookie
    val = (GSCookie) BIT64_ONLY(0x9ABCDEF012345678) NOT_BIT64(0x12345678);
#endif

    // To test if it is initialized. Also for ICorMethodInfo::getGSCookie()
    if (val == 0)
        val ++;
    *pGSCookiePtr = val;

    if(!ClrVirtualProtect((LPVOID)pGSCookiePtr, sizeof(GSCookie), oldProtection, &oldProtection))
    {
        ThrowLastError();
    }
}

Volatile<BOOL> g_bIsGarbageCollectorFullyInitialized = FALSE;

void SetGarbageCollectorFullyInitialized()
{
    LIMITED_METHOD_CONTRACT;

    g_bIsGarbageCollectorFullyInitialized = TRUE;
}

// Tells whether the garbage collector is fully initialized
// Stronger than IsGCHeapInitialized
BOOL IsGarbageCollectorFullyInitialized()
{
    LIMITED_METHOD_CONTRACT;

    return g_bIsGarbageCollectorFullyInitialized;
}

// ---------------------------------------------------------------------------
// %%Function: EEStartupHelper
//
// Returns:
//  S_OK                    - On success
//
// Description:
//  Reserved to initialize the EE runtime engine explicitly.
// ---------------------------------------------------------------------------

#ifndef IfFailGotoLog
#define IfFailGotoLog(EXPR, LABEL) \
do { \
    hr = (EXPR);\
    if(FAILED(hr)) { \
        STRESS_LOG2(LF_STARTUP, LL_ALWAYS, "%s failed with code %x", #EXPR, hr);\
        goto LABEL; \
    } \
    else \
       STRESS_LOG1(LF_STARTUP, LL_ALWAYS, "%s completed", #EXPR);\
} while (0)
#endif

#ifndef IfFailGoLog
#define IfFailGoLog(EXPR) IfFailGotoLog(EXPR, ErrExit)
#endif


#ifdef TARGET_UNIX
void EESocketCleanupHelper(bool isExecutingOnAltStack)
{
    CONTRACTL
    {
        GC_NOTRIGGER;
        MODE_ANY;
    } CONTRACTL_END;

    if (isExecutingOnAltStack)
    {
        Thread *pThread = GetThreadNULLOk();
        if (pThread)
        {
             pThread->SetExecutingOnAltStack();
        }
    }

    // Close the debugger transport socket first
    if (g_pDebugInterface != NULL)
    {
        g_pDebugInterface->CleanupTransportSocket();
    }

    // Close the diagnostic server socket.
#ifdef FEATURE_PERFTRACING
    DiagnosticServerAdapter::Shutdown();
#endif // FEATURE_PERFTRACING
}
#endif // TARGET_UNIX

void FatalErrorHandler(UINT errorCode, LPCWSTR pszMessage)
{
    EEPOLICY_HANDLE_FATAL_ERROR_WITH_MESSAGE(errorCode, pszMessage);
}

void EEStartupHelper()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    } CONTRACTL_END;

#ifdef ENABLE_CONTRACTS_IMPL
    {
        extern void ContractRegressionCheck();
        ContractRegressionCheck();
    }
#endif

    HRESULT hr = S_OK;
    static ConfigDWORD breakOnEELoad;
    EX_TRY
    {
        g_fEEInit = true;


        // We cache the SystemInfo for anyone to use throughout the life of the EE.
        GetSystemInfo(&g_SystemInfo);
        CDacPlatformMetadata::Init();

        // Set callbacks so that LoadStringRC knows which language our
        // threads are in so that it can return the proper localized string.
    // TODO: This shouldn't rely on the LCID (id), but only the name
        SetResourceCultureCallbacks(GetThreadUICultureNames,
        GetThreadUICultureId);

#ifndef TARGET_UNIX
        ::SetConsoleCtrlHandler(DbgCtrlCHandler, TRUE/*add*/);
#endif

#ifdef HOST_WINDOWS
        InitializeOptionalWindowsAPIPointers();
#endif // HOST_WINDOWS

        // SString initialization
        // This needs to be done before config because config uses SString::Empty()
        SString::Startup();

        IfFailGo(EEConfig::Setup());


#ifdef HOST_WINDOWS
        InitializeCrashDump();

#endif // HOST_WINDOWS
#ifndef TARGET_UNIX
        CPUGroupInfo::EnsureInitialized();
#endif // !TARGET_UNIX

        // Initialize global configuration settings based on startup flags
        // This needs to be done before the EE has started
        InitializeStartupFlags();

        PendingTypeLoadTable::Init();

        IfFailGo(ExecutableAllocator::StaticInitialize(FatalErrorHandler));

        if (g_pConfig != NULL)
        {
            IfFailGoLog(g_pConfig->sync());
        }

        Thread::StaticInitialize();

#ifdef FEATURE_INTERPRETER
        InitCallStubGenerator();
#endif // FEATURE_INTERPRETER

        JITInlineTrackingMap::StaticInitialize();
        MethodDescBackpatchInfoTracker::StaticInitialize();
        CodeVersionManager::StaticInitialize();
        TieredCompilationManager::StaticInitialize();
        CallCountingManager::StaticInitialize();
        OnStackReplacementManager::StaticInitialize();
        MethodTable::InitMethodDataCache();

#ifdef TARGET_UNIX
        ExecutableAllocator::InitPreferredRange();
#else
        {
            // Record coreclr.dll geometry
            PEDecoder pe(GetClrModuleBase());

            g_runtimeLoadedBaseAddress = (SIZE_T)pe.GetBase();
            g_runtimeVirtualSize = (SIZE_T)pe.GetVirtualSize();
            ExecutableAllocator::InitLazyPreferredRange(g_runtimeLoadedBaseAddress, g_runtimeVirtualSize, GetRandomInt(64));
        }
#endif // !TARGET_UNIX

        InitThreadManager();
        STRESS_LOG0(LF_STARTUP, LL_ALWAYS, "Returned successfully from InitThreadManager");

#ifdef FEATURE_PERFTRACING
        // Initialize the event pipe.
        EventPipeAdapter::Initialize();
#if defined(TARGET_LINUX)
        InitUserEvents();
#endif // TARGET_LINUX
#endif // FEATURE_PERFTRACING

#ifdef TARGET_UNIX
        PAL_SetShutdownCallback(EESocketCleanupHelper);
#endif // TARGET_UNIX

#ifdef STRESS_LOG
        if (CLRConfig::GetConfigValue(CLRConfig::UNSUPPORTED_StressLog, g_pConfig->StressLog()) != 0) {
            unsigned facilities = CLRConfig::GetConfigValue(CLRConfig::INTERNAL_LogFacility, LF_ALL);
            unsigned level = CLRConfig::GetConfigValue(CLRConfig::EXTERNAL_LogLevel, LL_INFO1000);
            unsigned bytesPerThread = CLRConfig::GetConfigValue(CLRConfig::UNSUPPORTED_StressLogSize, STRESSLOG_CHUNK_SIZE * 4);
            unsigned totalBytes = CLRConfig::GetConfigValue(CLRConfig::UNSUPPORTED_TotalStressLogSize, STRESSLOG_CHUNK_SIZE * 1024);
            CLRConfigStringHolder logFilename = CLRConfig::GetConfigValue(CLRConfig::UNSUPPORTED_StressLogFilename);
            StressLog::Initialize(facilities, level, bytesPerThread, totalBytes, GetClrModuleBase(), logFilename);
            g_pStressLog = &StressLog::theLog;
        }
#endif

#ifdef FEATURE_PERFTRACING
        DiagnosticServerAdapter::Initialize();
        DiagnosticServerAdapter::PauseForDiagnosticsMonitor();
#endif // FEATURE_PERFTRACING

#ifdef FEATURE_GDBJIT
        // Initialize gdbjit
        NotifyGdb::Initialize();
#endif // FEATURE_GDBJIT

#ifdef FEATURE_EVENT_TRACE
        // Initialize event tracing early so we can trace CLR startup time events.
        InitializeEventTracing();

        // Fire the EE startup ETW event
        ETWFireEvent(EEStartupStart_V1);
#endif // FEATURE_EVENT_TRACE

        InitGSCookie();

#ifdef LOGGING
        InitializeLogging();
#endif

#ifdef FEATURE_PERFMAP
        PerfMap::Initialize();
        InitThreadManagerPerfMapData();
#endif

#ifdef FEATURE_PGO
        PgoManager::Initialize();
#endif

        STRESS_LOG0(LF_STARTUP, LL_ALWAYS, "===================EEStartup Starting===================");

#ifndef TARGET_UNIX
        IfFailGoLog(EnsureRtlFunctions());
#endif // !TARGET_UNIX
        InitEventStore();

        UnwindInfoTable::Initialize();

        // Fire the runtime information ETW event
        ETW::InfoLog::RuntimeInformation(ETW::InfoLog::InfoStructs::Normal);

        if (breakOnEELoad.val(CLRConfig::UNSUPPORTED_BreakOnEELoad) == 1)
        {
#ifdef _DEBUG
            _ASSERTE(!"Start loading EE!");
#else
            DebugBreak();
#endif
        }

#ifdef ENABLE_STARTUP_DELAY
        _ASSERTE(NULL != g_pConfig);
        if (g_pConfig->StartupDelayMS())
        {
            ClrSleepEx(g_pConfig->StartupDelayMS(), FALSE);
        }
#endif

#if USE_DISASSEMBLER
        if ((g_pConfig->GetGCStressLevel() & (EEConfig::GCSTRESS_INSTR_JIT | EEConfig::GCSTRESS_INSTR_NGEN)) != 0)
        {
            Disassembler::StaticInitialize();
            if (!Disassembler::IsAvailable())
            {
                minipal_log_print_error("External disassembler is not available.\n");
                IfFailGo(E_FAIL);
            }
        }
#endif // USE_DISASSEMBLER

        // Monitors, Crsts, and SimpleRWLocks all use the same spin heuristics
        // Cache the (potentially user-overridden) values now so they are accessible from asm routines
        InitializeSpinConstants();

        StubManager::InitializeStubManagers();

        // Set up the cor handle map. This map is used to load assemblies in
        // memory instead of using the normal system load
        PEImage::Startup();

        AccessCheckOptions::Startup();

        CoreLibBinder::Startup();

        StubLinkerCPU::Init();
        StubPrecode::StaticInitialize();
        FixupPrecode::StaticInitialize();
        CDacPlatformMetadata::InitPrecodes();

        InitializeGarbageCollector();

        if (!GCHandleUtilities::GetGCHandleManager()->Initialize())
        {
            IfFailGo(E_OUTOFMEMORY);
        }

        g_pEEShutDownEvent = new CLREvent();
        g_pEEShutDownEvent->CreateManualEvent(FALSE);

        VirtualCallStubManager::InitStatic();

        // Setup the domains. Threads are started in a default domain.

        // Static initialization
        SystemDomain::Attach();

        COMDelegate::Init();

        ExecutionManager::Init();

        JitHost::Init();

#ifndef TARGET_UNIX
        if (!RegisterOutOfProcessWatsonCallbacks())
        {
            IfFailGo(E_FAIL);
        }
#endif // !TARGET_UNIX

#ifdef DEBUGGING_SUPPORTED
        // Initialize the debugging services. This must be done before any
        // EE thread objects are created, and before any classes or
        // modules are loaded.
        InitializeDebugger(); // throws on error
#endif // DEBUGGING_SUPPORTED

#ifdef PROFILING_SUPPORTED
        // Initialize the profiling services.
        // This must happen before Thread::HasStarted() that fires profiler notifications is called on the finalizer thread.
        hr = ProfilingAPIUtility::InitializeProfiling();

        _ASSERTE(SUCCEEDED(hr));
        IfFailGo(hr);
#endif // PROFILING_SUPPORTED

#ifdef TARGET_WINDOWS
        // Create the finalizer thread on windows earlier, as we will need to wait for
        // the completion of its initialization part that initializes COM as that has to be done
        // before the first Thread is attached. Thus we want to give the thread a bit more time.
        FinalizerThread::FinalizerThreadCreate();
#endif

        InitPreStubManager();

#ifdef FEATURE_COMINTEROP
        InitializeComInterop();
#endif // FEATURE_COMINTEROP

        StubHelpers::Init();

        // Before setting up the execution manager initialize the first part
        // of the JIT helpers.
        InitJITAllocationHelpers();
        InitJITWriteBarrierHelpers();

        // Set up the sync block
        SyncBlockCache::Start();

        // This isn't done as part of InitializeGarbageCollector() above because it
        // requires write barriers to have been set up on x86, which happens as part
        // of InitJITWriteBarrierHelpers.
        hr = g_pGCHeap->Initialize();
        if (FAILED(hr))
        {
            LogErrorToHost("GC heap initialization failed with error 0x%08X", hr);
        }

        IfFailGo(hr);

        InitializeExceptionHandling();

        //
        // Install our global exception filter
        //
        if (!InstallUnhandledExceptionFilter())
        {
            IfFailGo(E_FAIL);
        }

#ifdef TARGET_WINDOWS
        // g_pGCHeap->Initialize() above could take nontrivial time, so by now the finalizer thread
        // should have initialized FLS slot for thread cleanup notifications.
        // And ensured that COM is initialized (must happen before allocating FLS slot).
        // Make sure that this was done before we start creating Thread objects
        // Ex: The call to SetupThread below will create and attach a Thread object.
        //     Event pipe might also do that.
        FinalizerThread::WaitForFinalizerThreadStart();
#endif

        // throws on error
        _ASSERTE(GetThreadNULLOk() == NULL);
        SetupThread();

#ifdef DEBUGGING_SUPPORTED
        // Notify debugger once the first thread is created to finish initialization.
        if (g_pDebugInterface != NULL)
        {
            g_pDebugInterface->StartupPhase2(GetThread());
        }
#endif

#ifndef TARGET_WINDOWS
        // This isn't done as part of InitializeGarbageCollector() above because
        // debugger must be initialized before creating EE thread objects
        FinalizerThread::FinalizerThreadCreate();
#else
        // On windows the finalizer thread is already partially created and is waiting
        // right before doing HasStarted(). We will release it now.
        FinalizerThread::EnableFinalization();
#endif

#ifdef FEATURE_PERFTRACING
        // Finish setting up rest of EventPipe - specifically enable SampleProfiler if it was requested at startup.
        // SampleProfiler needs to cooperate with the GC which hasn't fully finished setting up in the first part of the
        // EventPipe initialization, so this is done after the GC has been fully initialized.
        EventPipeAdapter::FinishInitialize();
#endif // FEATURE_PERFTRACING
        GenAnalysis::Initialize();

        // Now we really have fully initialized the garbage collector
        SetGarbageCollectorFullyInitialized();

#ifdef DEBUGGING_SUPPORTED
        // Make a call to publish the DefaultDomain for the debugger
        // This should be done before assemblies/modules are loaded into it (i.e. SystemDomain::Init)
        // and after its OK to switch GC modes and synchronize for sending events to the debugger.
        // @dbgtodo  synchronization: this can probably be simplified in V3
        LOG((LF_CORDB | LF_SYNC | LF_STARTUP, LL_INFO1000, "EEStartup: adding default domain 0x%x\n",
             SystemDomain::System()->DefaultDomain()));
        SystemDomain::System()->PublishAppDomainAndInformDebugger(SystemDomain::System()->DefaultDomain());
#endif

#ifdef HAVE_GCCOVER
        MethodDesc::Init();
#endif

        Assembly::Initialize();

        SystemDomain::System()->Init();

#ifdef PROFILING_SUPPORTED
        // <TODO>This is to compensate for the DefaultDomain workaround contained in
        // SystemDomain::Attach in which the first user domain is created before profiling
        // services can be initialized.  Profiling services cannot be moved to before the
        // workaround because it needs SetupThread to be called.</TODO>

        SystemDomain::NotifyProfilerStartup();
#endif // PROFILING_SUPPORTED

        g_fEEInit = false;

        SystemDomain::System()->DefaultDomain()->LoadSystemAssemblies();

        SystemDomain::System()->DefaultDomain()->SetupSharedStatics();

        InitializeThreadStaticData();

#ifdef FEATURE_MINIMETADATA_IN_TRIAGEDUMPS
        // retrieve configured max size for the mini-metadata buffer (defaults to 64KB)
        g_MiniMetaDataBuffMaxSize = CLRConfig::GetConfigValue(CLRConfig::INTERNAL_MiniMdBufferCapacity);
        // align up to GetOsPageSize(), with a maximum of 1 MB
        g_MiniMetaDataBuffMaxSize = (DWORD) min(ALIGN_UP(g_MiniMetaDataBuffMaxSize, GetOsPageSize()), (DWORD)(1024 * 1024));
        // allocate the buffer. this is never touched while the process is running, so it doesn't
        // contribute to the process' working set. it is needed only as a "shadow" for a mini-metadata
        // buffer that will be set up and reported / updated in the Watson process (the
        // DacStreamsManager class coordinates this)
        g_MiniMetaDataBuffAddress = (TADDR) ClrVirtualAlloc(NULL,
                                                g_MiniMetaDataBuffMaxSize, MEM_COMMIT, PAGE_READWRITE);
#endif // FEATURE_MINIMETADATA_IN_TRIAGEDUMPS

        g_fEEStarted = TRUE;
        g_EEStartupStatus = S_OK;
        hr = S_OK;
        STRESS_LOG0(LF_STARTUP, LL_ALWAYS, "===================EEStartup Completed===================");


#ifdef _DEBUG

        //if g_fEEStarted was false when we loaded the System Module, we did not run ExpandAll on it.  In
        //this case, make sure we run ExpandAll here.  The rationale is that if we Jit before g_fEEStarted
        //is true, we can't initialize Com, so we can't jit anything that uses Com types.  Also, it's
        //probably not safe to Jit while g_fEEStarted is false.
        //
        //Also, if you run this it's possible we'll call CoInitialize, which defaults to MTA.  This might
        //mess up an application that uses STA.  However, this mode is only supported for certain limited
        //jit testing scenarios, so it can live with the limitation.
        if (g_pConfig->ExpandModulesOnLoad())
        {
            SystemDomain::SystemModule()->ExpandAll();
        }
#endif // _DEBUG


ErrExit: ;
    }
    EX_CATCH
    {
        hr = GET_EXCEPTION()->GetHR();
        RethrowTerminalExceptionsWithInitCheck();
    }
    EX_END_CATCH

    if (!g_fEEStarted) {
        if (g_fEEInit)
            g_fEEInit = false;

        if (!FAILED(hr))
            hr = E_FAIL;

        g_EEStartupStatus = hr;
    }

    if (breakOnEELoad.val(CLRConfig::UNSUPPORTED_BreakOnEELoad) == 2)
    {
#ifdef _DEBUG
        _ASSERTE(!"Done loading EE!");
#else
        DebugBreak();
#endif
    }

}

LONG FilterStartupException(PEXCEPTION_POINTERS p, PVOID pv)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
        PRECONDITION(CheckPointer(p));
        PRECONDITION(CheckPointer(pv));
    } CONTRACTL_END;

    g_EEStartupStatus = (HRESULT)p->ExceptionRecord->ExceptionInformation[0];

    // Make sure we got a failure code in this case
    if (!FAILED(g_EEStartupStatus))
        g_EEStartupStatus = E_FAIL;

    // Initializations has failed so reset the g_fEEInit flag.
    g_fEEInit = false;

    if (p->ExceptionRecord->ExceptionCode == BOOTUP_EXCEPTION_COMPLUS)
    {
        // Don't ever handle the exception in a checked build
#ifndef _DEBUG
        return EXCEPTION_EXECUTE_HANDLER;
#endif
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// EEStartup is responsible for all the one time initialization of the runtime.  Some of the highlights of
// what it does include
//     * Creates the default and shared, appdomains.
//     * Loads System.Private.CoreLib and loads up the fundamental types (System.Object ...)
//
// see code:EEStartup#TableOfContents for more on the runtime in general.
// see code:#EEShutdown for an analogous routine run during shutdown.
//
HRESULT EEStartup()
{
    // Cannot use normal contracts here because of the PAL_TRY.
    STATIC_CONTRACT_NOTHROW;

    _ASSERTE(!g_fEEStarted && !g_fEEInit && SUCCEEDED (g_EEStartupStatus));

    PAL_TRY(PVOID, p, NULL)
    {
        InitializeClrNotifications();
#ifdef TARGET_UNIX
        InitializeJITNotificationTable();
        DacGlobals::Initialize();
#endif

        EEStartupHelper();
    }
    PAL_EXCEPT_FILTER (FilterStartupException)
    {
        // The filter should have set g_EEStartupStatus to a failure HRESULT.
        _ASSERTE(FAILED(g_EEStartupStatus));
    }
    PAL_ENDTRY

    return g_EEStartupStatus;
}



// ---------------------------------------------------------------------------
// %%Function: ForceEEShutdown()
//
// Description: Force the EE to shutdown now.
//
// Note: returns when sca is SCA_ReturnWhenShutdownComplete.
// ---------------------------------------------------------------------------
void ForceEEShutdown(ShutdownCompleteAction sca)
{
    WRAPPER_NO_CONTRACT;

    // Don't bother to take the lock for this case.

    STRESS_LOG0(LF_STARTUP, INFO3, "EEShutdown invoked from ForceEEShutdown");
    EEPolicy::HandleExitProcess(sca);
}

void WaitForEndOfShutdown()
{
    CONTRACTL{
        NOTHROW;
        GC_NOTRIGGER;
        MODE_PREEMPTIVE;
    } CONTRACTL_END;

    // We are shutting down.  GC triggers does not have any effect now.
    CONTRACT_VIOLATION(GCViolation);

    Thread *pThread = GetThreadNULLOk();
    // After a thread is blocked in WaitForEndOfShutdown, the thread should not enter runtime again,
    // and block at WaitForEndOfShutdown again.
    if (pThread)
    {
        _ASSERTE(!pThread->HasThreadStateNC(Thread::TSNC_BlockedForShutdown));
        pThread->SetThreadStateNC(Thread::TSNC_BlockedForShutdown);
    }

    for (;;) g_pEEShutDownEvent->Wait(INFINITE, TRUE);
}

// ---------------------------------------------------------------------------
// Function: EEShutDownHelper(BOOL fIsDllUnloading)
//
// The real meat of shut down happens here.  See code:#EEShutDown for details, including
// what fIsDllUnloading means.
//
void STDMETHODCALLTYPE EEShutDownHelper(BOOL fIsDllUnloading)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    } CONTRACTL_END;

    // Used later for a callback.
    CEEInfo ceeInf;

#ifdef FEATURE_PGO
    EX_TRY
    {
        PgoManager::Shutdown();
    }
    EX_CATCH
    {
    }
    EX_END_CATCH
#endif

    if (!fIsDllUnloading)
    {
        ETW::EnumerationLog::ProcessShutdown();

#ifdef FEATURE_PERFTRACING
        EventPipeAdapter::Shutdown();
        DiagnosticServerAdapter::Shutdown();
#endif // FEATURE_PERFTRACING
    }

#if defined(FEATURE_COMINTEROP)
    // Get the current thread.
    Thread * pThisThread = GetThreadNULLOk();
#endif

    if (IsDbgHelperSpecialThread())
    {
        // Our debugger helper thread does not allow Thread object to be set up.
        // We should not run shutdown code on debugger helper thread.
        _ASSERTE(fIsDllUnloading);
        return;
    }

    STRESS_LOG1(LF_STARTUP, LL_INFO10, "EEShutDown entered unloading = %d", fIsDllUnloading);

#ifdef _DEBUG
    if (CLRConfig::GetConfigValue(CLRConfig::INTERNAL_BreakOnEEShutdown))
        _ASSERTE(!"Shutting down EE!");
#endif

#ifdef DEBUGGING_SUPPORTED
    // This is a nasty, terrible, horrible thing. If we're being
    // called from our DLL main, then the odds are good that our DLL
    // main has been called as the result of some person calling
    // ExitProcess. That rips the debugger helper thread away very
    // ungracefully. This check is an attempt to recognize that case
    // and avoid the impending hang when attempting to get the helper
    // thread to do things for us.
    if ((g_pDebugInterface != NULL) && IsAtProcessExit())
        g_pDebugInterface->EarlyHelperThreadDeath();
#endif // DEBUGGING_SUPPORTED

    EX_TRY
    {
        ClrFlsSetThreadType(ThreadType_Shutdown);

        if (fIsDllUnloading && g_fEEShutDown)
        {
            // I'm in the final shutdown and the first part has already been run.
            goto part2;
        }

        // Indicate the EE is the shut down phase.
        g_fEEShutDown |= ShutDown_Start;

        if (!IsAtProcessExit() && !g_fFastExitProcess)
        {
            // Wait for the finalizer thread to deliver process exit event
            GCX_PREEMP();
            FinalizerThread::RaiseShutdownEvents();
        }

        // Ok.  Let's stop the EE.
        if (!IsAtProcessExit())
        {
            // Convert key locks into "shutdown" mode. A lock in shutdown mode means:
            // - Only the finalizer/helper/shutdown threads will be able to take the lock.
            // - Any other thread that tries takes it will just get redirected to an endless WaitForEndOfShutdown().
            //
            // The only managed code that should run after this point is the finalizers for shutdown.
            // We convert locks needed for running + debugging such finalizers. Since such locks may need to be
            // juggled between multiple threads (finalizer/helper/shutdown), no single thread can take the
            // lock and not give it up.
            //
            // Each lock needs its own shutdown flag (they can't all be converted at once).
            // To avoid deadlocks, we need to convert locks in order of crst level (biggest first).

            // Notify the debugger that we're going into shutdown to convert debugger-lock to shutdown.
            if (g_pDebugInterface != NULL)
            {
                g_pDebugInterface->LockDebuggerForShutdown();
            }
        }

#ifdef FEATURE_EVENT_TRACE
        // Flush managed object allocation logging data.
        // We do this after finalization is complete and returning threads have been trapped, so that
        // no there will be no more managed allocations and no more GCs which will manipulate the
        // allocation sampling data structures.
        ETW::TypeSystemLog::FlushObjectAllocationEvents();
#endif // FEATURE_EVENT_TRACE

#ifdef FEATURE_PERFMAP
        // Flush and close the perf map file.
        PerfMap::Disable();
#endif

        ceeInf.JitProcessShutdownWork();  // Do anything JIT-related that needs to happen at shutdown.

        VirtualCallStubManager::LogFinalStats();

#ifdef PROFILING_SUPPORTED
        // If profiling is enabled, then notify of shutdown first so that the
        // profiler can make any last calls it needs to.  Do this only if we
        // are not detaching

        // NOTE: We haven't stopped other threads at this point and nothing is stopping
        // callbacks from coming into the profiler even after Shutdown() has been called.
        // See https://github.com/dotnet/runtime/issues/11885 for an example of how that
        // happens.
        //
        // To prevent issues when profilers are attached we intentionally skip freeing the
        // profiler here. Since there is no guarantee that the profiler won't be accessed after
        // we free it (e.g. through callbacks or ELT hooks), we can't safely free the profiler.
        if (CORProfilerPresent())
        {
            // Don't call back in to the profiler if we are being torn down, it might be unloaded
            if (!fIsDllUnloading)
            {
                BEGIN_PROFILER_CALLBACK(CORProfilerPresent());
                GCX_PREEMP();
                (&g_profControlBlock)->Shutdown();
                END_PROFILER_CALLBACK();
            }
        }
#endif // PROFILING_SUPPORTED


        {
            // From here on out we might call stuff that violates mode requirements, but we ignore these
            // because we are shutting down.
            CONTRACT_VIOLATION(ModeViolation);

#ifdef FEATURE_COMINTEROP
            // We need to call CoUninitialize in part one to ensure orderly shutdown of COM dlls.
            if (!g_fFastExitProcess)
            {
                if (pThisThread!= NULL)
                {
                    pThisThread->CoUninitialize();
                }
            }
#endif // FEATURE_COMINTEROP
        }

        // This is the end of Part 1.

part2:
        // If process shutdown is in progress and Crst locks to be used in shutdown phase 2
        // are already in use, then skip phase 2. This will happen only when those locks
        // are orphaned. In Vista, the penalty for attempting to enter such locks is
        // instant process termination.
        if (IsAtProcessExit())
        {
            // The assert below is a bit too aggressive and has generally brought cases that have been race conditions
            // and not easily reproed to validate a bug. A typical race scenario is when there are two threads,
            // T1 and T2, with T2 having taken a lock (e.g. SystemDomain lock), the OS terminates
            // T2 for some reason. Later, when we enter the shutdown thread, we would assert on such
            // a lock leak, but there is not much we can do since the OS wont notify us prior to thread
            // termination. And this is not even a user bug.
            //
            // Converting it to a STRESS LOG to reduce noise, yet keep things in radar if they need
            // to be investigated.
            //_ASSERTE_MSG(g_ShutdownCrstUsageCount == 0, "Some locks to be taken during shutdown may already be orphaned!");
            if (g_ShutdownCrstUsageCount > 0)
            {
                STRESS_LOG0(LF_STARTUP, LL_INFO10, "Some locks to be taken during shutdown may already be orphaned!");
                goto lDone;
            }
        }

        {
            CONTRACT_VIOLATION(ModeViolation);

            // On the new plan, we only do the tear-down under the protection of the loader
            // lock -- after the OS has stopped all other threads.
            if (fIsDllUnloading && (g_fEEShutDown & ShutDown_Phase2) == 0)
            {
                g_fEEShutDown |= ShutDown_Phase2;

                if (!g_fFastExitProcess)
                {
                    SystemDomain::DetachBegin();
                }


#ifdef DEBUGGING_SUPPORTED
                // Terminate the debugging services.
                TerminateDebugger();
#endif // DEBUGGING_SUPPORTED

                STRESS_LOG0(LF_STARTUP, LL_INFO10, "EEShutdown shutting down logging");

#if 0       // Dont clean up the stress log, so that even at process exit we have a log (after all the process is going away
                if (!g_fFastExitProcess)
                    StressLog::Terminate(TRUE);
#endif

#ifdef LOGGING
                ShutdownLogging();
#endif
                GCHeapUtilities::GetGCHeap()->Shutdown();
            }
        }

    lDone: ;
    }
    EX_CATCH
    {
    }
    EX_END_CATCH

    ClrFlsClearThreadType(ThreadType_Shutdown);
    if (!IsAtProcessExit())
    {
        g_pEEShutDownEvent->Set();
    }
}

// #EEShutDown
//
// Function: EEShutDown(BOOL fIsDllUnloading)
//
// Parameters:
//    BOOL fIsDllUnloading:
//         * TRUE => Called from CLR's DllMain (DLL_PROCESS_DETACH). Not safe point for
//             full cleanup
//         * FALSE => Called some other way (e.g., end of the CLR's main). Safe to do
//             full cleanup.
//
// Description:
//
//     All ee shutdown stuff should be done here. EEShutDown is generally called in one
//     of two ways:
//     * 1. From code:EEPolicy::HandleExitProcess (via HandleExitProcessHelper), with
//         fIsDllUnloading == FALSE. This code path is typically invoked by the CLR's
//         main just falling through to the end. Full cleanup can be performed when
//         EEShutDown is called this way.
//     * 2. From CLR's DllMain (DLL_PROCESS_DETACH), with fIsDllUnloading == TRUE. When
//         called this way, much cleanup code is unsafe to run, and is thus skipped.
//
// Actual shut down logic is factored out to EEShutDownHelper which may be called
// directly by EEShutDown, or indirectly on another thread (see code:#STAShutDown).
//
void STDMETHODCALLTYPE EEShutDown(BOOL fIsDllUnloading)
{
    CONTRACTL {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(g_fEEStarted);
    } CONTRACTL_END;

    // If we have not started runtime successfully, it is not safe to call EEShutDown.
    if (!g_fEEStarted || g_fFastExitProcess == 2)
    {
        return;
    }

    // We only do the first part of the shutdown once.
    static LONG OnlyOne = -1;

    if (!fIsDllUnloading)
    {
        if (InterlockedIncrement(&OnlyOne) != 0)
        {
            // I'm in a regular shutdown -- but another thread got here first.
            // It's a race if I return from here -- I'll call ExitProcess next, and
            // rip things down while the first thread is half-way through a
            // nice cleanup.  Rather than do that, I should just wait until the
            // first thread calls ExitProcess().  I'll die a nice death when that
            // happens.
            GCX_PREEMP_NO_DTOR();
            WaitForEndOfShutdown();
            return;
        }

#ifdef FEATURE_MULTICOREJIT
        MulticoreJitManager::StopProfileAll();
#endif
    }

    if (GetThreadNULLOk())
    {
        GCX_COOP();
        EEShutDownHelper(fIsDllUnloading);
    }
    else
    {
        EEShutDownHelper(fIsDllUnloading);
    }
}

// ---------------------------------------------------------------------------
// %%Function: IsRuntimeActive()
//
// Parameters:
//  none
//
// Returns:
//  TRUE or FALSE
//
// Description: Indicates if the runtime is active or not. "Active" implies
//              that the runtime has started and is in a position to run
//              managed code.
// ---------------------------------------------------------------------------
BOOL IsRuntimeActive()
{
    return (g_fEEStarted);
}


//
// Initialize the Garbage Collector
//

void InitializeGarbageCollector()
{
    CONTRACTL{
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    } CONTRACTL_END;

    HRESULT hr;

    // Build the special Free Object used by the Generational GC
    _ASSERT(g_pFreeObjectMethodTable == NULL);
    g_pFreeObjectMethodTable = (MethodTable *) new BYTE[sizeof(MethodTable)];
    ZeroMemory(g_pFreeObjectMethodTable, sizeof(MethodTable));

    // As the flags in the method table indicate there are no pointers
    // in the object, there is no gc descriptor, and thus no need to adjust
    // the pointer to skip the gc descriptor.

    g_pFreeObjectMethodTable->SetBaseSize(ARRAYBASE_BASESIZE);
    g_pFreeObjectMethodTable->SetComponentSize(1);

    hr = GCHeapUtilities::LoadAndInitialize();

    if (hr != S_OK)
    {
        LogErrorToHost("GC initialization failed with error 0x%08X", hr);
        ThrowHR(hr);
    }

    // Apparently the Windows linker removes global variables if they are never
    // read from, which is a problem for g_gcDacGlobals since it's expected that
    // only the DAC will read from it. This forces the linker to include
    // g_gcDacGlobals.
    volatile void* _dummy = g_gcDacGlobals;
}

/*****************************************************************************/
/* This is here only so that if we get an exception we stop before we catch it */
LONG DllMainFilter(PEXCEPTION_POINTERS p, PVOID pv)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(!"Exception happened in mscorwks!DllMain!");
    return EXCEPTION_EXECUTE_HANDLER;
}

#if !defined(CORECLR_EMBEDDED)

//*****************************************************************************
// This is the part of the old-style DllMain that initializes the
// stuff that the EE team works on. It's called from the real DllMain
// up in MSCOREE land. Separating the DllMain tasks is simply for
// convenience due to the dual build trees.
//*****************************************************************************
BOOL STDMETHODCALLTYPE EEDllMain( // TRUE on success, FALSE on error.
    HINSTANCE   hInst,             // Instance handle of the loaded module.
    DWORD       dwReason,          // Reason for loading.
    LPVOID      lpReserved)        // Unused.
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_TRIGGERS;

    // HRESULT hr;
    // BEGIN_EXTERNAL_ENTRYPOINT(&hr);
    // EE isn't spun up enough to use this macro

    struct Param
    {
        HINSTANCE hInst;
        DWORD dwReason;
        LPVOID lpReserved;
    } param;
    param.hInst = hInst;
    param.dwReason = dwReason;
    param.lpReserved = lpReserved;

    // Can't use PAL_TRY/EX_TRY here as they access the ClrDebugState which gets blown away as part of the
    // PROCESS_DETACH path. Must use special PAL_TRY_FOR_DLLMAIN, passing the reason were in the DllMain.
    PAL_TRY_FOR_DLLMAIN(Param *, pParam, &param, pParam->dwReason)
    {

    switch (pParam->dwReason)
        {
            case DLL_PROCESS_DETACH:
            {
                // lpReserved is NULL if we're here because someone called FreeLibrary
                // and non-null if we're here because the process is exiting.
                // Since nobody should ever be calling FreeLibrary on mscorwks.dll, lpReserved
                // should always be non NULL.
                _ASSERTE(pParam->lpReserved || !g_fEEStarted);
                g_fProcessDetach = TRUE;

                if (g_fEEStarted)
                {
                    if (GCHeapUtilities::IsGCInProgress())
                    {
                        g_fEEShutDown |= ShutDown_Phase2;
                        break;
                    }

                    LOG((LF_STARTUP, INFO3, "EEShutDown invoked from EEDllMain"));
                    EEShutDown(TRUE); // shut down EE if it was started up
                }
                break;
            }
        }

    }
    PAL_EXCEPT_FILTER(DllMainFilter)
    {
    }
    PAL_ENDTRY;

    return TRUE;
}

#endif // !defined(CORECLR_EMBEDDED)

static void RuntimeThreadShutdown(void* thread)
{
    Thread* pThread = (Thread*)thread;
    _ASSERTE(pThread == GetThreadNULLOk());

    if (pThread)
    {
#ifdef FEATURE_COMINTEROP
        // reset the CoInitialize state
        // so we don't call CoUninitialize during thread detach
        pThread->ResetCoInitialized();
#endif // FEATURE_COMINTEROP
        // For case where thread calls ExitThread directly, we need to reset the
        // frame pointer. Otherwise stackwalk would AV. We need to do it in cooperative mode.
        // We need to set m_GCOnTransitionsOK so this thread won't trigger GC when toggle GC mode
        if (pThread->m_pFrame != FRAME_TOP)
        {
#ifdef _DEBUG
            pThread->m_GCOnTransitionsOK = FALSE;
#endif
            GCX_COOP_NO_DTOR();
            pThread->m_pFrame = FRAME_TOP;
            GCX_COOP_NO_DTOR_END();
        }

        pThread->DetachThread(TRUE);
    }
    else
    {
        // Since we don't actually cleanup the TLS data along this path, verify that it is already cleaned up
        AssertThreadStaticDataFreed();
    }

    ThreadDetaching();
}

#ifdef TARGET_WINDOWS

// Index for the fiber local storage of the attached thread pointer
static uint32_t g_flsIndex = FLS_OUT_OF_INDEXES;

#define FLS_STATE_CLEAR 0
#define FLS_STATE_ARMED 1
#define FLS_STATE_INVOKED 2

static PLATFORM_THREAD_LOCAL byte t_flsState;

// This is called when each *fiber* is destroyed. When the home fiber of a thread is destroyed,
// it means that the thread itself is destroyed.
// Since we receive that notification outside of the Loader Lock, it allows us to safely acquire
// the ThreadStore lock in the RuntimeThreadShutdown.
static void __stdcall FiberDetachCallback(void* lpFlsData)
{
    _ASSERTE(g_flsIndex != FLS_OUT_OF_INDEXES);
    _ASSERTE(lpFlsData);

    if (t_flsState == FLS_STATE_ARMED)
    {
        RuntimeThreadShutdown(lpFlsData);
    }

    t_flsState = FLS_STATE_INVOKED;
}

void InitFlsSlot()
{
    // We use fiber detach callbacks to run our thread shutdown code because the fiber detach
    // callback is made without the OS loader lock
    g_flsIndex = FlsAlloc(FiberDetachCallback);
    if (g_flsIndex == FLS_OUT_OF_INDEXES)
    {
        COMPlusThrowWin32();
    }
}

// Register the thread with OS to be notified when thread is about to be destroyed
// It fails fast if a different thread was already registered with the current fiber.
// Parameters:
//  thread        - thread to attach
static void OsAttachThread(void* thread)
{
    _ASSERTE(g_flsIndex != FLS_OUT_OF_INDEXES);

    if (t_flsState == FLS_STATE_INVOKED)
    {
        _ASSERTE_ALL_BUILDS(!"Attempt to execute managed code after the .NET runtime thread state has been destroyed.");
    }

    t_flsState = FLS_STATE_ARMED;

    // Associate the current fiber with the current thread.  This makes the current fiber the thread's "home"
    // fiber.  This fiber is the only fiber allowed to execute managed code on this thread.  When this fiber
    // is destroyed, we consider the thread to be destroyed.
    _ASSERTE(thread != NULL);
    FlsSetValue(g_flsIndex, thread);
}

// Detach thread from OS notifications.
// It fails fast if some other thread value was attached to the current fiber.
// Parameters:
//  thread        - thread to detach
void OsDetachThread(void* thread)
{
    ASSERT(g_flsIndex != FLS_OUT_OF_INDEXES);
    void* threadFromCurrentFiber = FlsGetValue(g_flsIndex);

    if (threadFromCurrentFiber == NULL)
    {
        // Thread is not attached.
        // This could come from DestroyThread called when refcount reaches 0
        // and the thread may have already been detached or never attached.
        // We leave t_flsState as-is to keep track whether our callback has been called.
        return;
    }

    if (threadFromCurrentFiber != thread)
    {
        _ASSERTE_ALL_BUILDS(!"Detaching a thread from the wrong fiber");
    }

    // Leave the existing FLS value, to keep the callback "armed" so that we could observe the termination callback.
    // After that we will not allow to attach as we will no longer be able to clean up.
    t_flsState = FLS_STATE_CLEAR;
}

void EnsureTlsDestructionMonitor()
{
    OsAttachThread(GetThread());
}

#else
struct TlsDestructionMonitor
{
    bool m_activated = false;

    void Activate()
    {
        m_activated = true;
    }

    ~TlsDestructionMonitor()
    {
        if (m_activated)
        {
            RuntimeThreadShutdown(GetThreadNULLOk());
        }
    }
};

// This thread local object is used to detect thread shutdown. Its destructor
// is called when a thread is being shut down.
thread_local TlsDestructionMonitor tls_destructionMonitor;

void EnsureTlsDestructionMonitor()
{
    tls_destructionMonitor.Activate();
}

#endif

#ifdef DEBUGGING_SUPPORTED
//
// InitializeDebugger initialized the Runtime-side CLR Debugging Services
//
static void InitializeDebugger(void)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    // Ensure that if we throw, we'll call TerminateDebugger to cleanup.
    // This makes our Init more atomic by avoiding partially-init states.
    class EnsureCleanup {
        BOOL    fNeedCleanup;
    public:
        EnsureCleanup()
        {
            fNeedCleanup = TRUE;
        }

        void SuppressCleanup()
        {
            fNeedCleanup  = FALSE;
        }

        ~EnsureCleanup()
        {
             STATIC_CONTRACT_NOTHROW;
             STATIC_CONTRACT_GC_NOTRIGGER;
             STATIC_CONTRACT_MODE_ANY;

            if (fNeedCleanup)
            {
                TerminateDebugger();
            }
        }
    } hCleanup;

    HRESULT hr = S_OK;

    LOG((LF_CORDB, LL_INFO10, "Initializing left-side debugging services.\n"));

    FARPROC gi = (FARPROC) &CorDBGetInterface;

    // Init the interface the EE provides to the debugger,
    // ask the debugger for its interface, and if all goes
    // well call Startup on the debugger.
    EEDbgInterfaceImpl::Init();
    _ASSERTE(g_pEEDbgInterfaceImpl != NULL); // throws on OOM

    // This allocates the Debugger object.
    typedef HRESULT __cdecl CORDBGETINTERFACE(DebugInterface**);
    hr = ((CORDBGETINTERFACE*)gi)(&g_pDebugInterface);
    IfFailThrow(hr);

    g_pDebugInterface->SetEEInterface(g_pEEDbgInterfaceImpl);

    {
        hr = g_pDebugInterface->Startup(); // throw on error
        _ASSERTE(SUCCEEDED(hr));

        //
        // If the debug pack is not installed, Startup will return S_FALSE
        // and we should cleanup and proceed without debugging support.
        //
        if (hr != S_OK)
        {
            return;
        }
    }


    LOG((LF_CORDB, LL_INFO10, "Left-side debugging services setup.\n"));

    hCleanup.SuppressCleanup();

    return;
}


//
// TerminateDebugger shuts down the Runtime-side CLR Debugging Services
// InitializeDebugger will call this if it fails.
// This may be called even if the debugger is partially initialized.
// This can be called multiple times.
//
static void TerminateDebugger(void)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    LOG((LF_CORDB, LL_INFO10, "Shutting down left-side debugger services.\n"));

    // If initialized failed really early, then we didn't even get the Debugger object.
    if (g_pDebugInterface != NULL)
    {
        // Notify the out-of-process debugger that shutdown of the in-process debugging support has begun. This is only
        // really used in interop debugging scenarios.
        g_pDebugInterface->ShutdownBegun();

        // This will kill the helper thread, delete the Debugger object, and free all resources.
        g_pDebugInterface->StopDebugger();
    }

    g_CORDebuggerControlFlags = DBCF_NORMAL_OPERATION;

}

#endif // DEBUGGING_SUPPORTED

#ifndef LOCALE_SPARENT
#define LOCALE_SPARENT 0x0000006d
#endif

// ---------------------------------------------------------------------------
// Impl for UtilLoadStringRC Callback: In VM, we let the thread decide culture
// copy culture name into szBuffer and return length
// ---------------------------------------------------------------------------
extern BOOL g_fFatalErrorOccurredOnGCThread;
static HRESULT GetThreadUICultureNames(__inout StringArrayList* pCultureNames)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
        PRECONDITION(CheckPointer(pCultureNames));
    }
    CONTRACTL_END;

    HRESULT hr = S_OK;

    EX_TRY
    {
        InlineSString<LOCALE_NAME_MAX_LENGTH> sCulture;
        InlineSString<LOCALE_NAME_MAX_LENGTH> sParentCulture;

#if 0 // Enable and test if/once the unmanaged runtime is localized
        Thread * pThread = GetThreadNULLOk();

        // When fatal errors have occurred our invariants around GC modes may be broken and attempting to transition to co-op may hang
        // indefinitely. We want to ensure a clean exit so rather than take the risk of hang we take a risk of the error resource not
        // getting localized with a non-default thread-specific culture.
        // A canonical stack trace that gets here is a fatal error in the GC that comes through:
        // coreclr.dll!GetThreadUICultureNames
        // coreclr.dll!CCompRC::LoadLibraryHelper
        // coreclr.dll!CCompRC::LoadLibrary
        // coreclr.dll!CCompRC::GetLibrary
        // coreclr.dll!CCompRC::LoadString
        // coreclr.dll!CCompRC::LoadString
        // coreclr.dll!SString::LoadResourceAndReturnHR
        // coreclr.dll!SString::LoadResourceAndReturnHR
        // coreclr.dll!SString::LoadResource
        // coreclr.dll!EventReporter::EventReporter
        // coreclr.dll!EEPolicy::LogFatalError
        // coreclr.dll!EEPolicy::HandleFatalError
        if (pThread != NULL && !g_fFatalErrorOccurredOnGCThread) {

            // Switch to cooperative mode, since we'll be looking at managed objects
            // and we don't want them moving on us.
            GCX_COOP();

            CULTUREINFOBASEREF pCurrentCulture = (CULTUREINFOBASEREF)Thread::GetCulture(TRUE);

            if (pCurrentCulture != NULL)
            {
                STRINGREF cultureName = pCurrentCulture->GetName();

                if (cultureName != NULL)
                {
                    sCulture.Set(cultureName->GetBuffer(),cultureName->GetStringLength());
                }

                CULTUREINFOBASEREF pParentCulture = pCurrentCulture->GetParent();

                if (pParentCulture != NULL)
                {
                    STRINGREF parentCultureName = pParentCulture->GetName();

                    if (parentCultureName != NULL)
                    {
                        sParentCulture.Set(parentCultureName->GetBuffer(),parentCultureName->GetStringLength());
                    }

                }
            }
        }
#endif

        // If the lazily-initialized cultureinfo structures aren't initialized yet, we'll
        // need to do the lookup the hard way.
        if (sCulture.IsEmpty() || sParentCulture.IsEmpty())
        {
            LocaleIDValue id ;
            int tmp; tmp = GetThreadUICultureId(&id);   // TODO: We should use the name instead
            _ASSERTE(tmp!=0 && id != UICULTUREID_DONTCARE);
            SIZE_T cchParentCultureName=LOCALE_NAME_MAX_LENGTH;
            sCulture.Set(id);

#ifndef TARGET_UNIX
            if (!::GetLocaleInfoEx((LPCWSTR)sCulture, LOCALE_SPARENT, sParentCulture.OpenUnicodeBuffer(static_cast<COUNT_T>(cchParentCultureName)),static_cast<int>(cchParentCultureName)))
            {
                hr = HRESULT_FROM_GetLastError();
            }
            sParentCulture.CloseBuffer();
#else // !TARGET_UNIX
            sParentCulture = sCulture;
#endif // !TARGET_UNIX
        }
        sCulture.Normalize();
        sParentCulture.Normalize();
        pCultureNames->AppendIfNotThere(sCulture);
        pCultureNames->AppendIfNotThere(sParentCulture);
        pCultureNames->Append(SString::Empty());
    }
    EX_CATCH
    {
        hr=E_OUTOFMEMORY;
    }
    EX_END_CATCH

    return hr;
}

// The exit code for the process is communicated in one of two ways.  If the
// entrypoint returns an 'int' we take that.  Otherwise we take a latched
// process exit code.  This can be modified by the app via System.SetExitCode().
static INT32 LatchedExitCode;

void SetLatchedExitCode (INT32 code)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    STRESS_LOG1(LF_SYNC, LL_INFO10, "SetLatchedExitCode = %d\n", code);
    LatchedExitCode = code;
}

INT32 GetLatchedExitCode (void)
{
    LIMITED_METHOD_CONTRACT;
    return LatchedExitCode;
}

// ---------------------------------------------------------------------------
// Impl for UtilLoadStringRC Callback: In VM, we let the thread decide culture
// Return an int uniquely describing which language this thread is using for ui.
// ---------------------------------------------------------------------------
static int GetThreadUICultureId(_Out_ LocaleIDValue* pLocale)
{
    CONTRACTL{
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    } CONTRACTL_END;

    _ASSERTE(sizeof(LocaleIDValue)/sizeof(WCHAR) >= LOCALE_NAME_MAX_LENGTH);

    int Result = 0;

    Thread * pThread = GetThreadNULLOk();

#if 0 // Enable and test if/once the unmanaged runtime is localized
    // When fatal errors have occurred our invariants around GC modes may be broken and attempting to transition to co-op may hang
    // indefinitely. We want to ensure a clean exit so rather than take the risk of hang we take a risk of the error resource not
    // getting localized with a non-default thread-specific culture.
    // A canonical stack trace that gets here is a fatal error in the GC that comes through:
    // coreclr.dll!GetThreadUICultureNames
    // coreclr.dll!CCompRC::LoadLibraryHelper
    // coreclr.dll!CCompRC::LoadLibrary
    // coreclr.dll!CCompRC::GetLibrary
    // coreclr.dll!CCompRC::LoadString
    // coreclr.dll!CCompRC::LoadString
    // coreclr.dll!SString::LoadResourceAndReturnHR
    // coreclr.dll!SString::LoadResourceAndReturnHR
    // coreclr.dll!SString::LoadResource
    // coreclr.dll!EventReporter::EventReporter
    // coreclr.dll!EEPolicy::LogFatalError
    // coreclr.dll!EEPolicy::HandleFatalError
    if (pThread != NULL && !g_fFatalErrorOccurredOnGCThread)
    {

        // Switch to cooperative mode, since we'll be looking at managed objects
        // and we don't want them moving on us.
        GCX_COOP();

        CULTUREINFOBASEREF pCurrentCulture = (CULTUREINFOBASEREF)Thread::GetCulture(TRUE);

        if (pCurrentCulture != NULL)
        {
            STRINGREF currentCultureName = pCurrentCulture->GetName();

            if (currentCultureName != NULL)
            {
                int cchCurrentCultureNameResult = currentCultureName->GetStringLength();
                if (cchCurrentCultureNameResult < LOCALE_NAME_MAX_LENGTH)
                {
                    memcpy(*pLocale, currentCultureName->GetBuffer(), cchCurrentCultureNameResult*sizeof(WCHAR));
                    (*pLocale)[cchCurrentCultureNameResult]='\0';
                    Result=cchCurrentCultureNameResult;
                }
            }
        }
    }
#endif
    if (Result == 0)
    {
#ifndef TARGET_UNIX
        // This thread isn't set up to use a non-default culture. Let's grab the default
        // one and return that.

        Result = ::GetUserDefaultLocaleName(*pLocale, LOCALE_NAME_MAX_LENGTH);

        _ASSERTE(Result != 0);
#else // !TARGET_UNIX
        static const WCHAR enUS[] = W("en-US");
        memcpy(*pLocale, enUS, sizeof(enUS));
        Result = sizeof(enUS);
#endif // !TARGET_UNIX
    }
    return Result;
}

#ifdef ENABLE_CONTRACTS_IMPL

// Returns TRUE if any contract violation suppressions are in effect.
BOOL AreAnyViolationBitsOn()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;
    UINT_PTR violationMask = GetClrDebugState()->ViolationMask();
    violationMask &= ~((UINT_PTR)CanFreeMe);  //CanFreeMe is a borrowed bit and has nothing to do with violations
    if (violationMask & ((UINT_PTR)BadDebugState))
    {
        return FALSE;
    }

    return violationMask != 0;
}


// This function is intentionally invoked inside a big CONTRACT_VIOLATION that turns on every violation
// bit on the map. The dynamic contract at the beginning *should* turn off those violation bits.
// The body of this function tests to see that it did exactly that. This is to prevent the VSWhidbey B#564831 fiasco
// from ever recurring.
void ContractRegressionCheckInner()
{
    // DO NOT TURN THIS CONTRACT INTO A STATIC CONTRACT!!! The very purpose of this function
    // is to ensure that dynamic contracts disable outstanding contract violation bits.
    // This code only runs once at process startup so it's not going pooch the checked build perf.
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        FORBID_FAULT;
        LOADS_TYPE(CLASS_LOAD_BEGIN);
        CANNOT_TAKE_LOCK;
    }
    CONTRACTL_END

    if (AreAnyViolationBitsOn())
    {
        // If we got here, the contract above FAILED to turn off one or more violation bits. This is a
        // huge diagnostics hole and must be fixed immediately.
        _ASSERTE(!("WARNING: mscorwks has detected an internal error that may indicate contracts are"
                   " being silently disabled across the runtime. Do not ignore this assert!"));
    }
}

// This function executes once per process to ensure our CONTRACT_VIOLATION() mechanism
// is properly scope-limited by nested contracts.
void ContractRegressionCheck()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    {
        // DO NOT "FIX" THIS CONTRACT_VIOLATION!!!
        // The existence of this CONTRACT_VIOLATION is not a bug. This is debug-only code specifically written
        // to test the CONTRACT_VIOLATION mechanism itself. This is needed to prevent a regression of
        // B#564831 (which left a huge swath of contracts silently disabled for over six months)
        PERMANENT_CONTRACT_VIOLATION(ThrowsViolation
                                   | GCViolation
                                   | FaultViolation
                                   | LoadsTypeViolation
                                   | TakesLockViolation
                                   , ReasonContractInfrastructure
                                    );
        {
            FAULT_NOT_FATAL();
            ContractRegressionCheckInner();
        }
    }

    if (AreAnyViolationBitsOn())
    {
        // If we got here, the CONTRACT_VIOLATION() holder left one or more violation bits turned ON
        // after we left its scope. This is a huge diagnostic hole and must be fixed immediately.
        _ASSERTE(!("WARNING: mscorwks has detected an internal error that may indicate contracts are"
                   " being silently disabled across the runtime. Do not ignore this assert!"));
    }

}

#endif // ENABLE_CONTRACTS_IMPL

