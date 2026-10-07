#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * width / height acá son la resolución "grande" que se usa para
 * dimensionar el buffer de salida JPEG (normalmente CAMERA_WIDTH x
 * CAMERA_HEIGHT, para que alcance de sobra sin importar si después
 * codificás a una resolución más chica).
 */
esp_err_t init_jpeg_encoder(int width, int height);

/*
 * width / height acá son la resolución REAL del buffer que estás
 * pasando en in_buffer (puede ser distinta a la usada en init).
 */
esp_err_t encode_rgb565_to_jpeg(
    const uint8_t *in_buffer,
    size_t in_size,
    int width,
    int height,
    uint8_t quality,
    const uint8_t **out_buffer,
    size_t *out_size
);

esp_err_t encode_gray_to_jpeg(
    const uint8_t *in_buffer,
    size_t in_size,
    int width,
    int height,
    uint8_t quality,
    const uint8_t **out_buffer,
    size_t *out_size
);

#ifdef __cplusplus
}
#endif