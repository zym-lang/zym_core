# components/zym_core/cmake/esp-idf.cmake

message(STATUS "zym_core: CMAKE_CURRENT_LIST_DIR='${CMAKE_CURRENT_LIST_DIR}'")
message(STATUS "zym_core: COMPONENT_DIR='${COMPONENT_DIR}'")

# Compute component root from this file location: .../zym_core/cmake -> .../zym_core
get_filename_component(ZYM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
message(STATUS "zym_core: ZYM_ROOT='${ZYM_ROOT}'")

# Hard fail early if paths are not what we expect (prevents /include nonsense)
if(NOT EXISTS "${ZYM_ROOT}/include")
    message(FATAL_ERROR "zym_core: expected include dir at '${ZYM_ROOT}/include' but it does not exist")
endif()
if(NOT EXISTS "${ZYM_ROOT}/src")
    message(FATAL_ERROR "zym_core: expected src dir at '${ZYM_ROOT}/src' but it does not exist")
endif()

set(ZYM_CORE_SOURCES
        ${ZYM_ROOT}/src/value.c
        ${ZYM_ROOT}/src/chunk.c
        ${ZYM_ROOT}/src/vm.c
        ${ZYM_ROOT}/src/object.c
        ${ZYM_ROOT}/src/memory.c
        ${ZYM_ROOT}/src/table.c
        ${ZYM_ROOT}/src/serializer.c
        ${ZYM_ROOT}/src/utils.c
        ${ZYM_ROOT}/src/zym.c
        ${ZYM_ROOT}/src/gc.c
        ${ZYM_ROOT}/src/native.c
        ${ZYM_ROOT}/src/utf8.c
        ${ZYM_ROOT}/src/modules/core_modules.c
        ${ZYM_ROOT}/src/modules/continuation.c
        ${ZYM_ROOT}/src/modules/preemption.c
        ${ZYM_ROOT}/src/natives/core_natives.c
        ${ZYM_ROOT}/src/natives/gc_native.c
        ${ZYM_ROOT}/src/natives/conversions.c
        ${ZYM_ROOT}/src/natives/error.c
        ${ZYM_ROOT}/src/natives/list.c
        ${ZYM_ROOT}/src/natives/map.c
        ${ZYM_ROOT}/src/natives/shared.c
        ${ZYM_ROOT}/src/natives/string_natives.c
        ${ZYM_ROOT}/src/natives/math.c
        ${ZYM_ROOT}/src/natives/typeof.c
)

if(NOT CONFIG_ZYM_RUNTIME_ONLY)
    list(APPEND ZYM_CORE_SOURCES
            ${ZYM_ROOT}/src/scanner.c
            ${ZYM_ROOT}/src/preprocessor.c
            ${ZYM_ROOT}/src/ast.c
            ${ZYM_ROOT}/src/parser.c
            ${ZYM_ROOT}/src/compiler.c
            ${ZYM_ROOT}/src/debug.c
            ${ZYM_ROOT}/src/linemap.c
            ${ZYM_ROOT}/src/module_loader.c
    )
endif()

idf_component_register(
        SRCS ${ZYM_CORE_SOURCES}
        INCLUDE_DIRS ${ZYM_ROOT}/include
        PRIV_INCLUDE_DIRS ${ZYM_ROOT}/src
)

if(CONFIG_ZYM_RUNTIME_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ZYM_RUNTIME_ONLY)
endif()