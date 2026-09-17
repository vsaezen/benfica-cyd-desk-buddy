# Instalación en una CYD nueva

## 1. Hardware compatible

La configuración verificada corresponde a:

- Freenove `FNK0114L_3P2` / `E32R32P`.
- ESP32 clásico de doble núcleo.
- Pantalla IPS táctil ST7789, 240x320 píxeles.
- Ranura microSD y amplificador analógico integrados.
- Altavoz pasivo de 8 ohmios y 1 W.

Si el modelo serigrafiado es distinto, no lo programes con estos pines sin
comparar antes el esquema del fabricante.

## 2. Preparar Arduino IDE

Configuración usada para compilar la versión probada:

| Componente | Versión/configuración |
|---|---|
| ESP32 Arduino | 2.0.11 |
| Placa | ESP32 Dev Module |
| Upload Speed | 460800 |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| ArduinoJson | 7.4.2 |
| ESP8266Audio | 2.0.0 |
| TFT_eSPI Freenove | 2.5.43 |

Instala ArduinoJson y ESP8266Audio desde el gestor de bibliotecas. Para la
pantalla, instala la copia de TFT_eSPI incluida en el tutorial de Freenove para
esta placa. En `User_Setup_Select.h` debe quedar activa esta selección:

```cpp
#define FNK0114L_3P2_240x320_ST7789
```

Las demás selecciones Freenove del mismo bloque deben estar comentadas. El
archivo exacto de configuración usado está en:

```text
extras/TFT_eSPI_Setups/FNK0114L_3.2_240x320_ST7789.h
```

## 3. Preparar la microSD

1. Formatea la tarjeta como FAT32.
2. Copia la carpeta `sdcard/Benfica` completa a la raíz.
3. La ruta final debe ser `/Benfica/goal.mp3`, no
   `/sdcard/Benfica/goal.mp3` ni `/Benfica/Benfica/goal.mp3`.
4. Inserta la tarjeta con la placa apagada.

## 4. Subir el firmware

1. Conserva juntos `BenficaCYD.ino`, `BenficaAssets.h` y `BenficaFont.h` dentro
   de la carpeta `firmware/BenficaCYD`.
2. Abre `BenficaCYD.ino`.
3. Selecciona la placa, partición y puerto USB correctos.
4. Compila y sube.
5. No actives OTA: esta compilación utiliza una partición grande y las futuras
   actualizaciones se realizan por USB.

## 5. Configurar el Wi-Fi

En una placa nueva no existe ninguna contraseña guardada. Al arrancar creará la
red `Benfica-CYD-Setup`.

1. Conecta un móvil a esa red.
2. Abre `http://192.168.4.1` si el portal no aparece automáticamente.
3. Introduce la red Wi-Fi de 2,4 GHz y su contraseña.
4. La contraseña queda únicamente en la memoria privada de esa CYD y nunca se
   incorpora al sketch ni al repositorio.

## 6. Comprobación final

- La página inicial debe responder al tacto y deslizar con fluidez.
- La tarjeta debe reproducir los cuatro sonidos.
- La página 1904 debe mostrar la letra sincronizada.
- El dispositivo debe recuperar el Wi-Fi después de reproducir audio.
- La información deportiva puede tardar en la primera instalación si ESPN no
  entrega una respuesta completa. Esto no impide probar la interfaz y el audio.

Para repetir el montaje en otra unidad, vuelve a usar la misma carpeta, la misma
configuración de compilación y una copia independiente de la microSD.

