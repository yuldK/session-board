cmake_minimum_required(VERSION 4.2 FATAL_ERROR)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(session_board VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
option(SESSION_BOARD_BUILD_TESTS "Build the session state and storage tests." ON)

set(LUIL_BUILD_TESTS OFF)
set(LUIL_BUILD_EXAMPLES OFF)
set(LUIL_BUILD_TOOLING OFF)
set(LUIL_INSTALL OFF)
set(LUIL_ENABLE_DIRECT3D OFF)
set(LUIL_ENABLE_WEBVIEW OFF)
add_subdirectory(third_party/luil SYSTEM)

add_library(session_board_options INTERFACE)
target_compile_definitions(session_board_options INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(session_board_options INTERFACE /W4 /utf-8 /permissive- /EHsc)
target_link_options(session_board_options INTERFACE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

add_library(session_board_core STATIC src/core/session.cpp src/core/session.h src/core/store.cpp src/core/store.h src/core/codex_monitor.cpp src/core/codex_monitor.h
    src/core/claude_monitor.cpp src/core/claude_monitor.h
    src/core/claude_desktop.cpp src/core/claude_desktop.h
    src/core/claude_hooks.cpp src/core/claude_hooks.h
    src/core/codex_hooks.cpp src/core/codex_hooks.h src/core/hook_installation.h
    src/core/hook_command.cpp src/core/hook_command.h
    src/core/host_window.cpp src/core/host_window.h)
target_include_directories(session_board_core PUBLIC src)
target_link_libraries(session_board_core PUBLIC session_board_options luil::nlohmann_json shell32 ole32)

set(LUIL_SOURCE_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/luil/src")
set(LUIL_ASSET_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/luil/assets/codicons")
set(LUIL_NLOHMANN_JSON_ROOT "${PROJECT_SOURCE_DIR}/third_party/luil/third_party/nlohmann_json")
luil_generate_third_party_notices("${CMAKE_BINARY_DIR}/generated/third_party_notices.txt")
luil_configure_resource_script("${CMAKE_BINARY_DIR}/generated/luil_resources.rc" "${CMAKE_BINARY_DIR}/generated/third_party_notices.txt")

add_executable(session_board WIN32 src/app/main.cpp src/app/resource.h src/app/board_app.cpp src/app/board_app.h src/app/widget_window.cpp src/app/widget_window.h
    src/app/single_instance.cpp src/app/single_instance.h
    resources/session_board.rc "${CMAKE_BINARY_DIR}/generated/luil_resources.rc")
set_target_properties(session_board PROPERTIES OUTPUT_NAME session-board)
target_include_directories(session_board PRIVATE "${PROJECT_SOURCE_DIR}/assets")
target_link_libraries(session_board PRIVATE session_board_core luil::luil comctl32 dwmapi)

configure_file(third_party/luil/third_party/nlohmann_json/LICENSE.MIT "${CMAKE_BINARY_DIR}/bin/json-LICENSE.txt" COPYONLY)

# 앱이 Hook 등록 때 자신과 옆의 라이선스를 함께 복사하므로 둘을 같은 폴더에 둔다.
install(TARGETS session_board RUNTIME DESTINATION .)
install(FILES third_party/luil/third_party/nlohmann_json/LICENSE.MIT DESTINATION . RENAME json-LICENSE.txt)

if(SESSION_BOARD_BUILD_TESTS)
    enable_testing()
    add_executable(session_board_tests tests/session_tests.cpp)
    target_link_libraries(session_board_tests PRIVATE session_board_core)
    add_test(NAME session_board_core COMMAND session_board_tests)
    add_executable(session_board_claude_tests tests/claude_tests.cpp)
    target_link_libraries(session_board_claude_tests PRIVATE session_board_core)
    add_test(NAME session_board_claude COMMAND session_board_claude_tests)
    add_executable(session_board_hook_install_tests tests/codex_hooks_tests.cpp)
    target_link_libraries(session_board_hook_install_tests PRIVATE session_board_core)
    add_test(NAME session_board_hook_installation COMMAND session_board_hook_install_tests)
    find_program(SESSION_BOARD_POWERSHELL NAMES pwsh)
    if(SESSION_BOARD_POWERSHELL)
        add_test(NAME session_board_integration
            COMMAND "${SESSION_BOARD_POWERSHELL}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tests/integration.ps1"
                -Executable "$<TARGET_FILE:session_board>"
                -WorkDirectory "${CMAKE_BINARY_DIR}/integration-test")
    endif()
endif()
