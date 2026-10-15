#[[

Some notes on the DEBUG flag hierarchy:

The idea is that there are general flags that enable debugging across the whole
subsystem, but at a shallow level, and then ones that go a layer deeper (e.g.
DEBUG_SLAB vs DEBUG_SLAB_DEEP). This is to allow us to do surface level
assertions for cheap, but opt for in depth instrumentation if something
pops up and needs to be debugged. It is not mandatory that all subsystems
that feature debugging flags implement this hierarchy, however it is standard.

Implications and the AUTO state:

    ON     enabled, explicitly
    OFF    disabled, explicitly -- no implication may override this
    AUTO   (default) no opinion; implications decide

-DDEBUG_ASAN=ON gets you KASAN plus the slab stack-trace tracking, while
-DDEBUG_ASAN=ON -DDEBUG_SLAB_DEEP=OFF gets you KASAN on its own

]]

set(DEBUG_FLAGS
    #
    # Synchronization
    DEBUG_LOCK_CHK
    #
    # Scheduling and threads
    DEBUG_SCHED
    DEBUG_SCHED_NESTING
    DEBUG_CLIMB
    #
    # Drivers
    DEBUG_USB
    DEBUG_USB_XHCI
    #
    # Memory management
    DEBUG_ARENA
    DEBUG_SLAB
    DEBUG_SLAB_DEEP
    DEBUG_ALLOC_PARAM
    #
    # Misc.
    DEBUG_LIST
    DEBUG_RBT
    DEBUG_RBT_DEEP
    DEBUG_CMDLINE
    DEBUG_ENFORCE # Debug enforcement
)

set(PROFILING_FLAGS PROFILING_SCHED)

set(TEST_NIGHTMARE_FLAGS TEST_NIGHTMARE_LOCKS TEST_NIGHTMARE_WAKE TEST_NIGHTMARE_SMOKE)

# driver:implied -- soft, overridable by setting the implied flag to OFF
set(DEBUG_FLAG_MAP DEBUG_ASAN:DEBUG_SLAB_DEEP)

option(DEBUG_ASAN "Enable KASAN address sanitizer (clang only)" OFF)

# Every implication target is tri-state, so an OFF can veto
set(TRISTATE_FLAGS "")
foreach (pair ${DEBUG_FLAG_MAP})
    string(REPLACE ":" ";" _kv "${pair}")
    list(GET _kv 1 _implied)
    list(APPEND TRISTATE_FLAGS ${_implied})
endforeach ()
list(REMOVE_DUPLICATES TRISTATE_FLAGS)

macro (_flag_is_auto flag out)
    if ("${${flag}}" STREQUAL "AUTO")
        set(${out} TRUE)
    else ()
        set(${out} FALSE)
    endif ()
endmacro ()

function (declare_tristate_flags FLAGS)
    foreach (flag ${FLAGS})
        if (DEFINED CACHE{${flag}})
            get_property(
                _type
                CACHE ${flag}
                PROPERTY TYPE)
            if (NOT "${_type}" STREQUAL "STRING")
                if (${flag})
                    set(_carried ON)
                else ()
                    set(_carried OFF)
                endif ()
                unset(${flag} CACHE)
                set(${flag}
                    ${_carried}
                    CACHE STRING "Enable ${flag}: ON / OFF / AUTO")
            endif ()
        else ()
            set(${flag}
                AUTO
                CACHE STRING "Enable ${flag}: ON / OFF / AUTO")
        endif ()
        set_property(CACHE ${flag} PROPERTY STRINGS ON OFF AUTO)
    endforeach ()
endfunction ()

function (declare_flag_group GROUP_NAME ENABLE_ALL DEFAULT_ALL FLAGS)
    option(${ENABLE_ALL} "Enable all ${GROUP_NAME} flags" ${DEFAULT_ALL})
    foreach (flag ${FLAGS})
        if (NOT flag IN_LIST TRISTATE_FLAGS)
            option(${flag} "Enable ${flag}" OFF)
        endif ()
    endforeach ()
endfunction ()

macro (_apply_enable_all ENABLE_ALL FLAGS)
    if (${ENABLE_ALL})
        foreach (flag ${FLAGS})
            _flag_is_auto(${flag} _is_auto)
            if (_is_auto OR NOT flag IN_LIST TRISTATE_FLAGS)
                set(${flag} ON)
            endif ()
        endforeach ()
    endif ()
endmacro ()

macro (_normalize_tristate_flags)
    foreach (flag ${TRISTATE_FLAGS})
        _flag_is_auto(${flag} _is_auto)
        if (_is_auto)
            set(${flag} OFF)
        endif ()
    endforeach ()
endmacro ()

function (emit_flag_group GROUP_NAME ENABLE_ALL FLAGS)
    set(_LOCAL_GROUP_ENABLED OFF)
    foreach (flag ${FLAGS})
        if (${flag})
            set(_LOCAL_GROUP_ENABLED ON)
            add_compile_definitions(${flag})
        endif ()
    endforeach ()

    set(${GROUP_NAME}_ENABLED
        ${_LOCAL_GROUP_ENABLED}
        PARENT_SCOPE)

    if (${ENABLE_ALL})
        add_compile_definitions(${ENABLE_ALL})
    endif ()
    if (_LOCAL_GROUP_ENABLED)
        add_compile_definitions(${GROUP_NAME}_ENABLED)
    endif ()
endfunction ()

declare_tristate_flags("${TRISTATE_FLAGS}")

declare_flag_group(PROFILING PROFILING_ALL OFF "${PROFILING_FLAGS}")
declare_flag_group(TEST_NIGHTMARE TEST_NIGHTMARE_ALL ON "${TEST_NIGHTMARE_FLAGS}")
declare_flag_group(DEBUG DEBUG_ALL OFF "${DEBUG_FLAGS}")

_apply_enable_all(PROFILING_ALL "${PROFILING_FLAGS}")
_apply_enable_all(TEST_NIGHTMARE_ALL "${TEST_NIGHTMARE_FLAGS}")
_apply_enable_all(DEBUG_ALL "${DEBUG_FLAGS}")

foreach (pair ${DEBUG_FLAG_MAP})
    string(REPLACE ":" ";" _kv "${pair}")
    list(GET _kv 0 _debug_flag)
    list(GET _kv 1 _other_flag)
    _flag_is_auto(${_other_flag} _is_auto)
    if (${_debug_flag} AND _is_auto)
        set(${_other_flag} ON)
    elseif (${_debug_flag} AND NOT ${_other_flag})
        message(STATUS "charmOS: ${_debug_flag} would imply ${_other_flag}, "
                       "honouring the explicit ${_other_flag}=OFF")
    endif ()
endforeach ()

_normalize_tristate_flags()

foreach (flag ${DEBUG_FLAGS})
    if (flag MATCHES "_DEEP" AND ${flag})
        string(LENGTH ${flag} len)
        math(EXPR new_len "${len} - 5")
        string(SUBSTRING "${flag}" 0 ${new_len} trimmed)
        if (trimmed IN_LIST DEBUG_FLAGS)
            set(${trimmed} ON)
        endif ()
    endif ()
endforeach ()

emit_flag_group(PROFILING PROFILING_ALL "${PROFILING_FLAGS}")
emit_flag_group(TEST_NIGHTMARE TEST_NIGHTMARE_ALL "${TEST_NIGHTMARE_FLAGS}")
emit_flag_group(DEBUG DEBUG_ALL "${DEBUG_FLAGS}")
