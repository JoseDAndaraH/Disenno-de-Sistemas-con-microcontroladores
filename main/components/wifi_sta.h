#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
 * MODO DE FUNCIONAMIENTO
 * ============================================================ */

/*
 * WIFI_APP_MODE_AP
 *     El ESP32 crea su propia red Wi-Fi.
 *
 * WIFI_APP_MODE_STA
 *     El ESP32 se conecta a un router/AP externo.
 */

#define WIFI_APP_MODE_AP   0
#define WIFI_APP_MODE_STA  1


/*
 * ============================================================
 * IMPORTANTE:
 *
 * Seleccionar aquí el modo que queremos utilizar.
 *
 * AP  -> el rover crea su propia red
 * STA -> se conecta a una red Wi-Fi existente
 * ============================================================
 */

#define WIFI_APP_MODE WIFI_APP_MODE_AP


/* ============================================================
 * CONFIGURACIÓN ACCESS POINT
 * ============================================================ */

#define WIFI_AP_SSID            "ESP32p4jose"
#define WIFI_AP_PASSWORD        "TU_PASSWORD_AP"
#define WIFI_AP_CHANNEL         6
#define WIFI_AP_MAX_CONNECTIONS 2


/* ============================================================
 * CONFIGURACIÓN STATION
 * ============================================================ */

/*
 * Estas credenciales solamente se utilizan cuando:
 *
 * #define WIFI_APP_MODE WIFI_APP_MODE_STA
 */

#define WIFI_STA_SSID           "TU_SSID"
#define WIFI_STA_PASSWORD       "TU_PASSWORD"


/* ============================================================
 * API
 * ============================================================ */

/*
 * Inicializa el Wi-Fi según WIFI_APP_MODE.
 *
 * AP:
 *     crea la red ROVER_P4
 *
 * STA:
 *     se conecta a WIFI_STA_SSID
 */
esp_err_t wifi_start(void);


#ifdef __cplusplus
}
#endif