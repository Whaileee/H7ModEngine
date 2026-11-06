// Fingerprint of the community content the game loads from the user's Documents CookedPC folder, so two players
// can tell whether they run the same mods. Packages carry no version string (their FolderName is "None"), so a
// pack's "version" is a hash: identical copies match, a different release of the same mod does not.
#include "content.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace h7::net {

static uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static constexpr uint64_t kFnvBasis = 14695981039346656037ull;

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static std::string Utf8(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

static std::string Hex(uint64_t v, int digits) {
    char buf[20];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return std::string(buf + 16 - digits);
}

GameContent ScanGameContent() {
    GameContent out;
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        out.error = "Documents folder not found";
        return out;
    }
    fs::path root = fs::path(docs) / L"My Games" / L"Might & Magic Heroes VII" / L"MMH7Game" / L"CookedPC";
    CoTaskMemFree(docs);
    out.root = Utf8(root);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out; // nothing installed: unmodded

    struct File { std::string rel; uintmax_t size; uint64_t head; };
    std::map<std::string, std::vector<File>> groups;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = Lower(Utf8(it->path().extension()));
        if (ext != ".upk" && ext != ".umap") continue;
        fs::path rel = fs::relative(it->path(), root, ec);
        std::string relS = Utf8(rel);
        if (Lower(relS).rfind("localshadercache", 0) == 0) continue;

        // mods\<entry> is one pack per folder or loose file; other top-level folders (Data, Visuals, ...) are one
        // pack each; loose top-level packages are their own pack.
        auto first = rel.begin();
        std::string group;
        std::string top = Utf8(*first);
        auto second = std::next(first);
        if (Lower(top) == "mods" && second != rel.end()) {
            fs::path entry = *second;
            group = "mods/" + Utf8(std::next(second) == rel.end() ? entry.stem() : entry);
        } else if (second == rel.end()) {
            group = Utf8(rel.stem());
        } else {
            group = top;
        }

        File f{Lower(relS), it->file_size(ec), kFnvBasis};
        char head[4096];
        std::ifstream in(it->path(), std::ios::binary);
        in.read(head, sizeof head);
        f.head = Fnv(kFnvBasis, head, (size_t)in.gcount()); // includes the package GUID and table offsets
        groups[group].push_back(f);
    }

    uint64_t all = kFnvBasis;
    for (auto& [name, files] : groups) {
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
        uint64_t h = kFnvBasis;
        for (auto& f : files) {
            h = Fnv(h, f.rel.data(), f.rel.size());
            h = Fnv(h, &f.size, sizeof f.size);
            h = Fnv(h, &f.head, sizeof f.head);
        }
        out.packs.push_back({name, (int)files.size(), Hex(h, 8)});
    }
    std::sort(out.packs.begin(), out.packs.end(), [](auto& a, auto& b) { return Lower(a.name) < Lower(b.name); });
    for (auto& p : out.packs) {
        std::string n = Lower(p.name);
        all = Fnv(all, n.data(), n.size());
        all = Fnv(all, p.version.data(), p.version.size());
    }
    if (!out.packs.empty()) out.fingerprint = Hex(all, 16);
    return out;
}

} // namespace h7::net
