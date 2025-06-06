// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*++

Module Name:

    file.cpp

Abstract:

    Implementation of the file WIN API for the PAL

--*/

#include "pal/dbgmsg.h"
SET_DEFAULT_DEBUG_CHANNEL(FILE); // some headers have code with asserts, so do this first

#include "pal/thread.hpp"
#include "pal/file.hpp"
#include "pal/stackstring.hpp"

#include "pal/palinternal.h"
#include "pal/file.h"
#include "pal/filetime.h"
#include "pal/utils.h"

#include <time.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

using namespace CorUnix;

int MaxWCharToAcpLengthFactor = 3;

PAL_ERROR
InternalSetFilePointerForUnixFd(
    int iUnixFd,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod,
    PLONG lpNewFilePointerLow
    );

void
CFileProcessLocalDataCleanupRoutine(
    CPalThread *pThread,
    IPalObject *pObjectToCleanup
    );

void
FileCleanupRoutine(
    CPalThread *pThread,
    IPalObject *pObjectToCleanup,
    bool fShutdown
    );

CObjectType CorUnix::otFile(
                otiFile,
                FileCleanupRoutine,
                0,      // No immutable data
                NULL,   // No immutable data copy routine
                NULL,   // No immutable data cleanup routine
                sizeof(CFileProcessLocalData),
                CFileProcessLocalDataCleanupRoutine,
                CObjectType::UnwaitableObject,
                CObjectType::SignalingNotApplicable,
                CObjectType::ThreadReleaseNotApplicable,
                CObjectType::OwnershipNotApplicable
                );

CAllowedObjectTypes CorUnix::aotFile(otiFile);

void
CFileProcessLocalDataCleanupRoutine(
    CPalThread *pThread,
    IPalObject *pObjectToCleanup
    )
{
    PAL_ERROR palError;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;

    palError = pObjectToCleanup->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        ASSERT("Unable to obtain data to cleanup file object");
        return;
    }

    free(pLocalData->unix_filename);

    pLocalDataLock->ReleaseLock(pThread, FALSE);
}

void
FileCleanupRoutine(
    CPalThread *pThread,
    IPalObject *pObjectToCleanup,
    bool fShutdown
    )
{
    PAL_ERROR palError;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;

    palError = pObjectToCleanup->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        ASSERT("Unable to obtain data to cleanup file object");
        return;
    }

    if (!fShutdown && -1 != pLocalData->unix_fd)
    {
        close(pLocalData->unix_fd);
    }

    pLocalDataLock->ReleaseLock(pThread, FALSE);
}

typedef enum
{
  PIID_STDIN_HANDLE,
  PIID_STDOUT_HANDLE,
  PIID_STDERR_HANDLE
} PROCINFO_ID;

#define PAL_LEGAL_FLAGS_ATTRIBS (FILE_ATTRIBUTE_NORMAL| \
                                 FILE_FLAG_SEQUENTIAL_SCAN| \
                                 FILE_FLAG_WRITE_THROUGH| \
                                 FILE_FLAG_NO_BUFFERING| \
                                 FILE_FLAG_RANDOM_ACCESS| \
                                 FILE_FLAG_BACKUP_SEMANTICS)

/* Static global. The init function must be called
before any other functions and if it is not successful,
no other functions should be done. */
static HANDLE pStdIn = INVALID_HANDLE_VALUE;
static HANDLE pStdOut = INVALID_HANDLE_VALUE;
static HANDLE pStdErr = INVALID_HANDLE_VALUE;

/*++
Function :
    FILEGetProperNotFoundError

Returns the proper error code, based on the
Windows behavior.

    IN LPSTR lpPath - The path to check.
    LPDWORD lpErrorCode - The error to set.
*/
void FILEGetProperNotFoundError( LPCSTR lpPath, LPDWORD lpErrorCode )
{
    struct stat stat_data;
    LPSTR lpDupedPath = NULL;
    LPSTR lpLastPathSeparator = NULL;

    TRACE( "FILEGetProperNotFoundError( %s )\n", lpPath?lpPath:"(null)" );

    if ( !lpErrorCode )
    {
        ASSERT( "lpErrorCode has to be valid\n" );
        return;
    }

    if ( NULL == ( lpDupedPath = strdup(lpPath) ) )
    {
        ERROR( "strdup() failed!\n" );
        *lpErrorCode = ERROR_NOT_ENOUGH_MEMORY;
        return;
    }

    /* Determine whether it's a file not found or path not found. */
    lpLastPathSeparator = strrchr( lpDupedPath, '/');
    if ( lpLastPathSeparator != NULL )
    {
        *lpLastPathSeparator = '\0';

        /* If the last path component is a directory,
           we return file not found. If it's a file or
           doesn't exist, we return path not found. */
        if ( '\0' == *lpDupedPath ||
             ( stat( lpDupedPath, &stat_data ) == 0 &&
             ( stat_data.st_mode & S_IFMT ) == S_IFDIR ) )
        {
            TRACE( "ERROR_FILE_NOT_FOUND\n" );
            *lpErrorCode = ERROR_FILE_NOT_FOUND;
        }
        else
        {
            TRACE( "ERROR_PATH_NOT_FOUND\n" );
            *lpErrorCode = ERROR_PATH_NOT_FOUND;
        }
    }
    else
    {
        TRACE( "ERROR_FILE_NOT_FOUND\n" );
        *lpErrorCode = ERROR_FILE_NOT_FOUND;
    }

    free(lpDupedPath);
    lpDupedPath = NULL;
    TRACE( "FILEGetProperNotFoundError returning TRUE\n" );
    return;
}

/*++
Function :
    FILEGetLastErrorFromErrnoAndFilename

Returns the proper error code for errno, or, if errno is ENOENT,
based on the Windows behavior for nonexistent filenames.

    IN LPSTR lpPath - The path to check.
*/
PAL_ERROR FILEGetLastErrorFromErrnoAndFilename(LPCSTR lpPath)
{
    PAL_ERROR palError;
    if (ENOENT == errno)
    {
        FILEGetProperNotFoundError(lpPath, &palError);
    }
    else
    {
        palError = FILEGetLastErrorFromErrno();
    }
    return palError;
}

BOOL
CorUnix::RealPathHelper(LPCSTR lpUnixPath, PathCharString& lpBuffer)
{
    StringHolder lpRealPath;
    lpRealPath = realpath(lpUnixPath, NULL);
    if (lpRealPath.IsNull())
    {
        return FALSE;
    }

    lpBuffer.Set(lpRealPath, strlen(lpRealPath));
    return TRUE;
}
/*++
InternalCanonicalizeRealPath
    Wraps realpath() to hide platform differences. See the man page for
    realpath(3) for details of how realpath() works.

    On systems on which realpath() allows the last path component to not
    exist, this is a straight thunk through to realpath(). On other
    systems, we remove the last path component, then call realpath().

--*/
PAL_ERROR
CorUnix::InternalCanonicalizeRealPath(LPCSTR lpUnixPath, PathCharString& lpBuffer)
{
    PAL_ERROR palError = NO_ERROR;

#if !REALPATH_SUPPORTS_NONEXISTENT_FILES
    StringHolder lpExistingPath;
    LPSTR pchSeparator = NULL;
    LPSTR lpFilename = NULL;
#endif // !REALPATH_SUPPORTS_NONEXISTENT_FILES

    if (lpUnixPath == NULL)
    {
        ERROR ("Invalid argument to InternalCanonicalizeRealPath\n");
        palError = ERROR_INVALID_PARAMETER;
        goto LExit;
    }

#if REALPATH_SUPPORTS_NONEXISTENT_FILES
    RealPathHelper(lpUnixPath, lpBuffer);
#else   // !REALPATH_SUPPORTS_NONEXISTENT_FILES

    lpExistingPath = strdup(lpUnixPath);
    if (lpExistingPath.IsNull())
    {
        ERROR ("strdup failed with error %d\n", errno);
        palError = ERROR_NOT_ENOUGH_MEMORY;
        goto LExit;
    }

    pchSeparator = strrchr(lpExistingPath, '/');
    if (pchSeparator == NULL)
    {
        PathCharString pszCwdBuffer;

        if (GetCurrentDirectoryA(pszCwdBuffer)== 0)
        {
            WARN("getcwd(NULL) failed with error %d\n", errno);
            palError = DIRGetLastErrorFromErrno();
            goto LExit;
        }

        if (!RealPathHelper(pszCwdBuffer, lpBuffer))
        {
            WARN("realpath() failed with error %d\n", errno);
            palError = FILEGetLastErrorFromErrno();
            goto LExit;
        }
        lpFilename = lpExistingPath;
    }
    else if (pchSeparator == lpExistingPath)
    {
        // This is a path in the root i.e. '/tmp'
        // This scenario will probably only come up in WASM where it is normal to
        //  have a cwd of '/' and store files in the root of the virtual filesystem
        lpBuffer.Clear();
        lpBuffer.Append(lpExistingPath, strlen(lpExistingPath));
        return NO_ERROR;
    }
    else
    {
        bool fSetFilename = true;
        // Since realpath implementation cannot handle inexistent filenames,
        // check if we are going to truncate the "/" corresponding to the
        // root folder (e.g. case of "/Volumes"). If so:
        //
        // 1) Set the separator to point to the NULL terminator of the specified
        //    file/folder name.
        //
        // 2) Null terminate lpBuffer
        //
        // 3) Since there is no explicit filename component in lpExistingPath (as
        //    we only have "/" corresponding to the root), set lpFilename to NULL,
        //    alongwith a flag indicating that it has already been set.
        if (pchSeparator == lpExistingPath)
        {
            pchSeparator = lpExistingPath+strlen(lpExistingPath);

            // Set the lpBuffer to NULL
            lpBuffer.Clear();
            lpFilename = NULL;
            fSetFilename = false;
        }
        else
        {
            *pchSeparator = '\0';
        }

        if (!RealPathHelper(lpExistingPath, lpBuffer))
        {
            WARN("realpath() failed with error %d\n", errno);
            palError = FILEGetLastErrorFromErrno();
            goto LExit;
        }

        if (fSetFilename == true)
            lpFilename = pchSeparator + 1;
    }

    if (lpFilename == NULL)
        goto LExit;

    if (!lpBuffer.Append("/",1) || !lpBuffer.Append(lpFilename, strlen(lpFilename)))
    {
        ERROR ("Append failed!\n");
        palError = ERROR_INSUFFICIENT_BUFFER;

        // Doing a goto here since we want to exit now. This will work
        // incase someone else adds another if clause below us.
        goto LExit;
    }

#endif // REALPATH_SUPPORTS_NONEXISTENT_FILES
LExit:

    if ((palError == NO_ERROR) && lpBuffer.IsEmpty())
    {
        // convert all these into ERROR_PATH_NOT_FOUND
        palError = ERROR_PATH_NOT_FOUND;
    }

    return palError;
}

PAL_ERROR
CorUnix::InternalCreateFile(
    CPalThread *pThread,
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile,
    HANDLE *phFile
    )
{
    PAL_ERROR palError = 0;
    IPalObject *pFileObject = NULL;
    IPalObject *pRegisteredFile = NULL;
    IDataLock *pDataLock = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    CObjectAttributes oaFile(NULL, lpSecurityAttributes);
    BOOL fFileExists = FALSE;

    BOOL inheritable = FALSE;
    PathCharString lpUnixPath;
    int   filed = -1;
    int   create_flags = (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    int   open_flags = 0;

    // track whether we've created the file with the intended name,
    // so that it can be removed on failure exit
    BOOL bFileCreated = FALSE;

    const char* szNonfilePrefix = "\\\\.\\";
    PathCharString lpFullUnixPath;

    /* for dwShareMode only three flags are accepted */
    if ( dwShareMode & ~(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE) )
    {
        ASSERT( "dwShareMode is invalid\n" );
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    if ( lpFileName == NULL )
    {
        ERROR("InternalCreateFile called with NULL filename\n");
        palError = ERROR_PATH_NOT_FOUND;
        goto done;
    }

    if ( strncmp(lpFileName, szNonfilePrefix, strlen(szNonfilePrefix)) == 0 )
    {
        ERROR("InternalCreateFile does not support paths beginning with %s\n", szNonfilePrefix);
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    if( !lpUnixPath.Set(lpFileName,strlen(lpFileName)))
    {
        ERROR("strdup() failed\n");
        palError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    // Compute the absolute pathname to the file.  This pathname is used
    // to determine if two file names represent the same file.
    palError = InternalCanonicalizeRealPath(lpUnixPath, lpFullUnixPath);
    if (palError != NO_ERROR)
    {
        goto done;
    }

    lpUnixPath.Set(lpFullUnixPath);

    switch( dwDesiredAccess )
    {
    case 0:
        /* Device Query Access was requested. let's use open() with
           no flags, it's basically the equivalent of O_RDONLY, since
           O_RDONLY is defined as 0x0000 */
        break;
    case( GENERIC_READ ):
        open_flags |= O_RDONLY;
        break;
    case( GENERIC_WRITE ):
        open_flags |= O_WRONLY;
        break;
    case( GENERIC_READ | GENERIC_WRITE ):
        open_flags |= O_RDWR;
        break;
    default:
        ERROR("dwDesiredAccess value of %d is invalid\n", dwDesiredAccess);
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    TRACE("open flags are 0x%lx\n", open_flags);

    if ( lpSecurityAttributes )
    {
        if ( lpSecurityAttributes->nLength != sizeof( SECURITY_ATTRIBUTES ) ||
             lpSecurityAttributes->lpSecurityDescriptor != NULL ||
             !lpSecurityAttributes->bInheritHandle )
        {
            ASSERT("lpSecurityAttributes points to invalid values.\n");
            palError = ERROR_INVALID_PARAMETER;
            goto done;
        }
        inheritable = TRUE;
    }

    if ( (dwFlagsAndAttributes & PAL_LEGAL_FLAGS_ATTRIBS) !=
          dwFlagsAndAttributes)
    {
        ASSERT("Bad dwFlagsAndAttributes\n");
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }
    else if (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS)
    {
        /* Override the open flags, and always open as readonly.  This
           flag is used when opening a directory, to change its
           creation/modification/access times.  On Windows, the directory
           must be open for write, but on Unix, it needs to be readonly. */
        open_flags = O_RDONLY;
    } else {
        struct stat st;

        if (stat(lpUnixPath, &st) == 0 && (st.st_mode & S_IFDIR))
        {
            /* The file exists and it is a directory.  Without
                   FILE_FLAG_BACKUP_SEMANTICS, Win32 CreateFile always fails
                   to open directories. */
                palError = ERROR_ACCESS_DENIED;
                goto done;
        }
    }

    if ( hTemplateFile )
    {
        ASSERT("hTemplateFile is not NULL, as it should be.\n");
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    /* NB: According to MSDN docs, When CREATE_ALWAYS or OPEN_ALWAYS is
       set, CreateFile should SetLastError to ERROR_ALREADY_EXISTS,
       even though/if CreateFile will be successful.
    */
    switch( dwCreationDisposition )
    {
    case( CREATE_ALWAYS ):
        // check whether the file exists
        if ( access( lpUnixPath, F_OK ) == 0 )
        {
            fFileExists = TRUE;
        }
        open_flags |= O_CREAT | O_TRUNC;
        break;
    case( CREATE_NEW ):
        open_flags |= O_CREAT | O_EXCL;
        break;
    case( OPEN_EXISTING ):
        /* don't need to do anything here */
        break;
    case( OPEN_ALWAYS ):
        if ( access( lpUnixPath, F_OK ) == 0 )
        {
            fFileExists = TRUE;
        }
        open_flags |= O_CREAT;
        break;
    case( TRUNCATE_EXISTING ):
        open_flags |= O_TRUNC;
        break;
    default:
        ASSERT("dwCreationDisposition value of %d is not valid\n",
              dwCreationDisposition);
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    if ( dwFlagsAndAttributes & FILE_FLAG_NO_BUFFERING )
    {
        TRACE("I/O will be unbuffered\n");
#ifdef O_DIRECT
        open_flags |= O_DIRECT;
#endif
    }
    else
    {
        TRACE("I/O will be buffered\n");
    }

    filed = InternalOpen(lpUnixPath, open_flags, create_flags);
    TRACE("Allocated file descriptor [%d]\n", filed);

    if ( filed < 0 )
    {
        TRACE("open() failed; error is %s (%d)\n", strerror(errno), errno);
        palError = FILEGetLastErrorFromErrnoAndFilename(lpUnixPath);
        goto done;
    }

    // Deduce whether we created a file in the previous operation (there's a
    // small timing window between the access() used to determine fFileExists
    // and the open() operation, but there's not much we can do about that.
    bFileCreated = (dwCreationDisposition == CREATE_ALWAYS ||
                    dwCreationDisposition == CREATE_NEW ||
                    dwCreationDisposition == OPEN_ALWAYS) &&
        !fFileExists;

#ifndef O_DIRECT
    if ( dwFlagsAndAttributes & FILE_FLAG_NO_BUFFERING )
    {
#ifdef F_NOCACHE
        if (-1 == fcntl(filed, F_NOCACHE, 1))
        {
            ASSERT("Can't set F_NOCACHE; fcntl() failed. errno is %d (%s)\n",
               errno, strerror(errno));
            palError = ERROR_INTERNAL_ERROR;
            goto done;
        }
#elif HAVE_DIRECTIO
        if (-1 == directio(filed, DIRECTIO_ON))
        {
            ASSERT("Can't set DIRECTIO_ON; directio() failed. errno is %d (%s)\n",
               errno, strerror(errno));
            palError = ERROR_INTERNAL_ERROR;
            goto done;
        }
#else
#error Insufficient support for uncached I/O on this platform
#endif
    }
#endif

    /* make file descriptor close-on-exec; inheritable handles will get
      "uncloseonexeced" in CreateProcess if they are actually being inherited*/
    if(-1 == fcntl(filed,F_SETFD, FD_CLOEXEC))
    {
        ASSERT("can't set close-on-exec flag; fcntl() failed. errno is %d "
             "(%s)\n", errno, strerror(errno));
        palError = ERROR_INTERNAL_ERROR;
        goto done;
    }

    palError = g_pObjectManager->AllocateObject(
        pThread,
        &otFile,
        &oaFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        WriteLock,
        &pDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    _ASSERTE(pLocalData->unix_filename == NULL);
    pLocalData->unix_filename = strdup(lpUnixPath);
    if (pLocalData->unix_filename == NULL)
    {
        ASSERT("Unable to copy string\n");
        palError = ERROR_INTERNAL_ERROR;
        goto done;
    }

    pLocalData->inheritable = inheritable;
    pLocalData->unix_fd = filed;
    pLocalData->open_flags = open_flags;
    pLocalData->open_flags_deviceaccessonly = (dwDesiredAccess == 0);

    //
    // We've finished initializing our local data, so release that lock
    //

    pDataLock->ReleaseLock(pThread, TRUE);
    pDataLock = NULL;

    palError = g_pObjectManager->RegisterObject(
        pThread,
        pFileObject,
        &aotFile,
        phFile,
        &pRegisteredFile
        );

    //
    // pFileObject is invalidated by the call to RegisterObject, so NULL it
    // out here to ensure that we don't try to release a reference on
    // it down the line.
    //

    pFileObject = NULL;

done:

    // At this point, if we've been successful, palError will be NO_ERROR.
    // CreateFile can return ERROR_ALREADY_EXISTS in some success cases;
    // those cases are flagged by fFileExists and are handled below.
    if (NO_ERROR != palError)
    {
        if (filed >= 0)
        {
            close(filed);
        }
        if (bFileCreated)
        {
            if (-1 == unlink(lpUnixPath))
            {
                WARN("can't delete file; unlink() failed with errno %d (%s)\n",
                     errno, strerror(errno));
            }
        }
    }

    if (NULL != pDataLock)
    {
        pDataLock->ReleaseLock(pThread, TRUE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    if (NULL != pRegisteredFile)
    {
        pRegisteredFile->ReleaseReference(pThread);
    }

    if (NO_ERROR == palError && fFileExists)
    {
        palError = ERROR_ALREADY_EXISTS;
    }

    return palError;
}

/*++
Function:
  CreateFileA

Note:
  Only bInherit flag is used from the LPSECURITY_ATTRIBUTES struct.
  Desired access is READ, WRITE or 0
  Share mode is READ, WRITE or DELETE

See MSDN doc.
--*/
HANDLE
PALAPI
CreateFileA(
        IN LPCSTR lpFileName,
        IN DWORD dwDesiredAccess,
        IN DWORD dwShareMode,
        IN LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        IN DWORD dwCreationDisposition,
        IN DWORD dwFlagsAndAttributes,
        IN HANDLE hTemplateFile
        )
{
    CPalThread *pThread;
    PAL_ERROR palError = NO_ERROR;
    HANDLE  hRet = INVALID_HANDLE_VALUE;

    PERF_ENTRY(CreateFileA);
    ENTRY("CreateFileA(lpFileName=%p (%s), dwAccess=%#x, dwShareMode=%#x, "
          "lpSecurityAttr=%p, dwDisposition=%#x, dwFlags=%#x, "
          "hTemplateFile=%p )\n",lpFileName?lpFileName:"NULL",lpFileName?lpFileName:"NULL", dwDesiredAccess,
          dwShareMode, lpSecurityAttributes, dwCreationDisposition,
          dwFlagsAndAttributes, hTemplateFile);

    pThread = InternalGetCurrentThread();

    palError = InternalCreateFile(
        pThread,
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile,
        &hRet
        );

    //
    // We always need to set last error, even on success:
    // we need to protect ourselves from the situation
    // where last error is set to ERROR_ALREADY_EXISTS on
    // entry to the function
    //

    pThread->SetLastError(palError);

    LOGEXIT("CreateFileA returns HANDLE %p\n", hRet);
    PERF_EXIT(CreateFileA);
    return hRet;
}


/*++
Function:
  CreateFileW

Note:
  Only bInherit flag is used from the LPSECURITY_ATTRIBUTES struct.
  Desired access is READ, WRITE or 0
  Share mode is READ, WRITE or DELETE

See MSDN doc.
--*/
HANDLE
PALAPI
CreateFileW(
        IN LPCWSTR lpFileName,
        IN DWORD dwDesiredAccess,
        IN DWORD dwShareMode,
        IN LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        IN DWORD dwCreationDisposition,
        IN DWORD dwFlagsAndAttributes,
        IN HANDLE hTemplateFile)
{
    CPalThread *pThread;
    PAL_ERROR palError = NO_ERROR;
    PathCharString namePathString;
    char * name;
    int size;
    int length = 0;
    HANDLE  hRet = INVALID_HANDLE_VALUE;

    PERF_ENTRY(CreateFileW);
    ENTRY("CreateFileW(lpFileName=%p (%S), dwAccess=%#x, dwShareMode=%#x, "
          "lpSecurityAttr=%p, dwDisposition=%#x, dwFlags=%#x, hTemplateFile=%p )\n",
          lpFileName?lpFileName:W16_NULLSTRING,
          lpFileName?lpFileName:W16_NULLSTRING, dwDesiredAccess, dwShareMode,
          lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
          hTemplateFile);

    pThread = InternalGetCurrentThread();

    if (lpFileName != NULL)
    {
        length = (PAL_wcslen(lpFileName)+1) * MaxWCharToAcpLengthFactor;
    }

    name = namePathString.OpenStringBuffer(length);
    if (NULL == name)
    {
        palError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    size = WideCharToMultiByte( CP_ACP, 0, lpFileName, -1, name, length,
                                NULL, NULL );

    if( size == 0 )
    {
        namePathString.CloseBuffer(0);
        DWORD dwLastError = GetLastError();
        ASSERT("WideCharToMultiByte failure! error is %d\n", dwLastError);
        palError = ERROR_INTERNAL_ERROR;
        goto done;
    }

    namePathString.CloseBuffer(size - 1);

    palError = InternalCreateFile(
        pThread,
        name,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile,
        &hRet
        );

    //
    // We always need to set last error, even on success:
    // we need to protect ourselves from the situation
    // where last error is set to ERROR_ALREADY_EXISTS on
    // entry to the function
    //

done:
	pThread->SetLastError(palError);
    LOGEXIT( "CreateFileW returns HANDLE %p\n", hRet );
    PERF_EXIT(CreateFileW);
    return hRet;
}


/*++
Function:
  DeleteFileA

See MSDN doc.
--*/
BOOL
PALAPI
DeleteFileA(
        IN LPCSTR lpFileName)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;
    int     result;
    BOOL    bRet = FALSE;
    DWORD   dwLastError = 0;
    PathCharString lpunixFileName;
    PathCharString lpFullunixFileName;

    PERF_ENTRY(DeleteFileA);
    ENTRY("DeleteFileA(lpFileName=%p (%s))\n", lpFileName?lpFileName:"NULL", lpFileName?lpFileName:"NULL");

    pThread = InternalGetCurrentThread();

    if( !lpunixFileName.Set(lpFileName, strlen(lpFileName)))
    {
        palError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    // Compute the absolute pathname to the file.  This pathname is used
    // to determine if two file names represent the same file.
    palError = InternalCanonicalizeRealPath(lpunixFileName, lpFullunixFileName);
    if (palError != NO_ERROR)
    {
        if (!lpFullunixFileName.Set(lpunixFileName, strlen(lpunixFileName)))
        {
            palError = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }
    }

    result = unlink( lpFullunixFileName );

    if (result < 0)
    {
        TRACE("unlink returns %d\n", result);
        dwLastError = FILEGetLastErrorFromErrnoAndFilename(lpFullunixFileName);
    }
    else
    {
        bRet = TRUE;
    }

done:
    if(dwLastError)
    {
        pThread->SetLastError( dwLastError );
    }

    LOGEXIT("DeleteFileA returns BOOL %d\n", bRet);
    PERF_EXIT(DeleteFileA);
    return bRet;
}

/*++
InternalOpen

Wrapper for open.

Input parameters:

szPath = pointer to a pathname of a file to be opened
nFlags = arguments that control how the file should be accessed
mode = file permission settings that are used only when a file is created

Return value:
    File descriptor on success, -1 on failure
--*/
int
CorUnix::InternalOpen(
    const char *szPath,
    int nFlags,
    ...
    )
{
    int nRet = -1;
    int mode = 0;
    va_list ap;

    // If nFlags does not contain O_CREAT, the mode parameter will be ignored.
    if (nFlags & O_CREAT)
    {
        va_start(ap, nFlags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    do
    {
#if OPEN64_IS_USED_INSTEAD_OF_OPEN
        nRet = open64(szPath, nFlags, mode);
#else
        nRet = open(szPath, nFlags, mode);
#endif
    }
    while ((nRet == -1) && (errno == EINTR));

    return nRet;
}

PAL_ERROR
CorUnix::InternalWriteFile(
    CPalThread *pThread,
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped
    )
{
    PAL_ERROR palError = 0;
    IPalObject *pFileObject = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;
    int ifd;
    int res;

    if (NULL != lpNumberOfBytesWritten)
    {
        //
        // This must be set to 0 before any other error checking takes
        // place, per MSDN
        //

        *lpNumberOfBytesWritten = 0;
    }
    else
    {
        ASSERT( "lpNumberOfBytesWritten is NULL\n" );
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    // Win32 WriteFile disallows writing to STD_INPUT_HANDLE
    if (hFile == INVALID_HANDLE_VALUE || hFile == pStdIn)
    {
        palError = ERROR_INVALID_HANDLE;
        goto done;
    }
    else if ( lpOverlapped )
    {
        ASSERT( "lpOverlapped is not NULL, as it should be.\n" );
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    palError = g_pObjectManager->ReferenceObjectByHandle(
        pThread,
        hFile,
        &aotFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    if (pLocalData->open_flags_deviceaccessonly == TRUE)
    {
        ERROR("File open for device access only\n");
        palError = ERROR_ACCESS_DENIED;
        goto done;
    }

    ifd = pLocalData->unix_fd;

    //
    // Release the data lock before performing the (possibly blocking)
    // write call
    //

    pLocalDataLock->ReleaseLock(pThread, FALSE);
    pLocalDataLock = NULL;
    pLocalData = NULL;

#if WRITE_0_BYTES_HANGS_TTY
    if( nNumberOfBytesToWrite == 0 && isatty(ifd) )
    {
        res = 0;
        *lpNumberOfBytesWritten = 0;
        goto done;
    }
#endif

    res = write( ifd, lpBuffer, nNumberOfBytesToWrite );
    TRACE("write() returns %d\n", res);

    if ( res >= 0 )
    {
        *lpNumberOfBytesWritten = res;
    }
    else
    {
        palError = FILEGetLastErrorFromErrno();
    }

done:

    if (NULL != pLocalDataLock)
    {
        pLocalDataLock->ReleaseLock(pThread, FALSE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    return palError;
}


/*++
Function:
  WriteFileW

Note:
  lpOverlapped always NULL.

See MSDN doc.
--*/
BOOL
PALAPI
WriteFile(
      IN HANDLE hFile,
      IN LPCVOID lpBuffer,
      IN DWORD nNumberOfBytesToWrite,
      OUT LPDWORD lpNumberOfBytesWritten,
      IN LPOVERLAPPED lpOverlapped)
{
    PAL_ERROR palError;
    CPalThread *pThread;

    PERF_ENTRY(WriteFile);
    ENTRY("WriteFile(hFile=%p, lpBuffer=%p, nToWrite=%u, lpWritten=%p, "
          "lpOverlapped=%p)\n", hFile, lpBuffer, nNumberOfBytesToWrite,
          lpNumberOfBytesWritten, lpOverlapped);

    pThread = InternalGetCurrentThread();

    palError = InternalWriteFile(
        pThread,
        hFile,
        lpBuffer,
        nNumberOfBytesToWrite,
        lpNumberOfBytesWritten,
        lpOverlapped
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }

    LOGEXIT("WriteFile returns BOOL %d\n", NO_ERROR == palError);
    PERF_EXIT(WriteFile);
    return NO_ERROR == palError;
}

PAL_ERROR
CorUnix::InternalReadFile(
    CPalThread *pThread,
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped
    )
{
    PAL_ERROR palError = 0;
    IPalObject *pFileObject = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;
    int ifd;
    int res;

    if (NULL != lpNumberOfBytesRead)
    {
        //
        // This must be set to 0 before any other error checking takes
        // place, per MSDN
        //

        *lpNumberOfBytesRead = 0;
    }
    else
    {
        ERROR( "lpNumberOfBytesRead is NULL\n" );
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    if (INVALID_HANDLE_VALUE == hFile)
    {
        ERROR( "Invalid file handle\n" );
        palError = ERROR_INVALID_HANDLE;
        goto done;
    }
    else if (NULL != lpOverlapped)
    {
        ASSERT( "lpOverlapped is not NULL, as it should be.\n" );
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }
    else if (NULL == lpBuffer)
    {
        ERROR( "Invalid parameter. (lpBuffer:%p)\n", lpBuffer);
        palError = ERROR_NOACCESS;
        goto done;
    }

    palError = g_pObjectManager->ReferenceObjectByHandle(
        pThread,
        hFile,
        &aotFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    if (pLocalData->open_flags_deviceaccessonly == TRUE)
    {
        ERROR("File open for device access only\n");
        palError = ERROR_ACCESS_DENIED;
        goto done;
    }

    ifd = pLocalData->unix_fd;

    //
    // Release the data lock before performing the (possibly blocking)
    // read call
    //

    pLocalDataLock->ReleaseLock(pThread, FALSE);
    pLocalDataLock = NULL;
    pLocalData = NULL;

Read:
    TRACE("Reading from file descriptor %d\n", ifd);
    res = read(ifd, lpBuffer, nNumberOfBytesToRead);
    TRACE("read() returns %d\n", res);

    if (res >= 0)
    {
        *lpNumberOfBytesRead = res;
    }
    else if (errno == EINTR)
    {
        // Try to read again.
        goto Read;
    }
    else
    {
        palError = FILEGetLastErrorFromErrno();
    }

done:

    if (NULL != pLocalDataLock)
    {
        pLocalDataLock->ReleaseLock(pThread, FALSE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    return palError;
}

/*++
Function:
  ReadFile

Note:
  lpOverlapped always NULL.

See MSDN doc.
--*/
BOOL
PALAPI
ReadFile(
     IN HANDLE hFile,
     OUT LPVOID lpBuffer,
     IN DWORD nNumberOfBytesToRead,
     OUT LPDWORD lpNumberOfBytesRead,
     IN LPOVERLAPPED lpOverlapped)
{
    PAL_ERROR palError;
    CPalThread *pThread;

    PERF_ENTRY(ReadFile);
    ENTRY("ReadFile(hFile=%p, lpBuffer=%p, nToRead=%u, "
          "lpRead=%p, lpOverlapped=%p)\n",
          hFile, lpBuffer, nNumberOfBytesToRead,
          lpNumberOfBytesRead, lpOverlapped);

    pThread = InternalGetCurrentThread();

    palError = InternalReadFile(
        pThread,
        hFile,
        lpBuffer,
        nNumberOfBytesToRead,
        lpNumberOfBytesRead,
        lpOverlapped
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }

    LOGEXIT("ReadFile returns BOOL %d\n", NO_ERROR == palError);
    PERF_EXIT(ReadFile);
    return NO_ERROR == palError;
}


/*++
Function:
  GetStdHandle

See MSDN doc.
--*/
HANDLE
PALAPI
GetStdHandle(
         IN DWORD nStdHandle)
{
    CPalThread *pThread;
    HANDLE hRet = INVALID_HANDLE_VALUE;

    PERF_ENTRY(GetStdHandle);
    ENTRY("GetStdHandle(nStdHandle=%#x)\n", nStdHandle);

    pThread = InternalGetCurrentThread();
    switch( nStdHandle )
    {
    case STD_INPUT_HANDLE:
        hRet = pStdIn;
        break;
    case STD_OUTPUT_HANDLE:
        hRet = pStdOut;
        break;
    case STD_ERROR_HANDLE:
        hRet = pStdErr;
        break;
    default:
        ERROR("nStdHandle is invalid\n");
        pThread->SetLastError(ERROR_INVALID_PARAMETER);
        break;
    }

    LOGEXIT("GetStdHandle returns HANDLE %p\n", hRet);
    PERF_EXIT(GetStdHandle);
    return hRet;
}

//
// We need to break out the actual mechanics of setting the file pointer
// on the unix FD for InternalReadFile and InternalWriteFile, as they
// need to call this routine in order to determine the value of the
// current file pointer when computing the scope of their transaction
// lock. If we didn't break out this logic we'd end up referencing the file
// handle multiple times, and, in the process, would attempt to recursively
// obtain the local process data lock for the underlying file object.
//

PAL_ERROR
InternalSetFilePointerForUnixFd(
    int iUnixFd,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod,
    PLONG lpNewFilePointerLow
    )
{
    PAL_ERROR palError = NO_ERROR;
    int     seek_whence = 0;
    int64_t seek_offset = 0LL;
    int64_t seek_res = 0LL;
    off_t old_offset;

    switch( dwMoveMethod )
    {
    case FILE_BEGIN:
        seek_whence = SEEK_SET;
        break;
    case FILE_CURRENT:
        seek_whence = SEEK_CUR;
        break;
    case FILE_END:
        seek_whence = SEEK_END;
        break;
    default:
        ERROR("dwMoveMethod = %d is invalid\n", dwMoveMethod);
        palError = ERROR_INVALID_PARAMETER;
        goto done;
    }

    //
    // According to MSDN, if lpDistanceToMoveHigh is not null,
    // lDistanceToMove is treated as unsigned;
    // it is treated as signed otherwise
    //

    if ( lpDistanceToMoveHigh )
    {
        /* set the high 32 bits of the offset */
        seek_offset = ((int64_t)*lpDistanceToMoveHigh << 32);

        /* set the low 32 bits */
        /* cast to unsigned long to avoid sign extension */
        seek_offset |= (ULONG) lDistanceToMove;
    }
    else
    {
        seek_offset |= lDistanceToMove;
    }

    /* store the current position, in case the lseek moves the pointer
       before the beginning of the file */
    old_offset = lseek(iUnixFd, 0, SEEK_CUR);
    if (old_offset == -1)
    {
        ERROR("lseek(fd,0,SEEK_CUR) failed errno:%d (%s)\n",
              errno, strerror(errno));
        palError = ERROR_ACCESS_DENIED;
        goto done;
    }

    // Check to see if we're going to seek to a negative offset.
    // If we're seeking from the beginning or the current mark,
    // this is simple.
    if ((seek_whence == SEEK_SET && seek_offset < 0) ||
        (seek_whence == SEEK_CUR && seek_offset + old_offset < 0))
    {
        palError = ERROR_NEGATIVE_SEEK;
        goto done;
    }
    else if (seek_whence == SEEK_END && seek_offset < 0)
    {
        // We need to determine if we're seeking past the
        // beginning of the file, but we don't want to adjust
        // the mark in the process. stat is the only way to
        // do that.
        struct stat fileData;
        int result;

        result = fstat(iUnixFd, &fileData);
        if (result == -1)
        {
            // It's a bad fd. This shouldn't happen because
            // we've already called lseek on it, but you
            // never know. This is the best we can do.
            palError = ERROR_ACCESS_DENIED;
            goto done;
        }
        if (fileData.st_size < -seek_offset)
        {
            // Seeking past the beginning.
            palError = ERROR_NEGATIVE_SEEK;
            goto done;
        }
    }

    seek_res = (int64_t)lseek( iUnixFd,
                               seek_offset,
                               seek_whence );
    if ( seek_res < 0 )
    {
        /* lseek() returns -1 on error, but also can seek to negative
           file offsets, so -1 can also indicate a successful seek to offset
           -1.  Win32 doesn't allow negative file offsets, so either case
           is an error. */
        ERROR("lseek failed errno:%d (%s)\n", errno, strerror(errno));
        lseek(iUnixFd, old_offset, SEEK_SET);
        palError = ERROR_ACCESS_DENIED;
    }
    else
    {
        /* store high-order DWORD */
        if ( lpDistanceToMoveHigh )
            *lpDistanceToMoveHigh = (DWORD)(seek_res >> 32);

        /* return low-order DWORD of seek result */
        *lpNewFilePointerLow = (DWORD)seek_res;
    }

done:

    return palError;
}

PAL_ERROR
CorUnix::InternalSetFilePointer(
    CPalThread *pThread,
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod,
    PLONG lpNewFilePointerLow
    )
{
    PAL_ERROR palError = NO_ERROR;
    IPalObject *pFileObject = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;

    if (INVALID_HANDLE_VALUE == hFile)
    {
        ERROR( "Invalid file handle\n" );
        palError = ERROR_INVALID_HANDLE;
        goto InternalSetFilePointerExit;
    }

    palError = g_pObjectManager->ReferenceObjectByHandle(
        pThread,
        hFile,
        &aotFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto InternalSetFilePointerExit;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto InternalSetFilePointerExit;
    }

    palError = InternalSetFilePointerForUnixFd(
        pLocalData->unix_fd,
        lDistanceToMove,
        lpDistanceToMoveHigh,
        dwMoveMethod,
        lpNewFilePointerLow
        );

InternalSetFilePointerExit:

    if (NULL != pLocalDataLock)
    {
        pLocalDataLock->ReleaseLock(pThread, FALSE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    return palError;
}

/*++
Function:
  SetFilePointer

See MSDN doc.
--*/
DWORD
PALAPI
SetFilePointer(
           IN HANDLE hFile,
           IN LONG lDistanceToMove,
           IN PLONG lpDistanceToMoveHigh,
           IN DWORD dwMoveMethod)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;
    LONG lNewFilePointerLow = 0;

    PERF_ENTRY(SetFilePointer);
    ENTRY("SetFilePointer(hFile=%p, lDistance=%d, lpDistanceHigh=%p, "
          "dwMoveMethod=%#x)\n", hFile, lDistanceToMove,
          lpDistanceToMoveHigh, dwMoveMethod);

    pThread = InternalGetCurrentThread();

    palError = InternalSetFilePointer(
        pThread,
        hFile,
        lDistanceToMove,
        lpDistanceToMoveHigh,
        dwMoveMethod,
        &lNewFilePointerLow
        );

    if (NO_ERROR != palError)
    {
        lNewFilePointerLow = INVALID_SET_FILE_POINTER;
    }

    /* This function must always call SetLastError - even if successful.
       If we seek to a value greater than 2^32 - 1, we will effectively be
       returning a negative value from this function. Now, let's say that
       returned value is -1. Furthermore, assume that win32error has been
       set before even entering this function. Then, when this function
       returns to SetFilePointer in win32native.cs, it will have returned
       -1 and win32error will have been set, which will cause an error to be
       returned. Since -1 may not be an error in this case and since we
       can't assume that the win32error is related to SetFilePointer,
       we need to always call SetLastError here. That way, if this function
       succeeds, SetFilePointer in win32native won't mistakenly determine
       that it failed. */
    pThread->SetLastError(palError);

    LOGEXIT("SetFilePointer returns DWORD %#x\n", lNewFilePointerLow);
    PERF_EXIT(SetFilePointer);
    return lNewFilePointerLow;
}

/*++
Function:
  SetFilePointerEx

See MSDN doc.
--*/
BOOL
PALAPI
SetFilePointerEx(
           IN HANDLE hFile,
           IN LARGE_INTEGER liDistanceToMove,
           OUT PLARGE_INTEGER lpNewFilePointer,
           IN DWORD dwMoveMethod)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;
    BOOL Ret = FALSE;

    PERF_ENTRY(SetFilePointerEx);
    ENTRY("SetFilePointerEx(hFile=%p, liDistanceToMove=0x%llx, "
           "lpNewFilePointer=%p (0x%llx), dwMoveMethod=0x%x)\n", hFile,
           liDistanceToMove.QuadPart, lpNewFilePointer,
           (lpNewFilePointer) ? (*lpNewFilePointer).QuadPart : 0, dwMoveMethod);

    LONG lDistanceToMove;
    lDistanceToMove = (LONG)liDistanceToMove.u.LowPart;
    LONG lDistanceToMoveHigh;
    lDistanceToMoveHigh = liDistanceToMove.u.HighPart;

    LONG lNewFilePointerLow = 0;

    pThread = InternalGetCurrentThread();

    palError = InternalSetFilePointer(
        pThread,
        hFile,
        lDistanceToMove,
        &lDistanceToMoveHigh,
        dwMoveMethod,
        &lNewFilePointerLow
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }
    else
    {
        if (lpNewFilePointer != NULL)
        {
            lpNewFilePointer->u.LowPart = (DWORD)lNewFilePointerLow;
            lpNewFilePointer->u.HighPart = (DWORD)lDistanceToMoveHigh;
        }
        Ret = TRUE;
    }

    LOGEXIT("SetFilePointerEx returns BOOL %d\n", Ret);
    PERF_EXIT(SetFilePointerEx);
    return Ret;
}

PAL_ERROR
CorUnix::InternalGetFileSize(
    CPalThread *pThread,
    HANDLE hFile,
    DWORD *pdwFileSizeLow,
    DWORD *pdwFileSizeHigh
    )
{
    PAL_ERROR palError = NO_ERROR;
    IPalObject *pFileObject = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;

    struct stat stat_data;

    if (INVALID_HANDLE_VALUE == hFile)
    {
        ERROR( "Invalid file handle\n" );
        palError = ERROR_INVALID_HANDLE;
        goto InternalGetFileSizeExit;
    }

    palError = g_pObjectManager->ReferenceObjectByHandle(
        pThread,
        hFile,
        &aotFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto InternalGetFileSizeExit;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto InternalGetFileSizeExit;
    }

    if (fstat(pLocalData->unix_fd, &stat_data) != 0)
    {
        ERROR("fstat failed of file descriptor %d\n", pLocalData->unix_fd);
        palError = FILEGetLastErrorFromErrno();
        goto InternalGetFileSizeExit;
    }

    *pdwFileSizeLow = (DWORD)stat_data.st_size;

    if (NULL != pdwFileSizeHigh)
    {
#if SIZEOF_OFF_T > 4
        *pdwFileSizeHigh = (DWORD)(stat_data.st_size >> 32);
#else
        *pdwFileSizeHigh = 0;
#endif
    }

InternalGetFileSizeExit:

    if (NULL != pLocalDataLock)
    {
        pLocalDataLock->ReleaseLock(pThread, FALSE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    return palError;
}

/*++
Function:
  GetFileSize

See MSDN doc.
--*/
DWORD
PALAPI
GetFileSize(
        IN HANDLE hFile,
        OUT LPDWORD lpFileSizeHigh)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;
    DWORD dwFileSizeLow;

    PERF_ENTRY(GetFileSize);
    ENTRY("GetFileSize(hFile=%p, lpFileSizeHigh=%p)\n", hFile, lpFileSizeHigh);

    pThread = InternalGetCurrentThread();

    palError = InternalGetFileSize(
        pThread,
        hFile,
        &dwFileSizeLow,
        lpFileSizeHigh
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
        dwFileSizeLow = INVALID_FILE_SIZE;
    }

    LOGEXIT("GetFileSize returns DWORD %u\n", dwFileSizeLow);
    PERF_EXIT(GetFileSize);
    return dwFileSizeLow;
}

/*++
Function:
GetFileSizeEx

See MSDN doc.
--*/
BOOL
PALAPI GetFileSizeEx(
IN   HANDLE hFile,
OUT  PLARGE_INTEGER lpFileSize)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;
    DWORD dwFileSizeHigh;
    DWORD dwFileSizeLow;

    PERF_ENTRY(GetFileSizeEx);
    ENTRY("GetFileSizeEx(hFile=%p, lpFileSize=%p)\n", hFile, lpFileSize);

    pThread = InternalGetCurrentThread();

    if (lpFileSize != NULL)
    {
        palError = InternalGetFileSize(
            pThread,
            hFile,
            &dwFileSizeLow,
            &dwFileSizeHigh
            );

        if (NO_ERROR == palError)
        {
            lpFileSize->u.LowPart = dwFileSizeLow;
            lpFileSize->u.HighPart = dwFileSizeHigh;
        }
    }
    else
    {
        palError = ERROR_INVALID_PARAMETER;
    }

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }

    LOGEXIT("GetFileSizeEx returns BOOL %d\n", NO_ERROR == palError);
    PERF_EXIT(GetFileSizeEx);
    return NO_ERROR == palError;
}

PAL_ERROR
CorUnix::InternalFlushFileBuffers(
    CPalThread *pThread,
    HANDLE hFile
    )
{
    PAL_ERROR palError = NO_ERROR;
    IPalObject *pFileObject = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    IDataLock *pLocalDataLock = NULL;

    if (INVALID_HANDLE_VALUE == hFile)
    {
        ERROR( "Invalid file handle\n" );
        palError = ERROR_INVALID_HANDLE;
        goto InternalFlushFileBuffersExit;
    }

    palError = g_pObjectManager->ReferenceObjectByHandle(
        pThread,
        hFile,
        &aotFile,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto InternalFlushFileBuffersExit;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        ReadLock,
        &pLocalDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto InternalFlushFileBuffersExit;
    }

    if (pLocalData->open_flags_deviceaccessonly == TRUE)
    {
        ERROR("File open for device access only\n");
        palError = ERROR_ACCESS_DENIED;
        goto InternalFlushFileBuffersExit;
    }

#if HAVE_FSYNC || defined(__APPLE__)
    do
    {

#if defined(__APPLE__)
        if (fcntl(pLocalData->unix_fd, F_FULLFSYNC) != -1)
            break;
#else // __APPLE__
        if (fsync(pLocalData->unix_fd) == 0)
            break;
#endif // __APPLE__

        switch (errno)
        {
        case EINTR:
            // Execution was interrupted by a signal, so restart.
            TRACE("fsync(%d) was interrupted. Restarting\n", pLocalData->unix_fd);
            break;

        default:
            palError = FILEGetLastErrorFromErrno();
            WARN("fsync(%d) failed with error %d\n", pLocalData->unix_fd, errno);
            break;
        }
    } while (NO_ERROR == palError);
#else // HAVE_FSYNC
    /* flush all buffers out to disk - there is no way to flush
       an individual file descriptor's buffers out. */
    sync();
#endif // HAVE_FSYNC else


InternalFlushFileBuffersExit:

    if (NULL != pLocalDataLock)
    {
        pLocalDataLock->ReleaseLock(pThread, FALSE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    return palError;
}


/*++
Function:
  FlushFileBuffers

See MSDN doc.
--*/
BOOL
PALAPI
FlushFileBuffers(
         IN HANDLE hFile)
{
    PAL_ERROR palError = NO_ERROR;
    CPalThread *pThread;

    PERF_ENTRY(FlushFileBuffers);
    ENTRY("FlushFileBuffers(hFile=%p)\n", hFile);

    pThread = InternalGetCurrentThread();

    palError = InternalFlushFileBuffers(
        pThread,
        hFile
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }

    LOGEXIT("FlushFileBuffers returns BOOL %d\n", NO_ERROR == palError);
    PERF_EXIT(FlushFileBuffers);
    return NO_ERROR == palError;
}

#define ENSURE_UNIQUE_NOT_ZERO \
    if ( uUniqueSeed == 0 ) \
    {\
        uUniqueSeed++;\
    }

/*++
Function:
  FILEGetLastErrorFromErrno

Convert errno into the appropriate win32 error and return it.
--*/
DWORD FILEGetLastErrorFromErrno( void )
{
    DWORD dwRet;

    switch(errno)
    {
    case 0:
        dwRet = ERROR_SUCCESS;
        break;
    case ENAMETOOLONG:
        dwRet = ERROR_FILENAME_EXCED_RANGE;
        break;
    case ENOTDIR:
        dwRet = ERROR_PATH_NOT_FOUND;
        break;
    case ENOENT:
        dwRet = ERROR_FILE_NOT_FOUND;
        break;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR:
        dwRet = ERROR_ACCESS_DENIED;
        break;
    case EEXIST:
        dwRet = ERROR_ALREADY_EXISTS;
        break;
    case ENOTEMPTY:
        dwRet = ERROR_DIR_NOT_EMPTY;
        break;
    case EBADF:
        dwRet = ERROR_INVALID_HANDLE;
        break;
    case ENOMEM:
        dwRet = ERROR_NOT_ENOUGH_MEMORY;
        break;
    case EBUSY:
        dwRet = ERROR_BUSY;
        break;
    case ENOSPC:
    case EDQUOT:
        dwRet = ERROR_DISK_FULL;
        break;
    case ELOOP:
        dwRet = ERROR_BAD_PATHNAME;
        break;
    case EIO:
        dwRet = ERROR_WRITE_FAULT;
        break;
    case EMFILE:
        dwRet = ERROR_TOO_MANY_OPEN_FILES;
        break;
    case ERANGE:
        dwRet = ERROR_BAD_PATHNAME;
        break;
    default:
        ERROR("unexpected errno %d (%s); returning ERROR_GEN_FAILURE\n",
              errno, strerror(errno));
        dwRet = ERROR_GEN_FAILURE;
    }

    TRACE("errno = %d (%s), LastError = %d\n", errno, strerror(errno), dwRet);

    return dwRet;
}

/*++
Function:
  DIRGetLastErrorFromErrno

Convert errno into the appropriate win32 error and return it.
--*/
DWORD DIRGetLastErrorFromErrno( void )
{
    if (errno == ENOENT)
        return ERROR_PATH_NOT_FOUND;
    else
        return FILEGetLastErrorFromErrno();
}


PAL_ERROR
CorUnix::InternalCreatePipe(
    CPalThread *pThread,
    HANDLE *phReadPipe,
    HANDLE *phWritePipe,
    LPSECURITY_ATTRIBUTES lpPipeAttributes,
    DWORD nSize
    )
{
    PAL_ERROR palError = NO_ERROR;
    IPalObject *pReadFileObject = NULL;
    IPalObject *pReadRegisteredFile = NULL;
    IPalObject *pWriteFileObject = NULL;
    IPalObject *pWriteRegisteredFile = NULL;
    IDataLock *pDataLock = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    CObjectAttributes oaFile(NULL, lpPipeAttributes);

    int readWritePipeDes[2] = {-1, -1};

    if ((phReadPipe == NULL) || (phWritePipe == NULL))
    {
        ERROR("One of the two parameters hReadPipe(%p) and hWritePipe(%p) is Null\n",phReadPipe,phWritePipe);
        palError = ERROR_INVALID_PARAMETER;
        goto InternalCreatePipeExit;
    }

    if ((lpPipeAttributes == NULL) ||
        (lpPipeAttributes->bInheritHandle == FALSE) ||
        (lpPipeAttributes->lpSecurityDescriptor != NULL))
    {
        ASSERT("invalid security attributes!\n");
        palError = ERROR_INVALID_PARAMETER;
        goto InternalCreatePipeExit;
    }

    if (pipe(readWritePipeDes) == -1)
    {
        ERROR("pipe() call failed errno:%d (%s) \n", errno, strerror(errno));
        palError = ERROR_INTERNAL_ERROR;
        goto InternalCreatePipeExit;
    }

    /* enable close-on-exec for both pipes; if one gets passed to CreateProcess
       it will be "uncloseonexeced" in order to be inherited */
    if(-1 == fcntl(readWritePipeDes[0],F_SETFD,FD_CLOEXEC))
    {
        ASSERT("can't set close-on-exec flag; fcntl() failed. errno is %d "
             "(%s)\n", errno, strerror(errno));
        palError = ERROR_INTERNAL_ERROR;
        goto InternalCreatePipeExit;
    }
    if(-1 == fcntl(readWritePipeDes[1],F_SETFD,FD_CLOEXEC))
    {
        ASSERT("can't set close-on-exec flag; fcntl() failed. errno is %d "
             "(%s)\n", errno, strerror(errno));
        palError = ERROR_INTERNAL_ERROR;
        goto InternalCreatePipeExit;
    }

    //
    // Setup the object for the read end of the pipe
    //

    palError = g_pObjectManager->AllocateObject(
        pThread,
        &otFile,
        &oaFile,
        &pReadFileObject
        );

    if (NO_ERROR != palError)
    {
        goto InternalCreatePipeExit;
    }

    palError = pReadFileObject->GetProcessLocalData(
        pThread,
        WriteLock,
        &pDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto InternalCreatePipeExit;
    }

    pLocalData->inheritable = TRUE;
    pLocalData->open_flags = O_RDONLY;

    //
    // After storing the file descriptor in the object's local data
    // we want to clear it from the array to prevent a possible double
    // close if an error occurs.
    //

    pLocalData->unix_fd = readWritePipeDes[0];
    readWritePipeDes[0] = -1;

    pDataLock->ReleaseLock(pThread, TRUE);
    pDataLock = NULL;

    //
    // Setup the object for the write end of the pipe
    //

    palError = g_pObjectManager->AllocateObject(
        pThread,
        &otFile,
        &oaFile,
        &pWriteFileObject
        );

    if (NO_ERROR != palError)
    {
        goto InternalCreatePipeExit;
    }

    palError = pWriteFileObject->GetProcessLocalData(
        pThread,
        WriteLock,
        &pDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto InternalCreatePipeExit;
    }

    pLocalData->inheritable = TRUE;
    pLocalData->open_flags = O_WRONLY;

    //
    // After storing the file descriptor in the object's local data
    // we want to clear it from the array to prevent a possible double
    // close if an error occurs.
    //

    pLocalData->unix_fd = readWritePipeDes[1];
    readWritePipeDes[1] = -1;

    pDataLock->ReleaseLock(pThread, TRUE);
    pDataLock = NULL;

    //
    // Register the pipe objects
    //

    palError = g_pObjectManager->RegisterObject(
        pThread,
        pReadFileObject,
        &aotFile,
        phReadPipe,
        &pReadRegisteredFile
        );

    //
    // pReadFileObject is invalidated by the call to RegisterObject, so NULL it
    // out here to ensure that we don't try to release a reference on
    // it down the line.
    //

    pReadFileObject = NULL;

    if (NO_ERROR != palError)
    {
        goto InternalCreatePipeExit;
    }

    palError = g_pObjectManager->RegisterObject(
        pThread,
        pWriteFileObject,
        &aotFile,
        phWritePipe,
        &pWriteRegisteredFile
        );

    //
    // pWriteFileObject is invalidated by the call to RegisterObject, so NULL it
    // out here to ensure that we don't try to release a reference on
    // it down the line.
    //

    pWriteFileObject = NULL;

InternalCreatePipeExit:

    if (NO_ERROR != palError)
    {
        if (-1 != readWritePipeDes[0])
        {
            close(readWritePipeDes[0]);
        }

        if (-1 != readWritePipeDes[1])
        {
            close(readWritePipeDes[1]);
        }
    }

    if (NULL != pReadFileObject)
    {
        pReadFileObject->ReleaseReference(pThread);
    }

    if (NULL != pReadRegisteredFile)
    {
        pReadRegisteredFile->ReleaseReference(pThread);
    }

    if (NULL != pWriteFileObject)
    {
        pWriteFileObject->ReleaseReference(pThread);
    }

    if (NULL != pWriteRegisteredFile)
    {
        pWriteRegisteredFile->ReleaseReference(pThread);
    }

    return palError;
}

/*++
Function:
  CreatePipe

See MSDN doc.
--*/
PALIMPORT
BOOL
PALAPI
CreatePipe(
        OUT PHANDLE hReadPipe,
        OUT PHANDLE hWritePipe,
        IN LPSECURITY_ATTRIBUTES lpPipeAttributes,
        IN DWORD nSize)
{
    PAL_ERROR palError;
    CPalThread *pThread;

    PERF_ENTRY(CreatePipe);
    ENTRY("CreatePipe(hReadPipe:%p, hWritePipe:%p, lpPipeAttributes:%p, nSize:%d\n",
          hReadPipe, hWritePipe, lpPipeAttributes, nSize);

    pThread = InternalGetCurrentThread();

    palError = InternalCreatePipe(
        pThread,
        hReadPipe,
        hWritePipe,
        lpPipeAttributes,
        nSize
        );

    if (NO_ERROR != palError)
    {
        pThread->SetLastError(palError);
    }

    LOGEXIT("CreatePipe return %s\n", NO_ERROR == palError ? "TRUE":"FALSE");
    PERF_EXIT(CreatePipe);
    return NO_ERROR == palError;
}

/*++
init_std_handle [static]

utility function for FILEInitStdHandles. do the work that is common to all
three standard handles

Parameters:
    HANDLE pStd : Defines which standard handle to assign
    FILE *stream        : file stream to associate to handle

Return value:
    handle for specified stream, or INVALID_HANDLE_VALUE on failure
--*/
static HANDLE init_std_handle(HANDLE * pStd, FILE *stream)
{
    CPalThread *pThread = InternalGetCurrentThread();
    PAL_ERROR palError = NO_ERROR;
    IPalObject *pFileObject = NULL;
    IPalObject *pRegisteredFile = NULL;
    IDataLock *pDataLock = NULL;
    CFileProcessLocalData *pLocalData = NULL;
    CObjectAttributes oa;

    HANDLE hFile = INVALID_HANDLE_VALUE;
    int new_fd = -1;

    /* duplicate the FILE *, so that we can fclose() in FILECloseHandle without
       closing the original */
    new_fd = fcntl(fileno(stream), F_DUPFD_CLOEXEC, 0); // dup, but with CLOEXEC
    if(-1 == new_fd)
    {
        ERROR("dup() failed; errno is %d (%s)\n", errno, strerror(errno));
        goto done;
    }

    palError = g_pObjectManager->AllocateObject(
        pThread,
        &otFile,
        &oa,
        &pFileObject
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    palError = pFileObject->GetProcessLocalData(
        pThread,
        WriteLock,
        &pDataLock,
        reinterpret_cast<void**>(&pLocalData)
        );

    if (NO_ERROR != palError)
    {
        goto done;
    }

    pLocalData->inheritable = TRUE;
    pLocalData->unix_fd = new_fd;
    pLocalData->open_flags = 0;
    pLocalData->open_flags_deviceaccessonly = FALSE;

    //
    // We've finished initializing our local data, so release that lock
    //

    pDataLock->ReleaseLock(pThread, TRUE);
    pDataLock = NULL;

    palError = g_pObjectManager->RegisterObject(
        pThread,
        pFileObject,
        &aotFile,
        &hFile,
        &pRegisteredFile
        );

    //
    // pFileObject is invalidated by the call to RegisterObject, so NULL it
    // out here to ensure that we don't try to release a reference on
    // it down the line.
    //

    pFileObject = NULL;

done:

    if (NULL != pDataLock)
    {
        pDataLock->ReleaseLock(pThread, TRUE);
    }

    if (NULL != pFileObject)
    {
        pFileObject->ReleaseReference(pThread);
    }

    if (NULL != pRegisteredFile)
    {
        pRegisteredFile->ReleaseReference(pThread);
    }

    if (NO_ERROR == palError)
    {
        *pStd = hFile;
    }
    else if (-1 != new_fd)
    {
        close(new_fd);
    }

    return hFile;
}


/*++
FILEInitStdHandles

Create handle objects for stdin, stdout and stderr

(no parameters)

Return value:
    TRUE on success, FALSE on failure
--*/
BOOL FILEInitStdHandles(void)
{
    HANDLE stdin_handle;
    HANDLE stdout_handle;
    HANDLE stderr_handle;

    TRACE("creating handle objects for stdin, stdout, stderr\n");

    stdin_handle = init_std_handle(&pStdIn, stdin);
    if(INVALID_HANDLE_VALUE == stdin_handle)
    {
        ERROR("failed to create stdin handle\n");
        goto fail;
    }

    stdout_handle = init_std_handle(&pStdOut, stdout);
    if(INVALID_HANDLE_VALUE == stdout_handle)
    {
        ERROR("failed to create stdout handle\n");
        CloseHandle(stdin_handle);
        goto fail;
    }

    stderr_handle = init_std_handle(&pStdErr, stderr);
    if(INVALID_HANDLE_VALUE == stderr_handle)
    {
        ERROR("failed to create stderr handle\n");
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        goto fail;
    }
    return TRUE;

fail:
    pStdIn = INVALID_HANDLE_VALUE;
    pStdOut = INVALID_HANDLE_VALUE;
    pStdErr = INVALID_HANDLE_VALUE;
    return FALSE;
}
