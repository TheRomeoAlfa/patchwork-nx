#include "byml_merge.hpp"
#include "test.hpp"

using namespace mm;
using byml::Node;

namespace {

Node dict(std::initializer_list<std::pair<const std::string, Node>> items) {
    Node n = Node::makeDict();
    for (const auto& [k, v] : items) n.dict[k] = v;
    return n;
}

Node array(std::initializer_list<Node> items) {
    Node n = Node::makeArray();
    n.array = items;
    return n;
}

Node i(int v) { return Node::integer(v); }

// A mod's file as a changelog against the game's, then all of them merged onto the game's.
Node mergeMods(const std::string& canonical, const Node& vanilla, std::vector<Node> mods) {
    for (auto& m : mods) byml::logChanges(byml::fileType(canonical), m, vanilla);
    std::vector<const Node*> ptrs;
    for (const auto& m : mods) ptrs.push_back(&m);
    Node out = vanilla;
    byml::merge(canonical, out, ptrs);
    return out;
}

}  // namespace

TEST("byml merge: two mods changing different keys of the same file both apply") {
    Node vanilla = dict({{"A", i(1)}, {"B", i(2)}, {"C", dict({{"X", i(1)}, {"Y", i(2)}})}});
    Node mod1 = dict({{"A", i(5)}, {"C", dict({{"X", i(1)}, {"Y", i(2)}})}});  // A changed, B removed
    Node mod2 = dict({{"A", i(1)}, {"B", i(2)}, {"C", dict({{"X", i(1)}, {"Y", i(3)}})}, {"D", Node::str("new")}});
    Node out = mergeMods("Foo/Bar.game__x__Thing.bgyml", vanilla, {mod1, mod2});
    CHECK(out == dict({{"A", i(5)}, {"C", dict({{"X", i(1)}, {"Y", i(3)}})}, {"D", Node::str("new")}}));
    // The higher mod wins a key both change.
    Node mod3 = dict({{"A", i(9)}, {"B", i(2)}, {"C", dict({{"X", i(1)}, {"Y", i(2)}})}});
    CHECK(mergeMods("x.bgyml", vanilla, {mod1, mod3}).find("A")->i == 9);
    // An unchanged copy makes no changelog.
    Node same = vanilla;
    CHECK(byml::logChanges("", same, vanilla));
}

TEST("byml merge: plain lists keep every mod's additions and removals") {
    Node vanilla = dict({{"L", array({i(1), i(2), i(3)})}});
    Node add = dict({{"L", array({i(1), i(2), i(3), i(4)})}});
    Node drop = dict({{"L", array({i(1), i(3)})}});
    CHECK(mergeMods("x.bgyml", vanilla, {add, drop}) == dict({{"L", array({i(1), i(3), i(4)})}}));
    CHECK(mergeMods("x.bgyml", vanilla, {drop, add}) == dict({{"L", array({i(1), i(3), i(4)})}}));
}

TEST("byml merge: keyed lists find items by key, and merge items several mods add") {
    auto actor = [](int hash, int v, const char* extra = nullptr) {
        Node a = dict({{"Hash", Node::uinteger(uint32_t(hash))}, {"V", i(v)}});
        if (extra) a.dict[extra] = Node::boolean(true);
        return a;
    };
    const std::string map = "Banc/MainField/Test.bcett.byml";  // "Actors" by "Hash" in .bcett files
    Node vanilla = dict({{"Actors", array({actor(1, 0), actor(2, 0)})}});
    Node edit = dict({{"Actors", array({actor(1, 0), actor(2, 5)})}});
    Node addA = dict({{"Actors", array({actor(1, 0), actor(2, 0), actor(3, 0, "FromA")})}});
    Node addB = dict({{"Actors", array({actor(1, 0), actor(2, 0), actor(3, 0, "FromB")})}});
    Node out = mergeMods(map, vanilla, {edit, addA, addB});
    const auto& actors = out.find("Actors")->array;
    CHECK_EQ(actors.size(), size_t(3));
    CHECK_EQ(actors[1].find("V")->i, int64_t(5));
    // The two additions of Hash 3 became one item: as TKMM, the later one compared with the
    // first, so a field only the first has counts as removed.
    CHECK(actors[2].find("FromB") && !actors[2].find("FromA"));
    // The changelog names keyed edits by key.
    Node cl = edit;
    byml::logChanges("bcett", cl, vanilla);
    const auto& changes = cl.find("Actors")->changes;
    CHECK_EQ(changes.size(), size_t(1));
    CHECK(changes[0].change == Node::Change::Edit && changes[0].key.u == 2u);
}

TEST("byml merge: hash maps, removals and files keep their byte order") {
    Node vanilla;
    vanilla.type = Node::Type::HashMap32;
    vanilla.hashes[1] = i(1);
    vanilla.hashes[2] = i(2);
    Node mod = vanilla;
    mod.hashes.erase(1);
    mod.hashes[3] = i(3);
    Node out = mergeMods("x.byml", vanilla, {mod});
    CHECK(out == mod);

    byml::Document doc;
    doc.version = 7;
    doc.bigEndian = true;
    doc.root = dict({{"A", i(1)}});
    byml::Document modDoc = doc;
    modDoc.root.dict["A"] = i(2);
    auto cl = byml::buildChangelog("GameData/X.byml", byml::write(modDoc), byml::write(doc));
    CHECK(cl.has_value());
    auto merged = byml::parse(byml::mergeFiles("GameData/X.byml", byml::write(doc), {*cl}));
    CHECK(merged.bigEndian && merged.version == 7);
    CHECK_EQ(merged.root.find("A")->i, int64_t(2));
    CHECK(!byml::buildChangelog("GameData/X.byml", byml::write(doc), byml::write(doc)));
}

TEST("byml merge: types and rules") {
    CHECK_EQ(byml::fileType("Component/X/Foo.game__component__ArmyManagerParam.bgyml"), std::string("game__component__ArmyManagerParam"));
    CHECK_EQ(byml::fileType("Banc/Map.bcett.byml"), std::string("bcett"));
    CHECK_EQ(byml::fileType("Plain.byml"), std::string(""));
    CHECK(byml::arrayRule("Actors", "bcett", 1).primary == "Hash");
    CHECK(byml::arrayRule("Actors", "other", 1).kind == byml::ArrayRule::Kind::Plain);
    CHECK(byml::arrayRule("Translate", "", 1).kind == byml::ArrayRule::Kind::DirectIndex);
    CHECK(byml::arrayRule("ShootableActorSettings", "", 1).secondary == "Actor");
    CHECK(byml::arrayRule("Rails", "", 1).kind == byml::ArrayRule::Kind::Plain);
}
