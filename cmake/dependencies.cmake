# Downloads official Microsoft NuGet packages for Windows SDK/WDK headers and libs.
# Also downloads EDK2 headers for UEFI builds.
# All downloads go to ${FETCHCONTENT_BASE_DIR} (typically build/_deps/).

include(FetchContent)

set(NUGET_BASE_URL "https://api.nuget.org/v3-flatcontainer")
set(WINDOWS_SDK_VERSION "10.0.28000.2526")

# --- Windows SDK (headers: shared, um, ucrt) ---
function(download_windows_sdk dest_var)
    set(_pkg "microsoft.windows.sdk.cpp")
    set(_url "${NUGET_BASE_URL}/${_pkg}/${WINDOWS_SDK_VERSION}/${_pkg}.${WINDOWS_SDK_VERSION}.nupkg")
    set(_dir "${FETCHCONTENT_BASE_DIR}/windows-sdk-src")
    set(_stamp "${_dir}/.extracted")

    if(NOT EXISTS "${_stamp}")
        message(STATUS "Downloading Windows SDK headers...")
        file(DOWNLOAD "${_url}" "${_dir}/sdk.zip" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${_dir}/sdk.zip" DESTINATION "${_dir}")
        file(TOUCH "${_stamp}")
    endif()

    set(${dest_var} "${_dir}/c" PARENT_SCOPE)
endfunction()

# --- Windows WDK (km headers: ntddk.h, wdm.h, km libs) ---
function(download_windows_wdk dest_var)
    set(_pkg "microsoft.windows.wdk.x64")
    set(_url "${NUGET_BASE_URL}/${_pkg}/${WINDOWS_SDK_VERSION}/${_pkg}.${WINDOWS_SDK_VERSION}.nupkg")
    set(_dir "${FETCHCONTENT_BASE_DIR}/windows-wdk-src")
    set(_stamp "${_dir}/.extracted")

    if(NOT EXISTS "${_stamp}")
        message(STATUS "Downloading Windows WDK headers...")
        file(DOWNLOAD "${_url}" "${_dir}/wdk.zip" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${_dir}/wdk.zip" DESTINATION "${_dir}")
        file(TOUCH "${_stamp}")
    endif()

    set(${dest_var} "${_dir}/c" PARENT_SCOPE)
endfunction()

# --- Windows SDK x64 libs (ntoskrnl.lib etc.) ---
function(download_windows_sdk_libs dest_var)
    set(_pkg "microsoft.windows.sdk.cpp.x64")
    set(_url "${NUGET_BASE_URL}/${_pkg}/${WINDOWS_SDK_VERSION}/${_pkg}.${WINDOWS_SDK_VERSION}.nupkg")
    set(_dir "${FETCHCONTENT_BASE_DIR}/windows-sdk-libs-src")
    set(_stamp "${_dir}/.extracted")

    if(NOT EXISTS "${_stamp}")
        message(STATUS "Downloading Windows SDK x64 libs...")
        file(DOWNLOAD "${_url}" "${_dir}/sdk-libs.zip" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${_dir}/sdk-libs.zip" DESTINATION "${_dir}")
        file(TOUCH "${_stamp}")
    endif()

    set(${dest_var} "${_dir}/c" PARENT_SCOPE)
endfunction()

# --- EDK2 headers (for UEFI) ---
function(download_edk2 dest_var)
    set(_tag "edk2-stable202505")
    set(_url "https://github.com/tianocore/edk2/archive/refs/tags/${_tag}.tar.gz")
    set(_dir "${FETCHCONTENT_BASE_DIR}/edk2-src")
    set(_stamp "${_dir}/.extracted")

    if(NOT EXISTS "${_stamp}")
        message(STATUS "Downloading EDK2 headers...")
        file(DOWNLOAD "${_url}" "${_dir}/edk2.tar.gz" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${_dir}/edk2.tar.gz" DESTINATION "${_dir}")
        file(TOUCH "${_stamp}")
    endif()

    # Find the extracted directory (edk2-edk2-stable202505/)
    file(GLOB _edk2_dirs "${_dir}/edk2-*")
    list(GET _edk2_dirs 0 _edk2_dir)
    set(${dest_var} "${_edk2_dir}" PARENT_SCOPE)
endfunction()
