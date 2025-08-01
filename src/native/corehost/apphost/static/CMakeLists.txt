# Licensed to the .NET Foundation under one or more agreements.
# The .NET Foundation licenses this file to you under the MIT license.

project(singlefilehost)
set(DOTNET_PROJECT_NAME "singlefilehost")

include_directories(..)
include_directories(../..)
include_directories(../../hostmisc)
include_directories(../../json)
include_directories(${CLR_SRC_NATIVE_DIR}/libs/System.IO.Compression.Native)
include_directories(${CLR_SRC_NATIVE_DIR}/libs/Common)
include_directories(${CLR_ARTIFACTS_OBJ_DIR}) # Generated version files

add_subdirectory(../../hostmisc hostmisc)

configure_file(${CLR_SRC_NATIVE_DIR}/corehost/configure.h.in ${GENERATED_INCLUDE_DIR}/corehost/configure.h)
target_include_directories(hostmisc_interface INTERFACE ${GENERATED_INCLUDE_DIR}/corehost)

if ((NOT DEFINED CLR_CMAKE_USE_SYSTEM_RAPIDJSON) OR (NOT CLR_CMAKE_USE_SYSTEM_RAPIDJSON))
    include_directories(${CLR_SRC_NATIVE_DIR}/external/)
endif()

set(SOURCES
    ../bundle_marker.cpp
    ./hostfxr_resolver.cpp
    ./hostpolicy_resolver.cpp
    ../../hostpolicy/static/coreclr_resolver.cpp
    ../../fxr_resolver.cpp
    ../../corehost.cpp
)

set(HEADERS
    ../bundle_marker.h
    ../../hostfxr_resolver.h
    ../../fxr_resolver.h
)

# TODO: https://github.com/dotnet/runtime/issues/111523
if (NOT CLR_CMAKE_TARGET_ANDROID)
    add_compile_definitions(NATIVE_LIBS_EMBEDDED)
endif()

include(../../fxr/files.cmake)
include(../../hostpolicy/files.cmake)
include(../../hostcommon/files.cmake)

if(MSVC)
    # Host components don't try to handle asynchronous exceptions
    set_property(DIRECTORY PROPERTY CLR_EH_OPTION /EHsc)
    set_property(TARGET hostmisc PROPERTY CLR_EH_OPTION /EHsc)
elseif (CMAKE_CXX_COMPILER_ID MATCHES GNU)
    # Prevents libc from calling pthread_cond_destroy on static objects in
    # dlopen()'ed library which we dlclose() in pal::unload_library.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)
endif()

if(CLR_CMAKE_TARGET_WIN32)
    add_compile_definitions(UNICODE)
    list(APPEND SOURCES
        ../apphost.windows.cpp
        ${CLR_SRC_NATIVE_DIR}/libs/Common/delayloadhook_windows.cpp
    )

    list(APPEND HEADERS
        ../apphost.windows.h)
endif()

if(CLR_CMAKE_TARGET_WIN32)
    add_linker_flag("/DEF:${CMAKE_CURRENT_SOURCE_DIR}/singlefilehost.def")

else()
    if(CLR_CMAKE_TARGET_FREEBSD)
        set(DEF_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/singlefilehost_freebsdexports.src)
    else()
        set(DEF_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/singlefilehost_unixexports.src)
    endif()

    set(EXPORTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/singlefilehost.exports)
    generate_exports_file(${DEF_SOURCES} ${EXPORTS_FILE})

    set_exports_linker_option(${EXPORTS_FILE})
endif()

add_executable(singlefilehost ${SOURCES})

add_sanitizer_runtime_support(singlefilehost)

if(NOT CLR_CMAKE_TARGET_WIN32)
    disable_pax_mprotect(singlefilehost)
endif()

install_with_stripped_symbols(singlefilehost TARGETS corehost COMPONENT runtime)

include(configure.cmake)

if(CLR_CMAKE_HOST_UNIX)
    add_custom_target(singlefilehost_exports DEPENDS ${EXPORTS_FILE})
    add_dependencies(singlefilehost singlefilehost_exports)

    set_property(TARGET singlefilehost APPEND_STRING PROPERTY LINK_FLAGS ${EXPORTS_LINKER_OPTION})
    set_property(TARGET singlefilehost APPEND_STRING PROPERTY LINK_DEPENDS ${EXPORTS_FILE})
endif()

add_definitions(-DFEATURE_APPHOST=1)
add_definitions(-DFEATURE_STATIC_HOST=1)

if(CLR_CMAKE_TARGET_WIN32)
    # Disable manifest generation into the file .exe on Windows
    add_linker_flag("/MANIFEST:NO")

    # Enable CET-compatibility
    if (CLR_CMAKE_HOST_ARCH_AMD64)
        add_linker_flag("/CETCOMPAT")
    endif()

    # Incremental linking results in the linker inserting extra padding and routing function calls via thunks that can break the
    # invariants (e.g. size of region between Jit_PatchedCodeLast-Jit_PatchCodeStart needs to fit in a page).
    add_linker_flag("/INCREMENTAL:NO")

    # Delay load libraries required for WinRT as that is not supported on all platforms
    add_linker_flag("/DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll")

    # Delay load version.dll so that we can specify how to search when loading it as it is not part of Windows' known DLLs
    add_linker_flag("/DELAYLOAD:version.dll")
endif()

if(CLR_CMAKE_TARGET_WIN32)
    set(NATIVE_LIBS
        coreclr_static

        System.Globalization.Native-Static
        System.IO.Compression.Native-Static

        kernel32.lib
        advapi32.lib
        ole32.lib
        oleaut32.lib
        uuid.lib
        user32.lib
        version.lib
        shlwapi.lib
        shell32.lib
        bcrypt.lib
        RuntimeObject.lib
        delayimp.lib
    )

    # additional requirements for System.IO.Compression.Native
    include(${CLR_SRC_NATIVE_DIR}/libs/System.IO.Compression.Native/extra_libs.cmake)
    append_extra_compression_libs(NATIVE_LIBS)

    set(RUNTIMEINFO_LIB runtimeinfo)

else()
    if(CLR_CMAKE_HOST_OSX OR (CLR_CMAKE_HOST_LINUX AND NOT CLR_CMAKE_HOST_UNIX_X86 AND NOT CLR_CMAKE_HOST_ANDROID))
        LIST(APPEND NATIVE_LIBS createdump_static)
    endif()

    LIST(APPEND NATIVE_LIBS
        coreclr_static

        System.Globalization.Native-Static
        System.IO.Compression.Native-Static
        System.Native-Static
    )

    if(NOT CLR_CMAKE_TARGET_ANDROID)
        list(APPEND NATIVE_LIBS System.Net.Security.Native-Static)

        if(NOT CLR_CMAKE_TARGET_APPLE)
            list(APPEND NATIVE_LIBS System.Security.Cryptography.Native.OpenSsl-Static)
        endif()
    else()
        list(APPEND NATIVE_LIBS
            System.Security.Cryptography.Native.Android-Static
        )
    endif()

    list(APPEND NATIVE_LIBS
        coreclrpal_dac
        corguids
        dbgutil
    )
    if (FEATURE_EVENT_TRACE)
        LIST(APPEND NATIVE_LIBS
            eventprovider
        )
    endif(FEATURE_EVENT_TRACE)
    LIST(APPEND NATIVE_LIBS
        nativeresourcestring
    )

    # additional requirements for System.IO.Compression.Native
    include(${CLR_SRC_NATIVE_DIR}/libs/System.IO.Compression.Native/extra_libs.cmake)
    append_extra_compression_libs(NATIVE_LIBS)

    if (NOT CLR_CMAKE_TARGET_TVOS AND NOT CLR_CMAKE_TARGET_ANDROID AND NOT CLR_CMAKE_TARGET_BROWSER) # no gssapi on tvOS, see https://developer.apple.com/documentation/gss
        # Additional requirements for System.Net.Security.Native
        include(${CLR_SRC_NATIVE_DIR}/libs/System.Net.Security.Native/extra_libs.cmake)
        append_extra_security_libs(NATIVE_LIBS)
    endif()

    # Additional requirements for System.Native
    include(${CLR_SRC_NATIVE_DIR}/libs/System.Native/extra_libs.cmake)
    append_extra_system_libs(NATIVE_LIBS)

    if(NOT CLR_CMAKE_TARGET_APPLE AND NOT CLR_CMAKE_TARGET_ANDROID AND NOT CLR_CMAKE_TARGET_BROWSER)
        # Additional requirements for System.Security.Cryptography.Native.OpenSsl
        include(${CLR_SRC_NATIVE_DIR}/libs/System.Security.Cryptography.Native/extra_libs.cmake)
        append_extra_cryptography_libs(NATIVE_LIBS)
    endif()

    set(RUNTIMEINFO_LIB runtimeinfo)

endif()

LIST(APPEND NATIVE_LIBS
    gcinfo
)

if(CLR_CMAKE_TARGET_APPLE)
    LIST(APPEND NATIVE_LIBS
        System.Security.Cryptography.Native.Apple-Static
    )

    # Additional requirements for System.Security.Cryptography.Native.Apple
    include(${CLR_SRC_NATIVE_DIR}/libs/System.Security.Cryptography.Native.Apple/extra_libs.cmake)
    append_extra_cryptography_apple_libs(NATIVE_LIBS)
endif()

#
# Additional requirements for coreclr
#
if(CLR_CMAKE_TARGET_APPLE)
   find_library(COREFOUNDATION CoreFoundation)
   find_library(CORESERVICES CoreServices)
   find_library(SECURITY Security)
   find_library(SYSTEM System)

   LIST(APPEND NATIVE_LIBS
     ${COREFOUNDATION}
     ${CORESERVICES}
     ${SECURITY}
     ${SYSTEM}
   )
elseif(CLR_CMAKE_TARGET_NETBSD)
     find_library(KVM kvm)

     LIST(APPEND NATIVE_LIBS
       ${KVM}
     )
elseif (CLR_CMAKE_TARGET_SUNOS)
     LIST(APPEND NATIVE_LIBS
         socket
     )
elseif (CLR_CMAKE_TARGET_HAIKU)
     LIST(APPEND NATIVE_LIBS
         network
     )
endif(CLR_CMAKE_TARGET_APPLE)

# On *BSD, we always use the libunwind that's part of the OS
if(CLR_CMAKE_TARGET_FREEBSD)
  set(CLR_CMAKE_USE_SYSTEM_LIBUNWIND 1)
endif()

if(CLR_CMAKE_USE_SYSTEM_LIBUNWIND AND NOT CLR_CMAKE_TARGET_APPLE)
    find_unwind_libs(UNWIND_LIBS)

    LIST(APPEND NATIVE_LIBS
      ${UNWIND_LIBS}
    )
endif()

if(CLR_CMAKE_TARGET_LINUX OR CLR_CMAKE_TARGET_FREEBSD OR CLR_CMAKE_TARGET_NETBSD OR CLR_CMAKE_TARGET_SUNOS)
    # These options are used to force every object to be included even if it's unused.
    set(START_WHOLE_ARCHIVE -Wl,--whole-archive)
    set(END_WHOLE_ARCHIVE -Wl,--no-whole-archive)
endif(CLR_CMAKE_TARGET_LINUX OR CLR_CMAKE_TARGET_FREEBSD OR CLR_CMAKE_TARGET_NETBSD OR CLR_CMAKE_TARGET_SUNOS)

if(CLR_CMAKE_TARGET_LINUX AND CLR_CMAKE_TARGET_ARCH_RISCV64)
    add_linker_flag(-Wl,-z,notext)
endif()

if(CLR_CMAKE_TARGET_APPLE)
    # These options are used to force every object to be included even if it's unused.
    set(START_WHOLE_ARCHIVE -force_load)
    set(END_WHOLE_ARCHIVE )
endif(CLR_CMAKE_TARGET_APPLE)

set_property(TARGET singlefilehost PROPERTY ENABLE_EXPORTS 1)

target_link_libraries(
    singlefilehost
    PRIVATE
    ${NATIVE_LIBS}

    ${START_WHOLE_ARCHIVE}
    ${RUNTIMEINFO_LIB}
    ${END_WHOLE_ARCHIVE}
)

target_link_libraries(singlefilehost PRIVATE hostmisc)

add_sanitizer_runtime_support(singlefilehost)

if (CLR_CMAKE_HOST_APPLE)
    adhoc_sign_with_entitlements(singlefilehost "${CLR_ENG_NATIVE_DIR}/entitlements.plist")
endif()
