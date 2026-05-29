#pragma once

#include <cstdint>
#include <string>

namespace kuzu {
namespace algo_extension {

class DynamicConnectivityIndex {
public:
    using node_key_t = int64_t;

    virtual ~DynamicConnectivityIndex() = default;

    virtual void insertEdge(node_key_t u, node_key_t v) = 0;
    virtual void deleteEdge(node_key_t u, node_key_t v) = 0;
    virtual bool connected(node_key_t u, node_key_t v) const = 0;

    virtual bool containsNode(node_key_t key) const = 0;
    virtual uint64_t getNumNodes() const = 0;

    virtual std::string getName() const = 0;
};

} // namespace algo_extension
} // namespace kuzu