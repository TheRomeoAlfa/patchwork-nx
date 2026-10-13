#pragma once

#include <functional>
#include <string>

namespace app {

// A measurement run for full BCML support (merging packages into the game's own archives on
// the console). It downloads two small BCML packages that only need pack merging, merges
// them with the game's files the way BCML does, and times each step and the memory used.
// It also opens some of the game's biggest archives to see how long that takes. Nothing is
// installed; the merged title screen can be tried in the game separately.
//
// Needs Patchwork opened by holding L and R while starting Breath of the Wild. Runs on the
// file thread; `progress` gets a step name and 0..1. Returns the report (also saved to
// switch/patchwork/bcml-test/report.txt).
std::string runBcmlTest(const std::function<void(const std::string& step, float fraction)>& progress);

// The merged Title.pack from the last run, for trying in the game.
bool bcmlTitleReady();
bool bcmlTitleInGame();
// Copies it to atmosphere/contents/…/Pack/Title.pack. Throws if a file is already there.
void putBcmlTitleInGame();
// Removes it again (only if it's still the test's file).
void removeBcmlTitleFromGame();

}  // namespace app
