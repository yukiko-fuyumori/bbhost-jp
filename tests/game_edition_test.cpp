// Edition admission is enforced before plugins, including separate updates.
#undef NDEBUG
#include "core/config.h"
#include "core/game_edition.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
struct Entry { std::string key, value; unsigned format = 0x0204; };
void put(std::string& s, unsigned value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) s += char(value >> (8 * i));
}
std::vector<Entry> metadata(const char* title, unsigned region) {
    std::string r; put(r, region, 4);
    return {{"TITLE_ID", std::string(title) + '\0'}, {"USER_DEFINED_PARAM_1", r, 0x0404},
            {"APP_VER", std::string("01.09\0", 6)}, {"CATEGORY", std::string("gd\0", 3)}};
}
void write_sfo(const fs::path& folder, const std::vector<Entry>& values) {
    std::string head("\0PSF", 4), entries, keys, data;
    for (const auto& v : values) {
        put(entries, keys.size(), 2); put(entries, v.format, 2);
        put(entries, v.value.size(), 4); put(entries, v.value.size(), 4);
        put(entries, data.size(), 4);
        keys += v.key + '\0'; data += v.value;
    }
    put(head, 0x0101, 4); put(head, 20 + entries.size(), 4);
    put(head, 20 + entries.size() + keys.size(), 4); put(head, values.size(), 4);
    fs::create_directories(folder / "sce_sys");
    fs::create_directories(folder / "dvdroot_ps4");
    std::ofstream(folder / "sce_sys/param.sfo", std::ios::binary) << head << entries << keys << data;
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--check-app0") {
        const auto error = config_game_edition_error(argv[2]);
        std::cout << (error.empty() ? game_edition_name() : error) << '\n';
        return error.empty() ? 0 : 2;
    }
    const auto root = fs::temp_directory_path() / ("bbhost-edition-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto base = root / "game", update = root / "game-UPDATE";
    const auto jp = metadata("CUSA03014", 0), eu = metadata("CUSA03173", 13);
    {
        const auto& valid = jp;
        const auto& other = eu;
        const auto check = [&] { return game_edition_error(base.string(), "").empty(); };
        assert(!check());
        write_sfo(base, valid); assert(check());
        write_sfo(base, other); assert(!check());
        write_sfo(base, metadata("CUSA00900", 1)); assert(!check());
        auto bad = valid; bad[1] = other[1]; write_sfo(base, bad); assert(!check());
        bad = valid; bad.erase(bad.begin()); write_sfo(base, bad); assert(!check());
        bad = valid; bad.pop_back(); bad.erase(bad.begin() + 1); write_sfo(base, bad); assert(!check());
        bad = valid; bad[1].format = 0x0204; write_sfo(base, bad); assert(!check());
        bad = valid; bad[1].value.resize(1); write_sfo(base, bad); assert(!check());
        bad = valid; bad.push_back(valid[0]); write_sfo(base, bad); assert(!check());
        bad = valid; bad[0].format = 0x1234; write_sfo(base, bad); assert(!check());
        write_sfo(base, valid);
        const auto file = base / "sce_sys/param.sfo";
        fs::resize_file(file, fs::file_size(file) - 1); assert(!check());
        write_sfo(base, valid);
        { std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
          f.seekp(16); f.write("\xff\xff\xff\xff", 4); }
        assert(!check());
        write_sfo(base, valid); write_sfo(update, valid);
        assert(game_edition_error(base.string(), update.string()).empty());
        write_sfo(update, other);
        assert(!game_edition_error(base.string(), update.string()).empty());
        fs::remove(update / "sce_sys/param.sfo");
        assert(!game_edition_error(base.string(), update.string()).empty());
        fs::remove_all(base); fs::remove_all(update);
    }
    const auto& valid = jp;
    const auto& other = eu;
    write_sfo(base, valid);
    const auto eboot = root / "eboot.elf";
    std::ofstream(eboot) << "fixture; executable hash is checked separately";
    HostConfig cfg; cfg.app0 = base.string(); cfg.eboot = eboot.string(); cfg.headless = true;
    config_value_set("plugins.region_switch", "false");
    config_value_set("region_switch.region", "OFF");
    assert(config_setup_help(cfg).empty());
    write_sfo(base, other); assert(!config_setup_help(cfg).empty());
    write_sfo(base, valid); write_sfo(update, other);
    assert(!config_setup_help(cfg).empty());
    cfg.app0 = update.string(); assert(!config_game_edition_error(cfg.app0).empty());
    fs::remove_all(root);
    std::cout << "PASS: edition admission, missing/malformed metadata, update mixing, disabled plugin; "
              << BBHOST_EDITION_PROFILE << '\n';
}
