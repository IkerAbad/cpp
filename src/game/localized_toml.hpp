#pragma once

// Lectura y escritura de LocalizedText en TOML (campaña y escenarios).

#include <optional>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

#include "game/i18n.hpp"

namespace rts::game {

// Texto como cadena básica de TOML.
[[nodiscard]] inline std::string toml_quote(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += ch;
        }
    }
    return out + "\"";
}

// Una cadena (español) o una tabla de textos por idioma; nada si es otra cosa o falta.
[[nodiscard]] inline std::optional<LocalizedText> read_localized(const toml::node* n) {
    if (n == nullptr) {
        return std::nullopt;
    }
    if (const auto s = n->value<std::string>()) {
        return LocalizedText(*s);
    }
    const toml::table* t = n->as_table();
    if (t == nullptr) {
        return std::nullopt;
    }
    LocalizedText out;
    for (const auto& [k, v] : *t) {
        const auto s = v.value<std::string>();
        if (!s) {
            return std::nullopt;
        }
        out.set(std::string(k.str()), *s);
    }
    return out;
}

// Como se lee: cadena si solo hay español; si no, tabla en línea.
[[nodiscard]] inline std::string localized_toml(const LocalizedText& t) {
    if (t.all().size() <= 1) {
        return toml_quote(t.es());
    }
    std::string out = "{ ";
    bool first = true;
    for (const auto& [code, text] : t.all()) {
        out += (first ? "" : ", ") + code + " = " + toml_quote(text);
        first = false;
    }
    return out + " }";
}

}  // namespace rts::game
