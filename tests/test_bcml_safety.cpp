// BCML packages come from the internet: their logs mustn't reach outside the game's files,
// and one bad entry mustn't stop everything else from merging.

#include <algorithm>
#include <fstream>

#include "aamp.hpp"
#include "bcml.hpp"
#include "byml.hpp"
#include "sarc.hpp"
#include "sizetable.hpp"
#include "test.hpp"

using namespace mm;
using aamp::Parameter;
using aamp::Type;
using byml::Node;

namespace {

void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

std::string sarcOf(std::vector<SarcFile> files) {
    Sarc s;
    s.files = std::move(files);
    return s.write();
}

std::string emptyActorInfo() {
    byml::Document d;
    d.root = Node::makeDict();
    d.root.dict["Actors"] = Node::makeArray();
    d.root.dict["Hashes"] = Node::makeArray();
    return yaz0Compress(byml::write(d));
}

// An AAMP log listing `path` in its FileTable, with an empty change list for it.
std::string fileTableLog(const char* table, const std::string& path) {
    aamp::ParameterIO log;
    log.type = "";
    log.root.objectOrAdd(aamp::hash(table)).set(aamp::hash("File0"), Parameter::string(Type::StringRef, path));
    log.root.listOrAdd(aamp::hash(path)).objectOrAdd(aamp::hash("Thing")).set(aamp::hash("X"), Parameter::s32(1));
    return aamp::write(log);
}

bool anyNote(const bcml::Result& r, const std::string& text) {
    return std::any_of(r.notes.begin(), r.notes.end(), [&](const std::string& n) { return n.find(text) != std::string::npos; });
}

}  // namespace

TEST("bcml: only paths inside the game's romfs are accepted") {
    CHECK(bcml::isPackagePath("01007EF00011E000/romfs/Pack/Title.pack"));
    CHECK(bcml::isPackagePath("01007EF00011F001/romfs/Map/MainField/A-1/A-1_Dynamic.smubin"));
    CHECK(!bcml::isPackagePath("01007EF00011E000/romfs/"));
    CHECK(!bcml::isPackagePath("01007EF00011E000/romfs/../../../evil.pack"));
    CHECK(!bcml::isPackagePath("01007EF00011E000/romfs/Pack/..\\..\\x"));
    CHECK(!bcml::isPackagePath("../01007EF00011E000/romfs/x"));
    CHECK(!bcml::isPackagePath("/01007EF00011E000/romfs/x"));
    CHECK(!bcml::isPackagePath("Pack/Title.pack"));
    CHECK(!bcml::isPackagePath("0100F2C0115B6000/romfs/x"));
}

TEST("bcml: log paths that leave the game's files are skipped") {
    t::TempDir d;
    auto game = d.path / "game";
    const std::string title = "01007EF00011E000/romfs/Pack/Title.pack";
    put(game / title, sarcOf({{"a.txt", "game a"}}));
    put(d.path / "secret.pack", sarcOf({{"a.txt", "secret"}}));
    auto p = d.path / "p";
    auto files = p / "files";
    put(files / title, sarcOf({{"a.txt", "mod a"}}));
    put(p / "escape.pack", sarcOf({{"a.txt", "escape"}}));
    put(p / "logs/packs.json", R"({"a": ")" + title + R"(", "b": "../../secret.pack",
        "c": "01007EF00011E000/romfs/../../escape.pack", "d": "01007EF00011E000\\romfs\\..\\..\\escape.pack"})");
    put(p / "logs/drops.json", R"({"01007EF00011E000/romfs/../../../x.pack//a.bdrop": {"T": {"items": {"A": 1.0}}}})");
    put(p / "logs/deepmerge.aamp", fileTableLog("FileTable", "../../../game/" + title + "//a.bxml"));
    put(p / "logs/aslist.aamp", fileTableLog("FileTable", "/etc//a.baslist"));
    put(p / "logs/map.yml", "\"../../../../x_Dynamic\":\n  Objs:\n    add: []\n");
    bcml::FolderGameFiles reader(game);
    auto r = bcml::merge({{"p", files, {p / "logs"}}}, reader);
    CHECK_EQ(r.files.size(), 1u);
    CHECK(r.files.count(title));
    for (const auto& [path, data] : r.files) CHECK(bcml::isPackagePath(path));
    CHECK(anyNote(r, "../../secret.pack"));
    CHECK(anyNote(r, "not a path inside the game"));
    // Nothing outside is read either.
    CHECK(!reader.read("01007EF00011E000/romfs/../../secret.pack"));
    for (const auto& path : bcml::gameFilesNeeded({{"p", files, {p / "logs"}}})) CHECK(bcml::isPackagePath(path));
}

TEST("bcml: a malformed log entry is skipped; a missing game file still stops the merge") {
    t::TempDir d;
    auto game = d.path / "game";
    const std::string info = "01007EF00011E000/romfs/Actor/ActorInfo.product.sbyml";
    put(game / info, emptyActorInfo());
    auto p = d.path / "p";
    put(p / "logs/actorinfo.yml", "notahash: {instSize: 5}\n'1': {instSize: 7}\n");
    // A drop table in a file that isn't an archive, tables that aren't objects, texts that
    // aren't a language list.
    put(p / "logs/drops.json", R"({")" + info + R"(//a.bdrop": {"T": {"items": {"A": 1.0}}},
        "01007EF00011E000/romfs/Pack/Other.pack//b.bdrop": [1, 2], "x": "y"})");
    put(p / "logs/texts.json", "[1, 2, 3]");
    put(p / "logs/deepmerge.aamp", "not an aamp file");
    bcml::FolderGameFiles reader(game);
    bcml::Result r;
    bool threw = false;
    try {
        r = bcml::merge({{"p", p, {p / "logs"}}}, reader);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(r.files.count(info));  // the good actor entry still merged
    CHECK(anyNote(r, "notahash"));
    CHECK(anyNote(r, "a.bdrop"));
    CHECK(anyNote(r, "texts.json"));
    CHECK(anyNote(r, "deepmerge.aamp"));

    bcml::FolderGameFiles nothing(d.path / "nothing");
    std::string error;
    try {
        bcml::merge({{"p", p, {p / "logs"}}}, nothing);
    } catch (const std::exception& e) {
        error = e.what();
    }
    CHECK_EQ(error, "needs the game's " + info + ", which Patchwork hasn't read yet");
}

TEST("bcml: new animation defines don't take a number already used") {
    t::TempDir d;
    auto game = d.path / "game";
    const std::string pack = "01007EF00011E000/romfs/Actor/Pack/Npc.sbactorpack";
    const uint32_t asDefines = 3752287078u;
    aamp::ParameterIO aslist;
    {
        auto& defs = aslist.root.listOrAdd(asDefines);
        auto def = [&](const char* key, const char* name, const char* file) {
            auto& o = defs.objectOrAdd(aamp::hash(key));
            o.set(aamp::hash("Name"), Parameter::string(Type::String64, name));
            o.set(aamp::hash("Filename"), Parameter::string(Type::String64, file));
        };
        def("ASDefine_0", "Wait", "Npc_Wait");
        def("ASDefine_2", "Run", "Npc_Run");  // numbers needn't be contiguous
    }
    put(game / pack, yaz0Compress(sarcOf({{"Actor/ASList/Npc.baslist", aamp::write(aslist)}})));
    auto p = d.path / "p";
    {
        aamp::ParameterIO log;
        log.type = "";
        std::string path = pack + "//Actor/ASList/Npc.baslist";
        log.root.objectOrAdd(aamp::hash("FileTable")).set(aamp::hash("File0"), Parameter::string(Type::StringRef, path));
        auto& defs = log.root.listOrAdd(aamp::hash(path)).listOrAdd(asDefines);
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Name"), Parameter::string(Type::String64, "Dance"));
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Filename"), Parameter::string(Type::String64, "Mod_Dance"));
        defs.objectOrAdd(aamp::hash("ASDefine_1")).set(aamp::hash("Name"), Parameter::string(Type::String64, "Run"));
        defs.objectOrAdd(aamp::hash("ASDefine_1")).set(aamp::hash("Filename"), Parameter::string(Type::String64, "Mod_Run"));
        put(p / "logs/aslist.aamp", aamp::write(log));
    }
    bcml::FolderGameFiles reader(game);
    auto r = bcml::merge({{"p", p, {p / "logs"}}}, reader);
    auto as = aamp::parse(Sarc::parse(r.files.at(pack)).find("Actor/ASList/Npc.baslist")->data);
    const auto* defs = as.root.list(asDefines);
    CHECK_EQ(defs->objects.size(), 3u);
    CHECK_EQ(defs->object(aamp::hash("ASDefine_0"))->find("Filename")->data, std::string("Npc_Wait"));
    CHECK_EQ(defs->object(aamp::hash("ASDefine_2"))->find("Name")->data, std::string("Run"));  // still Run…
    CHECK_EQ(defs->object(aamp::hash("ASDefine_2"))->find("Filename")->data, std::string("Mod_Run"));  // …with the mod's file
    CHECK_EQ(defs->object(aamp::hash("ASDefine_3"))->find("Name")->data, std::string("Dance"));
}
