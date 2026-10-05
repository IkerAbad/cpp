# RTS medieval con capa de logística (nombre provisional)

RTS medieval en C++23, de la escuela de Tzar y Age of Empires II, con una capa de logística opcional modelada como grafo.
Todo el código y todos los recursos son originales o de licencia compatible. No se usa ningún contenido del juego original.

## Estado

| Hito | Contenido | Estado |
|------|-----------|--------|
| M0 | Esqueleto: ventana, paso fijo, ImGui, CMake + vcpkg, CI Windows/Linux | hecho, en revisión |
| M1 | Mapa isométrico por casillas, cámara, selección por rectángulo | hecho, en revisión |
| M2 | Unidades, HPA*, campos de flujo, evitación local, banco de 1000/2000 unidades | hecho |
| M3 | Economía: 4 recursos, aldeanos, construcción, colas de producción | hecho |
| M4 | Combate con proyectiles esquivables, experiencia por unidad y héroes | hecho |
| M7 (adelantado) | IA básica que juega con las mismas reglas que un humano; victoria y derrota | hecho |
| M5 | Repeticiones deterministas: grabación automática, reproductor y verificación | hecho |
| M6 | Logística: víveres, munición, bagaje, convoyes, campamentos y trabuquete | hecho |
| M7 | IA más inteligente (varias dificultades por cómo juega, nunca por trampas) | en curso: percepción con niebla, 18 módulos |
| M8 | Multijugador lockstep | — |

El plan completo, con lo que falta y en qué orden, está en [docs/PLAN.md](docs/PLAN.md).

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
./build/linux-clang-debug/src/rts --replay <fichero.rtsrep>        # ver una repetición
./build/linux-clang-debug/src/rts --verify-replay <fichero.rtsrep> # reproducirla sin ventana y comprobar sus hashes
./build/linux-clang-debug/src/rts --headless --ticks 2400 --record r.rtsrep  # grabar el guion sin ventana
```

Cada partida con ventana se graba sola al cerrar en `replays/partida-<fecha>_<hora>.rtsrep`, junto al ejecutable.

Controles:

| Acción | Entrada |
|---|---|
| Desplazar la cámara | Flechas o WASD; ratón en el borde de la ventana |
| Seleccionar | Clic sobre un marcador, o arrastre con el botón izquierdo |
| Añadir a la selección | Mayús + clic o arrastre |
| Vaciar la selección | Esc |
| Mover la selección | Clic derecho sobre el destino |
| Recoger un recurso | Clic derecho sobre el nodo (árbol, arbusto, mina) con aldeanos seleccionados |
| Construir o descargar | Clic derecho sobre un edificio propio: si está en obra, se construye; si es almacén, se descarga |
| Colocar un edificio | Botón del panel «Selección» con aldeanos seleccionados; clic para colocar, Mayús + clic para varios, clic derecho o Esc para cancelar |
| Producir unidades | Clic sobre un edificio propio y botones de su panel; «Cancelar la última» devuelve el coste entero |
| Atacar | Clic derecho sobre una unidad o un edificio enemigo |
| Ataque-movimiento | Ctrl + clic derecho sobre el destino: van peleando con lo que encuentren |
| Postura | Botones «Agresiva» y «Mantener posición» del panel «Selección» |
| Repetición: pausa y velocidad | Espacio; teclas 1-4 para x1, x2, x4 y x8 (o el panel «Repetición») |

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

### Economía (M3)

- **Jugadores.** `PlayerState` guarda las existencias de los 5 recursos (comida, madera, piedra, oro y hierro), la población y el tope, todo en enteros. Cada unidad y cada edificio llevan un `Owner`. Una orden sobre entidades de otro jugador se ignora.
- **Objetos estáticos.**
  - Los nodos de recurso (árbol, arbusto, minas de 2×2) y los edificios son entidades con una huella (`Footprint`).
  - La huella bloquea sus casillas en una capa de ocupación de `PassGrid`. El `TileMap` del terreno no cambia nunca, así que los snapshots lo siguen compartiendo sin copiarlo.
- **Pathfinding incremental.** Los cambios de ocupación de un tick se aplican juntos:
  - Se reetiquetan las componentes conexas.
  - Se vuelven a detectar los portales de todos los bordes, que es barato.
  - Los Dijkstra internos se repiten solo en los sectores sucios o en los que cambió su conjunto de nodos.
  - El grafo resultante es idéntico, nodo a nodo y arista a arista, al construido desde cero. En la prueba con 12 rondas de 4 cambios se recalculan 64 sectores de 3072.
  - Se rehacen los campos de flujo cuyo pasillo toca un sector cambiado y se replanifican los caminos que ahora cruzan una casilla bloqueada.
- **Ciclo del aldeano** (componente `Worker`):
  - Va al nodo y recoge una unidad cada `gather_ticks`. Con `carry_capacity` encima, va al almacén propio más cercano que acepte ese recurso, descarga y vuelve.
  - Si el nodo se agota, busca otro del mismo recurso a menos de `retarget_radius_tiles`. Tiene que ser accesible (con una casilla vecina en su componente) y tener sitio: `gatherers_per_tile` aldeanos por casilla de huella.
  - Si cambia de recurso, pierde lo que llevaba.
- **Acercarse a un objeto.**
  - El aldeano va a la casilla de acceso del primer anillo más cercana dentro de su componente.
  - Está al alcance cuando el borde de su disco queda a `interact_range` del borde de la huella.
  - Tras `approach_attempts` intentos sin llegar, cambia de objetivo.
- **Edificios.**
  - Se cobran al colocarlos. La casilla debe tener terreno transitable, sin objetos y sin unidades encima.
  - Cada aldeano al alcance aporta un tick de trabajo por tick, así que la obra crece linealmente con el número de aldeanos. Medido: una casa de 300 ticks tarda 300 ticks con 1 aldeano y 100 con 3.
- **Colas de producción.**
  - Caben `queue_capacity` unidades. Se cobran al encolar y cancelar la última devuelve el coste entero.
  - Sin plazas de población la cola se detiene sin perder nada.
  - La unidad nueva aparece en una casilla libre alrededor del edificio, repartida entre las del primer anillo que tenga sitio.
- **Preparación de partida** (`[setup]` y `[[player]]` en `engine.toml`, RNG propio con `setup.seed`):
  - El edificio inicial va al sitio válido más cercano al pedido, en una región de al menos `min_start_region_tiles` casillas.
  - Alrededor se colocan los recursos garantizados.
  - Los bosques se pueblan de árboles con la densidad pedida, dejando un claro alrededor de cada inicio.
  - Cada jugador recibe sus aldeanos iniciales.
- **Interfaz.**
  - Barra de recursos y población.
  - Con aldeanos seleccionados, menú de construcción con un fantasma verde o rojo.
  - Con un edificio seleccionado, su cola de producción.
  - Clic derecho contextual.
  - Los marcadores llevan un anillo con el color del jugador y un punto con el color del recurso que cargan.
- **Pendiente, a sabiendas:**
  - Punto de reunión, granjas, derribo y reparación.
  - Nada garantiza que un recurso no quede encerrado por el bosque.
  - La construcción es lineal; el género suele dar rendimientos decrecientes.

### Combate (M4)

- **Estadísticas por tipo** (`units.toml`): vida, ataque y armadura de cuerpo a cuerpo y de proyectil, clase de armadura, daño extra contra clases, alcance entre bordes, recarga, radio de visión y velocidad del proyectil. Los edificios tienen armadura y clase en `buildings.toml`.
- **Daño por golpe**, estilo del género:
  `max(1, max(0, cuerpo − armadura_cuerpo) + max(0, proyectil − armadura_proyectil) + bonus[clase])`.
- **Resolución simultánea.** Todos los golpes de un tick se calculan sobre el estado del principio del tick, se suman y se aplican de una vez; las muertes, al final. Dos unidades iguales que se atacan mueren en el mismo tick (prueba incluida).
- **Proyectiles esquivables.** Son entidades que vuelan hacia donde estaba el blanco al disparar. Al caer, dañan al blanco previsto si sigue ahí; si no, a la unidad enemiga más cercana en ese punto o al edificio enemigo de la casilla. Si no hay nadie, fallan. Una unidad que cruza la línea de tiro esquiva la flecha (prueba incluida).
- **Blancos.**
  - Orden de ataque explícita.
  - Adquisición automática del enemigo más cercano dentro de la visión, cada `acquire_interval_ticks` repartidos por id, con una rejilla espacial propia del tick.
  - Persecución que se recalcula si el blanco se aleja `repath_tiles`, y abandono tras `chase_attempts` llegadas sin alcanzarlo.
  - Posturas: agresiva, o mantener posición (no persigue).
  - Ataque-movimiento: un movimiento de grupo que se desvía para pelear y luego sigue.
- **Experiencia al estilo Tzar.**
  - 1 punto por punto de daño hecho, más `xp_kill_bonus` por baja.
  - 12 niveles con umbrales en TOML. Cada nivel da un porcentaje de vida y de ataque, y armadura cada pocos niveles.
  - En el último nivel la unidad se vuelve héroe: nombre propio y aura de ataque para los aliados cercanos. Las auras no se acumulan.
- **Muerte.** La entidad se destruye. Un edificio libera su huella con la actualización incremental de M3 (prueba: el grafo queda igual que uno construido desde cero).
- **Presentación.** Barras de vida, proyectiles en vuelo, anillo dorado de héroe, nivel y experiencia en el panel.

### IA básica (adelanto de M7)

- **Dentro de la simulación.** Es determinista, corre igual en todas las máquinas y en red no habrá que enviar sus órdenes. Solo lee el estado y emite las mismas órdenes que un humano, que se validan igual.
- **Sin trampas.** No recibe recursos ni ventajas; paga lo mismo que el jugador (prueba: sus existencias nunca bajan de cero). Las dificultades futuras saldrán de jugar mejor, no de hacer trampas.
- **Qué hace** (una vez por segundo, `[ai]` en `engine.toml`):
  - Entrena aldeanos hasta su objetivo y los reparte entre recursos según porcentajes, solo entre los que quedan en el mapa.
  - Construye casas antes de quedarse sin plazas, el cuartel a partir de cierto número de aldeanos, granjas cuando no le queda comida natural cerca y almacenes junto a recursos lejanos.
  - Entrena en el cuartel la primera unidad de su ciclo que pueda pagar.
  - Ataca con ataque-movimiento cuando reúne una oleada (cada una mayor) y defiende su base: saca el ejército y refugia a los aldeanos amenazados.
- **Granjas.** Un edificio que, terminado, da comida a su dueño hasta agotarse; la comida natural no dura toda la partida.
- **Victoria y derrota.** Pierde quien se queda sin ningún edificio vital terminado (`vital = true` en `buildings.toml`: el centro urbano) después de haber tenido alguno; uno en obra no cuenta. Un jugador que nunca tuvo uno (pruebas, demo) pierde al quedarse sin unidades ni edificios. La derrota es definitiva y lo que le quede al perdedor se retira del mapa. `ataque_fuerza` apunta primero al edificio vital enemigo más cercano. La pantalla muestra el resultado.
- **Configuración.** En `engine.toml`, cada jugador lleva `controller = "humano"` o `"ia"`; por defecto juegas tú (jugador 0) contra la IA. Un jugador de la IA elige su perfil con `ai_profile`.
- **Módulos y perfiles.** La IA se compone de tres capas:
  - **Percepción:** un resumen de lo que el jugador sabe, rehecho en cada decisión. Es el único sitio que lee el mundo; cuando haya niebla de guerra, filtrará con la misma visibilidad que un humano.
  - **Módulos:** `defensa`, `aldeanos`, `casas`, `cuartel`, `granjas`, `almacenes`, `obras`, `recoleccion`, `ejercito` y `ataque`. Comparten el presupuesto de la decisión.
  - **Perfiles** (`[[ai.profile]]`): qué módulos usa, en qué orden y con qué umbrales. Una dificultad nueva es un perfil con más módulos o módulos que piensan mejor; ningún parámetro de un perfil toca la economía ni las reglas.
  - Perfiles: `basica` (la IA original, de referencia) y `normal` (por defecto).
- **Qué añade `normal`.** Juega con las mismas reglas y la misma información; solo decide mejor:
  - `ejercito_contra`: entrena el tipo que más rinde contra lo que tiene el rival, por unidad de coste. Contra un ejército, compara cuánto mata por tick con cuánto le matan. Sin unidades armadas enemigas, elige lo que antes derriba sus edificios. Todo sale de la fórmula de daño y de los datos.
  - `ataque_fuerza`: ataca cuando la fuerza estimada de su ejército (vida × daño por tick) llega al 130 % de la enemiga conocida, y se retira si en la batalla baja del 60 %. Esa histéresis evita que oscile entre atacar y retirarse.
  - `concentrar`: cada unidad que pelea remata al enemigo armado que necesita menos golpes suyos. Los aldeanos enemigos no son prioridad: primero, lo que amenaza al ejército.
  - Economía: dos aldeanos en cola y casas con más margen.
- **Medido** con `rts_ai_match`, 40 partidas de 30 minutos (20 semillas con los lados cambiados): `normal` gana a `basica` 32 de 40 (80 %), 11 de ellas por derrota (al caer el centro urbano) y el resto a los puntos (valor vivo de unidades y edificios). Rematar exige asedio (arietes).

### Repeticiones (M5)

- **Qué se graba.** Una copia de los ficheros de `data/` (unos 20 KB) y las órdenes humanas, cada una con el tick en que se emitió. La IA vive dentro de la simulación y es determinista: sus órdenes no se graban, se regeneran. Una repetición se reproduce con los datos con que se jugó aunque luego cambien los de `data/`.
- **Comprobación.** Cada 10 s de juego (`[replay]` en `engine.toml`) se graba el hash de estado, y al final el hash final. Al reproducir, el primer hash distinto localiza la divergencia con esa resolución.
- **Formato `.rtsrep`.** Binario little-endian explícito: cabecera, versión del formato, hash de los datos, ficheros, órdenes, checkpoints y una suma FNV-1a de todo. Se rechaza un fichero ajeno, truncado o corrupto, o de otra versión, y el error dice cuál de los casos es.
- **Reproductor.** `rts --replay`: pausa, x1/x2/x4/x8, sin órdenes, con el estado de la verificación en pantalla. No hay retroceso.
- **Límite conocido.** Si cambia el formato de los datos (por ejemplo, los perfiles de IA), las repeticiones anteriores no se pueden cargar y el error dice qué clave falta.
- **Límite conocido.** El fichero no guarda la versión del código. Si cambia la simulación, las repeticiones antiguas divergen y se avisa; no se reproducen mal en silencio.

### Unidades con criterio histórico (fase 1 del rediseño)

Los papeles y costes reflejan el equipo y la instrucción de cada tipo, que es lo que después tendrá que mover la logística (hierro para armas y armaduras, oro para la paga, comida para el sustento):

| Unidad | Coste | Instrucción | Papel |
|---|---|---|---|
| Leva | comida, madera | 15 s | Lanza y escudo; masa barata; bonus contra caballería |
| Hombre de armas | comida, hierro, oro | 35 s | Armadura de hierro; choque contra infantería |
| Arquero | comida, madera | 45 s | Mucho volumen de tiro; flojo contra armadura |
| Ballestero | comida, madera, hierro | 17 s | Recarga lenta; mucho daño por disparo, perfora armadura |
| Jinete | comida, oro | 30 s | Rápido; exploración e incursiones |
| Caballero | comida, hierro, oro | 55 s | Armadura y carga; carísimo |

### Fuego y asedio (fase 1 del rediseño)

Una incursión quema y empobrece; solo un asedio conquista.

- **Fuego.** Sin armas de asedio no se daña un edificio. Cada golpe de otra unidad (antorchas; en los tiradores, flechas incendiarias) aviva un fuego (`ignite` en `units.toml`).
  - Por debajo de la intensidad de sostén, el fuego mengua y se apaga solo: hacen falta varios golpes seguidos.
  - Por encima, crece, quema vida en proporción a su intensidad y, muy vivo, prende los edificios de madera a una casilla o menos.
  - Cualquier unidad lo apaga (clic derecho sobre el edificio propio en llamas), cada tipo con su eficiencia (`extinguish`). Los aldeanos son los mejores.
  - Todo en `[fire]` de `engine.toml`.
- **Material** (`material` en `buildings.toml`).
  - La madera arde hasta caer: una casa sola, en unos 36 s.
  - La piedra (el centro urbano) solo pierde por el fuego el tejado y el interior: queda quemada e inutilizada (sin plazas, producción ni almacén), en pie, hasta que la reparan aldeanos con madera (`repair_cost_percent`).
- **Asedio.** El ariete (cobertizo con pieles húmedas: inmune al fuego, casi inmune a las flechas, sin defensa cuerpo a cuerpo y lento) es lo único que daña la piedra. Se construye en el taller de asedio, que exige tener un cuartel terminado (`requires` en `buildings.toml`; sin él, la orden se rechaza sin cobrar).
- **IA `normal`.**
  - `apagar`: manda aldeanos a sus fuegos.
  - `incendiar`: incursiones de jinetes contra edificios de madera sin defensa.
  - `taller`: construye el taller de asedio.
  - `ejercito_contra`: entrena cada unidad en el edificio que la produce. Sin ejército enemigo ahorra para lo que derriba la piedra (el ariete). Con pocos aldeanos solo entrena tropas si su ejército es más débil que el enemigo, para que la economía vaya primero.

### Escombros, demolición y minado (fase 2)

- **Escombros.** Lo derribado por asedio deja en su solar un nodo con el 50 % del coste de su material (`salvage_percent`): piedra si era de piedra, madera si era de madera. Bloquea el solar hasta que los aldeanos lo recogen y lo llevan a un almacén, como cualquier recurso (también el enemigo puede saquearlo). Lo que arde no deja nada aprovechable.
- **Demolición controlada.** Mayús + clic derecho con aldeanos sobre un edificio propio: lo desmontan al ritmo de construir y queda en escombros para transportar, no como reembolso instantáneo.
- **Zapador** (taller de asedio). Mina bajo la piedra: su ataque ignora la armadura de los muros, y bajo tierra las flechas apenas le alcanzan. Es más lento que el ariete. Contra la madera solo prende fuego, como cualquiera.

### Logística (M6)

Un ejército no vive del aire. Fuentes: ["Military logistics"](https://en.wikipedia.org/wiki/Military_logistics) («Ammunition could not as a rule be obtained locally») y ["English longbow"](https://en.wikipedia.org/wiki/English_longbow) (flechas «supplied in sheaves normally of 24 arrows»).

- **Víveres.** Cada unidad lleva raciones (`rations`, `ration_ticks` en `units.toml`): un soldado come 1 por minuto y lleva para 6 min. La caballería gasta el triple, por el forraje. Los aldeanos comen 1 cada 2 min y la reponen donde descargan.
- **Munición.** Los tiradores llevan 24 disparos (`ammo`). Sin munición no disparan ni buscan blancos.
- **Hambre.** Sin raciones, el ataque baja al 60 % y los aldeanos trabajan a la mitad. Tras 1 min de hambre las tropas pierden vida hasta morir; los aldeanos, no.
- **Reposición.** Poco a poco, junto a una fuente propia que paga lo que da (`[supply]` en `engine.toml`):
  - un edificio que abastece (`supplies`: centro urbano, molino, cuartel) paga del almacén del jugador: comida por ración; madera, y hierro para los virotes, por munición;
  - un campamento de campaña (`store_capacity`) paga de su propio almacén, que llenan los convoyes;
  - una acémila o carreta cargada (`convoy_capacity`) paga de su carga. Es un depósito móvil, y sus animales también comen de ella.
- **Convoyes.** Bagaje + clic derecho:
  - sobre un campamento: ruta de convoy (carga en casa, descarga allí y repite);
  - sobre otro edificio que abastece: carga y se queda.

  La acémila carga como un hombre y medio; la carreta, cinco veces más, a la mitad de velocidad.
- **Trabuquete** (de contrapeso). Se transportaba desmontado en carros y lo volvía a montar un maestro de ingenios ([Trebuchet](https://en.wikipedia.org/wiki/Trebuchet): «deconstructed for transportation to their destination, whether on carts or by ship»).
  - Aquí se monta en un campamento de campaña, pagado con su almacén: madera y hierro traídos en convoy. Cada viaje lleva lo que le falta al campamento para su `store_target`, contando lo que ya está en camino.
  - Montado no se mueve (velocidad 0). Más lento que el ariete, pero derriba a 10 casillas sin arrimarse al muro.
- **IA:**
  - `abastecer`: la tropa que no pelea y baja del 30 % vuelve a la fuente más cercana que puede darle algo (edificio o bagaje) y espera a llenarse.
  - `logistica`: dos acémilas siguen al ejército en campaña unas casillas por detrás y vuelven a casa a cargar.
  - Ambos perfiles guardan comida para las raciones (`upkeep_reserve_percent`).
  - Asedio: solo con su ejército en campaña a 20 casillas o menos del objetivo (`siege_front_tiles`), campamento a 9 casillas de él, convoyes hacia allí y hasta 2 trabuquetes montados con lo que traen (`siege_engines`). Medido: un campamento sin el ejército delante se perdía enseguida; con esta condición, ni gana ni pierde partidas (10 a 10 contra la misma IA sin asedio): los convoyes rara vez reúnen un trabuquete antes de los 30 minutos.
  - `incendiar` va antes a por el bagaje enemigo sin escolta (cortar convoyes) que a quemar edificios.
- **Torneo con traza:** `rts_ai_match --trace 120` imprime cada 2 min aldeanos, ejército, hambre, munición, bagaje, campamentos, almacén y lo cerca que está su ejército del centro enemigo.

### Sanidad

Un herido no vuelve solo al combate: hay que llevarlo a un puesto médico.

Fuentes:
- [Medieval medicine of Western Europe](https://en.wikipedia.org/wiki/Medieval_medicine_of_Western_Europe): los cirujanos trataban sobre todo heridas de filo («skin lacerations caused by a sharp edge, such as by a sword, dagger and axe»), y extraer flechas era lento.
- [Knights Hospitaller](https://en.wikipedia.org/wiki/Knights_Hospitaller): los hospitales de las órdenes acogían a «sick, poor, or injured».

**Puestos** (`beds`, `nurses`, `care_percent`, `heal_to_percent` en `buildings.toml`):

| Puesto | Material | Camas | Enfermeros | Cuidados | Cura hasta |
|---|---|---|---|---|---|
| Puesto de socorro (junto al frente) | madera | 4 | 1 | 70 % | 70 %: estabiliza; lo que queda es leve y sana solo |
| Hospital de campaña | madera | 10 | 3 | 100 % | 100 % |
| Hospital, como los de las órdenes | piedra | 20 | 6 | 150 % | 100 % |

**Cómo funciona** (`[medicine]` en `engine.toml`):
- **Ingreso:** heridos + clic derecho sobre el puesto. Ingresado, el paciente queda inoperativo y sin bienes (pierde víveres y munición). Se le puede atacar y, si el puesto cae, muere. Si no hay cama libre, espera a la puerta.
- **Cuidados:** cura el tiempo (reposo) y, sobre todo, el personal: aldeanos o cirujanos + clic derecho sobre el puesto. Cada miembro atiende a 3 pacientes con su pericia (`care_skill`). El aldeano hace de enfermero (100). El barbero cirujano (`cirujano`), oficio de pago que se forma en los hospitales, cura con 250 y ocupa antes que nadie las plazas de personal del puesto. Fuente: [Barber surgeon](https://en.wikipedia.org/wiki/Barber_surgeon), que documenta barberos cirujanos en la Valencia del siglo XV. Un soldado con 30 de herida tarda unos 45 s atendido en un hospital de campaña y unos 5 min sin atender.
- **Comida:** los pacientes comen como un aldeano, del almacén del jugador; sin comida no mejoran.
- **Alta:** sale sin víveres ni munición y tarda 20 s en reorganizarse antes de poder atacar. Cualquier otra orden lo saca antes de tiempo, con la misma reorganización.
- **Curación natural:** fuera de los puestos solo sanan solas las heridas leves (70 % de vida o más), con la unidad comida y 10 s sin recibir daño. Las graves no.
- **IA:** aún no usa la sanidad; se le enseñará cuando toda la estructura del juego esté terminada.

### Niebla de guerra

Solo se sabe lo que alguien ha visto, y lo mismo para la IA, que no tiene ventaja ni desventaja: su capa de percepción solo ve lo visible y lo recordado. Lo calcula la simulación cada 10 ticks, de forma determinista y dentro del hash (`[vision]` en `engine.toml`).

- **Tres estados por casilla y jugador:** sin explorar (negro), explorada (oscurecida: el terreno y lo último que se supo) y visible ahora.
- **Vista:** la de cada unidad y cada edificio (`sight_tiles`; el centro urbano y el campamento vigilan lejos).
- **Árboles** (`blocks_sight` en `resources.toml`): dejan ver 2 casillas dentro del bosque. Una unidad entre árboles solo se ve a 2 casillas o menos, lo que permite emboscadas. Un edificio, por su tamaño, se ve en cuanto se ve una casilla de su huella. Talar abre la vista.
- **Altura:** 1 casilla más de vista por cada nivel por encima de lo que se mira, y una loma más alta que ambos extremos tapa lo que hay detrás.
- **Día y noche:** 12 min de luz, con amanecer y anochecer graduales, y 6 min de noche con la vista a la mitad. La pantalla se oscurece.
- **Memoria:** de los edificios enemigos se recuerda el último estado visto (se dibujan oscurecidos). Si caen sin que nadie lo vea, siguen figurando hasta que se vuelve a mirar.
- **Combate:** nadie elige por sí solo un blanco que su jugador no ve.
- **Repeticiones:** se ve todo por defecto, y se puede elegir la vista de cada jugador.
- **IA:**
  - `explorar`: un explorador (jinete si lo hay) busca primero alrededor del punto simétrico de su base respecto al centro del mapa, como haría un jugador, sin alejarse demasiado de donde está. No repite puntos que no ha podido ver.
  - Recuerda el ejército enemigo que ha visto y lo va olvidando, un 1 % por segundo.
  - El perfil «normal» mantiene 6 tropas de guardia aunque no haya visto al enemigo: lo que no se ve no es que no exista.
- **Desactivada** (`enabled = false`, como en las pruebas de movimiento, combate y economía) todo se ve, y el hash es exactamente el de antes de la niebla.

### Prioridad de blancos

Una unidad que elige blanco sola (la del jugador igual que la de la IA) sigue `combat.target_priority` en `engine.toml`:

1. `me_ataca`: la unidad armada que la tiene por blanco (autopreservación).
2. `armada`: lo que puede herir a la tropa.
3. `asedio`: ingenios que solo atacan edificios.
4. `bagaje`: acémilas y carretas.
5. `aldeano`.
6. `otra`.

Dentro de cada clase, el más cercano. Con la lista vacía, solo cuenta la distancia (comportamiento anterior). Las órdenes explícitas del jugador y el módulo `concentrar` de la IA mandan sobre esta regla.

### Convenciones de `data/`

- Solo enteros. Una magnitud fraccionaria se escribe en una unidad menor que figura en el nombre de la clave: `max_speed_milli_tiles_per_tick = 150` significa 0,150 casillas por tick. Así la carga no depende del redondeo decimal→binario de cada plataforma.
- Las duraciones de la simulación van en ticks.
- `data/terrain.toml`, `units.toml`, `resources.toml` y `buildings.toml`: el id de cada tipo es su posición en la lista. Las referencias entre ficheros (bandas del generador, unidades que produce un edificio, edificio inicial, etc.) usan nombres, no ids.
- Los costes y existencias son tablas por recurso: `cost = { madera = 275, piedra = 100 }`. Las claves ausentes valen 0 y una clave que no es un recurso es un error.
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
| `unit` | Punto fijo, RNG contra los vectores de referencia de los autores, reloj de paso fijo, configuración, generación de mapas, proyección y recorte (contra fuerza bruta en 200 cámaras), cámara, selección, atlas, escena, caminos y movimiento, economía, regresión por hash |
| `unit`: economía | Tiempo exacto de recogida (ciclo de 121 ticks), conservación (existencias + carga + nodos = constante en cada tick), agotamiento que libera la casilla, cola (cobro, reembolso, capacidad, otro jugador), pausa por población, 1 frente a 3 constructores, colocación inválida sin cobro, replanificación sin pisar casillas bloqueadas, HPA\* incremental idéntico al construido desde cero, preparación de partida |
| `unit`: combate | Fórmula de daño (armaduras, bonus, mínimo 1, porcentaje de nivel), duelo simétrico con muerte en el mismo tick, proyectil que acierta a un blanco quieto y falla a uno que se mueve, niveles y héroe con aura, posturas, ataque-movimiento, edificio destruido que libera casillas, determinismo tick a tick |
| `unit`: módulos de IA | `ejercito_contra` elige arqueros contra soldados si les hacen más daño y, sin ejército enemigo, lo que más daña edificios; `concentrar` ataca al enemigo armado que antes cae, no al más cercano ni a un aldeano; `ataque_fuerza` ataca con ventaja, espera en desventaja y se retira de una batalla perdida |
| `unit`: IA | Contra un rival quieto crece, construye cuartel y granjas, ataca y lo derrota en 11 minutos sin gastar lo que no tiene; dos IA juegan la misma partida tick a tick; derrota al quedarse sin nada |
| `unit`: regresión | Hashes tras 1200 ticks de movimiento con 500 unidades, 1500 ticks de una partida económica de dos jugadores, 600 ticks de una batalla de 30 contra 30, y 3000 ticks de IA contra IA con cada perfil |
| `unit`: repeticiones | Codificar y decodificar sin pérdida; rechazo de ficheros ajenos, truncados, con cualquier byte cambiado o de otra versión; partida grabada (humano contra IA, órdenes programadas a futuro) reproducida con los mismos hashes; una orden alterada en el tick 800 se detecta en el checkpoint 1000, y quitar una orden también se detecta; grabación con los datos reales reproducida desde su copia |
| `sim_purity` | Regla 2: `src/sim/` limpio de tokens prohibidos |
| `headless_smoke` | El ejecutable arranca, lee `data/` y simula un minuto sin ventana ejecutando `data/scenarios/headless.toml` |
| CI "Humo con ventana" | En Linux, con Xvfb y lavapipe (Vulkan por software), crea el dispositivo SDL_GPU, compila el pipeline, sube el atlas y presenta 120 fotogramas |
| CI `determinism` | El hash tras 2400 ticks del guion de `data/scenarios/headless.toml` es idéntico en MSVC, clang-cl, Clang y GCC |
| CI `bench` | `rts_bench` en Release con 1000 y 2000 unidades en movimiento y en batalla, y 30 minutos de IA contra IA: falla si algún tick supera 50 ms. Además graba 30 minutos de partida y la reproduce con los mismos 180 hashes intermedios y el mismo hash final |
| CI torneo de IA | `normal` gana a `basica` al menos en el 70 % de 10 partidas de 30 minutos |
| CI `replay-cross` | La repetición grabada en Linux con GCC se verifica con MSVC, clang-cl, Clang y GCC |

Si el hash de regresión cambia **sin** cambio de diseño, es un fallo. Si el cambio de diseño es intencionado, se actualiza `kExpectedHash` en el mismo commit y se justifica en el mensaje.

## Rendimiento medido

Release, Clang 20, contenedor de 4 núcleos. Guion de `rts_bench`: un grupo grande con campo de flujo, dos mitades que se cruzan y 400 grupos de 5 con HPA\* individual.

Presupuesto del planificador: 60 000 nodos por tick desde M3. Los valores de M2 (20 000 nodos) van entre paréntesis.

| Unidades | Media | p99 | Máximo | Criterio |
|---|---|---|---|---|
| 1000 | 3,01 ms/tick (2,99) | 5,12 ms (6,39) | 18,0 ms (10,1) | ≤ 50 ms ✅ |
| 2000 | 6,54 ms/tick (6,68) | 16,8 ms (10,8) | 19,1 ms (15,9) | ≤ 50 ms ✅ |

Batalla (`--combat 1`): dos ejércitos (2/3 soldados, 1/3 arqueros) con los frentes a 40 casillas, en ataque-movimiento, 1200 ticks:

| Unidades | Media | p99 | Máximo | Bajas / proyectiles |
|---|---|---|---|---|
| 1000 (500 contra 500) | 5,29 ms/tick | 10,4 ms | 12,5 ms | 242 / 2537 |
| 2000 (1000 contra 1000) | 10,2 ms/tick | 18,2 ms | 30,2 ms | 642 / 4226 |

El peor tick de la batalla grande lo marca el planificador (61 200 nodos: persecuciones que se recalculan a la vez).

Con más presupuesto, el tick del aluvión de 400 órdenes (el 800) resuelve más caminos de golpe: el máximo sube 8 ms con 1000 unidades a cambio de vaciar antes la cola (1861 caminos esperando como mucho con 2000 unidades, frente a 1951).

| Otras medidas | Valor |
|---|---|
| Construir la escena de render (1963 sprites) | 0,036 ms/fotograma |
| Generar el mapa de 256×256 + el grafo HPA\* + la aparición de unidades (banco) | ~54 ms, una vez |
| Partida por defecto: mapa + preparación (6792 árboles, minas, edificios) + HPA\* | ~80 ms, una vez |
| Partida por defecto: tick de simulación + snapshot con 6790 objetos | 0,157 ms (el snapshot, 0,087 ms) |
| `state_hash()` de la partida por defecto (una vez por fotograma en el panel) | 0,38 ms |
| IA contra IA, 30 minutos de la partida por defecto | 0,15 ms/tick de media, 12,6 ms el peor tick |

## Licencia

MIT, ver `LICENSE`. Las dependencias tienen sus propias licencias (zlib, MIT, BSD).
