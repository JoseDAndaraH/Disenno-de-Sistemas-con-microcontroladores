#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Aplica el filtro de Sobel a una imagen en escala de grises (GRAY8, 1 byte/pixel).
 *
 * - src y dst deben tener el mismo tamaño (width * height bytes) y NO deben
 *   apuntar al mismo buffer.
 * - El resultado es la magnitud del gradiente |Gx| + |Gy| (aproximación L1,
 *   evita el sqrt), saturada a 255.
 * - El borde de 1 pixel se calcula con clamp de coordenadas (no queda negro).
 */
void sobel_gray8(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Igual que sobel_gray8, pero además binariza el resultado:
 * pixel = 255 si magnitud >= threshold, si no 0.
 * threshold < 0 desactiva la binarización (idéntico a sobel_gray8).
 */
void sobel_gray8_threshold(const uint8_t *src, uint8_t *dst, int width, int height, int threshold);

void gaussian3x3_gray8(
    const uint8_t *src,
    uint8_t *dst,
    int width,
    int height
);


#ifdef __cplusplus
}
#endif