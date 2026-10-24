include_guard(GLOBAL)

option(IVS_WITH_MVS "Discover and link Hikrobot MVS SDK integration hook (camera adapter unavailable)" OFF)
set(IVS_MVS_ROOT "" CACHE PATH "User-installed Hikrobot MVS SDK root; no SDK is downloaded or bundled")

function(ivs_configure_hardware_sdk target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "ivs_configure_hardware_sdk: unknown target '${target}'")
    endif()
    if(NOT IVS_WITH_MVS)
        return()
    endif()

    find_path(IVS_MVS_INCLUDE_DIR MvCameraControl.h
        HINTS "${IVS_MVS_ROOT}" "$ENV{MVCAM_COMMON_RUNENV}"
        PATH_SUFFIXES include Includes Development/Includes)
    find_library(IVS_MVS_LIBRARY
        NAMES MvCameraControl MvCameraControl64
        HINTS "${IVS_MVS_ROOT}" "$ENV{MVCAM_COMMON_RUNENV}"
        PATH_SUFFIXES lib lib64 lib/64 libs/64 Libraries/win64 Development/Libraries/win64)
    if(NOT IVS_MVS_INCLUDE_DIR OR NOT IVS_MVS_LIBRARY)
        message(FATAL_ERROR
            "IVS_WITH_MVS requires the user-installed SDK. Set IVS_MVS_ROOT or both "
            "IVS_MVS_INCLUDE_DIR and IVS_MVS_LIBRARY to MvCameraControl.h and its matching library. "
            "No proprietary SDK is included. This option only validates the SDK hook; camera acquisition is not implemented.")
    endif()
    target_compile_definitions("${target}" PRIVATE IVS_WITH_MVS=1)
    target_include_directories("${target}" PRIVATE "${IVS_MVS_INCLUDE_DIR}")
    target_link_libraries("${target}" PRIVATE "${IVS_MVS_LIBRARY}")
    message(WARNING "MVS SDK integration hook enabled. The camera acquisition adapter is unavailable; 'mvs' device configurations are rejected.")
endfunction()
