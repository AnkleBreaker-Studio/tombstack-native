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
