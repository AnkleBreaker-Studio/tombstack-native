#include "json_writer.h"
#include "test_framework.h"

using tombstone::json_escape;
using tombstone::JsonWriter;
using tombstone::utf8_safe_truncate;

TEST_CASE("json_writer", "escapes quotes backslashes and control characters") {
    CHECK_EQ(json_escape("plain"), std::string{"plain"});
    CHECK_EQ(json_escape("say \"hi\""), std::string{"say \\\"hi\\\""});
    CHECK_EQ(json_escape("a\\b"), std::string{"a\\\\b"});
    CHECK_EQ(json_escape("line1\nline2"), std::string{"line1\\nline2"});
    CHECK_EQ(json_escape("tab\there"), std::string{"tab\\there"});
    CHECK_EQ(json_escape("cr\rlf"), std::string{"cr\\rlf"});
    CHECK_EQ(json_escape(std::string{"nul"} + '\x01'), std::string{"nul\\u0001"});
    CHECK_EQ(json_escape("\b\f"), std::string{"\\b\\f"});
}

TEST_CASE("json_writer", "builds flat objects in call order") {
    JsonWriter json;
    json.begin_object();
    json.string_field("a", "1");
    json.bool_field("b", true);
    json.bool_field("c", false);
    json.end_object();
    CHECK_EQ(json.str(), std::string{R"({"a":"1","b":true,"c":false})"});
}

TEST_CASE("json_writer", "builds nested arrays of objects") {
    JsonWriter json;
    json.begin_object();
    json.string_field("name", "x");
    json.begin_array("items");
    json.begin_object();
    json.string_field("k", "v1");
    json.end_object();
    json.begin_object();
    json.string_field("k", "v2");
    json.end_object();
    json.end_array();
    json.bool_field("done", true);
    json.end_object();
    CHECK_EQ(json.str(),
             std::string{R"({"name":"x","items":[{"k":"v1"},{"k":"v2"}],"done":true})"});
}

TEST_CASE("json_writer", "builds nested object fields") {
    JsonWriter json;
    json.begin_object();
    json.begin_object("attributes");
    json.string_field("level", "3");
    json.string_field("boss", "true");
    json.end_object();
    json.end_object();
    CHECK_EQ(json.str(), std::string{R"({"attributes":{"level":"3","boss":"true"}})"});
}

TEST_CASE("json_writer", "utf8 truncation never splits a multibyte sequence") {
    // U+00E9 (e-acute) is 2 bytes: 0xC3 0xA9.
    const std::string two_byte = "caf\xC3\xA9";  // 5 bytes
    CHECK_EQ(std::string{utf8_safe_truncate(two_byte, 5)}, two_byte);
    CHECK_EQ(std::string{utf8_safe_truncate(two_byte, 4)}, std::string{"caf"});
    // U+20AC (euro) is 3 bytes: 0xE2 0x82 0xAC.
    const std::string three_byte = "ab\xE2\x82\xAC";  // 5 bytes
    CHECK_EQ(std::string{utf8_safe_truncate(three_byte, 4)}, std::string{"ab"});
    CHECK_EQ(std::string{utf8_safe_truncate(three_byte, 3)}, std::string{"ab"});
    CHECK_EQ(std::string{utf8_safe_truncate("ascii", 3)}, std::string{"asc"});
}
