#include "byml.hpp"
#include "test.hpp"
#include "tkmm_data.hpp"

using namespace mm;
using byml::Node;

namespace {

Node dict(std::initializer_list<std::pair<const std::string, Node>> items) {
    Node n = Node::makeDict();
    for (const auto& [k, v] : items) n.dict[k] = v;
    return n;
}

Node strs(std::initializer_list<const char*> items) {
    Node n = Node::makeArray();
    for (auto* s : items) n.array.push_back(Node::str(s));
    return n;
}

std::string file(Node root, bool bigEndian = false) {
    byml::Document d;
    d.version = 7;
    d.bigEndian = bigEndian;
    d.root = std::move(root);
    return byml::write(d);
}

Node u64(uint64_t v) {
    Node n;
    n.type = Node::Type::UInt64;
    n.u = v;
    return n;
}

}  // namespace

TEST("rsdb rows: mods change, remove and add rows by key; sorted tables stay sorted") {
    auto row = [](const char* id, int v) { return dict({{"__RowId", Node::str(id)}, {"V", Node::integer(v)}}); };
    Node rows = Node::makeArray();
    rows.array = {row("Armor_001", 1), row("Armor_002", 2), row("Armor_003", 3)};
    const std::string canonical = "RSDB/ActorInfo.Product.rstbl.byml";
    CHECK_EQ(tkmm::rsdbRowKey(canonical), std::string("__RowId"));
    CHECK_EQ(tkmm::rsdbRowKey("RSDB/Tag.Product.rstbl.byml"), std::string(""));

    Node a = dict({{"Armor_002", dict({{"V", Node::integer(20)}})}, {"Armor_003", Node::removed()}});
    Node b = dict({{"Armor_000", row("Armor_000", 0)}, {"Armor_009", row("Armor_009", 9)}});
    // A higher mod changing a row a lower one added.
    Node c = dict({{"Armor_009", dict({{"V", Node::integer(99)}})}});
    auto merged = byml::parse(tkmm::mergeRsdbRows(canonical, "__RowId", file(rows), {file(a), file(b), file(c)})).root;
    CHECK_EQ(merged.array.size(), size_t(4));
    CHECK_EQ(merged.array[0].find("__RowId")->s, std::string("Armor_000"));  // sorted
    CHECK_EQ(merged.array[2].find("V")->i, int64_t(20));
    CHECK_EQ(merged.array[3].find("__RowId")->s, std::string("Armor_009"));
    CHECK_EQ(merged.array[3].find("V")->i, int64_t(99));
    // Removed by one mod and changed by a higher one: it stays.
    Node keep = dict({{"Armor_003", dict({{"V", Node::integer(30)}})}});
    merged = byml::parse(tkmm::mergeRsdbRows(canonical, "__RowId", file(rows), {file(a), file(keep)})).root;
    CHECK_EQ(merged.array.size(), size_t(3));

    // Hash-keyed tables, in the game's own order: kept, new rows at the end.
    auto hrow = [](uint32_t h) { return dict({{"NameHash", Node::uinteger(h)}, {"On", Node::boolean(false)}}); };
    Node hrows = Node::makeArray();
    hrows.array = {hrow(30), hrow(10), hrow(20)};
    Node hc;
    hc.type = Node::Type::HashMap32;
    hc.hashes[10] = dict({{"On", Node::boolean(true)}});
    hc.hashes[5] = hrow(5);
    merged = byml::parse(tkmm::mergeRsdbRows("RSDB/GameSafetySetting.Product.rstbl.byml", "NameHash", file(hrows), {file(hc)})).root;
    CHECK_EQ(merged.array.size(), size_t(4));
    CHECK_EQ(merged.array[0].find("NameHash")->u, 30u);
    CHECK(merged.array[1].find("On")->b);
    CHECK_EQ(merged.array[3].find("NameHash")->u, 5u);
}

TEST("rsdb tags: tags added, entries given and losing tags, bit table rebuilt") {
    // Two entries, tags "A" and "B": entry 0 has A, entry 1 has A and B.
    Node table = dict({{"PathList", strs({"Work/Actor/", "Armor_001", ".engine__actor__ActorParam.gyml", "Work/Actor/", "Armor_002",
                                          ".engine__actor__ActorParam.gyml"})},
                       {"TagList", strs({"A", "B"})}});
    Node bits;
    bits.type = Node::Type::Binary;
    bits.s = std::string(1, char(0b1101));  // entry 0: bit 0 (A); entry 1: bits 2, 3 (A, B)
    table.dict["BitTable"] = bits;
    Node rank = bits;
    rank.s = "rank";
    table.dict["RankTable"] = rank;
    auto back = byml::parse(tkmm::mergeRsdbTags(file(table), {})).root;
    CHECK(back == table);

    Node removeB;
    removeB.type = Node::Type::ArrayChangelog;
    byml::ArrayChange rm;
    rm.change = Node::Change::Remove;
    rm.node = Node::str("B");
    rm.key.type = rm.key2.type = Node::Type::None;
    removeB.changes = {rm};
    Node changelog = dict({{"Tags", strs({"C"})},
                           {"Entries", Node::makeArray()}});
    auto& e = changelog.dict["Entries"].array;
    for (auto* s : {"Work/Actor/", "Armor_002", ".engine__actor__ActorParam.gyml"}) e.push_back(Node::str(s));
    e.push_back(removeB);
    for (auto* s : {"Work/Actor/", "Armor_000", ".engine__actor__ActorParam.gyml"}) e.push_back(Node::str(s));
    e.push_back(strs({"C", "A"}));
    auto merged = byml::parse(tkmm::mergeRsdbTags(file(table), {file(changelog)})).root;
    // Entries sorted (Armor_000 first), tags A, B, C.
    CHECK(merged.find("TagList")->array.size() == 3 && merged.find("TagList")->array[2].s == "C");
    CHECK_EQ(merged.find("PathList")->array[1].s, std::string("Armor_000"));
    // Armor_000: A, C (bits 0, 2); Armor_001: A (bit 3); Armor_002: A (bit 6).
    const auto& b = merged.find("BitTable")->s;
    CHECK_EQ(b.size(), size_t(2));
    CHECK_EQ(int(uint8_t(b[0])), 0b01001101);
    CHECK_EQ(int(uint8_t(b[1])), 0);
    CHECK(merged.find("RankTable")->s == "rank");
}

TEST("game data: rows by hash, rows two mods add merged, save data sizes worked out again") {
    auto flag = [](uint32_t h, bool v, int file) {
        return dict({{"Hash", Node::uinteger(h)}, {"DefaultValue", Node::boolean(v)}, {"SaveFileIndex", Node::integer(file)}});
    };
    Node boolTable = Node::makeArray();
    boolTable.array = {flag(10, false, 0), flag(20, false, -1)};
    Node intTable = Node::makeArray();
    intTable.array = {dict({{"Hash", u64(7)}, {"DefaultValue", Node::integer(0)}, {"SaveFileIndex", Node::integer(0)}})};
    Node meta = dict({{"SaveDirectory", strs({"slot", ""})}});
    Node root = dict({{"Data", dict({{"Bool", boolTable}, {"Int64", intTable}})}, {"MetaData", meta}});
    tkmm::saveDataMetadata(root.dict["MetaData"], *root.find("Data"));
    auto vanilla = file(root, true);
    CHECK_EQ(root.find("MetaData")->find("AllDataSaveOffset")->i, int64_t(0x130 + 3 * 8));

    Node a = Node::makeDict(), b = Node::makeDict();
    a.dict["Bool"].type = Node::Type::HashMap32;
    a.dict["Bool"].hashes[10] = dict({{"DefaultValue", Node::boolean(true)}});
    a.dict["Bool"].hashes[15] = flag(15, false, 0);  // new
    a.dict["Int64"].type = Node::Type::HashMap64;
    a.dict["Int64"].hashes[7] = Node::removed();
    b.dict["Bool"].type = Node::Type::HashMap32;
    b.dict["Bool"].hashes[15] = flag(15, true, 0);  // the same new flag, another value
    auto merged = byml::parse(tkmm::mergeGameData("GameData/GameDataList.Product.byml", vanilla, {file(a), file(b)}));
    CHECK(merged.bigEndian);
    const auto& rows = merged.root.find("Data")->find("Bool")->array;
    CHECK_EQ(rows.size(), size_t(3));
    CHECK_EQ(rows[1].find("Hash")->u, 15u);   // sorted by hash
    CHECK(rows[0].find("DefaultValue")->b);   // A's change
    CHECK(rows[1].find("DefaultValue")->b);   // B's value for the flag both add
    CHECK(merged.root.find("Data")->find("Int64")->array.empty());
    // One more flag saved, one Int64 fewer: 8 bytes of offsets more and less.
    const auto* m = merged.root.find("MetaData");
    CHECK_EQ(m->find("AllDataSaveOffset")->i, int64_t(0x130 + 3 * 8));
    CHECK_EQ(m->find("SaveDataOffsetPos")->array[0].i, int64_t(0x130 + 2 * 8));
}
