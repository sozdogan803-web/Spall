#include "test.h"

#include "core/json.h"

TEST(json_literals_and_numbers) {
    json::Value v;
    std::string err;
    CHECK(json::parse("[null, true, false, 0, -12.5, 3e2, 1E-3, 42]", v, err));
    CHECK(v.isArray());
    CHECK(v.size() == 8);
    CHECK(v[0].isNull());
    CHECK(v[1].asBool() == true);
    CHECK(v[2].asBool(true) == false);
    CHECK_NEAR(v[3].asNumber(), 0.0, 0.0);
    CHECK_NEAR(v[4].asNumber(), -12.5, 1e-12);
    CHECK_NEAR(v[5].asNumber(), 300.0, 1e-12);
    CHECK_NEAR(v[6].asNumber(), 0.001, 1e-15);
    CHECK_NEAR(v[7].asNumber(), 42.0, 0.0);
}

TEST(json_strings_and_escapes) {
    json::Value v;
    std::string err;
    CHECK(json::parse(R"(["a\"b", "tab\there", "é", "😀", "\/\\"])", v, err));
    CHECK_EQ_STR(v[0].asString(), "a\"b");
    CHECK_EQ_STR(v[1].asString(), "tab\there");
    CHECK_EQ_STR(v[2].asString(), "\xC3\xA9");             // U+00E9 in UTF-8
    CHECK_EQ_STR(v[3].asString(), "\xF0\x9F\x98\x80");     // U+1F600 via surrogate pair
    CHECK_EQ_STR(v[4].asString(), "/\\");
}

TEST(json_nested_objects_keep_order) {
    json::Value v;
    std::string err;
    CHECK(json::parse(R"({"b": {"x": [1, {"y": "z"}]}, "a": 2})", v, err));
    CHECK(v.isObject());
    CHECK(v.asObject().size() == 2);
    CHECK_EQ_STR(v.asObject()[0].key, "b");
    const json::Value* b = v.find("b");
    CHECK(b != nullptr);
    const json::Value* x = b ? b->find("x") : nullptr;
    CHECK(x && x->size() == 2);
    CHECK(x && (*x)[1].find("y") && (*x)[1].find("y")->asString() == "z");
    CHECK(v.find("missing") == nullptr);
    CHECK_NEAR(v.find("a")->asNumber(), 2.0, 0.0);
}

TEST(json_wrong_type_accessors_return_defaults) {
    json::Value v;
    std::string err;
    CHECK(json::parse(R"({"n": "text"})", v, err));
    CHECK_NEAR(v.find("n")->asNumber(-1.0), -1.0, 0.0);
    CHECK(v.find("n")->asArray().empty());
    CHECK(v.asString().empty());
}

TEST(json_error_reports_line_and_column) {
    json::Value v;
    std::string err;
    CHECK(!json::parse("{\n  \"a\": 1,\n  \"b\": tru\n}", v, err));
    CHECK(err.find("line 3") != std::string::npos);
    CHECK(err.find("column 8") != std::string::npos);
}

TEST(json_rejects_trailing_garbage_and_trailing_commas) {
    json::Value v;
    std::string err;
    CHECK(!json::parse("[1, 2] x", v, err));
    CHECK(!json::parse("[1, 2,]", v, err));
    CHECK(!json::parse("{\"a\": 1,}", v, err));
    CHECK(!json::parse("", v, err));
    CHECK(!json::parse("\"unterminated", v, err));
}
