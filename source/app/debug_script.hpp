#pragma once
// Desktop-only automation for development: drive the UI with scripted button presses
// and save screenshots, so screens can be checked without a Switch.
//
//   MM_DEBUG_SCRIPT="w2000 sbrowse kdown w800 sdetails ka w1500 sinstall q"
//   MM_SHOT_DIR=/tmp/shots
//
// Steps: w<ms> wait · f<label_text> focus the row with that label · t<x>,<y> tap · k<button> press (up down left right a b x y l r zl zr plus minus)
//        h<button>+<button>…,<ms> hold buttons (as seen by code polling the controller state)
//        s<name> screenshot to $MM_SHOT_DIR/<name>.png · q quit

namespace app::debug {

void startScriptFromEnv();
// Call after each Application::mainLoop() iteration.
void afterFrame();
// True while the script holds this button (always false outside desktop scripts).
bool isHeld(int button);

}  // namespace app::debug
