#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace structures {

inline std::uint64_t hashBytes(std::string_view key) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : key) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// A node owns its key and embeds these links. Rehashing never relocates it.
template <typename Node>
struct HashLink {
    Node* hashNext = nullptr;
    std::uint64_t hashCode = 0;
};

// Owns nodes; separate chaining, power-of-two buckets, incremental rehashing.
// Node must inherit HashLink<Node> and expose a std::string key.
template <typename Node>
class HashTable {
public:
    HashTable() = default;
    ~HashTable() { clear(); }
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    std::size_t size() const { return current_.size + old_.size; }
    bool rehashing() const { return !old_.buckets.empty(); }

    Node* find(std::string_view key) {
        migrate();
        const auto hash = hashBytes(key);
        if (Node* node = lookup(current_, key, hash)) return node;
        return lookup(old_, key, hash);
    }

    // A duplicate leaves the existing node unchanged and destroys the new node.
    bool insert(std::unique_ptr<Node> node) {
        if (find(node->key)) return false;
        node->hashCode = hashBytes(node->key);
        if (current_.buckets.empty()) current_.buckets.resize(4, nullptr);
        if (!rehashing() && current_.size >= current_.buckets.size() * 2) {
            Table larger;
            larger.buckets.resize(current_.buckets.size() * 2, nullptr);
            old_ = std::move(current_);
            current_ = std::move(larger);
            migrationBucket_ = 0;
        }
        attach(current_, node.release());
        migrate();
        return true;
    }

    std::unique_ptr<Node> erase(std::string_view key) {
        migrate();
        const auto hash = hashBytes(key);
        Node* node = detach(current_, key, hash);
        if (!node) node = detach(old_, key, hash);
        if (node) node->hashNext = nullptr;
        return std::unique_ptr<Node>(node);
    }

    // Visits each live node once, including during migration. Do not mutate here.
    template <typename Visitor>
    void forEach(Visitor visit) const {
        for (const Table* table : {&current_, &old_}) {
            for (const Node* head : table->buckets) {
                for (const Node* node = head; node; node = node->hashNext) visit(*node);
            }
        }
    }

    void clear() {
        for (Table* table : {&current_, &old_}) {
            for (Node* head : table->buckets) {
                while (head) {
                    Node* next = head->hashNext;
                    delete head;
                    head = next;
                }
            }
            *table = Table{};
        }
        migrationBucket_ = 0;
    }

private:
    struct Table {
        std::vector<Node*> buckets;
        std::size_t size = 0;
    };
    Table current_;
    Table old_;
    std::size_t migrationBucket_ = 0;

    static void attach(Table& table, Node* node) {
        const auto slot = node->hashCode & (table.buckets.size() - 1);
        node->hashNext = table.buckets[slot];
        table.buckets[slot] = node;
        ++table.size;
    }

    static Node* lookup(const Table& table, std::string_view key, std::uint64_t hash) {
        if (table.buckets.empty()) return nullptr;
        for (Node* node = table.buckets[hash & (table.buckets.size() - 1)];
             node; node = node->hashNext) {
            if (node->hashCode == hash && node->key == key) return node;
        }
        return nullptr;
    }

    static Node* detach(Table& table, std::string_view key, std::uint64_t hash) {
        if (table.buckets.empty()) return nullptr;
        Node** link = &table.buckets[hash & (table.buckets.size() - 1)];
        while (*link) {
            Node* node = *link;
            if (node->hashCode == hash && node->key == key) {
                *link = node->hashNext;
                --table.size;
                return node;
            }
            link = &node->hashNext;
        }
        return nullptr;
    }

    void migrate() {
        // Count both empty buckets scanned and nodes moved, bounding migration
        // work even when the old table is sparse. Allocation is still synchronous.
        for (unsigned work = 0; rehashing() && old_.size && work < 64; ++work) {
            assert(migrationBucket_ < old_.buckets.size());
            Node*& head = old_.buckets[migrationBucket_];
            if (!head) {
                ++migrationBucket_;
                continue;
            }
            Node* node = head;
            head = node->hashNext;
            --old_.size;
            attach(current_, node);
        }
        if (rehashing() && old_.size == 0) {
            old_ = Table{};
            migrationBucket_ = 0;
        }
    }
};
} // namespace structures
