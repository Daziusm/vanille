cmake_minimum_required(VERSION 3.20)
project(chocola-loader LANGUAGES NONE)

get_filename_component(VANILLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Vanille" ABSOLUTE CACHE PATH "Path to the Vanille project root")

set(PAYLOAD_ZIP "${CMAKE_BINARY_DIR}/payload.zip")
set(PAYLOAD_RC "${CMAKE_BINARY_DIR}/payload.rc")
set(PAYLOAD_VERSION_H "${CMAKE_BINARY_DIR}/payload_version.h")
set(CHOCOLA_CS_PROJ "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/Chocola.csproj")
set(CHOCOLA_CS_OUT "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/obj/Release/Vanille.exe")
set(CHOCOLA_CS_OUT_BIN "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/bin/Release/Vanille.exe")
set(CHOCOLA_CS_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/payload.zip")

add_custom_command(
    OUTPUT "${PAYLOAD_ZIP}" "${PAYLOAD_RC}" "${PAYLOAD_VERSION_H}"
    COMMAND "${CMAKE_COMMAND}"
        -DVANILLE_ROOT=${VANILLE_ROOT}
        -DCMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DPAYLOAD_ZIP=${PAYLOAD_ZIP}
        -DPAYLOAD_RC=${PAYLOAD_RC}
        -DPAYLOAD_VERSION_H=${PAYLOAD_VERSION_H}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_payload.cmake"
    DEPENDS
        "${VANILLE_ROOT}/build/vanille.exe"
        "${VANILLE_ROOT}/values.txt"
    COMMENT "Staging Vanille payload zip"
    VERBATIM
)

set(_msbuild_paths
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin"
)

if(DEFINED ENV{ProgramFiles\(x86\)})
    list(APPEND _msbuild_paths "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin")
    list(APPEND _msbuild_paths "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin")
endif()

find_program(MSBUILD_EXECUTABLE
    NAMES msbuild MSBuild
    PATHS ${_msbuild_paths}
)

if(NOT MSBUILD_EXECUTABLE)
    message(FATAL_ERROR "MSBuild not found. Install Visual Studio Build Tools.")
endif()

set(CHOCOLA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Release")
set(CHOCOLA_OUTPUT_EXE "${CHOCOLA_OUTPUT_DIR}/Chocola.exe")

add_custom_command(
    OUTPUT "${CHOCOLA_OUTPUT_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PAYLOAD_ZIP}" "${CHOCOLA_CS_PAYLOAD}"
    COMMAND "${MSBUILD_EXECUTABLE}" "${CHOCOLA_CS_PROJ}" /p:Configuration=Release /v:m
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CHOCOLA_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CHOCOLA_CS_OUT}" "${CHOCOLA_OUTPUT_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CHOCOLA_CS_OUT_BIN}" "${CHOCOLA_OUTPUT_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/bin/Release/Resources"
        "${CHOCOLA_OUTPUT_DIR}/Resources"
    DEPENDS
        "${PAYLOAD_ZIP}"
        "${CHOCOLA_CS_PROJ}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/Form1.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/Form2.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/ChocolaLoader/PayloadService.cs"
    COMMENT "Building gamesense-based Chocola loader"
    VERBATIM
)

add_custom_target(chocola-loader ALL DEPENDS "${CHOCOLA_OUTPUT_EXE}")
