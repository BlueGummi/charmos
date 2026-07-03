#[[

Some notes on the DEBUG flag hierarchy:

The idea is that there are general flags that enable debugging across the whole
subsystem, but at a shallow level, and then ones that go a layer deeper (e.g.
DEBUG_SLAB vs DEBUG_SLAB_DEEP). This is to allow us to do surface level
assertions for cheap, but opt for in depth instrumentation if something
pops up and needs to be debugged. It is not mandatory that all subsystems
that feature debugging flags implement this hierarchy, however it is standard.

]]

set(DEBUG_FLAGS
    DEBUG_LOCK
    DEBUG_CLIMB
    DEBUG_USB
    DEBUG_USB_XHCI
    DEBUG_SLAB
    DEBUG_SLAB_DEEP
)

set(PROFILING_FLAGS
    PROFILING_SCHED
    PROFILING_VFS
)

set(TEST_FLAGS
    TEST_SCHED TEST_TMPFS TEST_EXT2 TEST_MEM TEST_MINHEAP
    TEST_RCU TEST_RWLOCK TEST_MUTEX TEST_TIMER_DEFER TEST_FS
    TEST_APC TEST_BIO TEST_BIO_SCHED TEST_LOG TEST_MISC
    TEST_RBIT TEST_MM TEST_FOLIO TEST_RMAP
)

set(TEST_NIGHTMARE_FLAGS
    TEST_NIGHTMARE_LOCKS
    TEST_NIGHTMARE_WAKE
)

function(define_flag_group GROUP_NAME ENABLE_ALL DEFAULT_ALL FLAGS)
    option(${ENABLE_ALL} "Enable all ${GROUP_NAME} flags" ${DEFAULT_ALL})
    foreach(flag ${FLAGS})
        option(${flag} "Enable ${flag}" OFF)
    endforeach()

    if(${ENABLE_ALL})
        foreach(flag ${FLAGS})
            set(${flag} ON CACHE BOOL "" FORCE)
        endforeach()
    endif()

    set(_LOCAL_GROUP_ENABLED OFF)
    foreach(flag ${FLAGS})
        if(${flag})
            set(_LOCAL_GROUP_ENABLED ON)
            add_compile_definitions(${flag})
        endif()
    endforeach()

    set(${GROUP_NAME}_ENABLED ${_LOCAL_GROUP_ENABLED} PARENT_SCOPE)

    if(${ENABLE_ALL})
        add_compile_definitions(${ENABLE_ALL})
    endif()
    if(_LOCAL_GROUP_ENABLED)
        add_compile_definitions(${GROUP_NAME}_ENABLED)
    endif()
endfunction()

define_flag_group(PROFILING       PROFILING_ALL       OFF "${PROFILING_FLAGS}")
define_flag_group(TEST            TEST_ALL            ON  "${TEST_FLAGS}")
define_flag_group(TEST_NIGHTMARE  TEST_NIGHTMARE_ALL  ON  "${TEST_NIGHTMARE_FLAGS}")
define_flag_group(DEBUG           DEBUG_ALL           OFF "${DEBUG_FLAGS}")

# A *_DEEP flag implies its shallow parent: enabling DEBUG_SLAB_DEEP should also
# turn on DEBUG_SLAB. Walk the enabled deep flags and pull their parents on.
get_directory_property(current_defns COMPILE_DEFINITIONS)
foreach(flag ${DEBUG_FLAGS})
    if(flag MATCHES "_DEEP" AND flag IN_LIST current_defns)
        string(LENGTH ${flag} len)
        math(EXPR new_len "${len} - 5")
        string(SUBSTRING "${flag}" 0 ${new_len} trimmed)
        if (trimmed IN_LIST DEBUG_FLAGS)
            set(${trimmed} ON CACHE BOOL "" FORCE)
            add_compile_definitions(${trimmed})
        endif()
    endif()
endforeach()
