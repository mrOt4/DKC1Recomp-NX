# DKC1 HD — mod de texturas en alta resolución

> **Estado: en diseño.** Este README describe cómo funcionará el mod cuando
> esté implementado; el diseño completo está en
> [`HD_REMASTER.md`](HD_REMASTER.md). Las cifras marcadas como *estimación*
> se confirmarán en la Fase 2.

Mod opcional para DKC1Recomp-NX que vuelve a dibujar tiles, sprites y fondos
en alta resolución **solo en la presentación**. La lógica del juego, la
física, la colisión, los guardados y el framebuffer nativo no cambian; con el
mod desactivado el juego es byte a byte el de stock.

El mod **no incluye gráficos**. Cada usuario genera su propio pack HD a partir
de **su** ROM con las herramientas incluidas.

## Requisitos

| Requisito | Detalle |
|---|---|
| ROM | *Donkey Kong Country* (USA) v1.0, sin cabecera, 4 194 304 bytes. SHA-256 `fa8cacf5bbfc39ee6bbaa557adf89133d60d42f6cf9e1db30d5a36a469f74d15`. Cualquier otra ROM se rechaza. |
| Host | DKC1Recomp-NX con soporte HD: macOS (Metal), Windows (OpenGL 3.3) o Switch (homebrew, libnx). |
| Disco | Jungle Hijinxs a 4×: 18,9 MB (18 144 tiles). El juego completo se estima en unos cientos de MB. El volcado intermedio ocupa unos 120 MB. |
| Generar el pack | Python 3.10+; PyTorch y GPU recomendados para el paso de upscaling (con CPU funciona, pero mucho más lento). |
| Switch | Se recomienda lanzarlo en modo *title takeover* (mantener R al abrir un juego) para disponer de más memoria. |

## 1. Generar tu pack

Ninguno de estos pasos escribe dentro del repositorio. Usa una carpeta propia,
por ejemplo `~/dkc1-hd/`.

```sh
# 1. Volcar los tiles que aparecen al jugar, en widescreen (la ROM se
#    verifica por SHA-256). Escribe ~/dkc1-hd/dump/dump.bin al salir.
mkdir -p ~/dkc1-hd/dump
DKC1_HD_DUMP=~/dkc1-hd/dump DKC1_WIDESCREEN=1 \
DKC1_SCRIPT=recipes/hd_dump_jungle.dks \
  build/dkc1_snesrecomp_headless "/ruta/Donkey Kong Country (USA).sfc" 20000

# 2. Escalar, cuantizar a la paleta original y escribir el pack. Necesita
#    realesrgan-ncnn-vulkan (releases de xinntao/Real-ESRGAN) y una GPU Vulkan.
python tools/hd_pack.py --dump ~/dkc1-hd/dump \
  --rom "/ruta/Donkey Kong Country (USA).sfc" \
  --upscaler /ruta/realesrgan-ncnn-vulkan --out ~/dkc1-hd/DKC1-HD-Remaster
```

Valores por defecto de la receta "DKC1 HD Remaster": modelo
`realesr-animevideov3`, escala 4, `--detail 0.35`, `--dither 0.7`.

Cuantas más rutas o partidas vuelques en el paso 1, más cobertura tendrá el
pack. Los tiles que falten se dibujan con el píxel original (o con el filtro
Reconstruct), así que el juego nunca se rompe.

## 2. Instalar y activar

**macOS / Windows**: menú **Mods → Choose HD Pack…** (en Windows se elige
el `tiles.bin` de la carpeta del pack) y **Mods → HD Textures** para
activarlo o desactivarlo; lo mismo está en la pestaña *Mods* del menú de
pausa. La carpeta y el estado se recuerdan entre sesiones
(`NSUserDefaults` en macOS, ajustes del host en Windows). El HD solo se ve
con aspecto 16:9 o 16:10. `DKC1_HD_PACK` y `DKC1_HD_DISABLE` tienen
prioridad sobre lo guardado.

**Switch**: copia la carpeta del pack a `sdmc:/switch/dkc1/hd/` (debe quedar
`sdmc:/switch/dkc1/hd/pack.json`). Si existe, el mod se activa al arrancar.

**Variables de entorno** (QA y pruebas deterministas):

| Variable | Efecto |
|---|---|
| `DKC1_HD_PACK=/ruta/pack` | Carga este pack; tiene prioridad sobre la preferencia guardada. |
| `DKC1_HD_DISABLE=1` | Fuerza la ruta original. |
| `DKC1_HD_IDENTITY=1` | Pack de identidad sintético (prueba de equivalencia; no necesita pack). |
| `DKC1_HD_DEBUG=grid` / `misses` | Rejilla de tiles / tiñe de magenta lo que falta en el pack. |
| `DKC1_HD_DUMP=/ruta` | Modo volcado para generar packs. |
| `DKC1_HD_PPM`, `DKC1_HD_GBUF` | (headless) Guardan el último frame HD / el G-buffer. |
| `DKC1_HD_THREADS=N` | Hilos del compositor (por defecto, hasta 4; 1 = un solo hilo). |
| `DKC1_HD_DEBLOCK=0` | Desactiva el suavizado de costuras entre tiles. |

El HD solo se activa en widescreen (16:10 o 16:9). En 4:3 el juego se ve
siempre como el original.

## Compatibilidad

- **Widescreen** 256, 308 (16:10) y 342 (16:9) columnas: los márgenes también
  usan texturas HD y las proporciones no se deforman.
- **Filtros**: CRT y los perfiles de color se aplican sobre la imagen HD.
- **Baby Kong**: compatible. Kiddy se dibuja con sus gráficos nativos.
- **MSU-1**: independiente; se pueden usar ambos a la vez.
- **Partidas guardadas y estados**: no cambian, y son intercambiables con y
  sin el mod.

## Qué no hace

- No cambia la jugabilidad, la física, la colisión ni los guardados.
- No redibuja el arte con otro estilo: el objetivo es un remaster fiel al
  CGI pre-renderizado de Rare, con la misma paleta.
- No distribuye ni contiene gráficos de Nintendo/Rare.

## Créditos y licencia

- **Código del mod y del host DKC1Recomp**: licencia MIT.
- **Framework `snesrecomp`** (incluido el soporte de identidad de tiles):
  PolyForm Noncommercial 1.0.0. El mod es **gratuito** y no puede venderse
  ni usarse comercialmente junto con el framework.
- **Metadata estructural de DKC1**: conserva la licencia GPL-3 de la
  desensamblación de Yoshifanatic1 (ver `THIRD_PARTY_NOTICES.md`).
- **Modelos de upscaling**: Real-ESRGAN (BSD-3-Clause), SwinIR (Apache-2.0),
  waifu2x (MIT). Revisa además la licencia de **los pesos** que uses.
- **Packs HD**: son obras derivadas del arte original de *Donkey Kong
  Country* (© Nintendo / Rare). Genéralos para uso personal a partir de tu
  propia copia del juego y **no los redistribuyas**. Esto no es
  asesoramiento legal.
- DKC1Recomp: elliotttate y colaboradores. Port de Switch: Souldbminer.
  *Donkey Kong Country* es una marca de Nintendo; este proyecto no está
  afiliado a Nintendo ni a Rare.
