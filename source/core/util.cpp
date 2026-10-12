#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mm {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string toUpper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool isHex(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool endsWithI(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::string slugify(std::string_view s, size_t maxLen) {
    std::string out;
    bool lastDash = false;
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '.') {
            out += c;
            lastDash = false;
        } else if (!lastDash && !out.empty()) {
            out += '-';
            lastDash = true;
        }
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
    return out.empty() ? "mod" : out;
}

std::vector<std::string> pathParts(const fs::path& p) {
    std::vector<std::string> parts;
    for (const auto& part : p) {
        auto s = part.generic_string();
        if (!s.empty() && s != "/" && s != ".") parts.push_back(s);
    }
    return parts;
}

std::string fatSafeName(std::string_view name) {
    std::string out(name);
    for (auto& c : out)
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20)
            c = '_';
    // FAT drops trailing dots and spaces, which would make two names collide.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.back() = '_';
    return out.empty() ? "_" : out;
}

bool isSafeRelativePath(std::string_view p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\') return false;
    if (p.find(':') != std::string_view::npos) return false;  // "C:" or "sdmc:"
    std::string norm(p);
    for (auto& c : norm)
        if (c == '\\') c = '/';
    std::stringstream ss(norm);
    std::string part;
    while (std::getline(ss, part, '/'))
        if (part == "..") return false;
    return true;
}

bool removeTree(const fs::path& p, std::string* error) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (st.type() == fs::file_type::not_found) return true;
    if (ec) {
        if (error) *error = "can't access " + p.string() + ": " + ec.message();
        return false;
    }
    std::vector<fs::path> files, dirs;
    if (st.type() == fs::file_type::directory) {
        for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec))
                dirs.push_back(it->path());
            else
                files.push_back(it->path());
        }
        if (ec) {
            if (error) *error = "can't read " + p.string() + ": " + ec.message();
            return false;
        }
        dirs.push_back(p);
    } else {
        files.push_back(p);
    }
    bool ok = true;
    auto fail = [&](const fs::path& what, const std::error_code& e) {
        if (ok && error) *error = "can't delete " + what.string() + ": " + e.message();
        ok = false;
    };
    for (const auto& f : files)
        if (!fs::remove(f, ec) && ec) fail(f, ec);
    // Deepest directories first.
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (const auto& d : dirs)
        if (!fs::remove(d, ec) && ec) fail(d, ec);
    return ok;
}

void pruneEmptyDirs(const fs::path& dir, const fs::path& stopAt) {
    std::error_code ec;
    auto cur = dir;
    auto stop = stopAt.lexically_normal();
    while (!cur.empty() && cur.lexically_normal() != stop) {
        if (!fs::is_directory(cur, ec) || !fs::is_empty(cur, ec)) break;
        fs::remove(cur, ec);
        if (ec) break;
        cur = cur.parent_path();
    }
}

void copyFileCreatingDirs(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

int64_t stableId(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return static_cast<int64_t>(h & 0x7fffffffffffffffull) | 1;
}

std::string urlDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFileAtomic(const fs::path& p, std::string_view data) {
    fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) throw std::runtime_error("write failed " + tmp.string());
    }
    // Rename over the old file in one step where the filesystem allows it, so there is never
    // a moment without a state file. FAT on the Switch refuses; then remove + rename, and
    // load() falls back to the .tmp file if power is lost in between.
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p);
    }
}

}  // namespace mm
