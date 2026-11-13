cmake_minimum_required(VERSION 3.21)
option(FORMAT_VALIDATION_ONLY "Configure only formatting targets" OFF)
if(FORMAT_VALIDATION_ONLY)
    project(SvenSoundcacheControlFormat LANGUAGES NONE)
    include(cmake/FormatValidation.cmake)
    return()
endif()
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(SvenSoundcacheControl VERSION 1.0.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SvenSoundcacheControl requires Windows MSVC x86 (-A Win32).")
endif()
include(cmake/Dependencies.cmake)
svensoundcachecontrol_prepare_dependencies()
add_library(SvenSoundcacheControl SHARED src/plugins.cpp src/soundcache_control.h
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/common/interface.cpp")
target_compile_features(SvenSoundcacheControl PRIVATE cxx_std_11)
target_include_directories(SvenSoundcacheControl PRIVATE
    "${METAHOOK_SOURCE_PATH}/include"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/cl_dll"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/common"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/engine"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/pm_shared"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/public"
    "${METAHOOK_SOURCE_PATH}/include/Interface")
target_compile_definitions(SvenSoundcacheControl PRIVATE WIN32 _WINDOWS _USRDLL _MBCS)
target_compile_options(SvenSoundcacheControl PRIVATE /W3 /MP /Zi /wd4311 /wd4312 /wd4819 /wd4996)
target_link_options(SvenSoundcacheControl PRIVATE /DEBUG /SUBSYSTEM:WINDOWS)
set_target_properties(SvenSoundcacheControl PROPERTIES DEBUG_POSTFIX "" INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
install(TARGETS SvenSoundcacheControl RUNTIME DESTINATION svencoop/metahook/plugins)
install(FILES "$<TARGET_PDB_FILE:SvenSoundcacheControl>" DESTINATION svencoop/metahook/plugins)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SVENSOUNDCACHECONTROL_GAMEDATA_DIR "${PROJECT_BINARY_DIR}/assets/svencoop/metahook/gamedata/svensoundcachecontrol")
set(_soundcache_manifest "${PROJECT_SOURCE_DIR}/scripts/manifests/svensoundcachecontrol.json")
add_custom_target(SvenSoundcacheControlGameData
    COMMAND "${Python3_EXECUTABLE}" "${METAHOOK_SOURCE_PATH}/scripts/sync-gamedata.py"
        --manifest "${_soundcache_manifest}" --target-dir "${SVENSOUNDCACHECONTROL_GAMEDATA_DIR}"
        --temp-root "${PROJECT_BINARY_DIR}/gamedata-sync"
    COMMAND "${Python3_EXECUTABLE}" "${METAHOOK_SOURCE_PATH}/scripts/validate-gamedata.py"
        "${SVENSOUNDCACHECONTROL_GAMEDATA_DIR}" --manifest "${_soundcache_manifest}"
    COMMENT "Synchronizing and validating SvenSoundcacheControl gamedata" VERBATIM)
add_dependencies(SvenSoundcacheControl SvenSoundcacheControlGameData)
install(DIRECTORY "${SVENSOUNDCACHECONTROL_GAMEDATA_DIR}/" DESTINATION svencoop/metahook/gamedata/svensoundcachecontrol)

option(SVENSOUNDCACHECONTROL_BUILD_TESTS "Build soundcache behavior tests" ON)
if(SVENSOUNDCACHECONTROL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    option(METAHOOKSV_ENABLE_LAUNCH_GAME "Add Visual Studio F5 deploy/debug targets" OFF)
    if(METAHOOKSV_ENABLE_LAUNCH_GAME)
        include(cmake/LaunchGame.cmake)
        metahooksv_add_launch_game()
    endif()
endif()
include(cmake/FormatValidation.cmake)
