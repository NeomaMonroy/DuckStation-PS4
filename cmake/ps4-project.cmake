include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)

set(PS4_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "" FORCE)
get_filename_component(PS4_PORT_DIR "${PS4_PORT_DIR}" ABSOLUTE)

add_compile_definitions(FMT_CONSTEVAL=)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

function(ps4_duckstation_app target)
    target_sources(${target} PRIVATE
        ${PS4_PORT_DIR}/ps4/ps4_host.cpp
        ${PS4_PORT_DIR}/ps4/ps4_log.cpp
        ${PS4_PORT_DIR}/ps4/ps4_wsi.cpp
        ${PS4_PORT_DIR}/ps4/ps4_compat.cpp
        ${PS4_PORT_DIR}/port/ps4_relpath.cpp
        ${PS4_RUNTIME_SOURCES})
    set(_ps4_sources
        ${PS4_PORT_DIR}/ps4/ps4_host.cpp ${PS4_PORT_DIR}/ps4/ps4_log.cpp ${PS4_PORT_DIR}/ps4/ps4_wsi.cpp
        ${PS4_PORT_DIR}/ps4/ps4_compat.cpp ${PS4_PORT_DIR}/port/ps4_relpath.cpp ${PS4_RUNTIME_SOURCES})
    set_source_files_properties(${_ps4_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    foreach(_src IN LISTS _ps4_sources)
        if(_src MATCHES "\\.cpp$")
            set_source_files_properties(${_src} PROPERTIES COMPILE_OPTIONS "-fexceptions;-frtti")
        endif()
    endforeach()
    foreach(fn chdir getcwd lstat mkdir rmdir unlink rename access opendir chmod remove truncate utimes)
        target_link_options(${target} PRIVATE --wrap=${fn})
    endforeach()
    target_include_directories(${target} BEFORE PRIVATE ${PS4_PORT_DIR}/ps4/compat)
    target_include_directories(${target} PRIVATE ${PS4_PORT_DIR}/ps4 ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${target} PRIVATE vulkan-headers)
    target_link_options(${target} PRIVATE --error-limit=0 --undefined=fstatat)
    ps4_link_mesa(${target})
    target_link_libraries(${target} PRIVATE
        SceSysmodule SceSystemService SceUserService SceVideoOut SceAudioOut ScePad SceNet)
    ps4_add_eboot(${target})
endfunction()
