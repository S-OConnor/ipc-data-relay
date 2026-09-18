#include "ipcrelay/json_writer.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;

TEST(json_writer_basic) {
    JsonWriter j;
    j.begin_object();
    j.key("a").value(uint64_t{1});
    j.key("b").value("x\"y\n");
    j.key("c").begin_array().value(true).value(false).null().end_array();
    j.key("d").begin_object().key("e").value(1.5).end_object();
    j.end_object();
    CHECK_EQ(j.str(), std::string("{\"a\":1,\"b\":\"x\\\"y\\n\",\"c\":[true,false,null],\"d\":{\"e\":1.500}}"));
}
