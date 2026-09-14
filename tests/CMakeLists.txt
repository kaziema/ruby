function(ruby_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ruby::core ruby_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ruby_add_test(test_units)
ruby_add_test(test_identity)
ruby_add_test(test_animation)
ruby_add_test(test_document)
ruby_add_test(test_transform)

add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE ruby::script ruby_warnings)
add_test(NAME test_sandbox COMMAND test_sandbox)

add_executable(test_expressions test_expressions.cpp)
target_link_libraries(test_expressions PRIVATE ruby::script ruby::core ruby_warnings)
add_test(NAME test_expressions COMMAND test_expressions)

add_executable(test_aeimport test_aeimport.cpp)
target_link_libraries(test_aeimport PRIVATE ruby::script ruby::core ruby_warnings)
add_test(NAME test_aeimport COMMAND test_aeimport)
ruby_add_test(test_project)

add_executable(test_projectio test_projectio.cpp)
target_link_libraries(test_projectio PRIVATE ruby::io ruby_warnings)
add_test(NAME test_projectio COMMAND test_projectio)

add_executable(test_mediapool test_mediapool.cpp)
target_link_libraries(test_mediapool PRIVATE ruby::io ruby_warnings)
add_test(NAME test_mediapool COMMAND test_mediapool)

add_executable(test_peakcache test_peakcache.cpp)
target_link_libraries(test_peakcache PRIVATE ruby::media ruby_warnings)
add_test(NAME test_peakcache COMMAND test_peakcache)

# Needs an audio device; reports 77 and is recorded as SKIPPED without one.
add_executable(test_audiooutput test_audiooutput.cpp)
target_link_libraries(test_audiooutput PRIVATE ruby::audio ruby_warnings)
add_test(NAME test_audiooutput COMMAND test_audiooutput)
set_tests_properties(test_audiooutput PROPERTIES SKIP_RETURN_CODE 77)

# Needs Qt's font stack and a platform plugin; offscreen is enough.
add_executable(test_textraster test_textraster.cpp)
target_link_libraries(test_textraster PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_textraster COMMAND test_textraster -platform offscreen)
set_tests_properties(test_textraster PROPERTIES SKIP_RETURN_CODE 77)

# Needs a GPU adapter; reports 77 and is recorded as SKIPPED without one.
add_executable(test_compositor test_compositor.cpp)
target_link_libraries(test_compositor PRIVATE ruby::engine ruby::gpu ruby_warnings)
add_test(NAME test_compositor COMMAND test_compositor)
set_tests_properties(test_compositor PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE ruby::io ruby_warnings)
add_test(NAME test_history COMMAND test_history)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_transport COMMAND test_transport)

add_executable(test_effects test_effects.cpp)
target_link_libraries(test_effects PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_effects COMMAND test_effects)

# The GPU test brings up a real adapter, so it needs the Dawn runtime beside it.
add_executable(test_gpu test_gpu.cpp)
# engine brings gpu with it; naming both makes the linker warn about duplicates.
target_link_libraries(test_gpu PRIVATE ruby::engine ruby_warnings)
target_copy_webgpu_binaries(test_gpu)
add_test(NAME test_gpu COMMAND test_gpu)

# Decoding needs a real file. RUBY_TEST_VIDEO can be overridden; the test reports a
# skip rather than failing when it is unset.
set(RUBY_TEST_VIDEO "" CACHE STRING "Path to a video file for the media test")
add_executable(test_media test_media.cpp)
target_link_libraries(test_media PRIVATE ruby::media ruby_warnings)
add_test(NAME test_media COMMAND test_media)
if(RUBY_TEST_VIDEO)
    set_tests_properties(test_media PROPERTIES
        ENVIRONMENT "RUBY_TEST_VIDEO=${RUBY_TEST_VIDEO}")
endif()

# UI tests need Qt and run offscreen.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(test_menus test_menus.cpp)
target_link_libraries(test_menus PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_menus COMMAND test_menus)
set_tests_properties(test_menus PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_timelinerows test_timelinerows.cpp)
target_link_libraries(test_timelinerows PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_timelinerows COMMAND test_timelinerows)
set_tests_properties(test_timelinerows PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_viewerpick test_viewerpick.cpp)
target_link_libraries(test_viewerpick PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_viewerpick COMMAND test_viewerpick)

add_executable(test_framecache test_framecache.cpp)
target_link_libraries(test_framecache PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_framecache COMMAND test_framecache)

add_executable(test_rendergraph test_rendergraph.cpp)
target_link_libraries(test_rendergraph PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_rendergraph COMMAND test_rendergraph)

add_executable(test_migration test_migration.cpp)
target_link_libraries(test_migration PRIVATE ruby::engine ruby_warnings)
add_test(NAME test_migration COMMAND test_migration)

add_executable(test_schemas test_schemas.cpp)
target_link_libraries(test_schemas PRIVATE ruby::engine ruby::io nlohmann_json::nlohmann_json ruby_warnings)
add_test(NAME test_schemas COMMAND test_schemas)
set_tests_properties(test_schemas PROPERTIES
    ENVIRONMENT "RUBY_SCHEMA_DIR=${CMAKE_SOURCE_DIR}/schemas")

add_executable(gen_schemas gen_schemas.cpp)
target_link_libraries(gen_schemas PRIVATE ruby::engine ruby::io ruby_warnings)

add_executable(test_align test_align.cpp)
target_link_libraries(test_align PRIVATE ruby::core ruby_warnings)
add_test(NAME test_align COMMAND test_align)

add_executable(test_alignpanel test_alignpanel.cpp)
target_link_libraries(test_alignpanel PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_alignpanel COMMAND test_alignpanel)
set_tests_properties(test_alignpanel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_layerswitches test_layerswitches.cpp)
target_link_libraries(test_layerswitches PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_layerswitches COMMAND test_layerswitches)
set_tests_properties(test_layerswitches PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panelframe test_panelframe.cpp)
target_link_libraries(test_panelframe PRIVATE ruby::ui ruby_warnings)
add_test(NAME test_panelframe COMMAND test_panelframe)
set_tests_properties(test_panelframe PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
