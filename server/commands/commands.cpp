#include "commands/commands.h"
#include "resp.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {
std::string wrongType() {
    return resp::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}
std::string wrongArity(const std::string& command) {
    return resp::error("ERR wrong number of arguments for " + command);
}
bool parseInteger(const std::string& text, std::int64_t& result) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parseScore(const std::string& text, double& result) {
    if (text.empty() || text.find_first_of(" \t\r\n\v\f") != std::string::npos) return false;
    char* end = nullptr;
    errno = 0;
    result = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && errno != ERANGE && !std::isnan(result);
}
std::string formatScore(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}
std::string limited(std::string reply) {
    if (reply.size() > resp::MaxFrame) return resp::error("ERR response exceeds 1 MiB");
    return reply;
}
} // namespace

CommandProcessor::CommandProcessor(std::unique_ptr<storage::Storage> store) : store_(std::move(store)) {
    if (!store_) throw std::invalid_argument("storage is required");
}

std::string CommandProcessor::execute(const std::vector<std::string>& args) {
    if (args.empty()) return resp::error("ERR empty command");
    std::string name = args[0];
    for (char& ch : name) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');

    if (name == "PING") {
        if (args.size() == 1) return resp::simple("PONG");
        if (args.size() == 2) return resp::bulk(args[1]);
        return wrongArity(name);
    }
    if (name == "ECHO") {
        if (args.size() == 2) return resp::bulk(args[1]);
        return wrongArity(name);
    }

    // Storage operations and the reply snapshot are atomic across client threads.
    // PING/ECHO don't access storage and avoid this mutex.
    std::lock_guard<std::mutex> lock(mutex_);
    if (name == "GET") {
        if (args.size() != 2) return wrongArity(name);
        auto result = store_->get(args[1]);
        if (result.status == storage::Status::WrongType) return wrongType();
        return result.status == storage::Status::NotFound ? resp::nullBulk() : resp::bulk(result.value);
    }
    if (name == "SET") {
        if (args.size() != 3) return wrongArity(name);
        store_->set(args[1], args[2]);
        return resp::simple("OK");
    }
    if (name == "DEL") {
        if (args.size() < 2) return wrongArity(name);
        std::int64_t removed = 0;
        for (std::size_t i = 1; i < args.size(); ++i) removed += store_->erase(args[i]);
        return resp::integer(removed);
    }
    if (name == "KEYS") {
        if (args.size() > 2) return wrongArity(name);
        if (args.size() == 2 && args[1] != "*") return resp::error("ERR KEYS supports only *");
        std::vector<std::string> elements;
        for (const auto& key : store_->keys()) elements.push_back(resp::bulk(key));
        return limited(resp::array(elements));
    }
    if (name == "ZADD") {
        if (args.size() < 4 || args.size() % 2 != 0) return wrongArity(name);
        std::vector<storage::ScoredMember> members;
        for (std::size_t i = 2; i < args.size(); i += 2) {
            double score;
            if (!parseScore(args[i], score)) return resp::error("ERR score is not a valid number");
            members.push_back({args[i + 1], score});
        }
        auto result = store_->zadd(args[1], members);
        return result.status == storage::Status::WrongType ? wrongType() : resp::integer(result.value);
    }
    if (name == "ZREM") {
        if (args.size() < 3) return wrongArity(name);
        auto result = store_->zrem(args[1], {args.begin() + 2, args.end()});
        return result.status == storage::Status::WrongType ? wrongType() : resp::integer(result.value);
    }
    if (name == "ZSCORE") {
        if (args.size() != 3) return wrongArity(name);
        auto result = store_->zscore(args[1], args[2]);
        if (result.status == storage::Status::WrongType) return wrongType();
        return result.status == storage::Status::NotFound ? resp::nullBulk() : resp::bulk(formatScore(result.value));
    }
    if (name == "ZRANK") {
        if (args.size() != 3) return wrongArity(name);
        auto result = store_->zrank(args[1], args[2]);
        if (result.status == storage::Status::WrongType) return wrongType();
        return result.status == storage::Status::NotFound ? resp::nullBulk() : resp::integer(result.value);
    }
    if (name == "ZCARD") {
        if (args.size() != 2) return wrongArity(name);
        auto result = store_->zcard(args[1]);
        return result.status == storage::Status::WrongType ? wrongType() : resp::integer(result.value);
    }
    if (name == "ZQUERY") {
        if (args.size() != 6) return wrongArity(name);
        double score;
        std::int64_t offset, limit;
        if (!parseScore(args[2], score)) return resp::error("ERR score is not a valid number");
        if (!parseInteger(args[4], offset) || !parseInteger(args[5], limit)) {
            return resp::error("ERR offset and limit must be signed 64-bit integers");
        }
        auto result = store_->zquery(args[1], score, args[3], offset, limit <= 0 ? 0 : limit);
        if (result.status == storage::Status::WrongType) return wrongType();
        std::vector<std::string> elements;
        for (const auto& member : result.value) {
            elements.push_back(resp::bulk(member.member));
            elements.push_back(resp::bulk(formatScore(member.score)));
        }
        return limited(resp::array(elements));
    }
    return resp::error("ERR unknown command");
}
