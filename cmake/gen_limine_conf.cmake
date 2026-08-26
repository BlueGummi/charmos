# Copy limine.conf and append a kernel command line to it
#
# Mutually exclusive, in precedenec:
#
# CMDLINE=<path>          read <path>, append its contents NIGHTMARE_TESTS=<name>  shorthand, expands to
# nightmare=<name> TESTS=a,b,c             expands to test.filter=a,b,c
#
# Giving more than one is a configure-time error
#
# EXTRA_CMDLINE=<string> can be used to append at the end

if (NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "gen_limine_conf: IN and OUT must both be defined")
endif ()

file(READ "${IN}" _conf)

set(_cmdline_file "$ENV{CMDLINE}")
set(_nightmare "$ENV{NIGHTMARE_TESTS}")
set(_tests "$ENV{TESTS}")
set(_extra "$ENV{EXTRA_CMDLINE}")
foreach (_var _cmdline_file _nightmare _tests _extra)
    string(STRIP "${${_var}}" ${_var})
endforeach ()

set(_sources "")
if (NOT _cmdline_file STREQUAL "")
    list(APPEND _sources "CMDLINE")
endif ()
if (NOT _nightmare STREQUAL "")
    list(APPEND _sources "NIGHTMARE_TESTS")
endif ()
if (NOT _tests STREQUAL "")
    list(APPEND _sources "TESTS")
endif ()
list(LENGTH _sources _source_count)
if (_source_count GREATER 1)
    string(REPLACE ";" ", " _source_list "${_sources}")
    message(FATAL_ERROR "gen_limine_conf: ${_source_list} are set together")
endif ()

set(_line "")

if (NOT _cmdline_file STREQUAL "")
    if (NOT EXISTS "${_cmdline_file}")
        message(FATAL_ERROR "gen_limine_conf: CMDLINE points at '${_cmdline_file}', " "which does not exist")
    endif ()
    file(READ "${_cmdline_file}" _line)
    string(REGEX REPLACE "[\r\n]+" " " _line "${_line}")
    string(STRIP "${_line}" _line)
    if (_line STREQUAL "")
        message(FATAL_ERROR "gen_limine_conf: CMDLINE file '${_cmdline_file}' is empty")
    endif ()

elseif (NOT _nightmare STREQUAL "")
    if (_nightmare MATCHES "[ \t,]")
        message(FATAL_ERROR "gen_limine_conf: NIGHTMARE_TESTS takes exactly one test name " "(got '${_nightmare}')")
    endif ()
    set(_line "nightmare=${_nightmare}")

elseif (NOT _tests STREQUAL "")
    string(REGEX REPLACE "[ \t,]+" "," _tests "${_tests}")

    if (_tests MATCHES "^,|,$")
        message(FATAL_ERROR "gen_limine_conf: TESTS list has a leading or trailing comma " "(got '$ENV{TESTS}')")
    endif ()

    set(_line "test.filter=${_tests}")
endif ()

if (NOT _extra STREQUAL "")
    set(_line "${_line} ${_extra}")
    string(STRIP "${_line}" _line)
endif ()

if (_line STREQUAL "")
    file(WRITE "${OUT}" "${_conf}")
    return()
endif ()

if (_conf MATCHES "\n[ \t]*cmdline:[^\n]*")
    string(REGEX REPLACE "(\n[ \t]*cmdline:[^\n]*)" "\\1 ${_line}" _conf "${_conf}")
else ()
    string(REGEX REPLACE "(\n[ \t]*path:[^\n]*)" "\\1\n    cmdline: ${_line}" _conf "${_conf}")
endif ()

file(WRITE "${OUT}" "${_conf}")
