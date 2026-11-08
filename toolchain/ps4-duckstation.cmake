include(${CMAKE_CURRENT_LIST_DIR}/ps4-love-modern.cmake)

set(PS4 TRUE)
list(PREPEND CMAKE_FIND_ROOT_PATH ${PS4_SYSROOT})
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES} CMAKE_FIND_ROOT_PATH PS4)
foreach(_lang C CXX ASM)
    string(REPLACE "-I${OPENORBIS}/usr/include" "" CMAKE_${_lang}_FLAGS_INIT "${CMAKE_${_lang}_FLAGS_INIT}")
    string(REGEX REPLACE "-DPS4([ \t\r\n]|$)" "\\1" CMAKE_${_lang}_FLAGS_INIT "${CMAKE_${_lang}_FLAGS_INIT}")
endforeach()
