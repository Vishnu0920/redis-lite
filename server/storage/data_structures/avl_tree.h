#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace structures {
template <typename Node>
struct AvlLink {
    Node* left = nullptr;
    Node* right = nullptr;
    Node* parent = nullptr;
    int height = 1;
    std::size_t count = 1;
};

// Non-owning intrusive AVL tree with subtree sizes for rank/select queries.
// Node inherits AvlLink<Node>; Compare orders distinct nodes by their values.
template <typename Node, typename Compare>
class AvlTree {
public:
    Node* root() const { return root_; }
    std::size_t size() const { return count(root_); }

    void insert(Node* node) {
        assert(!node->parent && !node->left && !node->right);
        Node* parent = nullptr;
        Node** link = &root_;
        while (*link) {
            parent = *link;
            link = less_(*node, **link) ? &parent->left : &parent->right;
        }
        *link = node;
        node->parent = parent;
        rebalance(node);
    }

    void erase(Node* node) {
        Node* start = node->parent;
        if (!node->left || !node->right) {
            replace(node, node->left ? node->left : node->right);
        } else {
            Node* successor = minimum(node->right);
            if (successor->parent != node) {
                start = successor->parent;
                replace(successor, successor->right);
                successor->right = node->right;
                successor->right->parent = successor;
            } else {
                start = successor;
            }
            replace(node, successor);
            successor->left = node->left;
            successor->left->parent = successor;
        }
        node->left = node->right = node->parent = nullptr;
        node->height = 1;
        node->count = 1;
        rebalance(start);
    }

    Node* lowerBound(const Node& value) const {
        Node* result = nullptr;
        for (Node* node = root_; node;) {
            if (less_(*node, value)) node = node->right;
            else { result = node; node = node->left; }
        }
        return result;
    }

    std::size_t rank(const Node* node) const {
        std::size_t result = count(node->left);
        while (node->parent) {
            if (node == node->parent->right) result += count(node->parent->left) + 1;
            node = node->parent;
        }
        return result;
    }

    Node* select(std::size_t position) const {
        for (Node* node = root_; node;) {
            const auto leftCount = count(node->left);
            if (position < leftCount) node = node->left;
            else if (position == leftCount) return node;
            else { position -= leftCount + 1; node = node->right; }
        }
        return nullptr;
    }

    Node* offset(Node* node, std::int64_t distance) const {
        if (!node) return nullptr;
        const auto position = rank(node);
        if (distance < 0) {
            // Handle INT64_MIN without negating it directly.
            const auto magnitude = static_cast<std::uint64_t>(-(distance + 1)) + 1;
            return magnitude > position ? nullptr : select(position - magnitude);
        }
        const auto magnitude = static_cast<std::uint64_t>(distance);
        return magnitude >= size() - position ? nullptr : select(position + magnitude);
    }

    static Node* next(Node* node) {
        if (node->right) return minimum(node->right);
        while (node->parent && node == node->parent->right) node = node->parent;
        return node->parent;
    }

private:
    Node* root_ = nullptr;
    Compare less_;
    static int height(Node* node) { return node ? node->height : 0; }
    static std::size_t count(const Node* node) { return node ? node->count : 0; }
    static Node* minimum(Node* node) {
        while (node->left) node = node->left;
        return node;
    }
    static void update(Node* node) {
        node->height = 1 + std::max(height(node->left), height(node->right));
        node->count = 1 + count(node->left) + count(node->right);
    }
    void replace(Node* previous, Node* replacement) {
        if (!previous->parent) root_ = replacement;
        else if (previous == previous->parent->left) previous->parent->left = replacement;
        else previous->parent->right = replacement;
        if (replacement) replacement->parent = previous->parent;
    }
    Node* rotateLeft(Node* node) {
        Node* top = node->right;
        replace(node, top);
        node->right = top->left;
        if (node->right) node->right->parent = node;
        top->left = node;
        node->parent = top;
        update(node);
        update(top);
        return top;
    }
    Node* rotateRight(Node* node) {
        Node* top = node->left;
        replace(node, top);
        node->left = top->right;
        if (node->left) node->left->parent = node;
        top->right = node;
        node->parent = top;
        update(node);
        update(top);
        return top;
    }
    void rebalance(Node* node) {
        while (node) {
            update(node);
            const int balance = height(node->left) - height(node->right);
            if (balance > 1) {
                if (height(node->left->left) < height(node->left->right)) rotateLeft(node->left);
                node = rotateRight(node);
            } else if (balance < -1) {
                if (height(node->right->right) < height(node->right->left)) rotateRight(node->right);
                node = rotateLeft(node);
            }
            node = node->parent;
        }
    }
};
} // namespace structures
