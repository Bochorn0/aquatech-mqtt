# Instrucciones para Instalar Librerías - ESP32 LoRa

## Librerías Requeridas

Para compilar el sketch `componente_lora.ino`, necesitas instalar las siguientes librerías:

### 1. LoRa Library ⚠️ IMPORTANTE
- **Autor**: Sandeep Mistry
- **Método 1: Desde Library Manager (Recomendado)**
  1. Abre Arduino IDE
  2. Ve a: `Sketch` → `Include Library` → `Manage Libraries...` (o `Tools` → `Manage Libraries...`)
  3. Busca: **"LoRa"** (escribe exactamente "LoRa" sin espacios)
  4. Busca la librería **"LoRa"** por **Sandeep Mistry**
  5. Haz clic en **"Install"**

- **Método 2: Instalación Manual desde GitHub (Si no aparece en Library Manager)**
  1. Ve a: https://github.com/sandeepmistry/arduino-LoRa
  2. Haz clic en el botón verde **"Code"** → **"Download ZIP"**
  3. En Arduino IDE, ve a: `Sketch` → `Include Library` → `Add .ZIP Library...`
  4. Selecciona el archivo ZIP que descargaste
  5. Espera a que se instale

- **Método 3: Instalación Manual (Carpeta)**
  1. Descarga desde: https://github.com/sandeepmistry/arduino-LoRa/archive/refs/heads/master.zip
  2. Extrae el ZIP
  3. Renombra la carpeta extraída de `arduino-LoRa-master` a `LoRa`
  4. Copia la carpeta `LoRa` a tu carpeta de librerías de Arduino:
     - **Windows**: `C:\Users\TU_USUARIO\Documents\Arduino\libraries\`
     - **Mac**: `~/Documents/Arduino/libraries/`
     - **Linux**: `~/Arduino/libraries/`
  5. Reinicia Arduino IDE

### 2. Adafruit GFX Library
- **Autor**: Adafruit
- **Instalación**:
  1. En `Manage Libraries...`
  2. Busca: **"Adafruit GFX Library"**
  3. Instala: **"Adafruit GFX Library"** por Adafruit

### 3. Adafruit SSD1306
- **Autor**: Adafruit
- **Instalación**:
  1. En `Manage Libraries...`
  2. Busca: **"Adafruit SSD1306"**
  3. Instala: **"Adafruit SSD1306"** por Adafruit

## Instalación Rápida (Arduino IDE 2.x)

1. Abre Arduino IDE
2. Ve a: `Tools` → `Manage Libraries...` (o presiona `Ctrl+Shift+I` / `Cmd+Shift+I` en Mac)
3. Busca e instala en este orden:
   - **LoRa** (Sandeep Mistry) - Si no aparece, usa el Método 2 o 3 de arriba
   - **Adafruit GFX Library** (Adafruit)
   - **Adafruit SSD1306** (Adafruit)

## 🔧 Solución de Problemas

### Si "LoRa" no aparece en Library Manager:

1. **Verifica que tienes conexión a internet**
2. **Actualiza el índice de librerías:**
   - En Library Manager, haz clic en el botón de actualizar/refresh
3. **Busca variaciones del nombre:**
   - Prueba buscar: "lora" (minúsculas)
   - Prueba buscar: "arduino-LoRa"
   - Prueba buscar: "Sandeep Mistry"
4. **Instala manualmente desde GitHub** (Método 2 o 3 arriba)
5. **Verifica la ubicación de librerías:**
   - En Arduino IDE: `File` → `Preferences`
   - Revisa la ruta en "Sketchbook location"
   - Las librerías deben estar en: `[Sketchbook location]/libraries/`

## Verificación

Después de instalar las librerías, intenta compilar el sketch nuevamente.
Si aún hay errores, verifica que:
- Tienes el ESP32 Board Support instalado
- La versión de las librerías es compatible con ESP32

## Nota sobre ESP32 Board Support

Si no tienes el soporte para ESP32 instalado:
1. Ve a: `File` → `Preferences`
2. En "Additional Boards Manager URLs", agrega:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   O la URL más reciente:
   ```
   https://dl.espressif.com/dl/package_esp32_index.json
   ```
3. Ve a: `Tools` → `Board` → `Boards Manager...`
4. Busca: **"esp32"**
5. Instala: **"esp32"** por Espressif Systems

## 📋 Enlaces Directos para Descarga Manual

Si el Library Manager no funciona, descarga directamente:

- **LoRa Library**: https://github.com/sandeepmistry/arduino-LoRa/archive/refs/heads/master.zip
- **Adafruit GFX**: https://github.com/adafruit/Adafruit-GFX-Library/archive/refs/heads/master.zip
- **Adafruit SSD1306**: https://github.com/adafruit/Adafruit_SSD1306/archive/refs/heads/master.zip

**Instrucciones para instalar desde ZIP:**
1. Descarga el archivo ZIP
2. En Arduino IDE: `Sketch` → `Include Library` → `Add .ZIP Library...`
3. Selecciona el archivo ZIP descargado
4. Espera a que se instale
5. Reinicia Arduino IDE
