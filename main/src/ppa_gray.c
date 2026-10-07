#include "ppa_gray.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "PPA_GRAY";


/* ============================================================
 * PPA
 * ============================================================ */

static ppa_client_handle_t s_ppa_client = NULL;


/* ============================================================
 * Buffers intermedios
 * ============================================================ */

/*
 * RGB888 = 3 bytes/pixel
 *
 * Este buffer corresponde a la imagen ya escalada.
 */
static uint8_t *s_rgb888_buffer = NULL;
static size_t s_rgb888_buffer_size = 0;


/*
 * GRAY8 = 1 byte/pixel
 *
 * Este buffer también corresponde a la imagen ya escalada.
 */
static uint8_t *s_gray_buffer = NULL;
static size_t s_gray_buffer_size = 0;


/* ============================================================
 * Dimensiones
 * ============================================================ */

/*
 * Resolución que entrega la cámara.
 *
 * Ejemplo:
 *     800 x 800
 */
static int s_in_width = 0;
static int s_in_height = 0;


/*
 * Resolución de procesamiento.
 *
 * Ejemplo:
 *     400 x 400
 *
 * RGB888 y GRAY8 se generan a esta resolución.
 */
static int s_out_width = 0;
static int s_out_height = 0;


/* ============================================================
 * INICIALIZACION
 * ============================================================ */

esp_err_t init_ppa_gray(
    int in_width,
    int in_height,
    int out_width,
    int out_height
)
{
    if (in_width <= 0 ||
        in_height <= 0 ||
        out_width <= 0 ||
        out_height <= 0) {

        return ESP_ERR_INVALID_ARG;
    }


    s_in_width = in_width;
    s_in_height = in_height;

    s_out_width = out_width;
    s_out_height = out_height;


    /*
     * Los buffers intermedios se dimensionan según
     * la resolución de salida.
     */

    s_rgb888_buffer_size =
        (size_t)out_width *
        (size_t)out_height *
        3;


    s_gray_buffer_size =
        (size_t)out_width *
        (size_t)out_height;


    /* --------------------------------------------------------
     * Buffer RGB888
     * -------------------------------------------------------- */

    s_rgb888_buffer = heap_caps_aligned_alloc(
        64,
        s_rgb888_buffer_size,
        MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT
    );

    if (s_rgb888_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer RGB888"
        );

        return ESP_ERR_NO_MEM;
    }


    memset(
        s_rgb888_buffer,
        0,
        s_rgb888_buffer_size
    );


    /* --------------------------------------------------------
     * Buffer GRAY8
     * -------------------------------------------------------- */

    s_gray_buffer = heap_caps_aligned_alloc(
        64,
        s_gray_buffer_size,
        MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT
    );

    if (s_gray_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer GRAY8"
        );

        heap_caps_free(s_rgb888_buffer);

        s_rgb888_buffer = NULL;

        return ESP_ERR_NO_MEM;
    }


    memset(
        s_gray_buffer,
        0,
        s_gray_buffer_size
    );


    /* --------------------------------------------------------
     * Cliente PPA SRM
     * -------------------------------------------------------- */

    /*
     * El mismo cliente PPA se utiliza para:
     *
     * 1) RGB565 -> RGB888 + escalado
     *
     * 2) GRAY8 -> GRAY8 + escalado
     *
     * La conversión RGB888 -> GRAY8 continúa siendo
     * realizada por CPU.
     */

    ppa_client_config_t client_config = {

        .oper_type =
            PPA_OPERATION_SRM,

        .max_pending_trans_num =
            1,

        .data_burst_length =
            PPA_DATA_BURST_LENGTH_128,
    };


    esp_err_t ret = ppa_register_client(
        &client_config,
        &s_ppa_client
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "ppa_register_client fallo: %s",
            esp_err_to_name(ret)
        );

        heap_caps_free(s_gray_buffer);
        heap_caps_free(s_rgb888_buffer);

        s_gray_buffer = NULL;
        s_rgb888_buffer = NULL;

        return ret;
    }


    ESP_LOGI(
        TAG,
        "PPA inicializado: %dx%d -> %dx%d",
        s_in_width,
        s_in_height,
        s_out_width,
        s_out_height
    );


    ESP_LOGI(
        TAG,
        "RGB888 buffer: %zu bytes @ %p",
        s_rgb888_buffer_size,
        s_rgb888_buffer
    );


    ESP_LOGI(
        TAG,
        "GRAY8 buffer: %zu bytes @ %p",
        s_gray_buffer_size,
        s_gray_buffer
    );


    return ESP_OK;
}


/* ============================================================
 * RGB565 -> RGB888 con escalado mediante PPA
 * ============================================================ */

static esp_err_t convert_rgb565_to_rgb888(
    const uint8_t *rgb565_buffer
)
{
    /*
     * Entrada:
     *
     *     s_in_width x s_in_height
     *
     * Salida:
     *
     *     s_out_width x s_out_height
     */


    ppa_in_pic_blk_config_t in_config = {

        .buffer =
            rgb565_buffer,

        .pic_w =
            s_in_width,

        .pic_h =
            s_in_height,

        .block_w =
            s_in_width,

        .block_h =
            s_in_height,

        .block_offset_x = 0,

        .block_offset_y = 0,

        .srm_cm =
            PPA_SRM_COLOR_MODE_RGB565,
    };


    ppa_out_pic_blk_config_t out_config = {

        .buffer =
            s_rgb888_buffer,

        .buffer_size =
            s_rgb888_buffer_size,

        .pic_w =
            s_out_width,

        .pic_h =
            s_out_height,

        .block_offset_x = 0,

        .block_offset_y = 0,

        .srm_cm =
            PPA_SRM_COLOR_MODE_RGB888,
    };


    ppa_srm_oper_config_t srm_config = {

        .in =
            in_config,

        .out =
            out_config,

        .rotation_angle =
            PPA_SRM_ROTATION_ANGLE_0,

        .scale_x =
            (float)s_out_width /
            (float)s_in_width,

        .scale_y =
            (float)s_out_height /
            (float)s_in_height,

        .mirror_x =
            false,

        .mirror_y =
            false,

        .rgb_swap =
            false,

        .byte_swap =
            false,

        .alpha_update_mode =
            PPA_ALPHA_NO_CHANGE,

        .mode =
            PPA_TRANS_MODE_BLOCKING,
    };


    ESP_LOGI(
        TAG,
        "PPA: RGB565 %dx%d -> RGB888 %dx%d "
        "(scale_x=%.3f, scale_y=%.3f)",
        s_in_width,
        s_in_height,
        s_out_width,
        s_out_height,
        srm_config.scale_x,
        srm_config.scale_y
    );


    esp_err_t ret =
        ppa_do_scale_rotate_mirror(
            s_ppa_client,
            &srm_config
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "RGB565 -> RGB888 (escalado) fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /*
     * El PPA acaba de escribir RGB888.
     *
     * La CPU va a leer ese buffer, por lo que
     * sincronizamos memoria -> cache.
     */

    ret = esp_cache_msync(
        s_rgb888_buffer,
        s_rgb888_buffer_size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "msync RGB888 fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    return ESP_OK;
}


/* ============================================================
 * RGB888 -> GRAY8
 *
 * Conversión realizada por CPU.
 *
 * gray = (R*77 + G*150 + B*29) >> 8
 *
 * 77 + 150 + 29 = 256
 * ============================================================ */

static void convert_rgb888_to_gray(void)
{
    const int pixel_count =
        s_out_width *
        s_out_height;


    for (int i = 0; i < pixel_count; i++) {

        uint8_t r =
            s_rgb888_buffer[i * 3 + 0];

        uint8_t g =
            s_rgb888_buffer[i * 3 + 1];

        uint8_t b =
            s_rgb888_buffer[i * 3 + 2];


        s_gray_buffer[i] =
            (uint8_t)(
                (
                    (uint16_t)77 * r +
                    (uint16_t)150 * g +
                    (uint16_t)29 * b
                ) >> 8
            );
    }
}


/* ============================================================
 * FUNCION PUBLICA
 *
 * RGB565 -> RGB888 -> GRAY8
 *
 * La salida tiene s_out_width x s_out_height.
 * ============================================================ */

esp_err_t rgb565_to_gray(
    const uint8_t *rgb565_buffer,
    size_t rgb565_size,
    const uint8_t **gray_buffer,
    size_t *gray_size
)
{
    if (rgb565_buffer == NULL ||
        gray_buffer == NULL ||
        gray_size == NULL) {

        return ESP_ERR_INVALID_ARG;
    }


    if (s_ppa_client == NULL ||
        s_rgb888_buffer == NULL ||
        s_gray_buffer == NULL) {

        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Tamaño esperado de RGB565:
     *
     * 2 bytes/pixel
     */

    const size_t expected_rgb565_size =
        (size_t)s_in_width *
        (size_t)s_in_height *
        2;


    if (rgb565_size < expected_rgb565_size) {

        ESP_LOGE(
            TAG,
            "RGB565 demasiado pequeño: %zu, "
            "esperado: %zu",
            rgb565_size,
            expected_rgb565_size
        );

        return ESP_ERR_INVALID_SIZE;
    }


    /*
     * CSI/ISP escribió RGB565 en memoria.
     *
     * El PPA necesita ver esos datos.
     */

    esp_err_t ret = esp_cache_msync(
        (void *)rgb565_buffer,
        expected_rgb565_size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "msync RGB565 fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* --------------------------------------------------------
     * ETAPA 1
     *
     * RGB565 800x800
     *       ↓
     * PPA
     *       ↓
     * RGB888 400x400
     * -------------------------------------------------------- */

    ret = convert_rgb565_to_rgb888(
        rgb565_buffer
    );


    if (ret != ESP_OK) {
        return ret;
    }


    /* --------------------------------------------------------
     * ETAPA 2
     *
     * RGB888 400x400
     *       ↓
     * CPU
     *       ↓
     * GRAY8 400x400
     * -------------------------------------------------------- */

    convert_rgb888_to_gray();


    *gray_buffer =
        s_gray_buffer;

    *gray_size =
        s_gray_buffer_size;


    return ESP_OK;
}


/* ============================================================
 * GRAY8 -> GRAY8 con escalado mediante PPA
 *
 * Ejemplo:
 *
 *     400x400 -> 800x800
 *
 * El Sobel se ejecuta antes de esta función.
 * ============================================================ */

esp_err_t ppa_upscale_gray8(
    const uint8_t *in_buffer,
    int in_width,
    int in_height,
    uint8_t *out_buffer,
    int out_width,
    int out_height
)
{
    if (in_buffer == NULL ||
        out_buffer == NULL ||
        in_width <= 0 ||
        in_height <= 0 ||
        out_width <= 0 ||
        out_height <= 0) {

        return ESP_ERR_INVALID_ARG;
    }


    const size_t in_size =
        (size_t)in_width *
        (size_t)in_height;


    const size_t out_size =
        (size_t)out_width *
        (size_t)out_height;


    /*
     * La CPU acaba de escribir el buffer del Sobel.
     *
     * CPU/cache -> memoria
     *
     * El PPA leerá esos datos desde memoria.
     */

    esp_err_t ret = esp_cache_msync(
        (void *)in_buffer,
        in_size,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "msync input GRAY8 fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* --------------------------------------------------------
     * Configuración de entrada
     * -------------------------------------------------------- */

    ppa_in_pic_blk_config_t in_config = {

        .buffer =
            in_buffer,

        .pic_w =
            in_width,

        .pic_h =
            in_height,

        .block_w =
            in_width,

        .block_h =
            in_height,

        .block_offset_x = 0,

        .block_offset_y = 0,

        .srm_cm =
            PPA_SRM_COLOR_MODE_GRAY8,
    };


    /* --------------------------------------------------------
     * Configuración de salida
     * -------------------------------------------------------- */

    ppa_out_pic_blk_config_t out_config = {

        .buffer =
            out_buffer,

        .buffer_size =
            out_size,

        .pic_w =
            out_width,

        .pic_h =
            out_height,

        .block_offset_x = 0,

        .block_offset_y = 0,

        .srm_cm =
            PPA_SRM_COLOR_MODE_GRAY8,
    };


    /* --------------------------------------------------------
     * Configuración SRM
     * -------------------------------------------------------- */

    ppa_srm_oper_config_t srm_config = {

        .in =
            in_config,

        .out =
            out_config,

        .rotation_angle =
            PPA_SRM_ROTATION_ANGLE_0,

        .scale_x =
            (float)out_width /
            (float)in_width,

        .scale_y =
            (float)out_height /
            (float)in_height,

        .mirror_x =
            false,

        .mirror_y =
            false,

        .rgb_swap =
            false,

        .byte_swap =
            false,

        .alpha_update_mode =
            PPA_ALPHA_NO_CHANGE,

        .mode =
            PPA_TRANS_MODE_BLOCKING,
    };


    ESP_LOGI(
        TAG,
        "PPA: GRAY8 %dx%d -> GRAY8 %dx%d "
        "(scale_x=%.3f, scale_y=%.3f)",
        in_width,
        in_height,
        out_width,
        out_height,
        srm_config.scale_x,
        srm_config.scale_y
    );


    /* --------------------------------------------------------
     * Ejecutar PPA
     * -------------------------------------------------------- */

    ret =
        ppa_do_scale_rotate_mirror(
            s_ppa_client,
            &srm_config
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GRAY8 upscale %dx%d -> %dx%d fallo: %s",
            in_width,
            in_height,
            out_width,
            out_height,
            esp_err_to_name(ret)
        );

        return ret;
    }


    /*
     * El PPA acaba de escribir out_buffer.
     *
     * Memoria -> cache
     *
     * Así la CPU/JPEG verá el resultado.
     */

    ret = esp_cache_msync(
        out_buffer,
        out_size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "msync output GRAY8 fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    return ESP_OK;
}