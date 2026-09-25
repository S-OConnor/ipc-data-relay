#include <string>
#include <vector>

#include "ipcrelay/bridge/bridge_config.hpp"
#include "ipcrelay/common/config.hpp"
#include "receiver/receiver_config.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;

TEST(config_parse_sections_and_repeats) {
    ConfigFile f;
    std::string err;
    REQUIRE(parse_config_text(
        "# comment\n"
        "a = 1\n"
        "b = \"quoted # not comment\"  ; trailing comment\n"
        "[source]\n"
        "id = 1\n"
        "endpoint = ipc:///tmp/a.sock\n"
        "[source]\n"
        "id = 2\n"
        "endpoint=ipc:///tmp/b.sock\n"
        "filter = A\n"
        "filter = B\n",
        f, err));
    CHECK_EQ(f.global().get("a").value_or(""), "1");
    CHECK_EQ(f.global().get("b").value_or(""), "quoted # not comment");
    auto sources = f.named("source");
    REQUIRE(sources.size() == 2);
    CHECK_EQ(sources[1]->get("id").value_or(""), "2");
    int filters = 0;
    for (const auto& e : sources[1]->entries) if (e.key == "filter") ++filters;
    CHECK_EQ(filters, 2);
}

TEST(config_parse_errors) {
    ConfigFile f;
    std::string err;
    CHECK(!parse_config_text("[unterminated\n", f, err));
    CHECK(!parse_config_text("novalue\n", f, err));
    CHECK(!parse_config_text("= 3\n", f, err));
    CHECK(!load_config_file("/nonexistent/file.conf", f, err));
}

TEST(config_overrides) {
    ConfigFile f;
    std::string err;
    REQUIRE(parse_config_text("x = 1\n[source]\nid = 1\n[source]\nid = 2\n", f, err));
    REQUIRE(apply_override(f, "x=2", err));
    CHECK_EQ(f.global().get("x").value_or(""), "2");
    REQUIRE(apply_override(f, "source.id=9", err));
    CHECK_EQ(f.named("source")[1]->get("id").value_or(""), "9");
    CHECK_EQ(f.named("source")[0]->get("id").value_or(""), "1");
    CHECK(!apply_override(f, "nosuch.key=1", err));
    CHECK(!apply_override(f, "novalue", err));
}

TEST(config_value_parsers) {
    std::string err;
    bool b = false;
    CHECK(parse_bool("yes", b, err) && b);
    CHECK(parse_bool("Off", b, err) && !b);
    CHECK(!parse_bool("maybe", b, err));
    uint32_t u = 0;
    CHECK(parse_u32("0x10", u, err) && u == 16);
    CHECK(!parse_u32("-1", u, err));
    CHECK(!parse_u32("4294967296", u, err));
    uint16_t p = 0;
    CHECK(parse_u16("65535", p, err));
    CHECK(!parse_u16("65536", p, err));
    uint32_t ip = 0;
    CHECK(parse_ipv4("239.1.2.3", ip, err) && is_multicast_ipv4(ip));
    CHECK(parse_ipv4("192.168.1.1", ip, err) && !is_multicast_ipv4(ip));
    CHECK(!parse_ipv4("300.1.1.1", ip, err));
}

static ConfigFile parse(const std::string& text) {
    ConfigFile f;
    std::string err;
    if (!parse_config_text(text, f, err)) std::fprintf(stderr, "parse error: %s\n", err.c_str());
    return f;
}

TEST(bridge_config_valid) {
    BridgeConfig cfg;
    std::vector<std::string> errors;
    REQUIRE(build_bridge_config(parse(
        "multicast_group = 239.10.0.1\nmulticast_port = 5000\nmulticast_interface = 127.0.0.1\n"
        "multicast_ttl = 4\nmax_datagram_size = 1400\n"
        "[source]\nid = 1\nendpoint = ipc:///tmp/a.sock\n"
        "[source]\nid = 2\nendpoint = ipc:///tmp/b.sock\nfilter = TLM\nfilter = DBG\nrecv_hwm = 5\n"
        "[source]\nid = 3\nendpoint = ipc:///tmp/c.sock\nfilter = \n"), cfg, errors));
    CHECK_EQ(cfg.multicast_port, 5000);
    CHECK_EQ(cfg.multicast_interface, 0x7F000001u);
    CHECK_EQ(cfg.multicast_ttl, 4);
    CHECK_EQ(cfg.fragment_payload(), 1356u);
    REQUIRE(cfg.sources.size() == 3);
    CHECK(cfg.sources[0].filters.empty());
    CHECK_EQ(cfg.sources[1].filters.size(), 2u);
    CHECK_EQ(cfg.sources[1].recv_hwm, 5);
    CHECK(cfg.sources[2].filters.empty());  // explicit empty filter = all
}

TEST(bridge_config_invalid_reports_all_problems) {
    BridgeConfig cfg;
    std::vector<std::string> errors;
    CHECK(!build_bridge_config(parse(
        "multicast_group = 192.168.1.1\nmulticast_port = 0\nmulticast_ttl = 300\nmax_datagram_size = 10\n"
        "bogus = 1\n"
        "[source]\nid = 1\nendpoint = ipc:///tmp/a.sock\n"
        "[source]\nid = 1\nendpoint = ipc:///tmp/a.sock\n"
        "[source]\nendpoint = notanendpoint\n"
        "[other]\n"), cfg, errors));
    // not multicast, port 0, ttl range, datagram size, unknown key, duplicate
    // id, duplicate endpoint, missing id, bad endpoint, unknown section
    CHECK(errors.size() >= 10);
    CHECK(!build_bridge_config(parse("multicast_group = 239.1.1.1\nmulticast_port = 5\n"), cfg, errors));
    CHECK_EQ(errors.size(), 1u);  // no sources
}

TEST(receiver_config_valid_and_invalid) {
    ReceiverConfig cfg;
    std::vector<std::string> errors;
    REQUIRE(build_receiver_config(parse(
        "multicast_group = 239.10.0.1\nmulticast_port = 5000\ncapture_file = /tmp/x.cap\n"
        "stats_endpoint = tcp://*:6001\ncommand_endpoint = tcp://127.0.0.1:6002\ncommand_bind = true\n"
        "reassembly_timeout_ms = 250\nrecord_on_start = false\n"), cfg, errors));
    CHECK_EQ(cfg.reassembly_timeout_ms, 250);
    CHECK(!cfg.record_on_start);
    CHECK(cfg.command_bind);

    CHECK(!build_receiver_config(parse(
        "multicast_group = 239.10.0.1\nmulticast_port = 5000\n"
        "stats_endpoint = ipc:///bad\nreassembly_timeout_ms = 0\nlog_level = loud\n"), cfg, errors));
    CHECK(errors.size() >= 4);  // capture_file, stats_endpoint, timeout, log_level
}
