#include "archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mm {

bool isSupportedArchiveName(const std::string& filename) {
    return endsWithI(filename, ".zip") || endsWithI(filename, ".7z") || endsWithI(filename, ".rar");
}

void extractArchive(const fs::path& archivePath, const fs::path& destDir, const ExtractLimits& limits,
                    const ExtractProgressFn& progress) {
    std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), archive_read_free);
    archive_read_support_filter_all(a.get());
    archive_read_support_format_zip(a.get());
    archive_read_support_format_7zip(a.get());
    archive_read_support_format_rar(a.get());
    archive_read_support_format_rar5(a.get());
    archive_read_support_format_tar(a.get());

    if (archive_read_open_filename(a.get(), archivePath.string().c_str(), 1 << 16) != ARCHIVE_OK)
        throw std::runtime_error(std::string("cannot open archive: ") + archive_error_string(a.get()));

    fs::create_directories(destDir);
    std::error_code sizeEc;
    uint64_t archiveSize = fs::file_size(archivePath, sizeEc);
    uint64_t total = 0;
    uint32_t entries = 0;
    std::vector<char> buf(1 << 17);
    archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (++entries > limits.maxEntries) throw std::runtime_error("archive has too many entries");
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name) name = archive_entry_pathname(entry);
        if (!name) continue;
        std::string rel(name);
        for (auto& c : rel)
            if (c == '\\') c = '/';
        auto type = archive_entry_filetype(entry);
        if (!isSafeRelativePath(rel) || (type != AE_IFREG && type != AE_IFDIR)) {
            archive_read_data_skip(a.get());
            continue;
        }
        auto out = destDir / fs::path(rel);
        if (type == AE_IFDIR) {
            fs::create_directories(out);
            continue;
        }
        // macOS zip metadata, never mod content
        if (rel.rfind("__MACOSX/", 0) == 0 || out.filename().string().rfind("._", 0) == 0) {
            archive_read_data_skip(a.get());
            continue;
        }
        fs::create_directories(out.parent_path());
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        for (;;) {
            auto n = archive_read_data(a.get(), buf.data(), buf.size());
            if (n == 0) break;
            if (n < 0) throw std::runtime_error(std::string("archive read error: ") + archive_error_string(a.get()));
            total += static_cast<uint64_t>(n);
            if (total > limits.maxTotalBytes) throw std::runtime_error("archive unpacks to more than the size limit");
            f.write(buf.data(), n);
            if (!f) throw std::runtime_error("write failed (SD card full?) " + out.string());
        }
        if (progress) {
            auto read = static_cast<uint64_t>(std::max<la_int64_t>(0, archive_filter_bytes(a.get(), -1)));
            if (!progress(std::min(read, archiveSize), archiveSize, rel)) throw std::runtime_error("cancelled");
        }
    }
    if (r != ARCHIVE_EOF) throw std::runtime_error(std::string("archive error: ") + archive_error_string(a.get()));
}

void extractNestedArchives(const fs::path& dir, const ExtractLimits& limits) {
    std::vector<fs::path> nested;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && isSupportedArchiveName(e.path().filename().string())) nested.push_back(e.path());
    for (const auto& p : nested) {
        auto out = p;
        out += ".extracted";
        try {
            extractArchive(p, out, limits);
        } catch (const std::exception&) {
            // Not fatal: the outer archive may still contain usable files.
            removeTree(out);
        }
    }
}

}  // namespace mm
