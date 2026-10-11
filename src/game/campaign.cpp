#include "game/campaign.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include <toml++/toml.hpp>

#include "game/localized_toml.hpp"

namespace rts::game {

namespace {

std::string key_of(std::string_view campaign, std::string_view chapter) {
    return std::format("{}/{}", campaign, chapter);
}

}  // namespace

std::expected<Campaign, std::string> parse_campaign(std::string_view text, std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: {}", source, e.description()));
    }
    const auto fail = [&](std::string msg) { return std::unexpected(std::format("{}: {}", source, msg)); };
    Campaign c;
    c.name = read_localized(root.get("name")).value_or(LocalizedText());
    c.intro = read_localized(root.get("intro")).value_or(LocalizedText());
    if (c.name.empty()) {
        return fail("falta 'name'");
    }
    const toml::array* chapters = root["chapter"].as_array();
    if (chapters == nullptr || chapters->empty()) {
        return fail("una campaña necesita algún [[chapter]]");
    }
    for (std::size_t i = 0; i < chapters->size(); ++i) {
        const toml::table* t = (*chapters)[i].as_table();
        if (t == nullptr) {
            return fail(std::format("'chapter[{}]' debe ser una tabla", i));
        }
        CampaignChapter ch;
        ch.id = (*t)["id"].value_or(std::string());
        ch.title = read_localized(t->get("title")).value_or(LocalizedText());
        ch.date = read_localized(t->get("date")).value_or(LocalizedText());
        ch.scenario = (*t)["scenario"].value_or(std::string());
        ch.rival = (*t)["rival"].value_or(std::string());
        ch.fog = (*t)["fog"].value_or(false);
        ch.history = read_localized(t->get("history")).value_or(LocalizedText());
        if (ch.id.empty() || ch.title.empty() || ch.scenario.empty()) {
            return fail(std::format("'chapter[{}]' necesita 'id', 'title' y 'scenario'", i));
        }
        if (std::ranges::any_of(c.chapters, [&](const CampaignChapter& o) { return o.id == ch.id; })) {
            return fail(std::format("'chapter[{}].id' = \"{}\" está repetido", i, ch.id));
        }
        if (const toml::array* sources = (*t)["source"].as_array()) {
            for (std::size_t k = 0; k < sources->size(); ++k) {
                const toml::table* s = (*sources)[k].as_table();
                if (s == nullptr) {
                    return fail(std::format("'chapter[{}].source[{}]' debe ser una tabla", i, k));
                }
                CampaignSource src{(*s)["quote"].value_or(std::string()),
                                   read_localized(s->get("work")).value_or(LocalizedText()),
                                   (*s)["url"].value_or(std::string())};
                if (src.quote.empty() || src.work.empty()) {
                    return fail(std::format("'chapter[{}].source[{}]' necesita 'quote' y 'work'", i, k));
                }
                ch.sources.push_back(std::move(src));
            }
        }
        c.chapters.push_back(std::move(ch));
    }
    return c;
}

std::vector<Campaign> load_campaigns(const std::filesystem::path& data_dir, std::vector<std::string>& errors) {
    std::vector<Campaign> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir / kCampaignDir, ec)) {
        const auto file = entry.path() / kCampaignFile;
        if (!entry.is_directory() || !std::filesystem::exists(file)) {
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto c = parse_campaign(text, file.string());
        if (!c) {
            errors.push_back(c.error());
            continue;
        }
        c->id = entry.path().filename().string();
        c->dir = entry.path();
        out.push_back(std::move(*c));
    }
    std::ranges::sort(out, {}, &Campaign::id);
    return out;
}

bool CampaignProgress::has_won(std::string_view campaign, std::string_view chapter) const {
    return std::ranges::find(won, key_of(campaign, chapter)) != won.end();
}

void CampaignProgress::mark_won(std::string_view campaign, std::string_view chapter) {
    if (!has_won(campaign, chapter)) {
        won.push_back(key_of(campaign, chapter));
    }
}

bool CampaignProgress::unlocked(const Campaign& c, std::size_t chapter) const {
    return chapter == 0 || (chapter < c.chapters.size() && has_won(c.id, c.chapters[chapter - 1].id));
}

CampaignProgress parse_progress(std::string_view text) {
    CampaignProgress p;
    try {
        const toml::table root = toml::parse(text);
        if (const toml::array* won = root["won"].as_array()) {
            for (const auto& v : *won) {
                if (const auto s = v.value<std::string>()) {
                    p.won.push_back(*s);
                }
            }
        }
    } catch (const toml::parse_error&) {
        // Un progreso ilegible se trata como vacío: se vuelve a empezar, no se cae el juego.
    }
    return p;
}

std::string progress_toml(const CampaignProgress& p) {
    std::string out = "# Capítulos de campaña ganados (F4).\nwon = [";
    for (std::size_t i = 0; i < p.won.size(); ++i) {
        out += std::format("{}\"{}\"", i == 0 ? "" : ", ", p.won[i]);
    }
    return out + "]\n";
}

}  // namespace rts::game
