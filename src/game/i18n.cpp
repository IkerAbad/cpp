#include "game/i18n.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>

#include <toml++/toml.hpp>

namespace rts::game {

namespace {

// Idioma actual. Solo lo cambia el hilo de la interfaz (menú de opciones).
std::unique_ptr<Translations>& current() {
    static std::unique_ptr<Translations> t = std::make_unique<Translations>();
    return t;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

std::expected<Translations, std::string> Translations::parse(std::string_view text, std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source, e.description()));
    }
    Translations t;
    t.language_ = root["language"].value_or(std::string());
    if (t.language_.empty()) {
        return std::unexpected(std::format("{}: falta 'language'", source));
    }
    for (const auto& [section, dest] : {std::pair{"text", &t.texts_}, std::pair{"names", &t.names_}}) {
        const toml::table* table = root[section].as_table();
        if (table == nullptr) {
            continue;
        }
        for (const auto& [k, v] : *table) {
            const auto s = v.value<std::string>();
            if (!s) {
                return std::unexpected(std::format("{}: '{}.{}' debe ser un texto", source, section, k.str()));
            }
            dest->emplace(std::string(k.str()), *s);
        }
    }
    return t;
}

const char* Translations::text(const char* es) const {
    const auto it = texts_.find(es);
    return it != texts_.end() ? it->second.c_str() : es;
}

const char* Translations::name(const std::string& id) const {
    const auto it = names_.find(id);
    return it != names_.end() ? it->second.c_str() : id.c_str();
}

std::vector<std::string> Translations::text_keys() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : texts_) {
        out.push_back(k);
    }
    std::ranges::sort(out);
    return out;
}

std::vector<LanguageInfo> available_languages(const std::filesystem::path& data_dir) {
    std::vector<LanguageInfo> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir / kLangDir, ec)) {
        if (entry.path().extension() != ".toml") {
            continue;
        }
        const auto t = Translations::parse(read_file(entry.path()), entry.path().string());
        if (t) {
            out.push_back({entry.path().stem().string(), t->language()});
        }
    }
    std::ranges::sort(out, {}, &LanguageInfo::code);
    return out;
}

std::expected<void, std::string> set_language(const std::filesystem::path& data_dir, std::string_view code) {
    const auto path = data_dir / kLangDir / std::format("{}.toml", code);
    if (!std::filesystem::exists(path)) {
        return std::unexpected(std::format("no existe {}", path.string()));
    }
    auto t = Translations::parse(read_file(path), path.string());
    if (!t) {
        return std::unexpected(t.error());
    }
    t->set_code(std::string(code));
    current() = std::make_unique<Translations>(std::move(*t));
    return {};
}

std::string_view current_language_code() {
    const std::string& c = current_translations().code();
    return c.empty() ? kDefaultLanguage : std::string_view(c);
}

const std::string& LocalizedText::es() const {
    static const std::string kEmpty;
    const auto it = by_lang_.find(kDefaultLanguage);
    return it != by_lang_.end() ? it->second : kEmpty;
}

const std::string& LocalizedText::get() const {
    const auto it = by_lang_.find(current_language_code());
    return it != by_lang_.end() && !it->second.empty() ? it->second : es();
}

const Translations& current_translations() {
    return *current();
}

}  // namespace rts::game
