# Protección frente a actualizaciones interrumpidas

Esta protección se adapta de [ChoralRoot FM-1 v0.16](https://github.com/Quixotic7/ChoralRootFM1/tree/v0.16), commit `9c7445cdd7b767c779741d28654fb59cca3acd97`, bajo GPL-3.0-only, a través de Melodee `v1.3.5` (`baeabc495c5ae0a6df48c6cf81d056fbc59abb6b`). Se conservan las firmas `CRSTUB` y el formato de marcador `CRBM`, así como sus atribuciones en el código.

## Arquitectura

El SPL entra primero en un *boot stub* independiente. El stub valida un marcador de finalización y el CRC-32 del cuerpo de la aplicación antes de ejecutar TamaMPE. Si la aplicación está incompleta, comprueba el loader USB retenido; si es válido, solicita al SPL iniciar ese loader. Si no lo es, entra en UBOOT ROM. Mantener **OCT− solo** durante el encendido solicita igualmente la ruta de recuperación; OCT− y OCT+ quedan para el gesto de calibración de la aplicación.

| Rango de flash | Uso |
| --- | --- |
| `0x00000..0x03FFF` | SPL y configuración del chip; el actualizador no escribe aquí |
| `0x04000..0x0411F` | Directorio de aplicación |
| `0x04120..0x04FFF` | Boot stub |
| `0x05000..0x90FFF` | Cuerpo de TamaMPE, enlazado en XIP `0x02001000` |
| `0x91000..0x91FFF` | Sector del marcador; marcador en `0x91FE0` |
| `0x92000..0x92FFF` | Configuración y relleno de la ranura de aplicación |
| `0xE0000..0xE3FFF` | Loader USB retenido |
| `0xE4000..0xE4FFF` | Registro de actualización separado |

El marcador de 20 bytes contiene magia, formato, longitud alineada, CRC-32 del cuerpo y CRC-32 del propio marcador. Antes de escribir cualquier sector modificado, el loader borra y verifica el sector del marcador, escribe y verifica el cuerpo y confirma escribiendo ese sector al final. Rechaza un marcador inválido antes de borrar nada. Los paquetes heredados, sin stub, conservan el orden anterior.

Cuatro bytes de compensación mantienen constante el CRC-16 de `app.bin`. Así, con el mismo stub, el sector `0x4000` no cambia entre actualizaciones ordinarias y el loader no lo reescribe. El build fija versión y SHA-256 del stub en `firmware/stub/stub.lock`.

El stub reserva solo 32 bytes de RAM NOINIT en `0x01C7C000..0x01C7C01F`; el estado retenido de TamaMPE comienza en `0x01C7C020`. No cambia presets, proyectos ni regiones de datos de usuario.

## Límite importante

La **primera instalación** que introduce el stub, una actualización que cambie el stub o un cambio de directorio/SDK todavía debe reescribir el sector `0x4000`. Un corte de alimentación durante esa ventana no queda protegido y puede requerir recuperación física. Esta implementación no se considera apta para instalar hasta completar las pruebas físicas de arranque, OCT−, actualización interrumpida y recuperación USB.

Los tests de host validan formatos, CRC, orden de sectores y modelos de interrupción. No prueban la enumeración USB real, una pérdida de alimentación real ni la recuperación en un FM-1.
