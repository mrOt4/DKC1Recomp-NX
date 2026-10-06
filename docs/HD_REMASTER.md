# Mod HD (remaster) — diseño y plan de implementación

Estado: **Estrategia A elegida; Fase 1 implementada en la parte de CPU**
(captura de identidad + compositor de referencia). Se distribuyen las
herramientas y la receta, nunca los píxeles: cada usuario genera su pack a
partir de su propia ROM.

### Estado de la Fase 1 (2026-10-05)

- `snesrecomp/runner/src/snes/ppu.{c,h}`: `PpuSetIdentityCapture`. BG 4bpp
  (incluidos tiles mixtos de la sombra widescreen), BG 2bpp, OBJ, fusiones
  padded/stretched/mirror, overlay y sprites → G-buffer por píxel +
  CGRAM/fixed color/brillo por línea. Mosaico, big tiles y Mode 2/3/7: sin
  identidad (caen al píxel nativo).
- `runner/dkc1_hd.{c,h}`: compositor CPU de referencia a 1–4×, fuentes
  `identity` y `grid`, autocomprobación por píxel y estadísticas.
  `DKC1_HD_IDENTITY=1`, `DKC1_HD_DEBUG=grid`, `DKC1_HD_SCALE`,
  `DKC1_HD_DISABLE`, `DKC1_HD_PPM`; el host headless imprime `hd_stats`.
- Validación en `route_jungle.dks` (Linux, headless, 4×):

  | Leg | Frames | Hashes frame/WRAM/VRAM/CGRAM/OAM vs stock | `ref_mismatch` | `equiv_mismatch` | `self_check_fail` |
  |---|---|---|---|---|---|
  | 4:3 | 8070 | idénticos | 0 | 0 | 0 |
  | 16:9 | 7645 | idénticos | 0 | 0 | 0 |

  En 4:3, el 85,8 % de los píxeles no negros se recomponen desde un texel;
  el resto es sobre todo fondo (*backdrop*, sin texel).
- Presentación: pase `dkc1_hd` en `macos_graphics.metal` (fuente única; el
  generador lo traduce a GLSL para Windows), `encodeHdPixels` en Metal,
  cola HD en el presentador `CAMetalDisplayLink`,
  `Dkc1WindowsGraphicsDrawHd` en OpenGL y textura SDL propia en Switch.
  **Sin probar en hardware**: los 8 shaders GLSL compilan con
  glslangValidator, `windows_graphics.c` pasa `clang -fsyntax-only` y el NRO
  de Switch compila; el código Objective-C/Metal no se ha podido compilar
  (no hay SDK de Apple en Linux).
- El HD solo se activa con aspecto widescreen (16:10 o 16:9); en 4:3 el
  juego se presenta siempre stock.

### Estado de la Fase 2 (2026-10-05)

- Clave de tile = FNV-1a 64 del contenido del carácter (+ profundidad).
- `DKC1_HD_DUMP=<dir>` escribe `dump.bin` (privado, fuera de Git): cada
  carácter único con su mejor aparición, un recorte RGBA de 40×40 **de su
  propia capa** (tilemap BG renderizado desde la VRAM u OBJ desde la OAM, sin
  otras capas encima), su paleta y sus datos planares.
- `recipes/hd_dump_jungle.dks`: recorrido de Jungle Hijinxs (correr, rodar,
  saltar); 19 392 frames, 18 246 caracteres únicos.
- `tools/hd_pack.py`: hojas → Real-ESRGAN ncnn-vulkan
  (`realesr-animevideov3`, el que mejor conserva el grano del CGI) →
  recorte central → 35 % del grano original reinyectado → cuantización
  OKLab a los índices que usa el carácter con *dithering* Bayer 4×4 →
  silueta nativa → `tiles.bin` + `pack.json`. Verifica el SHA-256 del ROM.
- Pack **DKC1 HD Remaster** (local, no distribuible): 18 144 tiles a 4×,
  18,9 MB, unos 4 min en una RTX 4070 SUPER.
- Validación (16:9, `route_jungle.dks`): hashes de máquina idénticos a stock;
  99,99 % de los texeles principales servidos por el pack; 0 fallos de
  autocomprobación. La cobertura es optimista porque se volcó sobre la misma
  ruta.
- Límites de esta versión (ya corregidos, ver abajo): silueta nativa de los
  sprites y retícula 8×8 visible en el follaje.

### Correcciones posteriores y Fase 3 (2026-10-05)

- **Siluetas HD.** El PPU guarda además, por píxel, la capa opaca de debajo
  (`under`, o el fondo) y el tile más alto que es transparente en ese píxel
  (`cover`); las fusiones de widescreen y de sprites mantienen ese "top-2".
  El pack lleva alfa remuestreado (Lanczos) de la máscara de la propia capa,
  con la silueta exacta del carácter dentro de su celda; el compositor deja
  ver la capa de debajo donde el alfa HD encoge y dibuja el tile de encima
  donde crece. Validación en identidad: 15/76 M discrepancias en `under` y
  438/19 M en `cover` (casos residuales).
- **Costuras.** Métrica objetiva `tools/hd_seams.py` (salto HD/nativo entre
  tiles frente a dentro de un tile): pack anterior 1,22–1,24; escalado del
  frame completo (referencia sin costuras) 0,83. Ampliar el contexto a 32 px
  no cambia nada: la causa es que un carácter repetido tiene vecinos
  distintos según dónde aparezca. Se corrige en tiempo real con un filtro
  de *deblocking* en los bordes de tile de una misma capa cuando la imagen
  nativa no tiene borde allí (`DKC1_HD_DEBLOCK=0` lo desactiva): 0,82–0,84.
- **Rendimiento.** LUT de color por línea, muestreo con pasos precalculados
  y bandas de filas en un pool de hilos (pthreads / Win32;
  `DKC1_HD_THREADS`): 20 → 4,9 ms por frame con 1 hilo y 2,7 ms con 4
  (Linux, x86-64), *deblocking* incluido. En Switch no está medido.
- **Regresión de la ruta de packs.** `tools/hd_pack.py --identity` genera un
  pack con los tiles nativos; `tools/hd_seams.py --exact` comprueba que el
  frame HD reproduce exactamente el nativo: 0 bloques distintos.
- **Fase 3.** Menú **Mods → HD Textures / Choose HD Pack…** en macOS y
  Windows (barra de menús y página Mods del menú de pausa), persistencia
  (`DKC1HdPack`/`DKC1HdEnabled` en macOS; `HDPack`/`Mods.HdTextures` en
  Windows), prioridad de `DKC1_HD_PACK`/`DKC1_HD_DISABLE`, y en Switch
  activación por la carpeta `sdmc:/switch/dkc1/hd/`. Sin probar en macOS
  ni Windows (no hay hardware); Linux y Switch compilan.
- Pendiente: los caracteres que solo aparecen en la pantalla secundaria o
  con brillo < 15 no se vuelcan; volcar el resto de niveles.

### Pack v2 y compositor para Switch (2026-10-06)

Problemas medidos en Switch con el pack v1:

1. **Logos de Nintendo y Rare destrozados.** Cada tile HD del logo era de un
   solo índice: el más bajo de los que usa el tile. El volcado se quedaba con
   la primera aparición de cada carácter cuando las puntuaciones empataban, y
   el logo entra con un fundido que escribe la CGRAM. Esa primera aparición
   tenía la paleta en negro y la cuantización desempataba al primer índice.
   Ahora la puntuación suma los colores distintos que muestran los índices
   del carácter (y su luminancia como desempate). Los registros del logo
   pasan a 14–16 colores distintos.
2. **Aspecto granulado.** La cuantización a los 16 colores con tramado Bayer
   y la mezcla del 35 % de grano nativo (`--detail 0.35`, bloques de 4×4)
   pixelaban la imagen.
3. **GPU de 15 a 28 ms por frame** (con `glFinish`, `gpu.flag`) y captura +
   codificación de 4 a 9 ms en el hilo principal: no llegaba a 60 fps. El
   shader hacía divisiones y módulos enteros (emulados en Maxwell), leía dos
   texels de 128 bits y la imagen nativa, y añadía dos pases completos de
   *deblocking*.

Rediseño:

- **Formato v2** (`DKC1HDP2`, 2 bytes por texel): índices `i`, `j` (4 bits
  cada uno, 0 = transparente) y peso `w`. El color es el de `i` mezclado
  hacia el de `j`, donde 0 equivale a lo que hay debajo. `hd_pack.py` ajusta
  cada píxel del modelo a la mejor pareja de colores de la paleta y, con
  cobertura parcial, a un color mezclado con lo de debajo.
- **Composición** (CPU y GPU con la misma aritmética en coma flotante,
  `FinishColor`): con pesos 0/255 coincide con el PPU. El pack de identidad
  da 0 píxeles distintos frente al frame nativo; GPU frente a CPU, 0
  píxeles con diferencia mayor que 1 en 840 frames (Wine, GL 3.3).
- **GPU**: una sola textura RGBA32UI de entrada (16 bytes por píxel; los
  píxeles nativos llevan su color dentro y ya no se sube la imagen nativa),
  atlas RG8 de 256 tiles por fila, escala potencia de 2 (solo
  desplazamientos), un único pase de composición y tres juegos de texturas
  de entrada en rotación. Sin *deblocking*.
- **Escalas**: `tiles-2x.bin` se genera promediando el resultado 4× del
  modelo. Antes, la Switch reducía el pack 4× al cargar tomando un texel de
  cada cuatro.

### Juntas entre tiles en el compositor v2 (1.0.2, 2026-10-06)

Sin *deblocking*, el pack v2 dejaba una cuadrícula visible en fondos de
grano fino (follaje de Jungle Hijinxs): cada carácter se amplía en un solo
contexto y su interior suave hace evidentes los bordes de 8×8.

- **Marcado en CPU** (`SeamBits`, `dkc1_hd.c`): hay junta entre dos píxeles
  vecinos cuando son la columna/fila 7 y 0 de sus tiles (respetando el
  volteo), de la misma capa principal, ambos compuestos y no negros, los dos
  caracteres están en el pack y el salto nativo es ≤ 96 por canal. Un
  carácter sin versión HD conserva sus bordes de píxel. Los cuatro bits
  (arriba, derecha, abajo, izquierda) viajan en los bits 26, 29, 30 y 31 del
  canal `z` de la textura de entrada, sin subir datos nuevos.
- **Pase de GPU** (`kDeblock`, `dkc1_hd_gpu.c`) tras la composición: reparte
  el salto HD a través de la junta en una rampa de un píxel nativo a cada
  lado, `d·(n−i)/(2n+1)` en enteros. `DeblockFrame` hace lo mismo en CPU.
  `DKC1_HD_DEBLOCK=0` lo desactiva.
- **Medidas** (`tools/hd_seams.py`, 4×, antes → después): jungla 2,09 → 0,63,
  segunda jungla 2,19 → 0,60, fábrica 1,84 → 0,45, cueva 1,96 → 0,72, agua
  1,18 → 0,47; jungla a 2× (Switch) 1,52 → 0,55. Umbral nativo probado:
  24 deja la cuadrícula (1,43), 96 la quita sin suavizar bordes reales
  (escaleras, personajes, siluetas contra el cielo entre capas distintas).
- **GPU frente a CPU** (Wine, GL 3.3, 240 frames de jungla y de fábrica):
  igual que sin *deblocking*, 1 y 0 píxeles con diferencia mayor que 1.
  Probado en Switch: 60 fps.
- Descartado: retroproyectar en `hd_pack.py` (que cada bloque HD promedie el
  color nativo) no reduce las juntas (2,09 → 2,12) y cambiaría el pack.
- El pack no cambia: los packs de 1.0.0 y 1.0.1 sirven tal cual.

Objetivo: sustituir la imagen de tiles, sprites y fondos por versiones en alta
resolución **solo en la presentación**. La CPU recompilada, la WRAM, la VRAM,
la CGRAM, la OAM, la colisión, la física y los guardados no cambian, y el
framebuffer nativo (`renderBuffer`) sigue siendo byte a byte el mismo; el
resultado HD es una superficie aparte.

---

## 0. Correcciones a las premisas del encargo

Antes de diseñar conviene fijar cómo funciona realmente el código, porque dos
supuestos del encargo no se cumplen y cambian el diseño:

1. **snesrecomp no embebe snes9x.** Tiene su propio runtime de hardware
   (`snesrecomp/runner/src/snes/ppu.c`, `dma.c`, `apu.c`, `dsp.c`…). De snes9x
   solo toma fragmentos puntuales (temporización de registros en
   `recomp_hw.c`, referencias del DSP). La CPU 65816 del intérprete de
   reserva deriva de LakeSnes (`THIRD_PARTY_ATTRIBUTION.md`).
2. **Baby Kong no reinyecta nada en la VRAM.** Usa la API de *overlays* del PPU:
   marca un rango de la OAM para que sus píxeles OBJ **no** se compongan en el
   juego (`kPpuOverlayFlag_RemoveFromGame`) y después dibuja sus propios
   píxeles encima de `renderBuffer` tras el barrido
   (`runner/dkc1_baby_kong.c:451` y `:528`). Además, **la VRAM no puede
   contener tiles HD**: son tiles de 8×8 a 2/4/8 bpp a resolución nativa, y
   el PPU emite exactamente un píxel por píxel SNES. Por tanto la
   "Estrategia A" tal como está planteada (meter tiles HD en la VRAM simulada)
   no es posible; abajo se replantea como un **compositor HD** que se alimenta
   de la identidad de tile de cada píxel.
3. **Upscaling por IA en tiempo real en Switch no es viable.** La Switch se
   presenta hoy con el renderer de SDL2 (GLES2 vía `SDL_Renderer`), sin
   shaders propios ni ruta de cómputo. Ejecutar Real-ESRGAN/SwinIR por frame
   también exige una GPU de escritorio en Windows/macOS y produce parpadeo
   temporal (el modelo "alucina" distinto en cada frame). La IA sirve **fuera
   de línea**, para generar el pack.
4. **"120 Hz en macOS"** se refiere a la presentación: el juego sigue
   produciendo 60 frames lógicos por segundo (productor fijo a 60 Hz,
   `docs/HOST_PACING.md`) y el presentador Metal repite el último frame
   compuesto. El compositor HD trabaja una vez por frame lógico.

---

## 1. Puntos de intercepción gráfica

### 1.1 Dónde corre el PPU y cómo se compone el framebuffer

- El PPU **no** corre ciclo a ciclo intercalado con la CPU. Al final de cada
  frame lógico, `runner/dkc1_game.c` (≈ líneas 2200–2300) configura
  widescreen y sesgo de presentación, llama a `Dkc1BabyKongPrepareFrame`,
  arranca la HDMA y ejecuta `ppu_runLine(g_ppu, line)` para las líneas
  0–224, aplicando la HDMA por línea. Después dibuja Baby Kong
  (`Dkc1BabyKongDrawFrame`) y modela el VBlank.
- Dentro de `snesrecomp/runner/src/snes/ppu.c`, cada línea pasa por
  `PpuDrawWholeLine` (`:2115`):
  - `PpuDrawBackgrounds` (`:2046`) y `PpuDrawSprites` (`:1910`) rellenan
    búferes por capa (`PpuPixelPrioBufs`, `ppu.h:45`). Cada entrada es de
    16 bits: **prioridad en el byte alto, índice de CGRAM en el bajo**. No
    guarda qué tile ni qué texel lo produjo.
  - Las variantes de dibujo de fondos (`PpuDrawBackground_2bpp/4bpp/8bpp`,
    `_opt`, `_mosaic`, `Big`, `_policy`, merges de widescreen, `:848–1703`)
    resuelven scroll, ventanas, mosaico y márgenes.
  - La composición final (pantalla principal y secundaria, color math,
    ventanas de color y brillo `INIDISP`) convierte el índice a ARGB8888 con
    `brightnessMult` (≈ `:2230–2290`) y escribe en `ppu->renderBuffer`.
- Widescreen: los márgenes **no** son píxeles copiados, sino entradas de
  tilemap guardadas por mundo en `ws_shadow.c` ("virtual presentation
  tilemap") que el PPU dibuja con **las mismas rutinas de tiles**. Por tanto,
  cualquier información de identidad de tile que se capture en esas rutinas
  cubre automáticamente 256, 308 y 342 columnas.

### 1.2 API para leer/escribir píxeles antes de presentar

| Punto | Qué ofrece |
|---|---|
| `ppu->renderBuffer` / `renderPitch` | ARGB8888 final, ancho `Dkc1VideoWidth()` × 224. Baby Kong escribe aquí. |
| API de overlays (`ppu.h:63–93`, `PpuBindOverlaySurface`, `PpuSetOverlayCapture`, `PpuSetOverlayOamRange`) | Extrae a una superficie aparte los píxeles de una capa (BG1–4 u OBJ) dentro de un rectángulo, opcionalmente quitándolos del juego; para OBJ filtra por rango de OAM. |
| `PpuSetWidescreenLineEnhancer` (`ppu.h:15`, `:426`) | Callback por línea dentro del PPU. |
| Host: `Present()` → `PreparePresentation()`/`SubmitPresentation()` (`runner/sdl_host.c:1231`) | Copia el frame a los presentadores: Metal + `CAMetalDisplayLink` en macOS (`macos_metal_presenter.h`, `macos_graphics.metal`), OpenGL 3.3 en Windows (`windows_graphics.c`), `SDL_Renderer` GLES2 en Switch. |
| Filtros existentes | `desktop_filter.c` / `desktop_crt.c` (modelos en C) y sus versiones Metal/GLSL: Nearest, Bilinear, Sharp Bilinear, **Reconstruct** (5 modos tipo xBR/slopes) y CRT. Actúan sobre una **copia** de presentación; el píxel crudo no se toca (`docs/GRAPHICS_OPTIONS_PORT.md`). |

No existe hoy ninguna API que exponga *qué tile* generó cada píxel. Ese es el
único hueco que el mod HD necesita cerrar en el framework.

### 1.3 Cómo se integra Baby Kong

1. **Carga** (`Dkc1BabyKongLoadRom`, `Dkc1BabyKongInitializeFromEnvironment`):
   ROM externa del usuario verificada por SHA-256; decodifica 354 frames
   **en memoria**; nunca exporta ni guarda nada.
2. **Antes del barrido** (`Dkc1BabyKongPrepareFrame`): identifica el tramo
   contiguo de la OAM que pertenece a Donkey y le pide al PPU que lo capture y
   lo quite del juego (`PpuBindOverlaySurface` + `PpuSetOverlayCapture` +
   `PpuSetOverlayOamRange`).
3. **Después del barrido** (`Dkc1BabyKongDrawFrame`): dibuja el frame de Kiddy
   alineado (pies o centro opaco) en `renderBuffer`.
4. **Movimiento** (`Dkc1BabyKongApplyMoves`, `dkc1_game.c:138`): única parte
   que escribe en la WRAM; el mod HD **no** usará nada equivalente.
5. **Host**: variables `DKC1_BABY_KONG_ROM` / `DKC1_BABY_KONG`
   (`sdl_host.c:2344`), menú Mods en macOS (`macos_pause_menu.m:230`) y
   Windows (`windows_platform.c:194–257`), persistencia en preferencias.
6. **Validación**: con el mod desactivado los hashes de framebuffer y máquina
   deben ser los de stock.

El mod HD copia el patrón de carga, verificación, menú y variables de entorno,
pero **no** el de "quitar y redibujar en `renderBuffer`", porque el resultado HD
no cabe en un búfer de 342×224.

---

## 2. Estrategias

### Estrategia A (replanteada): compositor HD por identidad de tile

En lugar de tocar la VRAM, el PPU escribe, en paralelo a sus búferes de
prioridad, un **G-buffer de identidad**: para cada píxel nativo, de qué tile
procede en la pantalla principal y en la secundaria. Con eso un compositor
aparte vuelve a dibujar el frame a N× usando texturas HD:

```
PPU (sin cambios en su salida)
  ├─ renderBuffer 342×224 ARGB ─────────────────► ruta actual (hashes, filtros)
  └─ G-buffer 342×224 (main + sub, opt-in)
        tileKey · texel (0–7,0–7) · flips · base de paleta · op de color math · brillo
                 │
                 ▼
     Compositor HD (GPU: Metal / GLSL 3.3; Switch: CPU 2× o GL propio)
        tile con HD → muestrea atlas HD (índices o RGBA) a N×
        tile sin HD → píxel nativo replicado (o Reconstruct)
        aplica la misma color math / brillo que el píxel nativo
                 │
                 ▼
          superficie HD (342·N × 224·N) → presentador
```

- **Clave del tile**: hash del contenido del carácter (16/32/64 bytes para
  2/4/8 bpp), no su dirección en la VRAM. DKC transmite gráficos a la VRAM
  por DMA (sprites cada frame, fondos al cargar el nivel), así que la
  dirección cambia y el contenido no. Es el mismo principio que los HD packs
  de Mesen para NES.
- **HD indexado por defecto**: el tile HD es una imagen de **índices de
  paleta** (0–15 en 4 bpp, 0 = transparente) a N×. El color sale de la CGRAM
  viva, igual que en el juego, de modo que fundidos, destellos al recibir
  daño, ciclos de paleta del agua y color math siguen funcionando sin
  trabajo extra. Opcionalmente un tile puede tener una variante RGBA ligada a
  un hash de paleta concreto.
- **Color math/ventanas**: se resuelven a nivel de píxel nativo (como hoy) y
  el compositor aplica la misma operación a cada subpíxel del bloque N×N.
  Los bordes de ventana quedan a resolución nativa, lo cual es fiel.

### Estrategia B: post-proceso del framebuffer

Se toma `renderBuffer` (256/308/342×224) y se escala con un shader o un modelo.

- **Shader**: ya existe. Reconstruct y CRT están en Metal (macOS) y GLSL
  (Windows). En Switch falta porque `SDL_Renderer` no permite shaders
  propios.
- **IA en tiempo real**: descartada (punto 0.3). Además, sobre el frame
  compuesto el modelo no distingue capas: mezcla Donkey con el fondo,
  difumina transparencias y efectos de HDMA, y cambia de un frame a otro.

### Comparación

| Criterio | A: compositor por tile | B: post-proceso |
|---|---|---|
| Calidad | Arte HD real por tile, estable entre frames | Solo interpolación; IA inestable temporalmente |
| Transparencias, fundidos, paletas | Correctos (índices + CGRAM viva + misma color math) | Se degradan; el modelo ve colores ya mezclados |
| Widescreen 308/342 | Automático: los márgenes se dibujan con las mismas rutinas | Automático |
| Cambios en el framework | G-buffer opt-in en `ppu.c` (presentación pura) | Ninguno |
| Coste en ejecución | Bajo: muestreo de textura por subpíxel | Shader: bajo. IA: inviable en Switch |
| Trabajo de assets | Alto: generar el pack | Ninguno |
| Riesgo de determinismo | Nulo si el G-buffer no se serializa ni se lee desde el juego | Nulo |
| Switch | 2× por CPU o GL propio (Fase 4) | Requiere también GL propio para shaders |

**Elección: A como mod**, con B (Reconstruct) como **relleno** para tiles que
aún no tengan HD y como modo independiente para quien no tenga pack.

---

## 3. Pipeline de la estrategia A

### 3.1 Extracción sin violar la política de contenido

Nada sale del ROM hacia el repositorio. La extracción es una herramienta que el
usuario ejecuta sobre **su** ROM verificada y que escribe en una carpeta
**fuera** del árbol (o en `build/`, ignorado):

- **Modo dump del host**: `DKC1_HD_DUMP=/ruta/fuera/del/repo`. Mientras se juega
  o se reproduce una ruta determinista (`recipes/route_jungle.dks`), cada
  carácter visto se escribe una vez como PNG indexado nativo
  `<hash>.png` junto con su contexto: capa, paleta CGRAM usada, frecuencia,
  y para OBJ el tramo de la OAM y el ID de animación del actor.
- **Reconstrucción de contexto** (necesaria para un buen upscale): no se
  escalan tiles de 8×8 sueltos, porque las costuras saltarían a la vista. El
  dump guarda también:
  - **Fondos**: metatiles de 32×32 decodificados con
    `Dkc1VideoDecodeLevelTile` (ya existe para los márgenes), o capturas de
    capa completas con la API de overlays (BG1/BG2).
  - **Sprites**: frames ensamblados por tramo de OAM, el mismo método que usa
    Baby Kong para aislar a Donkey.
- El dump comprueba el SHA-256 del ROM (`verified_rom.c`) antes de escribir y
  pone en la carpeta un `README` que recuerda que su contenido es privado.

### 3.2 Upscaling preservando el estilo de Rare

DKC son renders CGI cuantizados a 15 colores por paleta, con *dithering* y
bordes duros. Un modelo genérico lo "pinta" o lo vuelve plástico. Pautas:

1. **Escalar contexto, no tiles**: metatiles o frames completos con 8 px de
   relleno replicado; después se cortan en tiles HD de 8N×8N y se indexan
   por el hash del tile nativo.
2. **Requantizar a la paleta**: tras el modelo, cada píxel se lleva al color
   más cercano de **la paleta de 15 colores del tile** (en espacio
   perceptual, p. ej. OKLab). Así el tile HD es indexable, no aparecen colores
   nuevos y se conserva el aspecto "pixelado" característico.
3. **Alfa binario**: el índice 0 sigue siendo transparente. La máscara se
   escala por separado (nearest + umbral, o xBR sobre la máscara) para que no
   haya halos.
4. **Herramientas**:
   - *Real-ESRGAN* (BSD-3; arquitecturas RRDB y SRVGG "compact"): la base
     recomendada para ajustar.
   - *SwinIR* (Apache-2.0): mejor en bordes finos, más lento; útil para fondos.
   - *waifu2x* (MIT): bueno con arte plano, peor con el sombreado CGI de DKC.
   - Escaladores no neuronales (xBRZ, ScaleFX) como referencia y para
     máscaras.
   - Revisar la licencia de **los pesos** de cada modelo, no solo la del
     código.
5. **Ajuste fino (*fine-tuning*)** sin arte de Nintendo como verdad de
   referencia, porque no existe una versión HD oficial:
   - Generar pares propios: renders 3D propios con iluminación de estilo
     CGI de los 90, degradados con el pipeline SNES (reducción a 1/N, 15
     colores por paleta en bloques de 8×8, *dithering* ordenado).
   - Entrenar el modelo para invertir **esa** degradación concreta. Así
     aprende a deshacer la conversión a SNES, no a inventar "realismo".
   - Validar a ojo frente a los tiles originales escalados a nearest; la
     regla es que una reducción N→1 del tile HD con la misma paleta vuelva a
     aproximar el tile original (métrica automática: error de
     reproyección).
6. **Escala**: 4× (8×8 → 32×32). A 1080p, 224×4 = 896 líneas sin reescalado
   fraccional excesivo; 2× para Switch en portátil o si la memoria aprieta.
   El pack puede llevar varias escalas.

### 3.3 Formato del pack HD (externo)

```
dkc1-hd-pack/
  pack.json            manifiesto (schema dkc1.hd-pack.v1)
  tiles.idx            índice binario: hash → página, rect, tipo (indexado/RGBA), escala
  pages/0000.png …     atlas 2048×2048: tiles indexados (PNG paleta 8 bits) o RGBA
  overrides.json       opcional: reglas por contexto (hash + paleta / capa / ID de animación)
  LICENSE.txt, CREDITS.txt
```

`pack.json` contiene:
- `schema` y versión del pack.
- `rom_sha256` (`fa8cacf5…f74d15`). Un pack para otro ROM se rechaza.
- `scale` (2, 4), formato de las páginas y recuento de tiles.
- Hash de cada página, para detectar corrupción.
- Opcionalmente `generator` (modelo, pesos y parámetros) para poder
  reproducir el pack.

El origen de autoría son PNG (atlas + JSON de mapeo de paletas). Un paso de
"compilación" del pack (`tools/hd_pack_build.py`) genera `tiles.idx` y,
para Switch, una variante con páginas en bruto comprimidas con LZ4 (sin
decodificar PNG en la consola).

### 3.4 Carga y caché sin bloquear el render

Precedente: MSU-1 (`ConfiguredMusicPackPath`, `sdl_host.c:862`; `dkc1_msu1.c`)
abre un directorio externo opt-in, lo valida y lo proyecta en memoria con
`mmap`, sin copiar nada al repo ni al bundle. El pack HD sigue ese patrón con
un añadido: las páginas se cargan en segundo plano.

1. **Arranque** (síncrono, rápido): leer y validar `pack.json` y cargar
   `tiles.idx`, que ocupa pocos MB. Si algo falla, el mod queda desactivado y
   se muestra el estado en el menú, igual que Baby Kong.
2. **Hilo de carga** (`SDL_CreateThread`; en Switch puede fijarse a un núcleo
   libre): decodifica páginas bajo demanda y por prioridad (las que piden los
   tiles visibles en el frame actual primero; luego las del nivel actual,
   que se conocen tras el primer barrido de una ruta).
3. **Subida a GPU** en el hilo de presentación, con un presupuesto por frame
   (p. ej. 1–2 páginas), usando el mismo esquema de texturas con
   seguimiento de finalización que ya usa `macos_graphics.m`.
4. **Mientras una página no esté lista**, sus tiles se dibujan con el píxel
   nativo replicado (o Reconstruct). El frame nunca espera al disco.
5. **Caché LRU** de páginas en GPU con límite de memoria configurable. En
   Switch el límite debe ser conservador y se recomienda ejecutar en modo
   *title takeover* (más memoria que en modo applet).

---

## 4. Integración con la capa de mods

### 4.1 Cambio mínimo en el framework (`snesrecomp`)

- `PpuSetIdentityCapture(Ppu*, PpuIdentityPixel *main, PpuIdentityPixel *sub,
  size_t pitch)`: si es NULL (por defecto) no hace nada y el coste es cero.
- En las rutinas de dibujo, junto a cada escritura en `PpuPixelPrioBufs`, se
  guarda en un búfer paralelo por capa (dirección del carácter, texel, flips
  y paleta). En `PpuDrawWholeLine`, al decidir qué capa gana en main y sub,
  se copia esa referencia al G-buffer, junto con la operación de color math
  y el brillo de la línea.
- El hash del carácter se calcula por dirección en la VRAM **una vez por
  frame** y se invalida cuando se escribe en la VRAM (DMA o puerto `$2118/9`).
- Nada de esto entra en `ppu_saveload`; es estado de presentación.

### 4.2 Host DKC1

- `runner/dkc1_hd.{c,h}` (nuevo), con la misma forma que la API de Baby Kong:
  `Dkc1HdLoadPack`, `Dkc1HdSetEnabled`, `Dkc1HdEnabled`, `Dkc1HdStatus`,
  `Dkc1HdInitializeFromEnvironment`, `Dkc1HdPrepareFrame` (activa el
  G-buffer) y `Dkc1HdCollectFrame` (lo entrega al presentador).
- **Interacción con Baby Kong**: los píxeles que Baby Kong dibuja tras el
  barrido no tienen identidad; `Dkc1BabyKongDrawFrame` marca esas posiciones
  del G-buffer como `host-drawn` y el compositor usa el píxel nativo
  replicado para ellas.
- **Presentadores**:
  - macOS: un pase Metal nuevo antes de Reconstruct/CRT, en
    `macos_graphics.m`/`.metal`.
  - Windows: un shader GLSL 3.3 en `windows_graphics.c`.
  - Los filtros CRT y de color se siguen aplicando **sobre** la superficie HD.

### 4.3 Menú

- **macOS**: en la pestaña Mods de `macos_pause_menu.m`, una sección "HD
  textures" con "Enable / Disable HD", "Choose HD pack…" y una línea de
  estado (escala, nº de tiles, cobertura).
- **Windows**: las mismas entradas en el menú `&Mods` y en la página "Mods /
  Music" (`windows_platform.c`).
- **Persistencia**: `Mods/HdEnabled` y `Mods/HdPack`, como `BabyKong`.
- **Switch**: no hay menú. El mod se activa si existe
  `sdmc:/switch/dkc1/hd/pack.json`; se puede añadir un atajo de alternancia
  en builds `DEBUG=1`.

### 4.4 Variables de entorno (QA determinista)

Siguen el patrón `DKC1_MSU1_PACK` / `DKC1_MSU1_DISABLE`:

| Variable | Efecto |
|---|---|
| `DKC1_HD_PACK=/ruta/pack` | Pack a cargar; tiene prioridad sobre la preferencia guardada. |
| `DKC1_HD_DISABLE=1` | Fuerza la ruta stock aunque haya pack configurado. |
| `DKC1_HD_SCALE=2\|4` | Escala de salida, si el pack trae varias. |
| `DKC1_HD_DUMP=/ruta` | Modo dump (§3.1). Nunca dentro del repo. |
| `DKC1_HD_IDENTITY=1` | Pack sintético de identidad generado en memoria (cada tile HD = tile nativo a nearest). Base del test de equivalencia. |
| `DKC1_HD_COVERAGE_LOG=/ruta.jsonl` | Por frame: % de píxeles servidos por HD, tiles que faltan y páginas pendientes. |
| `DKC1_HD_SYNC_LOAD=1` | Solo QA: carga todas las páginas antes del primer frame, para que las capturas sean deterministas. |

Se usa `DKC1_HD_PACK` en vez de `DKC1_HD_TEXTURES` para alinearse con
`DKC1_MSU1_PACK`.

### 4.5 Widescreen

- El G-buffer tiene el ancho de `Dkc1VideoWidth()` (256, 308 o 342). Los
  márgenes los dibuja el PPU a partir del tilemap sombra, así que tienen
  identidad igual que el centro.
- La salida es `ancho·N × 224·N` con la misma relación de aspecto de píxel
  7:6 que hoy; los tiles se escalan de forma uniforme y no se deforman.
- Las políticas de borde (`glide`, `bars`, `reflect`, `shift`) ya se aplican
  sobre el píxel nativo. En `bars` las columnas en negro quedan sin
  identidad y salen negras.
- El sesgo de presentación (`wsPresentationXBias`) entra en el scroll antes
  de dibujar, así que el texel registrado ya lo incluye.

### 4.6 MSU-1

Es independiente (solo audio). Comparte el patrón de carga y la página "Mods /
Music". El compositor no interactúa con él. En Switch MSU-1 sigue siendo un
stub.

---

## 5. Plan por fases

### Fase 1 — G-buffer e intercepción (sin assets externos)

Valida la tubería completa antes de producir arte.

Entregables:
- G-buffer opt-in en `snesrecomp` (§4.1) para BG 2/4 bpp y OBJ. Mosaico, 8
  bpp y Mode 7 quedan marcados como "sin identidad" (salen con nearest).
- `Dkc1HdIdentity`: pack sintético de identidad generado en memoria.
- Compositor CPU de referencia (C puro, usado también en tests y como ruta
  de Switch) y pase Metal en macOS.
- Pase GLSL en Windows.
- Superficie HD mostrada con Reconstruct como relleno.

Puertas de validación:
- Con HD desactivado: hashes de frame, WRAM, VRAM, CGRAM y OAM idénticos a
  stock en `route_jungle`.
- Con HD activado: WRAM, VRAM, CGRAM, OAM y `renderBuffer` crudo idénticos a
  HD desactivado (prueba de que es solo presentación), verificable con
  `first_divergence.py` y los contratos A/B.
- **Equivalencia de identidad**: reducir la salida N× por bloques con el pack
  de identidad debe dar exactamente `renderBuffer`, frame a frame. Cualquier
  diferencia es un píxel con identidad o color math mal resuelto.

### Fase 2 — Extracción y primer pack (Jungle Hijinxs)

- `DKC1_HD_DUMP` y `tools/hd_dump_index.py`, que agrupa por contexto
  (metatiles y frames de OAM).
- `tools/hd_upscale.py`: modelo → requantización a paleta → corte en tiles →
  atlas.
- `tools/hd_pack_build.py`: genera `tiles.idx` y la variante LZ4 para Switch.
- Pack de prueba **local** cubriendo `route_jungle.dks`: fondo de jungla,
  Donkey, Diddy, Kremlings básicos, bananas, barriles y HUD.
- Informe de cobertura (`DKC1_HD_COVERAGE_LOG`), con objetivo ≥ 95 % de los
  píxeles con HD a lo largo de la ruta.
- Revisión visual frente a nearest en 3–4 instantáneas fijas
  (`capture_jungle_snapshots.dks`).

### Fase 3 — Integración en mods, widescreen y MSU-1

- Entradas de menú en macOS y Windows, persistencia, variables de entorno y
  el comportamiento de Switch por presencia del pack.
- Cargador asíncrono con LRU y presupuesto de subida a GPU.
- Interacción con Baby Kong (`host-drawn`).
- Barridos widescreen 308/342 con `tools/level_sweep.py` y las rutas de
  cuerdas/márgenes (`rope_to_left_margin.dks`, `rope_to_right_margin.dks`);
  `compare_widescreen_regions.py` sobre la salida reducida.
- Prueba combinada MSU-1 + HD.

### Fase 4 — Rendimiento, validación y empaquetado

- **Switch**: compositor CPU a 2× sobre 342×224 (≈ 306 k subpíxeles por frame)
  con NEON, en un hilo aparte del de emulación. Si no llega a 60 Hz estables,
  se sustituye `SDL_Renderer` por un presentador EGL+GLES propio (mesa de
  devkitPro) con el mismo shader que Windows. Medición con
  `DKC1_PACING_LOG` y `tools/analyze_pacing.py`.
- **macOS 120 Hz**: composición una vez por frame lógico y re-presentación
  cacheada (mismo patrón que Reconstruct/CRT). Sin fotogramas perdidos en el
  registro de *pacing*.
- **Determinismo**: grabador de vuelo (`dkc1_flight_recorder.c`) con HD
  activo y `tools/verify_flight_bundle.py`; la reproducción debe coincidir
  en WRAM/VRAM/CGRAM/OAM con la grabación. Validador headless
  (`dkc1_snesrecomp_headless`) con y sin `DKC1_HD_PACK`, mismos hashes de
  máquina. `tools/run_regression.py` con los contratos `jungle-entry` y
  `jungle-death-transition`.
- **Empaquetado**: el binario no lleva pack; se publican las herramientas
  (`hd_dump`, `hd_upscale`, `hd_pack_build`) y la receta (modelo y
  parámetros) para que cada usuario genere su pack a partir de su ROM.

---

## 6. Riesgos y decisiones abiertas

- **Tiles compartidos en contextos distintos**: un mismo carácter de 8×8
  puede formar parte de varios metatiles. La primera versión usa el
  contexto más frecuente; `overrides.json` permite claves por paleta o
  capa más adelante.
- **Animaciones que regrabean la VRAM cada frame** (agua, cascadas): cada
  frame de animación es un hash distinto; todos deben estar en el pack o
  parpadearán entre HD y nativo. El informe de cobertura los delata.
- **Mosaico, 8 bpp, Mode 7**: fuera del alcance de la Fase 1; salen con
  nearest. Hay que confirmar en el barrido de niveles si DKC1 los usa y
  dónde.
- **Distribución del pack (legal)**: un pack escalado a partir de los gráficos
  del ROM es una obra derivada del arte de Nintendo/Rare, aunque se haya
  generado con IA o se haya repintado. Distribuirlo plantea el mismo
  problema que distribuir los assets extraídos. La recomendación es
  **distribuir la herramienta y la receta, no los píxeles**. Esto no es
  asesoramiento legal.
- **Licencias**: el código del host y del mod es MIT. El cambio en
  `snesrecomp` queda bajo PolyForm Noncommercial, igual que el framework: el
  mod es gratuito y no puede venderse junto con el framework. La metadata
  estructural derivada de la desensamblación sigue siendo GPL-3.
