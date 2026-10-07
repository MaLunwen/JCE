#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/core/jce_editor_history_identity.h"
TEST_CASE("history tokens survive multiple full restores and reject duplicate roster") {
    JceEditorHistoryIdentity identity;
    uint64_t epoch = identity.epoch(), a = identity.token(101), b = identity.token(102);
    REQUIRE(a != b);
    REQUIRE(identity.bind({{201, a}, {202, b}}, epoch));
    CHECK(identity.handle(a) == 201);
    REQUIRE(identity.bind({{301, a}, {302, b}}, epoch));
    CHECK(identity.handle(a) == 301);
    CHECK_FALSE(identity.bind({{401, a}, {402, a}}, epoch));
    CHECK(identity.handle(a) == 301);
    CHECK_FALSE(identity.bind({{401, a}, {401, b}}, epoch));
    identity.prune([](uint64_t handle) { return handle != 301; });
    CHECK(identity.handle(a) == 0);
    uint64_t recycled = identity.token(301);
    CHECK(recycled != a);
    identity.clear();
    CHECK(identity.handle(b) == 0);
    REQUIRE(identity.bind({{501, a}}, epoch));
    CHECK(identity.handle(a) == 0);
    CHECK(identity.token(501) != a);
}

TEST_CASE("history identity admission and transactional replacement are bounded") {
    JceEditorHistoryIdentity identity(512);
    auto epoch = identity.epoch();
    auto first = identity.token(1);
    REQUIRE(first != 0);
    REQUIRE(identity.token(2) != 0);
    CHECK(identity.token(3) == 0);
    CHECK_FALSE(identity.bind({{10, first}, {11, 100}}, epoch));
    CHECK(identity.handle(first) == 1);
    identity.clear();
    CHECK(identity.estimated_bytes() < 512);
    CHECK(identity.token(3) != 0);
}

TEST_CASE("prepared history rekeys all source nodes before colliding live handles") {
    JceEditorHistoryIdentity identity;
    auto epoch = identity.epoch();
    auto a = identity.token(101), b = identity.token(102);
    JceEditorHistoryIdentity::Prepared prepared;
    REQUIRE(identity.prepare({{101, a}, {102, b}}, epoch, prepared));
    CHECK(identity.handle(a) == 101);
    REQUIRE(identity.commit(prepared, {102, 101}));
    CHECK(identity.handle(a) == 102);
    CHECK(identity.handle(b) == 101);
    CHECK_FALSE(identity.commit(prepared, {201, 202}));
    JceEditorHistoryIdentity::Prepared bad;
    CHECK_FALSE(identity.prepare({{1, a}, {2, a}}, epoch, bad));
    CHECK(identity.handle(a) == 102);
    REQUIRE(identity.prepare({{1, a}, {2, b}}, epoch, bad));
    CHECK_FALSE(identity.commit(bad, {500, 500}));
    CHECK(identity.handle(a) == 102);
}

TEST_CASE("diagnostic identity budget refuses before changing live bindings") {
    JceEditorHistoryIdentity identity;
    auto epoch = identity.epoch();
    auto token = identity.token(1);
    CHECK_FALSE(identity.set_budget(63));
    CHECK_FALSE(identity.set_budget(64u * 1024u * 1024u + 1u));
    REQUIRE(identity.set_budget(1024));
    JceEditorHistoryIdentity::Prepared prepared;
    CHECK_FALSE(identity.prepare({{1, token}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}, epoch, prepared));
    CHECK(identity.handle(token) == 1);
    CHECK(identity.epoch() == epoch);
}

TEST_CASE("sparse history empty restore and inverse roster reintroduce scoped token") {
    JceEditorHistoryIdentity identity;
    auto epoch = identity.epoch();
    CHECK(identity.existing_token(101) == 0);
    auto a = identity.token(101);
    REQUIRE(a != 0);
    REQUIRE(identity.bind({}, epoch));
    CHECK(identity.count() == 0);
    CHECK(identity.handle(a) == 0);
    REQUIRE(identity.bind({{201, a}}, epoch));
    CHECK(identity.handle(a) == 201);
    CHECK(identity.existing_token(202) == 0);
    REQUIRE(identity.bind({{301, a}}, epoch));
    CHECK(identity.count() == 1);
    identity.clear();
    REQUIRE(identity.bind({{401, a}}, epoch));
    CHECK(identity.handle(a) == 0);
    CHECK(identity.existing_token(401) != a);
}
