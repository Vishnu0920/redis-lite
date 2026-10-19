#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace structures {
inline constexpr std::size_t NoHeapIndex = std::numeric_limits<std::size_t>::max();

// The owner stays at a stable address and exposes a heap_idx field. Keeping a
// typed reference also lets expiration recover the Entry without container_of.
template <typename Owner>
struct HeapNode {
    std::uint64_t expires_at_ms = 0;
    Owner* ref = nullptr; // Borrowed; the keyspace owns the Entry.
};

template <typename Owner>
class MinHeap {
public:
    MinHeap() = default;
    MinHeap(const MinHeap&) = delete;
    MinHeap& operator=(const MinHeap&) = delete;
    bool empty() const { return nodes_.empty(); }
    std::size_t size() const { return nodes_.size(); }
    const HeapNode<Owner>& top() const { assert(!empty()); return nodes_.front(); }
    const HeapNode<Owner>& at(std::size_t index) const { return nodes_.at(index); }

    // Updating an existing timer never appends a second node for the same owner.
    void createOrUpdate(Owner& owner, std::uint64_t expiresAtMS) {
        std::size_t index = owner.heap_idx;
        if (index == NoHeapIndex) {
            index = nodes_.size();
            nodes_.push_back({expiresAtMS, &owner});
            owner.heap_idx = index;
        } else {
            assert(index < size() && nodes_[index].ref == &owner);
            nodes_[index].expires_at_ms = expiresAtMS;
        }
        repair(index);
    }

    void erase(Owner& owner) {
        const std::size_t index = owner.heap_idx;
        if (index == NoHeapIndex) return;
        assert(index < size() && nodes_[index].ref == &owner);
        if (index != size() - 1) place(index, nodes_.back());
        nodes_.pop_back();
        owner.heap_idx = NoHeapIndex;
        if (index < size()) repair(index);
    }

private:
    std::vector<HeapNode<Owner>> nodes_;

    void place(std::size_t index, HeapNode<Owner> node) {
        nodes_[index] = node;
        node.ref->heap_idx = index; // Every move updates the owning Entry.
    }

    void repair(std::size_t index) {
        const auto node = nodes_[index];
        if (index && nodes_[(index - 1) / 2].expires_at_ms > node.expires_at_ms) {
            while (index) {
                const std::size_t parent = (index - 1) / 2;
                if (nodes_[parent].expires_at_ms <= node.expires_at_ms) break;
                place(index, nodes_[parent]);
                index = parent;
            }
        } else {
            while (index < size() / 2) {
                std::size_t child = index * 2 + 1;
                if (child + 1 < size() &&
                    nodes_[child + 1].expires_at_ms < nodes_[child].expires_at_ms) ++child;
                if (nodes_[child].expires_at_ms >= node.expires_at_ms) break;
                place(index, nodes_[child]);
                index = child;
            }
        }
        place(index, node);
    }
};
} // namespace structures
