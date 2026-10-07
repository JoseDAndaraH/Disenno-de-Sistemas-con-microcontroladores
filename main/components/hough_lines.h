#ifndef HOUGH_LINES_H
#define HOUGH_LINES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Configuración del detector Hough
 * ============================================================ */

typedef struct {
    uint16_t width;
    uint16_t height;

    /* Número de ángulos utilizados.
     * Normalmente 180 -> theta = 0...179 grados.
     */
    uint16_t theta_steps;

    /* Umbral aplicado sobre la magnitud del Sobel.
     * Un píxel con sobel >= threshold vota.
     */
    uint8_t edge_threshold;

    /* Cantidad máxima de líneas que queremos obtener. */
    uint8_t max_lines;

    /* Supresión de máximos cercanos.
     * Evita devolver varias veces prácticamente
     * la misma línea.
     */
    uint16_t rho_suppression;
    uint16_t theta_suppression;

} hough_config_t;


/* ============================================================
 * Línea detectada
 * ============================================================ */

typedef struct {
    float rho;
    float theta_rad;

    /* Ángulo en grados, para facilitar debug. */
    float theta_deg;

    /* Cantidad de votos del pico. */
    uint16_t votes;

} hough_line_t;


/* ============================================================
 * API
 * ============================================================ */

/*
 * Inicializa el contexto interno de Hough.
 *
 * Reserva el acumulador y precalcula seno/coseno.
 */
int hough_init(
    const hough_config_t *config
);


/*
 * Ejecuta la transformada de Hough sobre una imagen
 * de magnitud de Sobel de 8 bits.
 *
 * sobel:
 *     imagen 400x400, valores 0..255
 *
 * lines:
 *     array donde se devolverán las líneas.
 *
 * max_lines:
 *     cantidad máxima de líneas a devolver.
 *
 * return:
 *     cantidad de líneas detectadas.
 *     < 0 en caso de error.
 */
int hough_detect_lines(
    const uint8_t *sobel,
    hough_line_t *lines,
    size_t max_lines
);


/*
 * Libera la memoria utilizada por Hough.
 */
void hough_deinit(void);


void hough_draw_lines_gray8(
    uint8_t *image,
    int width,
    int height,
    const hough_line_t *lines,
    size_t num_lines,
    uint8_t value
);


#ifdef __cplusplus
}
#endif

#endif