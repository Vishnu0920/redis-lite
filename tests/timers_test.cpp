#include "storage/storage.h"
#include "storage/data_structures/min_heap.h"
#include "timers/idle_list.h"
#include "timers/time.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>

namespace {
struct HeapOwner { std::size_t heap_idx = structures::NoHeapIndex; };

void checkHeap() {
    std::array<HeapOwner, 128> owners{};
    std::array<std::optional<std::uint64_t>, 128> reference{};
    structures::MinHeap<HeapOwner> heap;
    std::mt19937 random(23);
    for (int step = 0; step < 10000; ++step) {
        const auto index = random() % owners.size();
        if (random() % 3 == 0) {
            heap.erase(owners[index]);
            reference[index].reset();
        } else {
            const auto deadline = random() % 20000;
            heap.createOrUpdate(owners[index], deadline);
            reference[index] = deadline;
        }
        std::size_t count = 0;
        std::optional<std::uint64_t> minimum;
        for (std::size_t i = 0; i < owners.size(); ++i) {
            if (!reference[i]) {
                assert(owners[i].heap_idx == structures::NoHeapIndex);
                continue;
            }
            ++count;
            if (!minimum || *reference[i] < *minimum) minimum = reference[i];
            const auto& node = heap.at(owners[i].heap_idx);
            assert(node.ref == &owners[i] && node.expires_at_ms == *reference[i]);
        }
        assert(heap.size() == count);
        assert(heap.empty() == !minimum.has_value());
        if (minimum) assert(heap.top().expires_at_ms == *minimum);
    }
    std::uint64_t previous = 0;
    while (!heap.empty()) {
        assert(heap.top().expires_at_ms >= previous);
        previous = heap.top().expires_at_ms;
        heap.erase(*heap.top().ref);
    }
    for (auto& owner : owners) assert(owner.heap_idx == structures::NoHeapIndex);
}

struct ListOwner { structures::ListNode<ListOwner> idle_node; };
void checkIdleList() {
    std::array<ListOwner, 64> owners{};
    structures::IntrusiveList<ListOwner, &ListOwner::idle_node> list;
    std::vector<ListOwner*> reference;
    std::mt19937 random(17);
    for (int step = 0; step < 5000; ++step) {
        auto& owner = owners[random() % owners.size()];
        const auto found = std::find(reference.begin(), reference.end(), &owner);
        if (found == reference.end()) {
            list.pushBack(owner);
            reference.push_back(&owner);
        } else if (random() % 2) {
            list.moveToBack(owner);
            reference.erase(found);
            reference.push_back(&owner);
        } else {
            list.erase(owner);
            reference.erase(found);
            assert(!owner.idle_node.next && !owner.idle_node.prev);
        }
        auto* actual = list.front();
        ListOwner* previous = nullptr;
        for (auto* expected : reference) {
            assert(actual == expected && actual->idle_node.prev == previous);
            previous = actual;
            actual = actual->idle_node.next;
        }
        assert(!actual && list.empty() == reference.empty());
    }
    while (auto* owner = list.front()) list.erase(*owner);
    assert(list.empty());
}

std::uint64_t now = 10000;
std::uint64_t fakeClock() { return now; }
auto makeStore() { return storage::makeCustomStorage(storage::makeCustomSortedSet, fakeClock); }

void checkTTL() {
    auto store = makeStore();
    assert(store->ttl("missing") == -2);
    assert(!store->expire("missing", 1) && !store->persist("missing"));
    store->set("key", "value");
    assert(store->ttl("key") == -1 && !store->persist("key"));
    assert(store->expire("key", 5) && store->nextTTLDeadlineMS() == now + 5000);
    assert(store->expire("key", 20) && store->nextTTLDeadlineMS() == now + 20000);
    assert(store->expire("key", 5) && store->nextTTLDeadlineMS() == now + 5000);
    now += 500;
    assert(store->ttl("key") == 5); // 4.5 seconds rounds to 5.
    ++now;
    assert(store->ttl("key") == 4);
    now += 4000;
    assert(store->ttl("key") == 0 && store->get("key").value == "value");
    now += 499; // Exact deadline: lookup must hide it before any timer sweep.
    assert(store->get("key").status == storage::Status::NotFound);
    assert(store->ttl("key") == -2 && !store->nextTTLDeadlineMS());

    store->set("key", "value");
    store->expire("key", 1);
    assert(store->persist("key") && !store->persist("key"));
    now += 1000;
    assert(store->processTTLExpirations(now) == 0 && store->get("key").value == "value");
    store->expire("key", 1);
    store->set("key", "replacement");
    assert(store->ttl("key") == -1 && !store->nextTTLDeadlineMS());
    assert(store->expire("key", 0) && store->ttl("key") == -2);
    store->set("key", "again");
    assert(store->expire("key", std::numeric_limits<std::int64_t>::min()));
    assert(!store->nextTTLDeadlineMS());

    store->zadd("board", {{"alice", 1}, {"bob", 2}});
    store->expire("board", 1);
    const auto deadline = store->nextTTLDeadlineMS();
    store->zadd("board", {{"alice", 3}, {"carol", 4}});
    store->zrem("board", {"carol"});
    assert(store->nextTTLDeadlineMS() == deadline);
    store->zrem("board", {"alice", "bob"});
    assert(!store->nextTTLDeadlineMS()); // Last-member deletion unlinks TTL too.
    store->zadd("board", {{"alice", 1}});
    store->expire("board", 1);
    store->set("board", "string");
    assert(store->ttl("board") == -1 && !store->nextTTLDeadlineMS());
    store->expire("board", 1);
    assert(store->erase("board") && !store->nextTTLDeadlineMS());
}

void checkExpiredAccesses() {
    auto store = makeStore();
    for (const auto* key : {"get", "set", "del", "keys", "expire", "persist", "ttl",
                            "zadd", "zrem", "zscore", "zrank", "zcard", "zquery"}) {
        store->set(key, "old string");
        store->expire(key, 1);
    }
    now += 1000;
    assert(store->keys().empty());
    assert(store->get("get").status == storage::Status::NotFound);
    store->set("set", "fresh");
    assert(store->get("set").value == "fresh" && store->ttl("set") == -1);
    assert(!store->erase("del") && !store->expire("expire", 10));
    assert(!store->persist("persist") && store->ttl("ttl") == -2);
    assert(store->zadd("zadd", {{"member", 1}}).value == 1); // No stale WRONGTYPE.
    assert(store->ttl("zadd") == -1);
    assert(store->zrem("zrem", {"member"}).value == 0);
    assert(store->zscore("zscore", "member").status == storage::Status::NotFound);
    assert(store->zrank("zrank", "member").status == storage::Status::NotFound);
    assert(store->zcard("zcard").value == 0);
    assert(store->zquery("zquery", 0, "", 0, 1).value.empty());
    assert(store->processTTLExpirations(now) == 1); // The KEYS-only entry remains.
    assert(!store->nextTTLDeadlineMS());
}

void checkActiveExpirationAndBounds() {
    auto store = makeStore();
    // Thousands of live timers survive hash-table migration and heap growth.
    for (int i = 0; i < 2500; ++i) {
        const auto key = std::to_string(i);
        store->set(key, "value");
        store->expire(key, 1);
    }
    assert(store->processTTLExpirations(now + 999) == 0);
    now += 1000;
    assert(store->processTTLExpirations(now, 0) == 0);
    assert(store->processTTLExpirations(now, 17) == 17);
    assert(store->processTTLExpirations(now) == timers::ExpirationBudget);
    assert(timers::pollTimeoutMS(store->nextTTLDeadlineMS(), now) == 0);
    assert(store->processTTLExpirations(now) == 483);
    assert(!store->nextTTLDeadlineMS() && store->keys().empty());

    store->set("large", "value");
    store->expire("large", 1);
    const auto deadline = store->nextTTLDeadlineMS();
    try {
        store->expire("large", std::numeric_limits<std::int64_t>::max());
        assert(false);
    } catch (const std::overflow_error&) {}
    assert(store->nextTTLDeadlineMS() == deadline);
    const auto maximumSeconds = (std::numeric_limits<std::uint64_t>::max() - now) / 1000;
    assert(store->expire("large", static_cast<std::int64_t>(maximumSeconds)));
    assert(store->ttl("large") == static_cast<std::int64_t>(maximumSeconds));
    assert(timers::pollTimeoutMS(store->nextTTLDeadlineMS(), now) == std::numeric_limits<int>::max());

    const auto nearEnd = std::numeric_limits<std::uint64_t>::max() - 500;
    now = nearEnd;
    auto nearEndStore = makeStore();
    nearEndStore->set("key", "value");
    try {
        nearEndStore->expire("key", 1);
        assert(false);
    } catch (const std::overflow_error&) {}
    assert(nearEndStore->ttl("key") == -1 && !nearEndStore->nextTTLDeadlineMS());
}
} // namespace

int main() {
    checkHeap();
    checkIdleList();
    assert(timers::DefaultIdleTimeoutMS == 60000);
    assert(timers::pollTimeoutMS(std::nullopt, 10) == -1);
    assert(timers::pollTimeoutMS(10, 10) == 0);
    assert(timers::pollTimeoutMS(9, 10) == 0);
    assert(timers::pollTimeoutMS(17, 10) == 7);
    assert(timers::pollTimeoutMS(std::numeric_limits<std::uint64_t>::max(), 10) ==
           std::numeric_limits<int>::max());
    checkTTL();
    checkExpiredAccesses();
    checkActiveExpirationAndBounds();
    std::cout << "Timer tests passed (heap indexes, idle list, expiration, cleanup, bounds)\n";
}
