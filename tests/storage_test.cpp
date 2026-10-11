#include "storage/storage.h"
#include "storage/data_structures/hash_table.h"
#include "storage/data_structures/avl_tree.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <unordered_map>

struct HashNode : structures::HashLink<HashNode> {
    std::string key;
    int value = 0;
};

void checkHashTable() {
    structures::HashTable<HashNode> table;
    std::unordered_map<std::string, int> reference;
    bool sawMigration = false;
    HashNode* first = nullptr;
    for (int i = 0; i < 10000; ++i) {
        auto node = std::make_unique<HashNode>();
        node->key = std::to_string(i);
        node->value = i;
        if (i == 0) first = node.get();
        assert(table.insert(std::move(node)));
        reference[std::to_string(i)] = i;
        sawMigration |= table.rehashing();
        assert(table.find("0") == first); // Existing entries never move in memory.
        if (i % 127 == 0) {
            std::size_t seen = 0;
            table.forEach([&](const HashNode& item) {
                assert(reference.at(item.key) == item.value);
                ++seen;
            });
            assert(seen == reference.size());
        }
    }
    assert(sawMigration);
    std::mt19937 random(42);
    for (int i = 0; i < 20000; ++i) {
        std::string key = std::to_string(random() % 12000);
        switch (random() % 3) {
        case 0: {
            auto node = std::make_unique<HashNode>();
            node->key = key;
            node->value = i;
            bool inserted = reference.emplace(key, i).second;
            assert(table.insert(std::move(node)) == inserted);
            break;
        }
        case 1:
            assert(static_cast<bool>(table.erase(key)) == (reference.erase(key) != 0));
            break;
        default: {
            auto expected = reference.find(key);
            auto actual = table.find(key);
            assert((actual != nullptr) == (expected != reference.end()));
            if (actual) assert(actual->value == expected->second);
        }
        }
        assert(table.size() == reference.size());
    }
    for (const auto& [key, value] : reference) assert(table.find(key)->value == value);
    table.clear();
    assert(table.size() == 0 && !table.rehashing());
}

struct TreeNode : structures::AvlLink<TreeNode> { int value = 0; };
struct TreeLess {
    bool operator()(const TreeNode& a, const TreeNode& b) const { return a.value < b.value; }
};
using Tree = structures::AvlTree<TreeNode, TreeLess>;

std::pair<int, std::size_t> validateTree(const TreeNode* node, const TreeNode* parent,
                                      int minimum, int maximum) {
    if (!node) return {0, 0};
    assert(node->parent == parent);
    assert(minimum < node->value && node->value < maximum);
    auto left = validateTree(node->left, node, minimum, node->value);
    auto right = validateTree(node->right, node, node->value, maximum);
    assert(std::abs(left.first - right.first) <= 1);
    assert(node->height == 1 + std::max(left.first, right.first));
    assert(node->count == 1 + left.second + right.second);
    return {node->height, node->count};
}

void checkAvlTree() {
    Tree tree;
    std::vector<std::unique_ptr<TreeNode>> nodes(2048);
    std::set<int> reference;
    std::mt19937 random(123);
    for (unsigned step = 0; step < 8000; ++step) {
        const int value = random() % nodes.size();
        if (nodes[value]) {
            tree.erase(nodes[value].get());
            nodes[value].reset();
            reference.erase(value);
        } else {
            nodes[value] = std::make_unique<TreeNode>();
            nodes[value]->value = value;
            tree.insert(nodes[value].get());
            reference.insert(value);
        }
        validateTree(tree.root(), nullptr, -1, 2048);
        assert(tree.size() == reference.size());
        TreeNode probe;
        probe.value = random() % 2049;
        auto expected = reference.lower_bound(probe.value);
        auto actual = tree.lowerBound(probe);
        assert((actual != nullptr) == (expected != reference.end()));
        if (actual) assert(actual->value == *expected);
        if (step % 31 == 0) {
            std::size_t position = 0;
            for (int item : reference) {
                assert(tree.select(position)->value == item);
                assert(tree.rank(nodes[item].get()) == position);
                assert(tree.offset(nodes[item].get(), -static_cast<std::int64_t>(position)) == tree.select(0));
                ++position;
            }
            assert(!tree.select(position));
        }
    }
    if (tree.size()) {
        auto node = tree.select(tree.size() / 2);
        assert(!tree.offset(node, std::numeric_limits<std::int64_t>::min()));
        assert(!tree.offset(node, std::numeric_limits<std::int64_t>::max()));
    }
    // Delete every remaining node, including roots with two children.
    while (tree.root()) {
        auto node = tree.root();
        tree.erase(node);
        nodes[node->value].reset();
        validateTree(tree.root(), nullptr, -1, 2048);
    }
}

void checkSortedSet() {
    auto set = storage::makeCustomSortedSet();
    std::map<std::string, double> reference;
    std::mt19937 random(9);
    const double lowest = -std::numeric_limits<double>::infinity();
    for (unsigned step = 0; step < 5000; ++step) {
        std::string member = std::to_string(random() % 400);
        if (step % 7 == 0) member += std::string("\0\xff", 2);
        if (random() % 4 == 0) {
            assert(set->remove(member) == (reference.erase(member) != 0));
        } else {
            const double score = static_cast<int>(random() % 20) - 10;
            const bool added = reference.find(member) == reference.end();
            reference[member] = score;
            assert(set->add(member, score) == added);
        }
        assert(set->size() == reference.size());
        if (step % 23 == 0) {
            std::vector<std::pair<double, std::string>> ordered;
            for (const auto& [name, score] : reference) ordered.emplace_back(score, name);
            std::sort(ordered.begin(), ordered.end());
            const auto all = set->query(lowest, "", 0, reference.size() + 1);
            assert(all.size() == ordered.size());
            for (std::size_t i = 0; i < all.size(); ++i) {
                assert(all[i].member == ordered[i].second && all[i].score == ordered[i].first);
                assert(set->rank(all[i].member) == i);
                assert(set->score(all[i].member) == all[i].score);
            }
            const std::pair<double, std::string> lower{0, "200"};
            auto start = std::lower_bound(ordered.begin(), ordered.end(), lower);
            const std::int64_t offset = -3;
            auto queried = set->query(lower.first, lower.second, offset, 8);
            const auto position = start - ordered.begin() + offset;
            std::size_t count = 0;
            if (start != ordered.end() && position >= 0) {
                for (auto i = static_cast<std::size_t>(position); i < ordered.size() && count < 8; ++i, ++count) {
                    assert(queried.at(count).member == ordered[i].second);
                }
            }
            assert(queried.size() == count);
        }
    }
    assert(set->query(lowest, "", std::numeric_limits<std::int64_t>::min(), 10).empty());
    assert(set->query(lowest, "", std::numeric_limits<std::int64_t>::max(), 10).empty());
    assert(set->query(lowest, "", 0, 0).empty());
}

void checkStorage() {
    auto store = storage::makeCustomStorage();
    assert(store->get("missing").status == storage::Status::NotFound);
    const std::string key("k\0\r\n", 4), value("v\0\r\n", 4);
    store->set(key, value);
    assert(store->get(key).value == value);
    store->set(key, "updated");
    assert(store->get(key).value == "updated");
    assert(store->zadd(key, {{"member", 1}}).status == storage::Status::WrongType);
    assert(store->zadd("board", {{"bob", 5}, {"alice", 5}, {"bob", 10}}).value == 2);
    assert(store->get("board").status == storage::Status::WrongType);
    assert(store->zrank("board", "alice").value == 0);
    assert(store->zscore("board", "bob").value == 10);
    assert(store->zrem("board", {"alice", "bob", "bob"}).value == 2);
    assert(store->get("board").status == storage::Status::NotFound);
    store->zadd("board", {{"member", 1}});
    store->set("board", "replacement");
    assert(store->get("board").value == "replacement");
    assert(store->zcard("board").status == storage::Status::WrongType);
    assert(store->erase("board") && !store->erase("board"));
    assert(store->keys() == std::vector<std::string>{key});
}

int main() {
    checkHashTable();
    checkAvlTree();
    checkSortedSet();
    checkStorage();
    std::cout << "Storage tests passed (hash migration, AVL invariants, rank, sorted sets)\n";
}
