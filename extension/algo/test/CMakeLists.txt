if (${BUILD_EXTENSION_TESTS})
    add_kuzu_test(algo_prepare_test prepare_test.cpp)
    add_kuzu_test(stree_test stree_test.cpp ../src/common/stree.cpp)
    target_include_directories(stree_test PRIVATE ../src/include)

    add_kuzu_test(dynamic_connectivity_index_test
        dynamic_connectivity_index_test.cpp
        ../src/common/stree.cpp
        ../src/common/stree_csr.cpp
        ../src/common/dtree.cpp
        ../src/common/dtree_csr.cpp
        ../src/common/dynamic_connectivity_index_factory.cpp
        ../src/index/native_dynamic_connectivity_index.cpp)
    target_include_directories(dynamic_connectivity_index_test PRIVATE ../src/include)

    add_kuzu_test(dynamic_connectivity_random_test
        dynamic_connectivity_random_test.cpp
        ../src/common/stree.cpp
        ../src/common/dtree.cpp
        ../src/common/stree_csr.cpp
        ../src/common/dtree_csr.cpp
        ../src/common/dynamic_connectivity_index_factory.cpp)
    target_include_directories(dynamic_connectivity_random_test PRIVATE ../src/include)

    # Not a test: replays an Imp_bench_dyn workload and reports timings, so it is
    # a plain executable rather than an add_kuzu_test target.
    add_executable(dynamic_connectivity_workload_bench
        dynamic_connectivity_workload_bench.cpp
        ../src/common/stree.cpp
        ../src/common/dtree.cpp
        ../src/common/stree_csr.cpp
        ../src/common/dtree_csr.cpp
        ../src/common/dynamic_connectivity_index_factory.cpp)
    target_include_directories(dynamic_connectivity_workload_bench PRIVATE ../src/include)
    target_link_libraries(dynamic_connectivity_workload_bench PRIVATE kuzu)
endif ()
