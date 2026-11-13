set(METAHOOK_SOURCE_PATH "$ENV{METAHOOK_SOURCE_PATH}" CACHE PATH "MetaHook source tree; empty fetches main")

# The plugin compiles part of the SDK and includes its public headers, so an
# external tree must provide both the sources and the interfaces it uses.
function(svensoundcachecontrol_validate_metahook_source source)
    foreach(required include/metahook.h include/HLSDK/common/interface.cpp
        include/HLSDK/common/cvardef.h include/Interface/IPlugins.h
        scripts/sync-gamedata.py scripts/validate-gamedata.py)
        if(NOT EXISTS "${source}/${required}" OR IS_DIRECTORY "${source}/${required}")
            message(FATAL_ERROR "METAHOOK_SOURCE_PATH is missing ${required}: ${source}")
        endif()
    endforeach()
endfunction()

function(svensoundcachecontrol_prepare_dependencies)
    # External trees are read-only inputs; validate explicit paths before downloading anything.
    if(METAHOOK_SOURCE_PATH)
        get_filename_component(metahook_source "${METAHOOK_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(svensoundcachecontrol_metahook
            GIT_REPOSITORY https://github.com/MetaHookSv/MetaHook
            # MetaHook is tracked as a branch: always fetch the latest main.
            GIT_TAG origin/main
            GIT_SUBMODULES ""
            # This SDK directory has no CMakeLists.txt: populate without building the launcher.
            SOURCE_SUBDIR include
        )
        FetchContent_MakeAvailable(svensoundcachecontrol_metahook)
        set(metahook_source "${svensoundcachecontrol_metahook_SOURCE_DIR}")
    endif()
    svensoundcachecontrol_validate_metahook_source("${metahook_source}")
    set(METAHOOK_SOURCE_PATH "${metahook_source}" PARENT_SCOPE)
    message(STATUS "METAHOOK_SOURCE_PATH: ${metahook_source}")

    # SvenSoundcacheControl consumes no Capstone and no SourceSDK headers, so there is
    # nothing else to resolve beyond the SDK tree.
endfunction()
