#include "hough_lines.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_heap_caps.h"


/* ============================================================
 * CONSTANTES
 * ============================================================ */

#define HOUGH_PI 3.14159265358979323846f


/* ============================================================
 * CONTEXTO INTERNO
 * ============================================================ */

typedef struct {

    hough_config_t config;

    /* Máximo valor absoluto de rho. */
    int rho_max;

    /* Cantidad total de posiciones de rho. */
    int rho_count;

    /*
     * Acumulador Hough.
     *
     * accumulator[
     *     theta * rho_count + rho_index
     * ]
     */
    uint16_t *accumulator;

    /* Tablas trigonométricas precalculadas. */
    float *cos_table;
    float *sin_table;

} hough_context_t;


static hough_context_t s_hough = {
    0
};


/* ============================================================
 * UTILIDADES
 * ============================================================ */

static int clamp_int(
    int value,
    int min_value,
    int max_value
)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}


/* ============================================================
 * INICIALIZACIÓN
 * ============================================================ */

int hough_init(
    const hough_config_t *config
)
{
    if (config == NULL) {
        return -1;
    }

    if (config->width == 0 ||
        config->height == 0) {
        return -1;
    }

    if (config->theta_steps == 0) {
        return -1;
    }

    /*
     * Por seguridad, liberar una instancia anterior
     * si existiera.
     */
    hough_deinit();

    memset(
        &s_hough,
        0,
        sizeof(s_hough)
    );

    s_hough.config = *config;


    /* --------------------------------------------------------
     * rho máximo
     *
     * rho = x*cos(theta) + y*sin(theta)
     *
     * El valor máximo absoluto está acotado por
     * la diagonal de la imagen.
     * -------------------------------------------------------- */

    float diagonal =
        sqrtf(
            (float)config->width *
            (float)config->width
            +
            (float)config->height *
            (float)config->height
        );

    s_hough.rho_max =
        (int)ceilf(diagonal);

    s_hough.rho_count =
        2 * s_hough.rho_max + 1;


    /* --------------------------------------------------------
     * Reservar acumulador
     *
     * Para 400x400 y 180 theta:
     *
     * ≈ 408 kB
     *
     * Lo colocamos explícitamente en PSRAM.
     * -------------------------------------------------------- */

    size_t accumulator_size =
        (size_t)config->theta_steps *
        (size_t)s_hough.rho_count *
        sizeof(uint16_t);

    s_hough.accumulator =
        heap_caps_calloc(
            1,
            accumulator_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (s_hough.accumulator == NULL) {

        hough_deinit();

        return -1;
    }


    /* --------------------------------------------------------
     * Reservar tablas trigonométricas
     *
     * Son pequeñas:
     *
     * 180 * sizeof(float)
     * ≈ 720 bytes por tabla
     * -------------------------------------------------------- */

    s_hough.cos_table =
        malloc(
            (size_t)config->theta_steps *
            sizeof(float)
        );

    s_hough.sin_table =
        malloc(
            (size_t)config->theta_steps *
            sizeof(float)
        );

    if (s_hough.cos_table == NULL ||
        s_hough.sin_table == NULL) {

        hough_deinit();

        return -1;
    }


    /* --------------------------------------------------------
     * Precalcular seno y coseno
     *
     * theta va de 0 a 179 grados cuando
     * theta_steps = 180.
     * -------------------------------------------------------- */

    for (uint16_t t = 0;
         t < config->theta_steps;
         t++) {

        float theta =
            ((float)t * HOUGH_PI) /
            (float)config->theta_steps;

        s_hough.cos_table[t] =
            cosf(theta);

        s_hough.sin_table[t] =
            sinf(theta);
    }


    return 0;
}


/* ============================================================
 * CONSTRUIR ACUMULADOR
 * ============================================================ */

static void build_accumulator(
    const uint8_t *sobel
)
{
    /*
     * Limpiar acumulador.
     */
    memset(
        s_hough.accumulator,
        0,
        (size_t)s_hough.config.theta_steps *
        (size_t)s_hough.rho_count *
        sizeof(uint16_t)
    );


    /*
     * Recorrer imagen.
     */
    for (int y = 0;
         y < s_hough.config.height;
         y++) {

        for (int x = 0;
             x < s_hough.config.width;
             x++) {

            uint8_t edge =
                sobel[
                    y * s_hough.config.width +
                    x
                ];


            /*
             * Solo los píxeles que superan
             * el umbral son considerados bordes.
             */
            if (edge <
                s_hough.config.edge_threshold) {

                continue;
            }


            /*
             * Cada píxel de borde vota para
             * todos los valores de theta.
             *
             * rho = x*cos(theta) + y*sin(theta)
             */
            for (uint16_t t = 0;
                 t < s_hough.config.theta_steps;
                 t++) {

                float rho =
                    (float)x *
                    s_hough.cos_table[t]
                    +
                    (float)y *
                    s_hough.sin_table[t];


                /*
                 * Discretización de rho.
                 */
                int rho_int =
                    (int)lroundf(rho);


                /*
                 * rho puede ser negativo.
                 *
                 * Lo desplazamos para que el
                 * índice del acumulador empiece en 0.
                 */
                int rho_index =
                    rho_int +
                    s_hough.rho_max;


                if (rho_index < 0 ||
                    rho_index >= s_hough.rho_count) {

                    continue;
                }


                size_t index =
                    (size_t)t *
                    (size_t)s_hough.rho_count
                    +
                    (size_t)rho_index;


                /*
                 * Incrementar voto.
                 *
                 * Evitamos overflow.
                 */
                if (s_hough.accumulator[index] <
                    UINT16_MAX) {

                    s_hough.accumulator[index]++;
                }
            }
        }
    }
}


/* ============================================================
 * SUPRESIÓN DE UN MÁXIMO
 * ============================================================ */

static void suppress_peak(
    int theta_index,
    int rho_index
)
{
    int theta_radius =
        s_hough.config.theta_suppression;

    int rho_radius =
        s_hough.config.rho_suppression;


    int theta_min =
        clamp_int(
            theta_index - theta_radius,
            0,
            (int)s_hough.config.theta_steps - 1
        );

    int theta_max =
        clamp_int(
            theta_index + theta_radius,
            0,
            (int)s_hough.config.theta_steps - 1
        );


    int rho_min =
        clamp_int(
            rho_index - rho_radius,
            0,
            s_hough.rho_count - 1
        );

    int rho_max =
        clamp_int(
            rho_index + rho_radius,
            0,
            s_hough.rho_count - 1
        );


    /*
     * Poner a cero una vecindad alrededor
     * del pico encontrado.
     *
     * Esto evita devolver varias veces
     * prácticamente la misma línea.
     */
    for (int t = theta_min;
         t <= theta_max;
         t++) {

        for (int r = rho_min;
             r <= rho_max;
             r++) {

            size_t index =
                (size_t)t *
                (size_t)s_hough.rho_count
                +
                (size_t)r;

            s_hough.accumulator[index] = 0;
        }
    }
}


/* ============================================================
 * DETECCIÓN DE LÍNEAS
 * ============================================================ */

int hough_detect_lines(
    const uint8_t *sobel,
    hough_line_t *lines,
    size_t max_lines
)
{
    if (sobel == NULL ||
        lines == NULL ||
        max_lines == 0) {

        return -1;
    }

    if (s_hough.accumulator == NULL) {
        return -1;
    }


    /* --------------------------------------------------------
     * 1. Construir acumulador
     * -------------------------------------------------------- */

    build_accumulator(sobel);


    /* --------------------------------------------------------
     * 2. Determinar cuántas líneas pedir
     * -------------------------------------------------------- */

    size_t requested =
        max_lines;

    if (requested >
        s_hough.config.max_lines) {

        requested =
            s_hough.config.max_lines;
    }


    size_t detected = 0;


    /* --------------------------------------------------------
     * 3. Buscar máximos
     * -------------------------------------------------------- */

    for (size_t n = 0;
         n < requested;
         n++) {

        uint16_t best_votes = 0;

        int best_theta = -1;
        int best_rho = -1;


        /*
         * Buscar el máximo global actual.
         */
        for (uint16_t t = 0;
             t < s_hough.config.theta_steps;
             t++) {

            for (int r = 0;
                 r < s_hough.rho_count;
                 r++) {

                size_t index =
                    (size_t)t *
                    (size_t)s_hough.rho_count
                    +
                    (size_t)r;

                uint16_t votes =
                    s_hough.accumulator[index];


                if (votes > best_votes) {

                    best_votes =
                        votes;

                    best_theta =
                        (int)t;

                    best_rho =
                        r;
                }
            }
        }


        /*
         * No quedan votos.
         */
        if (best_theta < 0 ||
            best_rho < 0 ||
            best_votes == 0) {

            break;
        }


        /* ----------------------------------------------------
         * 4. Convertir rho
         * ---------------------------------------------------- */

        int rho_value =
            best_rho -
            s_hough.rho_max;


        /* ----------------------------------------------------
         * 5. Convertir theta
         *
         * theta = 0...pi
         * ---------------------------------------------------- */

        float theta_rad =
            ((float)best_theta * HOUGH_PI) /
            (float)s_hough.config.theta_steps;

        float theta_deg =
            theta_rad *
            180.0f /
            HOUGH_PI;


        /* ----------------------------------------------------
         * 6. Guardar línea
         * ---------------------------------------------------- */

        lines[detected].rho =
            (float)rho_value;

        lines[detected].theta_rad =
            theta_rad;

        lines[detected].theta_deg =
            theta_deg;

        lines[detected].votes =
            best_votes;


        detected++;


        /* ----------------------------------------------------
         * 7. Suprimir vecindad del máximo
         * ---------------------------------------------------- */

        suppress_peak(
            best_theta,
            best_rho
        );
    }


    return (int)detected;
}


/* ============================================================
 * LIBERAR RECURSOS
 * ============================================================ */

void hough_deinit(void)
{
    /*
     * El acumulador fue reservado específicamente
     * en PSRAM.
     */
    if (s_hough.accumulator != NULL) {

        heap_caps_free(
            s_hough.accumulator
        );
    }


    /*
     * Las tablas trigonométricas usan malloc().
     */
    if (s_hough.cos_table != NULL) {

        free(
            s_hough.cos_table
        );
    }

    if (s_hough.sin_table != NULL) {

        free(
            s_hough.sin_table
        );
    }


    memset(
        &s_hough,
        0,
        sizeof(s_hough)
    );
}

void hough_draw_lines_gray8(
    uint8_t *image,
    int width,
    int height,
    const hough_line_t *lines,
    size_t num_lines,
    uint8_t value
)
{
    if (image == NULL || lines == NULL || num_lines == 0) {
        return;
    }

    for (size_t i = 0; i < num_lines; i++) {

        float rho = lines[i].rho;
        float theta = lines[i].theta_rad;

        float c = cosf(theta);
        float s = sinf(theta);

        /* -------------------------------------------------
         * Caso 1: sin(theta) no es cero
         * rho = x*cos(theta) + y*sin(theta)
         *
         * y = (rho - x*cos(theta)) / sin(theta)
         * ------------------------------------------------- */

        if (fabsf(s) > 0.001f) {

            for (int x = 0; x < width; x++) {

                float y_float =
                    (rho - (float)x * c) / s;

                int y = (int)lroundf(y_float);

                if (y >= 0 && y < height) {

                    image[y * width + x] = value;

                    if (y + 1 < height) {
                        image[(y + 1) * width + x] = value;
                    }

                    if (y - 1 >= 0) {
                        image[(y - 1) * width + x] = value;
                    }
                }
            }
        }

        /* -------------------------------------------------
         * Caso 2: sin(theta) ≈ 0
         *
         * En este caso:
         * x = rho / cos(theta)
         * ------------------------------------------------- */

        else {

            if (fabsf(c) < 0.001f) {
                continue;
            }

            int x = (int)lroundf(rho / c);

            if (x >= 0 && x < width) {

                for (int y = 0; y < height; y++) {

                    image[y * width + x] = 255;

                    if (x + 1 < width) {
                        image[y * width + (x + 1)] = 255;
                    }

                    if (x - 1 >= 0) {
                        image[y * width + (x - 1)] = 255;
                    }
                }
            }
        }
    }
}