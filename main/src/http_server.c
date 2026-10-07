#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_server.h"

static const char *TAG = "HTTP_SERVER";

static httpd_handle_t server = NULL;


/*
 * Esta función está implementada en mipi_isp_dsi_main.c
 *
 * Espera un frame RGB565, lo codifica a JPEG y devuelve
 * el buffer JPEG listo para enviar.
 */
extern esp_err_t obtener_frame_jpeg(
    const uint8_t **out_buffer,
    size_t *out_len
);
extern esp_err_t obtener_frame_compare_jpeg(
    const uint8_t **out_buffer,
    size_t *out_len
);



static const char *INDEX_HTML =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>ESP32-P4 Sobel</title>"
"<style>"
"html,body{"
"margin:0;"
"padding:0;"
"width:100%;"
"height:100%;"
"background:#111;"
"display:flex;"
"justify-content:center;"
"align-items:center;"
"overflow:hidden;"
"}"
".viewer{"
"width:90vw;"
"max-width:1400px;"
"}"
".labels{"
"width:100%;"
"display:flex;"
"justify-content:space-around;"
"color:white;"
"font-family:Arial,sans-serif;"
"font-size:18px;"
"margin-bottom:8px;"
"}"
"img{"
"width:100%;"
"height:auto;"
"display:block;"
"}"
"</style>"
"</head>"
"<body>"
"<div class='viewer'>"
"<div class='labels'>"
"<span>ORIGINAL</span>"
"<span>SOBEL</span>"
"</div>"
"<img src='/compare'>"
"</div>"
"</body>"
"</html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * GET /api/status
 * ============================================================ */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const char *response =
        "{\"status\":\"ok\",\"camera\":\"OV5647\","
        "\"resolution\":\"800x800\"}";

    httpd_resp_set_type(
        req,
        "application/json"
    );

    return httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}

/* ============================================================
 * GET /snapshot
 * ============================================================ */

 static esp_err_t snapshot_handler(httpd_req_t *req)
{
    const uint8_t *jpeg_buffer = NULL;
    size_t jpeg_size = 0;

    ESP_LOGI(TAG, "Capturando snapshot...");

    esp_err_t ret = obtener_frame_jpeg(
        &jpeg_buffer,
        &jpeg_size
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error obteniendo snapshot: %s",
            esp_err_to_name(ret)
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Error capturando imagen"
        );

        return ret;
    }

    httpd_resp_set_type(req, "image/jpeg");

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store, no-cache, must-revalidate, max-age=0"
    );

    httpd_resp_set_hdr(
        req,
        "Pragma",
        "no-cache"
    );

    httpd_resp_set_hdr(
        req,
        "Content-Disposition",
        "inline; filename=\"snapshot.jpg\""
    );

    return httpd_resp_send(
        req,
        (const char *)jpeg_buffer,
        jpeg_size
    );
}


/* ============================================================
 * GET /stream
 * ============================================================ */

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    esp_err_t ret;

    /*
     * Tipo MIME del stream MJPEG.
     */
    ret = httpd_resp_set_type(
        req,
        "multipart/x-mixed-replace;boundary=frame"
    );

    if (ret != ESP_OK) {
        return ret;
    }

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-cache"
    );

    ESP_LOGI(
        TAG,
        "Cliente conectado al stream"
    );

    char part_header[128];

    while (true) {

        const uint8_t *jpeg_data = NULL;
        size_t jpeg_len = 0;

        /*
         * Obtener un frame JPEG.
         */
        ret = obtener_frame_jpeg(
            &jpeg_data,
            &jpeg_len
        );

        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Error obteniendo frame JPEG: %s",
                esp_err_to_name(ret)
            );

            break;
        }

        /*
         * Header de la parte MJPEG.
         */
        int header_len = snprintf(
            part_header,
            sizeof(part_header),

            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n"
            "\r\n",

            (unsigned)jpeg_len
        );

        if (header_len < 0 ||
            header_len >= (int)sizeof(part_header)) {

            ESP_LOGE(
                TAG,
                "Error construyendo header MJPEG"
            );

            break;
        }

        /*
         * Enviar header.
         */
        ret = httpd_resp_send_chunk(
            req,
            part_header,
            header_len
        );

        if (ret != ESP_OK) {
            break;
        }

        /*
         * Enviar JPEG.
         */
        ret = httpd_resp_send_chunk(
            req,
            (const char *)jpeg_data,
            jpeg_len
        );

        if (ret != ESP_OK) {
            break;
        }

        /*
         * Separador.
         */
        ret = httpd_resp_send_chunk(
            req,
            "\r\n",
            2
        );

        if (ret != ESP_OK) {
            break;
        }

        /*
         * Dar oportunidad al scheduler.
         */
        vTaskDelay(
            pdMS_TO_TICKS(1)
        );
    }

    ESP_LOGI(
        TAG,
        "Cliente desconectado"
    );

    return ESP_FAIL;
}


/*=============================================================
*GET/compare
*=============================================================*/

static esp_err_t compare_handler(httpd_req_t *req)
{
    const uint8_t *jpeg_buffer = NULL;
    size_t jpeg_size = 0;

    esp_err_t ret;

    ret = httpd_resp_set_type(
        req,
        "multipart/x-mixed-replace;boundary=frame"
    );

    if (ret != ESP_OK) {
        return ret;
    }

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-cache"
    );

    ESP_LOGI(
        TAG,
        "Cliente conectado al compare"
    );

    char part_header[128];

    while (true) {

        jpeg_buffer = NULL;
        jpeg_size = 0;

        ret = obtener_frame_compare_jpeg(
            &jpeg_buffer,
            &jpeg_size
        );

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Error obteniendo compare: %s",
                esp_err_to_name(ret)
            );
            break;
        }

        int header_len = snprintf(
            part_header,
            sizeof(part_header),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            (unsigned)jpeg_size
        );

        if (header_len < 0 ||
            header_len >= sizeof(part_header)) {

            break;
        }

        ret = httpd_resp_send_chunk(
            req,
            part_header,
            header_len
        );

        if (ret != ESP_OK) {
            break;
        }

        ret = httpd_resp_send_chunk(
            req,
            (const char *)jpeg_buffer,
            jpeg_size
        );

        if (ret != ESP_OK) {
            break;
        }

        ret = httpd_resp_send_chunk(
            req,
            "\r\n",
            2
        );

        if (ret != ESP_OK) {
            break;
        }

        vTaskDelay(
            pdMS_TO_TICKS(200)
        );
    }

    ESP_LOGI(
        TAG,
        "Cliente compare desconectado"
    );

    return ESP_FAIL;
}



/* ============================================================
 * Inicialización del servidor
 * ============================================================ */

esp_err_t start_web_server(void)
{
    if (server != NULL) {
        ESP_LOGW(
            TAG,
            "Servidor HTTP ya iniciado"
        );

        return ESP_OK;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    /*
     * El handler /stream permanece activo mientras
     * el navegador está conectado.
     */
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(
        &server,
        &config
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Error iniciando servidor HTTP: %s",
            esp_err_to_name(ret)
        );

        server = NULL;

        return ret;
    }


    /* --------------------------------------------------------
     * /api/status
     * -------------------------------------------------------- */

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(
        server,
        &status_uri
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error registrando /api/status"
        );

        return ret;
    }

    httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL,
};

ret = httpd_register_uri_handler(server, &index_uri);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error registrando /");
    return ret;
}


    /*---------------------------------------------------------
    * /snapshot
    *----------------------------------------------------------*/

    httpd_uri_t snapshot_uri = {
    .uri       = "/snapshot",
    .method    = HTTP_GET,
    .handler   = snapshot_handler,
    .user_ctx  = NULL
};

ret = httpd_register_uri_handler(
    server,
    &snapshot_uri
);

if (ret != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Error registrando /snapshot"
    );
    return ret;
}

    


    /* --------------------------------------------------------
     * /stream
     * -------------------------------------------------------- */

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(
        server,
        &stream_uri
    );

    

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error registrando /stream"
        );

        return ret;
    }

    /*-----------------------------------------------------
    * /compare
    *-----------------------------------------------------*/
    
    httpd_uri_t compare_uri = {
    .uri = "/compare",
    .method = HTTP_GET,
    .handler = compare_handler,
    .user_ctx = NULL,
};

ret = httpd_register_uri_handler(
    server,
    &compare_uri
);

if (ret != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Error registrando /compare"
    );

    return ret;
}

    ESP_LOGI(
        TAG,
        "Servidor HTTP iniciado"
    );

    ESP_LOGI(
        TAG,
        "  /api/status"
    );

    ESP_LOGI(
        TAG,
        "  /stream"
    );

    ESP_LOGI(
    TAG,
    "  /snapshot"
    );

    ESP_LOGI(
    TAG,
    "  /compare"
    );

    return ESP_OK;
}
