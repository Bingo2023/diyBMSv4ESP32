/*
 * DIYBMS ESP32 - Controller-Firmware ueber den Browser aktualisieren (OTA)
 *
 * POST /ota?xss=<Schluessel>   Body: diybms_controller_firmware_*.bin
 * Ersetzt den einfachen /ota-Handler des Originals: Pruefung von Sitzung, Dateigroesse und
 * Image-Kopf, JSON-Antwort fuer die Fortschrittsanzeige, danach Neustart.
 *
 * Die neue Firmware wird in die freie OTA-Partition (app0/app1) geschrieben.
 * esp_ota_end() prueft das Image (Kopf, Pruefsumme, SHA256). Nur wenn alles stimmt,
 * wird die neue Partition als Startpartition gesetzt und der ESP32 startet neu.
 * Bei einem Uebertragungs- oder Pruefungsfehler bleibt die bisherige Firmware aktiv.
 * Einstellungen (NVS), WLAN und das Dateisystem (Modul-Firmware) bleiben erhalten.
 */
#define USE_ESP_IDF_LOG 1
static constexpr const char *const TAG = "diybms-ota";

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include "esp32_ota.h"
#include "EmbeddedFiles_Defines.h"

extern char CookieValue[20 + 1];
extern void url_decode(char *encoded, char *decoded);
extern void suspendTasksDuringFirmwareUpdate();
extern void resumeTasksAfterFirmwareUpdateFailure();


static volatile bool ota_running = false;

// Standard: nichts blockiert das Update (wird vom Modul-Firmware-Update ueberschrieben)
__attribute__((weak)) const char *ESP32Ota_BlockedReason()
{
    return nullptr;
}

// Laufzeit, aktive Partition und Version - die Seite erkennt daran den Neustart
esp_err_t ESP32Ota_StatusHandler(httpd_req_t *req)
{
    char buf[160];
    const esp_partition_t *rp = esp_ota_get_running_partition();
    int n = snprintf(buf, sizeof(buf), "{\"uptime\":%lu,\"part\":\"%s\",\"ver\":\"%s\"}",
                     (unsigned long)(esp_timer_get_time() / 1000000ULL), rp ? rp->label : "?", GIT_VERSION);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t ota_reply(httpd_req_t *req, bool ok, const char *msg, size_t bodyRead)
{
    // restlichen Body verwerfen, damit der Browser die Antwort sicher erhaelt
    char tmp[256];
    size_t left = req->content_len > bodyRead ? req->content_len - bodyRead : 0;
    int timeouts = 0;
    while (left > 0)
    {
        int r = httpd_req_recv(req, tmp, left < sizeof(tmp) ? left : sizeof(tmp));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
            continue;
        if (r <= 0)
            break;
        left -= r;
    }

    char buf[220];
    JsonDocument d;
    d["ok"] = ok ? 1 : 0;
    d["message"] = msg;
    size_t n = serializeJson(d, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, buf, n);
}

// Direkt im aufrufenden Handler neu starten (nicht ueber eine eigene Aufgabe,
// die evtl. nicht mehr drankommt)
void ESP32Ota_RestartAfterReply(const char *reason)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "Neustart (%s)", reason);
    esp_restart();
}

esp_err_t ESP32Ota_UploadHandler(httpd_req_t *req)
{
    // Sitzung pruefen: Cookie + Schluessel im URL (kann $ # @ enthalten -> dekodieren)
    char cookie[sizeof(CookieValue) + 2] = {0};
    size_t cookielen = sizeof(cookie);
    char query[160] = {0};
    char enc[3 * sizeof(CookieValue) + 4] = {0};
    char xss[3 * sizeof(CookieValue) + 4] = {0};
    if (httpd_req_get_cookie_val(req, "DIYBMS", cookie, &cookielen) != ESP_OK ||
        strncmp(cookie, CookieValue, sizeof(CookieValue)) != 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "xss", enc, sizeof(enc)) != ESP_OK)
    {
        return ota_reply(req, false, "Seite ist nicht mehr aktuell (Controller wurde neu gestartet) - Seite neu laden (F5), Datei erneut waehlen", 0);
    }
    url_decode(enc, xss);
    if (strncmp(xss, CookieValue, sizeof(CookieValue)) != 0)
        return ota_reply(req, false, "Seite ist nicht mehr aktuell (Controller wurde neu gestartet) - Seite neu laden (F5), Datei erneut waehlen", 0);

    const char *blocked = ESP32Ota_BlockedReason();
    if (blocked != nullptr)
        return ota_reply(req, false, blocked, 0);
    if (ota_running)
        return ota_reply(req, false, "Es laeuft bereits ein Controller-Update", 0);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr)
        return ota_reply(req, false, "Keine OTA-Partition gefunden", 0);

    size_t len = req->content_len;
    if (len < 100000 || len > target->size)
    {
        char m[120];
        snprintf(m, sizeof(m), "Dateigroesse %u Byte passt nicht (max. %u) - diybms_controller_firmware_*.bin verwenden",
                 (unsigned)len, (unsigned)target->size);
        return ota_reply(req, false, m, 0);
    }

    ota_running = true;
    ESP_LOGI(TAG, "OTA: %u Byte von %s nach %s", (unsigned)len, running ? running->label : "?", target->label);

    const size_t CHUNK = 4096;
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (buf == nullptr)
    {
        ota_running = false;
        return ota_reply(req, false, "Kein Speicher frei", 0);
    }

    esp_ota_handle_t h = 0;
    bool begun = false;
    bool suspended = false;
    size_t got = 0;
    int timeouts = 0;
    const char *err = nullptr;

    while (got < len)
    {
        int r = httpd_req_recv(req, (char *)buf, (len - got) < CHUNK ? (len - got) : CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10)
            continue;
        if (r <= 0)
        {
            err = "Uebertragung abgebrochen";
            break;
        }
        timeouts = 0;

        if (!begun)
        {
            // ESP32-Image beginnt mit 0xE9
            if (buf[0] != 0xE9)
            {
                err = "Keine ESP32-Firmware (diybms_controller_firmware_*.bin verwenden, nicht das Komplettimage)";
                got += r;
                break;
            }
            // Nicht die ganze Partition vorab loeschen (1,7 MB = viele Sekunden, in denen WLAN und
            // Weboberflaeche stehen), sondern jeweils kurz vor dem Schreiben
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
            {
                err = "OTA-Start fehlgeschlagen";
                got += r;
                break;
            }
            begun = true;
            // wie beim /ota des Originals: Anzeige, SD-Log, RS485/CAN und Modulabfrage anhalten
            suspendTasksDuringFirmwareUpdate();
            suspended = true;
        }

        if (esp_ota_write(h, buf, r) != ESP_OK)
        {
            err = "Schreiben in den Flash fehlgeschlagen";
            got += r;
            break;
        }
        got += r;
        // anderen Tasks (WLAN, Anzeige, Modul-Kommunikation) Zeit geben
        vTaskDelay(1);
    }
    free(buf);

    if (err == nullptr && begun)
    {
        esp_err_t e = esp_ota_end(h);
        begun = false;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED)
            err = "Pruefung der Firmware fehlgeschlagen (Datei beschaedigt?) - alte Firmware bleibt aktiv";
        else if (e != ESP_OK)
            err = "OTA-Abschluss fehlgeschlagen - alte Firmware bleibt aktiv";
        else if (esp_ota_set_boot_partition(target) != ESP_OK)
            err = "Startpartition konnte nicht gesetzt werden - alte Firmware bleibt aktiv";
    }
    if (begun)
        esp_ota_abort(h);

    if (err != nullptr)
    {
        ota_running = false;
        if (suspended)
            resumeTasksAfterFirmwareUpdateFailure();
        ESP_LOGE(TAG, "OTA: %s", err);
        return ota_reply(req, false, err, got);
    }

    char m[120];
    snprintf(m, sizeof(m), "Firmware geschrieben (%u Byte, %s). Controller startet neu ...", (unsigned)len, target->label);
    ESP_LOGI(TAG, "%s", m);
    ota_reply(req, true, m, got);
    ESP32Ota_RestartAfterReply("neue Firmware");
}
