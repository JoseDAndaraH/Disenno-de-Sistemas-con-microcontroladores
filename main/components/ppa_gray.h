#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * in_width / in_height   -> resolución que entrega la cámara (RGB565)
 * out_width / out_height -> resolución escalada de salida (RGB888 intermedio y GRAY8 final)
 *
 * Si out_width == in_width y out_height == in_height, no hay escalado
 * (comportamiento equivalente al que ya tenías).
 */
esp_err_t init_ppa_gray(
    int in_width,
    int in_height,
    int out_width,
    int out_height
);

/*
 * rgb565_buffer / rgb565_size deben corresponder a in_width x in_height.
 * *gray_buffer / *gray_size quedan en out_width x out_height.
 */
esp_err_t rgb565_to_gray(
    const uint8_t *rgb565_buffer,
    size_t rgb565_size,
    const uint8_t **gray_buffer,
    size_t *gray_size
);

esp_err_t ppa_upscale_gray8(
    const uint8_t *in_buffer,
    int in_width,
    int in_height,
    uint8_t *out_buffer,
    int out_width,
    int out_height
);

#ifdef __cplusplus
}
#endif