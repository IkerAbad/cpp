#pragma once

// Campañas (F4): capítulos históricos en orden, cada uno un escenario con objetivos.
//
// data/campaigns/<id>/campana.toml:
//
//   name = "..."
//   intro = "..."
//   [[chapter]]
//   id = "toledo"
//   title = "..."
//   date = "mayo-junio de 1212"
//   scenario = "1_toledo.toml"     # en la misma carpeta
//   rival = "normal"               # perfil de IA por omisión ([[ai.profile]])
//   fog = true
//   history = "..."                # nota histórica, texto propio
//   [[chapter.source]]             # de dónde sale: cita literal verificada
//   quote = "..."
//   work = "Wikipedia, «Battle of Las Navas de Tolosa»"
//   url = "https://..."
//
// El progreso del jugador (capítulos ganados) va en un fichero suyo, no en data/.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rts::game {

inline constexpr std::string_view kCampaignDir = "campaigns";
inline constexpr std::string_view kCampaignFile = "campana.toml";

struct CampaignSource {
    std::string quote;
    std::string work;
    std::string url;
};

struct CampaignChapter {
    std::string id;
    std::string title;
    std::string date;
    std::string scenario;  // fichero, relativo a la carpeta de la campaña
    std::string rival;     // vacío: el del menú
    bool fog = false;
    std::string history;
    std::vector<CampaignSource> sources;
};

struct Campaign {
    std::string id;  // nombre de la carpeta
    std::string name;
    std::string intro;
    std::vector<CampaignChapter> chapters;
    std::filesystem::path dir;
};

[[nodiscard]] std::expected<Campaign, std::string> parse_campaign(std::string_view text,
                                                                std::string_view source = "<memoria>");

// Todas las campañas de data_dir/campaigns, por id. Las que no se leen, en errors.
[[nodiscard]] std::vector<Campaign> load_campaigns(const std::filesystem::path& data_dir,
                                                   std::vector<std::string>& errors);

// Capítulos ganados, como "campaña/capítulo".
struct CampaignProgress {
    std::vector<std::string> won;

    [[nodiscard]] bool has_won(std::string_view campaign, std::string_view chapter) const;
    void mark_won(std::string_view campaign, std::string_view chapter);
    // Se puede jugar el primero y los que siguen a uno ganado.
    [[nodiscard]] bool unlocked(const Campaign& c, std::size_t chapter) const;
};

[[nodiscard]] CampaignProgress parse_progress(std::string_view text);
[[nodiscard]] std::string progress_toml(const CampaignProgress& p);

}  // namespace rts::game
