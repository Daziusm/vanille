cmake_minimum_required(VERSION 3.20)

if(NOT VANILLE_ROOT OR NOT PAYLOAD_ZIP OR NOT PAYLOAD_RC OR NOT PAYLOAD_VERSION_H)
    message(FATAL_ERROR "stage_payload.cmake: missing required variables")
endif()

get_filename_component(VANILLE_ROOT "${VANILLE_ROOT}" ABSOLUTE)

set(staging_dir "${CMAKE_BINARY_DIR}/payload_staging")
file(REMOVE_RECURSE "${staging_dir}")
file(MAKE_DIRECTORY "${staging_dir}/fonts")

set(vanille_exe "${VANILLE_ROOT}/build/vanille.exe")
if(NOT EXISTS "${vanille_exe}")
    message(FATAL_ERROR "vanille.exe not found at ${vanille_exe}. Build Vanille Release|x64 first.")
endif()

file(COPY "${vanille_exe}" DESTINATION "${staging_dir}")

set(values_txt "${VANILLE_ROOT}/values.txt")
if(NOT EXISTS "${values_txt}")
    set(values_txt "${VANILLE_ROOT}/values.txt.example")
    if(NOT EXISTS "${values_txt}")
        message(FATAL_ERROR "values.txt not found. Place Vanille/values.txt or values.txt.example before staging payload.")
    endif()
    message(WARNING "Vanille/values.txt missing — staging placeholder offsets (loader refreshes at runtime).")
endif()
file(COPY "${values_txt}" DESTINATION "${staging_dir}")

set(lua_dll "${VANILLE_ROOT}/third_party/lua/lua53-64.dll")
if(EXISTS "${lua_dll}")
    file(COPY "${lua_dll}" DESTINATION "${staging_dir}")
else()
    message(WARNING "lua53-64.dll not found at ${lua_dll} - Lua VM will not work until it is built.")
endif()

file(GLOB font_files "${VANILLE_ROOT}/fonts/*.ttf")
if(font_files)
    file(COPY ${font_files} DESTINATION "${staging_dir}/fonts")
endif()

file(REMOVE "${PAYLOAD_ZIP}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar "cf" "${PAYLOAD_ZIP}" --format=zip .
    WORKING_DIRECTORY "${staging_dir}"
    COMMAND_ERROR_IS_FATAL ANY
)

file(SHA256 "${PAYLOAD_ZIP}" payload_sha256)

file(WRITE "${PAYLOAD_VERSION_H}"
    "#pragma once\n#define CHOCOLA_PAYLOAD_SHA256 \"${payload_sha256}\"\n")

file(TO_CMAKE_PATH "${PAYLOAD_ZIP}" payload_zip_rc_path)
string(REPLACE "\\" "/" payload_zip_rc_path "${payload_zip_rc_path}")

file(WRITE "${PAYLOAD_RC}"
    "#include \"resource.h\"\nIDR_PAYLOAD_ZIP RCDATA \"${payload_zip_rc_path}\"\n")
