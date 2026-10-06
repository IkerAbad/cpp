#include "game/sound_director.hpp"

#include <algorithm>
#include <unordered_map>

namespace rts::game {

namespace {
constexpr float kPercentF = 100.0f;
}  // namespace

void SoundDirector::cue(std::int32_t recipe, float volume, float pan, double now_ms) {
    if (recipe < 0 || volume <= 0.0f) {
        return;
    }
    const auto r = static_cast<std::size_t>(recipe);
    // El mismo sonido no se repite antes de su espera: cien espadas no suenan cien veces.
    if (now_ms - last_played_[r] < spec_->recipes[r].cooldown_ms) {
        return;
    }
    last_played_[r] = now_ms;
    cues_.push_back({recipe, volume, pan});
}

void SoundDirector::cue_at(std::int32_t recipe, render::Vec2 at, render::Vec2 screen, double now_ms) {
    // Dentro de la pantalla, a plena voz; fuera, cada vez más bajo hasta el margen.
    const float out_x = std::max({0.0f, -at.x, at.x - screen.x});
    const float out_y = std::max({0.0f, -at.y, at.y - screen.y});
    const float beyond = std::max(out_x, out_y);
    const auto margin = static_cast<float>(spec_->config.hearing_margin_px);
    const float volume = beyond <= 0.0f ? 1.0f : (margin > 0.0f ? 1.0f - beyond / margin : 0.0f);
    if (volume <= 0.0f) {
        return;
    }
    const float half = screen.x * 0.5f;
    const float pan = half > 0.0f ? std::clamp((at.x - half) / half, -1.0f, 1.0f) : 0.0f;
    cue(recipe, volume, pan * static_cast<float>(spec_->config.pan_percent) / kPercentF, now_ms);
}

std::int32_t SoundDirector::hit_sound(const sim::SnapshotHit& h) const {
    const audio::EventSounds& e = spec_->events;
    if (h.attacker_type >= data_->units.types.size()) {
        return e.blunt_hit;
    }
    const sim::CombatStats& c = data_->units.types[h.attacker_type].type.combat;
    if (c.siege) {
        return e.siege_hit;
    }
    if (c.projectile_speed.raw() != 0) {
        return e.ranged_hit;
    }
    // Filo contra filo, o golpe sordo, según el arma del dibujo (F1).
    if (h.attacker_type < data_->art.units.size()) {
        const render::Weapon w = data_->art.units[h.attacker_type].weapon;
        if (w == render::Weapon::Sword || w == render::Weapon::Axe) {
            return e.melee_hit;
        }
    }
    return e.blunt_hit;
}

void SoundDirector::on_tick(const sim::Snapshot& prev, const sim::Snapshot& curr, sim::PlayerId me,
                            const Locate& locate, render::Vec2 screen, double now_ms) {
    const audio::EventSounds& e = spec_->events;
    for (const sim::SnapshotHit& h : curr.hits) {
        if (const auto at = locate(h.pos)) {
            last_combat_ms_ = now_ms;
            cue_at(hit_sound(h), *at, screen, now_ms);
        }
    }
    for (const sim::SnapshotShot& s : curr.shots) {
        if (const auto at = locate(s.pos)) {
            last_combat_ms_ = now_ms;
            cue_at(e.shot, *at, screen, now_ms);
        }
    }
    // Bajas: lo que estaba y ya no está (sin contar a quien entra en una torre, que
    // sigue en el snapshot marcado como guarnecido).
    if (!curr.hits.empty() || prev.entities.size() != curr.entities.size()) {
        std::unordered_map<std::uint32_t, bool> alive;
        alive.reserve(curr.entities.size());
        for (const sim::SnapshotEntity& c : curr.entities) {
            alive[c.id] = true;
        }
        for (const sim::SnapshotEntity& p : prev.entities) {
            if (!alive.contains(p.id) && !p.garrisoned) {
                if (const auto at = locate(p.pos)) {
                    cue_at(e.death, *at, screen, now_ms);
                }
            }
        }
    }
    // Derrumbes y obras terminadas.
    std::unordered_map<std::uint32_t, const sim::SnapshotObject*> now_objects;
    now_objects.reserve(curr.objects.size());
    for (const sim::SnapshotObject& o : curr.objects) {
        now_objects[o.id] = &o;
    }
    for (const sim::SnapshotObject& o : prev.objects) {
        if (o.kind != sim::ObjectKind::Building) {
            continue;
        }
        const auto it = now_objects.find(o.id);
        const sim::Fixed half = sim::Fixed::from_int(o.size) / sim::Fixed::from_int(2);
        const sim::Position center{sim::Fixed::from_int(o.origin.x) + half, sim::Fixed::from_int(o.origin.y) + half};
        if (it == now_objects.end()) {
            if (const auto at = locate(center)) {
                cue_at(e.collapse, *at, screen, now_ms);
            }
        } else if (!o.complete && it->second->complete && o.owner == me) {
            cue(e.built, 1.0f, 0.0f, now_ms);  // propio: se oye esté donde esté
        }
    }
}

void SoundDirector::on_alert(AlertKind kind, double now_ms) {
    const audio::EventSounds& e = spec_->events;
    switch (kind) {
        case AlertKind::UnderAttack:
        case AlertKind::Fire:
        case AlertKind::Rout:
            cue(e.alarm, 1.0f, 0.0f, now_ms);
            break;
        case AlertKind::UnitReady:
            cue(e.ready, 1.0f, 0.0f, now_ms);
            break;
        case AlertKind::Hunger:
        case AlertKind::NoFood:
        case AlertKind::Count:
            break;
    }
}

void SoundDirector::on_order(double now_ms) { cue(spec_->events.order, 1.0f, 0.0f, now_ms); }

void SoundDirector::on_work(WorkSound kind, render::Vec2 at, render::Vec2 screen, double now_ms) {
    const audio::EventSounds& e = spec_->events;
    const std::int32_t recipe = kind == WorkSound::Chop ? e.chop : (kind == WorkSound::Mine ? e.mine : e.build);
    cue_at(recipe, at, screen, now_ms);
}

void SoundDirector::on_fires(const std::vector<render::Vec2>& burning, render::Vec2 screen, double now_ms) {
    if (burning.empty()) {
        return;
    }
    const std::int32_t recipe = spec_->events.fire;
    if (recipe < 0 || now_ms - last_played_[static_cast<std::size_t>(recipe)] < spec_->config.fire_interval_ms) {
        return;
    }
    // El más cercano al centro de la pantalla.
    const render::Vec2 mid = screen * 0.5f;
    const auto nearest = std::ranges::min_element(burning, {}, [&](render::Vec2 p) {
        const render::Vec2 d = p - mid;
        return d.x * d.x + d.y * d.y;
    });
    cue_at(recipe, *nearest, screen, now_ms);
}

std::vector<audio::SoundCue> SoundDirector::take() {
    std::vector<audio::SoundCue> out;
    out.swap(cues_);
    return out;
}

std::int32_t SoundDirector::music(double now_ms) const noexcept {
    return now_ms - last_combat_ms_ < spec_->config.battle_hold_ms ? spec_->battle_music : spec_->peace_music;
}

}  // namespace rts::game
