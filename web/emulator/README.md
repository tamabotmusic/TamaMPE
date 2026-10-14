# TamaMPE · prototipo FM6 en navegador

Herramienta auxiliar para desarrollar el firmware del M-VAVE FM-1. No es un
plugin ni un producto independiente. Base: `9481863c7ad3810274e75a9b817d1b314b491e4c`.

## Compilar y escuchar

Necesita Emscripten (verificado con 3.1.69), Node.js y un compilador C nativo.
Activa tu SDK Emscripten antes de ejecutar:

```sh
cd /workspace/TamaMPE
# Si no existen build/gen/*, generar los encabezados sin compilar hardware:
python3 -c 'import sys; sys.path.insert(0,"tools"); import build; build.generate()'
python3 tools/gen_909_tables.py build/gen/x0x_drum_tables.h
bash web/emulator/build.sh
python3 -m http.server 8000 --bind 127.0.0.1
```

Abre `http://localhost:8000/web/emulator/`, pulsa **Activar audio** y mantén una
tecla virtual pulsada. También puedes enfocar una tecla y usar espacio o Enter.
AudioWorklet requiere HTTPS o localhost. No abrir mediante `file://`.
La compilación escribe únicamente en `build/emulator/`, ignorado por Git.

En este entorno el paquete Debian se extrajo sin instalación global. Activación:

```sh
wasm_root=/workspace/.tamampe-deps/wasm-debian
export PATH="/opt/codex/runtimes/codex-primary-runtime/dependencies/node/bin:$wasm_root/usr/share/emscripten:$wasm_root/usr/bin:$PATH"
export LD_LIBRARY_PATH="$wasm_root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EM_LLVM_ROOT="$wasm_root/usr/bin"
export EM_BINARYEN_ROOT="$wasm_root/usr"
export EM_NODE_JS=/opt/codex/runtimes/codex-primary-runtime/dependencies/node/bin/node
```

## Código y verificación

`fm6_bridge.c` incluye el harness existente `tests/hostsim.c`, que incluye
directamente `eng_fm6.c`, `fm6_core.c`, la gestión de voces y la modulación del
firmware. No contiene una copia del algoritmo FM. Las secciones de memoria
hardware se neutralizan mediante la adaptación existente de hostsim.
No se invoca la inicialización HAL ni se modifica el mapa de memoria del FM-1.
Otros motores presentes en el harness no se exponen por la API del prototipo.

Preset inicial: FM6 factory 0, TINE EP, en POLY. El motor conserva 44.100 Hz,
bloques de pista de 32 muestras y su control interno FM6 de 64 muestras.
Salida seca mono duplicada en estéreo, con atenuación fija de 18,06 dB para
dejar margen al sumar voces. No hay efectos ni cambios al preset DX7.

El hilo principal descarga y transfiere los bytes de WebAssembly; el worklet
compila e instancia el módulo antes de habilitar el teclado. No necesita
COOP/COEP ni SharedArrayBuffer. AudioWorklet ejecuta C sin crecer memoria (16 MiB reservados), sin
crear arrays, hacer fetch ni enviar mensajes por bloque en `process()`.
El adaptador conserva la fase entre cuantos Web Audio y usa interpolación
lineal cuando cambia la frecuencia. No añade un productor asíncrono ni una
cola que pueda quedarse sin datos; debe cumplir el plazo de audio del navegador.
Los eventos se aplican entre cuantos, con hasta un bloque C ya precalculado.

El build compara 131.072 muestras float32 con el mismo harness compilado
nativamente (dos notas, solapamiento y release). El umbral de diferencia RMS
relativa es 0,1%; se informa la diferencia real y se comprueban señal no nula,
valores finitos y rango. También prueba el adaptador AudioWorklet a 22.050,
44.100, 48.000 y 96.000 Hz con tamaños de cuanto variables y referencia directa.

Para la integración opcional en navegador, con el servidor anterior activo y
Playwright disponible en Node:

```sh
CHROMIUM_PATH=/usr/bin/chromium node web/emulator/browser_test.mjs
```

Comprueba audio no nulo a 44.100 y 48.000 Hz, inicialización real de AudioWorklet,
Note On/Off y liberación al perder el foco. En la validación inicial pasó en
Chromium: RMS 0,0407 y 0,0411, respectivamente. La comparación C/WASM produjo
error PCM cero, pico 0,1280 y silencio al terminar las colas de release.

## Límites y siguientes pasos

- Un track FM6, un preset, velocidad fija 100; no reproduce interfaz ni periféricos.
- Sin Web MIDI/MPE en la interfaz inicial. El código de bend y presión del motor
  está reutilizado, pero la API solo ofrece notas convencionales.
- La interpolación lineal no sustituye un remuestreador con filtro antialias,
  especialmente al bajar de 44.100 Hz; comparar PCM antes de remuestrear.
- Un único hilo WASM: no valida temporización, concurrencia, cachés, RAMTEXT,
  DAC ni latencia del dual-core hardware. Los 16 MiB no representan la RAM del FM-1.
- El navegador/OS puede incumplir plazos bajo carga. Las pruebas offline no
  garantizan ausencia de underruns en todos los dispositivos; falta escucha manual.
- Próximo paso: añadir una entrada MIDI al harness usando las colas y RPN
  existentes, y probar notas MPE, pitch bend y presión por identidad.
  CC74 necesita primero una implementación y pruebas propias en el firmware.

Licencia: GPL-3.0-only, como el firmware reutilizado; ver LICENSE y LICENSING.md.
