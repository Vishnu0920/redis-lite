#pragma once

#include <cassert>

namespace structures {
// Links are embedded in the connection. They point to connections directly, so
// the list needs neither a separate allocation nor an unsafe offset cast.
template <typename Owner>
struct ListNode {
    Owner* prev = nullptr;
    Owner* next = nullptr;
};

template <typename Owner, ListNode<Owner> Owner::* Link>
class IntrusiveList {
public:
    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    bool empty() const { return front_ == nullptr; }
    Owner* front() const { return front_; }

    void pushBack(Owner& owner) {
        auto& node = owner.*Link;
        assert(!node.prev && !node.next && front_ != &owner);
        node.prev = back_;
        if (back_) (back_->*Link).next = &owner;
        else front_ = &owner;
        back_ = &owner;
    }

    void erase(Owner& owner) {
        auto& node = owner.*Link;
        assert(node.prev || node.next || front_ == &owner);
        if (node.prev) (node.prev->*Link).next = node.next;
        else front_ = node.next;
        if (node.next) (node.next->*Link).prev = node.prev;
        else back_ = node.prev;
        node = {};
    }

    void moveToBack(Owner& owner) {
        erase(owner);
        pushBack(owner);
    }

private:
    Owner* front_ = nullptr;
    Owner* back_ = nullptr;
};
} // namespace structures
