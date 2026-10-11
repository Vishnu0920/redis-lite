#include "storage/storage.h"
#include "storage/data_structures/avl_tree.h"
#include "storage/data_structures/hash_table.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace storage {
namespace {
struct Member : structures::HashLink<Member>, structures::AvlLink<Member> {
    std::string key; // Member name: shared by the hash and tree indexes.
    double score = 0;
};
struct MemberLess {
    bool operator()(const Member& left, const Member& right) const {
        if (left.score != right.score) return left.score < right.score;
        return left.key < right.key;
    }
};

class CustomSortedSet final : public SortedSet {
    structures::HashTable<Member> byName_; // Owns each member once.
    structures::AvlTree<Member, MemberLess> byScore_; // Borrows those same nodes.
public:
    bool add(std::string_view name, double score) override {
        if (std::isnan(score)) throw std::invalid_argument("NaN score");
        if (Member* member = byName_.find(name)) {
            if (member->score != score) {
                byScore_.erase(member);
                member->score = score;
                byScore_.insert(member);
            }
            return false;
        }
        auto member = std::make_unique<Member>();
        member->key = name;
        member->score = score;
        Member* pointer = member.get();
        byName_.insert(std::move(member));
        byScore_.insert(pointer);
        return true;
    }
    bool remove(std::string_view name) override {
        Member* member = byName_.find(name);
        if (!member) return false;
        byScore_.erase(member);
        byName_.erase(name); // Detach from the owning index and destroy it.
        return true;
    }
    std::optional<double> score(std::string_view name) override {
        if (Member* member = byName_.find(name)) return member->score;
        return std::nullopt;
    }
    std::optional<std::size_t> rank(std::string_view name) override {
        if (Member* member = byName_.find(name)) return byScore_.rank(member);
        return std::nullopt;
    }
    std::size_t size() const override { return byName_.size(); }
    std::vector<ScoredMember> query(double score, std::string_view name,
                                    std::int64_t offset, std::size_t limit) override {
        if (std::isnan(score)) throw std::invalid_argument("NaN score");
        Member lower;
        lower.key = name;
        lower.score = score;
        Member* member = byScore_.offset(byScore_.lowerBound(lower), offset);
        std::vector<ScoredMember> result;
        while (member && result.size() < limit) {
            result.push_back({member->key, member->score});
            member = decltype(byScore_)::next(member);
        }
        return result;
    }
};

struct Entry : structures::HashLink<Entry> {
    std::string key;
    std::string value;
    std::unique_ptr<SortedSet> sortedSet; // Non-null identifies a sorted-set key.
};

class CustomStorage final : public Storage {
    structures::HashTable<Entry> entries_;
    SortedSetFactory sortedSets_;
public:
    explicit CustomStorage(SortedSetFactory sortedSets) : sortedSets_(sortedSets) {}
    Result<std::string> get(std::string_view key) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::NotFound, {}};
        if (entry->sortedSet) return {Status::WrongType, {}};
        return {Status::Ok, entry->value};
    }
    void set(std::string_view key, std::string_view value) override {
        // Allocate/copy before changing the existing value.
        std::string replacement(value);
        if (Entry* entry = entries_.find(key)) {
            entry->value.swap(replacement);
            entry->sortedSet.reset(); // SET replaces either previous value type.
        } else {
            auto created = std::make_unique<Entry>();
            created->key = key;
            created->value = std::move(replacement);
            entries_.insert(std::move(created));
        }
    }
    bool erase(std::string_view key) override { return static_cast<bool>(entries_.erase(key)); }
    std::vector<std::string> keys() override {
        std::vector<std::string> result;
        entries_.forEach([&](const Entry& entry) { result.push_back(entry.key); });
        std::sort(result.begin(), result.end()); // Stable display, independent of buckets.
        return result;
    }
    Result<std::size_t> zadd(std::string_view key, const std::vector<ScoredMember>& members) override {
        for (const auto& member : members) {
            if (std::isnan(member.score)) throw std::invalid_argument("NaN score");
        }
        Entry* entry = entries_.find(key);
        if (entry && !entry->sortedSet) return {Status::WrongType, 0};
        std::unique_ptr<Entry> created;
        if (!entry) {
            created = std::make_unique<Entry>();
            created->key = key;
            created->sortedSet = sortedSets_();
            entry = created.get();
        }
        std::size_t added = 0;
        for (const auto& member : members) added += entry->sortedSet->add(member.member, member.score);
        if (created) entries_.insert(std::move(created));
        return {Status::Ok, added};
    }
    Result<std::size_t> zrem(std::string_view key, const std::vector<std::string>& members) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::Ok, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        std::size_t removed = 0;
        for (const auto& member : members) removed += entry->sortedSet->remove(member);
        if (entry->sortedSet->size() == 0) entries_.erase(key);
        return {Status::Ok, removed};
    }
    Result<double> zscore(std::string_view key, std::string_view member) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::NotFound, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        auto score = entry->sortedSet->score(member);
        return score ? Result<double>{Status::Ok, *score} : Result<double>{Status::NotFound, 0};
    }
    Result<std::size_t> zrank(std::string_view key, std::string_view member) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::NotFound, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        auto rank = entry->sortedSet->rank(member);
        return rank ? Result<std::size_t>{Status::Ok, *rank} : Result<std::size_t>{Status::NotFound, 0};
    }
    Result<std::size_t> zcard(std::string_view key) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::Ok, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        return {Status::Ok, entry->sortedSet->size()};
    }
    Result<std::vector<ScoredMember>> zquery(std::string_view key, double score,
                                            std::string_view member, std::int64_t offset,
                                            std::size_t limit) override {
        Entry* entry = entries_.find(key);
        if (!entry) return {Status::Ok, {}};
        if (!entry->sortedSet) return {Status::WrongType, {}};
        return {Status::Ok, entry->sortedSet->query(score, member, offset, limit)};
    }
};
} // namespace
std::unique_ptr<SortedSet> makeCustomSortedSet() { return std::make_unique<CustomSortedSet>(); }
std::unique_ptr<Storage> makeCustomStorage(SortedSetFactory sortedSets) {
    return std::make_unique<CustomStorage>(sortedSets);
}
} // namespace storage
