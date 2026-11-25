// Web build: minimal AssetManager. Fonts are embedded into the wasm at /fonts (see build-web.sh).
#include "asset_manager.h"

#include <cstdio>

namespace app {

struct AssetManager::LoadedFont {
    std::filesystem::path path;
    uint32_t size;
    lv_font_t* font;
};

AssetManager::AssetManager(std::string, std::filesystem::path) { roots_.push_back("/"); }

AssetManager::~AssetManager() {
    for (auto& f : loaded_fonts_) {
        if (f->font) lv_freetype_font_delete(f->font);
    }
}

std::string AssetManager::default_app_name() { return "velvet"; }
std::filesystem::path AssetManager::default_cmake_assets_root() { return "/"; }
const std::vector<std::filesystem::path>& AssetManager::roots() const { return roots_; }
std::filesystem::path AssetManager::resolve(const std::filesystem::path& p) const { return "/" / p; }
std::filesystem::path AssetManager::resolve_font(const std::filesystem::path& f) const {
    return std::filesystem::path("/fonts") / f;
}

lv_font_t* AssetManager::load_font(const std::filesystem::path& file_name, uint32_t size) {
    for (auto& f : loaded_fonts_) {
        if (f->path == file_name && f->size == size) return f->font;
    }
    const auto path = resolve_font(file_name);
    lv_font_t* font = lv_freetype_font_create(path.string().c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                              size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!font) std::printf("[web] font load failed: %s\n", path.string().c_str());
    loaded_fonts_.push_back(std::make_unique<LoadedFont>(LoadedFont{file_name, size, font}));
    return font;
}

lv_font_t* AssetManager::load_standard_font(uint32_t size, StandardFontWeight) {
    return load_font("Anton-Regular.ttf", size);
}

void AssetManager::add_root_if_valid(std::filesystem::path p) { roots_.push_back(std::move(p)); }

} // namespace app
