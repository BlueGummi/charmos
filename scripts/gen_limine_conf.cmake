# Copy limine.conf and generate a new one according to feature flags

if(NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "gen_limine_conf: IN and OUT must both be defined")
endif()

file(READ "${IN}" _conf)

set(_tests "$ENV{TESTS}")
string(STRIP "${_tests}" _tests)

if(_tests STREQUAL "")
    file(WRITE "${OUT}" "${_conf}")
    return()
endif()

# var=a,b,c is the canonical pattern
string(REGEX REPLACE "[ \t,]+" "," _tests "${_tests}")

# "a, b, c," is malformed syntax
if(_tests MATCHES "^,|,$")
    message(FATAL_ERROR
        "gen_limine_conf: TESTS list has a leading or trailing comma "
        "(got '$ENV{TESTS}'); remove the empty element")
endif()

if(_conf MATCHES "\n[ \t]*cmdline:[^\n]*")
    string(REGEX REPLACE "(\n[ \t]*cmdline:[^\n]*)"
           "\\1 test.filter=${_tests}" _conf "${_conf}")
else()
    string(REGEX REPLACE "(\n[ \t]*path:[^\n]*)"
           "\\1\n    cmdline: test.filter=${_tests}" _conf "${_conf}")
endif()

file(WRITE "${OUT}" "${_conf}")
