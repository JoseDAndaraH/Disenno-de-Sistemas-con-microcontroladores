#include "wifi_sta.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"


static const char *TAG = "WIFI";


/* ============================================================
 * BITS DE EVENTOS
 * ============================================================ */

#define WIFI_READY_BIT  BIT0
#define WIFI_FAIL_BIT   BIT1

#define WIFI_MAX_RETRY  5


static EventGroupHandle_t s_wifi_event_group = NULL;

static int s_retry_count = 0;


/* ============================================================
 * HANDLER DE EVENTOS
 * ============================================================ */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_data;


    /* ========================================================
     * SOFTAP
     * ======================================================== */

#if WIFI_APP_MODE == WIFI_APP_MODE_AP

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_AP_START) {

        ESP_LOGI(
            TAG,
            "SoftAP iniciado"
        );

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_READY_BIT
        );
    }


    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STACONNECTED) {

        wifi_event_ap_staconnected_t *event =
            (wifi_event_ap_staconnected_t *)event_data;

        ESP_LOGI(
            TAG,
            "Cliente conectado: MAC %02X:%02X:%02X:%02X:%02X:%02X",
            event->mac[0],
            event->mac[1],
            event->mac[2],
            event->mac[3],
            event->mac[4],
            event->mac[5]
        );
    }


    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STADISCONNECTED) {

        wifi_event_ap_stadisconnected_t *event =
            (wifi_event_ap_stadisconnected_t *)event_data;

        ESP_LOGI(
            TAG,
            "Cliente desconectado: MAC %02X:%02X:%02X:%02X:%02X:%02X",
            event->mac[0],
            event->mac[1],
            event->mac[2],
            event->mac[3],
            event->mac[4],
            event->mac[5]
        );
    }


    /* ========================================================
     * STATION
     * ======================================================== */

#else

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(
            TAG,
            "STA iniciada"
        );

        ESP_ERROR_CHECK(
            esp_wifi_connect()
        );
    }


    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        if (s_retry_count < WIFI_MAX_RETRY) {

            ESP_ERROR_CHECK(
                esp_wifi_connect()
            );

            s_retry_count++;

            ESP_LOGW(
                TAG,
                "Reintentando Wi-Fi (%d/%d)",
                s_retry_count,
                WIFI_MAX_RETRY
            );
        }

        else {

            ESP_LOGE(
                TAG,
                "No se pudo conectar al AP"
            );

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAIL_BIT
            );
        }
    }


    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "IP obtenida: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        s_retry_count = 0;

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_READY_BIT
        );
    }

#endif
}


/* ============================================================
 * INICIO WIFI
 * ============================================================ */

esp_err_t wifi_start(void)
{
    /* --------------------------------------------------------
     * Event group
     * -------------------------------------------------------- */

    s_wifi_event_group =
        xEventGroupCreate();

    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }


    /* --------------------------------------------------------
     * TCP/IP
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    /* --------------------------------------------------------
     * Event loop
     * -------------------------------------------------------- */

    esp_err_t ret =
        esp_event_loop_create_default();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {

        return ret;
    }


    /* --------------------------------------------------------
     * Crear interfaz de red
     * -------------------------------------------------------- */

#if WIFI_APP_MODE == WIFI_APP_MODE_AP

    esp_netif_t *wifi_netif =
        esp_netif_create_default_wifi_ap();

    if (wifi_netif == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo crear interfaz SoftAP"
        );

        return ESP_FAIL;
    }

#else

    esp_netif_t *wifi_netif =
        esp_netif_create_default_wifi_sta();

    if (wifi_netif == NULL) {

        ESP_LOGE(
            TAG,
            "No se pudo crear interfaz STA"
        );

        return ESP_FAIL;
    }

#endif

    (void)wifi_netif;


    /* --------------------------------------------------------
     * Inicializar Wi-Fi
     * -------------------------------------------------------- */

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );


    /* --------------------------------------------------------
     * Eventos
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        )
    );


#if WIFI_APP_MODE == WIFI_APP_MODE_STA

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );

#endif


    /* ========================================================
     * CONFIGURAR SOFTAP
     * ======================================================== */

#if WIFI_APP_MODE == WIFI_APP_MODE_AP

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.ap.ssid,
        WIFI_AP_SSID,
        sizeof(wifi_config.ap.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.ap.password,
        WIFI_AP_PASSWORD,
        sizeof(wifi_config.ap.password) - 1
    );

    wifi_config.ap.ssid_len =
        strlen(WIFI_AP_SSID);

    wifi_config.ap.channel =
        WIFI_AP_CHANNEL;

    wifi_config.ap.max_connection =
        WIFI_AP_MAX_CONNECTIONS;

    wifi_config.ap.authmode =
        WIFI_AUTH_WPA2_PSK;

    wifi_config.ap.pmf_cfg.required =
        false;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_AP
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &wifi_config
        )
    );


    /* --------------------------------------------------------
     * Arrancar
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    ESP_LOGI(
        TAG,
        "================================="
    );

    ESP_LOGI(
        TAG,
        "Wi-Fi en modo ACCESS POINT"
    );

    ESP_LOGI(
        TAG,
        "SSID: %s",
        WIFI_AP_SSID
    );

    ESP_LOGI(
        TAG,
        "Canal: %d",
        WIFI_AP_CHANNEL
    );

    ESP_LOGI(
        TAG,
        "IP esperada: 192.168.4.1"
    );

    ESP_LOGI(
        TAG,
        "================================="
    );


    EventBits_t bits =
        xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_READY_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(5000)
        );

    if (bits & WIFI_READY_BIT) {

        ESP_LOGI(
            TAG,
            "SoftAP listo"
        );

        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "No se pudo iniciar SoftAP"
    );

    return ESP_FAIL;


/* ========================================================
 * CONFIGURAR STATION
 * ======================================================== */

#else

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        WIFI_STA_SSID,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        WIFI_STA_PASSWORD,
        sizeof(wifi_config.sta.password) - 1
    );

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    wifi_config.sta.pmf_cfg.capable =
        true;

    wifi_config.sta.pmf_cfg.required =
        false;


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );


    /* --------------------------------------------------------
     * Arrancar
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    ESP_LOGI(
        TAG,
        "Conectando a: %s...",
        WIFI_STA_SSID
    );


    EventBits_t bits =
        xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_READY_BIT |
            WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000)
        );


    if (bits & WIFI_READY_BIT) {

        ESP_LOGI(
            TAG,
            "Wi-Fi conectado"
        );

        return ESP_OK;
    }


    ESP_LOGE(
        TAG,
        "Fallo de conexión Wi-Fi"
    );

    return ESP_FAIL;

#endif
}