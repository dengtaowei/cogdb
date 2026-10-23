cmake_minimum_required(VERSION 3.16)
project(cogdb VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions(_GNU_SOURCE _DEFAULT_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic)

option(COGDB_SANITIZE "Build with ASan/UBSan" OFF)
if(COGDB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(cjson STATIC deps/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC deps/cJSON)
target_compile_options(cjson PRIVATE -w)

# Layers, bottom up. Each only reaches downward.
add_library(cogdb_util STATIC
    src/util/buf.c
    src/util/hex.c
    src/util/log.c
    src/util/sys.c
)
target_include_directories(cogdb_util PUBLIC src)

add_library(cogdb_core STATIC
    src/core/loop.c
    src/core/conn.c
)
target_link_libraries(cogdb_core PUBLIC cogdb_util)

add_library(cogdb_rsp STATIC
    src/rsp/rsp.c
    src/rsp/stop.c
    src/rsp/tdesc.c
)
target_link_libraries(cogdb_rsp PUBLIC cogdb_util)

add_library(cogdb_transport STATIC
    src/transport/transport.c
    src/transport/tcp.c
    src/transport/unix.c
    src/transport/serial.c
)
target_link_libraries(cogdb_transport PUBLIC cogdb_util)

add_library(cogdb_hub STATIC
    src/hub/events.c
    src/hub/target.c
    src/hub/regmap.c
    src/hub/policy.c
    src/hub/gdbseat.c
    src/hub/grant.c
    src/hub/gdbcmd.c
    src/hub/miseat.c
    src/hub/ctl.c
    src/hub/ctl_cmd.c
    src/hub/record.c
    src/hub/hub.c
)
target_link_libraries(cogdb_hub PUBLIC cogdb_core cogdb_rsp cogdb_transport)
target_compile_definitions(cogdb_hub PRIVATE COGDB_VERSION="${PROJECT_VERSION}")

add_executable(cogdb
    src/cli/main.c
    src/cli/client.c
    src/cli/mcp.c
)
target_link_libraries(cogdb PRIVATE cogdb_hub cjson)
target_compile_definitions(cogdb PRIVATE COGDB_VERSION="${PROJECT_VERSION}")

include(CTest)
if(BUILD_TESTING)
    add_executable(test_unit tests/test_unit.c)
    target_include_directories(test_unit PRIVATE tests)
    target_link_libraries(test_unit PRIVATE cogdb_hub)
    add_test(NAME unit COMMAND test_unit)

    find_package(Python3 COMPONENTS Interpreter)
    find_program(GDB_EXE gdb)
    find_program(GDBSERVER_EXE gdbserver)
    if(Python3_FOUND)
        add_test(NAME fake_stub
                 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fake_stub.py
                         $<TARGET_FILE:cogdb>)
        add_test(NAME mcp
                 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.py
                         $<TARGET_FILE:cogdb>)
        if(GDB_EXE AND GDBSERVER_EXE)
            add_test(NAME e2e_gdbserver
                     COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e2e.py
                             $<TARGET_FILE:cogdb>)
        endif()
    endif()
endif()

install(TARGETS cogdb RUNTIME DESTINATION bin)
