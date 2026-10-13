#include <fstream>

#include "aamp.hpp"
#include "bcml.hpp"
#include "byml.hpp"
#include "sarc.hpp"
#include "sizetable.hpp"
#include "test.hpp"

using namespace mm;
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

std::string bymlOf(Node root) {
    byml::Document d;
    d.root = std::move(root);
    return byml::write(d);
}

Node entry(const std::string& name, int32_t hash, int32_t init) {
    Node e = Node::makeDict();
    e.dict["DataName"] = Node::str(name);
    e.dict["HashValue"] = Node::integer(hash);
    e.dict["InitValue"] = Node::integer(init);
    return e;
}

Node dictWith(const std::string& key, Node value) {
    Node d = Node::makeDict();
    d.dict[key] = std::move(value);
    return d;
}

Node arrayOf(std::vector<Node> items) {
    Node a = Node::makeArray();
    a.array = std::move(items);
    return a;
}

std::string linkFile(const std::string& model) {
    aamp::ParameterIO pio;
    auto& obj = pio.root.objectOrAdd(aamp::hash("LinkTarget"));
    obj.set(aamp::hash("ModelUser"), aamp::Parameter::string(aamp::Type::String64, model));
    obj.set(aamp::hash("SlinkUser"), aamp::Parameter::string(aamp::Type::String64, "Dummy"));
    return aamp::write(pio);
}

std::string saveFile(std::vector<Node> entries) {
    Node header = Node::makeDict();
    header.dict["file_name"] = Node::str("game_data.sav");
    Node root = Node::makeDict();
    root.dict["file_list"] = arrayOf({header, arrayOf(std::move(entries))});
    return bymlOf(root);
}

}  // namespace

TEST("bcml: packs, deep merge, flags, save data, events and actor info together") {
    t::TempDir d;
    auto game = d.path / "game";
    auto content = game / "01007EF00011E000" / "romfs";
    // The game's files.
    std::string gamedata = sarcOf({{"/bool_data_0.bgdata", bymlOf(dictWith("bool_data", arrayOf({entry("FlagA", 1, 0), entry("FlagB", 2, 0)})))},
                                   {"/s32_data_0.bgdata", bymlOf(dictWith("s32_data", arrayOf({entry("BowPorchStockNum", 3, 5)})))}});
    std::vector<SarcFile> saves;
    for (int i = 0; i < 6; ++i) saves.push_back({"/saveformat_" + std::to_string(i) + ".bgsvdata", saveFile(i == 0 ? std::vector<Node>{entry("FlagA", 1, 0)} : std::vector<Node>{})});
    saves.push_back({"/saveformat_6.bgsvdata", "caption"});
    saves.push_back({"/saveformat_7.bgsvdata", "option"});
    Node events = Node::makeDict();
    events.dict["EvA"] = dictWith("mode", Node::str("Normal"));
    put(content / "Pack" / "Bootup.pack", sarcOf({{"GameData/gamedata.ssarc", yaz0Compress(gamedata)},
                                                  {"GameData/savedataformat.ssarc", yaz0Compress(sarcOf(saves))},
                                                  {"Event/EventInfo.product.sbyml", yaz0Compress(bymlOf(events))}}));
    put(content / "Actor" / "Pack" / "Foo.sbactorpack",
        yaz0Compress(sarcOf({{"Actor/ActorLink/Foo.bxml", linkFile("Foo")}, {"Actor/ModelList/Foo.bmodellist", "models"}})));
    std::string titleLayout = sarcOf({{"blyt/Title.bflyt", "game layout"}, {"timg/__Combined.bntx", "game textures"}});
    put(content / "Pack" / "Title.pack", sarcOf({{"Layout/Title.sblarc", yaz0Compress(titleLayout)}, {"Sound/x.bfsar", "sound"}}));
    Node foo = Node::makeDict();
    foo.dict["name"] = Node::str("Foo");
    foo.dict["instSize"] = Node::integer(100);
    Node actorInfo = Node::makeDict();
    actorInfo.dict["Actors"] = arrayOf({foo});
    actorInfo.dict["Hashes"] = arrayOf({Node::uinteger(aamp::hash("Foo"))});
    put(content / "Actor" / "ActorInfo.product.sbyml", yaz0Compress(bymlOf(actorInfo)));

    // Package 1 (lower): new title textures, a changed link, a flag's initial value.
    auto p1 = d.path / "p1";
    put(p1 / "01007EF00011E000/romfs/Pack/Title.pack",
        sarcOf({{"Layout/Title.sblarc", yaz0Compress(sarcOf({{"timg/__Combined.bntx", "linkle textures"}}))}}));
    put(p1 / "logs/packs.json", R"({"Pack/Title.pack": "01007EF00011E000\\romfs\\Pack\\Title.pack"})");
    put(p1 / "logs/gamedata.yml", R"(s32_data:
  add:
    BowPorchStockNum: {DataName: BowPorchStockNum, HashValue: 3, InitValue: 20}
  del: []
bool_data:
  add: {}
  del: [FlagB]
)");
    {
        aamp::ParameterIO log;
        log.type = "";
        std::string path = "01007EF00011E000/romfs/Actor/Pack/Foo.sbactorpack//Actor/ActorLink/Foo.bxml";
        log.root.objectOrAdd(aamp::hash("FileTable")).set(aamp::hash("File0"), aamp::Parameter::string(aamp::Type::StringRef, path));
        auto& diff = log.root.listOrAdd(aamp::hash(path));
        diff.objectOrAdd(aamp::hash("LinkTarget")).set(aamp::hash("ModelUser"), aamp::Parameter::string(aamp::Type::String64, "Linkle"));
        put(p1 / "logs/deepmerge.aamp", aamp::write(log));
    }
    // Package 2 (higher): actor info, an event, a save flag, and the same flag set differently.
    auto p2 = d.path / "p2";
    put(p2 / "logs/actorinfo.yml", "'" + std::to_string(aamp::hash("Foo")) + "': {instSize: 200, itemBuyingPrice: 50}\n'" +
                                       std::to_string(aamp::hash("Bar")) + "': {name: Bar, instSize: 10}\n");
    put(p2 / "logs/eventinfo.yml", "EvB: {mode: Seamless}\n");
    put(p2 / "logs/savedata.yml", "add:\n- {DataName: NewFlag, HashValue: 77}\ndel: []\n");
    put(p2 / "logs/gamedata.yml", "s32_data:\n  add:\n    BowPorchStockNum: {DataName: BowPorchStockNum, HashValue: 3, InitValue: 30}\n  del: []\n");

    std::vector<bcml::Package> pkgs = {{"p1", p1, {p1 / "logs"}}, {"p2", p2, {p2 / "logs"}}};
    auto needed = bcml::gameFilesNeeded(pkgs);
    CHECK(needed.count("01007EF00011E000/romfs/Pack/Bootup.pack"));
    CHECK(needed.count("01007EF00011E000/romfs/Pack/Title.pack"));
    CHECK(needed.count("01007EF00011E000/romfs/Actor/ActorInfo.product.sbyml"));

    bcml::FolderGameFiles files(game);
    auto r = bcml::merge(pkgs, files);

    // Title.pack: the game's layout with the mod's textures.
    auto title = Sarc::parse(r.files.at("01007EF00011E000/romfs/Pack/Title.pack"));
    auto layout = Sarc::parse(title.find("Layout/Title.sblarc")->data);
    CHECK(layout.find("blyt/Title.bflyt")->data == "game layout");
    CHECK(layout.find("timg/__Combined.bntx")->data == "linkle textures");
    CHECK(title.find("Sound/x.bfsar")->data == "sound");

    // The actor pack: the link merged, the rest kept.
    auto fooPack = Sarc::parse(r.files.at("01007EF00011E000/romfs/Actor/Pack/Foo.sbactorpack"));
    auto link = aamp::parse(fooPack.find("Actor/ActorLink/Foo.bxml")->data);
    CHECK_EQ(link.root.object(aamp::hash("LinkTarget"))->find("ModelUser")->data, std::string("Linkle"));
    CHECK_EQ(link.root.object(aamp::hash("LinkTarget"))->find("SlinkUser")->data, std::string("Dummy"));
    CHECK(fooPack.find("Actor/ModelList/Foo.bmodellist")->data == "models");

    // Bootup.pack: flags, save data and events.
    auto bootup = Sarc::parse(r.files.at("01007EF00011E000/romfs/Pack/Bootup.pack"));
    auto gd = Sarc::parse(bootup.find("GameData/gamedata.ssarc")->data);
    auto s32 = byml::parse(gd.find("/s32_data_0.bgdata")->data).root.dict.at("s32_data");
    CHECK_EQ(s32.array.at(0).dict.at("InitValue").i, int64_t(30));  // the higher package wins
    auto bools = byml::parse(gd.find("/bool_data_0.bgdata")->data).root.dict.at("bool_data");
    CHECK_EQ(bools.array.size(), 1u);  // FlagB deleted
    auto sv = Sarc::parse(bootup.find("GameData/savedataformat.ssarc")->data);
    CHECK_EQ(sv.files.size(), 3u);  // one chunk + the two other save files
    auto svList = byml::parse(sv.find("/saveformat_0.bgsvdata")->data).root.dict.at("file_list").array.at(1).array;
    CHECK_EQ(svList.size(), 2u);
    CHECK(sv.find("/saveformat_2.bgsvdata")->data == "option");
    auto ev = byml::parse(bootup.find("Event/EventInfo.product.sbyml")->data).root;
    CHECK(ev.find("EvA") && ev.find("EvB"));

    // ActorInfo: Foo changed, Bar added, sorted by hash with matching hashes.
    auto info = byml::parse(r.files.at("01007EF00011E000/romfs/Actor/ActorInfo.product.sbyml")).root;
    CHECK_EQ(info.dict.at("Actors").array.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        const auto& a = info.dict.at("Actors").array[i];
        const auto& h = info.dict.at("Hashes").array[i];
        uint32_t hv = h.type == Node::Type::UInt ? uint32_t(h.u) : uint32_t(h.i);
        CHECK_EQ(hv, aamp::hash(a.dict.at("name").s));
        if (a.dict.at("name").s == "Foo") {
            CHECK_EQ(a.dict.at("instSize").i, int64_t(200));
            CHECK_EQ(a.dict.at("itemBuyingPrice").i, int64_t(50));
        }
    }

    // Size table entries for what changed.
    CHECK(r.sizes.count("Actor/ActorLink/Foo.bxml"));
    CHECK(r.sizes.count("Actor/Pack/Foo.bactorpack"));
    CHECK(r.sizes.count("GameData/gamedata.sarc"));
    CHECK(r.sizes.count("Layout/Title.blarc"));
    CHECK(!r.sizes.count("Pack/Title.pack"));                 // packs never get one
    CHECK(!r.sizes.count("Actor/ModelList/Foo.bmodellist"));  // unchanged
    CHECK(!r.sizes.count("Actor/ActorInfo.product.byml"));
}

TEST("bcml: a missing game file is named") {
    t::TempDir d;
    auto p = d.path / "p";
    put(p / "logs/actorinfo.yml", "'1': {instSize: 5}\n");
    bcml::FolderGameFiles files(d.path / "nothing");
    std::string error;
    try {
        bcml::merge({{"p", p, {p / "logs"}}}, files);
    } catch (const std::exception& e) {
        error = e.what();
    }
    CHECK(error.find("ActorInfo") != std::string::npos);
}
