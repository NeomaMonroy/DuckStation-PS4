set(PS4_RUNTIME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_modules.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_dlmalloc.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crashlog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_sampler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_string.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_random.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_net.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_netfs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_fileat.cpp)

add_compile_options(-march=btver2)

set(PS4_LIBC_HEAP_MIB 64 CACHE STRING "System libc heap size (MiB) declared in crt1.o")
set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-heap${PS4_LIBC_HEAP_MIB}.o)
execute_process(
  COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/patch-crt1.sh ${OPENORBIS}/lib/crt1.o ${PS4_CRT1}
          ${PS4_LIBC_HEAP_MIB}
  RESULT_VARIABLE _ps4_crt1_result)
if(NOT _ps4_crt1_result EQUAL 0)
  message(FATAL_ERROR "patch-crt1.sh failed: ${_ps4_crt1_result}")
endif()
option(PS4_MALLOC_REPLACE "Route all mallocs, system modules included, to the app heap" ON)
if(PS4_MALLOC_REPLACE)
  set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-malloc-replace.o)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -target x86_64-pc-freebsd12-elf -c
            ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crt1.S -o ${PS4_CRT1}
    RESULT_VARIABLE _ps4_crt1_result)
  if(NOT _ps4_crt1_result EQUAL 0)
    message(FATAL_ERROR "assembling port/ps4_crt1.S failed: ${_ps4_crt1_result}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crt1.S)
endif()
foreach(_lang C CXX)
  string(REPLACE "${OPENORBIS}/lib/crt1.o" "${PS4_CRT1}" CMAKE_${_lang}_LINK_EXECUTABLE
                 "${CMAKE_${_lang}_LINK_EXECUTABLE}")
endforeach()

set(PS4_LINK_SCRIPT_IN ${OPENORBIS}/link.x)
set(PS4_LINK_SCRIPT ${CMAKE_BINARY_DIR}/ps4-link.x)
file(READ ${PS4_LINK_SCRIPT_IN} PS4_LINK_SCRIPT_TEXT)
string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(.init_array);" PS4_INIT_ARRAY_POS)
if (PS4_INIT_ARRAY_POS EQUAL -1)
    message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(.init_array);' rule to patch")
endif ()
string(REPLACE "*(.init_array);"
    "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))); KEEP(*(.init_array));"
    PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
foreach (rule ".data" ".tdata" ".tbss")
    string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(${rule})" PS4_RULE_POS)
    if (PS4_RULE_POS EQUAL -1)
        message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(${rule})' rule to patch")
    endif ()
    string(REPLACE "*(${rule})" "*(${rule} ${rule}.*)" PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
endforeach ()
file(WRITE ${PS4_LINK_SCRIPT} "${PS4_LINK_SCRIPT_TEXT}")
string(REPLACE "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ps4-link.x")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${PS4_LINK_SCRIPT}")
endif ()

foreach (fn malloc free calloc realloc memalign __memalign)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=${fn}")
endforeach ()
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=pthread_once")
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=mmap")
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=clock_gettime")
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=pthread_create")
set(PS4_NETFS_WRAPS --wrap=open --wrap=close)
foreach (fn read _read readv _readv pread lseek fstat stat getdents)
    list(APPEND PS4_NETFS_WRAPS --wrap=${fn})
endforeach ()
foreach (fn fstatat openat unlinkat fchmodat utimensat realpath readlink statvfs)
    list(APPEND PS4_NETFS_WRAPS --wrap=${fn})
endforeach ()

set(PS4_PAID "0x3100000000000002" CACHE STRING "Program authority ID of eboot.bin")
set(PS4_AUTH_INFO "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

set(PS4_MESA_BUILD "$ENV{HOME}/dk64-build/build/mesa" CACHE PATH "Mesa PS4 build directory")
function(ps4_link_mesa target)
    set(libs
        src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a
        src/amd/compiler/libaco.a src/amd/addrlib/libaddrlib.a src/vulkan/util/libvulkan_util.a
        src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
        src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
        src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libparson.a
        src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
    list(TRANSFORM libs PREPEND "${PS4_MESA_BUILD}/")
    target_link_libraries(${target} PRIVATE --whole-archive ${libs} --no-whole-archive)
endfunction()

function(ps4_add_eboot target)
    target_link_options(${target} PRIVATE ${PS4_NETFS_WRAPS})
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_eboot)
    add_custom_command(
        OUTPUT ${out_dir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OPENORBIS}
            ${OPENORBIS}/bin/create-fself -in=$<TARGET_FILE:${target}>
            -out=${out_dir}/${target}.oelf --eboot ${out_dir}/eboot.bin
            --paid ${PS4_PAID} --authinfo ${PS4_AUTH_INFO}
        COMMAND ${CMAKE_COMMAND} -DOELF=${out_dir}/${target}.oelf
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check-oelf.cmake
        DEPENDS ${target}
        VERBATIM)
    add_custom_target(${target}_eboot ALL DEPENDS ${out_dir}/eboot.bin)
endfunction()
