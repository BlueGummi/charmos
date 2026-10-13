#[[

Test flags

Tests live in discrete and defined places, so the build system
simply gets the tests off the source tree

    kernel/<sub>/tests/          ->  TEST_<SUB>          (derived)
    nested, or kernel/tests/     ->  name from tests.toml (file REQUIRED)

Compiling a test in happens at this granularity,
and a directory is compiled when TEST_ALL is on, or when its own flag is on

tests.toml is parsed by cmake/toml.cmake

]]

include(toml)

set(TEST_TREE_ROOT "${CMAKE_SOURCE_DIR}/kernel")

set(TEST_DIR_EXCLUDED_FROM_RULE "${TEST_TREE_ROOT}/nightmare/tests")

option(TEST_ALL "Enable every test directory" ON)

set(TEST_TIERS unit smoke integration)

file(
    GLOB_RECURSE _test_tree_entries
    LIST_DIRECTORIES true
    CONFIGURE_DEPENDS "${TEST_TREE_ROOT}/*")
list(APPEND _test_tree_entries "${TEST_TREE_ROOT}/tests")

set(_test_dirs "")
foreach (_entry ${_test_tree_entries})
    if (IS_DIRECTORY "${_entry}" AND _entry MATCHES "/tests$")
        if (NOT _entry MATCHES "/uACPI/")
            list(APPEND _test_dirs "${_entry}")
        endif ()
    endif ()
endforeach ()
list(REMOVE_DUPLICATES _test_dirs)
list(SORT _test_dirs)

set(TEST_DIRS_DISABLED "")
set(TEST_FLAGS_ENABLED "")
set(TEST_FLAGS_DISABLED "")
set(_seen_names "")

foreach (_dir ${_test_dirs})
    if (_dir IN_LIST TEST_DIR_EXCLUDED_FROM_RULE)
        continue()
    endif ()

    file(RELATIVE_PATH _rel "${TEST_TREE_ROOT}" "${_dir}")
    string(REGEX REPLACE "/?tests$" "" _subsys "${_rel}")

    set(_toml "${_dir}/tests.toml")
    toml_read("${_toml}" _doc)
    toml_get("${_doc}" _name name)

    if (_name STREQUAL "" AND (_subsys STREQUAL "" OR _subsys MATCHES "/"))
        message(FATAL_ERROR "${_dir} is nested, needs tests.toml\n")
    elseif (_name STREQUAL "")
        set(_name "${_subsys}")
    endif ()

    if (NOT _name MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "${_toml}: name \"${_name}\" must be lower_snake_case")
    endif ()

    if (_name IN_LIST _seen_names)
        message(FATAL_ERROR "two test directories both have the name \"${_name}\"")
    endif ()
    list(APPEND _seen_names "${_name}")

    string(TOUPPER "${_name}" _name_upper)
    set(_flag "TEST_${_name_upper}")

    toml_get("${_doc}" _desc description)
    if (_desc STREQUAL "")
        set(_desc "${_name} tests")
    endif ()

    toml_get("${_doc}" _default default)
    if (_default STREQUAL "")
        set(_default OFF)
    elseif (NOT _default STREQUAL "ON" AND NOT _default STREQUAL "OFF")
        message(FATAL_ERROR "${_toml}: default must be a boolean (true/false), got \"${_default}\"")
    endif ()

    option(${_flag} "${_desc}" ${_default})

    if (TEST_ALL OR ${_flag})
        list(APPEND TEST_FLAGS_ENABLED "${_flag}")
    elseif (_dir STREQUAL "${TEST_TREE_ROOT}/tests")
        file(
            GLOB _framework_tests
            LIST_DIRECTORIES true
            "${_dir}/*")
        foreach (_sub ${_framework_tests})
            if (IS_DIRECTORY "${_sub}")
                list(APPEND TEST_DIRS_DISABLED "${_sub}")
            endif ()
        endforeach ()
        list(APPEND TEST_FLAGS_DISABLED "${_flag}")
    else ()
        list(APPEND TEST_DIRS_DISABLED "${_dir}")
        list(APPEND TEST_FLAGS_DISABLED "${_flag}")
    endif ()
endforeach ()

list(LENGTH TEST_FLAGS_ENABLED TEST_ENABLED_COUNT)
list(LENGTH TEST_FLAGS_DISABLED TEST_DISABLED_COUNT)

if (TEST_ENABLED_COUNT GREATER 0)
    add_compile_definitions(TEST_ENABLED)
endif ()
if (TEST_ALL)
    add_compile_definitions(TEST_ALL)
endif ()

# Printed by the root CMakeLists status block
function (test_report_configuration)
    message(STATUS "  Test dirs    : ${TEST_ENABLED_COUNT} on, ${TEST_DISABLED_COUNT} off (TEST_ALL=${TEST_ALL})")
    foreach (_f ${TEST_FLAGS_ENABLED})
        message(STATUS "                 ${_f}")
    endforeach ()
    foreach (_f ${TEST_FLAGS_DISABLED})
        message(STATUS "                 ${_f}  [off]")
    endforeach ()
endfunction ()
