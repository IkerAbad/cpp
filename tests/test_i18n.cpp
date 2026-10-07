// Idiomas (F5): todo texto marcado en el código (T, TK, TF) tiene traducción en cada
// idioma, con los mismos especificadores de formato, y todo nombre de los datos tiene
// su nombre a la vista.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "game/i18n.hpp"
#include "game/options.hpp"

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

rts::game::Translations load(std::string_view code) {
    const auto path = std::filesystem::path(RTS_DATA_DIR) / rts::game::kLangDir / (std::string(code) + ".toml");
    auto t = rts::game::Translations::parse(read_file(path), path.string());
    REQUIRE_MESSAGE(t.has_value(), (t ? std::string() : t.error()));
    return std::move(*t);
}

// Deshace los escapes de un literal de C++ (los que se usan en la interfaz).
std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

// Textos marcados en el código de la interfaz: T("..."), TK("...") y TF("...", ...),
// con literales que se concatenan.
std::set<std::string> marked_texts() {
    std::set<std::string> out;
    const std::regex call(R"re(\bT[KF]?\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)[,)])re");
    const std::regex piece(R"re("((?:[^"\\]|\\.)*)")re");
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(RTS_SOURCE_DIR) / "game")) {
        if (entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string src = read_file(entry.path());
        for (auto it = std::sregex_iterator(src.begin(), src.end(), call); it != std::sregex_iterator(); ++it) {
            const std::string seq = (*it)[1].str();
            std::string text;
            for (auto p = std::sregex_iterator(seq.begin(), seq.end(), piece); p != std::sregex_iterator(); ++p) {
                text += unescape((*p)[1].str());
            }
            if (!text.empty()) {
                out.insert(text);
            }
        }
    }
    return out;
}

// Especificadores de formato, en orden: %d, %.2f, %%, {}...
std::vector<std::string> specifiers(const std::string& s) {
    static const std::regex spec(R"(%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[a-zA-Z%]|\{[^}]*\})");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), spec); it != std::sregex_iterator(); ++it) {
        out.push_back(it->str());
    }
    return out;
}

}  // namespace

TEST_CASE("Idiomas: cada texto marcado tiene traducción al inglés, con el mismo formato") {
    const auto texts = marked_texts();
    CHECK(texts.size() > 200);
    const auto en = load("en");
    CHECK(en.language() == "English");
    for (const std::string& t : texts) {
        CAPTURE(t);
        REQUIRE(en.has_text(t));
        CHECK(specifiers(en.text(t.c_str())) == specifiers(t));
    }
    // Y nada traducido que el código ya no use.
    for (const std::string& k : en.text_keys()) {
        CAPTURE(k);
        CHECK(texts.contains(k));
    }
}

TEST_CASE("Idiomas: cada nombre de los datos tiene nombre a la vista en cada idioma") {
    auto data = rts::game::load_game_data(RTS_DATA_DIR);
    REQUIRE(data.has_value());
    std::vector<std::string> ids;
    for (const auto& u : data->units.types) {
        ids.push_back(u.name);
    }
    for (const auto& b : data->buildings.types) {
        ids.push_back(b.name);
    }
    for (const auto& u : data->buildings.upgrades) {
        ids.push_back(u.name);
    }
    for (const auto& n : data->nodes.types) {
        ids.push_back(n.name);
    }
    for (const auto& t : data->terrain.types) {
        ids.push_back(t.name);
    }
    for (const auto& p : data->engine.ai_profile_names) {
        ids.push_back(p);
    }
    for (const auto& m : data->engine.map_presets) {
        ids.push_back(m.name);
    }
    for (const std::string_view code : {"es", "en"}) {
        const auto t = load(code);
        for (const std::string& id : ids) {
            CAPTURE(code);
            CAPTURE(id);
            CHECK(t.has_name(id));
        }
    }
}

TEST_CASE("Idiomas: sin traducción se ve el español, y se cambia de idioma") {
    const auto t = rts::game::Translations::parse("language = \"Prueba\"\n[text]\n\"Hola\" = \"Hello\"\n");
    REQUIRE(t.has_value());
    CHECK(std::string(t->text("Hola")) == "Hello");
    CHECK(std::string(t->text("Adiós")) == "Adiós");
    CHECK(std::string(t->name("aldeano")) == "aldeano");
    CHECK_FALSE(rts::game::Translations::parse("[text]\n").has_value());  // sin 'language'

    const auto langs = rts::game::available_languages(RTS_DATA_DIR);
    REQUIRE(langs.size() >= 2);
    REQUIRE(rts::game::set_language(RTS_DATA_DIR, "en").has_value());
    CHECK(std::string(rts::game::T("Empezar")) == "Start");
    CHECK(rts::game::TF("jugador {}", 2) == "player 2");
    CHECK(std::string(rts::game::N("hombre_armas")) == "Man-at-arms");
    CHECK_FALSE(rts::game::set_language(RTS_DATA_DIR, "xx").has_value());
    CHECK(std::string(rts::game::T("Empezar")) == "Start");  // se queda el anterior
    REQUIRE(rts::game::set_language(RTS_DATA_DIR, "es").has_value());
    CHECK(std::string(rts::game::T("Empezar")) == "Empezar");
    CHECK(std::string(rts::game::N("hombre_armas")) == "Hombre de armas");
}

TEST_CASE("Opciones: las de data/ están completas y las del jugador solo cambian lo suyo") {
    std::ifstream in(std::filesystem::path(RTS_DATA_DIR) / rts::game::kOptionsFile, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto defaults = rts::game::parse_options(text, nullptr);
    REQUIRE_MESSAGE(defaults.has_value(), (defaults ? std::string() : defaults.error()));
    CHECK(defaults->language == rts::game::kDefaultLanguage);
    CHECK_FALSE(defaults->resolutions.empty());
    CHECK(defaults->key(rts::game::KeyAction::Menu) == "F10");
    // Lo del jugador encima: solo lo que trae.
    const auto mine = rts::game::parse_options("language = \"en\"\n[volume]\nmusic = 0\n[keys]\nmenu = \"Escape\"\n",
                                               &*defaults);
    REQUIRE(mine.has_value());
    CHECK(mine->language == "en");
    CHECK(mine->music_percent == 0);
    CHECK(mine->effects_percent == defaults->effects_percent);
    CHECK(mine->key(rts::game::KeyAction::Menu) == "Escape");
    CHECK(mine->key(rts::game::KeyAction::Help) == "F2");
    // Ida y vuelta por lo que se guarda.
    const auto back = rts::game::parse_options(rts::game::options_toml(*mine), &*defaults);
    REQUIRE(back.has_value());
    CHECK(back->keys == mine->keys);
    CHECK(back->music_percent == 0);
    CHECK(back->width == mine->width);
    // Errores: incompleto sin base, fuera de rango.
    CHECK_FALSE(rts::game::parse_options("language = \"es\"\n", nullptr).has_value());
    CHECK_FALSE(rts::game::parse_options("[volume]\nmaster = 150\n", &*defaults).has_value());
    CHECK_FALSE(rts::game::parse_options("width = 10\n", &*defaults).has_value());
    // El idioma por omisión existe.
    CHECK(rts::game::set_language(RTS_DATA_DIR, defaults->language).has_value());
}
