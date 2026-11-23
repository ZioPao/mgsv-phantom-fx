# Precompiles HLSL at build time with fxc, so a release build never compiles a shader at run time.
#
#     include(<core>/cmake/phantom_shaders.cmake)
#     phantom_precompile_shader(my_addon
#         SOURCE   shaders/pool_ps.hlsl
#         PROFILE  ps_5_0
#         VARIABLE g_poolPs                 # the byte array's name in the header
#         INCLUDES shaders/common.hlsli     # every file the source includes: a change rebuilds the header
#         DEFINES  QUALITY=2)
#
# The header lands in <build>/phantom_shaders/<source name>.hpp (include it as "phantom_shaders/<name>.hpp")
# and holds `const BYTE <VARIABLE>[]`. It is regenerated whenever the source or an INCLUDES file changes,
# and a compile error fails the build.
include_guard(GLOBAL)

# fxc ships with the Windows SDK, not on PATH. Prefer the SDK the generator picked, then the newest installed.
if(NOT PHANTOM_FXC)
    set(_phantom_sdk_roots "$ENV{WindowsSdkVerBinPath}" "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}")
    if(CMAKE_VS_WINDOWS_SDK_VERSION)
        list(APPEND _phantom_sdk_roots "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_SDK_VERSION}")
    endif()
    file(GLOB _phantom_sdk_bins "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*")
    list(SORT _phantom_sdk_bins ORDER DESCENDING)
    list(APPEND _phantom_sdk_roots ${_phantom_sdk_bins})
    find_program(PHANTOM_FXC fxc
        HINTS ${_phantom_sdk_roots}
        PATH_SUFFIXES x64
        DOC "The HLSL compiler (fxc.exe) that precompiles shaders.")
endif()
if(NOT PHANTOM_FXC)
    message(FATAL_ERROR "fxc.exe not found. Install the Windows SDK or set -DPHANTOM_FXC=<path to fxc.exe>.")
endif()



function(phantom_precompile_shader target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE;PROFILE;VARIABLE;ENTRY" "INCLUDES;DEFINES")
    foreach(required SOURCE PROFILE VARIABLE)
        if(NOT arg_${required})
            message(FATAL_ERROR "phantom_precompile_shader: ${required} is required.")
        endif()
    endforeach()
    if(NOT arg_ENTRY)
        set(arg_ENTRY main)
    endif()

    cmake_path(ABSOLUTE_PATH arg_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE source)
    cmake_path(GET source PARENT_PATH source_dir)
    cmake_path(GET source STEM name)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/phantom_shaders/${name}.hpp")

    set(include_files "")
    foreach(file IN LISTS arg_INCLUDES)
        cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE absolute)
        list(APPEND include_files "${absolute}")
    endforeach()

    set(define_flags "")
    foreach(define IN LISTS arg_DEFINES)
        list(APPEND define_flags "/D" "${define}")
    endforeach()

    # /O3 matches what D3DCompile used at run time. /Qstrip_debug and /Qstrip_reflect keep the blob small; the
    # game's own shaders are what carry debug data, not ours.
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/phantom_shaders"
        COMMAND "${PHANTOM_FXC}" /nologo /T ${arg_PROFILE} /E ${arg_ENTRY} /O3 /Qstrip_debug /Qstrip_reflect
                /I "${source_dir}" ${define_flags} /Vn ${arg_VARIABLE} /Fh "${output}" "${source}"
        DEPENDS "${source}" ${include_files}
        COMMENT "fxc ${arg_PROFILE}: ${name}.hlsl"
        VERBATIM)

    target_sources(${target} PRIVATE "${output}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
