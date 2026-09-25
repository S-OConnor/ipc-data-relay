// ipc-relay-ctl backend protocol code: WebSocket handshake and framing,
// HTTP request parsing and the JSON parser used for browser messages.
#include <string>

#include "http.hpp"
#include "json_value.hpp"
#include "test_framework.hpp"
#include "websocket.hpp"

using namespace ipcrelay::ctl;

namespace {

std::string hex(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int v : b) s.push_back(static_cast<char>(v));
    return s;
}

// Client frame with the given mask applied, as a browser would send it.
std::string masked_frame(int first_byte, const std::string& payload) {
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    std::string f;
    f.push_back(static_cast<char>(first_byte));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    for (unsigned char m : mask) f.push_back(static_cast<char>(m));
    for (std::size_t i = 0; i < payload.size(); ++i)
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    return f;
}

}  // namespace

TEST(ctl_sha1_and_base64_vectors) {
    CHECK_EQ(hex(sha1("")), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(hex(sha1("abc")), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ(base64_encode(""), std::string(""));
    CHECK_EQ(base64_encode("f"), std::string("Zg=="));
    CHECK_EQ(base64_encode("fo"), std::string("Zm8="));
    CHECK_EQ(base64_encode("foobar"), std::string("Zm9vYmFy"));
}

TEST(ctl_websocket_accept_key_rfc6455_example) {
    CHECK_EQ(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(ctl_websocket_parse_rfc6455_masked_hello) {
    const std::string buf = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    WsFrame f;
    std::size_t consumed = 0;
    std::string err;
    REQUIRE(ws_parse_frame(buf, 1024, f, consumed, err) == WsParse::Frame);
    CHECK(f.fin);
    CHECK_EQ(int{f.opcode}, int{ws_opcode::kText});
    CHECK_EQ(f.payload, std::string("Hello"));
    CHECK_EQ(consumed, buf.size());

    // Every strict prefix needs more data.
    for (std::size_t n = 0; n < buf.size(); ++n) {
        CHECK(ws_parse_frame(buf.substr(0, n), 1024, f, consumed, err) == WsParse::NeedMore);
    }
}

TEST(ctl_websocket_parse_extended_length_and_pipelining) {
    const std::string big(300, 'x');
    const std::string buf = masked_frame(0x81, big) + masked_frame(0x89, "p");
    WsFrame f;
    std::size_t consumed = 0;
    std::string err;
    REQUIRE(ws_parse_frame(buf, 1024, f, consumed, err) == WsParse::Frame);
    CHECK_EQ(f.payload, big);
    REQUIRE(ws_parse_frame(buf.substr(consumed), 1024, f, consumed, err) == WsParse::Frame);
    CHECK_EQ(int{f.opcode}, int{ws_opcode::kPing});
    CHECK_EQ(f.payload, std::string("p"));
}

TEST(ctl_websocket_parse_rejects_bad_frames) {
    WsFrame f;
    std::size_t consumed = 0;
    std::string err;
    // Unmasked client frame.
    CHECK(ws_parse_frame(bytes({0x81, 0x01, 'a'}), 1024, f, consumed, err) == WsParse::Error);
    // Reserved bits.
    CHECK(ws_parse_frame(masked_frame(0xC1, "a"), 1024, f, consumed, err) == WsParse::Error);
    // Unknown opcode.
    CHECK(ws_parse_frame(masked_frame(0x83, "a"), 1024, f, consumed, err) == WsParse::Error);
    // Fragmented control frame.
    CHECK(ws_parse_frame(masked_frame(0x09, "a"), 1024, f, consumed, err) == WsParse::Error);
    // Over the size limit (rejected from the header alone).
    CHECK(ws_parse_frame(masked_frame(0x81, std::string(200, 'x')).substr(0, 4), 100, f, consumed, err) ==
          WsParse::Error);
}

TEST(ctl_websocket_encode_lengths) {
    CHECK_EQ(hex(ws_encode_frame(ws_opcode::kText, "Hello")), std::string("810548656c6c6f"));
    const std::string mid = ws_encode_frame(ws_opcode::kText, std::string(300, 'a'));
    CHECK_EQ(hex(mid.substr(0, 4)), std::string("817e012c"));
    CHECK_EQ(mid.size(), std::size_t{304});
    const std::string large = ws_encode_frame(ws_opcode::kText, std::string(70000, 'a'));
    CHECK_EQ(hex(large.substr(0, 10)), std::string("817f0000000000011170"));
    CHECK_EQ(hex(ws_close_payload(1001, "bye")), std::string("03e9627965"));
}

TEST(ctl_http_parse_upgrade_request) {
    const std::string buf =
        "GET /ws?x=1 HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key:  dGhlIHNhbXBsZSBub25jZQ== \r\n"
        "\r\n"
        "trailing";
    HttpRequest req;
    std::size_t consumed = 0;
    std::string err;
    REQUIRE(parse_http_request(buf, 4096, req, consumed, err) == HttpParse::Request);
    CHECK_EQ(req.method, std::string("GET"));
    CHECK_EQ(req.target, std::string("/ws?x=1"));
    CHECK_EQ(req.path, std::string("/ws"));
    CHECK_EQ(req.header("host"), std::string("localhost:8080"));
    CHECK_EQ(req.header("sec-websocket-key"), std::string("dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK(header_has_token(req.header("connection"), "upgrade"));
    CHECK(!header_has_token(req.header("connection"), "close"));
    CHECK_EQ(buf.substr(consumed), std::string("trailing"));
}

TEST(ctl_http_parse_incomplete_and_invalid) {
    HttpRequest req;
    std::size_t consumed = 0;
    std::string err;
    CHECK(parse_http_request("GET / HTTP/1.1\r\nHost: x\r\n", 4096, req, consumed, err) == HttpParse::NeedMore);
    CHECK(parse_http_request(std::string(5000, 'a'), 4096, req, consumed, err) == HttpParse::Error);
    CHECK(parse_http_request("GET\r\n\r\n", 4096, req, consumed, err) == HttpParse::Error);
    CHECK(parse_http_request("GET / FTP/1.0\r\n\r\n", 4096, req, consumed, err) == HttpParse::Error);
    CHECK(parse_http_request("GET / HTTP/1.1\r\nno colon\r\n\r\n", 4096, req, consumed, err) == HttpParse::Error);
}

TEST(ctl_http_response_format) {
    const std::string r = http_response(404, "text/plain", "nope", false);
    CHECK(r.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK(r.find("Content-Length: 4\r\n") != std::string::npos);
    CHECK(r.size() >= 4 && r.substr(r.size() - 8) == "\r\n\r\nnope");
    const std::string head = http_response(200, "text/plain", "body", true);
    CHECK(head.find("Content-Length: 4\r\n") != std::string::npos);
    CHECK(head.substr(head.size() - 4) == "\r\n\r\n");
}

TEST(ctl_json_parse_browser_message) {
    json::Value v;
    std::string err;
    REQUIRE(json::parse(" {\"type\":\"record\",\"enabled\":true,\"id\":7,\"x\":[1,-2.5e3,null,\"a\\u00e9\\n\"]} ",
                        v, err));
    REQUIRE(v.is_object());
    REQUIRE(v.get("type") != nullptr);
    CHECK_EQ(v.get("type")->string, std::string("record"));
    CHECK(v.get("enabled")->is_bool() && v.get("enabled")->boolean);
    CHECK_EQ(v.get("id")->number, 7.0);
    const json::Value* x = v.get("x");
    REQUIRE(x && x->is_array() && x->array.size() == 4);
    CHECK_EQ(x->array[1].number, -2500.0);
    CHECK(x->array[2].is_null());
    CHECK_EQ(x->array[3].string, std::string("a\xc3\xa9\n"));
    CHECK(v.get("missing") == nullptr);
}

TEST(ctl_json_parse_surrogate_pair) {
    json::Value v;
    std::string err;
    REQUIRE(json::parse("\"\\ud83d\\ude00\"", v, err));
    CHECK_EQ(v.string, std::string("\xf0\x9f\x98\x80"));
    CHECK(!json::parse("\"\\ud83d\"", v, err));
}

TEST(ctl_json_parse_rejects_invalid) {
    json::Value v;
    std::string err;
    const char* bad[] = {"", "{", "{\"a\":}", "{\"a\" 1}", "[1,]", "01", "1.", "-", "tru", "\"a", "{} x",
                         "\"tab\there\"", "{'a':1}", "nul"};
    for (const char* text : bad) {
        if (json::parse(text, v, err)) ::testfw::report_failure(__FILE__, __LINE__, std::string("accepted: ") + text);
    }
    CHECK(!json::parse("[[[[1]]]]", v, err, 3));
    CHECK(json::parse("[[[1]]]", v, err, 3));
}
