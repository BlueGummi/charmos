# Copy limine.conf and generate a new one according to feature flags
#
# TESTS=a,b,c    becomes test.filter=a,b,c
# CMDLINE="..."  is appended

if(NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "gen_limine_conf: IN and OUT must both be defined")
endif()

file(READ "${IN}" _conf)

set(_tests "$ENV{TESTS}")
string(STRIP "${_tests}" _tests)

set(_extra "$ENV{CMDLINE}")
string(STRIP "${_extra}" _extra)

if(NOT _tests STREQUAL "")
    # var=a,b,c is the canonical pattern
    string(REGEX REPLACE "[ \t,]+" "," _tests "${_tests}")

    # "a, b, c," is malformed
    if(_tests MATCHES "^,|,$")
        message(FATAL_ERROR
            "gen_limine_conf: TESTS list has a leading or trailing comma "
            "(got '$ENV{TESTS}'); remove the empty element")
    endif()

    set(_extra "test.filter=${_tests} ${_extra}")
    string(STRIP "${_extra}" _extra)
endif()

if(_extra STREQUAL "")
    file(WRITE "${OUT}" "${_conf}")
    return()
endif()

if(_conf MATCHES "\n[ \t]*cmdline:[^\n]*")
    string(REGEX REPLACE "(\n[ \t]*cmdline:[^\n]*)"
           "\\1 ${_extra}" _conf "${_conf}")
else()
    string(REGEX REPLACE "(\n[ \t]*path:[^\n]*)"
           "\\1\n    cmdline: ${_extra}" _conf "${_conf}")
endif()

file(WRITE "${OUT}" "${_conf}")
