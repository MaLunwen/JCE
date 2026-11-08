/* Complete Editor scan service and complete Engine transport; no real UDP/UI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_lan_discovery.h"
#include <jce/middleware/net/jce_lan_discovery.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_log.h>
#include <enet/enet.h>
#include <cstring>
#include <map>
#include <vector>

namespace {
struct Socket { std::vector<unsigned char> response; };
struct Ports {
    uint64_t now = 10000;
    unsigned acquired = 0, released = 0, refs = 0, created = 0, destroyed = 0;
    unsigned received = 0, clock_reads = 0;
    bool acquire_ok = true, create_ok = true, bind_ok = true;
    ENetSocket next = (ENetSocket)100;
    std::map<ENetSocket, Socket> live;
    std::vector<unsigned char> request;
} ports;
}

extern "C" {
bool jce__net_enet_acquire(void) {
    if (!ports.acquire_ok) return false;
    ++ports.acquired; ++ports.refs; return true;
}
void jce__net_enet_release(void) {
    CHECK(ports.refs > 0); ++ports.released;
    if (ports.refs) --ports.refs;
}
uint64_t JCE_CALL jce_test_lan_ticks(void) { ++ports.clock_reads; return ports.now; }
uint64_t JCE_CALL jce_test_lan_counter(void) { return 12345; }
ENetSocket jce_test_lan_create(ENetSocketType) {
    if (!ports.create_ok) return ENET_SOCKET_NULL;
    ENetSocket socket = ports.next++;
    ports.live.emplace(socket, Socket{}); ++ports.created; return socket;
}
void jce_test_lan_destroy(ENetSocket socket) {
    CHECK(ports.live.erase(socket) == 1); ++ports.destroyed;
}
int jce_test_lan_option(ENetSocket socket, ENetSocketOption, int) {
    CHECK(ports.live.count(socket) == 1); return 0;
}
int jce_test_lan_bind(ENetSocket socket, const ENetAddress *) {
    CHECK(ports.live.count(socket) == 1); return ports.bind_ok ? 0 : -1;
}
int jce_test_lan_send(ENetSocket socket, const ENetAddress *, const ENetBuffer *buffers, size_t count) {
    CHECK(ports.live.count(socket) == 1); CHECK(count == 1);
    const auto *bytes = static_cast<const unsigned char *>(buffers[0].data);
    ports.request.assign(bytes, bytes + buffers[0].dataLength);
    return static_cast<int>(buffers[0].dataLength);
}
int jce_test_lan_receive(ENetSocket socket, ENetAddress *address, ENetBuffer *buffers, size_t count) {
    ++ports.received;
    CHECK(ports.live.count(socket) == 1); CHECK(count == 1);
    auto &response = ports.live.at(socket).response;
    if (response.empty()) return 0;
    REQUIRE(response.size() <= buffers[0].dataLength);
    std::memcpy(buffers[0].data, response.data(), response.size());
    address->host = 1; address->port = 47775;
    const int length = static_cast<int>(response.size()); response.clear(); return length;
}
int jce_test_lan_host_ip(const ENetAddress *, char *text, size_t size) {
    if (size < sizeof("127.0.0.1")) return -1;
    std::memcpy(text, "127.0.0.1", sizeof("127.0.0.1")); return 0;
}
#define enet_socket_create jce_test_lan_create
#define enet_socket_destroy jce_test_lan_destroy
#define enet_socket_set_option jce_test_lan_option
#define enet_socket_bind jce_test_lan_bind
#define enet_socket_send jce_test_lan_send
#define enet_socket_receive jce_test_lan_receive
#define enet_address_get_host_ip jce_test_lan_host_ip
#define jce_time_ticks_ms jce_test_lan_ticks
#define jce_time_perf_counter jce_test_lan_counter
#include JCE_TEST_LAN_TRANSPORT_OWNER
#undef enet_socket_create
#undef enet_socket_destroy
#undef enet_socket_set_option
#undef enet_socket_bind
#undef enet_socket_send
#undef enet_socket_receive
#undef enet_address_get_host_ip
#undef jce_time_ticks_ms
#undef jce_time_perf_counter
}

/* Other destinations of the complete maintenance/shutdown bodies are typed
 * no-op ports. The LAN service and transport above remain actual owners. */
static struct { bool initialized = false, active = true; uint64_t last_time = 0; } s_editor;
static void jce_editor_recorder_update(void) {}
static void jce_editor_config_flush_tick(float) {}
static void jce_editor_pstate_flush_tick(float) {}
static void jce_editor_panel_bundle_browser_shutdown(void) {}
static void jce_editor_panel_test_runner_shutdown(void) {}
static void jce_hierarchy_storage_shutdown(void) {}
static void jce_file_viewer_shutdown(void) {}
static void jce_editor_panel_console_shutdown(void) {}
#define JCE_EDITOR_HOST_SERVICE_CALL(name, call) do { call; } while (false)
#define jce_time_perf_counter jce_test_lan_counter
#include JCE_TEST_LAN_HOST_BODIES
#undef jce_time_perf_counter
#undef JCE_EDITOR_HOST_SERVICE_CALL

namespace {
struct Fixture {
    Fixture() {
        jce_editor_lan_discovery_shutdown();
        jce_lan_discovery_server_stop();
        ports = {}; s_editor = {}; s_editor.active = true;
        jce_log_set_level(JCE_LOG_LEVEL_OFF);
    }
    ~Fixture() {
        // Direct cleanup remains intact in ordinary missing-hook controls.
        jce_editor_lan_discovery_shutdown();
        jce_lan_discovery_server_stop();
        CHECK(ports.refs == 0); CHECK(ports.live.empty());
    }
};
void response_for_scan() {
    REQUIRE(ports.request.size() == 11);
    REQUIRE(ports.live.size() == 1);
    // Independent wire vector: JCEL, v1, RESP, echoed nonce, port7777,
    // players2/max8, protocol1, four-byte name. No copied decoder predicate.
    std::vector<unsigned char> response = {
        'J','C','E','L',1,0,2,0,0,0,0,0x61,0x1e,0,0,2,0,8,0,1,0,0,0,4,0,'T','e','s','t'};
    std::memcpy(response.data()+7, ports.request.data()+7, 4);
    ports.live.begin()->second.response = response;
}
}

TEST_CASE("never-started maintenance and shutdown borrow no socket or ENet reference") {
    Fixture fixture;
    jce_editor_panels_shutdown(); jce_editor_panels_shutdown();
    jce_lan_discovery_client_stop_scan();
    for (int i=0; i<4; ++i) jce_editor_tick_cpu_services(1.0f, true);
    s_editor.initialized = true;
    for (int i=0; i<4; ++i) jce_editor_tick_cpu_services(1.0f, true);
    CHECK(ports.acquired == 0); CHECK(ports.created == 0); CHECK(ports.destroyed == 0);
    CHECK(ports.released == 0); CHECK(ports.received == 0); CHECK(ports.clock_reads == 0);
    CHECK_FALSE(jce_lan_discovery_client_is_scanning());
}
TEST_CASE("hidden tab expiry uses actual CPU maintenance without drawing") {
    Fixture fixture; s_editor.initialized = true;
    REQUIRE(jce_editor_lan_discovery_refresh()); response_for_scan();
    jce_editor_tick_cpu_services(0.0f, true);
    REQUIRE(jce_lan_discovery_client_server_count() == 1);
    JceLanDiscoveredServer server = {};
    REQUIRE(jce_lan_discovery_client_get_server(0, &server));
    CHECK(std::strcmp(server.server_name, "Test") == 0); CHECK(server.game_port == 7777);
    ports.now += 2999;
    jce_editor_tick_cpu_services(0.0f, true);
    CHECK(jce_lan_discovery_client_is_scanning()); CHECK(ports.refs == 1);
    ++ports.now;
    jce_editor_tick_cpu_services(0.0f, true);
    CHECK_FALSE(jce_lan_discovery_client_is_scanning());
    CHECK(ports.destroyed == 1); CHECK(ports.released == 1); CHECK(ports.live.empty());
    CHECK(jce_lan_discovery_client_server_count() == 1); // expiry retains results
    const auto clocks = ports.clock_reads, receives = ports.received;
    jce_editor_tick_cpu_services(0.0f, true);
    CHECK(ports.clock_reads == clocks); CHECK(ports.received == receives);
}
TEST_CASE("active shutdown retires the client once and clears retained results") {
    Fixture fixture; s_editor.initialized = true;
    REQUIRE(jce_editor_lan_discovery_refresh()); response_for_scan();
    jce_editor_tick_cpu_services(0.0f, false);
    REQUIRE(jce_lan_discovery_client_server_count() == 1);
    jce_editor_panels_shutdown();
    CHECK_FALSE(jce_lan_discovery_client_is_scanning()); CHECK(ports.refs == 0);
    CHECK(ports.live.empty()); CHECK(ports.destroyed == 1);
    CHECK(jce_lan_discovery_client_server_count() == 0);
    jce_editor_panels_shutdown(); jce_lan_discovery_client_stop_scan();
    CHECK(ports.destroyed == 1); CHECK(ports.released == 1);
}
TEST_CASE("explicit stop and repeated Editor sessions remain balanced") {
    Fixture fixture;
    bool explicitly_stopped = false;
    SUBCASE("active shutdown each session") {}
    SUBCASE("explicit Stop before shutdown") { explicitly_stopped = true; }
    for (unsigned session=1; session<=3; ++session) {
        s_editor.initialized = true;
        REQUIRE(jce_editor_lan_discovery_refresh());
        if (explicitly_stopped) {
            jce_lan_discovery_client_stop_scan(); jce_lan_discovery_client_stop_scan();
        }
        else {
            CHECK(jce_lan_discovery_client_is_scanning());
        }
        jce_editor_panels_shutdown(); s_editor.initialized = false;
        CHECK_FALSE(jce_lan_discovery_client_is_scanning());
        jce_editor_tick_cpu_services(1.0f, true);
        CHECK(ports.created == session); CHECK(ports.destroyed == session);
        CHECK(ports.acquired == session); CHECK(ports.released == session);
    }
}
TEST_CASE("Refresh failures do not retain a socket or reference and allow retry") {
    Fixture fixture;
    SUBCASE("ENet admission") { ports.acquire_ok = false; }
    SUBCASE("socket creation") { ports.create_ok = false; }
    SUBCASE("socket bind") { ports.bind_ok = false; }
    CHECK_FALSE(jce_editor_lan_discovery_refresh());
    CHECK_FALSE(jce_lan_discovery_client_is_scanning()); CHECK(ports.refs == 0);
    CHECK(ports.live.empty());
    jce_editor_panels_shutdown();
    ports.acquire_ok = ports.create_ok = ports.bind_ok = true;
    REQUIRE(jce_editor_lan_discovery_refresh()); CHECK(ports.refs == 1);
}
TEST_CASE("Refresh replaces its active scan but Editor shutdown leaves the server owner alone") {
    Fixture fixture;
    REQUIRE(jce_editor_lan_discovery_refresh());
    REQUIRE(jce_editor_lan_discovery_refresh());
    CHECK(ports.created == 2); CHECK(ports.destroyed == 1); CHECK(ports.refs == 1);
    REQUIRE(jce_lan_discovery_server_start("Test",7777,0,8,0));
    CHECK(ports.refs == 2);
    jce_editor_panels_shutdown();
    CHECK_FALSE(jce_lan_discovery_client_is_scanning());
    CHECK(jce_lan_discovery_server_is_running()); CHECK(ports.refs == 1);
}
