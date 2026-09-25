add_subdirectory(config)

add_library(kuzu_algo_function
        OBJECT
        component_ids.cpp
        strongly_connected_components.cpp
        strongly_connected_components_kosaraju.cpp
        weakly_connected_components.cpp
        create_dynamic_connectivity_index.cpp
        dynamic_connectivity_query.cpp
        dynamic_connectivity_insert_edge.cpp
        dynamic_connectivity_delete_edge.cpp
        page_rank.cpp
        k_core_decomposition.cpp
        louvain.cpp
        spanning_forest.cpp
        )

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:kuzu_algo_function>
        PARENT_SCOPE)
