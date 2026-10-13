#include "test.hpp"
#include "yaml.hpp"

using namespace mm;

TEST("yaml: block and flow collections, tags and quoting as BCML writes them") {
    auto n = yaml::parse(R"(s32_data:
  add:
    BowPorchStockNum:
      DataName: BowPorchStockNum
      HashValue: -405912493
      IsSave: true
  del: []
'1006387283': {seriesArmorSeriesType: AmiiboOcarinaOfTime}
HatenoMini<Carry_A>: {disable_event_start_interval: true, is_pause_non_demo_member: false,
  is_timeline: false, mode: Seamless}
B-3_Static:
  add:
  - '!Parameters': {TerritoryArea: 0.0}
    HashId: !u 0xd80fef01
    Translate: [-3270.375, 224.548004, -1755.42896]
  - HashId: !u 0x1
  del: []
  mod: {}
path.sbactorpack//Actor/x.bdrop: !list
  objects: {}
  lists:
    2767637356: !list
      objects:
        1776201207: !obj {2546362036: 4, 2880596286: !str64 Normal2}
      lists: {}
)");
    const auto& add = n["s32_data"]["add"]["BowPorchStockNum"];
    CHECK_EQ(add["HashValue"].asInt(), int64_t(-405912493));
    CHECK(add["IsSave"].asBool());
    CHECK(n["s32_data"]["del"].kind == yaml::Node::Kind::Seq);
    CHECK_EQ(n["1006387283"]["seriesArmorSeriesType"].asString(), std::string("AmiiboOcarinaOfTime"));
    CHECK_EQ(n["HatenoMini<Carry_A>"].map.size(), 4u);
    CHECK_EQ(n["HatenoMini<Carry_A>"]["mode"].asString(), std::string("Seamless"));
    const auto& units = n["B-3_Static"]["add"];
    CHECK_EQ(units.items.size(), 2u);
    CHECK_EQ(units.items[0]["HashId"].tag, std::string("!u"));
    CHECK_EQ(units.items[0]["HashId"].asUInt(), uint64_t(0xd80fef01));
    CHECK_EQ(units.items[0]["Translate"].items.size(), 3u);
    CHECK(units.items[0]["!Parameters"]["TerritoryArea"].looksFloat());
    CHECK_EQ(units.items[1]["HashId"].asUInt(), uint64_t(1));
    const auto& diff = n["path.sbactorpack//Actor/x.bdrop"];
    CHECK_EQ(diff.tag, std::string("!list"));
    const auto& obj = diff["lists"]["2767637356"]["objects"]["1776201207"];
    CHECK_EQ(obj.tag, std::string("!obj"));
    CHECK_EQ(obj["2880596286"].tag, std::string("!str64"));
    CHECK_EQ(obj["2880596286"].asString(), std::string("Normal2"));
    CHECK_EQ(obj["2546362036"].asInt(), int64_t(4));
}

TEST("yaml: multi-line strings fold like YAML") {
    auto n = yaml::parse(R"(Item_Desc:
  contents:
  - text: 'Revali''s Great Eagle Bow, broken due

      to overuse. You can repair it.'
  - text: "two\tparts é"
  - text: plain words
      continued here
lit: |
  line one
  line two
fold: >-
  joined
  words
)");
    CHECK_EQ(n["Item_Desc"]["contents"].items[0]["text"].asString(),
             std::string("Revali's Great Eagle Bow, broken due\nto overuse. You can repair it."));
    CHECK_EQ(n["Item_Desc"]["contents"].items[1]["text"].asString(), std::string("two\tparts \xC3\xA9"));
    CHECK_EQ(n["Item_Desc"]["contents"].items[2]["text"].asString(), std::string("plain words continued here"));
    CHECK_EQ(n["lit"].asString(), std::string("line one\nline two\n"));
    CHECK_EQ(n["fold"].asString(), std::string("joined words"));
}

TEST("yaml: errors name the line") {
    bool threw = false;
    try {
        yaml::parse("a: 'unterminated\n");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("line") != std::string::npos;
    }
    CHECK(threw);
}
