# BlockerPasses — Plugify port of the Metamod plugin BlockerPasses (ABKAM): per-map blocker
# items/walls with an admin menu, gated on the player count, stored in bp_data.ini.
#
#   plugify-BlockerPasses  the plugin shared library (entry "bin/BlockerPasses")
#   bp_core                the engine-independent settings/data/geometry code (+ its tests)

set(BLOCKERPASSES_VERSION "2.0.2")

# --- engine-independent core --------------------------------------------------
add_library(bp_core STATIC src/bp_core.cpp)
target_include_directories(bp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat (for pisex::KeyValues) is linked by the users of bp_core; the tests link it too.
target_include_directories(bp_core SYSTEM PUBLIC
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_compile_options(bp_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(bp_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-BlockerPasses SHARED
        src/plugin.cpp
        # __s2sdk_* / __admin_system_* binding slots, filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:admin_system_bindings_obj>
)
target_include_directories(plugify-BlockerPasses PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated pisexcompat/plg/s2sdk/admin_system headers must not fail -Werror.
target_include_directories(plugify-BlockerPasses SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
)
target_link_libraries(plugify-BlockerPasses PRIVATE
        bp_core
        pisexcompat
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-BlockerPasses PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-BlockerPasses PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="BlockerPasses"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-BlockerPasses PROPERTIES OUTPUT_NAME BlockerPasses)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-BlockerPasses PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/BlockerPasses.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/BlockerPasses.pplugin"
)

# --- tests --------------------------------------------------------------------
add_executable(bp_core_tests
        tests/bp_core_tests.cpp
        src/bp_core.cpp
)
target_include_directories(bp_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(bp_core_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(bp_core_tests PRIVATE pisexcompat -static-libstdc++ -static-libgcc)
target_compile_options(bp_core_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(bp_core_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        BP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(bp_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
