#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mm {

namespace fs = std::filesystem;

bool iequals(std::string_view a, std::string_view b);
std::string toLower(std::string_view s);
std::string toUpper(std::string_view s);
bool isHex(std::string_view s);
bool endsWithI(std::string_view s, std::string_view suffix);

// Reduce a name to something safe as a single FAT32 path component.
std::string slugify(std::string_view s, size_t maxLen = 48);

// Split a relative generic path into components ("a/b/c" -> {a,b,c}).
std::vector<std::string> pathParts(const fs::path& p);

// True if a relative path from an archive is safe to extract (no "..", not absolute).
bool isSafeRelativePath(std::string_view p);

// Remove empty directories walking upward from `dir` until (not including) `stopAt`.
void pruneEmptyDirs(const fs::path& dir, const fs::path& stopAt);

// Copy a file, creating parent directories. Throws on failure.
void copyFileCreatingDirs(const fs::path& from, const fs::path& to);

std::string readFile(const fs::path& p);
// Writes via a temp file + rename so a power loss never leaves a half-written file.
void writeFileAtomic(const fs::path& p, std::string_view data);

}  // namespace mm
