# TamaMPE Virtual FM-1

Prototipo de desarrollo que ejecuta el firmware C de TamaMPE en WebAssembly.
La pantalla de 240×240 procede de `ui_draw.c` y el audio procede del ISR y el
mezclador reales; la página solo representa el panel y transporta entradas.

## Construir y abrir

```sh
cd /workspace/TamaMPE
bash web/fm1-virtual/build.sh
python3 -m http.server 8000 --directory build/fm1-virtual
```

Abrir `http://localhost:8000/` y pulsar **Activar audio y firmware**. El
panel envía botones, 27 teclas, encoderes y MASTER al código C; la pantalla no
se reproduce en HTML. `build.sh` usa Emscripten disponible en `PATH` o el
paquete reproducible del entorno de desarrollo; fuera de este entorno se puede
indicar su ubicación con `TAMAMPE_WASM_ROOT`.

`node web/fm1-virtual/verify.mjs` compara 65.536 muestras de la ruta de audio
WASM con la misma adaptación compilada en host y verifica la pantalla y una
tecla física.

## Límites del primer prototipo

No emula USB, TRS, Bluetooth, el controlador LCD ni las temporizaciones
eléctricas. MPE IN/OUT, Smart Macros y persistencia se conservan en el
firmware compilado, pero todavía no tienen transporte o UX web específico.
La NOR simulada está expuesta por el adaptador para una futura capa de
persistencia del navegador.

## Licencia y atribución

Este adaptador es GPL-3.0-only. Se adaptó de
`felucca-base/web/emu/felucca_web.c` de
[DUELL](https://github.com/oscillr/DUELL), también GPL-3.0-only; ese diseño de
emulador de navegador procede a su vez de X0X de charlesvestal. Conserva los
avisos de copyright de Felucca/Hügelton Instruments. El panel web de TamaMPE
es propio y no utiliza logotipos ni identidad gráfica de DUELL.
