# ESP32-P4 + OV5647 — Cámara MIPI CSI con ISP, PPA, Gaussian, Sobel, JPEG y streaming MJPEG

## Estado del proyecto

Este README documenta el estado actual del sistema al momento de escribirlo.

El sistema está construido alrededor de un **ESP32-P4** y un sensor **OV5647** conectado mediante **MIPI CSI de 2 lanes**. El objetivo actual es capturar una imagen de cámara, procesarla con hardware y software de bajo nivel, aplicar un filtro de **Gaussian 3x3** seguido de **Sobel**, codificar el resultado en **JPEG mediante el periférico JPEG de hardware** y entregarlo por HTTP como **MJPEG**.

La configuración que está funcionando actualmente utiliza:

- ESP32-P4 rev. 1.0.
- 32 MB de PSRAM a 200 MHz.
- Sensor OV5647.
- MIPI CSI de 2 lanes.
- Modo del sensor: `MIPI_2lane_24Minput_RAW8_800x800_50fps`.
- CSI en `RAW8` passthrough.
- ISP: `RAW8 -> RGB565` con demosaic.
- Orden Bayer utilizado: `GBRG`.
- PPA: conversión `RGB565 -> GRAY8` y reducción a 400x400.
- Gaussian 3x3 a 400x400.
- Sobel a 400x400.
- Comparación en vivo `GRAY | SOBEL` en una única imagen JPEG de 800x400.
- JPEG por hardware.
- Servidor HTTP integrado.
- Wi-Fi del ESP32-P4 mediante coprocesador ESP32-C6 usando ESP-Hosted/SDIO.

La visualización que está validada actualmente es el vídeo en vivo. El endpoint `/snapshot` no forma parte del flujo de uso actual porque la ruta de 400x400 a 800x800 mediante el PPA en modo `GRAY8` no es utilizable en el ESP32-P4 rev. 1.0 empleado en este proyecto. El vídeo en vivo no depende de esa etapa de escalado porque el modo `/compare` genera directamente una imagen 800x400.

---

# 1. Resumen de alto nivel

La cadena completa es:

```text
                    SENSOR OV5647
                         |
                         | MIPI CSI-2, 2 lanes
                         v
                 +----------------+
                 |   CSI / CAM    |
                 |   RAW8         |
                 +----------------+
                         |
                         v
                 +----------------+
                 |      ISP       |
                 | RAW8 -> RGB565 |
                 |   Demosaic     |
                 +----------------+
                         |
                         | RGB565 800x800
                         v
                 +----------------+
                 |      PPA       |
                 | RGB565 -> GRAY8|
                 | 800x800 ->     |
                 | 400x400        |
                 +----------------+
                         |
                         | GRAY8 400x400
                         v
                 +----------------+
                 |   Gaussian 3x3 |
                 |  suavizado      |
                 +----------------+
                         |
                         v
                 +----------------+
                 |     Sobel      |
                 | gradiente       |
                 +----------------+
                         |
                         | GRAY8 400x400
                         v
              +------------------------+
              |                        |
              |       /compare         |
              |                        |
              v                        v
       +-------------+          +-------------+
       | ORIGINAL    |          |    SOBEL    |
       | 400x400     |          | 400x400     |
       +-------------+          +-------------+
              \                      /
               \                    /
                +------------------+
                | JPEG HW encoder  |
                +------------------+
                         |
                         v
                HTTP MJPEG / Wi-Fi

```

En otras palabras, el sistema no transmite el RAW8 de la cámara por Wi-Fi. Tampoco transmite RGB565. El dato que finalmente viaja por HTTP es una secuencia de imágenes JPEG.

---

# 2. Objetivo actual

El objetivo actual es utilizar la cámara como fuente de datos para una cadena de procesamiento de imágenes en tiempo real.

La idea conceptual es:

```text
capturar
   -> convertir
   -> suavizar
   -> detectar cambios bruscos de intensidad
   -> visualizar
   -> comprimir
   -> transmitir
```

Esto permite que, posteriormente, la salida de Sobel pueda alimentar algoritmos más específicos, por ejemplo:

```text
Sobel
  -> threshold
  -> ROI
  -> detección de líneas
  -> contornos
  -> geometría de bordes
  -> detección de estructuras
```

Una de las aplicaciones que se está considerando es detectar patrones asociados con una **zanja**, pero esa etapa todavía no forma parte del sistema.

---

# 3. Hardware

## 3.1 ESP32-P4

El ESP32-P4 es el procesador principal del sistema.

En las pruebas actuales el sistema informa:

```text
Chip revision: v1.0
Found 32MB PSRAM device
Speed: 200MHz
```

La PSRAM es fundamental porque algunos buffers de imagen tienen cientos de kilobytes o más de un megabyte.

### Memoria principal utilizada

Un frame RGB565 de 800x800 requiere:

```text
800 * 800 * 2 bytes = 1,280,000 bytes
```

Por eso el frame principal se reserva en PSRAM con capacidades adecuadas para DMA.

---

## 3.2 OV5647

El sensor OV5647 entrega RAW8 por CSI en el modo seleccionado.

El driver utilizado por este proyecto expone varios modos. El modo seleccionado es:

```text
MIPI_2lane_24Minput_RAW8_800x800_50fps
```

Esto es importante porque el proyecto no utiliza un modo arbitrario de 1024x600. El sensor y el driver utilizado no ofrecen ese modo RAW8 en la configuración actual.

---

## 3.3 MIPI CSI

La cámara está conectada al periférico CSI del ESP32-P4.

El CSI recibe:

```text
RAW8
```

y hace passthrough del RAW8.

Una prueba anterior intentó hacer directamente:

```text
CSI RAW8 -> RGB565
```

pero esa combinación no era soportada en la configuración usada. Por eso la conversión de formato se realiza mediante el ISP.

---

## 3.4 ISP

El ISP realiza dos tareas fundamentales:

1. interpretar el patrón Bayer RAW8;
2. producir RGB565 mediante demosaicing.

La configuración utilizada actualmente es:

```c
.clk_hz = 80 * 1000 * 1000,
.input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
.input_data_color_type = ISP_COLOR_RAW8,
.output_data_color_type = ISP_COLOR_RGB565,
.h_res = CAMERA_WIDTH,
.v_res = CAMERA_HEIGHT,
.bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
```

El orden Bayer `GBRG` se eligió experimentalmente porque produjo una reproducción cromática coherente en las pruebas realizadas.

El demosaic utiliza:

```c
.grad_ratio.integer = 2,
.grad_ratio.decimal = 5,
```

---

## 3.5 PPA

El PPA del ESP32-P4 se utiliza actualmente en la etapa que convierte y reduce la imagen.

La conversión que sí está validada es:

```text
RGB565 800x800
       |
       v
GRAY8 400x400
```

El PPA también fue probado para:

```text
GRAY8 400x400 -> GRAY8 800x800
```

pero en el ESP32-P4 rev. 1.0 utilizado el motor SRM devuelve:

```text
unsupported color mode
ESP_ERR_INVALID_ARG
```

Por esta razón esa ruta no se considera parte funcional del flujo actual.

---

## 3.6 JPEG de hardware

El ESP32-P4 posee un periférico JPEG que se utiliza para evitar realizar la compresión JPEG completamente por CPU.

Eso es muy importante para mantener el sistema en tiempo real.

Conceptualmente:

```text
GRAY8 buffer
     |
     v
JPEG hardware
     |
     v
JPEG comprimido
```

---

## 3.7 Wi-Fi

El sistema utiliza un **ESP32-C6 como coprocesador Wi-Fi** mediante **ESP-Hosted sobre SDIO**.

El ESP32-P4 no ejecuta la radio Wi-Fi localmente. Desde el punto de vista de la aplicación, `wifi_sta_start()` inicializa la conexión y posteriormente el servidor HTTP utiliza la red disponible.

La comunicación P4-C6 utiliza SDIO.

---

# 4. Software

La aplicación está desarrollada con:

```text
ESP-IDF 6.0.2
```

El toolchain utilizado es el de RISC-V para ESP32-P4.

El proyecto está estructurado como una aplicación ESP-IDF tradicional.

---

# 5. Estructura de carpetas

La estructura conceptual actual es:

```text
Camera3_filtro/
│
├── CMakeLists.txt
├── sdkconfig
│
├── main/
│   │
│   ├── CMakeLists.txt
│   │
│   ├── mipi_isp_dsi_main.c
│   │
│   ├── http_server.h
│   ├── wifi_sta.h
│   ├── jpeg_encoder.h
│   ├── ppa_gray.h
│   ├── sobel_gray.h
│   ├── example_config.h
│   │
│   ├── src/
│   │   ├── http_server.c
│   │   ├── wifi_sta.c
│   │   ├── jpeg_encoder.c
│   │   ├── ppa_gray.c
│   │   └── sobel_gray.c
│   │
│   └── components/
│
└── managed_components/
    ├── espressif__esp_cam_sensor/
    ├── espressif__esp_sccb_intf/
    ├── espressif__esp_hosted/
    ├── espressif__esp_wifi_remote/
    └── ...
```

> Nota: el nombre del archivo principal contiene `mipi_isp_dsi_main.c`, pero dentro de la aplicación sigue siendo el archivo donde vive `app_main()`. “main” aquí se entiende por función y papel dentro de la aplicación, no necesariamente por el nombre del fichero.

El `CMakeLists.txt` del componente `main` referencia actualmente módulos equivalentes a:

```cmake
idf_component_register(
    SRCS
        "mipi_isp_dsi_main.c"
        "src/wifi_sta.c"
        "src/http_server.c"
        "src/jpeg_encoder.c"
        "src/sobel_gray.c"
        "src/ppa_gray.c"
    INCLUDE_DIRS
        "."
        "components"
    REQUIRES
        esp_mm
        esp_driver_isp
        esp_driver_cam
        esp_driver_i2c
        esp_driver_jpeg
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        esp_hosted
        esp_wifi_remote
        dsi_init
        sensor_init
        esp_driver_ppa
)
```

Los componentes `dsi_init` y `sensor_init` pertenecen al entorno de ejemplos/componentes usados por la plataforma. Aunque la pantalla DSI ya no es necesaria para el flujo actual, las dependencias siguen presentes porque el proyecto evolucionó a partir de una base que inicialmente utilizaba DSI.

---

# 6. Función de cada archivo

## `mipi_isp_dsi_main.c`

Es el orquestador principal.

Se encarga de:

- inicializar Wi-Fi;
- calcular tamaños de buffers;
- inicializar PPA;
- reservar buffers;
- inicializar el regulador LDO de MIPI;
- inicializar I2C/SCCB;
- detectar el OV5647;
- seleccionar el modo 800x800 RAW8;
- configurar exposición y ganancia manuales;
- reservar el frame buffer;
- configurar ISP;
- configurar el CSI;
- configurar callbacks de cámara;
- crear el semáforo de frame;
- arrancar el sensor;
- arrancar la captura continua;
- inicializar el servidor HTTP.

También contiene las funciones que generan los JPEG que consume `http_server.c`.

---

## `ppa_gray.c / ppa_gray.h`

Contiene la abstracción del PPA para el proyecto.

Actualmente hay dos ideas conceptualmente distintas:

### Conversión RGB565 -> GRAY8

```text
RGB565 800x800
       |
       v
PPA
       |
       v
GRAY8 400x400
```

### Escalado GRAY8

Existe una función de escalado 2x:

```text
GRAY8 400x400 -> GRAY8 800x800
```

pero no se debe considerar funcional en el ESP32-P4 rev. 1.0 actual porque el SRM rechaza el modo de color en esa operación.

Esto queda documentado en el código para evitar que alguien vuelva a activar accidentalmente esa ruta en el flujo en tiempo real.

---

## `sobel_gray.c / sobel_gray.h`

Es el módulo de procesamiento digital de imagen implementado por CPU.

Contiene dos conceptos principales:

1. Gaussian 3x3.
2. Sobel.

---

## `jpeg_encoder.c / jpeg_encoder.h`

Abstrae el periférico JPEG de hardware.

El resto de la aplicación no necesita conocer los detalles del driver JPEG. Solo necesita llamar a una función del tipo:

```c
encode_gray_to_jpeg(...)
```

Esto simplifica mucho el servidor HTTP y permite reutilizar el codificador para la grabación futura en SD.

---

## `http_server.c / http_server.h`

Contiene el servidor HTTP y los endpoints.

Conceptualmente expone:

```text
/                    página web
/api/status          estado del sistema
/stream              MJPEG del Sobel
/compare             MJPEG original | Sobel
/snapshot            endpoint existente pero no utilizable en el flujo actual
```

El frontend actual utiliza `/compare` para mostrar la comparación en tiempo real.

---

## `wifi_sta.c / wifi_sta.h`

Abstrae la inicialización de la conexión Wi-Fi.

La función principal utilizada por `app_main()` es:

```c
wifi_sta_start("SSID", "PASSWORD");
```

Las credenciales deben mantenerse fuera del repositorio público o reemplazarse por mecanismos de configuración seguros.

---

# 7. Flujo de arranque de `app_main()`

La función `app_main()` es deliberadamente secuencial. Esta sección explica el orden en que se levantan los periféricos.

## Paso 0 — Wi-Fi

Primero se inicia la red:

```c
ESP_ERROR_CHECK(
    wifi_sta_start(
        "TU_SSID",
        "TU_PASSWORD"
    )
);
```

La aplicación espera a que la función termine de configurar la estación y obtenga conectividad.

---

## Paso 1 — Tamaño del frame

Se calcula:

```c
s_frame_buffer_size =
    CAMERA_WIDTH *
    CAMERA_HEIGHT *
    CAMERA_BPP;
```

Con los valores actuales:

```text
800 * 800 * 2 = 1,280,000 bytes
```

---

## Paso 2 — Inicialización del PPA

Se llama a:

```c
init_ppa_gray(
    CAMERA_WIDTH,
    CAMERA_HEIGHT,
    OUT_WIDTH,
    OUT_HEIGHT
);
```

Valores actuales:

```text
input  = 800x800
output = 400x400
```

---

## Paso 3 — Cache line

Se consulta el tamaño de línea de caché externo:

```c
uint32_t cache_line =
    cache_hal_get_cache_line_size(
        CACHE_LL_LEVEL_EXT_MEM,
        CACHE_TYPE_DATA
    );
```

Esto es relevante porque algunos buffers se utilizan con DMA/periféricos y deben estar correctamente alineados.

---

# 8. Buffers de memoria

La aplicación actual necesita varios buffers.

## 8.1 `s_frame_buffer`

Es el buffer principal de captura.

```text
800x800 RGB565
= 1,280,000 bytes
```

Es el buffer que recibe la transacción CSI.

Se coloca en PSRAM y se marca con capacidades adecuadas para DMA.

---

## 8.2 `s_gaussian_buffer`

```text
400x400 GRAY8
= 160,000 bytes
```

Aquí se almacena la salida del filtro Gaussian.

---

## 8.3 `s_sobel_buffer`

```text
400x400 GRAY8
= 160,000 bytes
```

Aquí se almacena la salida del Sobel.

---

## 8.4 `s_upscaled_buffer`

```text
800x800 GRAY8
= 640,000 bytes
```

Actualmente existe porque fue necesario para la ruta `/snapshot` y para la variante de streaming que utiliza un resultado final 800x800.

Sin embargo, debido a la limitación del PPA `GRAY8` en el P4 rev. 1.0, esta ruta de escalado no debe considerarse parte del camino estable del modo `/compare` actual.

---

## 8.5 `s_compare_buffer`

```text
800x400 GRAY8
= 320,000 bytes
```

Este buffer concatena horizontalmente dos imágenes:

```text
+----------------------+----------------------+
|                      |                      |
|      ORIGINAL        |        SOBEL         |
|       400x400        |        400x400       |
|                      |                      |
+----------------------+----------------------+
```

El resultado final es:

```text
800x400
```

---

# 9. Inicialización del LDO MIPI

El código reserva un canal LDO para la parte física MIPI:

```c
esp_ldo_channel_config_t ldo_cfg = {
    .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
    .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
};
```

En la configuración actual:

```text
canal = 3
voltaje = 2500 mV
```

Este valor depende de la placa y de la alimentación física utilizada por la interfaz MIPI.

---

# 10. SCCB / I2C del OV5647

El OV5647 se configura por SCCB, implementado sobre la interfaz I2C del ESP32-P4.

Se crea un bus I2C:

```c
i2c_master_bus_config_t i2c_bus_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,
    .scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
```

Después se crea el dispositivo SCCB:

```c
sccb_i2c_config_t sccb_cfg = {
    .scl_speed_hz = 100000,
    .device_address = OV5647_SCCB_ADDR,
    .addr_bits_width = 7,
};
```

La dirección utilizada es:

```text
0x36
```

---

# 11. Detección del OV5647

El sensor se detecta mediante:

```c
esp_cam_sensor_config_t sensor_cfg = {
    .sccb_handle = sccb_handle,
    .reset_pin = GPIO_NUM_NC,
    .pwdn_pin = GPIO_NUM_NC,
    .xclk_pin = GPIO_NUM_NC,
    .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
};
```

Luego:

```c
esp_cam_sensor_device_t *sensor_dev =
    ov5647_detect(&sensor_cfg);
```

Si no se detecta el sensor, la aplicación termina la inicialización.

---

# 12. Selección del formato de cámara

Se consulta la lista de formatos soportados por el sensor:

```c
esp_cam_sensor_query_format(
    sensor_dev,
    &format_array
);
```

Después se recorre la lista buscando:

```text
width  == 800
height == 800
format == RAW8
```

Cuando se encuentra:

```c
chosen = f;
```

y posteriormente:

```c
esp_cam_sensor_set_format(
    sensor_dev,
    chosen
);
```

El log esperado es similar a:

```text
Formato elegido:
MIPI_2lane_24Minput_RAW8_800x800_50fps
```

---

# 13. Exposición manual

El código desactiva el control automático de exposición del sensor y escribe directamente los registros relevantes.

Se utilizan los registros:

```text
0x3503  AEC manual
0x3500  exposure high
0x3501  exposure middle
0x3502  exposure low
0x350A  gain high
0x350B  gain low
0x380E  VTS high
0x380F  VTS low
```

La configuración actual utilizada durante las pruebas es:

```c
ov5647_set_exposure_gain(
    sccb_handle,
    800,
    512
);
```

Por tanto:

```text
exposure = 800 líneas
gain     = 512
```

La función además lee VTS y puede elevarlo si el tiempo de exposición requerido lo necesita.

---

# 14. ¿Por qué se usa exposición manual?

Para procesamiento de bordes, un cambio fuerte en la exposición entre frames puede cambiar radicalmente la imagen Sobel aunque el escenario físico sea el mismo.

Por eso, para experimentación reproducible con visión, fijar exposición y ganancia puede ser más conveniente que dejar al sensor reaccionar continuamente a la iluminación.

La contrapartida es que la imagen puede quedar subexpuesta o sobreexpuesta cuando cambia mucho la escena.

---

# 15. Frame buffer de CSI

El buffer principal se reserva con:

```c
heap_caps_aligned_calloc(
    cache_line,
    1,
    s_frame_buffer_size,
    MALLOC_CAP_SPIRAM |
    MALLOC_CAP_DMA
);
```

Esto permite que el periférico de cámara pueda utilizar la memoria externa de forma compatible con DMA.

---

# 16. Configuración ISP

La configuración central es:

```c
esp_isp_processor_cfg_t isp_config = {
    .clk_hz = 80 * 1000 * 1000,
    .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
    .input_data_color_type = ISP_COLOR_RAW8,
    .output_data_color_type = ISP_COLOR_RGB565,
    .has_line_start_packet = false,
    .has_line_end_packet = false,
    .h_res = CAMERA_WIDTH,
    .v_res = CAMERA_HEIGHT,
    .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
};
```

Esto significa:

```text
Entrada : RAW8
Origen  : CSI
Bayer   : GBRG
Salida  : RGB565
Tamaño  : 800x800
```

---

# 17. Demosaic

Se configura:

```c
esp_isp_demosaic_config_t demosaic_cfg = {
    .grad_ratio.integer = 2,
    .grad_ratio.decimal = 5,
};
```

y se habilita con:

```c
esp_isp_demosaic_enable(isp_proc);
```

El demosaic permite convertir la matriz Bayer en una imagen RGB válida.

---

# 18. Inicialización JPEG

Se inicializa el encoder con la resolución principal:

```c
init_jpeg_encoder(
    CAMERA_WIDTH,
    CAMERA_HEIGHT
);
```

El encoder de hardware mantiene la compresión fuera del núcleo principal de procesamiento de píxeles.

---

# 19. Configuración CSI

El CSI recibe RAW8:

```c
esp_cam_ctlr_csi_config_t csi_config = {
    .ctlr_id = 0,
    .h_res = CAMERA_WIDTH,
    .v_res = CAMERA_HEIGHT,
    .lane_bit_rate_mbps =
        (uint32_t)(chosen->mipi_info.mipi_clk / 1000000u),
    .input_data_color_type = CAM_CTLR_COLOR_RAW8,
    .output_data_color_type = CAM_CTLR_COLOR_RAW8,
    .data_lane_num = chosen->mipi_info.lane_num,
    .byte_swap_en = false,
    .queue_items = 1,
};
```

Un detalle importante es:

```c
.input_data_color_type  = RAW8
.output_data_color_type = RAW8
```

El CSI no convierte RAW8 a RGB565.

Eso se hace después en el ISP.

---

# 20. ¿Por qué `queue_items = 1`?

La implementación actual usa un solo buffer de cámara.

La captura está deliberadamente simplificada:

```text
CSI
 |
 v
un único frame buffer
 |
 v
señal de “frame terminado”
```

Cuando el frame termina, el callback libera un semáforo.

Esto es suficiente para el flujo experimental actual, pero tiene una consecuencia importante:

> No se debe asumir que el sistema actual soporta libremente múltiples consumidores independientes del frame al mismo tiempo.

Por ejemplo, abrir simultáneamente varias rutas HTTP que intenten procesar el mismo frame no constituye una arquitectura de buffering segura.

Para grabación SD simultánea, esta parte debería evolucionar a un esquema de ownership de buffers o una pequeña cola/ring buffer.

---

# 21. Callback `s_camera_get_new_vb`

Este callback devuelve al controlador la dirección del buffer de captura:

```c
trans->buffer = ctx->trans->buffer;
trans->buflen = ctx->trans->buflen;
```

La implementación devuelve `false`, por lo que no solicita comportamiento adicional del driver.

---

# 22. Callback `s_camera_get_finished_trans`

Cuando el frame termina:

```c
xSemaphoreGiveFromISR(
    s_frame_ready_sem,
    &woken
);
```

Eso permite que las funciones de procesamiento esperen a que haya un frame completo.

La sincronización es:

```text
ISR / callback
      |
      | give
      v
semaphore
      |
      | take
      v
procesamiento CPU
```

---

# 23. Semáforo de frame

Se crea como semáforo binario:

```c
s_frame_ready_sem =
    xSemaphoreCreateBinary();
```

Las funciones que generan JPEG esperan:

```c
xSemaphoreTake(
    s_frame_ready_sem,
    pdMS_TO_TICKS(1000)
);
```

Si no llega ningún frame durante un segundo, la función devuelve `ESP_ERR_TIMEOUT`.

---

# 24. PPA: RGB565 -> GRAY8

Esta es una de las optimizaciones de hardware más importantes del proyecto.

En lugar de hacer todo el recorrido RGB pixel por pixel en C sobre 800x800, el PPA se usa para transformar la imagen.

La cadena es:

```text
RGB565 800x800
      |
      | PPA
      v
GRAY8 400x400
```

La salida es la base para el procesamiento Sobel.

La memoria grayscale contiene un byte por píxel:

```text
400 * 400 * 1 = 160,000 bytes
```

---

# 25. Cache coherency

Una parte que puede pasar desapercibida pero es crítica en este sistema es la coherencia de caché.

Cuando la CPU escribe un buffer y luego un periférico DMA lo lee, el periférico debe poder observar los datos actualizados.

Por eso aparecen llamadas como:

```c
esp_cache_msync(
    buffer,
    size,
    ESP_CACHE_MSYNC_FLAG_DIR_C2M
);
```

Y cuando el periférico produce/modifica memoria que luego leerá la CPU se utiliza el sentido contrario cuando corresponde.

Este concepto es importante al modificar el proyecto.

No basta con preguntar “¿el buffer está lleno?”; hay que preguntar también:

```text
¿quién escribió el buffer?
¿quién lo va a leer después?
¿hay DMA de por medio?
¿la caché puede contener una copia antigua?
```

---

# 26. Conversión a grayscale

El concepto de grayscale utiliza la luminancia de los canales RGB.

La implementación CPU utilizada en el proyecto aplica una combinación equivalente a:

```c
gray = (R * 77 + G * 150 + B * 29) >> 8;
```

Eso evita utilizar números flotantes y produce una operación entera rápida.

Los pesos aproximan:

```text
R ≈ 0.30
G ≈ 0.59
B ≈ 0.11
```

---

# 27. Gaussian 3x3

El filtro Gaussian previo al Sobel se utiliza para reducir ruido de alta frecuencia.

La máscara implementada es:

```text
1 2 1
2 4 2
1 2 1
```

con normalización:

```text
suma / 16
```

Por tanto, para cada píxel:

```text
P = (p00
   +2*p01 +p02
   +2*p10 +4*p11 +2*p12
   +p20 +2*p21 +p22) / 16
```

La implementación utiliza aritmética entera y redondeo:

```c
dst[y * width + x] =
    (sum + 8) >> 4;
```

El `+8` realiza un redondeo antes de dividir por 16 mediante el desplazamiento.

---

# 28. Bordes de Gaussian

En los bordes de la imagen no existen todos los vecinos.

La implementación actual resuelve esto reutilizando la coordenada límite.

Por ejemplo:

```c
xm1 = (x > 0) ? x - 1 : x;
xp1 = (x < width - 1) ? x + 1 : x;
```

Esto equivale a una extensión tipo “clamp”.

---

# 29. Sobel

El Sobel calcula la variación local de intensidad.

Las máscaras conceptuales son:

```text
Gx:
-1   0  +1
-2   0  +2
-1   0  +1
```

```text
Gy:
-1  -2  -1
 0   0   0
+1  +2  +1
```

En la implementación se calcula:

```c
gx = (p02 + 2*p12 + p22)
   - (p00 + 2*p10 + p20);
```

```c
gy = (p20 + 2*p21 + p22)
   - (p00 + 2*p01 + p02);
```

La magnitud se aproxima con:

```c
mag = abs(gx) + abs(gy);
```

en lugar de:

```text
sqrt(gx^2 + gy^2)
```

Esto es más barato computacionalmente y suficiente para detección visual de bordes.

---

# 30. `SOBEL_THRESHOLD`

El proyecto utiliza actualmente:

```c
#define SOBEL_THRESHOLD -1
```

El significado es especial:

```text
threshold < 0
    -> se conserva la magnitud del gradiente
```

Por tanto la imagen Sobel es continua en escala de grises.

Si se utiliza:

```c
#define SOBEL_THRESHOLD 100
```

la salida se convierte en binaria:

```text
mag >= 100 -> 255
mag < 100  -> 0
```

Eso será útil más adelante si se implementa reconocimiento geométrico.

---

# 31. ¿Por qué una zona muy iluminada puede verse negra en Sobel?

El Sobel no mide “qué tan brillante es la imagen”.

Mide “qué tan rápido cambia la imagen en el espacio”.

Por eso una región:

```text
██████████████████
██████████████████
██████████████████
```

puede tener un Sobel muy pequeño si es uniforme.

En cambio una transición:

```text
████████░░░░░░░░░░
████████░░░░░░░░░░
████████░░░░░░░░░░
```

produce un borde fuerte.

Esto es exactamente el comportamiento deseado de Sobel.

---

# 32. Ruta `/compare`

La ruta `/compare` se diseñó para que el usuario pueda comparar visualmente el origen y el procesamiento sobre **el mismo frame lógico**.

La función:

```c
obtener_frame_compare_jpeg()
```

hace:

```text
1. esperar frame
2. RGB565 -> GRAY
3. Gaussian
4. Sobel
5. copiar GRAY en mitad izquierda
6. copiar SOBEL en mitad derecha
7. limpiar/escribir caché para DMA
8. JPEG
```

El buffer se organiza como:

```text
800x400

0                       399 400                    799
+-------------------------+--------------------------+
|                         |                          |
|        GRAY              |         SOBEL            |
|        400x400           |         400x400          |
|                         |                          |
+-------------------------+--------------------------+
```

---

# 33. Ventaja de `/compare`

En lugar de tener dos clientes HTTP peleando por un único buffer de cámara, `/compare` produce una sola imagen JPEG que contiene ambos resultados.

Eso simplifica mucho el sistema.

No se necesita:

```text
cliente A -> cámara
cliente B -> cámara
```

sino:

```text
cámara -> procesamiento -> una imagen compare -> cliente
```

---

# 34. Servidor HTTP

El servidor HTTP recibe una petición y utiliza funciones de procesamiento del `main` para producir una imagen JPEG.

El módulo HTTP no conoce los detalles de ISP, PPA o Sobel.

La separación conceptual es:

```text
HTTP
 |
 +-- pide JPEG
 |
 v
mipi_isp_dsi_main.c
 |
 +-- captura
 +-- procesa
 +-- JPEG
 |
 v
HTTP envía bytes
```

Esto es una buena separación de responsabilidades.

---

# 35. MJPEG

La ruta `/stream` no produce un archivo de vídeo comprimido H.264.

Produce una secuencia HTTP multipart:

```text
JPEG frame 1
JPEG frame 2
JPEG frame 3
JPEG frame 4
...
```

El navegador interpreta esto como vídeo continuo.

Conceptualmente:

```text
HTTP response
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg

[JPEG 1]

--frame
Content-Type: image/jpeg

[JPEG 2]

--frame
Content-Type: image/jpeg
...
```

Cada frame es independiente.

Esta propiedad es precisamente la que hace interesante la futura grabación en SD.

---

# 36. ¿Se puede agregar una tarjeta SD?

Sí.

De hecho, para este sistema la SD es una evolución natural porque la salida ya es una secuencia de JPEG individuales.

ESP-IDF dispone de soporte de FATFS y de montaje de tarjetas mediante `esp_vfs_fat_sdmmc_mount()` y `esp_vfs_fat_sdspi_mount()`. El ESP32-P4 dispone además de un periférico SDMMC con dos slots; la documentación del P4 indica que el slot 1 se puede enrutar mediante GPIO Matrix y está pensado para uso SD no-UHS. La elección exacta de slot y pines debe hacerse según el esquemático de la placa concreta. [Documentación oficial de FatFs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html) · [Documentación oficial SDMMC para ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32p4/api-reference/peripherals/sdmmc_host.html)

---

# 37. ¿Cómo conviene grabar el vídeo?

La opción que mejor encaja con la arquitectura actual es **MJPEG sobre SD**.

No conviene que el ESP32-P4 vuelva a comprimir una imagen diferente para la SD si ya acaba de generar un JPEG para el stream.

La idea es:

```text
Frame de cámara
      |
      v
Gaussian
      |
      v
Sobel
      |
      v
JPEG hardware
      |
      +--------------------+
      |                    |
      v                    v
   Wi-Fi /stream        SD card
```

El mismo JPEG puede enviarse por Wi-Fi y escribirse en la SD.

Esto evita:

```text
procesamiento adicional de píxeles
segunda compresión JPEG
```

---

# 38. ¿Qué archivo guardar?

Hay tres posibilidades.

## Opción A — guardar JPEG individuales

Ejemplo:

```text
/frames/
    frame_000001.jpg
    frame_000002.jpg
    frame_000003.jpg
    ...
```

Ventajas:

- implementación muy sencilla;
- excelente para debugging;
- fácil inspección individual.

Desventajas:

- muchos archivos;
- gestión de directorio más complicada;
- menos cómodo para reproducir como vídeo.

---

## Opción B — guardar MJPEG en un contenedor AVI

Esta es la opción más interesante para el proyecto.

Conceptualmente:

```text
video.avi
   |
   +-- header
   +-- JPEG frame 1
   +-- JPEG frame 2
   +-- JPEG frame 3
   +-- ...
   +-- index
```

No se vuelve a comprimir el frame.

El JPEG que ya produce el encoder se inserta en el contenedor.

Para un prototipo de cámara embebida es una solución muy atractiva porque simplifica muchísimo el pipeline.

---

## Opción C — H.264/H.265/MP4

Esto ya es otra arquitectura.

No debería asumirse que “grabar a MP4” es simplemente escribir los JPEG actuales a un archivo. MP4 y los codecs inter-frame requieren una cadena de codificación/containerización diferente.

Para la primera versión de grabación, **MJPEG/AVI es mucho más coherente con el sistema actual**.

---

# 39. El problema real: escritura SD en tiempo real

Agregar la SD no es solo:

```c
fwrite(jpeg, 1, jpeg_size, file);
```

El problema es que el sistema actual trabaja con un único buffer de cámara y un único flujo de procesamiento.

Una escritura SD puede tardar de forma variable.

Si durante una escritura la CPU o el bus se bloquean demasiado, se puede producir:

```text
frame capturado
    ↓
procesado
    ↓
JPEG listo
    ↓
SD tarda demasiado
    ↓
frame siguiente llega
    ↓
colisión / pérdida
```

Por eso la grabación debe separarse del camino crítico.

---

# 40. Arquitectura recomendada para la grabación

La evolución recomendada sería:

```text
                  CAPTURA
                     |
                     v
              FRAME BUFFER / QUEUE
                     |
                     v
               PROCESAMIENTO
                     |
                     v
                JPEG HW
                     |
          +----------+----------+
          |                     |
          v                     v
       STREAM               RECORD TASK
          |                     |
          v                     v
        Wi-Fi                  SD
```

La idea es que el task de grabación no bloquee el camino de procesamiento.

---

# 41. Ring buffer / cola de JPEG

Una solución razonable es tener una cola de buffers JPEG.

Por ejemplo:

```text
JPEG buffer A
JPEG buffer B
JPEG buffer C
JPEG buffer D
```

El procesamiento escribe en uno mientras SD escribe otro.

Conceptualmente:

```text
            PRODUCER
                |
                v
       +------------------+
       | JPEG queue       |
       |                  |
       | A B C D          |
       +------------------+
          |          |
          |          |
          v          v
       STREAM       SD TASK
```

Con esto se consigue desacoplar:

```text
captura/procesamiento
```

de:

```text
latencia de almacenamiento
```

---

# 42. Grabación de una duración determinada

La interfaz más simple sería algo como:

```text
/record/start?seconds=10
/record/stop
```

Y se podría crear:

```text
/video_0001.avi
```

Cuando empieza una grabación:

```text
recording = true
start_time = now
```

Para cada frame JPEG:

```text
si recording:
    escribir JPEG en AVI
```

Y cuando:

```text
now - start_time >= duration
```

se cierra el archivo.

---

# 43. ¿A cuántos FPS grabar?

No conviene asumir que el sensor de 50 fps significa que el sistema completo puede almacenar 50 fps de forma estable en SD.

Hay tres velocidades distintas:

```text
sensor fps
processing fps
recording fps
```

El sistema actual puede recibir el sensor a 50 fps, pero eso no significa automáticamente que:

```text
50 JPEG/s
```

puedan procesarse, transmitirse y escribirse simultáneamente sin pérdidas.

Para una primera grabación experimental, es razonable probar:

```text
5 fps
10 fps
15 fps
20 fps
```

y medir.

---

# 44. Estimación de espacio en SD

La capacidad necesaria depende sobre todo de:

- tamaño JPEG promedio;
- FPS;
- duración.

La fórmula es:

```text
bytes/s = promedio_JPEG_bytes * FPS
```

Entonces:

```text
MB/min ≈ promedio_JPEG_bytes * FPS * 60 / 1,000,000
```

Ejemplo puramente ilustrativo:

```text
JPEG promedio = 40 KB
FPS = 10

40,000 * 10 = 400,000 bytes/s

≈ 24 MB/min
```

Otro ejemplo:

```text
JPEG promedio = 80 KB
FPS = 15

80,000 * 15 = 1,200,000 bytes/s

≈ 72 MB/min
```

Por eso una microSD de 16 GB es enorme en comparación con un experimento de varios minutos, aunque el ancho de escritura sostenido sigue siendo una variable que debe medirse.

---

# 45. Qué microSD utilizar

Para la etapa experimental conviene utilizar una tarjeta:

- original;
- de fabricante conocido;
- con capacidad moderada;
- con buena velocidad de escritura sostenida.

No es necesario buscar una tarjeta enorme solamente por capacidad.

Más importante que el número “MB/s” de marketing es que la escritura sostenida del modelo concreto sea suficientemente estable para el flujo de vídeo.

---

# 46. SDMMC vs SDSPI

El sistema puede acceder a una SD de dos maneras principales.

## SDMMC

```text
ESP32-P4 SDMMC
    |
    +-- CLK
    +-- CMD
    +-- D0
    +-- D1
    +-- D2
    +-- D3
    |
    v
   SD card
```

Ventajas:

- mayor throughput potencial;
- interfaz dedicada;
- mejor opción para streaming de datos sostenido si la placa está preparada.

---

## SDSPI

```text
ESP32-P4 SPI
    |
    +-- SCK
    +-- MOSI
    +-- MISO
    +-- CS
    |
    v
   SD card
```

Ventajas:

- selección de pines más flexible;
- útil si la placa no expone correctamente el bus SDMMC.

La documentación de Espressif señala que SDSPI ofrece menor throughput pero mayor flexibilidad de selección de pines, mientras que SDMMC puede proporcionar mayor throughput usando sus señales dedicadas. ([SD SPI Host](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdspi_host.html))

---

# 47. Alimentación de la SD

La interfaz SD necesita 3.3 V adecuados.

No se debe asumir que un adaptador cualquiera de microSD es eléctricamente equivalente a una interfaz SD nativa.

Algunos módulos baratos incluyen reguladores y conversores de nivel; otros están diseñados específicamente para señales SPI y no son ideales para las velocidades de una interfaz SDMMC rápida.

Antes de seleccionar el hardware final hay que comprobar el esquemático de la placa y el módulo SD.

---

# 48. Estructura futura recomendada para la SD

Cuando se implemente, una separación limpia sería:

```text
main/
│
├── sd_recorder.h
├── src/
│   └── sd_recorder.c
```

`sd_recorder.c` debería encargarse solamente de:

```text
mount
open
write
flush
close
unmount
```

El procesamiento de imagen no debería saber nada de FATFS.

---

# 49. API futura del grabador

Una interfaz posible sería:

```c
essp_err_t sd_recorder_init(void);
```

```c
essp_err_t sd_recorder_start(
    const char *filename,
    uint32_t fps
);
```

```c
essp_err_t sd_recorder_write_jpeg(
    const uint8_t *jpeg,
    size_t jpeg_size,
    uint32_t timestamp_ms
);
```

```c
bool sd_recorder_is_running(void);
```

```c
esp_err_t sd_recorder_stop(void);
```

La implementación real podría tener además un task interno y una cola de buffers.

---

# 50. Flujo futuro completo con grabación

La arquitectura final podría quedar:

```text
OV5647
 |
 v
CSI RAW8
 |
 v
ISP RGB565
 |
 v
PPA RGB565 -> GRAY8 400x400
 |
 v
Gaussian
 |
 v
Sobel
 |
 v
JPEG HW
 |
 +--------------------+
 |                    |
 v                    v
HTTP MJPEG         SD Recorder
 |                    |
 v                    v
 navegador          AVI/MJPEG
```

Esto tiene una propiedad muy importante:

> La SD no obliga a repetir todo el procesamiento de imagen.

---

# 51. Página web

La página principal se construye como HTML embebido en una cadena C.

El concepto actual es:

```html
<img src="/compare">
```

El navegador recibe el MJPEG y actualiza la imagen automáticamente.

La interfaz muestra:

```text
+----------------------------------------------+
|                  ORIGINAL | SOBEL            |
|                                              |
|                  vídeo                       |
|                                              |
+----------------------------------------------+
```

---

# 52. Endpoint `/api/status`

El endpoint de estado es útil para depurar el sistema sin abrir el stream.

Es una ruta JSON.

En el futuro puede ampliarse para incluir:

```json
{
  "camera": "OV5647",
  "width": 800,
  "height": 800,
  "fps": 10,
  "recording": true,
  "sd_free": 123456789,
  "jpeg_avg": 42000
}
```

Esto todavía es una propuesta; no se debe asumir que todos esos campos existen en la implementación actual.

---

# 53. Endpoint `/snapshot`

Existe una ruta `/snapshot` en la arquitectura HTTP.

Sin embargo, actualmente no debe considerarse una función estable del sistema.

La causa no es una falla del JPEG ni del Wi-Fi. La cadena llega correctamente hasta:

```text
Sobel 400x400
```

y falla cuando intenta ejecutar:

```text
PPA GRAY8 400x400 -> GRAY8 800x800
```

Por decisión de diseño, el sistema actual prioriza el streaming `/compare` y no utiliza esta ruta para la operación normal.

---

# 54. Por qué no se arregló `/snapshot` simplemente usando CPU

Es técnicamente posible duplicar cada píxel:

```text
400x400 -> 800x800
```

mediante CPU.

Pero eso añade otra operación sobre 160,000 píxeles que debe realizarse y mantenerse en el camino del sistema.

En este proyecto la prioridad actual es preservar la fluidez del vídeo en vivo.

Por eso se decidió no introducir ese coste solo para mantener un endpoint secundario.

---

# 55. Flujo actual de `/compare`

La función `obtener_frame_compare_jpeg()` es aproximadamente:

```c
wait_frame();

rgb565_to_gray(...);

gaussian3x3_gray8(...);
sobel_gray8_threshold(...);

for (y = 0; y < 400; y++) {
    copy_gray_to_left_half();
    copy_sobel_to_right_half();
}

esp_cache_msync(..., DIR_C2M);

encode_gray_to_jpeg(
    compare_buffer,
    800,
    400,
    ...
);
```

El resultado es un JPEG:

```text
800x400
```

---

# 56. Qué hace exactamente `rgb565_to_gray()`

Esta función pertenece al módulo de PPA/procesamiento.

Recibe el frame que proviene del ISP:

```text
RGB565 800x800
```

y produce una vista grayscale adecuada para el filtro.

El dato ya no contiene la información de color necesaria para la aplicación Sobel.

Desde ese punto de vista, el pipeline queda especializado en geometría/contraste y ya no en color.

---

# 57. Por qué el Sobel se ejecuta sobre 400x400

El sensor entrega 800x800, pero el Sobel se ejecuta a 400x400.

La razón es que un filtro convolucional CPU sobre:

```text
800x800 = 640,000 píxeles
```

es aproximadamente cuatro veces más costoso que sobre:

```text
400x400 = 160,000 píxeles
```

si se compara el número de posiciones de píxel.

Reducir antes del Sobel permite mantener un mejor rendimiento.

---

# 58. Por qué la imagen visual puede seguir verse bien después de reducir

El resultado final se destina principalmente a visualización de bordes, no a reconstrucción fotográfica de máxima fidelidad.

Para detectar estructuras relativamente grandes, 400x400 puede ser suficiente.

El detalle más pequeño sí se pierde, por lo que esta decisión tiene que revisarse si posteriormente se pretende detectar objetos muy pequeños.

---

# 59. Qué ocurre si se aumenta `OUT_WIDTH` y `OUT_HEIGHT`

Por ejemplo:

```c
#define OUT_WIDTH 600
#define OUT_HEIGHT 600
```

el procesamiento CPU crecerá considerablemente.

Eso aumenta:

- número de píxeles Gaussian;
- número de píxeles Sobel;
- memoria utilizada;
- tráfico de memoria;
- tiempo de procesamiento.

Durante las pruebas anteriores se observó que subir la resolución de procesamiento mejoraba el detalle pero reducía apreciablemente los FPS.

Por eso 400x400 es actualmente un compromiso de rendimiento/calidad.

---

# 60. `JPEG_QUALITY`

La configuración actual es:

```c
#define JPEG_QUALITY 80
```

Aumentar calidad generalmente aumenta tamaño de JPEG.

Eso impacta directamente:

```text
más bytes por frame
     |
     +--> más Wi-Fi
     |
     +--> más SD
     |
     +--> posiblemente menos FPS
```

Por eso, cuando se implemente grabación, la calidad JPEG y el FPS deben tratarse como parámetros ligados al ancho de banda de almacenamiento.

---

# 61. `SOBEL_THRESHOLD`

Valores recomendados para experimentación:

```text
-1    -> gradiente continuo
30    -> borde muy sensible
60    -> sensibilidad media
100   -> bordes más fuertes
150   -> bordes muy marcados
200+  -> solo cambios muy intensos
```

No existe un valor universal.

Debe elegirse experimentalmente según:

- iluminación;
- exposición;
- textura del terreno;
- cámara;
- distancia;
- objetivo geométrico.

---

# 62. Gaussian vs Sobel

El orden actual es:

```text
GRAY
 |
 v
Gaussian
 |
 v
Sobel
```

No conviene intercambiarlos.

Gaussian elimina ruido de alta frecuencia antes de que Sobel lo interprete como borde.

Sin Gaussian:

```text
ruido
 |
 v
Sobel
 |
 v
muchos bordes pequeños
```

Con Gaussian:

```text
ruido
 |
 v
Gaussian
 |
 v
ruido reducido
 |
 v
Sobel
 |
 v
bordes más limpios
```

---

# 63. Próximo paso lógico: detección de zanja

Una vez estabilizado el pipeline básico, la siguiente etapa puede ser:

```text
GRAY
 |
Gaussian
 |
Sobel
 |
Threshold
 |
ROI
 |
Detección de líneas/contornos
 |
Clasificación geométrica
 |
“posible zanja”
```

La detección de zanja no requiere necesariamente inteligencia artificial.

Puede resolverse mediante geometría.

Por ejemplo, buscar dos bordes aproximadamente paralelos con determinadas relaciones de distancia y orientación.

---

# 64. Posible algoritmo clásico para zanja

Una estrategia futura:

### Paso A
Aplicar Gaussian.

### Paso B
Aplicar Sobel.

### Paso C
Binarizar:

```text
threshold = 80..150
```

### Paso D
Restringir una ROI.

Por ejemplo, la parte central/inferior de la imagen donde se espera la zanja.

### Paso E
Buscar líneas.

Se podría utilizar una variante de Hough o una aproximación propia basada en acumulación de pendientes.

### Paso F
Comparar líneas.

Buscar pares:

```text
línea 1
línea 2
```

con:

- orientación parecida;
- separación compatible;
- continuidad espacial.

---

# 65. Por qué la arquitectura actual es buena base para eso

Porque el sistema ya separa:

```text
captura
procesamiento de imagen
codificación
red
```

Por ejemplo, un futuro módulo podría ser:

```text
src/trench_detector.c
src/trench_detector.h
```

y consumir directamente:

```c
s_sobel_buffer
```

sin modificar el driver de cámara.

---

# 66. Recomendación de diseño para el futuro

A medida que aumenten las funciones conviene mantener la separación:

```text
camera/
processing/
compression/
network/
storage/
application/
```

Una evolución futura podría ser:

```text
main/
├── app_main.c
├── camera_pipeline.c
├── image_processing.c
├── jpeg_encoder.c
├── http_server.c
├── wifi_sta.c
└── sd_recorder.c
```

El archivo actual todavía concentra demasiada lógica porque se desarrolló incrementalmente y eso fue útil para depurar el hardware.

---

# 67. Por qué no conviene separar demasiado pronto

En sistemas embebidos con hardware experimental, primero conviene demostrar:

```text
sensor funcionando
-> CSI funcionando
-> ISP funcionando
-> PPA funcionando
-> procesamiento funcionando
-> JPEG funcionando
-> Wi-Fi funcionando
```

Después se refactoriza.

Separar prematuramente puede ocultar qué periférico está fallando.

Por eso el `main` actual sigue siendo relativamente grande.

---

# 68. Consumo de memoria aproximado

Buffers principales:

```text
Frame RGB565      1,280,000 bytes
Gaussian            160,000 bytes
Sobel               160,000 bytes
Upscaled            640,000 bytes
Compare             320,000 bytes
------------------------------------------------
Total aproximado  2,560,000 bytes
```

Esto es alrededor de:

```text
2.56 MB
```

de buffers explícitos.

Gracias a los 32 MB de PSRAM, existe mucho margen para ampliar el sistema.

La disponibilidad real depende de qué más reserve ESP-IDF y de cómo estén configurados los componentes.

---

# 69. Por qué PSRAM es importante

Sin PSRAM sería mucho más difícil reservar simultáneamente:

```text
1.28 MB frame
+ 0.16 MB Gaussian
+ 0.16 MB Sobel
+ 0.64 MB upscaled
+ 0.32 MB compare
```

La RAM interna disponible es mucho menor.

Por ello casi toda la imagen grande se mantiene en PSRAM.

---

# 70. Reservas y DMA

No todos los buffers pueden ser tratados igual.

Un buffer utilizado por:

```text
CSI
ISP
PPA
JPEG
```

necesita cumplir requisitos del periférico correspondiente.

Por eso aparecen combinaciones como:

```c
MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
```

mientras que otros buffers escritos únicamente por CPU pueden utilizar:

```c
MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
```

La elección debe mantenerse explícita.

---

# 71. Diagnóstico por logs

Los logs son una parte importante de este proyecto.

Ejemplo de arranque correcto:

```text
Found 32MB PSRAM device
Speed: 200MHz
Camera: 800x800 RGB565
Frame buffer size: 1280000 bytes
Detected Camera sensor PID=0x5647
Formato elegido: ...800x800_50fps
readback: 0x3503=0x03 exposure=800 lineas gain=512 vts=984
JPEG_HW: Motor JPEG de hardware iniciado
Servidor HTTP iniciado
Sistema listo
```

Cada bloque confirma una capa distinta.

---

# 72. Cómo depurar la cámara

Si aparece:

```text
OV5647 no detectado
```

revisar:

- alimentación;
- LDO MIPI;
- SCCB/I2C;
- dirección `0x36`;
- conexión física.

Si aparece:

```text
No existe RAW8 800x800
```

revisar el driver/versiones y los formatos expuestos por `esp_cam_sensor_query_format()`.

---

# 73. Cómo depurar ISP

Si los colores aparecen extraños:

revisar:

```c
.bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG
```

El orden Bayer es crítico.

Cambiarlo puede hacer que rojo, verde y azul queden intercambiados o produzcan tonos incorrectos.

---

# 74. Cómo depurar PPA

Si PPA devuelve:

```text
ESP_ERR_INVALID_ARG
unsupported color mode
```

no asumir inmediatamente que el buffer está mal.

Primero verificar:

```text
color mode
input/output size
alignment
peripheral capability
chip revision
```

En este proyecto precisamente el escalado `GRAY8 -> GRAY8` falló por el modo de color soportado por el silicio/driver de la configuración actual.

---

# 75. Cómo depurar Sobel

Si todo se ve negro:

- revisar exposición;
- revisar ganancia;
- revisar Gaussian;
- revisar umbral;
- comprobar si la escena realmente tiene gradientes.

Recordar que Sobel no representa brillo absoluto.

Si se quiere diagnosticar el brillo real, hay que mirar el grayscale previo.

---

# 76. Cómo depurar JPEG

Si el processing funciona pero el navegador no muestra nada:

revisar:

```text
JPEG input pointer
JPEG input size
JPEG dimensions
output buffer
HTTP Content-Type
multipart boundary
```

El JPEG debe recibir el tamaño correcto del frame que realmente se le está pasando.

---

# 77. Cómo depurar HTTP

Mensajes típicos:

```text
Cliente conectado al stream
Cliente desconectado
```

son normales.

Si el navegador se cierra o pierde la conexión puede aparecer un error de socket.

Eso no significa necesariamente que haya fallado la cámara.

---

# 78. Advertencia de flash

En los logs actuales aparece una advertencia similar a:

```text
Detected size(16384k) larger than the size in the binary image header(2048k).
Using size in binary image header.
```

Esto significa que la flash física detectada es mayor que la capacidad indicada en la cabecera de la imagen.

No es la causa del fallo de Snapshot.

Pero debería corregirse más adelante para que la configuración de flash y particiones represente correctamente el hardware.

---

# 79. ESP32-C6 y ESP-Hosted

El log del sistema muestra el enlace entre P4 y C6 mediante SDIO.

Conceptualmente:

```text
ESP32-P4
   |
   | SDIO
   v
ESP32-C6
   |
   v
 Wi-Fi
```

ESP-Hosted actúa como puente para que el procesador principal pueda utilizar las capacidades inalámbricas del coprocesador.

---

# 80. No confundir SDIO del C6 con SD de almacenamiento

Son dos cosas completamente diferentes:

```text
SDIO P4 <-> ESP32-C6
```

es el enlace del coprocesador inalámbrico.

Mientras que:

```text
SDMMC/SDSPI P4 <-> microSD
```

sería almacenamiento masivo.

Agregar una microSD no implica sustituir el enlace SDIO del C6.

---

# 81. ¿Se puede usar simultáneamente Wi-Fi y microSD?

En principio sí, siempre que se utilicen periféricos y pines compatibles con la placa.

El diseño debe verificar:

- pines disponibles;
- conflictos con MIPI;
- conflictos con SDIO del C6;
- alimentación;
- DMA;
- ancho de banda.

El hecho de que el C6 utilice SDIO no significa automáticamente que todo SDMMC del P4 esté ocupado.

Pero los pines concretos sí deben verificarse en el hardware.

---

# 82. Primera versión recomendada del grabador

Para reducir riesgo, la primera implementación debería hacer solamente:

```text
Montar SD
    |
    v
Crear archivo
    |
    v
Grabar 5 segundos
    |
    v
Cerrar archivo
    |
    v
Desmontar SD
```

Sin controles HTTP al principio.

Luego:

```text
/record/start
/record/stop
```

Y finalmente:

```text
selección de duración
FPS
nombre de archivo
estado SD
espacio libre
```

---

# 83. Orden de implementación recomendado para la SD

### Etapa 1
Detectar la tarjeta.

### Etapa 2
Montar FATFS en `/sdcard`.

### Etapa 3
Crear `/sdcard/test.txt`.

### Etapa 4
Escribir un JPEG estático ya generado.

### Etapa 5
Escribir varios JPEG secuenciales.

### Etapa 6
Medir throughput sostenido.

### Etapa 7
Implementar cola/ring buffer.

### Etapa 8
Implementar contenedor MJPEG/AVI.

### Etapa 9
Integrar start/stop vía HTTP.

Este orden permite saber exactamente en qué punto aparece el problema.

---

# 84. Test de almacenamiento mínimo

Antes de intentar vídeo, el primer programa SD debería poder hacer:

```c
FILE *f = fopen("/sdcard/test.txt", "w");

fprintf(f, "ESP32-P4 SD test\\n");

fclose(f);
```

Después leerlo.

Si esto falla, no tiene sentido investigar todavía el rendimiento del vídeo.

---

# 85. Segundo test: JPEG individuales

El segundo test debería guardar:

```text
frame_0001.jpg
frame_0002.jpg
frame_0003.jpg
...
```

Esto permite comprobar:

- JPEG válido;
- SD válida;
- throughput;
- ausencia de corrupción.

---

# 86. Tercer test: grabación continua

Solo después de validar JPEG individuales se debería implementar:

```text
frame -> JPEG -> SD -> frame -> JPEG -> SD
```

Y medir:

```text
frames producidos
frames guardados
frames perdidos
```

---

# 87. Contadores recomendados

El recorder debería tener:

```c
uint32_t frames_generated;
uint32_t frames_written;
uint32_t frames_dropped;
uint32_t bytes_written;
```

y reportar:

```text
Generated : 1000
Written   : 998
Dropped   : 2
Bytes     : 41,233,120
```

Esto es mucho más útil que simplemente decir “la grabación parece andar”.

---

# 88. Tamaño del JPEG como variable importante

Durante la grabación se debería medir el tamaño real:

```c
out_len
```

Porque la calidad 80 no produce un tamaño constante.

Una escena con mucho detalle puede generar un JPEG bastante mayor que una escena uniforme.

Por eso el buffer y la SD deben dimensionarse para el peor caso razonable, no para el promedio mínimo.

---

# 89. Relación entre Sobel y almacenamiento

El Sobel suele producir imágenes más fáciles de comprimir que una fotografía natural llena de textura y color, especialmente en una salida grayscale.

Eso puede reducir el tamaño promedio del JPEG.

Pero no debe asumirse un tamaño fijo porque depende del escenario.

---

# 90. Modos de operación recomendados

Se pueden considerar dos modos:

## Modo LIVE

```text
camera
 -> processing
 -> JPEG
 -> Wi-Fi
```

## Modo RECORD

```text
camera
 -> processing
 -> JPEG
 -> SD
```

## Modo LIVE + RECORD

```text
camera
 -> processing
 -> JPEG
    |      \
    |       \
    v        v
  Wi-Fi      SD
```

El tercer modo es el que requiere la mejor arquitectura de buffers.

---

# 91. Lo que no se debe hacer

No conviene implementar:

```text
frame
 -> JPEG para Wi-Fi
 -> volver a procesar frame
 -> JPEG para SD
```

Eso duplica trabajo.

Tampoco conviene:

```text
cliente HTTP 1
cliente HTTP 2
cliente HTTP 3
```

cada uno tomando independientemente el mismo buffer de cámara actual.

---

# 92. Arquitectura ideal a futuro

Una evolución robusta podría ser:

```text
              +----------------+
              | Camera / CSI   |
              +--------+-------+
                       |
                       v
                +-------------+
                | Frame Queue  |
                +------+------+
                       |
                       v
                +-------------+
                | ISP / PPA    |
                +------+------+
                       |
                       v
                +-------------+
                | Gaussian     |
                +------+------+
                       |
                       v
                +-------------+
                | Sobel        |
                +------+------+
                       |
                       v
                +-------------+
                | JPEG HW      |
                +------+------+
                       |
              +--------+--------+
              |                 |
              v                 v
        HTTP/MJPEG         SD Recorder
              |                 |
              v                 v
          navegador          AVI/MJPEG
```

---

# 93. Estado actual frente al estado deseado

| Función | Estado actual |
|---|---|
| OV5647 | Funcional |
| MIPI CSI RAW8 | Funcional |
| 800x800 RAW8 | Funcional |
| ISP RAW8 -> RGB565 | Funcional |
| Demosaic GBRG | Funcional |
| PPA RGB565 -> GRAY8 | Funcional |
| Gaussian 3x3 | Funcional |
| Sobel continuo | Funcional |
| Compare original/Sobel | Funcional |
| JPEG HW | Funcional |
| HTTP MJPEG | Funcional |
| Wi-Fi mediante ESP32-C6 | Funcional |
| Snapshot con upscale PPA GRAY8 | No utilizar |
| microSD | No implementada todavía |
| Grabación MJPEG/AVI | No implementada todavía |
| Detección de zanja | No implementada |
| IA/visión artificial | No implementada |

---

# 94. Consideraciones de seguridad

Nunca debe subirse al repositorio una contraseña Wi-Fi real.

En el código de ejemplo utilizar:

```c
wifi_sta_start(
    "TU_SSID",
    "TU_PASSWORD"
);
```

O, mejor todavía, utilizar configuración separada, variables de entorno o un mecanismo seguro de provisioning.

---

# 95. Reproducibilidad

Para reproducir el proyecto, documentar siempre:

```text
ESP-IDF version
board
ESP32-P4 revision
OV5647 driver version
managed components versions
camera format
Sobel threshold
JPEG quality
exposure
analog gain
```

Un cambio de cualquiera de ellos puede modificar el resultado.

---

# 96. Parámetros principales en un solo lugar

Los parámetros de alto nivel se concentran actualmente en macros como:

```c
#define CAMERA_WIDTH   800
#define CAMERA_HEIGHT  800
#define CAMERA_BPP     2
#define OUT_WIDTH      400
#define OUT_HEIGHT     400
#define COMPARE_WIDTH  (OUT_WIDTH * 2)
#define COMPARE_HEIGHT OUT_HEIGHT
#define JPEG_QUALITY    80
#define SOBEL_THRESHOLD -1
```

Esto permite experimentar rápidamente sin reescribir la arquitectura.

---

# 97. Modificación de resolución

Si se modifica:

```c
#define CAMERA_WIDTH
#define CAMERA_HEIGHT
```

deben revisarse también:

- formato soportado por OV5647;
- configuración CSI;
- configuración ISP;
- tamaño del frame buffer;
- JPEG;
- PPA;
- buffers;
- compare;
- frontend.

No es una modificación aislada.

---

# 98. Modificación del tamaño de procesamiento

Si se modifica:

```c
#define OUT_WIDTH
#define OUT_HEIGHT
```

también se modifica:

- tamaño del Gaussian;
- tamaño del Sobel;
- tamaño del compare;
- costo computacional;
- cantidad de memoria.

Por eso deben mantenerse consistentes.

---

# 99. Convención conceptual del proyecto

Hay cuatro tipos de dato importantes:

```text
RAW8
RGB565
GRAY8
JPEG
```

Conviene pensar el sistema como una cadena de representaciones:

```text
RAW8
  |
  v
RGB565
  |
  v
GRAY8
  |
  v
Sobel GRAY8
  |
  v
JPEG
```

Cada transición tiene un responsable.

---

# 100. Responsable de cada transformación

| Transformación | Responsable |
|---|---|
| Sensor -> RAW8 | OV5647 |
| MIPI -> memoria | CSI |
| RAW8 -> RGB565 | ISP |
| RGB565 -> GRAY8 | PPA/CPU según etapa |
| Suavizado | CPU, Gaussian |
| Gradiente | CPU, Sobel |
| JPEG | JPEG HW |
| Transporte | HTTP + Wi-Fi |
| Almacenamiento futuro | SD + FATFS |

---

# 101. Regla fundamental al modificar el sistema

Cada vez que se agregue una etapa nueva preguntarse:

```text
¿quién produce el buffer?
¿quién consume el buffer?
¿en qué formato está?
¿cuánto mide?
¿está en RAM o PSRAM?
¿lo usa DMA?
¿la caché está coherente?
```

Esta lista evita la mayoría de los errores difíciles de depurar en este proyecto.

---

# 102. Ejemplo de razonamiento para un nuevo filtro

Supóngase que se quiere agregar un filtro de mediana.

No conviene empezar modificando HTTP.

El análisis correcto sería:

```text
¿Qué entra?
GRAY8 400x400

¿Qué sale?
GRAY8 400x400

¿CPU o hardware?
CPU

¿Nuevo buffer?
probablemente sí

¿DMA?
no necesariamente

¿dónde entra?
antes de Sobel
```

Entonces la cadena quedaría:

```text
GRAY
 -> Gaussian
 -> Median
 -> Sobel
 -> JPEG
```

---

# 103. Ejemplo de razonamiento para grabación

La grabación es distinta porque no es solo otro filtro.

No cambia el contenido del frame.

Cambia el destino:

```text
JPEG
 |
 +--> red
 |
 +--> SD
```

Por eso debe implementarse como consumidor del JPEG, no como otro algoritmo de píxeles.

---

# 104. Recomendación para el recorder

La primera versión debería **reutilizar exactamente el JPEG que ya produce el encoder**.

Es la arquitectura que minimiza procesamiento adicional.

El recorder no debería recibir:

```text
RAW8
RGB565
GRAY8
Sobel
```

Debería recibir:

```text
JPEG + tamaño + timestamp
```

Eso lo hace independiente del pipeline de imagen.

---

# 105. Diseño de mensaje para cola de grabación

Por ejemplo:

```c
typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t timestamp_ms;
} jpeg_frame_t;
```

Una cola puede transportar ese objeto desde el encoder hacia el recorder.

En un diseño real, el buffer al que apunta `data` debe tener ownership claramente definido para evitar que la cámara sobrescriba la memoria mientras la SD todavía la utiliza.

---

# 106. Ownership de buffers

Este será probablemente el cambio arquitectónico más importante cuando se agregue SD.

Actualmente:

```text
1 frame buffer principal
```

Con grabación robusta:

```text
buffer A = procesamiento
buffer B = SD
buffer C = próximo frame
```

o un esquema equivalente.

La regla es:

> Un buffer no debe ser reutilizado por el productor hasta que todos sus consumidores hayan terminado.

---

# 107. ¿Hace falta un segundo ESP32 para la SD?

No necesariamente.

El ESP32-P4 tiene recursos suficientes para manejar cámara, procesamiento, JPEG, red y, potencialmente, almacenamiento.

La dificultad principal no es “tener otro procesador”, sino diseñar correctamente:

- concurrencia;
- buffers;
- DMA;
- latencias.

---

# 108. Papel del ESP32-C6

El C6 está siendo usado para Wi-Fi.

No hay motivo para convertirlo ahora en procesador de imagen.

El pipeline de imagen está deliberadamente en el P4 porque allí están:

- CSI;
- ISP;
- PPA;
- JPEG.

El C6 funciona como coprocesador de conectividad.

---

# 109. Por qué el diseño es interesante para un proyecto universitario

El sistema reúne varias áreas de ingeniería electrónica:

```text
sensores
interfaces MIPI
procesamiento digital de señales
procesamiento de imágenes
arquitectura de computadores
DMA
memoria externa
sistemas embebidos
redes
almacenamiento
```

No se limita a programar una cámara.

Existe una cadena hardware/software completa.

---

# 110. Conceptos importantes para estudiar alrededor del proyecto

Para comprender completamente este proyecto conviene estudiar:

### MIPI CSI-2
Cómo se transmiten píxeles serialmente desde un sensor.

### Bayer pattern
Cómo RAW8 representa color.

### Demosaicing
Cómo recuperar RGB a partir de Bayer.

### RGB565
Por qué se utiliza un formato de 16 bits.

### Grayscale
Conversión a luminancia.

### Convolución 2D
Base matemática de Gaussian y Sobel.

### Gradiente de una imagen
Interpretación de `Gx` y `Gy`.

### JPEG
Compresión intra-frame.

### MJPEG
Secuencia de JPEG independientes.

### DMA
Transferencia de memoria sin copiar todo mediante CPU.

### PSRAM
Memoria externa de alta capacidad.

### Cache coherency
Problemas de CPU versus periféricos DMA.

### FreeRTOS
Tasks, semáforos y colas.

### FATFS
Sistema de archivos para SD.

---

# 111. Flujo completo explicado en una sola secuencia

Un frame hace exactamente esto conceptualmente:

```text
1. OV5647 captura una imagen Bayer RAW8.

2. El MIPI CSI recibe ese RAW8.

3. CSI lo deposita en el frame buffer.

4. El ISP interpreta el patrón Bayer GBRG.

5. El ISP genera RGB565.

6. El PPA convierte y reduce a GRAY8 400x400.

7. Gaussian suaviza el ruido.

8. Sobel calcula la magnitud aproximada del gradiente.

9. Para /compare:
       se construye una imagen 800x400
       con original a la izquierda y Sobel a la derecha.

10. JPEG HW comprime esa imagen.

11. HTTP envía el JPEG como una parte de un stream MJPEG.

12. El navegador muestra cada JPEG como parte del vídeo.
```

---

# 112. Qué cambia al grabar en SD

Solo se agrega un consumidor después de JPEG:

```text
JPEG
 |
 +--> HTTP
 |
 +--> SD
```

No es necesario cambiar:

```text
OV5647
CSI
ISP
Gaussian
Sobel
JPEG
```

La modificación importante está en el manejo de buffers y concurrencia después de JPEG.

---

# 113. Arquitectura de grabación propuesta

La arquitectura recomendada para una implementación posterior es:

```text
                         +--------------+
                         |   OV5647     |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |     CSI      |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |     ISP      |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |     PPA      |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |   Gaussian   |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |    Sobel     |
                         +------+-------+
                                |
                                v
                         +--------------+
                         |   JPEG HW    |
                         +------+-------+
                                |
                     +----------+----------+
                     |                     |
                     v                     v
              +-------------+       +-------------+
              |   HTTP      |       | SD Recorder |
              |   MJPEG     |       |   task      |
              +-------------+       +-------------+
```

---

# 114. Checklist antes de implementar SD

```text
[ ] Confirmar slot SDMMC disponible en la placa
[ ] Confirmar pines exactos en el esquemático
[ ] Confirmar alimentación 3.3 V
[ ] Montar FATFS
[ ] Crear archivo de prueba
[ ] Medir escritura secuencial
[ ] Guardar un JPEG
[ ] Guardar 10 JPEG
[ ] Medir tamaño medio JPEG
[ ] Medir FPS real
[ ] Añadir cola
[ ] Añadir recorder task
[ ] Implementar AVI/MJPEG
[ ] Integrar /record/start
[ ] Integrar /record/stop
```

---

# 115. Checklist para cualquier modificación del pipeline

```text
[ ] ¿Cambió resolución?
[ ] ¿Cambió formato de pixel?
[ ] ¿Cambió tamaño del buffer?
[ ] ¿Cambió quién escribe el buffer?
[ ] ¿Cambió quién lee el buffer?
[ ] ¿Se necesita DMA?
[ ] ¿Se necesita msync?
[ ] ¿Se mantiene la alineación?
[ ] ¿Se mantiene el throughput?
[ ] ¿Se mantiene el tiempo real?
```

---

# 116. Conclusión del estado actual

El proyecto ya tiene una cadena funcional de visión embebida:

```text
OV5647
  -> MIPI CSI
  -> RAW8
  -> ISP / demosaic
  -> RGB565
  -> PPA
  -> GRAY8 400x400
  -> Gaussian
  -> Sobel
  -> compare
  -> JPEG HW
  -> HTTP MJPEG
  -> Wi-Fi
  -> navegador
```

La siguiente ampliación natural es almacenamiento local.

La forma más limpia de incorporarlo no es volver a procesar imágenes, sino reutilizar el JPEG ya generado:

```text
JPEG
 +----> Wi-Fi
 +----> SD
```

Para una primera implementación, el objetivo debe ser **MJPEG/AVI con grabación de una duración definida**, utilizando un task independiente y una cola de buffers para desacoplar la velocidad de procesamiento de la latencia de la tarjeta SD.

---

# 117. Referencias oficiales recomendadas

- ESP-IDF — FATFS: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html
- ESP-IDF — SDMMC ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32p4/api-reference/peripherals/sdmmc_host.html
- ESP-IDF — SDSPI: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdspi_host.html
- ESP-IDF — PPA ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/ppa.html
- ESP-IDF — JPEG ESP32-P4: https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html

---

# 118. Nota final para futuros desarrolladores

No tratar este proyecto como una aplicación monolítica de cámara.

Es más útil visualizarlo como una cadena de procesamiento con contratos entre etapas:

```text
                 FORMATO       TAMAÑO        RESPONSABLE

OV5647            RAW8          800x800       sensor
CSI               RAW8          800x800       cámara
ISP               RGB565        800x800       hardware ISP
PPA               GRAY8         400x400       hardware PPA
Gaussian          GRAY8         400x400       CPU
Sobel             GRAY8         400x400       CPU
Compare           GRAY8         800x400       CPU
JPEG              compressed    variable       hardware JPEG
HTTP              bytes         variable       network
SD                bytes         variable       storage futuro
```

Siempre que una etapa funcione, documentar:

```text
entrada
salida
formato
resolución
memoria
DMA
cache
rendimiento
```

Así el proyecto puede crecer sin perder trazabilidad.
