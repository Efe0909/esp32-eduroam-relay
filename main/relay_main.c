/* ── Feature flags ────────────────────────────────────────────────────── */
/* Uncomment USE_ENTERPRISE to connect upstream via WPA2-Enterprise (eduroam).
 * Leave commented to use a plain WPA2-PSK hotspot for testing. */
#define USE_ENTERPRISE

/* Uncomment USE_MAC_FILTER to enforce a MAC address whitelist on the AP.
 * Any device not in ALLOWED_MACS will be deauthenticated immediately. */
#define USE_MAC_FILTER

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#ifdef USE_ENTERPRISE
#include "esp_eap_client.h"
#endif
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"
#include "lwip/stats.h"

/* ── Upstream: plain PSK fallback (USE_ENTERPRISE not defined) ────────── */
/* Used only when USE_ENTERPRISE is commented out — handy for local testing
 * with a regular WPA2 hotspot before deploying to eduroam. */
#ifndef USE_ENTERPRISE
#define STA_SSID     "YOUR_HOTSPOT_SSID"
#define STA_PASSWORD "YOUR_HOTSPOT_PASSWORD"
#endif

/* ── Private AP ───────────────────────────────────────────────────────── */
/* The AP that clients (e.g. your printer) connect to.
 * AP_HIDDEN=1 hides the SSID from scans; toggle at runtime with `ssid show`. */
#define AP_SSID     "BambuBridge"
#define AP_PASSWORD "YOUR_AP_PASSWORD"
#define AP_CHANNEL  6
#define AP_HIDDEN   1
#define AP_MAX_CONN 4

/* Maximum STA reconnect attempts before giving up and logging an error.
 * The AP stays up even if upstream fails, so clients can still join the AP;
 * they just won't have internet until the STA reconnects. */
#define MAX_RETRY    10

/* DNS upstream — all queries from AP clients are proxied here.
 * Using 8.8.8.8 because advertising the eduroam gateway as DNS
 * via DHCP was unreliable; a dedicated UDP forwarder task is more robust. */
#define DNS_UPSTREAM "8.8.8.8"

/* ── NVS credential storage ───────────────────────────────────────────── */
/* EAP credentials are stored in NVS (non-volatile storage) rather than
 * compiled into the binary. On first boot with no credentials, the firmware
 * drops to a serial REPL so you can provision them with `provision <id> <user> <pass>`. */
#define NVS_NS    "relay"
#define NVS_IDENT "identity"
#define NVS_USER  "username"
#define NVS_PASS  "password"

/* ── MAC whitelist ────────────────────────────────────────────────────── */
/* Add one entry per device you want to allow on the AP.
 * Devices not listed here are deauthenticated on connect.
 * Find a device's MAC by running `status` after it connects and gets blocked. */
#ifdef USE_MAC_FILTER
static const uint8_t ALLOWED_MACS[][6] = {
    { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 },  /* your printer */
    { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02 },  /* your phone   */
};
static const int ALLOWED_COUNT = sizeof(ALLOWED_MACS) / 6;
#endif

static const char       *TAG        = "relay";
static EventGroupHandle_t s_wifi_eg;
#define STA_CONNECTED_BIT BIT0
#define STA_FAIL_BIT      BIT1

static int          s_retries   = 0;
static bool         s_monitor   = false;
static esp_netif_t *g_sta_netif = NULL;
static esp_netif_t *g_ap_netif  = NULL;

/* ── NVS credential helpers ───────────────────────────────────────────── */
#ifdef USE_ENTERPRISE
static bool creds_load(char *ident, size_t il,
                       char *user,  size_t ul,
                       char *pass,  size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, NVS_IDENT, ident, &il) == ESP_OK) &&
              (nvs_get_str(h, NVS_USER,  user,  &ul) == ESP_OK) &&
              (nvs_get_str(h, NVS_PASS,  pass,  &pl) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void creds_save(const char *ident, const char *user, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    nvs_set_str(h, NVS_IDENT, ident);
    nvs_set_str(h, NVS_USER,  user);
    nvs_set_str(h, NVS_PASS,  pass);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}
#endif /* USE_ENTERPRISE */

/* ── DNS forwarder ────────────────────────────────────────────────────── */
/* Logs the queried domain name from a raw DNS packet.
 * DNS question section starts at byte 12; each label is a length-prefixed string.
 * Pointers (0xC0 prefix) are compression; we stop there since we only need the name. */
static bool s_dns_log = false;

static void dns_log_query(const uint8_t *buf, int n)
{
    if (n < 13) return;
    char name[128]; int ni = 0, pos = 12;
    while (pos < n && buf[pos] != 0 && ni < (int)sizeof(name) - 2) {
        uint8_t len = buf[pos++];
        if (len >= 0xC0) break;
        if (ni) name[ni++] = '.';
        for (uint8_t i = 0; i < len && pos < n && ni < (int)sizeof(name) - 1; i++)
            name[ni++] = buf[pos++];
    }
    name[ni] = '\0';
    if (ni) ESP_LOGI("dns", "%s", name);
}

/* UDP DNS proxy: receives queries from AP clients on port 53, forwards them
 * to DNS_UPSTREAM, and relays the response back.
 * Opens a new ephemeral socket per query to avoid state management. */
static void dns_forwarder_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct sockaddr_in upstream = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_pton(AF_INET, DNS_UPSTREAM, &upstream.sin_addr);

    int srv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bind(srv, (struct sockaddr *)&local, sizeof(local));
    ESP_LOGI(TAG, "DNS forwarder -> %s", DNS_UPSTREAM);

    struct sockaddr_in client;
    socklen_t clen = sizeof(client);
    struct timeval tv = { .tv_sec = 3 };

    while (1) {
        int n = recvfrom(srv, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n <= 0) continue;
        if (s_dns_log) dns_log_query(buf, n);
        int fwd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        setsockopt(fwd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sendto(fwd, buf, n, 0, (struct sockaddr *)&upstream, sizeof(upstream));
        int r = recv(fwd, buf, sizeof(buf), 0);
        close(fwd);
        if (r > 0) sendto(srv, buf, r, 0, (struct sockaddr *)&client, clen);
    }
}

/* ── Bandwidth monitor task ───────────────────────────────────────────── */
/* Polls lwIP IP packet counters once per second and estimates throughput.
 * Uses a fixed 1400-byte average packet size — rough, but good enough for
 * a sanity check. Requires CONFIG_LWIP_STATS=y in sdkconfig. */
static void monitor_task(void *arg)
{
    uint32_t prev_rx = 0, prev_tx = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_monitor) { prev_rx = lwip_stats.ip.recv; prev_tx = lwip_stats.ip.xmit; continue; }

        uint32_t rx = lwip_stats.ip.recv, tx = lwip_stats.ip.xmit;
        float rx_mbps = ((rx - prev_rx) * 1400.0f * 8) / 1e6f;
        float tx_mbps = ((tx - prev_tx) * 1400.0f * 8) / 1e6f;
        prev_rx = rx; prev_tx = tx;

        wifi_ap_record_t ap;
        int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : -99;
        wifi_sta_list_t cl; esp_wifi_ap_get_sta_list(&cl);

        printf("[monitor] RSSI:%d dBm  ~RX:%.2f Mbps  ~TX:%.2f Mbps  clients:%d\n",
               rssi, rx_mbps, tx_mbps, cl.num);
    }
}

/* ── Console commands ─────────────────────────────────────────────────── */
static int cmd_status(int argc, char **argv)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        printf("STA  : \"%s\"  RSSI:%d dBm  ch:%d\n", ap.ssid, ap.rssi, ap.primary);
    else
        printf("STA  : disconnected\n");

    if (g_sta_netif) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(g_sta_netif, &ip);
        printf("IP   : " IPSTR "\n", IP2STR(&ip.ip));
    }
    wifi_sta_list_t list; esp_wifi_ap_get_sta_list(&list);
    printf("Clients: %d\n", list.num);
    for (int i = 0; i < list.num; i++) {
        uint8_t *m = list.sta[i].mac;
        printf("  [%d] %02x:%02x:%02x:%02x:%02x:%02x  RSSI:%d\n",
               i, m[0],m[1],m[2],m[3],m[4],m[5], list.sta[i].rssi);
    }
    printf("IP pkts  RX:%u  TX:%u\n", lwip_stats.ip.recv, lwip_stats.ip.xmit);
    return 0;
}

static int cmd_monitor(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: monitor <on|off>\n"); return 1; }
    s_monitor = (strcmp(argv[1], "on") == 0);
    printf("Monitor %s\n", s_monitor ? "started" : "stopped");
    return 0;
}

static int cmd_ssid(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: ssid <show|hide>\n"); return 1; }
    bool hide = (strcmp(argv[1], "hide") == 0);
    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    cfg.ap.ssid_hidden = hide ? 1 : 0;
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    printf("SSID broadcast %s\n", hide ? "hidden" : "visible");
    return 0;
}

static int cmd_dnslog(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: dnslog <on|off>\n"); return 1; }
    s_dns_log = (strcmp(argv[1], "on") == 0);
    printf("DNS logging %s\n", s_dns_log ? "on" : "off");
    return 0;
}

static int cmd_reconnect(int argc, char **argv)
{
    printf("Reconnecting...\n");
    s_retries = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;
}

#ifdef USE_ENTERPRISE
static int cmd_provision(int argc, char **argv)
{
    if (argc != 4) {
        printf("Usage: provision <identity> <username> <password>\n");
        printf("  identity : outer EAP identity  (e.g. anonymous@university.edu)\n");
        printf("  username : inner EAP identity  (e.g. user@university.edu)\n");
        printf("  password : EAP password\n");
        return 1;
    }
    creds_save(argv[1], argv[2], argv[3]);
    printf("Saved. Restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int cmd_clear_creds(int argc, char **argv)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    printf("Credentials cleared. Restart to re-provision.\n");
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    char ident[64], user[64], pass[64];
    if (creds_load(ident, sizeof(ident), user, sizeof(user), pass, sizeof(pass))) {
        printf("identity : %s\n", ident);
        printf("username : %s\n", user);
        printf("password : %s\n", "********");
    } else {
        printf("No credentials stored.\n");
    }
    return 0;
}
#endif /* USE_ENTERPRISE */

static void console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "relay> ";
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    esp_console_register_help_command();

    const esp_console_cmd_t cmds[] = {
        { "status",    "WiFi status and client list",           NULL, cmd_status    },
        { "ssid",      "ssid <show|hide>  toggle AP broadcast", NULL, cmd_ssid      },
        { "monitor",   "monitor <on|off>  bandwidth estimate",  NULL, cmd_monitor   },
        { "dnslog",    "dnslog <on|off>  log DNS queries",      NULL, cmd_dnslog    },
        { "reconnect", "Reconnect upstream WiFi",               NULL, cmd_reconnect },
        { "restart",   "Restart ESP32",                         NULL, cmd_restart   },
#ifdef USE_ENTERPRISE
        { "provision",   "provision <identity> <user> <pass>  Store EAP creds in NVS", NULL, cmd_provision   },
        { "clear_creds", "Wipe stored EAP credentials",                                NULL, cmd_clear_creds },
        { "whoami",      "Show stored identity/username",                              NULL, cmd_whoami      },
#endif
    };
    for (int i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

/* ── WiFi event handler ───────────────────────────────────────────────── */
static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retries < MAX_RETRY) {
                esp_wifi_connect();
                ESP_LOGI(TAG, "STA retry %d/%d", ++s_retries, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_eg, STA_FAIL_BIT);
                ESP_LOGE(TAG, "Gave up connecting to upstream WiFi");
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "Client joined - %02x:%02x:%02x:%02x:%02x:%02x AID:%d",
                     e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5], e->aid);
#ifdef USE_MAC_FILTER
            /* Walk the whitelist; return immediately if the MAC is allowed. */
            for (int i = 0; i < ALLOWED_COUNT; i++)
                if (memcmp(e->mac, ALLOWED_MACS[i], 6) == 0) return;
            ESP_LOGW(TAG, "Blocked  - %02x:%02x:%02x:%02x:%02x:%02x",
                     e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5]);
            esp_wifi_deauth_sta(e->aid);
#endif
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "Client left  - %02x:%02x:%02x:%02x:%02x:%02x",
                     e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5]);
            break;
        }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_eg, STA_CONNECTED_BIT);
    }
}

/* ── Entry point ──────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS must be initialised before anything else — WiFi and console both use it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_eg   = xEventGroupCreate();
    g_ap_netif  = esp_netif_create_default_wifi_ap();
    g_sta_netif = esp_netif_create_default_wifi_sta();

#ifdef USE_ENTERPRISE
    /* Try to load EAP credentials from NVS.
     * If none are stored (first boot, or after clear_creds), drop to the REPL
     * so the user can provision them over serial before the relay starts. */
    char ident[64] = {0}, user[64] = {0}, pass[64] = {0};
    bool provisioned = creds_load(ident, sizeof(ident),
                                  user,  sizeof(user),
                                  pass,  sizeof(pass));
    if (!provisioned) {
        ESP_LOGW(TAG, "Not provisioned. Connect via serial and run:");
        ESP_LOGW(TAG, "  provision <identity> <username> <password>");
        console_init();
        return;
    }
#endif

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure the private AP. */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .password       = AP_PASSWORD,
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden    = AP_HIDDEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

#ifdef USE_ENTERPRISE
    /* Configure the STA for WPA2-Enterprise (PEAP/MSCHAPv2).
     * disable_time_check skips certificate expiry validation — useful when
     * the university cert chain is self-signed or your device clock is off. */
    wifi_config_t sta_cfg = { .sta = { .ssid = "eduroam" } };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_eap_client_set_identity((uint8_t *)ident, strlen(ident)));
    ESP_ERROR_CHECK(esp_eap_client_set_username((uint8_t *)user,  strlen(user)));
    ESP_ERROR_CHECK(esp_eap_client_set_password((uint8_t *)pass,  strlen(pass)));
    esp_eap_client_set_disable_time_check(true);
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
    /* Zero buffers — the EAP stack has copied the credentials internally. */
    memset(ident, 0, sizeof(ident));
    memset(user,  0, sizeof(user));
    memset(pass,  0, sizeof(pass));
#else
    wifi_config_t sta_cfg = {
        .sta = { .ssid = STA_SSID, .password = STA_PASSWORD },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP up: %s (WPA2)", AP_SSID);
    ESP_LOGI(TAG, "Connecting to upstream...");

    /* Block until the STA connects or exhausts retries. */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, STA_CONNECTED_BIT | STA_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & STA_CONNECTED_BIT) {
        /* Enable NAPT on the AP interface so traffic from 192.168.4.x is
         * masqueraded through the STA's upstream IP. */
        esp_netif_ip_info_t ap_ip;
        esp_netif_get_ip_info(g_ap_netif, &ap_ip);
        ip_napt_enable(ap_ip.ip.addr, 1);
        ESP_LOGI(TAG, "NAT enabled");
        xTaskCreate(dns_forwarder_task, "dns_fwd", 4096, NULL, 5, NULL);
        xTaskCreate(monitor_task,       "monitor",  4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Relay is live");
    } else {
        ESP_LOGE(TAG, "Upstream failed — AP up but no internet");
    }

    console_init();
}
