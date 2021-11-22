#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "serial_out.h"
#include "wifi.h"
#include "util.h"
#include "command_functions.h"

#define WAIT_QUEUE ((TickType_t)(10 / portTICK_PERIOD_MS))
#define DELAY ((TickType_t)(50 / portTICK_PERIOD_MS))
#define LOCATE_DELAY ((TickType_t)(5000 / portTICK_PERIOD_MS))
#define STATUS_DELAY ((TickType_t)(1000 / portTICK_PERIOD_MS))

#define FRAME_SIZE 152
#define VERSION 0x11
#define BROADCAST 0xFF
#define LOCATE 0x01
#define LINK 0x02
#define STATUS 0x03
#define MAX_NODES 4

typedef struct wifi_link {
    uint8_t node_id;
    uint8_t mac_addr[6];
} wifi_link;

static SemaphoreHandle_t links_access;
static SemaphoreHandle_t status_access;

static esp_now_peer_info_t peerInfo;

static uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t own_id = 0xFF;
static uint8_t locate_identifier = 0xFF;

static wifi_link wifi_links[MAX_NODES];

int active_nodes;
int new_nodes;


void net_table() {
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);

  if (wifi_links[0].node_id == NULL) {
    sprintf(error,"empty table");
    serial_out("empty table");
    xSemaphoreGive(links_access);
    return;
  }

  char res[40];
  for (int i = 0; i < MAX_NODES; i++) {
    if (wifi_links[i].node_id != NULL) {
      snprintf(res, sizeof(res), "%d %02X %02X:%02X:%02X:%02X:%02X:%02X",
      i,
      wifi_links[i].node_id,
      wifi_links[i].mac_addr[0],
      wifi_links[i].mac_addr[1],
      wifi_links[i].mac_addr[2],
      wifi_links[i].mac_addr[3],
      wifi_links[i].mac_addr[4],
      wifi_links[i].mac_addr[5]);
      serial_out(res);
    }
  }
  xSemaphoreGive(links_access);
}

void net_reset () {
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);

  for (int i = 0; i < MAX_NODES; i++) {
    wifi_links[i].node_id = 0;
    wifi_links[i].mac_addr[0] = 0;
    wifi_links[i].mac_addr[1] = 0;
    wifi_links[i].mac_addr[2] = 0;
    wifi_links[i].mac_addr[3] = 0;
    wifi_links[i].mac_addr[4] = 0;
    wifi_links[i].mac_addr[5] = 0;
  }
  xSemaphoreGive(links_access);
  serial_out("node reset");
}

void net_status () {
  active_nodes = 0;

  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);
  
  uint8_t wifi_msg[FRAME_SIZE] = {0};
  wifi_msg[0] = VERSION;
  wifi_msg[1] = own_id;
  wifi_msg[4] = STATUS;

  for (int i = 0; i < MAX_NODES; i++) {
    if(wifi_links[i].node_id != 0) {
      wifi_msg[2] = wifi_links[i].node_id;
      wifi_msg[3] = wifi_msg[0] + wifi_msg[1] + wifi_msg[2] + wifi_msg[4];
      if (esp_now_send(wifi_links[i].mac_addr, (uint8_t *)wifi_msg, FRAME_SIZE) != ESP_OK)
        return;
    }
  }
  xSemaphoreGive(links_access);
  vTaskDelay(STATUS_DELAY);
  char res[120];
  sprintf(res, "%i nodes active and %i nodes inactive", active_nodes, MAX_NODES-active_nodes);
  serial_out(res);
}

void net_locate () {
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);

  bool there_is_space = false;
  for (int i = 0; i < MAX_NODES; i++) {
    if (wifi_links[i].node_id == 0) {
      there_is_space = true;
      break;
    }
  } 

  xSemaphoreGive(links_access);

  if (!there_is_space) {
    sprintf(error, "table is full");
    serial_out("table is full");
    return;
  }

  uint8_t wifi_msg[FRAME_SIZE];
  wifi_msg[0] = VERSION;
  wifi_msg[1] = own_id;
  wifi_msg[2] = BROADCAST;
  wifi_msg[4] = LOCATE;
  locate_identifier = rand() % 256;
  wifi_msg[5] = locate_identifier;
  wifi_msg[3] = wifi_msg[0] + wifi_msg[1] + wifi_msg[2] + wifi_msg[4] + wifi_msg[5];

  if (esp_now_send(broadcast, (uint8_t *)wifi_msg, FRAME_SIZE) != ESP_OK)
    return;

  new_nodes = 0;
  vTaskDelay(LOCATE_DELAY);
  char res[120];
  sprintf(res, "Linked %i nodes", new_nodes);
  serial_out(res);
}

void send_link (const uint8_t node, const uint8_t id, const uint8_t *mac) {
  printf("hello");
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);
  
  for (int i = 0; i < MAX_NODES; i++) {
    if (wifi_links[i].node_id == node) {
      xSemaphoreGive(links_access);
      return;
    }
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (wifi_links[i].node_id == 0) {
      wifi_links[i].node_id = node;
      wifi_links[i].mac_addr[1] = id;
      break;
    }
  }
  xSemaphoreGive(links_access);
  uint8_t wifi_msg[FRAME_SIZE] = {0};
  wifi_msg[0] = VERSION;
  wifi_msg[1] = own_id;
  wifi_msg[2] = node;
  wifi_msg[4] = LINK;
  wifi_msg[5] = id;
  wifi_msg[3] = wifi_msg[0] + wifi_msg[1] + wifi_msg[2] + wifi_msg[4] + wifi_msg[5];
  if (esp_now_send(mac, (uint8_t *)wifi_msg, FRAME_SIZE) != ESP_OK)
      return;
  return;
}

void receive_link (const uint8_t node, const uint8_t id, const uint8_t *mac) {
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);
  if (locate_identifier == id) {
    for (int i = 0; i < MAX_NODES; i++) {
      if (wifi_links[i].node_id == node) {
        xSemaphoreGive(links_access);
        return;
      }
    }
  }
    for (int i = 0; i < MAX_NODES; i++) {
      if (wifi_links[i].node_id == 0) {
        wifi_links[i].node_id = node;
        memcpy(wifi_links[i].mac_addr, mac, sizeof(uint8_t) * 6);
        new_nodes++;
        break;
      }
    }    
  xSemaphoreGive(links_access);
}

void receive_status(const uint8_t node, const uint8_t *mac) {
  while (xSemaphoreTake(links_access, WAIT_QUEUE) != pdTRUE)
    vTaskDelay(DELAY);
  bool node_exists = false;
  for (int i = 0; i < MAX_NODES; i++) {
    if (wifi_links[i].node_id == node) {
      node_exists=true;
      active_nodes++;
      break;
    }
  }
  xSemaphoreGive(links_access);
  if (!node_exists) {
      return;
  }

  uint8_t wifi_msg[FRAME_SIZE];
  wifi_msg[0] = VERSION;
  wifi_msg[1] = own_id;
  wifi_msg[2] = node;
  wifi_msg[4] = STATUS;
  wifi_msg[3] = wifi_msg[0] + wifi_msg[1] + wifi_msg[2] + wifi_msg[4];

  if (esp_now_send(mac, (uint8_t *)wifi_msg, FRAME_SIZE) != ESP_OK)
    return;
}

void espnow_onreceive (const uint8_t *mac, const uint8_t *data, int len) {
  int check = 0x00;
  for (int i = 0; i < len; i++) {
    if (i != 3)
      check += data[i];
  }

  if (data[0] ^ 0x11 || check ^ data[3] || (data[2] ^ own_id && data[2] && BROADCAST))
    return;
  switch (data[4]) {
    case LOCATE:
      send_link(data[1], data[5], mac);
      break;
    case LINK:
      receive_link(data[1], data[5], mac);
      break;
    case STATUS:
      receive_status(data[1], mac);
      break;
    }
}

void espnow_init(void) {
  if (esp_now_init() != ESP_OK) {
      printf("Error initializing ESP-NOW\n");
      return;
  }
  esp_now_register_recv_cb(espnow_onreceive);
  memset(&peerInfo, 0, sizeof(esp_now_peer_info_t));
  memcpy(peerInfo.peer_addr, broadcast, 6);
  peerInfo.channel = 0;  
  peerInfo.ifidx   = ESP_IF_WIFI_STA;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      printf("Failed to add peer\n");
      return;
  }
  uint8_t curr_mac[6];
  esp_efuse_mac_get_default(curr_mac);
  own_id = curr_mac[0] == 0x30 ? 0x1e : 0x1d;
  links_access = xSemaphoreCreateBinary();
  xSemaphoreGive(links_access);
  status_access = xSemaphoreCreateBinary();
  xSemaphoreGive(status_access);
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start());

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}
