#include "batch.h"
#include "test_framework.h"

#include <chrono>
#include <string>
#include <vector>

#include "payloads.h"

using tombstone::Batch;

TEST_CASE("batch", "forced flush drains buffered items into an envelope") {
    Batch batch;
    CHECK(!batch.has_items());
    CHECK(!batch.add(R"({"name":"a"})"));  // one item: below the count trigger
    CHECK(batch.has_items());
    CHECK_EQ(batch.size(), std::size_t{1});
    const auto envelope =
        batch.drain_if_ready("2026-06-11T00:00:00.000Z", std::chrono::steady_clock::now(), true);
    CHECK(envelope.has_value());
    CHECK_EQ(*envelope,
             std::string{R"({"sentAtIso":"2026-06-11T00:00:00.000Z","items":[{"name":"a"}]})"});
    CHECK(!batch.has_items());  // emptied after draining
}

TEST_CASE("batch", "no trigger leaves the buffer intact") {
    Batch batch;
    batch.add(R"({"n":1})");
    const auto envelope =
        batch.drain_if_ready("t", std::chrono::steady_clock::now(), /*force=*/false);
    CHECK(!envelope.has_value());
    CHECK(batch.has_items());
}

TEST_CASE("batch", "count trigger fires at flush_count") {
    Batch batch;
    bool triggered = false;
    for (std::size_t i = 0; i < Batch::flush_count; ++i) {
        triggered = batch.add(R"({"n":1})");
    }
    CHECK(triggered);  // the flush_count-th add reports the count trigger
    CHECK(batch.drain_if_ready("t", std::chrono::steady_clock::now(), false).has_value());
}

TEST_CASE("batch", "age trigger fires once the oldest item exceeds flush_age") {
    Batch batch;
    const auto t0 = std::chrono::steady_clock::now();
    batch.add(R"({"n":1})");
    CHECK(!batch.drain_if_ready("t", t0, false).has_value());  // fresh: no age trigger yet
    const auto aged = t0 + Batch::flush_age + std::chrono::seconds{1};
    CHECK(batch.drain_if_ready("t", aged, false).has_value());
}

TEST_CASE("batch", "bounded capacity drops the oldest item") {
    Batch batch;
    for (std::size_t i = 0; i < Batch::max_items + 5; ++i) {
        batch.add("{\"i\":" + std::to_string(i) + "}");
    }
    CHECK_EQ(batch.size(), Batch::max_items);  // never grows past the cap
    const auto envelope =
        batch.drain_if_ready("t", std::chrono::steady_clock::now(), /*force=*/true);
    CHECK(envelope.has_value());
    // The earliest items were dropped; the newest survived.
    CHECK(envelope->find("{\"i\":0}") == std::string::npos);
    CHECK(envelope->find("{\"i\":" + std::to_string(Batch::max_items + 4) + "}") !=
          std::string::npos);
}

TEST_CASE("batch", "splits large UTF-8 and escaped items without loss or reordering") {
    for (const std::string value : {std::string{"x"}, std::string{"\xe6\xbc\xa2"},
                                     std::string{"\\u0001"}}) {
        for (const std::size_t count : {Batch::flush_count, Batch::max_items}) {
            for (const bool snapshot : {false, true}) {
                Batch batch;
                std::vector<std::string> expected;
                std::string attributes;
                for (int key = 0; key < 32; ++key) {
                    if (key) attributes += ',';
                    attributes += "\"key" + std::to_string(key) + "\":\"";
                    for (int i = 0; i < 512; ++i) attributes += value;
                    attributes += '"';
                }
                for (std::size_t i = 0; i < count; ++i) {
                    expected.push_back("{\"name\":\"event_" + std::to_string(i) +
                                       "\",\"attributes\":{" + attributes + "}}");
                    batch.add(expected.back());
                }
                std::vector<std::string> envelopes;
                if (snapshot) {
                    envelopes = batch.drain_envelopes_if_ready("t", std::chrono::steady_clock::now(), true);
                    CHECK(!batch.has_items());
                } else {
                    while (batch.has_items()) {
                        envelopes.push_back(*batch.drain_if_ready("t", std::chrono::steady_clock::now(), true));
                        CHECK(envelopes.size() <= count);
                    }
                }
                CHECK(envelopes.size() > 1);
                std::string items;
                for (const auto &envelope : envelopes) {
                    CHECK(envelope.size() <= Batch::max_batch_bytes);
                    const auto start = envelope.find("\"items\":[") + 9;
                    if (!items.empty()) items += ',';
                    items += envelope.substr(start, envelope.size() - start - 2);
                }
                CHECK_EQ("{\"sentAtIso\":\"t\",\"items\":[" + items + "]}",
                         tombstone::build_batch_envelope("t", expected));
            }
        }
    }
}

TEST_CASE("batch", "snapshot honours readiness and isolates oversized invalid items") {
    Batch batch;
    batch.add("{}");
    CHECK(batch.drain_envelopes_if_ready("t", std::chrono::steady_clock::now(), false).empty());
    batch.add(std::string(Batch::max_batch_bytes, 'x'));
    batch.add("{}");
    const auto result = batch.drain_envelopes_if_ready("t", std::chrono::steady_clock::now(), true);
    CHECK_EQ(result.size(), std::size_t{3});
    CHECK(result.front().size() < Batch::max_batch_bytes);
    CHECK(result.back().size() < Batch::max_batch_bytes);
    CHECK(!batch.has_items());
}
