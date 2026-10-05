#include "sim/medicine.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

#include "sim/state_hash.hpp"

namespace rts::sim {

namespace {

constexpr std::int32_t kMilli = 1000;

bool affordable(const Stock& stock, const Stock& cost) noexcept {
    for (std::size_t i = 0; i < kResourceCount; ++i) {
        if (stock[i] < cost[i]) {
            return false;
        }
    }
    return true;
}

// Cuántos de la lista (pares entidad, cuenta) tiene e; 0 si no está.
std::int32_t count_of(const std::vector<std::pair<entt::entity, std::int32_t>>& counts, entt::entity e) {
    const auto it = std::ranges::find(counts, e, &std::pair<entt::entity, std::int32_t>::first);
    return it != counts.end() ? it->second : 0;
}

void add_count(std::vector<std::pair<entt::entity, std::int32_t>>& counts, entt::entity e) {
    const auto it = std::ranges::find(counts, e, &std::pair<entt::entity, std::int32_t>::first);
    if (it != counts.end()) {
        ++it->second;
    } else {
        counts.emplace_back(e, 1);
    }
}

}  // namespace

MedicineSystem::MedicineSystem(const MedicineParams& params, const Stock& ration_cost, std::vector<UnitType> units,
                               std::vector<BuildingType> buildings)
    : params_(params), ration_cost_(ration_cost), units_(std::move(units)), buildings_(std::move(buildings)) {
    assert(params_.patients_per_nurse > 0);
    assert(params_.patient_ration_ticks > 0);
}

std::int32_t MedicineSystem::heal_target(const BuildingType& post, std::int32_t max_hp) const noexcept {
    return static_cast<std::int32_t>(std::int64_t{max_hp} * post.heal_to_percent / kPercent);
}

bool MedicineSystem::is_post(const entt::registry& registry, entt::entity b, PlayerId player) const {
    if (b == entt::null || !registry.valid(b) || !registry.all_of<Building, Owner, Footprint>(b)) {
        return false;
    }
    const Building& bd = registry.get<Building>(b);
    return registry.get<Owner>(b).player == player && bd.working() && buildings_[bd.type].beds > 0;
}

void MedicineSystem::discharge(entt::registry& registry, entt::entity e) const {
    registry.remove<Patient>(e);
    registry.emplace_or_replace<Reorganizing>(e, params_.reorganize_ticks);
}

void MedicineSystem::apply(entt::registry& registry, MovementSystem& movement, const Command& command,
                           std::span<const entt::entity> units, std::uint32_t& next_order_id, Tick tick) {
    if (command.type == CommandType::SetStance) {
        return;
    }
    if (command.type == CommandType::Tend) {
        // Personal que no es aldeano (los aldeanos los lleva la economía).
        const auto post = static_cast<entt::entity>(command.object);
        if (command.object == kNoObject || !is_post(registry, post, command.player)) {
            return;
        }
        std::vector<entt::entity> staff;
        for (const entt::entity e : units) {
            const UnitType& ut = units_[registry.get<Unit>(e).type];
            if (!ut.worker && ut.care_skill > 0) {
                staff.push_back(e);
            }
        }
        if (staff.empty()) {
            return;
        }
        const std::uint32_t order = next_order_id++;
        movement.order_move(registry, staff, center_of(registry.get<Footprint>(post)), order, tick);
        for (const entt::entity e : staff) {
            registry.emplace_or_replace<Carer>(e, post, order, 0);
            registry.remove<Patient>(e);
        }
        return;
    }
    for (const entt::entity e : units) {
        registry.remove<Carer>(e);  // cualquier otra orden lo saca de su puesto
    }
    if (command.type != CommandType::Treat) {
        // Cualquier otra orden saca del puesto (o del camino hacia él).
        for (const entt::entity e : units) {
            if (const Patient* p = registry.try_get<Patient>(e)) {
                if (p->admitted) {
                    discharge(registry, e);
                } else {
                    registry.remove<Patient>(e);
                }
            }
        }
        return;
    }
    const auto post = static_cast<entt::entity>(command.object);
    if (command.object == kNoObject || !is_post(registry, post, command.player)) {
        return;
    }
    std::vector<entt::entity> going;
    for (const entt::entity e : units) {
        const Health& hp = registry.get<Health>(e);
        if (!units_[registry.get<Unit>(e).type].treatable || hp.hp >= hp.max_hp) {
            continue;  // no es persona o no está herido
        }
        if (const Patient* p = registry.try_get<Patient>(e); p != nullptr && p->admitted) {
            if (p->post == post) {
                continue;  // ya está ingresado aquí
            }
            discharge(registry, e);  // traslado: sale del otro puesto
        }
        going.push_back(e);
    }
    if (going.empty()) {
        return;
    }
    const std::uint32_t order = next_order_id++;
    movement.order_move(registry, going, center_of(registry.get<Footprint>(post)), order, tick);
    for (const entt::entity e : going) {
        Patient p;
        p.post = post;
        p.move_order = order;
        registry.emplace_or_replace<Patient>(e, p);
    }
}

void MedicineSystem::update_patients(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                                     std::uint32_t& next_order_id, Tick tick) {
    // Camas ocupadas y enfermeros al alcance, por puesto.
    std::vector<std::pair<entt::entity, std::int32_t>> occupied;
    for (const auto [e, p] : registry.view<const Patient>().each()) {
        if (p.admitted) {
            add_count(occupied, p.post);
        }
    }
    // Personal al alcance de cada puesto: aldeanos enfermeros y cirujanos. Los que no
    // son aldeanos se acercan por su cuenta; si no lo alcanzan tras dos llegadas, desisten.
    scratch_.clear();
    for (const entt::entity e : registry.view<Carer>()) {
        scratch_.push_back(e);
    }
    for (const entt::entity e : scratch_) {
        Carer& c = registry.get<Carer>(e);
        if (!is_post(registry, c.post, registry.get<Owner>(e).player)) {
            registry.remove<Carer>(e);
            continue;
        }
        const Footprint& f = registry.get<Footprint>(c.post);
        const Position& pos = registry.get<Position>(e);
        const Fixed reach = registry.get<Unit>(e).radius + params_.care_reach;
        if (distance_sq_to(f, {pos.x, pos.y}) <= mul_wide(reach, reach)) {
            continue;
        }
        const MoveGoal* g = registry.try_get<MoveGoal>(e);
        const bool arrived = g != nullptr && g->order_id == c.move_order && g->arrived;
        if (arrived && ++c.approaches >= 2) {
            registry.remove<Carer>(e);
            continue;
        }
        if (g == nullptr || g->order_id != c.move_order || arrived) {
            c.move_order = next_order_id++;
            const std::array<entt::entity, 1> one{e};
            movement.order_move(registry, one, clamp_to(f, tile_of(pos)), c.move_order, tick);
        }
    }
    struct Staff {
        entt::entity post;
        std::int32_t skill;
        entt::entity who;
    };
    std::vector<Staff> staff;
    const auto add_staff = [&](entt::entity e, entt::entity post) {
        if (!registry.valid(post) || !registry.all_of<Footprint>(post)) {
            return;
        }
        const Position& pos = registry.get<Position>(e);
        const Unit& u = registry.get<Unit>(e);
        const Fixed reach = u.radius + params_.care_reach;
        const std::int32_t skill = units_[u.type].care_skill;
        if (skill > 0 && distance_sq_to(registry.get<Footprint>(post), {pos.x, pos.y}) <= mul_wide(reach, reach)) {
            staff.push_back({post, skill, e});
        }
    };
    for (const auto [e, w] : registry.view<const Worker>().each()) {
        if (w.task == WorkerTask::Nurse) {
            add_staff(e, w.building);
        }
    }
    for (const auto [e, c] : registry.view<const Carer>().each()) {
        add_staff(e, c.post);
    }
    // Por puesto, los más expertos primero (a igualdad, por entidad).
    std::ranges::sort(staff, [](const Staff& a, const Staff& b) {
        const auto pa = entt::to_integral(a.post);
        const auto pb = entt::to_integral(b.post);
        if (pa != pb) {
            return pa < pb;
        }
        return a.skill != b.skill ? a.skill > b.skill : entt::to_integral(a.who) < entt::to_integral(b.who);
    });
    // Pericia del que atiende al paciente de rango rank en el puesto post (0 = nadie).
    const auto carer_skill = [&](entt::entity post, std::int32_t rank, std::int32_t slots) {
        const auto first = std::ranges::find(staff, post, &Staff::post);
        const std::int32_t index = rank / params_.patients_per_nurse;
        std::int32_t n = 0;
        for (auto it = first; it != staff.end() && it->post == post && n < slots; ++it, ++n) {
            if (n == index) {
                return it->skill;
            }
        }
        return 0;
    };

    scratch_.clear();
    for (const entt::entity e : registry.view<Patient>()) {
        scratch_.push_back(e);
    }
    // Ingresos y caídas de puestos; los ingresados, agrupados por puesto para curarlos.
    std::vector<std::pair<entt::entity, entt::entity>> in_care;  // puesto, paciente
    for (const entt::entity e : scratch_) {
        Patient& p = registry.get<Patient>(e);
        const PlayerId player = registry.get<Owner>(e).player;
        if (!is_post(registry, p.post, player)) {
            if (!p.admitted) {
                registry.remove<Patient>(e);  // el puesto ya no existe o no funciona
            } else if (registry.valid(p.post)) {
                discharge(registry, e);  // inutilizado (quemado) pero en pie: los sacan
            } else {
                dead_.push_back(e);  // el puesto cayó con ellos dentro
            }
            continue;
        }
        const BuildingType& bt = buildings_[registry.get<Building>(p.post).type];
        const Health& hp = registry.get<Health>(e);
        const std::int32_t target = heal_target(bt, hp.max_hp);
        if (p.admitted) {
            in_care.emplace_back(p.post, e);
            continue;
        }
        if (hp.hp >= target) {
            registry.remove<Patient>(e);  // este puesto ya no puede hacer más por él
            continue;
        }
        const Footprint& f = registry.get<Footprint>(p.post);
        const Position& pos = registry.get<Position>(e);
        const Fixed reach = registry.get<Unit>(e).radius + params_.care_reach;
        const MoveGoal* g = registry.try_get<MoveGoal>(e);
        if (distance_sq_to(f, {pos.x, pos.y}) > mul_wide(reach, reach)) {
            const bool arrived = g != nullptr && g->order_id == p.move_order && g->arrived;
            if (arrived && ++p.approaches >= 2) {
                registry.remove<Patient>(e);  // llegó dos veces a lo más cerca que puede y no alcanza
                continue;
            }
            if (g == nullptr || g->order_id != p.move_order || arrived) {
                p.move_order = next_order_id++;
                const std::array<entt::entity, 1> one{e};
                movement.order_move(registry, one, clamp_to(f, tile_of(pos)), p.move_order, tick);
            }
            continue;
        }
        if (count_of(occupied, p.post) >= bt.beds) {
            continue;  // sin cama libre: espera a la puerta
        }
        // Ingreso: inoperativo y sin bienes.
        add_count(occupied, p.post);
        p.admitted = true;
        p.heal_acc = 0;
        p.ration_timer = params_.patient_ration_ticks - 1;  // llega sin bienes: come ya
        p.fed = false;
        registry.remove<MoveGoal, PathFollow>(e);
        if (Supply* s = registry.try_get<Supply>(e)) {
            s->rations = 0;
            s->ration_timer = 0;
            s->hungry_ticks = 0;
            s->ammo = 0;
        }
        if (Combatant* c = registry.try_get<Combatant>(e)) {
            c->target = entt::null;
            c->explicit_target = false;
            c->attack_move = false;
            c->chase_order = 0;
        }
        ++stats_.admitted;
        in_care.emplace_back(p.post, e);
    }

    // Cuidados: todos reposan; en cada puesto, por orden de entidad, cada miembro del
    // personal (hasta nurses, los más expertos primero) atiende a patients_per_nurse
    // pacientes, con su pericia.
    std::ranges::sort(in_care, [](const auto& a, const auto& b) {
        const auto pa = entt::to_integral(a.first);
        const auto pb = entt::to_integral(b.first);
        return pa != pb ? pa < pb : entt::to_integral(a.second) < entt::to_integral(b.second);
    });
    std::int32_t rank = 0;
    for (std::size_t i = 0; i < in_care.size(); ++i) {
        const auto [post, e] = in_care[i];
        rank = i > 0 && in_care[i - 1].first == post ? rank + 1 : 0;
        const BuildingType& bt = buildings_[registry.get<Building>(post).type];
        const std::int32_t skill = carer_skill(post, rank, bt.nurses);
        Patient& p = registry.get<Patient>(e);
        ++stats_.patients;
        // Come como un aldeano, del almacén del jugador; sin comida no mejora.
        if (++p.ration_timer >= params_.patient_ration_ticks) {
            p.ration_timer = 0;
            Stock& stock = economy.player_state(registry.get<Owner>(e).player).stock;
            p.fed = affordable(stock, ration_cost_);
            if (p.fed) {
                for (std::size_t r = 0; r < kResourceCount; ++r) {
                    stock[r] -= ration_cost_[r];
                }
            }
        }
        if (!p.fed) {
            continue;
        }
        const std::int64_t care = std::int64_t{params_.nurse_heal_milli_per_tick} * skill / kPercent;
        const std::int64_t rate = (params_.bed_heal_milli_per_tick + care) * bt.care_percent / kPercent;
        p.heal_acc += static_cast<std::int32_t>(rate);
        Health& hp = registry.get<Health>(e);
        const std::int32_t target = heal_target(bt, hp.max_hp);
        hp.hp = std::min(hp.hp + p.heal_acc / kMilli, std::max(target, hp.hp));
        p.heal_acc %= kMilli;
        if (hp.hp >= target) {
            discharge(registry, e);
            ++stats_.discharged;
        }
    }
}

void MedicineSystem::natural_healing(entt::registry& registry, Tick tick) const {
    if (params_.natural_heal_interval_ticks <= 0) {
        return;
    }
    const auto interval = static_cast<std::uint32_t>(params_.natural_heal_interval_ticks);
    const auto view = registry.view<Health, const Unit>(entt::exclude<Patient>);
    for (const entt::entity e : view) {
        if ((entt::to_integral(e) + tick) % interval != 0 || !units_[view.get<const Unit>(e).type].treatable) {
            continue;
        }
        Health& hp = view.get<Health>(e);
        if (hp.hp >= hp.max_hp || std::int64_t{hp.hp} * kPercent < std::int64_t{hp.max_hp} * params_.light_wound_percent) {
            continue;  // sano, o herida que no sana sola
        }
        if (const Hurt* h = registry.try_get<Hurt>(e); h != nullptr && tick - h->tick < static_cast<Tick>(params_.calm_ticks)) {
            continue;  // aún en combate
        }
        if (const Supply* s = registry.try_get<Supply>(e);
            s != nullptr && units_[view.get<const Unit>(e).type].supply.rations > 0 && s->hungry()) {
            continue;  // sin comer no sana
        }
        ++hp.hp;
    }
}

void MedicineSystem::update(entt::registry& registry, MovementSystem& movement, EconomySystem& economy,
                            std::uint32_t& next_order_id, Tick tick) {
    stats_ = MedicineTickStats{};
    dead_.clear();
    // Reorganización: cuenta atrás.
    scratch_.clear();
    for (const auto [e, r] : registry.view<Reorganizing>().each()) {
        if (--r.ticks_left <= 0) {
            scratch_.push_back(e);
        }
    }
    for (const entt::entity e : scratch_) {
        registry.remove<Reorganizing>(e);
    }
    update_patients(registry, movement, economy, next_order_id, tick);
    natural_healing(registry, tick);
    for (const entt::entity e : dead_) {
        economy.record_loss(registry.get<Owner>(e).player);
        registry.destroy(e);
        ++stats_.died;
    }
}

void MedicineSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, p] : registry.view<const Patient>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(p.post));
        h.add_u32(p.admitted ? 1U : 0U);
        h.add_i32(p.heal_acc);
        h.add_i32(p.ration_timer);
        h.add_u32(p.fed ? 1U : 0U);
        h.add_u32(p.move_order);
        h.add_i32(p.approaches);
    }
    for (const auto [e, r] : registry.view<const Reorganizing>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(r.ticks_left);
    }
    for (const auto [e, c] : registry.view<const Carer>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(entt::to_integral(c.post));
        h.add_u32(c.move_order);
        h.add_i32(c.approaches);
    }
    for (const auto [e, hurt] : registry.view<const Hurt>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_u32(hurt.tick);
    }
}

}  // namespace rts::sim
