// Arte propio generado por código (F1): figuras, edificios, recursos, terreno, efectos,
// atlas y escena ordenada en profundidad.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "game/config.hpp"
#include "render/art.hpp"
#include "render/atlas.hpp"
#include "render/scene.hpp"

using rts::render::Atlas;
using rts::render::Image;
using rts::render::Pose;
using rts::render::SpriteImages;

namespace {

const rts::game::GameData& game_data() {
    static const rts::game::GameData data = [] {
        auto d = rts::game::load_game_data(RTS_DATA_DIR);
        REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error()));
        return std::move(*d);
    }();
    return data;
}

bool same_pixels(const Image& a, const Image& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}

bool anchor_inside(const Image& img) {
    return img.anchor.x >= 0.0f && img.anchor.y >= 0.0f && img.anchor.x <= static_cast<float>(img.width) &&
           img.anchor.y <= static_cast<float>(img.height) + 1.0f;
}

}  // namespace

TEST_CASE("Arte: data/art.toml cubre todos los tipos de los catálogos") {
    const auto& d = game_data();
    CHECK(d.art.units.size() == d.units.types.size());
    CHECK(d.art.buildings.size() == d.buildings.types.size());
    CHECK(d.art.nodes.size() == d.nodes.types.size());
    CHECK(d.art.terrain.size() == d.terrain.types.size());
    // Los tamaños salen de los catálogos, no del arte.
    for (std::size_t i = 0; i < d.buildings.types.size(); ++i) {
        CHECK(d.art.buildings[i].size_tiles == d.buildings.types[i].type.size);
    }
}

TEST_CASE("Arte: un nombre que no existe, uno repetido o uno que falta se rechazan") {
    const auto& d = game_data();
    const auto art = std::ranges::find(d.files, std::string("art.toml"), &rts::game::DataFile::path);
    REQUIRE(art != d.files.end());
    const rts::game::Catalogs catalogs{d.terrain, d.units, d.buildings, d.nodes};
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string t = art->text;
        const auto at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        return rts::game::parse_art_spec(t, catalogs);
    };
    REQUIRE(rts::game::parse_art_spec(art->text, catalogs).has_value());
    const auto unknown = replaced("name = \"aldeano\"", "name = \"dragon\"");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().find("dragon") != std::string::npos);
    const auto repeated = replaced("name = \"leva\"", "name = \"aldeano\"");
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().find("repetido") != std::string::npos);
    const auto bad_value = replaced("figure = \"persona\"", "figure = \"robot\"");
    REQUIRE_FALSE(bad_value.has_value());
    CHECK(bad_value.error().find("robot") != std::string::npos);
}

TEST_CASE("Arte: cada figura tiene sus poses, con la tela del jugador, y andar cambia el dibujo") {
    const auto& d = game_data();
    for (std::size_t u = 0; u < d.art.units.size(); ++u) {
        CAPTURE(d.units.types[u].name);
        const SpriteImages idle = rts::render::paint_unit(d.art.units[u], d.art.style, Pose::Idle);
        REQUIRE_FALSE(idle.body.empty());
        CHECK(idle.body.opaque_pixels() > 40);
        CHECK(anchor_inside(idle.body));
        // Toda figura lleva algo del color de su jugador.
        CHECK_FALSE(idle.team.empty());
        // La capa del jugador va en grises: se tiñe al dibujar.
        bool gray = true;
        for (std::size_t i = 0; i + 3 < idle.team.rgba.size(); i += 4) {
            gray = gray && idle.team.rgba[i] == idle.team.rgba[i + 1] && idle.team.rgba[i + 1] == idle.team.rgba[i + 2];
        }
        CHECK(gray);
        const SpriteImages step = rts::render::paint_unit(d.art.units[u], d.art.style, Pose::StepA);
        const SpriteImages strike = rts::render::paint_unit(d.art.units[u], d.art.style, Pose::Strike);
        CHECK_FALSE(same_pixels(idle.body, step.body));
        // Quien golpea (lleva arma o es un ingenio) cambia al golpear; la acémila y la
        // carreta no combaten.
        const auto& art = d.art.units[u];
        const bool fights = art.figure == rts::render::Figure::Ram || art.figure == rts::render::Figure::Trebuchet ||
                            ((art.figure == rts::render::Figure::Person || art.figure == rts::render::Figure::Rider) &&
                             art.weapon != rts::render::Weapon::None && art.weapon != rts::render::Weapon::Satchel);
        if (fights) {
            CHECK_FALSE(same_pixels(idle.body, strike.body));
        }
    }
}

TEST_CASE("Arte: el mismo arte da los mismos píxeles (generación determinista)") {
    const auto& d = game_data();
    const auto a = rts::render::paint_building(d.art.buildings[0], d.art.style, d.engine.view);
    const auto b = rts::render::paint_building(d.art.buildings[0], d.art.style, d.engine.view);
    CHECK(same_pixels(a.body, b.body));
    const auto t1 = rts::render::paint_terrain(d.art.terrain[0], d.engine.view, 1);
    const auto t2 = rts::render::paint_terrain(d.art.terrain[0], d.engine.view, 1);
    CHECK(same_pixels(t1, t2));
}

TEST_CASE("Arte: edificios con altura salen por encima de su huella; campos y caminos, a ras de suelo") {
    const auto& d = game_data();
    const auto& view = d.engine.view;
    for (std::size_t b = 0; b < d.art.buildings.size(); ++b) {
        CAPTURE(d.buildings.types[b].name);
        const auto& art = d.art.buildings[b];
        const SpriteImages s = rts::render::paint_building(art, d.art.style, view);
        REQUIRE_FALSE(s.body.empty());
        CHECK(anchor_inside(s.body));
        const float above = s.body.anchor.y;  // del ancla (centro de la huella) hacia arriba
        const float half_foot = static_cast<float>(view.tile_height_px * art.size_tiles) * 0.5f;
        const bool flat = art.shape == rts::render::BuildingShape::Field || art.shape == rts::render::BuildingShape::Road;
        const bool low = art.shape == rts::render::BuildingShape::Tents || art.shape == rts::render::BuildingShape::Stalls;
        if (flat) {
            CHECK(above <= half_foot + 2.0f);
        } else if (!low) {  // tiendas y puestos son bajos y no llenan la huella
            // Por encima de la esquina de atrás del bloque, al menos medio muro.
            CHECK(above > half_foot * static_cast<float>(art.inset_percent) / 100.0f + static_cast<float>(art.wall_px) * 0.5f);
        }
        // Con estandarte, capa del jugador; sin estandarte, lonas ni toldos, ninguna.
        const bool team = art.banner || art.shape == rts::render::BuildingShape::Stalls;
        CHECK(s.team.empty() == !team);
    }
}

TEST_CASE("Arte: el terreno cubre su rombo entero (sin juntas) y deja fuera las esquinas") {
    const auto& d = game_data();
    const auto& view = d.engine.view;
    for (std::size_t t = 0; t < d.art.terrain.size(); ++t) {
        const Image img = rts::render::paint_terrain(d.art.terrain[t], view, 0);
        CHECK(img.width == view.tile_width_px);
        CHECK(img.height == view.tile_height_px);
        CHECK(img.at(view.tile_width_px / 2, view.tile_height_px / 2)[3] == 255);
        CHECK(img.at(view.tile_width_px / 2, 0)[3] == 255);  // vértice de arriba
        CHECK(img.at(0, 0)[3] == 0);
        CHECK(img.at(view.tile_width_px - 1, view.tile_height_px - 1)[3] == 0);
    }
}

TEST_CASE("Arte: los árboles varían y uno de cada tres es un pino") {
    const auto& d = game_data();
    const auto tree = std::ranges::find(d.art.nodes, rts::render::NodeShape::Tree, &rts::render::NodeArt::shape);
    REQUIRE(tree != d.art.nodes.end());
    REQUIRE(tree->variants >= 3);
    const Image a = rts::render::paint_node(*tree, d.art.style, d.engine.view, 0);
    const Image b = rts::render::paint_node(*tree, d.art.style, d.engine.view, 1);
    const Image pine = rts::render::paint_node(*tree, d.art.style, d.engine.view, 2);
    CHECK_FALSE(same_pixels(a, b));
    CHECK_FALSE(same_pixels(a, pine));
    // Un pino es más estrecho que un árbol frondoso.
    CHECK(pine.width < a.width);
}

TEST_CASE("Atlas: todo el arte cabe, sin solaparse, y cada sprite apunta a sus píxeles") {
    const auto& d = game_data();
    const Atlas atlas = rts::render::build_atlas(d.engine.view, d.art);
    CHECK(atlas.has_art());
    CHECK(atlas.width <= 2048);
    std::vector<rts::render::AtlasRegion> regions;
    for (const auto& poses : atlas.units) {
        for (const auto& p : poses) {
            REQUIRE(p.body.valid());
            regions.push_back(p.body.region);
            if (p.team.valid()) {
                regions.push_back(p.team.region);
            }
        }
    }
    for (const auto& b : atlas.buildings) {
        REQUIRE(b.body.valid());
        regions.push_back(b.body.region);
    }
    for (const auto& variants : atlas.nodes) {
        for (const auto& v : variants) {
            REQUIRE(v.valid());
            regions.push_back(v.region);
        }
    }
    for (const auto& f : atlas.flames) {
        REQUIRE(f.valid());
        regions.push_back(f.region);
    }
    REQUIRE(atlas.smoke.valid());
    REQUIRE(atlas.ground_ring.valid());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        CHECK(r.x_px + r.width_px <= atlas.width);
        CHECK(r.y_px + r.height_px <= atlas.height);
        for (std::size_t k = i + 1; k < regions.size(); ++k) {
            const auto& o = regions[k];
            const bool overlap = r.x_px < o.x_px + o.width_px && o.x_px < r.x_px + r.width_px &&
                                 r.y_px < o.y_px + o.height_px && o.y_px < r.y_px + r.height_px;
            CHECK_FALSE(overlap);
        }
    }
    // Los píxeles del atlas son los de la imagen pintada.
    const SpriteImages idle = rts::render::paint_unit(d.art.units[0], d.art.style, Pose::Idle);
    const auto& r = atlas.units[0][0].body.region;
    REQUIRE(r.width_px == idle.body.width);
    for (std::int32_t y = 0; y < r.height_px; y += 3) {
        for (std::int32_t x = 0; x < r.width_px; x += 3) {
            CHECK(atlas.texel(r.x_px + x, r.y_px + y) == idle.body.at(x, y));
        }
    }
    CHECK(atlas.flat_buildings.size() == d.art.buildings.size());
}

TEST_CASE("Escena con arte: lo de delante se pinta después; espejo; obra a medias; fuego y humo") {
    const auto& d = game_data();
    const auto& view = d.engine.view;
    const Atlas atlas = rts::render::build_atlas(view, d.art);
    std::vector<std::array<std::uint8_t, 3>> colors;
    for (const auto& t : d.terrain.types) {
        colors.push_back(t.color);
    }
    rts::render::SceneBuilder builder(view, atlas, colors);
    const rts::render::IsoProjection proj(view);
    rts::render::Scene scene;
    scene.screen = {1280.0f, 720.0f};
    scene.camera.center_on(proj.tile_to_world({50.0f, 50.0f}), scene.screen);

    // Un edificio y dos figuras: una detrás (y menor) y otra delante.
    std::vector<rts::render::SceneObject> objects(1);
    objects[0].origin = {50, 50};
    objects[0].size = d.buildings.types[0].type.size;
    objects[0].art = rts::render::SceneObject::Art::Building;
    objects[0].art_type = 0;
    objects[0].team = {255, 0, 0, 255};
    const rts::render::Vec2 center =
        scene.camera.world_to_screen(proj.tile_to_world({50.0f + 1.5f, 50.0f + 1.5f}));
    std::vector<rts::render::Marker> markers(2);
    for (auto& m : markers) {
        m.unit_type = 0;
        m.owner = {0, 0, 255, 255};
    }
    markers[0].screen_pos = center + rts::render::Vec2{0.0f, 200.0f};   // delante
    markers[1].screen_pos = center + rts::render::Vec2{0.0f, -200.0f};  // detrás
    markers[1].mirror = true;
    scene.objects = objects;
    scene.markers = markers;

    rts::render::SpriteBatch batch;
    builder.build(scene, batch);
    const auto& b_body = atlas.buildings[0].body.region;
    const auto& u_body = atlas.units[0][0].body.region;
    std::vector<std::size_t> building_at;
    std::vector<std::pair<std::size_t, float>> unit_at;  // índice, y
    const auto inst = batch.instances();
    for (std::size_t i = 0; i < inst.size(); ++i) {
        if (inst[i].u0 == b_body.u0 && inst[i].v0 == b_body.v0) {
            building_at.push_back(i);
        }
        if (inst[i].u0 == u_body.u0 && inst[i].v0 == u_body.v0) {
            unit_at.emplace_back(i, inst[i].y);
            if (inst[i].y < center.y) {
                CHECK(inst[i].w < 0.0f);  // la de detrás mira a la izquierda: ancho negativo
            }
        }
    }
    REQUIRE(building_at.size() == 1);
    REQUIRE(unit_at.size() == 2);
    for (const auto& [index, y] : unit_at) {
        if (y < center.y) {
            CHECK(index < building_at[0]);  // detrás: antes que el edificio
        } else {
            CHECK(index > building_at[0]);  // delante: después
        }
    }

    // En obra se ve solo la parte de abajo; ardiendo, llamas y humo.
    objects[0].build_permille = 0;
    objects[0].fire_permille = 1000;
    scene.objects = objects;
    scene.markers = {};
    batch.clear();
    builder.build(scene, batch);
    bool cropped = false;
    std::size_t flames = 0;
    std::size_t smoke = 0;
    for (const auto& i : batch.instances()) {
        if (i.u0 == b_body.u0 && i.v0 > b_body.v0) {
            cropped = true;
            CHECK(i.h < static_cast<float>(b_body.height_px));
        }
        for (const auto& f : atlas.flames) {
            flames += i.u0 == f.region.u0 && i.v0 == f.region.v0 ? 1 : 0;
        }
        smoke += i.u0 == atlas.smoke.region.u0 && i.v0 == atlas.smoke.region.v0 ? 1 : 0;
    }
    CHECK(cropped);
    const auto expected_flames =
        static_cast<std::size_t>(view.flames_per_tile * d.buildings.types[0].type.size);
    CHECK(flames == expected_flames);
    CHECK(smoke == expected_flames * 3);
}
