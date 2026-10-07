/*
 * ESP32-P4
 * OV5647
 *
 * Pipeline:
 *
 * OV5647
 *   ↓
 * MIPI CSI
 *   ↓
 * ISP + Demosaic
 *   ↓
 * RGB565
 *   ↓
 * PPA → GRAY8 400x400
 *   ↓
 * Gaussian 3x3
 *   ↓
 * Sobel
 *   ↓
 * ┌───────────────┐
 * │               │
 * ↓               ↓
 * JPEG          Hough
 * │               │
 * ↓               ↓
 * HTTP          líneas
 * MJPEG
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "hal/cache_hal.h"
#include "hal/cache_ll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"

#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"

#include "ov5647.h"

#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"

#include "esp_ldo_regulator.h"

#include "example_config.h"

#include "jpeg_encoder.h"
#include "http_server.h"
#include "wifi_sta.h"
#include "ppa_gray.h"
#include "sobel_gray.h"
#include "hough_lines.h"
#include "esp_timer.h"

/* ============================================================
 * CONFIGURACION
 * ============================================================ */

static const char *TAG = "camera_800x800";

#define CAMERA_WIDTH   800
#define CAMERA_HEIGHT  800
#define CAMERA_BPP     2       /* RGB565 = 2 bytes/pixel */

#define OUT_WIDTH      400
#define OUT_HEIGHT     400

#define COMPARE_WIDTH  (OUT_WIDTH * 2)
#define COMPARE_HEIGHT OUT_HEIGHT

#define JPEG_QUALITY            80
#define COMPARE_JPEG_QUALITY    80


/*
 * Umbral del Sobel.
 *
 * -1     = magnitud continua de gradiente 0..255
 * 0..255 = salida binaria
 */
#define SOBEL_THRESHOLD -1


/* ============================================================
 * HOUGH
 * ============================================================ */

#define HOUGH_THETA_STEPS       180
#define HOUGH_EDGE_THRESHOLD    100
#define HOUGH_MAX_LINES         5

#define HOUGH_RHO_SUPPRESSION   10
#define HOUGH_THETA_SUPPRESSION 4


/* ============================================================
 * OV5647 SCCB
 * ============================================================ */

#ifndef OV5647_SCCB_ADDR
#define OV5647_SCCB_ADDR 0x36
#endif


/* ============================================================
 * LDO MIPI
 * ============================================================ */

#ifndef CONFIG_EXAMPLE_USED_LDO_CHAN_ID
#define CONFIG_EXAMPLE_USED_LDO_CHAN_ID 3
#endif

#ifndef CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV
#define CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV 2500
#endif


/* ============================================================
 * FRAME BUFFER
 * ============================================================ */

static void *s_frame_buffer = NULL;
static size_t s_frame_buffer_size = 0;

static uint8_t *s_compare_buffer = NULL;
static size_t s_compare_buffer_size = 0;

static uint8_t *s_upscaled_buffer = NULL;
static size_t s_upscaled_buffer_size = 0;


/*
 * Buffers de procesamiento:
 *
 * GRAY      400x400
 * Gaussian  400x400
 * Sobel     400x400
 * Upscaled  800x800
 * Compare   800x400
 */

static uint8_t *s_sobel_buffer = NULL;
static size_t s_sobel_buffer_size = 0;

static uint8_t *s_gaussian_buffer = NULL;
static size_t s_gaussian_buffer_size = 0;


/* ============================================================
 * SEMAFORO DE FRAME
 * ============================================================ */

static SemaphoreHandle_t s_frame_ready_sem = NULL;


/* ============================================================
 * CONTEXTO CALLBACK
 * ============================================================ */

typedef struct {
    esp_cam_ctlr_trans_t *trans;
} cam_ctlr_ctx_t;

static cam_ctlr_ctx_t s_ctlr_ctx;


/* ============================================================
 * CALLBACK: NUEVO BUFFER
 * ============================================================ */

static bool s_camera_get_new_vb(
    esp_cam_ctlr_handle_t handle,
    esp_cam_ctlr_trans_t *trans,
    void *user_data
)
{
    (void)handle;

    cam_ctlr_ctx_t *ctx =
        (cam_ctlr_ctx_t *)user_data;

    trans->buffer =
        ctx->trans->buffer;

    trans->buflen =
        ctx->trans->buflen;

    return false;
}


/* ============================================================
 * CALLBACK: FRAME TERMINADO
 * ============================================================ */

static bool s_camera_get_finished_trans(
    esp_cam_ctlr_handle_t handle,
    esp_cam_ctlr_trans_t *trans,
    void *user_data
)
{
    (void)handle;
    (void)trans;
    (void)user_data;

    BaseType_t woken = pdFALSE;

    xSemaphoreGiveFromISR(
        s_frame_ready_sem,
        &woken
    );

    if (woken) {
        portYIELD_FROM_ISR();
    }

    return false;
}


/* ============================================================
 * OV5647 EXPOSICION MANUAL
 * ============================================================ */

#define OV5647_REG_AEC_MANUAL     0x3503

#define OV5647_REG_EXPOSURE_HI    0x3500
#define OV5647_REG_EXPOSURE_MID   0x3501
#define OV5647_REG_EXPOSURE_LO    0x3502

#define OV5647_REG_GAIN_HI        0x350A
#define OV5647_REG_GAIN_LO        0x350B

#define OV5647_REG_GROUP_HOLD     0x3212

#define OV5647_REG_VTS_HI         0x380E
#define OV5647_REG_VTS_LO         0x380F


static esp_err_t ov5647_read_reg(
    esp_sccb_io_handle_t io,
    uint16_t reg,
    uint8_t *out_val
)
{
    return esp_sccb_transmit_receive_reg_a16v8(
        io,
        reg,
        out_val
    );
}


static esp_err_t ov5647_read_vts(
    esp_sccb_io_handle_t io,
    uint16_t *out_vts
)
{
    uint8_t hi = 0;
    uint8_t lo = 0;

    esp_err_t err;

    err = ov5647_read_reg(
        io,
        OV5647_REG_VTS_HI,
        &hi
    );

    if (err != ESP_OK) {
        return err;
    }

    err = ov5647_read_reg(
        io,
        OV5647_REG_VTS_LO,
        &lo
    );

    if (err != ESP_OK) {
        return err;
    }

    *out_vts =
        ((uint16_t)hi << 8) | lo;

    return ESP_OK;
}


static esp_err_t ov5647_enable_manual_ae(
    esp_sccb_io_handle_t io
)
{
    return esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_AEC_MANUAL,
        0x03
    );
}


static esp_err_t ov5647_set_exposure_gain(
    esp_sccb_io_handle_t io,
    uint32_t exposure_lines,
    uint16_t gain
)
{
    uint32_t exposure_x16 =
        exposure_lines << 4;

    uint16_t current_vts = 0;

    ov5647_read_vts(
        io,
        &current_vts
    );

    uint32_t desired_vts =
        exposure_lines + 4;

    bool raise_vts =
        desired_vts > current_vts;

    esp_err_t err = ESP_OK;


    if (raise_vts) {

        ESP_LOGI(
            TAG,
            "Subiendo VTS %u -> %" PRIu32,
            current_vts,
            desired_vts
        );

        err |= esp_sccb_transmit_reg_a16v8(
            io,
            OV5647_REG_VTS_HI,
            (desired_vts >> 8) & 0xFF
        );

        err |= esp_sccb_transmit_reg_a16v8(
            io,
            OV5647_REG_VTS_LO,
            desired_vts & 0xFF
        );
    }


    err |= esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_EXPOSURE_HI,
        (exposure_x16 >> 16) & 0x0F
    );

    err |= esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_EXPOSURE_MID,
        (exposure_x16 >> 8) & 0xFF
    );

    err |= esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_EXPOSURE_LO,
        exposure_x16 & 0xFF
    );


    err |= esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_GAIN_HI,
        (gain >> 8) & 0x03
    );

    err |= esp_sccb_transmit_reg_a16v8(
        io,
        OV5647_REG_GAIN_LO,
        gain & 0xFF
    );


    return err;
}


/* ============================================================
 * DEBUG EXPOSICION
 * ============================================================ */

static void ov5647_log_exposure_gain_regs(
    esp_sccb_io_handle_t io
)
{
    uint8_t manual = 0;

    uint8_t exp_hi = 0;
    uint8_t exp_mid = 0;
    uint8_t exp_lo = 0;

    uint8_t gain_hi = 0;
    uint8_t gain_lo = 0;

    uint16_t vts = 0;

    esp_err_t err = ESP_OK;


    err |= ov5647_read_reg(
        io,
        OV5647_REG_AEC_MANUAL,
        &manual
    );

    err |= ov5647_read_reg(
        io,
        OV5647_REG_EXPOSURE_HI,
        &exp_hi
    );

    err |= ov5647_read_reg(
        io,
        OV5647_REG_EXPOSURE_MID,
        &exp_mid
    );

    err |= ov5647_read_reg(
        io,
        OV5647_REG_EXPOSURE_LO,
        &exp_lo
    );

    err |= ov5647_read_reg(
        io,
        OV5647_REG_GAIN_HI,
        &gain_hi
    );

    err |= ov5647_read_reg(
        io,
        OV5647_REG_GAIN_LO,
        &gain_lo
    );

    err |= ov5647_read_vts(
        io,
        &vts
    );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "No se pudieron leer registros AE"
        );

        return;
    }


    uint32_t exposure_x16 =
        ((uint32_t)exp_hi << 16) |
        ((uint32_t)exp_mid << 8) |
        exp_lo;

    uint16_t gain =
        ((uint16_t)gain_hi << 8) |
        gain_lo;

    uint32_t exposure_lines =
        exposure_x16 >> 4;


    ESP_LOGI(
        TAG,
        "readback: 0x3503=0x%02X exposure=%" PRIu32
        " lineas gain=%u vts=%u",
        manual,
        exposure_lines,
        gain,
        vts
    );
}


/* ============================================================
 * PUENTE HTTP → JPEG
 *
 * Estas son las funciones que busca http_server.c.
 *
 * obtener_frame_jpeg():
 *   - GRAY 400x400
 *   - Gaussian 3x3
 *   - Sobel 400x400
 *   - PPA 400x400 -> 800x800
 *   - JPEG 800x800
 *
 * obtener_frame_compare_jpeg():
 *   - GRAY original 400x400 a la izquierda
 *   - Sobel 400x400 a la derecha
 *   - Hough sobre el Sobel
 *   - JPEG 800x400
 * ============================================================ */


/* ============================================================
 * FRAME JPEG NORMAL
 * ============================================================ */

esp_err_t obtener_frame_jpeg(
    const uint8_t **out_buffer,
    size_t *out_len
)
{
    if (out_buffer == NULL ||
        out_len == NULL) {

        return ESP_ERR_INVALID_ARG;
    }


    if (xSemaphoreTake(
            s_frame_ready_sem,
            pdMS_TO_TICKS(1000)
        ) != pdTRUE) {

        ESP_LOGW(
            TAG,
            "Timeout esperando frame"
        );

        return ESP_ERR_TIMEOUT;
    }


    const uint8_t *gray_buffer = NULL;
    size_t gray_size = 0;


    esp_err_t ret =
        rgb565_to_gray(
            s_frame_buffer,
            s_frame_buffer_size,
            &gray_buffer,
            &gray_size
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "RGB565 -> GRAY8 fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    gaussian3x3_gray8(
        gray_buffer,
        s_gaussian_buffer,
        OUT_WIDTH,
        OUT_HEIGHT
    );


    sobel_gray8_threshold(
        s_gaussian_buffer,
        s_sobel_buffer,
        OUT_WIDTH,
        OUT_HEIGHT,
        SOBEL_THRESHOLD
    );


    /*
     * Esta función conserva la ruta anterior.
     *
     * Hough NO se ejecuta aquí.
     */
    ret = ppa_upscale_gray8(
        s_sobel_buffer,
        OUT_WIDTH,
        OUT_HEIGHT,
        s_upscaled_buffer,
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Upscale Sobel fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ret = encode_gray_to_jpeg(
        s_upscaled_buffer,
        s_upscaled_buffer_size,
        CAMERA_WIDTH,
        CAMERA_HEIGHT,
        COMPARE_JPEG_QUALITY,
        out_buffer,
        out_len
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Sobel -> JPEG fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    return ESP_OK;
}


/* ============================================================
 * FRAME COMPARE + HOUGH
 * ============================================================ */

esp_err_t obtener_frame_compare_jpeg(
    const uint8_t **out_buffer,
    size_t *out_len
)
{
    if (out_buffer == NULL ||
        out_len == NULL) {

        return ESP_ERR_INVALID_ARG;
    }


    /* ========================================================
     * Esperar nuevo frame
     * ======================================================== */

    if (xSemaphoreTake(
            s_frame_ready_sem,
            pdMS_TO_TICKS(1000)
        ) != pdTRUE) {

        ESP_LOGW(
            TAG,
            "Timeout esperando frame compare"
        );

        return ESP_ERR_TIMEOUT;
    }


    /*
     * Los tiempos empiezan DESPUÉS de obtener el frame.
     * Así no medimos el tiempo que el programa estuvo
     * esperando al semáforo.
     */
    int64_t t_total_start =
        esp_timer_get_time();

    int64_t t0;
    int64_t t1;


    /* ========================================================
     * RGB565 -> GRAY8
     * ======================================================== */

    const uint8_t *gray_buffer = NULL;
    size_t gray_size = 0;

    t0 = esp_timer_get_time();

    esp_err_t ret =
        rgb565_to_gray(
            s_frame_buffer,
            s_frame_buffer_size,
            &gray_buffer,
            &gray_size
        );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO RGB565->GRAY: %" PRId64 " us",
        t1 - t0
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "RGB565 -> GRAY8 compare fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* ========================================================
     * Gaussian
     * ======================================================== */

    t0 = esp_timer_get_time();

    gaussian3x3_gray8(
        gray_buffer,
        s_gaussian_buffer,
        OUT_WIDTH,
        OUT_HEIGHT
    );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO GAUSSIAN: %" PRId64 " us",
        t1 - t0
    );


    /* ========================================================
     * Sobel
     * ======================================================== */

    t0 = esp_timer_get_time();

    sobel_gray8_threshold(
        s_gaussian_buffer,
        s_sobel_buffer,
        OUT_WIDTH,
        OUT_HEIGHT,
        SOBEL_THRESHOLD
    );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO SOBEL: %" PRId64 " us",
        t1 - t0
    );


    /* ========================================================
     * Hough
     * ======================================================== */

    hough_line_t hough_lines[HOUGH_MAX_LINES];

    t0 = esp_timer_get_time();

    int num_lines =
        hough_detect_lines(
            s_sobel_buffer,
            hough_lines,
            HOUGH_MAX_LINES
        );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO HOUGH: %" PRId64 " us",
        t1 - t0
    );


    if (num_lines < 0) {

        ESP_LOGE(
            TAG,
            "Error ejecutando Hough"
        );

    } else {

        for (int i = 0;
             i < num_lines;
             i++) {

            ESP_LOGI(
                TAG,
                "Hough[%d]: rho=%.1f theta=%.1f° votes=%u",
                i,
                hough_lines[i].rho,
                hough_lines[i].theta_deg,
                hough_lines[i].votes
            );
        }
    }


    /* ========================================================
     * Dibujar líneas Hough
     * ======================================================== */

    if (num_lines > 0) {

        hough_draw_lines_gray8(
            s_sobel_buffer,
            OUT_WIDTH,
            OUT_HEIGHT,
            hough_lines,
            num_lines,
            255
        );
    }


    /* ========================================================
     * Construir imagen compare
     *
     * ORIGINAL | SOBEL + HOUGH
     *
     * 800x400 GRAY8
     * ======================================================== */

    t0 = esp_timer_get_time();

    for (int y = 0;
         y < OUT_HEIGHT;
         y++) {

        memcpy(
            &s_compare_buffer[
                y * COMPARE_WIDTH
            ],

            &gray_buffer[
                y * OUT_WIDTH
            ],

            OUT_WIDTH
        );


        memcpy(
            &s_compare_buffer[
                y * COMPARE_WIDTH +
                OUT_WIDTH
            ],

            &s_sobel_buffer[
                y * OUT_WIDTH
            ],

            OUT_WIDTH
        );
    }


    /* ========================================================
     * Cache
     * ======================================================== */

    ret =
        esp_cache_msync(
            s_compare_buffer,
            s_compare_buffer_size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M
        );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO COMPARE+MSYNC: %" PRId64 " us",
        t1 - t0
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "msync compare fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* ========================================================
     * JPEG
     * ======================================================== */

    t0 = esp_timer_get_time();

    ret =
        encode_gray_to_jpeg(
            s_compare_buffer,
            s_compare_buffer_size,
            COMPARE_WIDTH,
            COMPARE_HEIGHT,
            JPEG_QUALITY,
            out_buffer,
            out_len
        );

    t1 = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO JPEG: %" PRId64 " us",
        t1 - t0
    );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Compare -> JPEG fallo: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* ========================================================
     * Tiempo total de procesamiento
     * ======================================================== */

    int64_t t_total_end =
        esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "TIEMPO TOTAL PROCESAMIENTO: %" PRId64 " us",
        t_total_end - t_total_start
    );


    return ESP_OK;
}


/* ============================================================
 * MAIN
 * ============================================================ */

void app_main(void)
{
    esp_err_t ret;


    /* ========================================================
     * Wi-Fi
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "Inicializando Wi-Fi..."
    );

    ESP_ERROR_CHECK(
    wifi_start()
    );

    ESP_LOGI(
        TAG,
        "Wi-Fi listo"
    );


    /* ========================================================
     * Tamaño del frame RGB565
     * ======================================================== */

    s_frame_buffer_size =
        (size_t)CAMERA_WIDTH *
        (size_t)CAMERA_HEIGHT *
        CAMERA_BPP;


    ESP_LOGI(
        TAG,
        "Camera: %dx%d RGB565",
        CAMERA_WIDTH,
        CAMERA_HEIGHT
    );


    ESP_LOGI(
        TAG,
        "Frame buffer size: %zu bytes",
        s_frame_buffer_size
    );


    /* ========================================================
     * PPA
     * ======================================================== */

    ESP_ERROR_CHECK(
        init_ppa_gray(
            CAMERA_WIDTH,
            CAMERA_HEIGHT,
            OUT_WIDTH,
            OUT_HEIGHT
        )
    );


    /* ========================================================
     * Cache line
     * ======================================================== */

    uint32_t cache_line =
        cache_hal_get_cache_line_size(
            CACHE_LL_LEVEL_EXT_MEM,
            CACHE_TYPE_DATA
        );


    /* ========================================================
     * Buffers de procesamiento
     *
     * Gaussian: 400x400 GRAY8
     * Sobel:    400x400 GRAY8
     * Upscaled: 800x800 GRAY8
     * Compare:  800x400 GRAY8
     * ======================================================== */


    /* --------------------------------------------------------
     * Gaussian
     * -------------------------------------------------------- */

    s_gaussian_buffer_size =
        (size_t)OUT_WIDTH *
        (size_t)OUT_HEIGHT;


    s_gaussian_buffer =
        heap_caps_aligned_calloc(
            cache_line,
            1,
            s_gaussian_buffer_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_DMA
        );


    if (s_gaussian_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer Gaussian"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Gaussian buffer: %zu bytes @ %p",
        s_gaussian_buffer_size,
        s_gaussian_buffer
    );


    /* --------------------------------------------------------
     * Sobel
     * -------------------------------------------------------- */

    s_sobel_buffer_size =
        (size_t)OUT_WIDTH *
        (size_t)OUT_HEIGHT;


    s_sobel_buffer =
        heap_caps_aligned_alloc(
            64,
            s_sobel_buffer_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );


    if (s_sobel_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer Sobel"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Sobel buffer: %zu bytes @ %p",
        s_sobel_buffer_size,
        s_sobel_buffer
    );


    /* --------------------------------------------------------
     * Upscaled
     * -------------------------------------------------------- */

    s_upscaled_buffer_size =
        (size_t)CAMERA_WIDTH *
        (size_t)CAMERA_HEIGHT;


    s_upscaled_buffer =
        heap_caps_aligned_alloc(
            cache_line,
            s_upscaled_buffer_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT |
            MALLOC_CAP_DMA
        );


    if (s_upscaled_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer Sobel escalado"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Upscaled buffer: %zu bytes @ %p",
        s_upscaled_buffer_size,
        s_upscaled_buffer
    );


    /* --------------------------------------------------------
     * Compare
     * -------------------------------------------------------- */

    s_compare_buffer_size =
        (size_t)COMPARE_WIDTH *
        (size_t)COMPARE_HEIGHT;


    s_compare_buffer =
        heap_caps_aligned_alloc(
            cache_line,
            s_compare_buffer_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT |
            MALLOC_CAP_DMA
        );


    if (s_compare_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar buffer compare"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Compare buffer: %zu bytes @ %p",
        s_compare_buffer_size,
        s_compare_buffer
    );


    /* ========================================================
     * Inicializar Hough
     * ======================================================== */

    hough_config_t hough_config = {

        .width = OUT_WIDTH,

        .height = OUT_HEIGHT,

        .theta_steps =
            HOUGH_THETA_STEPS,

        .edge_threshold =
            HOUGH_EDGE_THRESHOLD,

        .max_lines =
            HOUGH_MAX_LINES,

        .rho_suppression =
            HOUGH_RHO_SUPPRESSION,

        .theta_suppression =
            HOUGH_THETA_SUPPRESSION,
    };


    if (hough_init(
            &hough_config
        ) != 0) {

        ESP_LOGE(
            TAG,
            "No se pudo inicializar Hough"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Hough inicializado: %dx%d, theta=%d",
        OUT_WIDTH,
        OUT_HEIGHT,
        HOUGH_THETA_STEPS
    );


    /* ========================================================
     * 1. LDO MIPI
     * ======================================================== */

    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;


    esp_ldo_channel_config_t ldo_cfg = {

        .chan_id =
            CONFIG_EXAMPLE_USED_LDO_CHAN_ID,

        .voltage_mv =
            CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
    };


    ESP_ERROR_CHECK(
        esp_ldo_acquire_channel(
            &ldo_cfg,
            &ldo_mipi_phy
        )
    );


    /* ========================================================
     * 2. SCCB / I2C
     * ======================================================== */

    i2c_master_bus_config_t i2c_bus_cfg = {

        .i2c_port = I2C_NUM_0,

        .sda_io_num =
            EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,

        .scl_io_num =
            EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,

        .clk_source =
            I2C_CLK_SRC_DEFAULT,

        .glitch_ignore_cnt = 7,

        .flags.enable_internal_pullup =
            true,
    };


    i2c_master_bus_handle_t i2c_bus = NULL;


    ESP_ERROR_CHECK(
        i2c_new_master_bus(
            &i2c_bus_cfg,
            &i2c_bus
        )
    );


    sccb_i2c_config_t sccb_cfg = {

        .scl_speed_hz = 100000,

        .device_address =
            OV5647_SCCB_ADDR,

        .addr_bits_width = 7,
    };


    esp_sccb_io_handle_t sccb_handle = NULL;


    ESP_ERROR_CHECK(
        sccb_new_i2c_io(
            i2c_bus,
            &sccb_cfg,
            &sccb_handle
        )
    );


    /* ========================================================
     * 3. OV5647
     * ======================================================== */

    esp_cam_sensor_config_t sensor_cfg = {

        .sccb_handle = sccb_handle,

        .reset_pin =
            GPIO_NUM_NC,

        .pwdn_pin =
            GPIO_NUM_NC,

        .xclk_pin =
            GPIO_NUM_NC,

        .sensor_port =
            ESP_CAM_SENSOR_MIPI_CSI,
    };


    esp_cam_sensor_device_t *sensor_dev =
        ov5647_detect(
            &sensor_cfg
        );


    if (sensor_dev == NULL) {

        ESP_LOGE(
            TAG,
            "OV5647 no detectado"
        );

        return;
    }


    /* ========================================================
     * 4. Buscar 800x800 RAW8
     * ======================================================== */

    esp_cam_sensor_format_array_t format_array = {
        0
    };


    ESP_ERROR_CHECK(
        esp_cam_sensor_query_format(
            sensor_dev,
            &format_array
        )
    );


    const esp_cam_sensor_format_t *chosen =
        NULL;


    for (size_t i = 0;
         i < format_array.count;
         ++i) {

        const esp_cam_sensor_format_t *f =
            &format_array.format_array[i];


        if (f->width == CAMERA_WIDTH &&
            f->height == CAMERA_HEIGHT &&
            f->format ==
                ESP_CAM_SENSOR_PIXFORMAT_RAW8) {

            chosen = f;

            break;
        }
    }


    if (chosen == NULL) {

        ESP_LOGE(
            TAG,
            "No existe RAW8 %dx%d",
            CAMERA_WIDTH,
            CAMERA_HEIGHT
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Formato elegido: %s "
        "(mipi_clk=%" PRIu32 " Hz, lanes=%d)",

        chosen->name ?
            chosen->name : "?",

        (uint32_t)
            chosen->mipi_info.mipi_clk,

        chosen->mipi_info.lane_num
    );


    ESP_ERROR_CHECK(
        esp_cam_sensor_set_format(
            sensor_dev,
            chosen
        )
    );


    /* ========================================================
     * 5. Exposición manual
     * ======================================================== */

    ESP_ERROR_CHECK(
        ov5647_enable_manual_ae(
            sccb_handle
        )
    );


    ret =
        ov5647_set_exposure_gain(
            sccb_handle,
            800,
            512
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "No se pudo configurar exposure/gain: %s",
            esp_err_to_name(ret)
        );
    }


    ov5647_log_exposure_gain_regs(
        sccb_handle
    );


    /* ========================================================
     * 6. Frame buffer RGB565
     * ======================================================== */

    s_frame_buffer =
        heap_caps_aligned_calloc(
            cache_line,
            1,
            s_frame_buffer_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_DMA
        );


    if (s_frame_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo reservar frame buffer"
        );

        return;
    }


    /* ========================================================
     * 7. ISP
     * ======================================================== */

    isp_proc_handle_t isp_proc =
        NULL;


    esp_isp_processor_cfg_t isp_config = {

        .clk_hz =
            80 * 1000 * 1000,

        .input_data_source =
            ISP_INPUT_DATA_SOURCE_CSI,

        .input_data_color_type =
            ISP_COLOR_RAW8,

        .output_data_color_type =
            ISP_COLOR_RGB565,

        .has_line_start_packet =
            false,

        .has_line_end_packet =
            false,

        .h_res =
            CAMERA_WIDTH,

        .v_res =
            CAMERA_HEIGHT,

        /*
         * Configuración comprobada
         * experimentalmente.
         */
        .bayer_order =
            COLOR_RAW_ELEMENT_ORDER_GBRG,
    };


    ESP_ERROR_CHECK(
        esp_isp_new_processor(
            &isp_config,
            &isp_proc
        )
    );


    ESP_ERROR_CHECK(
        esp_isp_enable(
            isp_proc
        )
    );


    esp_isp_demosaic_config_t demosaic_cfg = {

        .grad_ratio.integer = 2,

        .grad_ratio.decimal = 5,
    };


    ESP_ERROR_CHECK(
        esp_isp_demosaic_configure(
            isp_proc,
            &demosaic_cfg
        )
    );


    ESP_ERROR_CHECK(
        esp_isp_demosaic_enable(
            isp_proc
        )
    );


    /* ========================================================
     * 8. Inicializar JPEG
     * ======================================================== */

    ESP_ERROR_CHECK(
        init_jpeg_encoder(
            CAMERA_WIDTH,
            CAMERA_HEIGHT
        )
    );


    /* ========================================================
     * 9. CSI
     * ======================================================== */

    esp_cam_ctlr_csi_config_t csi_config = {

        .ctlr_id = 0,

        .h_res =
            CAMERA_WIDTH,

        .v_res =
            CAMERA_HEIGHT,

        .lane_bit_rate_mbps =
            (uint32_t)(
                chosen->mipi_info.mipi_clk /
                1000000u
            ),

        .input_data_color_type =
            CAM_CTLR_COLOR_RAW8,

        /*
         * CSI hace passthrough RAW8.
         * ISP hace RAW8 -> RGB565.
         */
        .output_data_color_type =
            CAM_CTLR_COLOR_RAW8,

        .data_lane_num =
            chosen->mipi_info.lane_num,

        .byte_swap_en =
            false,

        .queue_items =
            1,
    };


    esp_cam_ctlr_handle_t cam_handle =
        NULL;


    ret =
        esp_cam_new_csi_ctlr(
            &csi_config,
            &cam_handle
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "CSI init fallo: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    /* ========================================================
     * 10. Transacción
     * ======================================================== */

    esp_cam_ctlr_trans_t new_trans = {

        .buffer =
            s_frame_buffer,

        .buflen =
            s_frame_buffer_size,
    };


    s_ctlr_ctx.trans =
        &new_trans;


    esp_cam_ctlr_evt_cbs_t cbs = {

        .on_get_new_trans =
            s_camera_get_new_vb,

        .on_trans_finished =
            s_camera_get_finished_trans,
    };


    ret =
        esp_cam_ctlr_register_event_callbacks(
            cam_handle,
            &cbs,
            &s_ctlr_ctx
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Error registrando callbacks: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    /* ========================================================
     * 11. Semáforo
     * ======================================================== */

    s_frame_ready_sem =
        xSemaphoreCreateBinary();


    if (s_frame_ready_sem == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo crear semáforo"
        );

        return;
    }


    /* ========================================================
     * 12. Enable + Start CSI
     * ======================================================== */

    ESP_ERROR_CHECK(
        esp_cam_ctlr_enable(
            cam_handle
        )
    );


    ESP_ERROR_CHECK(
        esp_cam_ctlr_start(
            cam_handle
        )
    );


    /* ========================================================
     * 13. Stream ON sensor
     * ======================================================== */

    int stream_on = 1;


    ret =
        esp_cam_sensor_ioctl(
            sensor_dev,
            ESP_CAM_SENSOR_IOC_S_STREAM,
            &stream_on
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "sensor stream-on fallo: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Pipeline iniciado:"
        " OV5647 -> CSI RAW8 -> ISP -> RGB565"
        " -> PPA GRAY8 -> Gaussian -> Sobel -> Hough -> JPEG"
    );


    /* ========================================================
     * 14. Armamos captura continua
     * ======================================================== */

    ret =
        esp_cam_ctlr_receive(
            cam_handle,
            &new_trans,
            pdMS_TO_TICKS(1000)
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "receive inicial fallo: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    /* ========================================================
     * 15. HTTP
     * ======================================================== */

    ESP_ERROR_CHECK(
        start_web_server()
    );


    ESP_LOGI(
        TAG,
        "Sistema listo."
    );


    ESP_LOGI(
        TAG,
        "Abrir / desde el navegador"
    );


    /* ========================================================
     * 16. Main idle
     * ======================================================== */

    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}