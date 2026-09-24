# RTS medieval con capa de logística (nombre provisional)

RTS medieval en C++23, de la escuela de Tzar y Age of Empires II, con una capa de logística opcional modelada como grafo.
Todo el código y todos los recursos son originales o de licencia compatible. No se usa ningún contenido del juego original.

## Estado

| Hito | Contenido | Estado |
|------|-----------|--------|
| M0 | Esqueleto: ventana, paso fijo, ImGui, CMake + vcpkg, CI Windows/Linux | hecho, en revisión |
| M1 | Mapa isométrico por casillas, cámara, selección por rectángulo | hecho, en revisión |
| M2 | Unidades, HPA*, campos de flujo, evitación local, banco de 1000/2000 unidades | **en curso** |
| M3 | Economía | — |
| M4 | Combate y experiencia | — |
| M5 | Repeticiones deterministas | — |
| M6 | Logística | — |
| M7 | IA | — |
| M8 | Multijugador lockstep | — |

## Requisitos

| | Mínimo | Motivo |
|---|---|---|
| CMake | 3.28 | |
| Linux | Ubuntu 24.04, **Clang ≥ 19** o **GCC ≥ 14**, Ninja | Clang 18 no ve `std::expected` en libstdc++ (medido: define `__cpp_concepts = 201907L`, libstdc++ exige `202002L`) |
| Windows | Visual Studio 2022 o posterior (la CI usa VS 2026, MSVC 14.51) con MSVC o clang-cl, Ninja | |
| GPU | Vulkan 1.0 (Linux y Windows) o Direct3D 12 nivel 11_0 (Windows) | Requisitos de SDL_GPU, ver `SDL_gpu.h` |
| vcpkg | cualquier versión reciente, con `VCPKG_ROOT` definido | modo manifiesto, baseline fijado en `vcpkg.json` |

Paquetes de sistema en Ubuntu 24.04:

```sh
sudo apt-get install ninja-build clang-20 g++-14 pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
  libxfixes-dev libxtst-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev
# opcional, para ejecutar con ventana sin GPU (como la CI): xvfb mesa-vulkan-drivers
```

## Compilar y ejecutar

```sh
cmake --preset linux-clang-debug         # o windows-msvc-debug, windows-clang-cl-debug, linux-gcc-debug
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug

./build/linux-clang-debug/src/rts                           # con ventana
./build/linux-clang-debug/src/rts --frames 600              # con ventana, sale tras 600 fotogramas e imprime tiempos
./build/linux-clang-debug/src/rts --headless --ticks 12000  # sin ventana: imprime el hash de estado
./build/linux-clang-debug/src/rts --data <carpeta>          # otra carpeta de datos
```

Controles:

| Acción | Entrada |
|---|---|
| Desplazar la cámara | Flechas o WASD; ratón en el borde de la ventana |
| Seleccionar | Clic sobre un marcador, o arrastre con el botón izquierdo |
| Añadir a la selección | Mayús + clic o arrastre |
| Vaciar la selección | Esc |
| Mover la selección | Clic derecho sobre el destino |

Cada preset tiene su versión `-release` (RelWithDebInfo). Opciones de CMake: `RTS_WERROR` (ON), `RTS_TRACY` (OFF) y `RTS_BUILD_TESTS` (ON).

## Arquitectura

```
src/sim/       simulación pura: sin SDL, sin E/S, sin coma flotante
src/render/    presentación: lee snapshots e interpola
src/platform/  ventana, entrada, reloj, perfilado
src/game/      pegamento: bucle, configuración, arranque
src/render/shaders/  HLSL: dxc lo compila a SPIR-V (Vulkan) y DXIL (D3D12) y se empaqueta en el binario
src/tools/     editor de mapas (pendiente)
data/          TOML de diseño; se copia junto al ejecutable
tests/         doctest + pruebas de CTest
```

Grafo de bibliotecas (las flechas solo bajan):

```
rts ─┬─ rts_render ─┬─ rts_platform ── SDL3
     │              ├─ imgui, shaders (dxc)
     │              └─ rts_render_core     proyección, atlas, escena: sin SDL, se prueba sin GPU
     ├─ rts_game_core ── toml++, rts_render_core   cámara, selección, configuración
     └─ rts_sim ── EnTT                    sin SDL, sin E/S, sin coma flotante
```

`rts_sim` no enlaza SDL, así que un `#include <SDL3/...>` en `src/sim/` no compila.

### Render (M1)

- **Proyección isométrica 2:1.** La casilla (i, j) tiene su esquina superior en ((i−j)·w/2, (i+j)·h/2) píxeles del mundo.
- **Recorte.** `for_each_visible_tile` recorre la pantalla por filas r = i + j, así que visita solo las casillas visibles y ya en orden de pintado. A 1920×1080 son 2104 casillas de 65 536 (medido en `test_projection`).
- **Un solo draw call.**
  - Cada sprite es una instancia de 28 B (posición, tamaño, UV en 16 bits, color RGBA8).
  - El vertex shader genera el quad a partir de `SV_VertexID`.
  - Todo el lote se sube por un búfer de transferencia que SDL recicla (`cycle = true`) y se dibuja con `SDL_DrawGPUPrimitives(6, N)`.
- **Atlas generado por código.** Mientras no haya arte propio es un rombo, un disco, un anillo y un bloque sólido, todos en blanco; el color de la instancia los tiñe. Con casillas de 64×32 el atlas ocupa 128×128 RGBA8 (64 KiB).
- **El mapa no se copia por tick.** El `Snapshot` comparte el `TileMap` por `shared_ptr<const>`. El render recalcula los colores por casilla (256 KiB) solo cuando cambia `TileMap::revision()`.

### Las tres reglas duras y cómo se hacen cumplir

1. **Simulación y presentación separadas.** La simulación escribe un `Snapshot` por tick y el render interpola entre los dos últimos con `alpha ∈ [0, 1)`. El render nunca toca el `entt::registry`.
2. **Determinismo.**
   - Toda la simulación usa `Fixed` (16.16 sobre `int32_t`) y enteros.
   - La aleatoriedad sale de xoshiro256++, con semilla explícita que se expande con splitmix64.
   - La prueba de CTest `sim_purity` falla si en `src/sim/` aparece `float`, `double`, `<cmath>`, `<chrono>`, `<random>`, `unordered_*`, E/S o SDL.
   - La CI compila con cuatro compiladores (MSVC, clang-cl, Clang 20 y GCC 14) y exige el mismo hash de estado en los cuatro.
3. **Paso fijo a 20 Hz.**
   - El tick dura exactamente 50 000 000 ns.
   - El reloj acumula nanosegundos enteros y ejecuta como mucho `loop.max_ticks_per_frame` ticks por fotograma. El retraso que sobre se descarta para evitar la espiral de la muerte.

### Movimiento (M2)

- **Órdenes.** `sim::Command{tick, jugador, tipo, unidades, destino}` se encola con `World::issue` y se aplica al inicio del tick indicado, en orden de llegada. Es la unidad que grabarán las repeticiones (M5) y que viajará en lockstep (M8).
- **Rejilla.**
  - 8 direcciones con coste entero octil: 10 en recto, 14 en diagonal.
  - Prohibido atajar esquinas bloqueadas.
  - Las componentes conexas precalculadas descartan al instante los destinos inalcanzables, que se sustituyen por la casilla alcanzable más cercana.
- **HPA\*.**
  - Sectores de 16×16, con portales en los tramos abiertos de cada borde (como mucho 8 casillas por portal) y aristas internas precalculadas con Dijkstra acotado al sector.
  - Una consulta es un A\* sobre el grafo abstracto (1626 nodos y 11 390 aristas en el mapa por defecto), más un refinado perezoso tramo a tramo.
  - Medido frente a A\* óptimo en 200 rutas: 3,6 % más largas de media y 15,7 % en el peor caso.
- **Presupuesto determinista.** El planificador expande como mucho `path_node_budget_per_tick` nodos por tick. Se mide en nodos, no en milisegundos, para que el reparto sea idéntico en todas las máquinas.
- **Campos de flujo.**
  - Una orden a 8 o más unidades usa un único Dijkstra desde el destino, limitado al pasillo de sectores del camino abstracto.
  - Cada unidad tiene su hueco de llegada: su desplazamiento respecto al centroide del grupo, comprimido al radio de un racimo compacto (el "magic box" de StarCraft).
- **Evitación local "RVO muestreado".**
  - Rejilla espacial por ordenación por conteo y hasta 8 vecinas a menos de 2 casillas.
  - 18 velocidades candidatas, puntuadas por desvío más el riesgo de colisión en 20 ticks, con velocidad relativa recíproca `2v' − vᵢ − vⱼ`.
  - La separación de solapamientos es de tipo Jacobi, así que no depende del orden de las unidades.
- **Llegada.**
  - En el radio del destino.
  - En cadena, al tocar a una compañera ya llegada dentro del radio esperado del racimo.
  - Por atasco, tras `stuck_arrive_ticks` casi parada.
  - Las llegadas quedan quietas y son obstáculos para las demás.
- **Sin coma flotante.** Todo el movimiento usa `Fixed` y enteros; la raíz cuadrada es entera (`isqrt`). El mismo hash tras 1200 ticks de movimiento con Clang 20 y GCC 14.

### Convenciones de `data/`

- Solo enteros. Una magnitud fraccionaria se escribe en una unidad menor que figura en el nombre de la clave: `max_speed_milli_tiles_per_tick = 150` significa 0,150 casillas por tick. Así la carga no depende del redondeo decimal→binario de cada plataforma.
- Las duraciones de la simulación van en ticks.
- `data/terrain.toml`: el id de cada terreno es su posición en la lista. Las bandas del generador (`[[map.bands]]` en `engine.toml`) nombran terrenos, no ids.
- Los colores son listas `[r, g, b]` o `[r, g, b, a]` de 0 a 255.
- La frecuencia de 20 Hz es una constante de arquitectura (`sim::kTicksPerSecond`), no un dato: cambiarla invalida los datos y las repeticiones.

### Punto fijo: límites que hay que conocer

- El rango es ±32 768 con una resolución de 1/65 536.
- La distancia al cuadrado en la diagonal de un mapa de 256×256 vale 131 072 y no cabe en 16.16. Para eso está `mul_wide()`, que devuelve el producto en 32.32 sobre `int64_t`.
- Si una operación desborda, salta un `assert` en Debug. En Release el resultado envuelve módulo 2³² (comportamiento definido desde C++20), así que es incorrecto pero sigue siendo determinista.
- `mul` redondea hacia −∞ y `div` hacia cero.

## Pruebas

| Prueba | Qué garantiza |
|---|---|
| `unit` | Punto fijo, RNG contra los vectores de referencia de los autores, reloj de paso fijo, configuración, generación de mapas, proyección y recorte (contra fuerza bruta en 200 cámaras), cámara, selección, atlas, escena, regresión por hash (mapa de 256×256 + 1000 entidades, 12 000 ticks) |
| `sim_purity` | Regla 2: `src/sim/` limpio de tokens prohibidos |
| `headless_smoke` | El ejecutable arranca, lee `data/` y simula un minuto sin ventana ejecutando `data/scenarios/headless.toml` |
| CI "Humo con ventana" | En Linux, con Xvfb y lavapipe (Vulkan por software), crea el dispositivo SDL_GPU, compila el pipeline, sube el atlas y presenta 120 fotogramas |
| CI `determinism` | El hash tras 2400 ticks del guion de `data/scenarios/headless.toml` es idéntico en MSVC, clang-cl, Clang y GCC |
| CI `bench` | `rts_bench` en Release con 1000 y 2000 unidades: falla si algún tick supera 50 ms |

Si el hash de regresión cambia **sin** cambio de diseño, es un fallo. Si el cambio de diseño es intencionado, se actualiza `kExpectedHash` en el mismo commit y se justifica en el mensaje.

## Rendimiento medido

Release, Clang 20, contenedor de 4 núcleos. Guion de `rts_bench`: un grupo grande con campo de flujo, dos mitades que se cruzan y 400 grupos de 5 con HPA\* individual.

| Unidades | Media | p99 | Máximo | Criterio |
|---|---|---|---|---|
| 1000 | 2,99 ms/tick | 6,39 ms | 10,1 ms | ≤ 50 ms ✅ |
| 2000 | 6,68 ms/tick | 10,8 ms | 15,9 ms | ≤ 50 ms ✅ |

| Otras medidas | Valor |
|---|---|
| Construir la escena de render (1963 sprites) | 0,036 ms/fotograma |
| Generar el mapa de 256×256 + el grafo HPA\* + la aparición de unidades | ~51 ms, una vez |

## Licencia

MIT, ver `LICENSE`. Las dependencias tienen sus propias licencias (zlib, MIT, BSD).
