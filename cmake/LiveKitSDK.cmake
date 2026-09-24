# Minimaler LiveKit-SDK-Downloader fuer dieses Projekt.
include_guard(GLOBAL)

function(cc_livekit_sdk_setup)
  if(DEFINED ENV{CC_LIVEKIT_SDK_DIR} AND NOT "$ENV{CC_LIVEKIT_SDK_DIR}" STREQUAL "")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{CC_LIVEKIT_SDK_DIR}")
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    return()
  endif()

  set(_download_dir "${CMAKE_BINARY_DIR}/_downloads")
  set(_sdk_dir "${CMAKE_BINARY_DIR}/_deps/livekit-sdk")
  file(MAKE_DIRECTORY "${_download_dir}")
  file(MAKE_DIRECTORY "${_sdk_dir}")

  set(_latest_json "${_download_dir}/latest.json")
  file(DOWNLOAD
    "https://api.github.com/repos/livekit/client-sdk-cpp/releases/latest"
    "${_latest_json}"
    HTTPHEADER "User-Agent: community-clash-client"
    HTTPHEADER "Accept: application/vnd.github+json"
    TLS_VERIFY ON
    STATUS _st
  )
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    message(FATAL_ERROR "LiveKit Release konnte nicht ermittelt werden: ${_st}")
  endif()

  file(READ "${_latest_json}" _json)
  string(JSON _tag GET "${_json}" tag_name)
  string(REGEX REPLACE "^v" "" _ver "${_tag}")

  set(_archive "livekit-sdk-windows-x64-${_ver}.zip")
  set(_url "https://github.com/livekit/client-sdk-cpp/releases/download/v${_ver}/${_archive}")
  set(_zip "${_download_dir}/${_archive}")
  set(_root "${_sdk_dir}/livekit-sdk-windows-x64-${_ver}")

  if(NOT EXISTS "${_root}/lib/cmake")
    message(STATUS "Lade LiveKit C++ SDK ${_ver} ...")
    file(DOWNLOAD "${_url}" "${_zip}" SHOW_PROGRESS TLS_VERIFY ON STATUS _dl)
    list(GET _dl 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
      message(FATAL_ERROR "LiveKit SDK Download fehlgeschlagen: ${_dl}")
    endif()
    file(REMOVE_RECURSE "${_root}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_sdk_dir}")
  endif()

  list(PREPEND CMAKE_PREFIX_PATH "${_root}")
  set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
  set(LiveKit_DIR "${_root}/lib/cmake/LiveKit" PARENT_SCOPE)
endfunction()
