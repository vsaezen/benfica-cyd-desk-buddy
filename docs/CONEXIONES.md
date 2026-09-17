# Esquema de conexiones

La pantalla, el táctil, la microSD y el amplificador ya forman parte de la
Freenove. La versión final no requiere una protoboard ni un cableado externo de
GPIO.

```text
                    USB-C / 5 V
                         │
                         ▼
              ┌────────────────────┐
              │ Freenove 3.2 CYD   │
 microSD ───► │ FNK0114L / E32R32P │
              │                    │
              │ salida SPK+  ──────┼────► altavoz 8 ohmios / 1 W
              │ salida SPK-  ──────┼────► altavoz 8 ohmios / 1 W
              └────────────────────┘
```

El altavoz se conecta a la **salida de dos pines del amplificador integrado**.
No debe conectarse directamente entre GPIO25 y masa: GPIO25 es la señal DAC que
alimenta internamente el amplificador, no una salida capaz de mover por sí sola
el altavoz.

## Pines usados internamente

| Función | GPIO |
|---|---:|
| Retroiluminación | 27 |
| Activación del amplificador | 4 |
| DAC de audio hacia el amplificador | 25 |
| SD SCK | 18 |
| SD MISO | 19 |
| SD MOSI | 23 |
| SD CS | 5 |

La propia pantalla usa un bus SPI diferente configurado en la biblioteca de
Freenove:

| Función | GPIO |
|---|---:|
| TFT MISO | 12 |
| TFT MOSI | 13 |
| TFT SCLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| Touch CS | 33 |

## Control háptico

El soporte DRV2605L está desactivado en esta versión. En esta revisión de la
placa, el conector I2C utiliza `IO32` para SDA y `IO25` para SCL, pero IO25
también alimenta el amplificador. Para evitar ruidos, bloqueos y pérdida de
fluidez se deja GPIO25 dedicado al audio.

No conectes el controlador háptico en la réplica final. El hueco de la carcasa
puede conservarse para futuras revisiones del hardware.

