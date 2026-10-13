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

#include "library.hpp"

TEST("bcml: the library deploys merged files instead of a package's partial packs") {
    t::TempDir d;
    auto sd = d.path / "sd", data = d.path / "sd/switch/patchwork", game = d.path / "game";
    std::string titleLayout = sarcOf({{"blyt/Title.bflyt", "game layout"}, {"timg/__Combined.bntx", "game textures"}});
    put(game / "01007EF00011E000/romfs/Pack/Title.pack", sarcOf({{"Layout/Title.sblarc", yaz0Compress(titleLayout)}}));
    // The game's size table, so size entries have something to merge into.
    SizeTable table;
    table.crc[sizeTableCrc("Layout/Title.blarc")] = 100;
    put(data / "original/botw/ResourceSizeTable.product.srsizetable", yaz0Store(table.write()));

    auto src = d.path / "pkg";
    std::string partial = sarcOf({{"Layout/Title.sblarc", yaz0Compress(sarcOf({{"timg/__Combined.bntx", "linkle textures"}}))}});
    put(src / "Title.pack", partial);
    put(src / "logs/packs.json", R"({"Pack/Title.pack": "01007EF00011E000/romfs/Pack/Title.pack"})");

    Library lib(sd, data);
    lib.load();
    lib.setOriginalSizeTable(data / "original/botw/ResourceSizeTable.product.srsizetable");
    bcml::FolderGameFiles files(game);
    lib.setGameFileReader([&](const std::string& p) {
        auto f = files.read(p);
        return f ? Library::GameFile{Library::GameFile::State::Found, *f} : Library::GameFile{Library::GameFile::State::Absent, {}};
    });
    CHECK(lib.missingGameFiles({src / "logs"}).empty());

    ModRecord r;
    r.id = "linkle-title";
    r.game = Game::Botw;
    r.name = "Linkle title";
    r.bcmlLogs = {"0"};
    const std::string target = "atmosphere/contents/01007EF00011E000/romfs/Pack/Title.pack";
    lib.add(r, {{src / "Title.pack", target}}, true, {}, {{src / "logs/packs.json", "bcml/0/packs.json"}});

    auto onCard = readFile(sd / target);
    CHECK(onCard != partial);
    auto merged = Sarc::parse(Sarc::parse(onCard).find("Layout/Title.sblarc")->data);
    CHECK(merged.find("blyt/Title.bflyt")->data == "game layout");
    CHECK(merged.find("timg/__Combined.bntx")->data == "linkle textures");
    CHECK(!lib.bcmlPending());
    // The size table got the merged layout's entry (it only grows).
    auto sdTable = SizeTable::parse(readFile(sd / kBotwSizeTable));
    CHECK(sdTable.crc.at(sizeTableCrc("Layout/Title.blarc")) >= 100u);
    // Logs are kept with the mod, not copied to the SD card.
    CHECK(fs::exists(data / "library/botw/linkle-title/bcml/0/packs.json"));
    CHECK(!fs::exists(sd / "bcml"));

    lib.setEnabled("linkle-title", false);
    CHECK(!fs::exists(sd / target));
    CHECK(!fs::exists(data / "bcml-merged"));
    lib.setEnabled("linkle-title", true);
    CHECK(fs::exists(sd / target));

    // Without the game's files the merge waits, and says what it needs.
    Library lib2(sd, data);
    lib2.load();
    lib2.setGameFileReader([](const std::string&) { return Library::GameFile{}; });
    CHECK_EQ(lib2.missingGameFiles({src / "logs"}).size(), 1u);
}

namespace {
using aamp::Parameter;
using aamp::Type;

std::string bdrop(const std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>>& tables) {
    aamp::ParameterIO pio;
    auto& h = pio.root.objectOrAdd(aamp::hash("Header"));
    h.set(aamp::hash("TableNum"), Parameter::s32(int32_t(tables.size())));
    for (size_t i = 0; i < tables.size(); ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "Table%02zu", i + 1);
        h.set(aamp::hash(key), Parameter::string(Type::String64, tables[i].first));
        auto& o = pio.root.objectOrAdd(aamp::hash(tables[i].first));
        for (const char* k : {"RepeatNumMin", "RepeatNumMax", "ApproachType", "OccurrenceSpeedType"}) o.set(aamp::hash(k), Parameter::s32(1));
        o.set(aamp::hash("ColumnNum"), Parameter::s32(int32_t(tables[i].second.size())));
        for (size_t k = 0; k < tables[i].second.size(); ++k) {
            char n[32], p[32];
            std::snprintf(n, sizeof n, "ItemName%02zu", k + 1);
            std::snprintf(p, sizeof p, "ItemProbability%02zu", k + 1);
            o.set(aamp::hash(n), Parameter::string(Type::String64, tables[i].second[k].first));
            o.set(aamp::hash(p), Parameter::f32(tables[i].second[k].second));
        }
    }
    return aamp::write(pio);
}

std::string bshop(const std::vector<std::pair<std::string, std::vector<std::string>>>& tables) {
    aamp::ParameterIO pio;
    auto& h = pio.root.objectOrAdd(aamp::hash("Header"));
    h.set(aamp::hash("TableNum"), Parameter::s32(int32_t(tables.size())));
    for (size_t i = 0; i < tables.size(); ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "Table%02zu", i + 1);
        h.set(aamp::hash(key), Parameter::string(Type::String64, tables[i].first));
        auto& o = pio.root.objectOrAdd(aamp::hash(tables[i].first));
        o.set(aamp::hash("ColumnNum"), Parameter::s32(int32_t(tables[i].second.size())));
        for (size_t k = 0; k < tables[i].second.size(); ++k) {
            auto key3 = [&](const char* base) {
                char b[32];
                std::snprintf(b, sizeof b, "%s%03zu", base, k + 1);
                return aamp::hash(b);
            };
            o.set(key3("ItemSort"), Parameter::s32(int32_t(k)));
            o.set(key3("ItemName"), Parameter::string(Type::String64, tables[i].second[k]));
            o.set(key3("ItemNum"), Parameter::s32(1));
            o.set(key3("ItemAdjustPrice"), Parameter::s32(0));
            o.set(key3("ItemLookGetFlg"), Parameter::boolean(false));
            o.set(key3("ItemAmount"), Parameter::s32(0));
        }
    }
    return aamp::write(pio);
}
}  // namespace

TEST("bcml: drop tables, shops and animation lists") {
    t::TempDir d;
    auto game = d.path / "game";
    const std::string pack = "01007EF00011E000/romfs/Actor/Pack/Npc_Shop.sbactorpack";
    aamp::ParameterIO aslist;
    {
        auto& defs = aslist.root.listOrAdd(3752287078u);  // ASDefines
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Name"), Parameter::string(Type::String64, "Wait"));
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Filename"), Parameter::string(Type::String64, "Npc_Wait"));
        auto& res = aslist.root.listOrAdd(2777926231u);  // AddReses
        res.objectOrAdd(aamp::hash("AddRes_0")).set(aamp::hash("Anim"), Parameter::string(Type::String64, "Npc_Base"));
    }
    put(game / pack, yaz0Compress(sarcOf({{"Actor/DropTable/Npc_Shop.bdrop", bdrop({{"Normal", {{"Item_Apple", 50}, {"Item_Ore_A", 50}}}, {"Rare", {{"Item_Ore_B", 100}}}})},
                                          {"Actor/ShopData/Npc_Shop.bshop", bshop({{"Normal", {"Item_Apple", "Item_Meat"}}})},
                                          {"Actor/ASList/Npc_Shop.baslist", aamp::write(aslist)}})));
    auto p = d.path / "p";
    // Drops: Normal changed (an item gone, one added), Rare dropped, Extra new.
    put(p / "logs/drops.json", R"({")" + pack + R"(//Actor/DropTable/Npc_Shop.bdrop": {
        "Normal": {"repeat_num_min": 2, "repeat_num_max": 3, "approach_type": 0, "occurrence_speed_type": 0,
                   "items": {"Item_Apple": 30.0, "Item_Star": 70.0}},
        "Extra": {"repeat_num_min": 1, "repeat_num_max": 1, "approach_type": 0, "occurrence_speed_type": 0, "items": {"Item_Gem": 100.0}}}})");
    // Shop: Meat removed, Fish added.
    {
        aamp::ParameterIO log;
        log.type = "";
        std::string path = pack + "//Actor/ShopData/Npc_Shop.bshop";
        log.root.objectOrAdd(aamp::hash("Filenames")).set(aamp::hash(path), Parameter::string(Type::StringRef, path));
        auto& diff = log.root.listOrAdd(aamp::hash(path));
        auto& add = diff.listOrAdd(aamp::hash("Additions"));
        auto& addTable = add.listOrAdd(aamp::hash("Normal"));
        auto& fish = addTable.objectOrAdd(aamp::hash("Item_Fish"));
        fish.set(aamp::hash("ItemName"), Parameter::string(Type::String64, "Item_Fish"));
        fish.set(aamp::hash("ItemNum"), Parameter::s32(3));
        fish.set(aamp::hash("ItemAdjustPrice"), Parameter::s32(5));
        fish.set(aamp::hash("ItemLookGetFlg"), Parameter::boolean(false));
        fish.set(aamp::hash("ItemAmount"), Parameter::s32(0));
        diff.listOrAdd(aamp::hash("Removals")).listOrAdd(aamp::hash("Normal")).objectOrAdd(aamp::hash("Item_Meat"));
        put(p / "logs/shop.aamp", aamp::write(log));
    }
    // AS list: Wait's file changed, a new define, a new animation resource.
    {
        aamp::ParameterIO log;
        log.type = "";
        std::string path = pack + "//Actor/ASList/Npc_Shop.baslist";
        log.root.objectOrAdd(aamp::hash("FileTable")).set(aamp::hash("File0"), Parameter::string(Type::StringRef, path));
        auto& diff = log.root.listOrAdd(aamp::hash(path));
        auto& defs = diff.listOrAdd(3752287078u);
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Name"), Parameter::string(Type::String64, "Wait"));
        defs.objectOrAdd(aamp::hash("ASDefine_0")).set(aamp::hash("Filename"), Parameter::string(Type::String64, "Linkle_Wait"));
        defs.objectOrAdd(aamp::hash("ASDefine_1")).set(aamp::hash("Name"), Parameter::string(Type::String64, "Dance"));
        defs.objectOrAdd(aamp::hash("ASDefine_1")).set(aamp::hash("Filename"), Parameter::string(Type::String64, "Linkle_Dance"));
        diff.listOrAdd(2777926231u).objectOrAdd(aamp::hash("AddRes_0")).set(aamp::hash("Anim"), Parameter::string(Type::String64, "Linkle_Anim"));
        put(p / "logs/aslist.aamp", aamp::write(log));
    }
    bcml::FolderGameFiles files(game);
    auto r = bcml::merge({{"p", p, {p / "logs"}}}, files);
    auto out = Sarc::parse(r.files.at(pack));

    auto drops = aamp::parse(out.find("Actor/DropTable/Npc_Shop.bdrop")->data);
    const auto* header = drops.root.object(aamp::hash("Header"));
    CHECK_EQ(header->find("TableNum")->asInt(), 2);
    CHECK_EQ(header->find("Table01")->data, std::string("Normal"));  // the game's order, Rare gone
    CHECK_EQ(header->find("Table02")->data, std::string("Extra"));
    const auto* normal = drops.root.object(aamp::hash("Normal"));
    CHECK_EQ(normal->find("ColumnNum")->asInt(), 2);
    CHECK_EQ(normal->find("ItemName01")->data, std::string("Item_Apple"));
    CHECK_EQ(normal->find("ItemProbability01")->asF32(), 30.0f);
    CHECK_EQ(normal->find("ItemName02")->data, std::string("Item_Star"));
    CHECK_EQ(normal->find("RepeatNumMax")->asInt(), 3);

    auto shop = aamp::parse(out.find("Actor/ShopData/Npc_Shop.bshop")->data);
    const auto* table = shop.root.object(aamp::hash("Normal"));
    CHECK_EQ(table->find("ColumnNum")->asInt(), 2);
    CHECK_EQ(table->find("ItemName001")->data, std::string("Item_Apple"));
    CHECK_EQ(table->find("ItemName002")->data, std::string("Item_Fish"));
    CHECK_EQ(table->find("ItemNum002")->asInt(), 3);
    CHECK_EQ(table->find("ItemSort002")->asInt(), 1);

    auto as = aamp::parse(out.find("Actor/ASList/Npc_Shop.baslist")->data);
    const auto* defs = as.root.list(3752287078u);
    CHECK_EQ(defs->objects.size(), 2u);
    CHECK_EQ(defs->object(aamp::hash("ASDefine_0"))->find("Filename")->data, std::string("Linkle_Wait"));
    CHECK_EQ(defs->object(aamp::hash("ASDefine_1"))->find("Name")->data, std::string("Dance"));
    const auto* res = as.root.list(2777926231u);
    CHECK_EQ(res->objects.size(), 2u);  // Npc_Base kept, Linkle_Anim added
    CHECK(r.sizes.count("Actor/ShopData/Npc_Shop.bshop"));
}

#include "msbt.hpp"

TEST("bcml: text changes go into the game's language, controls encoded") {
    t::TempDir d;
    auto game = d.path / "game";
    msbt::File item;
    item.hasAttributes = true;
    item.attrSize = 4;
    item.attrStrings = true;
    msbt::Message apple;
    apple.text = msbt::toUtf16("Apple");
    apple.attribute = msbt::toUtf16("Item") + std::string(2, '\0');
    item.set("Item_Apple_Name", apple);
    msbt::Message ore;
    ore.text = msbt::toUtf16("Ore");
    ore.attribute = msbt::toUtf16("Item") + std::string(2, '\0');
    item.set("Item_Ore_Name", ore);
    std::string msgs = sarcOf({{"ActorType/Item.msbt", msbt::write(item)}});
    put(game / "01007EF00011E000/romfs/Pack/Bootup_EUen.pack", sarcOf({{"Message/Msg_EUen.product.ssarc", yaz0Compress(msgs)}}));

    auto p = d.path / "p";
    put(p / "logs/texts.json", R"({"USen": {
      "ActorType/Item.msyt": {
        "Item_Apple_Name": {"attributes": "Item", "contents": [{"text": "Linkle's "}, {"control": {"kind": "set_colour", "colour": "red"}}, {"text": "Apple"}]},
        "Item_New_Name": {"attributes": "Item", "contents": [{"text": "Gem"}]}
      },
      "EventFlowMsg/Npc_New.msyt": {"Talk00": {"contents": [{"text": "Hi!"}, {"control": {"kind": "pause", "length": "long"}}]}}
    }})");
    bcml::FolderGameFiles files(game);
    bcml::Options opt;
    opt.language = "EUen";  // a UK console: the package's US English is used
    std::vector<bcml::Package> pkgs = {{"p", p, {p / "logs"}}};
    CHECK(bcml::gameFilesNeeded(pkgs, opt).count("01007EF00011E000/romfs/Pack/Bootup_EUen.pack"));
    auto r = bcml::merge(pkgs, files, opt);
    auto pack = Sarc::parse(r.files.at("01007EF00011E000/romfs/Pack/Bootup_EUen.pack"));
    auto msgSarc = Sarc::parse(pack.find("Message/Msg_EUen.product.ssarc")->data);
    auto merged = msbt::parse(msgSarc.find("ActorType/Item.msbt")->data);
    CHECK_EQ(merged.messages.size(), 3u);
    CHECK_EQ(merged.messages[0].first, std::string("Item_Apple_Name"));  // stays in place
    CHECK(merged.find("Item_Apple_Name")->text.find(std::string("\x0E\x00\x00\x00\x03\x00", 6)) != std::string::npos);
    CHECK_EQ(msbt::toUtf8(merged.find("Item_Ore_Name")->text), std::string("Ore"));  // untouched
    CHECK_EQ(msbt::toUtf8(merged.find("Item_New_Name")->attribute.substr(0, 8)), std::string("Item"));
    auto talk = msbt::parse(msgSarc.find("EventFlowMsg/Npc_New.msbt")->data);
    CHECK_EQ(talk.messages.size(), 1u);
    CHECK_EQ(talk.attrSize, 0u);
    CHECK_EQ(r.sizes.at("Message/Msg_EUen.product.sarc"), 0u);  // dropped from the size table
}

TEST("bcml: map units merge by HashId, with and without the DLC") {
    auto obj = [](uint32_t id, const std::string& name) {
        Node o = Node::makeDict();
        o.dict["HashId"] = Node::uinteger(id);
        o.dict["UnitConfigName"] = Node::str(name);
        return o;
    };
    auto unitOf = [](std::vector<Node> objs) {
        Node root = Node::makeDict();
        root.dict["Objs"] = arrayOf(std::move(objs));
        root.dict["Rails"] = Node::makeArray();
        return yaz0Compress(bymlOf(root));
    };
    auto mapYml = R"(A-1_Static:
  Objs:
    add:
    - {HashId: !u 0x5, UnitConfigName: Anvil}
    mod:
      '2': {HashId: !u 0x2, UnitConfigName: TreeChanged}
    del: [!u 0x3]
  Rails: {add: [], mod: {}, del: []}
A-1_Dynamic:
  Objs:
    add:
    - {HashId: !u 0x9, UnitConfigName: Horse}
    mod: {}
    del: []
)";
    for (bool dlc : {false, true}) {
        t::TempDir d;
        auto game = d.path / "game";
        std::string staticUnit = unitOf({obj(1, "Rock"), obj(2, "Tree"), obj(3, "Bush")});
        put(game / "01007EF00011E000/romfs/Pack/TitleBG.pack",
            sarcOf({{"Map/MainField/A-1/A-1_Static.smubin", staticUnit}, {"Actor/Pack/Other.sbactorpack", "other"}}));
        put(game / "01007EF00011E000/romfs/Map/MainField/A-1/A-1_Dynamic.smubin", unitOf({obj(7, "Enemy")}));
        if (dlc) {
            put(game / "01007EF00011F001/romfs/Pack/AocMainField.pack",
                sarcOf({{"Map/MainField/A-1/A-1_Static.smubin", unitOf({obj(1, "Rock"), obj(2, "Tree"), obj(3, "Bush"), obj(4, "DlcThing")})}}));
            put(game / "01007EF00011F001/romfs/Map/MainField/A-1/A-1_Dynamic.smubin", unitOf({obj(7, "Enemy"), obj(8, "DlcEnemy")}));
        }
        auto p = d.path / "p";
        put(p / "logs/map.yml", mapYml);
        bcml::FolderGameFiles files(game);
        auto r = bcml::merge({{"p", p, {p / "logs"}}}, files);
        std::string root = dlc ? "01007EF00011F001/romfs/" : "01007EF00011E000/romfs/";
        auto st = byml::parse(r.files.at(root + "Map/MainField/A-1/A-1_Static.smubin")).root.dict.at("Objs").array;
        std::vector<std::string> names;
        for (const auto& o : st) names.push_back(o.dict.at("UnitConfigName").s);
        std::vector<std::string> want = dlc ? std::vector<std::string>{"Rock", "TreeChanged", "DlcThing", "Anvil"}
                                            : std::vector<std::string>{"Rock", "TreeChanged", "Anvil"};
        CHECK(names == want);  // sorted by HashId, Bush deleted
        auto dyn = byml::parse(r.files.at(root + "Map/MainField/A-1/A-1_Dynamic.smubin")).root.dict.at("Objs").array;
        CHECK_EQ(dyn.size(), dlc ? 3u : 2u);
        // TitleBG.pack keeps everything but the static unit, so the loose one is read.
        auto title = Sarc::parse(r.files.at("01007EF00011E000/romfs/Pack/TitleBG.pack"));
        CHECK(!title.find("Map/MainField/A-1/A-1_Static.smubin"));
        CHECK(title.find("Actor/Pack/Other.sbactorpack"));
        CHECK_EQ(r.files.count("01007EF00011F001/romfs/Pack/AocMainField.pack"), dlc ? 1u : 0u);
        CHECK(r.sizes.count(dlc ? "Aoc/0010/Map/MainField/A-1/A-1_Static.mubin" : "Map/MainField/A-1/A-1_Static.mubin"));
    }
}
