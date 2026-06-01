# Dynamic Connectivity Index Prototype

Current milestone:

- `STree` is implemented as a standalone dynamic connectivity data structure.
- `DTree` is implemented as a standalone dynamic connectivity data structure.
- `STreeIndex` adapts `STree` to the `DynamicConnectivityIndex` interface.
- `DTreeIndex` adapts `DTree` to the `DynamicConnectivityIndex` interface.
- `dynamic_connectivity_index_test` verifies basic insertion, deletion, connectivity, and replacement-edge behavior for both STree and DTree.
- The current interface-level test suite contains 6 tests.

Next milestone:

- Add `dynamic_connectivity_index_factory.h/.cpp`.
- Support `createDynamicConnectivityIndex("stree")` and `createDynamicConnectivityIndex("dtree")`.
- Add factory tests for STree, DTree, case-insensitive method names, and unknown-method exceptions.
- Add HKS for evaluation