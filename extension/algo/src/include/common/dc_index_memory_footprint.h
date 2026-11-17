#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

namespace kuzu::algo_extension {

struct IndexMemoryFootprint {
    size_t bytesNodes = 0;
    size_t bytesEdges = 0;
    size_t numNodes = 0;
    size_t numTreeEdges = 0;
    size_t numNonTreeEdges = 0;

    size_t total() const { return bytesNodes + bytesEdges; }
};

namespace memory_footprint_detail {

struct AllocationCounter {
    static inline size_t requested = 0;
};

template<typename T>
struct Allocator {
    using value_type = T;

    Allocator() = default;

    template<typename U>
    Allocator(const Allocator<U>&) {}

    T* allocate(size_t count) {
        AllocationCounter::requested += count * sizeof(T);
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* ptr, size_t count) {
        std::allocator<T>{}.deallocate(ptr, count);
    }
};

template<typename T>
size_t setNodeBytes() {
    std::set<T, std::less<T>, Allocator<T>> measurementSet;
    AllocationCounter::requested = 0;
    measurementSet.insert(T{});
    return AllocationCounter::requested;
}

template<typename K, typename V>
size_t mapNodeBytes() {
    using Pair = std::pair<const K, V>;
    std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, Allocator<Pair>>
        measurementMap;
    measurementMap.rehash(1);
    AllocationCounter::requested = 0;
    measurementMap.emplace(K{}, V{});
    return AllocationCounter::requested;
}

template<typename K, typename V>
size_t mapBucketBytes(size_t bucketCount, size_t elementCount) {
    if (elementCount == 0) {
        return 0;
    }

    using Pair = std::pair<const K, V>;
    std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, Allocator<Pair>>
        measurementMap;
    AllocationCounter::requested = 0;
    measurementMap.rehash(32);
    return bucketCount *
           (AllocationCounter::requested / measurementMap.bucket_count());
}

} // namespace memory_footprint_detail
} // namespace kuzu::algo_extension