# Fail the build if .data does not start where .text ends (see CMakeLists.txt:
# Elf2Mac drops the gap, and the application then crashes before main).
file(STRINGS ${SECTIONS} lines REGEX "^ +[0-9]+ \\.(text|data) ")
foreach(l ${lines})
    string(REGEX MATCH "\\.(text|data) +([0-9a-f]+) +([0-9a-f]+)" m "${l}")
    set(${CMAKE_MATCH_1}_size ${CMAKE_MATCH_2})
    set(${CMAKE_MATCH_1}_vma ${CMAKE_MATCH_3})
endforeach()
math(EXPR text_end "0x${text_vma} + 0x${text_size}")
math(EXPR data_start "0x${data_vma}")
if(NOT text_end EQUAL data_start)
    message(FATAL_ERROR ".data starts at ${data_start}, .text ends at ${text_end}: "
        "the application would relocate itself wrongly. Something is over-aligned.")
endif()
