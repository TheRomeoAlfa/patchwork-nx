#pragma once

#include <cstdint>
#include <string>

namespace mm {

// How much memory BOTW (Switch) reserves for a resource, as BCML calculates it for the size
// table: the file's unpacked size rounded up to 32, plus the per-type overhead of the game's
// resource factory. Types the factory sizes by parsing get an estimate when `guess` is set.
// `name` is the romfs name (".s" extensions allowed); `data` may be Yaz0-compressed.
// Returns 0 when there's no answer (BCML then leaves the entry alone).
uint32_t resourceSize(const std::string& name, const std::string& data, bool guess = true);

// "Actor/Pack/Foo.sbactorpack" -> "Actor/Pack/Foo.bactorpack" (size table names have no ".s").
std::string sizeTableName(const std::string& name);

}  // namespace mm
