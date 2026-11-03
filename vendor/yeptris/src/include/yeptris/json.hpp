// json.hpp — the nlohmann-flavored C++17 wrapper (TODO.impl/21).
//
// Value semantics over the C ABI: yeptris::json owns its document
// (RAII), children are views into it. JSON in, JSON out — strict
// parsing via yeptris_parse_json, output via the JSON writer.
// Errors throw (nlohmann discipline); no exceptions in the C core.
//
// Deliberately a view, not a copy: j["k"] returns a child that stays
// valid while the parent json lives (the nlohmann ergonomics without
// the deep-template machinery).

#ifndef YEPTRIS_JSON_HPP
#define YEPTRIS_JSON_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <yeptris/dom.h>
#include <yeptris/json.h>

namespace yeptris {

class parse_error : public std::runtime_error {
  public:
    explicit parse_error(const char* msg) : std::runtime_error(msg) {}
};

class type_error : public std::logic_error {
  public:
    explicit type_error(const char* msg) : std::logic_error(msg) {}
};

class json {
  public:
    json() = default;

    json(const json&) = delete;
    json& operator=(const json&) = delete;

    json(json&& other) noexcept : doc_(other.doc_), node_(other.node_), owns_(other.owns_) {
        other.doc_ = nullptr;
        other.node_ = nullptr;
        other.owns_ = false;
    }

    json& operator=(json&& other) noexcept {
        if (this != &other) {
            destroy();
            doc_ = other.doc_;
            node_ = other.node_;
            owns_ = other.owns_;
            other.doc_ = nullptr;
            other.node_ = nullptr;
            other.owns_ = false;
        }
        return *this;
    }

    ~json() {
        destroy();
    }

    static json parse(std::string_view text) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument d = yeptris_parse_json(text.data(), text.size(), &st);
        if (d == nullptr) {
            uint32_t line = 0, col = 0;
            const char* msg = yeptris_last_error(&line, &col);
            throw parse_error(msg ? msg : "not valid JSON");
        }
        json j;
        j.doc_ = d;
        j.node_ = yeptris_document_root(d, 0);
        j.owns_ = true;
        return j;
    }

    std::string dump() const {
        check_root();
        size_t len = 0;
        char* out = yeptris_serialize_json(doc_, &len);
        if (out == nullptr) {
            throw std::runtime_error("serialization failed");
        }
        std::string s(out, len);
        free(out);
        return s;
    }

    // ---- type queries (nlohmann names) ----
    bool is_object() const {
        return kind() == YEPTRIS_NODE_MAPPING;
    }
    bool is_array() const {
        return kind() == YEPTRIS_NODE_SEQUENCE;
    }
    bool is_string() const {
        return kind() == YEPTRIS_NODE_SCALAR && !is_number() && !is_boolean() && !is_null();
    }
    bool is_number() const {
        return is_number_integer() || is_number_float();
    }
    bool is_number_integer() const {
        return typed_ok<int64_t>();
    }
    bool is_number_float() const {
        return typed_ok<double>();
    }
    bool is_boolean() const {
        return typed_ok<bool>();
    }
    bool is_null() const;

    // ---- object access ----
    json operator[](std::string_view key) const {
        require(YEPTRIS_NODE_MAPPING, "object");
        YeptrisNode n = yeptris_node_map_get(node_, key.data(), key.size());
        if (n == nullptr) {
            throw std::out_of_range("key not found");
        }
        return child(n);
    }

    json at(std::string_view key) const {
        return (*this)[key];
    }

    bool contains(std::string_view key) const {
        return kind() == YEPTRIS_NODE_MAPPING &&
               yeptris_node_map_get(node_, key.data(), key.size()) != nullptr;
    }

    size_t size() const {
        if (kind() == YEPTRIS_NODE_MAPPING) {
            return yeptris_node_map_count(node_);
        }
        if (kind() == YEPTRIS_NODE_SEQUENCE) {
            return yeptris_node_seq_count(node_);
        }
        return 0;
    }

    // ---- array access ----
    json operator[](size_t index) const {
        require(YEPTRIS_NODE_SEQUENCE, "array");
        if (index >= yeptris_node_seq_count(node_)) {
            throw std::out_of_range("index out of range");
        }
        return child(yeptris_node_seq_at(node_, index));
    }

    json at(size_t index) const {
        return (*this)[index];
    }

    // ---- typed getters ----
    std::string get_string() const {
        if (kind() != YEPTRIS_NODE_SCALAR) {
            throw type_error("not a scalar");
        }
        size_t len = 0;
        const char* v = yeptris_node_value(node_, &len);
        return std::string(v ? v : "", len); /* json-c semantics: the
            raw text — "1" for numbers, "true" for booleans */
    }

    int64_t get_int() const {
        int64_t v = 0;
        if (yeptris_node_int(node_, &v) != YEPTRIS_OK) {
            throw type_error("not an integer");
        }
        return v;
    }

    double get_double() const {
        double v = 0;
        if (yeptris_node_float(node_, &v) != YEPTRIS_OK) {
            throw type_error("not a float");
        }
        return v;
    }

    bool get_bool() const {
        int v = 0;
        if (yeptris_node_bool(node_, &v) != YEPTRIS_OK) {
            throw type_error("not a boolean");
        }
        return v != 0;
    }

  private:
    json(YeptrisDocument doc, YeptrisNode node) : doc_(doc), node_(node), owns_(false) {}

    json child(YeptrisNode n) const {
        return json(doc_, n);
    }

    void destroy() {
        if (owns_ && doc_ != nullptr) {
            yeptris_document_free(doc_);
        }
        doc_ = nullptr;
        node_ = nullptr;
        owns_ = false;
    }

    void check_root() const {
        if (doc_ == nullptr) {
            throw std::logic_error("empty json");
        }
    }

    int kind() const {
        return node_ != nullptr ? (int)yeptris_node_kind(node_) : -1;
    }

    void require(int k, const char* what) const {
        if (kind() != k) {
            throw type_error(what);
        }
    }

    template <typename T> bool typed_ok() const {
        if (node_ == nullptr || kind() != YEPTRIS_NODE_SCALAR) {
            return false;
        }
        if constexpr (std::is_same_v<T, int64_t>) {
            int64_t v;
            return yeptris_node_int(node_, &v) == YEPTRIS_OK;
        } else if constexpr (std::is_same_v<T, double>) {
            double v;
            return yeptris_node_float(node_, &v) == YEPTRIS_OK;
        } else {
            int v;
            return yeptris_node_bool(node_, &v) == YEPTRIS_OK;
        }
    }

    YeptrisDocument doc_ = nullptr;
    YeptrisNode node_ = nullptr;
    bool owns_ = false;
};

inline bool json::is_null() const {
    if (kind() != YEPTRIS_NODE_SCALAR) {
        return false;
    }
    size_t len = 0;
    const char* v = yeptris_node_value(node_, &len);
    return len == 4 && v != nullptr && std::string_view(v, len) == "null";
}

} // namespace yeptris

#endif // YEPTRIS_JSON_HPP
