#include "jpeg_encoder.h"

#include "esp_log.h"
#include "driver/jpeg_encode.h"

static const char *TAG = "JPEG_HW";

static jpeg_encoder_handle_t jpeg_handle = NULL;

static uint8_t *jpeg_out_buffer = NULL;
static size_t jpeg_out_capacity = 0;


/* ============================================================
 * Inicialización
 * ============================================================ */

esp_err_t init_jpeg_encoder(int width, int height)
{
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };

    esp_err_t ret = jpeg_new_encoder_engine(
        &eng_cfg,
        &jpeg_handle
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Fallo al iniciar motor JPEG: %s",
            esp_err_to_name(ret)
        );
        return ret;
    }

    /*
     * Reservamos un buffer amplio para la salida JPEG.
     *
     * Para 800x800 esto es deliberadamente holgado.
     */
    size_t est_size =
        ((size_t)width * (size_t)height * 2) + 4096;

    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    jpeg_out_buffer = (uint8_t *)jpeg_alloc_encoder_mem(
        est_size,
        &mem_cfg,
        &jpeg_out_capacity
    );

    if (jpeg_out_buffer == NULL) {
        ESP_LOGE(
            TAG,
            "Sin memoria para buffer de salida JPEG"
        );
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Motor JPEG de hardware iniciado. "
        "Capacidad salida: %zu bytes",
        jpeg_out_capacity
    );

    return ESP_OK;
}


/* ============================================================
 * RGB565 -> JPEG
 *
 * Esta es la función que vamos a utilizar AHORA.
 * ============================================================ */

esp_err_t encode_rgb565_to_jpeg(
    const uint8_t *in_buffer,
    size_t in_size,
    int width,
    int height,
    uint8_t quality,
    const uint8_t **out_buffer,
    size_t *out_size
)
{
    if (!jpeg_handle ||
        !jpeg_out_buffer ||
        !in_buffer ||
        !out_buffer ||
        !out_size) {

        return ESP_ERR_INVALID_STATE;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality,
        .width = width,
        .height = height,
    };

    uint32_t jpeg_len = 0;

    esp_err_t ret = jpeg_encoder_process(
        jpeg_handle,
        &enc_cfg,
        in_buffer,
        in_size,
        jpeg_out_buffer,
        jpeg_out_capacity,
        &jpeg_len
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Fallo RGB565 -> JPEG: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    *out_buffer = jpeg_out_buffer;
    *out_size = jpeg_len;

    return ESP_OK;
}


/* ============================================================
 * GRAY8 -> JPEG
 *
 * Esta NO la usamos todavía.
 *
 * La utilizaremos cuando tengamos:
 *
 * RGB565 -> PPA -> GRAY8 -> Sobel -> GRAY8 -> JPEG
 * ============================================================ */

esp_err_t encode_gray_to_jpeg(
    const uint8_t *in_buffer,
    size_t in_size,
    int width,
    int height,
    uint8_t quality,
    const uint8_t **out_buffer,
    size_t *out_size
)
{
    if (!jpeg_handle ||
        !jpeg_out_buffer ||
        !in_buffer ||
        !out_buffer ||
        !out_size) {

        return ESP_ERR_INVALID_STATE;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_GRAY,
        .sub_sample = JPEG_DOWN_SAMPLING_GRAY,
        .image_quality = quality,
        .width = width,
        .height = height,
    };

    uint32_t jpeg_len = 0;

    esp_err_t ret = jpeg_encoder_process(
        jpeg_handle,
        &enc_cfg,
        in_buffer,
        in_size,
        jpeg_out_buffer,
        jpeg_out_capacity,
        &jpeg_len
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Fallo GRAY8 -> JPEG: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    *out_buffer = jpeg_out_buffer;
    *out_size = jpeg_len;

    return ESP_OK;
}