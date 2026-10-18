#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace mm::msyt {

// BCML logs text changes (texts.json) in msyt's JSON form: per message an optional
// "attributes" string and "contents", a list of {"text": …} and {"control": {"kind": …}}
// items (pauses, colours, icons, choices, variables…). These turn one into the UTF-16
// bytes the game reads (Switch byte order), byte for byte as msyt writes them.

// The message's TXT2 bytes, without the terminating 0x0000. Throws on controls it can't
// encode.
std::string encodeContents(const nlohmann::json& contents);

// The ATR1 string for "attributes" (UTF-16 with its terminator).
std::string encodeAttributes(const std::string& attributes);

}  // namespace mm::msyt
