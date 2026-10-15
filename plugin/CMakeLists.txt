cmake_minimum_required(VERSION 3.20)
project(zw3dmcp LANGUAGES CXX)

# ---------------------------------------------------------------------------
# ZW3D SDK location. Works for ZW3D 2025 and newer: pass -DZW3D_SDK_DIR=<install dir>, or set env ZW3D_DIR,
# or let us pick the newest "C:/Program Files/ZWSOFT/ZW3D <year>" (>= 2025) that contains api/inc/VxApi.h.
# ---------------------------------------------------------------------------
set(ZW3D_SDK_DIR "" CACHE PATH "ZW3D install directory (contains ZW3D.lib and api/inc/VxApi.h)")
if(NOT ZW3D_SDK_DIR AND DEFINED ENV{ZW3D_DIR})
  file(TO_CMAKE_PATH "$ENV{ZW3D_DIR}" ZW3D_SDK_DIR)
endif()
if(NOT ZW3D_SDK_DIR)
  file(GLOB _zw_candidates "C:/Program Files/ZWSOFT/ZW3D *")
  list(SORT _zw_candidates ORDER DESCENDING)
  foreach(_c IN LISTS _zw_candidates)
    if(EXISTS "${_c}/api/inc/VxApi.h" AND EXISTS "${_c}/ZW3D.lib")
      set(ZW3D_SDK_DIR "${_c}")
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${ZW3D_SDK_DIR}/api/inc/VxApi.h" OR NOT EXISTS "${ZW3D_SDK_DIR}/ZW3D.lib")
  message(FATAL_ERROR "ZW3D SDK not found. Pass -DZW3D_SDK_DIR=\"C:/Program Files/ZWSOFT/ZW3D 2025\" "
                      "(needs api/inc/VxApi.h and ZW3D.lib).")
endif()
message(STATUS "ZW3D SDK: ${ZW3D_SDK_DIR}")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json.hpp")
  message(FATAL_ERROR "third_party/json.hpp (nlohmann/json single header, MIT) is missing. "
                      "See third_party/README.md.")
endif()

add_library(zw3dmcp SHARED
  src/plugin_main.cpp
  src/server.cpp
  src/dispatcher.cpp
  src/encoding.cpp
  src/errors.cpp
  src/handlers/common.cpp
  src/handlers/part.cpp
  src/handlers/query.cpp
  src/handlers/file.cpp)

set_target_properties(zw3dmcp PROPERTIES OUTPUT_NAME "zw3dmcp" PREFIX "")  # DLL name fixes Init/Exit names
target_compile_features(zw3dmcp PRIVATE cxx_std_17)
target_include_directories(zw3dmcp PRIVATE src third_party "${ZW3D_SDK_DIR}/api/inc")
target_link_libraries(zw3dmcp PRIVATE "${ZW3D_SDK_DIR}/ZW3D.lib" ws2_32)
target_compile_definitions(zw3dmcp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  # Source is UTF-8. Keep the default /MD runtime (ZW3D itself uses the DLL runtime) -- do not switch to /MT.
  target_compile_options(zw3dmcp PRIVATE /utf-8 /W3 /EHsc)
endif()

# Optional: copy into the per-user plugin folder ZW3D scans at start-up (see docs/api_notes.md).
option(ZW3D_MCP_INSTALL_USER "Copy zw3dmcp.dll to %APPDATA%/ZWSOFT/ZW3D/ZW 3D <year>/custom/apilibs after build" OFF)
set(ZW3D_MCP_YEAR "2025" CACHE STRING "ZW3D release year for the user apilibs folder")
if(ZW3D_MCP_INSTALL_USER)
  add_custom_command(TARGET zw3dmcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{APPDATA}/ZWSOFT/ZW3D/ZW 3D ${ZW3D_MCP_YEAR}/custom/apilibs"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:zw3dmcp>"
            "$ENV{APPDATA}/ZWSOFT/ZW3D/ZW 3D ${ZW3D_MCP_YEAR}/custom/apilibs/"
    COMMENT "Installing zw3dmcp.dll into the user apilibs folder (close ZW3D first)")
endif()
