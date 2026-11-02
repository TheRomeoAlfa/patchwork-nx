#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "byml.hpp"

namespace mm::byml {

// TKMM's BYML merging, after TkSharp (MIT). A changelog is the mod's file with everything
// the game's copy already has taken out: maps keep only changed keys (removed ones marked
// Node::removed()), arrays become ArrayChangelog nodes (add, edit or remove by index, or by
// a key field for the arrays TKMM knows, like "Actors" by "Hash" in .bcett files).

// How TKMM tells an array's items apart, by the array's key in its map, the file's type
// ("bcett", "game__component__ArmyManagerParam": the second-to-last extension) and depth.
struct ArrayRule {
    enum class Kind { Plain, Keyed, NameHash, DirectIndex } kind = Kind::Plain;
    std::string primary, secondary;  // Keyed: the key fields (dotted paths allowed)
};
ArrayRule arrayRule(std::string_view key, std::string_view type, int depth);

// "Foo.game__component__X.bgyml" -> "game__component__X"; "Map.bcett.byml" -> "bcett".
std::string fileType(std::string_view canonical);

// Turn `src` into a changelog against `vanilla`, in place. True when nothing changed.
bool logChanges(std::string_view type, Node& src, const Node& vanilla, int depth = 0);

// Apply changelogs (lowest priority first) to a copy of the game's tree.
void merge(std::string_view canonical, Node& base, const std::vector<const Node*>& changelogs);

// The same on files: `vanilla` and the changelogs are BYML files; the result keeps the game
// file's byte order and version. Throws std::runtime_error when a changelog doesn't fit.
std::string mergeFiles(std::string_view canonical, const std::string& vanilla, const std::vector<std::string_view>& changelogs);

// For the mergers built on this one (RSDB tables, game data): changes to parts of one tree,
// with removals and additions applied together at the end, as TKMM's BymlMergeTracking.
class MergeSession {
public:
    // `type`: the file type for TKMM's list rules ("Struct" for game data's Struct table);
    // empty: from `canonical`, as for a whole file.
    MergeSession(std::string_view canonical, Node* root, std::string type = {});
    ~MergeSession();
    void mergeMap(Node& base, const Node& changes);  // a map with a map changelog
    void merge(Node& base, const Node& changelog);   // any container with its changelog
    void apply();                                    // once, after all merges

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A changelog file for a mod's copy of a game file; nullopt when it's the same as the game's.
std::optional<std::string> buildChangelog(std::string_view canonical, const std::string& modFile, const std::string& vanilla);

}  // namespace mm::byml
