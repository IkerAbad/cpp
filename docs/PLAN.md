# Plan completo del juego

Documento vivo. Dice qué tiene que ser el juego, qué está hecho y qué falta, en el
orden en que se va a hacer. Cada punto tiene un criterio de terminado comprobable.
Al cerrar un punto se marca aquí, en el mismo commit que lo termina.

## 1. Qué es este juego

Un RTS medieval de la escuela de Tzar y AoE2, con una capa de logística y de mando
propia de un wargame. Se gana igual que en ellos: destruyendo el poder del rival.
Lo que lo distingue es que **la guerra cuesta lo que costaba**: un ejército come,
gasta flechas y se cansa. Los heridos hay que curarlos, la noche y el bosque
esconden, y un asedio sin suministro fracasa.

### Pilares (no se negocian)

1. **Realismo histórico y operativo.** Cada mecánica tiene un porqué histórico,
   documentado con una fuente cuando la hay. No se copia a otros juegos del género:
   se estudia la realidad.
2. **La logística decide.** Víveres, munición, convoyes, sanidad y moral pesan
   tanto como el número de soldados.
3. **Simulación determinista.** Punto fijo a 20 Hz, mismo resultado en cualquier
   máquina y compilador. Es la base de las repeticiones y del multijugador lockstep.
4. **IA justa.** Ve lo mismo que un jugador (niebla incluida) y juega con las mismas
   reglas y costes. La dificultad sale de pensar mejor, nunca de hacer trampas.
5. **Datos, no números mágicos.** Todo valor de diseño vive en TOML con su
   explicación. El código solo tiene reglas.
6. **Recursos propios.** Nada del juego original: ni arte, ni sonido, ni textos, ni
   datos. Hasta que haya arte propio, formas generadas por código.
7. **Calidad medible.** Cada mecánica tiene prueba; hay hashes de regresión, un banco
   de rendimiento y un torneo de IA en la CI.

## 2. Hecho

| Área | Contenido |
|---|---|
| Motor | SDL3 GPU (Vulkan/D3D12), ImGui, paso fijo 20 Hz, punto fijo, CI Linux y Windows, paquete de Windows |
| Mapa | Generación por semilla, alturas, terrenos, bosques, recursos junto a cada inicio |
| Movimiento | HPA*, campos de flujo, evitación local, banco de 2000 unidades |
| Economía | 5 recursos (comida, madera, piedra, oro, hierro), aldeanos, almacenes, granjas, construcción, colas, población |
| Combate | Proyectiles esquivables, armaduras y bonificaciones, experiencia, héroes con aura, prioridad de blancos |
| Unidades | Leva, hombre de armas, arquero, ballestero, jinete, caballero, ariete, zapador, trabuquete, acémila, carreta, cirujano |
| Fuego y asedio | Fuego que se propaga, piedra que se quema pero solo cae por asedio, ariete, minado, escombros, demolición, reparación |
| Logística | Víveres, munición, hambre, bagaje, convoyes, campamento de campaña, trabuquete montado con convoyes |
| Sanidad | Puesto de socorro, hospital de campaña, hospital, enfermeros, cirujanos, heridas leves, reorganización |
| Niebla | Tres estados, árboles, altura, día y noche, memoria de edificios |
| Terreno | Altura para tiradores y cuerpo a cuerpo, bosque que cubre, marcha por terreno, carga en llano |
| Fatiga | Marcha y combate cansan, descanso, paso forzado, aguante por tipo |
| Moral | Bajas cercanas, flanco y retaguardia, contagio, hambre, noche, héroes; desbandada, persecución y rehacerse |
| IA | Percepción con niebla, 18 módulos, perfiles «básica» y «normal», torneo en la CI |
| Repeticiones | Grabación automática, reproductor, verificación por hashes, vista por jugador |
| Interfaz | Ayuda (F2), depuración (F1), recuadro al pasar el ratón, iniciales de unidad, paneles |

## 3. Lo que falta, en orden

### Fase A — Jugable de principio a fin

Objetivo: que una persona pueda abrir el juego, configurar una partida, jugarla con
controles cómodos, guardarla y terminarla sabiendo qué ha pasado.

- [x] **A1. Controles de mando.**
  - Grupos de control: Ctrl+1…9 asigna el grupo y 1…9 lo selecciona.
  - Doble clic selecciona las unidades de ese tipo que hay en pantalla.
  - Mayús+clic derecho encola movimientos (puntos de paso).
  - Puntos de reunión: clic derecho con un edificio seleccionado.
  - *Terminado:* pruebas de la cola de puntos de paso y del punto de reunión en la
    simulación; los grupos se prueban en la selección.
- [x] **A2. Minimapa con niebla.** Terreno, unidades y edificios propios, enemigos
  vistos o recordados; clic para mover la cámara.
  - *Terminado:* prueba de que lo no visto no aparece; se ve en la captura.
- [x] **A3. Avisos.** Mensajes en pantalla, con salto de cámara, cuando:
  - atacan la base;
  - un edificio arde;
  - no queda comida para las raciones;
  - un ejército pasa hambre;
  - termina una producción.

  *Terminado:* los avisos se generan a partir del estado del juego y tienen prueba.
- [x] **A4. Menú y configuración de partida.**
  - Pantalla inicial con: nueva partida (semilla, rival IA con su perfil, niebla
    sí/no), cargar, repeticiones y salir.
  - Fin de partida con estadísticas: bajas, recursos recogidos, edificios
    perdidos y ejército máximo.
  - *Terminado:* se puede jugar sin tocar la línea de órdenes.
- [x] **A5. Guardar y cargar.**
  - Estado completo serializado, con versión y hash.
  - Al cargar y seguir, se obtiene el mismo hash que sin haber parado.
  - *Terminado:* prueba de ida y vuelta del hash a mitad de una partida de IA.

### Fase B — Guerra realista

- [x] **B1. Moral y desbandada.** La moral de cada unidad baja con:
  - las bajas cercanas;
  - los flancos y la retaguardia atacados;
  - el hambre y la noche;
  - la muerte del héroe.

  Sube con un héroe cerca, con la victoria y con los compañeros alrededor. Con la
  moral baja, la unidad se desbanda: huye y no obedece hasta rehacerse. Las batallas
  medievales se decidían más por la desbandada que por la muerte de todos.
  - *Terminado:* pruebas de desbandada y de rehacerse; en el torneo hay victorias
    por desbandada.
- [x] **B2. Terreno en combate.**
  - Ventaja de altura (alcance y daño de los tiradores cuesta abajo).
  - El bosque cubre de las flechas.
  - Velocidad por terreno: bosque y colinas lentos; la carreta, muy lenta fuera de
    llano.
  - La carga de caballería pide llano.
  - *Terminado:* pruebas por regla.
- [x] **B3. Fatiga.** Marchar cansa y descansar repone. Las tropas cansadas pegan y
  andan peor; se puede marchar a paso forzado.
  - *Terminado:* pruebas de fatiga y recuperación.
- [ ] **B4. Fortificaciones.**
  - Empalizada (madera) y muralla (piedra), con puertas que solo abren al dueño.
  - Torres con tiradores dentro (guarnición).
  - Escalas de asedio para tomar la muralla.
  - *Terminado:* la IA rodea o derriba los muros; pruebas de paso por la puerta y
    de guarnición.
- [ ] **B5. Formaciones.** Línea, columna y cuadro, con efectos:
  - la columna marcha más rápido;
  - el cuadro aguanta la carga de caballería;
  - la línea concentra el tiro.

  *Terminado:* pruebas de los efectos.

### Fase C — Economía y sociedad

- [ ] **C1. Herrería y mejoras.** Mejoras de armas y armaduras que cuestan hierro y
  tiempo, por niveles. *Terminado:* pruebas de que se aplican a las unidades nuevas
  y a las existentes.
- [ ] **C2. Forrajeo y saqueo.** Un ejército puede vivir del terreno: forrajea en las
  granjas y aldeas enemigas, quemándolas o vaciándolas, y en el bosque, más
  despacio. Es la *chevauchée* histórica. *Terminado:* pruebas y uso por la IA.
- [ ] **C3. Mercado y comercio.** Cambio de recursos con precios que se mueven, y
  caravanas entre mercados propios (reutilizan los convoyes). *Terminado:* pruebas
  de precios y de rutas.
- [ ] **C4. Caminos.** Construibles; aceleran la marcha y, sobre todo, a las
  carretas. *Terminado:* pruebas de velocidad.

### Fase D — Inteligencia artificial (M7)

- [ ] **D1. Sanidad en la IA.** Lleva a sus heridos a los puestos, tiene enfermeros y
  cirujanos.
- [ ] **D2. Guerra con niebla.** Emboscadas en el bosque, ataques de noche,
  exploración continua y defensa con torres.
- [ ] **D3. Moral y formaciones en la IA.** Usa B1 a B5.
- [ ] **D4. Niveles.** Fácil, normal, difícil y experto, diferenciados por los
  módulos que usan y lo bien que piensan.
  - *Terminado (D1–D4):* en la CI, cada nivel gana al anterior en al menos el 70 %
    de las partidas.

### Fase E — Multijugador (M8)

- [ ] **E1. Lockstep.** Las órdenes viajan, no el estado. Cada tick se confirma el
  hash, y si alguien se desincroniza se detecta y se dice en qué tick.
- [ ] **E2. Sala sencilla.** Anfitrión e invitado por dirección IP, chat y partida
  de 2 a 4 jugadores, mezclando humanos e IA.
- *Terminado (E1–E2):* dos procesos en la CI juegan una partida de IA contra IA por
  red local con hashes idénticos.

### Fase F — Presentación y contenido propio

- [ ] **F1. Arte propio.** Sprites generados por código o dibujados por nosotros,
  animaciones, edificios reconocibles y efectos de fuego y humo.
- [ ] **F2. Sonido propio.** Sonidos sintetizados y música de dominio público o con
  licencia CC0.
- [ ] **F3. Mapas.** Ríos con vados, mapas para 2–4 jugadores y editor de
  escenarios.
- [ ] **F4. Campaña.** Escenarios históricos con datos propios.
- [ ] **F5. Idiomas y opciones.** Español e inglés; resolución, volumen y teclas.

### Fase G — Calidad continua

- [ ] **G1. Rendimiento.** 4000 unidades por debajo de 50 ms por tick, con banco en
  la CI.
- [ ] **G2. Pruebas aleatorias de órdenes.** Órdenes al azar sobre partidas reales
  sin que nada se rompa ni se desincronice.
- [ ] **G3. Informe de errores.** Ante un fallo, guardar la repetición y el registro.

## 4. Cómo se trabaja cada punto

1. Diseño breve (al ser un sistema nuevo, en el commit o en este documento).
2. Datos en TOML con su explicación y su fuente.
3. Pruebas de cada regla. Los hashes de regresión solo cambian si se cambia el
   comportamiento a propósito, y se demuestra.
4. Ambos compiladores, banco, torneo y repetición de 30 minutos antes de subir.
5. README actualizado y casilla marcada aquí.
