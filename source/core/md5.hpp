#pragma once

#include <string>

#include "util.hpp"

namespace mm {

// MD5 of a file as 32 lower-case hex characters (GameBanana publishes one per download).
// Throws if the file can't be read.
std::string md5File(const fs::path& path);
std::string md5String(const std::string& data);

}  // namespace mm
