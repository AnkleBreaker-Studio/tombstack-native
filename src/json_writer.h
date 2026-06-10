#ifndef TOMBSTONE_SRC_JSON_WRITER_H
#define TOMBSTONE_SRC_JSON_WRITER_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tombstone {

/** Standard JSON string escaping (quotes, backslash, control chars as \uXXXX). */
std::string json_escape(std::string_view value);

/**
 * Truncate to at most `max_bytes`, backing off so a multi-byte UTF-8 sequence
 * is never split (a split sequence would be invalid UTF-8 on the wire).
 */
std::string_view utf8_safe_truncate(std::string_view value, std::size_t max_bytes) noexcept;

/**
 * Minimal, dependency-free JSON builder for the Tombstone wire bodies.
 * Field order is exactly the call order — the payload builders use this to
 * produce byte-stable JSON the tests assert against. Absent optionals are
 * simply never written (the server rejects empty-string enums).
 */
class JsonWriter {
public:
    JsonWriter();

    /** Open the root object (or a nested object element inside an array). */
    void begin_object();
    /** Open a nested object-valued field: `"name":{`. */
    void begin_object(std::string_view name);
    /** Open an array-valued field: `"name":[`. */
    void begin_array(std::string_view name);
    void end_object();
    void end_array();

    void string_field(std::string_view name, std::string_view value);
    void bool_field(std::string_view name, bool value);

    /** The serialized document so far. Valid once every begin_* is closed. */
    const std::string &str() const noexcept { return out_; }

private:
    void element_prefix();
    void name_prefix(std::string_view name);
    void push_scope();
    void pop_scope();

    std::string out_;
    std::vector<bool> needs_comma_;
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_JSON_WRITER_H
