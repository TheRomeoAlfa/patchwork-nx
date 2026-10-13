#include "msyt.hpp"

#include <map>
#include <stdexcept>

#include "msbt.hpp"

namespace mm::msyt {

namespace {

using json = nlohmann::json;

void u8(std::string& o, uint32_t v) { o += char(v & 0xff); }
void u16(std::string& o, uint32_t v) {
    o += char(v & 0xff);
    o += char((v >> 8) & 0xff);
}
void u32(std::string& o, uint32_t v) {
    for (int k = 0; k < 4; ++k) o += char((v >> (8 * k)) & 0xff);
}
void bytes(std::string& o, const json& arr) {
    for (const auto& b : arr) u8(o, b.get<uint32_t>());
}
uint32_t num(const json& j, const char* key) {
    if (!j.contains(key)) throw std::runtime_error(std::string("text control without ") + key);
    return j.at(key).get<uint32_t>();
}
// u16 byte count, then the UTF-16 string (no terminator).
void str(std::string& o, const std::string& s) {
    auto w = msbt::toUtf16(s);
    u16(o, uint32_t(w.size()));
    o += w;
}
void start(std::string& o, uint32_t group) {
    u16(o, 0x0E);
    u16(o, group);
}

uint32_t subIndex(const std::string& name) {
    static const std::map<std::string, uint32_t> n = {{"zero", 0}, {"one", 1}, {"two", 2},   {"three", 3}, {"four", 4}, {"five", 5},
                                                      {"six", 6},  {"seven", 7}, {"eight", 8}, {"nine", 9},  {"ten", 10}};
    auto it = n.find(name);
    if (it == n.end()) throw std::runtime_error("unknown text control subtype " + name);
    return it->second;
}

uint32_t icon(const json& j) {
    static const std::map<std::string, uint32_t> named = {
        {"l_stick_forward", 0}, {"l_stick_back", 1}, {"l_stick_left", 2},  {"l_stick_right", 3}, {"r_stick_vertical", 4},
        {"r_stick_horizontal", 5}, {"d_pad_up", 6},   {"d_pad_down", 7},    {"d_pad_left", 8},    {"d_pad_right", 9},
        {"y", 13},               {"b", 17},          {"l", 20},            {"r", 21},            {"plus", 23},
        {"minus", 24},           {"right_arrow", 25}, {"left_arrow", 26},  {"up_arrow", 27},     {"l_stick_press", 33},
        {"r_stick_press", 34},   {"gamepad", 36}};
    if (j.is_string()) {
        auto it = named.find(j.get<std::string>());
        if (it == named.end()) throw std::runtime_error("unknown icon " + j.get<std::string>());
        return it->second;
    }
    if (j.is_object() && j.size() == 1) return j.begin().value().get<uint32_t>();  // {"a": 10}, {"unknown": N}…
    throw std::runtime_error("bad icon");
}

uint32_t localisationKind(const json& j) {
    if (j.is_string()) {
        if (j == "gender") return 5;
        if (j == "plural") return 6;
    }
    if (j.is_object() && j.contains("unknown")) return j["unknown"].get<uint32_t>();
    throw std::runtime_error("bad localisation kind");
}

void localisationBody(std::string& o, uint32_t kind, const json& options) {
    u16(o, kind);
    std::string body;
    for (const auto& s : options) str(body, s.get<std::string>());
    u16(o, uint32_t(body.size()));
    o += body;
}

void raw(std::string& o, const json& c) {
    for (const auto& [group, v] : c.items()) {
        if (group == "kind") continue;
        if (group == "zero") {
            start(o, 0);
            auto [sub, f] = *v.items().begin();
            u16(o, subIndex(sub));
            for (const char* k : {"field_1", "field_2", "field_3"})
                if (f.contains(k)) u16(o, f[k].get<uint32_t>());
        } else if (group == "one") {
            start(o, 1);
            auto [subName, f] = *v.items().begin();
            uint32_t sub = subIndex(subName);
            u16(o, sub);
            switch (sub) {
                case 0:
                case 1:
                case 3:
                    u16(o, num(f, "field_1"));
                    u32(o, num(f, "field_2"));
                    break;
                case 2: u16(o, num(f, "field_1")); break;
                case 4:
                case 5:
                case 6: {
                    int n = int(sub) - 1;  // 3, 4 or 5 u16 fields, then 2 bytes
                    for (int k = 1; k <= n; ++k) u16(o, num(f, ("field_" + std::to_string(k)).c_str()));
                    bytes(o, f.at("field_" + std::to_string(n + 1)));
                    break;
                }
                case 7:
                    u16(o, num(f, "field_1"));
                    bytes(o, f.at("field_2"));
                    break;
                case 8: {
                    const auto& un = f.at("unknown_1");
                    const auto& f1 = f.at("field_1");
                    u16(o, uint32_t(4 * un.size() + 2 * f1.size() + 4));
                    for (const auto& g : un) bytes(o, g);
                    for (const auto& x : f1) u16(o, x.get<uint32_t>());
                    bytes(o, f.at("field_2"));
                    break;
                }
                case 9: {
                    std::string body;
                    if (f.contains("unknown_1") && !f["unknown_1"].is_null()) bytes(body, f["unknown_1"]);
                    for (const auto& s : f.at("strings")) {
                        u16(body, num(s, "field_1"));
                        str(body, s.at("string").get<std::string>());
                    }
                    u16(body, num(f, "field_3"));
                    u16(body, num(f, "field_4"));
                    if (f.contains("unknown_2") && !f["unknown_2"].is_null()) bytes(body, f["unknown_2"]);
                    bytes(body, f.at("field_6"));
                    u16(o, uint32_t(body.size()));
                    o += body;
                    break;
                }
                case 10:
                    u16(o, num(f, "field_1"));
                    u16(o, num(f, "field_2"));
                    bytes(o, f.at("field_3"));
                    break;
                default: throw std::runtime_error("unknown raw group 1 subtype");
            }
        } else if (group == "two") {
            start(o, 2);
            auto [sub, arr] = *v.items().begin();
            u16(o, arr.at(0).get<uint32_t>());
            const auto& f = arr.at(1);
            u16(o, num(f, "field_1"));
            if (sub == "variable") {
                str(o, f.at("string").get<std::string>());
                u16(o, num(f, "field_3"));
            }
        } else if (group == "three") {
            start(o, 3);
            u16(o, num(v, "field_1"));
            u16(o, uint32_t(v.at("field_2").size()));
            bytes(o, v.at("field_2"));
        } else if (group == "four") {
            start(o, 4);
            auto [subName, f] = *v.items().begin();
            uint32_t sub = subIndex(subName);
            u16(o, sub);
            if (sub == 0 || sub == 2) {
                u16(o, num(f, "field_1"));
                str(o, f.at("string").get<std::string>());
            } else if (sub == 1) {
                u16(o, uint32_t(f.at("field_1").size()));
                bytes(o, f.at("field_1"));
            } else {
                u16(o, num(f, "field_1"));
            }
        } else if (group == "five") {
            start(o, 5);
            u16(o, num(v, "field_1"));
            u16(o, num(v, "field_2"));
        } else if (group == "two_hundred_one") {
            start(o, 201);
            auto [sub, arr] = *v.items().begin();
            if (sub == "localisation") {
                localisationBody(o, localisationKind(arr.at(0)), arr.at(1).at("strings"));
            } else {
                u16(o, arr.at(0).get<uint32_t>());
                const auto& f = arr.at(1);
                if (sub == "dynamic") {
                    u16(o, num(f, "len"));
                    bytes(o, f.at("field_2"));
                } else {
                    u16(o, num(f, "field_1"));
                }
            }
        } else {
            throw std::runtime_error("unknown raw text control group " + group);
        }
        return;
    }
    throw std::runtime_error("empty raw text control");
}

void control(std::string& o, const json& c) {
    const auto kind = c.at("kind").get<std::string>();
    if (kind == "raw") return raw(o, c);
    if (kind == "set_colour" || kind == "reset_colour") {
        static const std::map<std::string, uint32_t> colours = {{"red", 0},         {"light_green1", 1}, {"blue", 2},       {"grey", 3},
                                                                {"light_green4", 4}, {"orange", 5},       {"light_grey", 6}};
        uint32_t v = 0xFFFF;
        if (kind == "set_colour") {
            auto it = colours.find(c.at("colour").get<std::string>());
            if (it == colours.end()) throw std::runtime_error("unknown colour");
            v = it->second;
        }
        start(o, 0);
        u16(o, 3);
        u16(o, 2);
        u16(o, v);
    } else if (kind == "font") {
        start(o, 0);
        u16(o, 1);
        u16(o, 2);
        u16(o, c.at("font_kind") == "hylian" ? 0x0000 : 0xFFFF);
    } else if (kind == "text_size") {
        start(o, 0);
        u16(o, 2);
        u16(o, 2);
        u16(o, num(c, "percent"));
    } else if (kind == "pause") {
        if (c.contains("frames")) {
            start(o, 1);
            u16(o, 0);
            u16(o, 4);
            u32(o, num(c, "frames"));
        } else {
            const auto& len = c.at("length");
            start(o, 5);
            u16(o, len == "short" ? 0 : len == "long" ? 1 : 2);
            u16(o, 0);
        }
    } else if (kind == "auto_advance") {
        start(o, 1);
        u16(o, 3);
        u16(o, 4);
        u32(o, num(c, "frames"));
    } else if (kind == "choice") {
        const auto& labels = c.at("choice_labels");
        if (labels.size() < 2 || labels.size() > 4) throw std::runtime_error("a choice needs 2 to 4 answers");
        start(o, 1);
        u16(o, uint32_t(labels.size() + 2));  // type 4, 5 or 6
        u16(o, num(c, "unknown"));
        for (const auto& l : labels) u16(o, l.get<uint32_t>());
        u8(o, num(c, "selected_index"));
        u8(o, num(c, "cancel_index"));
    } else if (kind == "single_choice") {
        start(o, 1);
        u16(o, 10);
        u16(o, 4);
        u16(o, num(c, "label"));
        u8(o, 1);
        u8(o, 0xCD);
    } else if (kind == "icon") {
        start(o, 1);
        u16(o, 7);
        u16(o, 2);
        u8(o, icon(c.at("icon")));
        u8(o, 0xCD);
    } else if (kind == "variable") {
        auto name = c.at("name").get<std::string>();
        start(o, 2);
        u16(o, num(c, "variable_kind"));
        u16(o, uint32_t(name.size() * 2 + 4));  // msyt sizes it from UTF-8 bytes
        str(o, name);
        u16(o, 0);
    } else if (kind == "sound" || kind == "sound2") {
        start(o, kind == "sound" ? 3 : 4);
        u16(o, 1);
        u16(o, uint32_t(c.at("unknown").size()));
        bytes(o, c.at("unknown"));
    } else if (kind == "animation") {
        auto name = c.at("name").get<std::string>();
        start(o, 4);
        u16(o, 2);
        u16(o, uint32_t(name.size() * 2 + 2));
        str(o, name);
    } else if (kind == "localisation") {
        start(o, 201);
        localisationBody(o, localisationKind(c.at("localisation_kind")), c.at("options"));
    } else {
        throw std::runtime_error("unknown text control " + kind);
    }
}

}  // namespace

std::string encodeContents(const json& contents) {
    std::string out;
    for (const auto& item : contents) {
        if (item.contains("text"))
            out += msbt::toUtf16(item["text"].get<std::string>());
        else if (item.contains("control"))
            control(out, item["control"]);
        else
            throw std::runtime_error("unknown text content");
    }
    return out;
}

std::string encodeAttributes(const std::string& attributes) { return msbt::toUtf16(attributes) + std::string(2, '\0'); }

}  // namespace mm::msyt
