# Bootstraps CPM.cmake — a tiny dependency manager built on CMake's FetchContent.
# https://github.com/cpm-cmake/CPM.cmake
#
# On first configure this downloads a pinned CPM release and caches it. Set the
# CPM_SOURCE_CACHE environment variable (CI does) to share downloaded sources
# (JUCE, Catch2, ...) across build trees so they aren't re-fetched every time.

set(CPM_DOWNLOAD_VERSION 0.40.2)

if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
  set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

get_filename_component(CPM_DOWNLOAD_LOCATION "${CPM_DOWNLOAD_LOCATION}" ABSOLUTE)

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
  message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
    "${CPM_DOWNLOAD_LOCATION}"
    # TODO: pin EXPECTED_HASH once we settle on a CPM version, for supply-chain safety.
    STATUS _cpm_download_status)
  list(GET _cpm_download_status 0 _cpm_download_code)
  if(NOT _cpm_download_code EQUAL 0)
    message(FATAL_ERROR "Failed to download CPM.cmake: ${_cpm_download_status}")
  endif()
endif()

include("${CPM_DOWNLOAD_LOCATION}")
