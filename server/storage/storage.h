#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace storage {
struct ScoredMember {
    std::string member;
    double score = 0;
};

// A sorted set's behavior; callers never receive internal nodes or iterators.
class SortedSet {
public:
    virtual ~SortedSet() = default;
    virtual bool add(std::string_view member, double score) = 0;
    virtual bool remove(std::string_view member) = 0;
    virtual std::optional<double> score(std::string_view member) = 0;
    virtual std::optional<std::size_t> rank(std::string_view member) = 0;
    virtual std::size_t size() const = 0;
    virtual std::vector<ScoredMember> query(double minimumScore, std::string_view minimumMember,
                                           std::int64_t offset, std::size_t limit) = 0;
};

enum class Status { Ok, NotFound, WrongType };
template <typename T>
struct Result {
    Status status = Status::Ok;
    T value{};
};

// One keyspace: each key holds a string or a sorted set. Results own their data.
// Implementations need not be thread-safe: CommandProcessor locks whole commands.
class Storage {
public:
    virtual ~Storage() = default;
    virtual Result<std::string> get(std::string_view key) = 0;
    virtual void set(std::string_view key, std::string_view value) = 0;
    virtual bool erase(std::string_view key) = 0;
    virtual std::vector<std::string> keys() = 0;
    virtual Result<std::size_t> zadd(std::string_view key, const std::vector<ScoredMember>& members) = 0;
    virtual Result<std::size_t> zrem(std::string_view key, const std::vector<std::string>& members) = 0;
    virtual Result<double> zscore(std::string_view key, std::string_view member) = 0;
    virtual Result<std::size_t> zrank(std::string_view key, std::string_view member) = 0;
    virtual Result<std::size_t> zcard(std::string_view key) = 0;
    virtual Result<std::vector<ScoredMember>> zquery(std::string_view key, double minimumScore,
                                                    std::string_view minimumMember,
                                                    std::int64_t offset, std::size_t limit) = 0;
};

std::unique_ptr<SortedSet> makeCustomSortedSet();
using SortedSetFactory = std::unique_ptr<SortedSet> (*)();
std::unique_ptr<Storage> makeCustomStorage(SortedSetFactory sortedSets = makeCustomSortedSet);
} // namespace storage
