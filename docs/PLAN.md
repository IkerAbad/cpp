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
| Caminos | Construibles; aceleran la marcha y sobre todo a las carretas (la búsqueda de caminos aún no los prefiere) |
| Mercado | Cambio por oro con precios comunes que se mueven; caravanas entre mercados propios |
| Forrajeo | Saqueo de granjas y aldeas enemigas, caza en el bosque; la IA saquea lo que no está defendido |
| Herrería | Mejoras de armas y armaduras por niveles, con hierro; valen para las unidades nuevas y las existentes |
| Formaciones | Línea (tiro), columna (marcha), cuadro (contra caballería y carga) |
| Fortificaciones | Empalizada, muralla, puertas del dueño, torres con guarnición, escalas; la IA rodea o abre brecha |
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
- [x] **B4. Fortificaciones.**
  - Empalizada (madera) y muralla (piedra), con puertas que solo abren al dueño.
  - Torres con tiradores dentro (guarnición).
  - Escalas de asedio para tomar la muralla.
  - *Terminado:* la IA rodea o derriba los muros; pruebas de paso por la puerta y
    de guarnición.
- [x] **B5. Formaciones.** Línea, columna y cuadro, con efectos:
  - la columna marcha más rápido;
  - el cuadro aguanta la carga de caballería;
  - la línea concentra el tiro.

  *Terminado:* pruebas de los efectos.

### Fase C — Economía y sociedad

- [x] **C1. Herrería y mejoras.** Mejoras de armas y armaduras que cuestan hierro y
  tiempo, por niveles. *Terminado:* pruebas de que se aplican a las unidades nuevas
  y a las existentes.
- [x] **C2. Forrajeo y saqueo.** Un ejército puede vivir del terreno: forrajea en las
  granjas y aldeas enemigas, quemándolas o vaciándolas, y en el bosque, más
  despacio. Es la *chevauchée* histórica. *Terminado:* pruebas y uso por la IA.
- [x] **C3. Mercado y comercio.** Cambio de recursos con precios que se mueven, y
  caravanas entre mercados propios (reutilizan los convoyes). *Terminado:* pruebas
  de precios y de rutas.
- [x] **C4. Caminos.** Construibles; aceleran la marcha y, sobre todo, a las
  carretas. *Terminado:* pruebas de velocidad.

### Fase D — Inteligencia artificial (M7)

- [x] **D1. Sanidad en la IA.** Lleva a sus heridos a los puestos, tiene enfermeros y
  cirujanos.
- [x] **D2. Guerra con niebla.** Emboscadas en el bosque, ataques de noche,
  exploración continua y defensa con torres.
- [x] **D3. Moral y formaciones en la IA.** Usa B1 a B5.
- [x] **D4. Niveles de IA.** Cada nivel gana al de debajo. Hecho: el experto manda la
  mitad de los aldeanos a la comida (la que limita aldeanos y raciones). Medido con 20
  partidas (10 mapas jugados desde los dos lados): en muchos mapas gana el mismo lado
  juegue quien juegue, así que se cuentan los mapas ganados desde los dos lados.
  Normal contra fácil 7 a 0 (85 % en bruto), difícil contra normal 3 a 0 (65 %),
  experto contra difícil 5 a 0 (75 %); la CI lo exige. Después se midió por qué gana
  tanto un mismo lado: en mapas reflejados (idénticos para los dos) y con la misma IA,
  cada mapa sigue teniendo un ganador claro; lo decide el caos de la partida (desempates
  que no son simétricos y que crecen en 30 minutos), no el mapa. Se añadió el tipo de
  mapa «Espejo» para partidas de dos justas. Falta: un experto que le saque más al
  normal (4 a 1).
- [x] **E1. Lockstep.** Las órdenes viajan, no el estado. Se confirma el hash cada
  segundo (5 turnos de 200 ms; cada tick sería caro para nada) y, si alguien se
  desincroniza, se detecta y se dice en qué tick y con quién. Hecho: turnos con
  retraso de 2 (como AoE), estrella con reenvío, sockets propios, CI con dos procesos
  en las cuatro plataformas.
- [x] **E2. Sala sencilla.** Anfitrión e invitado por dirección IP, chat y partida
  de 2 a 4 jugadores, mezclando humanos e IA. Hecho, con relevo de la IA si un
  invitado se va; unirse no bloquea la ventana (reintenta hasta el plazo).
- *Terminado (E1–E2):* dos procesos en la CI juegan una partida de IA contra IA por
  red local con hashes idénticos.

### Fase F — Presentación y contenido propio

- [x] **F1. Arte propio.** Sprites generados por código o dibujados por nosotros,
  animaciones, edificios reconocibles y efectos de fuego y humo. Hecho, todo por código
  desde `data/art.toml`: figuras con 5 poses y capa del jugador, edificios isométricos
  con materiales, árboles, rocas, terreno con textura, llamas y humo, y escena ordenada
  en profundidad. Falta: 8 sentidos (hoy 2, con espejo), animación de muerte y flechas
  dibujadas.
- [x] **F2. Sonido propio.** Sonidos sintetizados y música de dominio público o con
  licencia CC0. Hecho, todo propio: 15 efectos sintetizados desde `data/sound.toml` y
  dos piezas modales (paz y batalla) compuestas por código, que cambian según haya
  combate a la vista; posición en pantalla, niebla y esperas deciden qué suena. Falta:
  ambiente (viento, agua, pájaros) y volumen en pantalla (F5).
- [x] **F3. Mapas.** Ríos con vados, mapas para 2–4 jugadores y editor de
  escenarios. Hecho: ríos con vados donde cortan tierra (tipos de mapa «Ríos» y «Gran
  río»), inicios siempre en la misma región de tierra y sendas taladas si el bosque
  corta el paso (comprobado en 270 combinaciones), y editor de escenarios (terreno,
  altura, recursos, edificios, unidades, jugadores; guardar, jugar, editar), con
  deshacer y rehacer (Ctrl+Z, Ctrl+Y; 64 pasos).
- [x] **F4. Campaña.** Escenarios históricos con datos propios. Hecho: objetivos en la
  simulación (destruir, conservar, sobrevivir, llegar, reunir, derrotar), escenarios
  escritos a mano con pinceladas, y la campaña de Las Navas de Tolosa (Toledo,
  Calatrava, Las Navas) con informe, nota histórica y citas literales verificadas;
  progreso guardado. De paso, la brecha de un muro derribado ya se puede cruzar.
  Falta: probarla con personas y más campañas.
- [x] **F5. Idiomas y opciones.** Español e inglés; resolución, volumen y teclas.
  Hecho: toda la interfaz en español e inglés (el texto español es la clave; una prueba
  exige traducción, con los mismos especificadores, de cada texto marcado y nombre a la
  vista de cada tipo de los datos) y menú de opciones (idioma, pantalla completa,
  tamaño de ventana, tres volúmenes y 13 teclas), guardadas aparte de los datos de
  partida. Después, también los textos de la campaña y de los escenarios
  (clave.es, clave.en), con una prueba que exige el inglés.

### Fase G — Calidad continua

- [x] **G1. Rendimiento.** 4000 unidades por debajo de 50 ms por tick, con banco en
  la CI. Hecho: medido aquí (4 núcleos), movimiento media 14,4 ms y máximo 30,5 ms;
  batalla de 2000 contra 2000, media 18,8 ms y máximo 37,8 ms. La CI lo comprueba en
  cada cambio (pasa también en su máquina).
- [x] **G2. Pruebas aleatorias de órdenes.** Órdenes al azar sobre partidas reales
  sin que nada se rompa ni se desincronice. Hecho: `rts_fuzz` (y pruebas cortas)
  con órdenes válidas y mal formadas, invariantes, dos mundos a la par y la
  repetición verificada. Encontró una caída real (una unidad repetida en una orden,
  posible por la red), ya arreglada. Medido: 672 000 órdenes en Release y 63 000 con
  aserciones, sin fallos; en la CI, 3 semillas x 6000 ticks en cada partida.
- [x] **G3. Informe de errores.** Ante un fallo, guardar la repetición y el registro.
  Hecho: ante una caída (violación de memoria, abort o aserción, excepción sin
  capturar) o una desincronización en red, `informes/informe-<fecha>/` con la
  repetición hasta ese tick, el registro y un resumen. Una prueba de CTest provoca una
  caída y comprueba que la repetición del informe se reproduce.

## 4. Cómo se trabaja cada punto

1. Diseño breve (al ser un sistema nuevo, en el commit o en este documento).
2. Datos en TOML con su explicación y su fuente.
3. Pruebas de cada regla. Los hashes de regresión solo cambian si se cambia el
   comportamiento a propósito, y se demuestra.
4. Ambos compiladores, banco, torneo y repetición de 30 minutos antes de subir.
5. README actualizado y casilla marcada aquí.
