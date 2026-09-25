add_library(algo_extension_index
        OBJECT
        native_dynamic_connectivity_index.cpp
        )

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:algo_extension_index>
        PARENT_SCOPE)