cmake_minimum_required(VERSION 3.12)
project(qsink LANGUAGES CXX)

find_package(Boost QUIET COMPONENTS graph)

add_library(lsqecclib
    src/resource_profile.cpp
    src/dag/directed_graph.cpp
    src/dag/domain_dags.cpp
    src/gates/parse_gates.cpp
    src/gates/gates.cpp
    src/gates/gate_approximator.cpp
    src/gates/decompose_rotation_stream.cpp
    src/patches/dense_slice.cpp
    src/patches/dense_patch_computation.cpp
    src/patches/patches.cpp
    src/patches/sparse_slice.cpp
    src/patches/slices_to_json.cpp
    src/patches/slice_stats.cpp
    src/pipelines/slicer.cpp
    src/rtls/evaluation_metrics.cpp
    src/rtls/json_helpers.cpp
    src/rtls/operation_schedule.cpp
    src/rtls/reordered_qasm.cpp
    src/rtls/rtls.cpp
    src/rtls/streaming_renderer.cpp
    src/layout/dynamic_layouts/compact_layout.cpp
    src/layout/dynamic_layouts/edpc_layout.cpp
    src/layout/dynamic_layouts/determine_exposed_operators.cpp
    src/layout/graph_search/boost_based_graph_search.cpp
    src/layout/graph_search/custom_graph_search.cpp
    src/layout/router.cpp
    src/layout/ascii_layout_spec.cpp
    src/layout/layout.cpp
    src/ls_instructions/boundary_rotation_injection_stream.cpp
    src/ls_instructions/ls_instructions_from_gates.cpp
    src/ls_instructions/ls_instructions_parse.cpp
    src/ls_instructions/ls_instruction_stream.cpp
    src/ls_instructions/ls_instructions.cpp
    src/ls_instructions/qsink_instruction_stream.cpp
    src/ls_instructions/parse_utils.cpp
    src/ls_instructions/teleported_s_gate_injection_stream.cpp
    src/ls_instructions/catalytic_s_gate_injection_stream.cpp
    src/ls_instructions/local_instructions.cpp
    src/scheduler/wave_scheduler.cpp
)

target_compile_features(lsqecclib PUBLIC cxx_std_20)
target_include_directories(lsqecclib PUBLIC include)
target_include_directories(lsqecclib SYSTEM PUBLIC
    external/include
    external/json/include
    external/ordered-map/include
)
set_target_properties(lsqecclib PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(Boost_FOUND)
    target_compile_definitions(lsqecclib PRIVATE ENABLE_BOOST_GRAPH_SEARCH)
    target_link_libraries(lsqecclib PRIVATE Boost::graph)
endif()

add_executable(qsink src/lsqecc_slicer_main.cpp)
target_link_libraries(qsink PRIVATE lsqecclib)
