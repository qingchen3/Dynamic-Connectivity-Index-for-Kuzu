if (${BUILD_EXTENSION_TESTS})
    add_kuzu_test(algo_prepare_test prepare_test.cpp)
    add_kuzu_test(stree_test stree_test.cpp ../src/common/stree.cpp)
    target_include_directories(stree_test PRIVATE ../src/include)

    add_kuzu_test(dynamic_connectivity_index_test
        dynamic_connectivity_index_test.cpp
        ../src/common/stree.cpp
        ../src/common/dtree.cpp
        ../src/common/dynamic_connectivity_index_factory.cpp)
    target_include_directories(dynamic_connectivity_index_test PRIVATE ../src/include)

    add_executable(dynamic_connectivity_benchmark
        dynamic_connectivity_benchmark.cpp
        ../src/common/stree.cpp
        ../src/common/dtree.cpp
        ../src/common/dynamic_connectivity_index_factory.cpp)
    target_include_directories(dynamic_connectivity_benchmark PRIVATE ../src/include)
endif ()
