#pragma once

#include <string>
#include <vector>

#include "http.hpp"

namespace mm {

// The file names inside a 7z archive on a web server, read without downloading it: a 7z
// keeps its table of contents at the end, pointed to by the first 32 bytes, so a few small
// range requests are enough (usually under a few KB in total). Throws if the file isn't a
// 7z or the server doesn't support ranges.
std::vector<std::string> remote7zListing(HttpClient& http, const std::string& url);

// The same, from pieces already fetched (offset -> bytes) of a file of `totalSize` bytes.
std::vector<std::string> list7zFromPieces(const std::vector<std::pair<uint64_t, std::string>>& pieces, uint64_t totalSize);

}  // namespace mm
