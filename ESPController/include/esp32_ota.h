/*
 * DIYBMS ESP32 - Controller-Firmware ueber den Browser aktualisieren (OTA)
 *
 * POST /ota?xss=<Schluessel>    Body: diybms_controller_firmware_*.bin
 * GET  /api/otastatus           {"uptime":s,"part":"app0|app1","ver":"..."}
 */
#ifndef DIYBMS_ESP32_OTA_H
#define DIYBMS_ESP32_OTA_H

#include <esp_http_server.h>

esp_err_t ESP32Ota_UploadHandler(httpd_req_t *req);
esp_err_t ESP32Ota_StatusHandler(httpd_req_t *req);

// Kann von anderen Teilen ueberschrieben werden (z.B. Modul-Firmware-Update),
// um ein Controller-Update waehrend einer laufenden Aktion abzulehnen.
// Rueckgabe: nullptr = frei, sonst Meldung fuer den Browser.
const char *ESP32Ota_BlockedReason();

// Nach einer bereits gesendeten Antwort: kurz warten, damit der Browser sie sicher
// erhaelt, dann neu starten. Gemeinsam fuer Firmware-Update und "Restart Controller".
[[noreturn]] void ESP32Ota_RestartAfterReply(const char *reason);

#endif
