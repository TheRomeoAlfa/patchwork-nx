#include "rstbcalc.hpp"

#include <cmath>
#include <map>
#include <set>

#include "sizetable.hpp"

namespace mm {

namespace {

struct Factory {
    uint32_t size;
    bool complex;  // sized by parsing: no fixed answer
    uint32_t parse = 0;
};

// The game's resource factories (Switch columns), from leoetlino's rstb tables via BCML.
const std::map<std::string, Factory>& factories() {
    static const std::map<std::string, Factory> f = {
        {"*", {0x38, false}},        {"sarc", {0x68, false}},       {"pack", {0x68, false}},       {"bactorpack", {0x68, false}},
        {"bmodelsh", {0x68, false}}, {"beventpack", {0x68, false}}, {"stera", {0x68, false}},      {"stats", {0x68, false}},
        {"bfres", {0x1a8, true}},    {"bcamanim", {0x50, true}},    {"batpl", {0x40, false}},      {"bnfprl", {0x40, false}},
        {"bplacement", {0x48, false}}, {"hks", {0x38, false}},      {"lua", {0x38, false}},        {"bactcapt", {0x538, false}},
        {"bitemico", {0x60, false}}, {"jpg", {0x80, false}},        {"bmaptex", {0x60, false}},    {"bstftex", {0x60, false}},
        {"bmapopen", {0x60, false}}, {"breviewtex", {0x60, false}}, {"bgdata", {0x140, false}},    {"bgsvdata", {0x38, false}},
        {"hknm2", {0x48, true}},     {"bmscdef", {0x2a8, true}},    {"bars", {0xb0, true}},        {"bxml", {0x778, true}},
        {"bgparamlist", {0x2c0, true}}, {"bmodellist", {0x7d0, true}}, {"baslist", {0x410, true}}, {"baiprog", {0x448, true}},
        {"bphysics", {0x470, true}}, {"bchemical", {0x3c0, true}},  {"bas", {0x3c8, true}},        {"batcllist", {0x3f0, true}},
        {"batcl", {0x428, true}},    {"baischedule", {0x2b8, false}}, {"bdmgparam", {0x11d0, false, 0x790}},
        {"brgconfiglist", {0x3d0, true}}, {"brgconfig", {0x42d8, false}}, {"brgbw", {0x2c0, true}}, {"bawareness", {0xb38, false}},
        {"bdrop", {0x320, true}},    {"bshop", {0x320, true}},      {"brecipe", {0x320, true}},    {"blod", {0x3c0, false}},
        {"bbonectrl", {0x8d0, true}}, {"blifecondition", {0x4b0, true}}, {"bumii", {0x2b8, false}}, {"baniminfo", {0x2c8, true}},
        {"byaml", {0x20, false}},    {"bassetting", {0x260, true}}, {"hkrb", {0x20, false}},       {"hkrg", {0x20, false}},
        {"bphyssb", {0x5b0, true}},  {"hkcl", {0xe8, true}},        {"hksc", {0x140, true}},       {"hktmrb", {0x48, true}},
        {"brgcon", {0x48, true}},    {"esetlist", {0x38, false}},   {"bdemo", {0xb20, false}},     {"bfevfl", {0x40, false}},
        {"bfevtm", {0x40, false}},
    };
    return f;
}

std::string extension(const std::string& name) {
    auto slash = name.rfind('/');
    auto dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return name.substr(dot);
}

double tableLookup(double s, std::initializer_list<std::pair<double, double>> steps) {
    double mult = 0;
    for (const auto& [from, m] : steps)
        if (s >= from) mult = m;
    return mult;
}

uint32_t guessBfres(size_t len, const std::string& name) {
    double s = std::floor(len * 1.05);
    double mult;
    if (name.find(".Tex") != std::string::npos)
        mult = tableLookup(s, {{0, 2.0}, {10000, 1.5}, {30000, 1.3}, {50000, 1.2}});
    else
        mult = tableLookup(s, {{0, 9.5}, {1250, 6.0}, {2500, 4.25}, {50000, 3.66}, {100000, 3.5}, {800000, 3.15}, {2000000, 2.5},
                               {3000000, 1.667}, {4000000, 1.6}});
    return uint32_t(s * mult);
}

uint32_t guessAamp(size_t len, const std::string& ext) {
    double s = len * 1.05;
    if (ext == ".bas") s *= 1.05;
    s = std::floor(s);
    double ret = 0;
    if (ext == ".baiprog")
        ret = s * tableLookup(s, {{0, 7.0}, {380, 6.0}, {400, 5.5}, {450, 5.0}, {600, 4.0}, {1000, 3.5}, {1750, 3.0}});
    else if (ext == ".bas")
        ret = s * tableLookup(s, {{0, 20}, {100, 12.5}, {200, 10}, {300, 8}, {600, 6}, {1500, 5.5}, {2000, 5}, {15000, 4.5}});
    else if (ext == ".baslist")
        ret = s * tableLookup(s, {{0, 15}, {100, 10}, {200, 8}, {300, 6}, {500, 5}, {800, 4}, {4000, 3.5}});
    else if (ext == ".bdrop")
        ret = s * tableLookup(s, {{0, 8.5}, {200, 7}, {250, 6}, {350, 5.25}, {450, 4.5}, {850, 4}});
    else if (ext == ".bgparamlist")
        ret = s * tableLookup(s, {{0, 20}, {100, 12}, {150, 10}, {250, 8}, {350, 7}, {450, 6}});
    else if (ext == ".brecipe")
        ret = s * tableLookup(s, {{0, 12.5}, {100, 8.5}, {160, 7.5}, {200, 7}, {215, 6.5}});
    else if (ext == ".bshop")
        ret = s * tableLookup(s, {{0, 7.25}, {200, 6}, {400, 5}, {500, 4.05}});
    else if (ext == ".bxml")
        ret = s * tableLookup(s, {{0, 6}, {350, 5}, {450, 4.5}, {550, 4}, {650, 3.5}, {800, 3}});
    else if (ext == ".bdmgparam")
        ret = ((-0.0018 * s) + 6.6273) * s + 500;
    else if (ext == ".bphysics")
        ret = double(((uint64_t(s) + 32) & ~uint64_t(31)) + 0x4E + 0x324) * std::max(4 * std::floor(s / 1388), 3.0);
    return uint32_t(ret * 1.5);
}

bool isAampExt(const std::string& ext) {
    static const std::set<std::string> exts = {".baiprog", ".bas", ".baslist", ".bdrop", ".bgparamlist", ".brecipe", ".bshop",
                                               ".bxml", ".bdmgparam", ".bphysics"};
    return exts.count(ext) > 0;
}

}  // namespace

std::string sizeTableName(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '.' && i + 1 < name.size() && name[i + 1] == 's') {
            out += '.';
            ++i;
            continue;
        }
        out += name[i];
    }
    return out;
}

uint32_t resourceSize(const std::string& name, const std::string& raw, bool guess) {
    std::string data;
    try {
        data = isYaz0(raw) ? yaz0Decompress(raw) : raw;
    } catch (const std::exception&) {
        return 0;
    }
    auto canon = sizeTableName(name);
    auto ext = extension(canon);
    size_t len = data.size();
    uint64_t rounded = (uint64_t(len) + 31) & ~uint64_t(31);
    const auto& f = factories();
    auto it = f.find(ext.empty() ? "*" : ext.substr(1));
    if (it == f.end()) it = f.find("*");
    uint64_t size = it->second.complex ? 0 : rounded + 0x168 + it->second.size + it->second.parse;
    // (BCML's code adds 100 for .baischedule, but its logs on real packages don't show it.)
    if (ext == ".bdmgparam") size = 0;
    if (ext == ".hkrb") size += 40;
    if (ext == ".baniminfo") {
        size = uint64_t(double(rounded) * (len > 36864 ? 1.5 : 4) + 0xE4 + 0x24C);
        size = uint64_t(double(size) * 1.5);
    }
    if (size == 0 && guess) {
        if (isAampExt(ext))
            size = guessAamp(len, ext);
        else if (ext == ".bfres")
            size = guessBfres(len, canon);
    }
    return uint32_t(size);
}

}  // namespace mm
