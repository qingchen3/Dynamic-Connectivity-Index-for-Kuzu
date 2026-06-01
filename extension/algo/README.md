Current milestone:

- STree is implemented as a standalone data structure.
- DTree is implemented as a standalone data structure.
- Both are wrapped by STreeIndex and DTreeIndex.
- Both wrappers implement DynamicConnectivityIndex.
- dynamic_connectivity_index_test verifies insertion, deletion, connectivity, and replacement-edge behavior.
- All 6 interface tests pass.

Next milestone:
- ADD HKS
- Experimental evaluations for STree and DTree
- Add dynamic_connectivity_index_factory.h/.cpp.