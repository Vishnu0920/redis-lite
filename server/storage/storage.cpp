#include "storage/storage.h"
#include "storage/data_structures/avl_tree.h"
#include "storage/data_structures/hash_table.h"
#include "storage/data_structures/min_heap.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
    std::size_t heap_idx = structures::NoHeapIndex; // No TTL by default.
};

class CustomStorage final : public Storage {
    structures::HashTable<Entry> entries_;
    structures::MinHeap<Entry> ttlHeap_; // Borrows entries; destroyed before them.
    SortedSetFactory sortedSets_;
    MillisecondClock clock_;

    bool expired(const Entry& entry, std::uint64_t now) const {
        return entry.heap_idx != structures::NoHeapIndex &&
               ttlHeap_.at(entry.heap_idx).expires_at_ms <= now;
    }

    void eraseEntry(Entry& entry) {
        ttlHeap_.erase(entry); // Detach borrowed pointers before freeing the entry.
        entries_.erase(entry.key);
    }

    Entry* findLive(std::string_view key, std::uint64_t now) {
        Entry* entry = entries_.find(key);
        if (entry && expired(*entry, now)) {
            eraseEntry(*entry);
            return nullptr;
        }
        return entry;
    }
    Entry* findLive(std::string_view key) { return findLive(key, clock_()); }

    void createOrUpdateTTLTimer(Entry& entry, std::uint64_t deadline) {
        ttlHeap_.createOrUpdate(entry, deadline);
    }
public:
    CustomStorage(SortedSetFactory sortedSets, MillisecondClock clock)
        : sortedSets_(sortedSets), clock_(clock) {
        if (!clock_) throw std::invalid_argument("clock is required");
    }
    Result<std::string> get(std::string_view key) override {
        Entry* entry = findLive(key);
        if (!entry) return {Status::NotFound, {}};
        if (entry->sortedSet) return {Status::WrongType, {}};
        return {Status::Ok, entry->value};
    }
    void set(std::string_view key, std::string_view value) override {
        // Allocate/copy before changing the existing value.
        std::string replacement(value);
        if (Entry* entry = findLive(key)) {
            ttlHeap_.erase(*entry); // SET replaces the value and clears its TTL.
            entry->value.swap(replacement);
            entry->sortedSet.reset(); // SET replaces either previous value type.
        } else {
            auto created = std::make_unique<Entry>();
            created->key = key;
            created->value = std::move(replacement);
            entries_.insert(std::move(created));
        }
    }
    bool erase(std::string_view key) override {
        Entry* entry = findLive(key);
        if (!entry) return false;
        eraseEntry(*entry);
        return true;
    }
    std::vector<std::string> keys() override {
        const auto now = clock_();
        std::vector<std::string> result;
        // Do not mutate hash chains during traversal. Expired keys are omitted;
        // timer processing (or their next lookup) releases their storage.
        entries_.forEach([&](const Entry& entry) {
            if (!expired(entry, now)) result.push_back(entry.key);
        });
        std::sort(result.begin(), result.end()); // Stable display, independent of buckets.
        return result;
    }
    bool expire(std::string_view key, std::int64_t seconds) override {
        const auto now = clock_();
        if (seconds > 0 && static_cast<std::uint64_t>(seconds) >
                (std::numeric_limits<std::uint64_t>::max() - now) / 1000) {
            throw std::overflow_error("expiration deadline is out of range");
        }
        Entry* entry = findLive(key, now);
        if (!entry) return false;
        if (seconds <= 0) eraseEntry(*entry);
        else createOrUpdateTTLTimer(*entry, now + static_cast<std::uint64_t>(seconds) * 1000);
        return true;
    }
    std::int64_t ttl(std::string_view key) override {
        const auto now = clock_();
        Entry* entry = findLive(key, now);
        if (!entry) return -2;
        if (entry->heap_idx == structures::NoHeapIndex) return -1;
        const auto remaining = ttlHeap_.at(entry->heap_idx).expires_at_ms - now;
        // Redis TTL rounds to the nearest second. Split the addition to avoid
        // overflowing when a deadline is near the uint64_t maximum.
        return static_cast<std::int64_t>(remaining / 1000 + (remaining % 1000 >= 500));
    }
    bool persist(std::string_view key) override {
        Entry* entry = findLive(key);
        if (!entry || entry->heap_idx == structures::NoHeapIndex) return false;
        ttlHeap_.erase(*entry);
        return true;
    }
    std::optional<std::uint64_t> nextTTLDeadlineMS() const override {
        if (ttlHeap_.empty()) return std::nullopt;
        return ttlHeap_.top().expires_at_ms;
    }
    std::size_t processTTLExpirations(std::uint64_t now, std::size_t limit) override {
        std::size_t removed = 0;
        while (!ttlHeap_.empty() && ttlHeap_.top().expires_at_ms <= now && removed < limit) {
            eraseEntry(*ttlHeap_.top().ref);
            ++removed;
        }
        return removed;
    }
    Result<std::size_t> zadd(std::string_view key, const std::vector<ScoredMember>& members) override {
        for (const auto& member : members) {
            if (std::isnan(member.score)) throw std::invalid_argument("NaN score");
        }
        Entry* entry = findLive(key);
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
        Entry* entry = findLive(key);
        if (!entry) return {Status::Ok, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        std::size_t removed = 0;
        for (const auto& member : members) removed += entry->sortedSet->remove(member);
        if (entry->sortedSet->size() == 0) eraseEntry(*entry);
        return {Status::Ok, removed};
    }
    Result<double> zscore(std::string_view key, std::string_view member) override {
        Entry* entry = findLive(key);
        if (!entry) return {Status::NotFound, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        auto score = entry->sortedSet->score(member);
        return score ? Result<double>{Status::Ok, *score} : Result<double>{Status::NotFound, 0};
    }
    Result<std::size_t> zrank(std::string_view key, std::string_view member) override {
        Entry* entry = findLive(key);
        if (!entry) return {Status::NotFound, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        auto rank = entry->sortedSet->rank(member);
        return rank ? Result<std::size_t>{Status::Ok, *rank} : Result<std::size_t>{Status::NotFound, 0};
    }
    Result<std::size_t> zcard(std::string_view key) override {
        Entry* entry = findLive(key);
        if (!entry) return {Status::Ok, 0};
        if (!entry->sortedSet) return {Status::WrongType, 0};
        return {Status::Ok, entry->sortedSet->size()};
    }
    Result<std::vector<ScoredMember>> zquery(std::string_view key, double score,
                                            std::string_view member, std::int64_t offset,
                                            std::size_t limit) override {
        Entry* entry = findLive(key);
        if (!entry) return {Status::Ok, {}};
        if (!entry->sortedSet) return {Status::WrongType, {}};
        return {Status::Ok, entry->sortedSet->query(score, member, offset, limit)};
    }
};
} // namespace
std::unique_ptr<SortedSet> makeCustomSortedSet() { return std::make_unique<CustomSortedSet>(); }
std::unique_ptr<Storage> makeCustomStorage(SortedSetFactory sortedSets, MillisecondClock clock) {
    return std::make_unique<CustomStorage>(sortedSets, clock);
}
} // namespace storage
