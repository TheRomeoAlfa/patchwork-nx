#include <fstream>

#include "layout.hpp"
#include "test.hpp"

using namespace mm;

static void touch(const fs::path& p, const std::string& content = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

static std::vector<std::string> targets(const Variant& v) {
    std::vector<std::string> out;
    for (const auto& f : v.files) out.push_back(f.target);
    return out;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

TEST("layout: emulator-style romfs + exefs ips folder") {
    t::TempDir d;
    touch(d.path / "MyMod/romfs/Actor/Pack/Foo.pack");
    touch(d.path / "MyMod/exefs/ABCDEF0123456789ABCDEF0123456789.ips");
    touch(d.path / "MyMod/exefs/patch.pchtxt");
    auto r = detectLayout(d.path, Game::Totk, "my-mod");
    CHECK(r.kind == PackageKind::Installable);
    CHECK_EQ(r.variants.size(), 1u);
    auto t = targets(r.variants[0]);
    CHECK_EQ(t.size(), 2u);
    CHECK(has(t, "atmosphere/contents/0100F2C0115B6000/romfs/Actor/Pack/Foo.pack"));
    CHECK(has(t, "atmosphere/exefs_patches/my-mod/ABCDEF0123456789ABCDEF0123456789.ips"));
    CHECK_EQ(r.variants[0].name, std::string("MyMod"));
    CHECK_EQ(r.warnings.size(), 1u);  // pchtxt skipped
}

TEST("layout: full atmosphere tree, system files ignored") {
    t::TempDir d;
    touch(d.path / "atmosphere/contents/0100f2c0115b6000/romfs/a.txt");
    touch(d.path / "atmosphere/exefs_patches/ultracam/0011223344556677.ips");
    touch(d.path / "atmosphere/config/system_settings.ini");
    touch(d.path / "atmosphere/contents/0100000000001000/romfs/evil");  // a system title
    auto r = detectLayout(d.path, Game::Totk, "x");
    CHECK(r.kind == PackageKind::Installable);
    CHECK_EQ(r.variants.size(), 1u);
    auto t = targets(r.variants[0]);
    CHECK_EQ(t.size(), 2u);
    CHECK(has(t, "atmosphere/contents/0100F2C0115B6000/romfs/a.txt"));
    CHECK(has(t, "atmosphere/exefs_patches/ultracam/0011223344556677.ips"));
    CHECK_EQ(r.variants[0].name, std::string("Main"));
    CHECK_EQ(r.warnings.size(), 2u);
}

TEST("layout: titles/ (old atmosphere) and bare title id folder") {
    t::TempDir d;
    touch(d.path / "pack/titles/01007EF00011E000/romfs/Pack/Bootup.pack");
    auto r = detectLayout(d.path, Game::Botw, "x");
    CHECK_EQ(r.variants.size(), 1u);
    CHECK(has(targets(r.variants[0]), "atmosphere/contents/01007EF00011E000/romfs/Pack/Bootup.pack"));

    t::TempDir d2;
    touch(d2.path / "01007EF00011E000/romfs/a");
    touch(d2.path / "01007EF00011F001/romfs/b");  // BOTW DLC title
    auto r2 = detectLayout(d2.path, Game::Botw, "x");
    CHECK_EQ(r2.variants.size(), 1u);
    CHECK_EQ(r2.variants[0].files.size(), 2u);
}

TEST("layout: option folders become separate variants") {
    t::TempDir d;
    touch(d.path / "Pick One/60fps/romfs/a");
    touch(d.path / "Pick One/30fps/romfs/a");
    touch(d.path / "readme.txt");
    auto r = detectLayout(d.path, Game::Totk, "x");
    CHECK_EQ(r.variants.size(), 2u);
    CHECK_EQ(r.variants[0].name, std::string("Pick One/30fps"));
    CHECK_EQ(r.variants[1].name, std::string("Pick One/60fps"));
}

TEST("layout: nested archive folder names are cleaned up") {
    t::TempDir d;
    touch(d.path / "Option A.zip.extracted/romfs/a");
    auto r = detectLayout(d.path, Game::Totk, "x");
    CHECK_EQ(r.variants.size(), 1u);
    CHECK_EQ(r.variants[0].name, std::string("Option A"));
}

TEST("layout: loose ips patches") {
    t::TempDir d;
    touch(d.path / "UltraCam/0123456789abcdef0123456789abcdef.ips");
    auto r = detectLayout(d.path, Game::Totk, "ultra-cam");
    CHECK_EQ(r.variants.size(), 1u);
    CHECK(has(targets(r.variants[0]), "atmosphere/exefs_patches/ultra-cam/0123456789ABCDEF0123456789ABCDEF.ips"));
}

TEST("layout: unsupported packages") {
    t::TempDir a;
    touch(a.path / "Linkle.bnp");
    CHECK(detectLayout(a.path, Game::Botw, "x").kind == PackageKind::Bcml);

    t::TempDir b;
    touch(b.path / "mod/Thing.tkcl");
    CHECK(detectLayout(b.path, Game::Totk, "x").kind == PackageKind::Tkmm);

    t::TempDir c;
    touch(c.path / "Mod/rules.txt");
    touch(c.path / "Mod/content/Pack/x.pack");
    CHECK(detectLayout(c.path, Game::Botw, "x").kind == PackageKind::WiiU);

    t::TempDir e;
    touch(e.path / "0100F2C0115B6000/romfs/a");  // TOTK mod opened as BOTW
    auto r = detectLayout(e.path, Game::Botw, "x");
    CHECK(r.kind == PackageKind::Empty);
    CHECK_EQ(r.warnings.size(), 1u);
}

TEST("layout: emulator mod folders inside a title id folder") {
    t::TempDir d;
    touch(d.path / "0100F2C0115B6000/Faster Horses/romfs/a");
    touch(d.path / "0100F2C0115B6000/Slower Horses/romfs/b");
    auto r = detectLayout(d.path, Game::Totk, "x");
    CHECK(r.kind == PackageKind::Installable);
    CHECK_EQ(r.variants.size(), 2u);
    if (r.variants.size() == 2) {
        CHECK_EQ(r.variants[0].name, std::string("Faster Horses"));
        CHECK(has(targets(r.variants[0]), "atmosphere/contents/0100F2C0115B6000/romfs/a"));
    }
}

TEST("layout: a patch directly in exefs_patches gets its own folder") {
    t::TempDir d;
    touch(d.path / "atmosphere/exefs_patches/0011223344556677.ips");
    auto r = detectLayout(d.path, Game::Totk, "my-mod");
    CHECK_EQ(r.variants.size(), 1u);
    if (!r.variants.empty()) CHECK(has(targets(r.variants[0]), "atmosphere/exefs_patches/my-mod/0011223344556677.ips"));
}
