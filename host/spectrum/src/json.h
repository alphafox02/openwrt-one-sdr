// SPDX-License-Identifier: MIT
// Minimal JSON: a value tree, a parser and a writer.
#pragma once

#include <string>
#include <vector>

struct JsonValue {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> items;   // array elements, or object member values
    std::vector<std::string> keys;  // object member names, parallel to items

    bool is_null() const { return kind == Null; }
    bool is_number() const { return kind == Number; }
    bool is_string() const { return kind == String; }
    bool is_boolean() const { return kind == Boolean; }

    // The member `key`, or nullptr when there is none or this is not an object.
    const JsonValue *find(const std::string &key) const;
    // The member at a dotted path such as "stream.center_hz", or nullptr.
    const JsonValue *at(const std::string &path) const;
    // True when the member exists and is not null.
    bool has(const std::string &key) const;

    // Typed member reads, with a fallback for a missing or differently typed member.
    double number_or(const std::string &key, double fallback) const;
    std::string string_or(const std::string &key, const std::string &fallback = "") const;
    bool boolean_or(const std::string &key, bool fallback) const;
};

// Parses one JSON document. Throws std::runtime_error naming the position of the first error.
JsonValue parse_json(const std::string &text);

std::string json_write(const JsonValue &value);   // compact
std::string json_quote(const std::string &text);  // "text", escaped
std::string json_number(double value);            // finite; null otherwise
