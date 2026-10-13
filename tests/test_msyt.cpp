#include "msyt.hpp"
#include "test.hpp"

using namespace mm;

static std::string hex(const std::string& b) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : b) {
        if (!out.empty()) out += ' ';
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static std::string enc(const char* control) {
    return hex(msyt::encodeContents(nlohmann::json::array({{{"control", nlohmann::json::parse(control)}}})));
}

TEST("msyt: text and colours") {
    auto contents = nlohmann::json::parse(R"([{"text":"Hi "},{"control":{"kind":"set_colour","colour":"red"}},{"text":"A"},{"control":{"kind":"reset_colour"}}])");
    CHECK_EQ(hex(msyt::encodeContents(contents)),
             std::string("48 00 69 00 20 00 0E 00 00 00 03 00 02 00 00 00 41 00 0E 00 00 00 03 00 02 00 FF FF"));
    CHECK_EQ(hex(msyt::encodeAttributes("Npc")), std::string("4E 00 70 00 63 00 00 00"));
}

TEST("msyt: control codes match msyt's bytes") {
    CHECK_EQ(enc(R"({"kind":"variable","variable_kind":2,"name":"Cook"})"), std::string("0E 00 02 00 02 00 0C 00 08 00 43 00 6F 00 6F 00 6B 00 00 00"));
    CHECK_EQ(enc(R"({"kind":"icon","icon":"plus"})"), std::string("0E 00 01 00 07 00 02 00 17 CD"));
    CHECK_EQ(enc(R"({"kind":"icon","icon":{"a":10}})"), std::string("0E 00 01 00 07 00 02 00 0A CD"));
    CHECK_EQ(enc(R"({"kind":"pause","length":"long"})"), std::string("0E 00 05 00 01 00 00 00"));
    CHECK_EQ(enc(R"({"kind":"pause","frames":30})"), std::string("0E 00 01 00 00 00 04 00 1E 00 00 00"));
    CHECK_EQ(enc(R"({"kind":"auto_advance","frames":60})"), std::string("0E 00 01 00 03 00 04 00 3C 00 00 00"));
    CHECK_EQ(enc(R"({"kind":"text_size","percent":125})"), std::string("0E 00 00 00 02 00 02 00 7D 00"));
    CHECK_EQ(enc(R"({"kind":"font","font_kind":"hylian"})"), std::string("0E 00 00 00 01 00 02 00 00 00"));
    CHECK_EQ(enc(R"({"kind":"single_choice","label":3})"), std::string("0E 00 01 00 0A 00 04 00 03 00 01 CD"));
    CHECK_EQ(enc(R"({"kind":"choice","choice_labels":[0,1],"selected_index":0,"cancel_index":1,"unknown":6})"),
             std::string("0E 00 01 00 04 00 06 00 00 00 01 00 00 01"));
    CHECK_EQ(enc(R"({"kind":"sound","unknown":[1,2]})"), std::string("0E 00 03 00 01 00 02 00 01 02"));
    CHECK_EQ(enc(R"({"kind":"animation","name":"Ab"})"), std::string("0E 00 04 00 02 00 06 00 04 00 41 00 62 00"));
    CHECK_EQ(enc(R"({"kind":"localisation","localisation_kind":"gender","options":["a","bc"]})"),
             std::string("0E 00 C9 00 05 00 0A 00 02 00 61 00 04 00 62 00 63 00"));
    CHECK_EQ(enc(R"({"kind":"raw","two":{"one_field":[3,{"field_1":2}]}})"), std::string("0E 00 02 00 03 00 02 00"));
    CHECK_EQ(enc(R"({"kind":"raw","zero":{"four":{"field_1":2}}})"), std::string("0E 00 00 00 04 00 02 00"));
    bool threw = false;
    try {
        enc(R"({"kind":"choice","choice_labels":[1],"selected_index":0,"cancel_index":0,"unknown":0})");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
