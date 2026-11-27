#include "test_framework.h"

#include <fstream>
#include <iterator>
#include <string>

// Statements in the public header that integrators act on, pinned so they cannot
// silently disappear. TOMBSTONE_SOURCE_DIR is set by CMakeLists.txt.

namespace {

std::string read_public_header() {
    std::ifstream in{std::string{TOMBSTONE_SOURCE_DIR} + "/include/tombstone/tombstone.h",
                     std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/** The doc comment directly above the first occurrence of `declaration`. */
std::string doc_comment_before(const std::string &header, const std::string &declaration) {
    const std::size_t at = header.find(declaration);
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t open = header.rfind("/**", at);
    return open == std::string::npos ? std::string{} : header.substr(open, at - open);
}

}  // namespace

TEST_CASE("header_contract", "tombstone_report_crash is documented as not async-signal-safe") {
    const std::string header = read_public_header();
    CHECK(!header.empty());
    const std::string doc =
        doc_comment_before(header, "tombstone_result tombstone_report_crash(");
    CHECK(doc.find("NOT async-signal-safe") != std::string::npos);
    CHECK(doc.find("signal handler") != std::string::npos);
    CHECK(doc.find("exception") != std::string::npos);
}

TEST_CASE("header_contract", "the server refusals and what happens to reports are documented") {
    const std::string header = read_public_header();
    const std::string top = header.substr(0, header.find("#include"));
    CHECK(top.find("402") != std::string::npos);
    CHECK(top.find("401 invalid_api_key") != std::string::npos);
    CHECK(top.find("403") != std::string::npos);
    CHECK(top.find("404") != std::string::npos);
    CHECK(top.find("(400, 413, 422)") != std::string::npos);  // the only payloads deleted
    CHECK(top.find("90 days") != std::string::npos);
}

TEST_CASE("header_contract", "the header does not say that every entry point returns a result") {
    // Eight entry points return void.
    const std::string header = read_public_header();
    CHECK(!header.empty());
    CHECK(header.find("Every entry point returns a tombstone_result") == std::string::npos);
}
