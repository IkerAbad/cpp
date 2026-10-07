#pragma once

// Idiomas (F5). El texto español del código es la clave: T("Empezar") devuelve su
// traducción en el idioma elegido o, si no la hay, el propio texto. Los nombres de los
// tipos de los datos ("hombre_armas") se muestran con N(): "Hombre de armas".
//
// data/lang/<código>.toml:
//
//   language = "English"          # cómo se llama el idioma en su propia lengua
//   [text]                        # texto español del código = traducción
//   "Empezar" = "Start"
//   [names]                       # nombre en los datos = nombre a la vista
//   "hombre_armas" = "Man-at-arms"
//
// Es presentación pura: la simulación no sabe de idiomas.

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rts::game {

inline constexpr std::string_view kLangDir = "lang";
inline constexpr std::string_view kDefaultLanguage = "es";

class Translations {
public:
    [[nodiscard]] static std::expected<Translations, std::string> parse(std::string_view text,
                                                                     std::string_view source = "<memoria>");

    // Traducción del texto (puntero estable mientras viva el objeto) o el propio texto.
    [[nodiscard]] const char* text(const char* es) const;
    // Nombre a la vista de un tipo de los datos, o el propio nombre.
    [[nodiscard]] const char* name(const std::string& id) const;
    [[nodiscard]] bool has_text(std::string_view es) const { return texts_.contains(std::string(es)); }
    [[nodiscard]] bool has_name(std::string_view id) const { return names_.contains(std::string(id)); }
    [[nodiscard]] const std::string& language() const noexcept { return language_; }
    [[nodiscard]] std::vector<std::string> text_keys() const;

private:
    std::string language_;
    std::unordered_map<std::string, std::string> texts_;
    std::unordered_map<std::string, std::string> names_;
};

// Idiomas disponibles en data_dir/lang: código (nombre del fichero) y nombre propio.
struct LanguageInfo {
    std::string code;
    std::string name;
};
[[nodiscard]] std::vector<LanguageInfo> available_languages(const std::filesystem::path& data_dir);

// Carga data_dir/lang/<code>.toml como idioma actual. Si falla, deja el anterior y
// devuelve el error.
std::expected<void, std::string> set_language(const std::filesystem::path& data_dir, std::string_view code);
// Idioma actual (vacío al empezar: todo en español, sin nombres bonitos).
[[nodiscard]] const Translations& current_translations();

[[nodiscard]] inline const char* T(const char* es) { return current_translations().text(es); }
// Marca un literal para traducirlo después con T() (listas de textos): no hace nada.
[[nodiscard]] constexpr const char* TK(const char* es) noexcept { return es; }
// std::format con el formato traducido.
template <typename... Args>
[[nodiscard]] std::string TF(const char* es, const Args&... args) {
    return std::vformat(T(es), std::make_format_args(args...));
}
[[nodiscard]] inline const char* N(const std::string& id) { return current_translations().name(id); }

}  // namespace rts::game
