#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "network.h"
#include "net_layer.h"
#include "command_functions.h"
#include "serial_out.h"

static const char* TAG = "NetworkLayer";


NodeState node;
LinkEntry link_broadcast = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    0xFF,
    NULL
};

QueueHandle_t outbound;



int net_init(uint8_t node_id, int isDebugRoot) {
    assert(node_id != 0);

    init_sys();
    init_node(&node, node_id);

    if (isDebugRoot) {
        node.link_table.usage |= (1ul << LINK_UP);
        node.isRoot = 1;
    }
    else {
        uint64_t wnd = PERIOD_LOCATE + (esp_random() % WINDOW_LOCATE);
        if (esp_timer_start_once(node.join_timer, wnd) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start network join timer.");
            return -1;
        }
    }

    return 0;
}

void net_table() {
  char res[40];
  int lined_nodes = 0;
  for (int i = 0; i < LINK_TABLE_SIZE; i++) {
    if (node.link_table.usage & (1ul << i)) {
      lined_nodes++;
      snprintf(res, sizeof(res), "%d %02X %02X:%02X:%02X:%02X:%02X:%02X",
        i,
        node.link_table.entry[i].id,
        node.link_table.entry[i].mac[0],
        node.link_table.entry[i].mac[1],
        node.link_table.entry[i].mac[2],
        node.link_table.entry[i].mac[3],
        node.link_table.entry[i].mac[4],
        node.link_table.entry[i].mac[5]);
      serial_out(res);
    }
  }
  if (!lined_nodes) {
    sprintf(error, "empty table");
    serial_out("empty table");
  }
}

int net_register_app(uint16_t app_id) {
    assert(app_id > 0);

    uint32_t slot = 0xFFFFFFFF;
    for (int i = 0; i < 32; ++i) {
        uint32_t mask = (1ul << i);
        if (!(node.app_table.usage & mask)) {
            if (node.app_table.apps[i].id == 0 && slot >= 32) {
                slot = i;
            }
        }
        else if (node.app_table.apps[i].id == app_id) {
            ESP_LOGE(TAG, "Error: Application type %d already registered.", app_id);
            return -1;
        }
    }

    if (slot > 32) {
        // Failed to find an empty slot:
        ESP_LOGE(TAG, "Error: Could not register application type %d, application table full.", app_id);
        return -2;
    }

    while (xSemaphoreTake(node.app_table.lock, WAIT_LOCK) != pdTRUE) {
        // Spin..
    }
    node.app_table.usage |= (1ul << slot);
    node.app_table.apps[slot].id = app_id;
    node.app_table.apps[slot].inbound = xQueueCreate(INBOUND_QUEUE_SIZE, sizeof(app_header_t) + NET_MAX_PAYLOAD);
    xSemaphoreGive(node.app_table.lock);
    return 0;
}


int net_unregister_app(uint16_t app_id) {
    ESP_LOGW(TAG, "net_unregister_app(..) is currently stubbed in.  Functionality reserved for future versions.");
    return -1;
}


int net_send_up(const app_header_t* head, const uint8_t* data) {
    assert(head != NULL);
    assert(data != NULL);

    if (node.isRoot) {
        ESP_LOGI(TAG, "Root node send_up(..) -- ignoring.");
        return 0;
    }

    // NOTE: This method does NOT verify that outbound packets have a valid app-id.

    if (!has_uplink(&node.link_table)) {
        ESP_LOGW(TAG, "net_send_up(..) failure.  No up-stream link.");
        return -1;
    }
    if (head->len > NET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "net_send_up(..) failure.  Invalid length: %d", head->len);
        return -2;
    }

    NetFrame out = {};
    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.destination = node.link_table.entry[LINK_UP].id;
    out.head.control = CONTROL_DEFAULT;

    memcpy(out.contents, head, sizeof(app_header_t));

    // NOTE: Is this still well-behaved if len == 0?  Verify.
    memcpy(out.contents + sizeof(app_header_t), data, head->len);

    out.head.checksum = pak_checksum(&out);
    net_send_raw(&out);
    return 0;
}


int net_send_down(const app_header_t* head, const uint8_t* data) {
    assert(head != NULL);
    assert(data != NULL);

    if (head->len > NET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "net_send_down(..) failure.  Invalid length: %d", head->len);
        return -2;
    }

    NetFrame out = {};
    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.control = CONTROL_DEFAULT;

    memcpy(out.contents, head, sizeof(app_header_t));
    // NOTE: Is this still well-behaved if len == 0?  Verify.
    memcpy(out.contents + sizeof(app_header_t), data, head->len);

    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (i == LINK_UP)
            continue;

        if (node.link_table.usage & (1ul << i)) {
            out.head.destination = node.link_table.entry[i].id;
            out.head.checksum = pak_checksum(&out);
            net_send_raw(&out);
        }
    }
    return 0;
}

int net_receive(uint16_t app_id, app_header_t* h, uint8_t* d, int32_t timeout) {
    assert(app_id > 0);
    assert(h != NULL);
    assert(d != NULL);

    uint8_t buffer[NET_MAX_PAYLOAD + sizeof(app_header_t)];

    // Some offset pointers for convenience into the buffer.
    app_header_t* head = (app_header_t*)buffer;
    uint8_t* data = buffer + sizeof(app_header_t);

    QueueHandle_t qh = find_app(app_id);

    if (qh == NULL) {
        ESP_LOGE(TAG, "Error: Application type %d not registered.", app_id);
        return -1;
    }

    if (timeout < 0) {
        while (xQueueReceive(qh, buffer, UINT32_MAX) != pdTRUE) {
            // Spin...
        }
    }
    else {
        if (xQueueReceive(qh, buffer, timeout / portTICK_RATE_MS) != pdTRUE) {
            return -2;
        }
    }

    if (head->len > NET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Received nominally overlength (%d) packet, truncating.", head->len);
        head->len = NET_MAX_PAYLOAD;
    }
    memcpy(h, head, sizeof(app_header_t));
    memcpy(d, data, head->len);
    return 0;
}



/*
* TIMER CALLBACK method for the LINK proposals originating from this node.
*  This callback only fires if the timeout for response has elapsed.
*/
void timer_cb_pending_link(void* param) {
    node.flags &= ~STATE_PENDING_LINK;
    esp_now_del_peer(node.pending_mac);
    memset(node.pending_mac, 0, 6);
    node.pending_id = 0;
}

/*
* TIMER CALLBACK method for the LOCATE interval.  Node should pick ONE
*  proposal and accept it -- this becomes the up-stream link.
* This callback fires once the window for LINK proposals has elapsed.
*/
void timer_cb_locating(void* param) {
    node.flags &= ~(STATE_LOCATING);

    // Ensure we got more than zero responses.  If not, restart the
    //  network join timer.
    if (node.loc_count == 0) {
        ESP_LOGW(TAG, "Failed to join network -- no nodes proposed LINK.");

        uint64_t wnd = PERIOD_LOCATE + (esp_random() % WINDOW_LOCATE);
        if (esp_timer_start_once(node.join_timer, wnd) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start network join timer.");
        }
        return;
    }

    NetFrame out = {};
    esp_now_peer_info_t peerInfo = {};

    // Pick a random node of those that responded.
    uint32_t x = esp_random() % node.loc_count;
    memcpy(peerInfo.peer_addr, node.loc_response[x].mac, 6);
    peerInfo.channel = 0;
    peerInfo.ifidx = ESP_IF_WIFI_STA;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add up-stream link peer.");
        return;
    }

    form_uplink(&node.link_table, node.loc_response[x].mac, node.loc_response[x].id);

    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.destination = node.loc_response[x].id;
    out.head.control = CONTROL_LINK;
    out.head.reserved[RES_IDENT] = node.loc_ident;
    out.head.checksum = pak_checksum(&out);

    net_send_raw(&out);

    ESP_LOGI(TAG, "Added up-stream link 0x%02X", node.loc_response[x].id);

    node.loc_count = 0;
    memset(node.loc_response, 0, sizeof(LinkEntry) * LOCATE_SIZE);
}

/*
* TIMER CALLBACK method -- fires if an up-stream status check times out without
*  a valid response.  This should trigger a downstream blackout.
*/
void timer_cb_up_status(void* param) {
    ESP_LOGE(TAG, "Failed to receive up-stream status response.");

    exec_blackout();
}

/*
* TIMER CALLBACK method -- if the upstream status check period elapses, issue a
*  status check to the up-stream node.
*/
void timer_cb_upstream(void* param) {
    assert(((int)param) == LINK_UP);

    // NOTE: The timer associated with this callback is NOT started if the node
    //  is initialized as debug root.  Thus we need not check whether this node
    //  is root or not before trying to send a packet upstream.

    NetFrame out = {};
    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.destination = node.link_table.entry[LINK_UP].id;
    out.head.control = CONTROL_STATUS;
    out.head.checksum = pak_checksum(&out);

    net_send_raw(&out);

    if (esp_timer_start_once(node.status_timer, TIMEOUT_STATUS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STATUS check timer.");
        return;
    }

    node.flags |= STATE_UPLINK_STATUS;

    // Restart the up-stream check timer.
    uint64_t wnd = PERIOD_UP_STATUS + (esp_random() % WINDOW_UP_STATUS);
    if (esp_timer_start_once(node.link_table.entry[LINK_UP].timer, wnd) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart up-stream status timer.");
        return;
    }
}

/*
* TIMER CALLBACK method -- if the link decay time has elapsed for a down-stream link,
*  we need to remove the link as it has expired.
*/
void timer_cb_downstream(void* param) {
    int x = (int)param;

    assert(x != LINK_UP && x < LINK_TABLE_SIZE && (node.link_table.usage & (1ul << x)));

    ESP_LOGI(TAG, "Down-stream link %d, %02X decayed.", x, node.link_table.entry[x].id);

    esp_now_del_peer(node.link_table.entry[x].mac);

    node.link_table.usage &= ~(1ul << x);
    node.link_table.entry[x].id = 0;
    memset(node.link_table.entry[x].mac, 0, 6);
}

/*
* TIMER CALLBACK method -- when this timer fires the node should attempt to join
*  a network, by sending out a LOCATE packet.
*/
void timer_cb_join(void* param) {
    node.flags |= STATE_LOCATING;

    NetFrame out = {};
    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.destination = link_broadcast.id;
    out.head.control = CONTROL_LOCATE;
    out.head.reserved[RES_IDENT] = ++node.loc_ident;
    out.head.checksum = pak_checksum(&out);

    net_send_raw(&out);

    if (esp_timer_start_once(node.loc_timer, TIMEOUT_LOCATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LOCATE timer.");
        return;
    }
}

/*
* The callback method for esp-now packet receival serves as a dispatch function.
*  It does simple verification of network layer state, and determines where the
*  packet needs to be enqueued for processing or immediately dealt with.
*/
void espnow_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!valid_packet(mac, data, len)) {
        return;
    }

    const NetFrame* frame = (const NetFrame*)data;
    NodeId src = frame->head.source;

    NetFrame out = {};

    esp_now_peer_info_t peerInfo = {};

    switch (frame->head.control) {
    case CONTROL_LOCATE:
        if (node.flags & STATE_FROZEN) break;

        // There is only one circumstance in which we would respond to a LOCATE
        //  packet.  The node must:
        //      - have an up-stream link.
        //      - have available entries in the link table.
        //      - not already be awaiting a response to a LINK proposal.
        if (has_uplink(&node.link_table) &&
            has_available_downlinks(&node.link_table) > 0 &&
            !(node.flags & STATE_PENDING_LINK)) {
            // Set the pending flag (link proposal) and then
            //  enqueue the LINK packet.
            node.flags |= STATE_PENDING_LINK;
            out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
            out.head.source = node.id;
            out.head.destination = src;
            out.head.control = CONTROL_LINK;
            out.head.reserved[RES_IDENT] = frame->head.reserved[RES_IDENT];
            out.head.checksum = pak_checksum(&out);

            node.pending_id = src;
            memcpy(node.pending_mac, mac, 6);
            memcpy(peerInfo.peer_addr, mac, 6);
            peerInfo.channel = 0;
            peerInfo.ifidx = ESP_IF_WIFI_STA;
            peerInfo.encrypt = false;
            if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add pending link peer.");
                break;
            }

            net_send_raw(&out);

            if (esp_timer_start_once(node.pending_timer, TIMEOUT_PROPOSE_LINK) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start LINK proposal timer.");
                break;
            }
        }
        break;

    case CONTROL_LINK:
        if (node.flags & STATE_FROZEN) break;

        // There are two possible interpretations of LINK packets.  Either other
        //  nodes are proposing linkage (after our LOCATE), or they are confirming a
        //  linkage we proposed in response to _their_ LOCATE.
        if (node.flags & STATE_LOCATING && frame->head.reserved[RES_IDENT] == node.loc_ident) {
            if (node.loc_count < LOCATE_SIZE) {
                node.loc_response[node.loc_count].id = src;
                memcpy(node.loc_response[node.loc_count].mac, mac, 6);
                node.loc_count++;
            }
        }
        else if (node.flags & STATE_PENDING_LINK) {
            // Verify the MAC address to ensure the node we proposed linkage to is
            //  the node which as responded to us!
            if (!cmp_mac(mac, node.pending_mac)) {
                break;
            }
            if (src != node.pending_id) {
                break;
            }

            ESP_LOGI(TAG, "Added down-stream link 0x%02X", src);

            esp_timer_stop(node.pending_timer);
            node.flags &= ~(STATE_PENDING_LINK);
            form_downlink(&node.link_table, mac, src);
            memset(node.pending_mac, 0, 6);
            node.pending_id = 0;
        }
        break;

    // NOTE: In all further cases, me MUST verify that the packet is arriving from a
    //  node to which we are linked -- discard otherwise.

    case CONTROL_STATUS:
        if (!is_linked(src) || (node.flags & STATE_FROZEN)) break;

        // Two possible valid cases for STATUS packets received.  Either we have
        //  already requested a STATUS from the upstream node and this is a response,
        //  or this is a request from a down-stream node which we should respond to.
        if (node.flags & STATE_UPLINK_STATUS && is_upstream(src)) {
            // Up-stream STATUS response detected.
            node.flags &= ~(STATE_UPLINK_STATUS);
            esp_timer_stop(node.status_timer);
        }
        else if (is_downstream(src)) {
            // Down-stream STATUS request detected.
            
            // Restart the link decay timer for the link.
            LinkEntry* link = find_entry(src);
            esp_timer_stop(link->timer);
            if (esp_timer_start_once(link->timer, TIMEOUT_LINK_DECAY) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restart down-stream link decay timer.");
                break;
            }

            // Respond with a STATUS packet.
            out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
            out.head.source = node.id;
            out.head.destination = src;
            out.head.control = CONTROL_STATUS;
            out.head.checksum = pak_checksum(&out);

            // TODO: Replace with queue mechanism.
            net_send_raw(&out);
        }
        break;

    case CONTROL_MAP:
        if (!is_linked(src)) break;

        // Two scenarios -- either this is coming from up-stream, in which case 
        //  we compose a response and also forward downstream.  Alternately, if
        //  this comes from down-stream, forward it upwards.
        if (is_upstream(src)) {
            // Send local map info back upstream.
            out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
            out.head.source = node.id;
            out.head.destination = src;
            out.head.control = CONTROL_MAP;
            out.head.reserved[RES_ORIGIN] = node.id;
            out.head.reserved[RES_UPSTREAM] = src;
            out.head.checksum = pak_checksum(&out);
            net_send_raw(&out);

            // Forward original packet downstream.
            memcpy(&out, frame, sizeof(NetFrame));
            out.head.source = node.id;
            for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
                if (i == LINK_UP)
                    continue;
                if (node.link_table.usage & (1ul << i)) {
                    out.head.destination = node.link_table.entry[i].id;
                    out.head.checksum = pak_checksum(&out);
                    net_send_raw(&out);
                }
            }
        }
        else if (is_downstream(src) && !node.isRoot) {
            memcpy(&out, frame, sizeof(NetFrame));
            out.head.source = node.id;
            out.head.destination = node.link_table.entry[LINK_UP].id;
            out.head.checksum = pak_checksum(&out);
            net_send_raw(&out);
        }
        break;

    case CONTROL_BLACKOUT:
        if (node.flags & STATE_FROZEN) break;

        if (!is_linked(src) || !is_upstream(src))
            break;

        exec_blackout();
        break;

    case CONTROL_FREEZE:
        if (!is_linked(src) || !is_upstream(src))
            break;

        if (node.flags & STATE_FROZEN) {
            // Node is frozen --> Unfreeze.
            node.flags &= ~(STATE_FROZEN);

            // Restart the link timers.
            for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
                if (node.link_table.usage & (1ul << i)) {
                    if (i == LINK_UP) {
                        if (!node.isRoot) {
                            uint64_t wnd = PERIOD_UP_STATUS + (esp_random() % WINDOW_UP_STATUS);
                            if (esp_timer_start_once(node.link_table.entry[i].timer, wnd) != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to restart up-stream status timer after freeze.");
                                return;
                            }
                        }
                    }
                    else {
                        if (esp_timer_start_once(node.link_table.entry[i].timer, TIMEOUT_LINK_DECAY) != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to restart down-stream link decay timer after freeze.");
                            return;
                        }
                    }
                }
            }
        }
        else {
            // Node is normal --> Freeze.
            node.flags |= STATE_FROZEN;

            /*
            * What is involved in freezing the node?
            *   - stopping all link entry timers (link decay + upstream status check)
            *   - cancelling any pending status timeouts (if we have sent a status 
            *       packet upstream and then FREEZE arrives before response).
            */
            node.flags &= ~(STATE_UPLINK_STATUS);
            esp_timer_stop(node.status_timer);

            for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
                if (node.link_table.usage & (1ul << i)) {
                    if ((i == LINK_UP && !node.isRoot) || i != LINK_UP) {
                        esp_timer_stop(node.link_table.entry[i].timer);
                    }
                }
            }
        }

        break;

    case CONTROL_DEFAULT: {
            // TODO: Re-evaluate default behaviour.  Maybe.. no default behaviour?
            //  Let the applicates decide what packet forwarding behaviour is appropriate
            //  for their application type.
            if (!is_linked(src))
                break;

            uint8_t app_pkt[NET_MAX_PAYLOAD + sizeof(app_header_t)];
            memcpy(&app_pkt, frame->contents, sizeof(app_header_t) + NET_MAX_PAYLOAD);

            // NOTE: This is a bit of a hack.  Encode first app header reserved byte as
            //  0x01 if the packet came from upstream, otherwise 0x00.  This behaviour is
            //  NOT defined in the spec and may be subject to change.

            ((app_header_t*)app_pkt)->reserved[0] = (is_upstream(src) ? 0x01 : 0x00);

            uint16_t app_id = ((app_header_t*)app_pkt)->type;

            QueueHandle_t qh = find_app(app_id);
            if (qh != NULL) {
                xQueueSend(qh, app_pkt, 0);
            }
            else {
                // No application registered for the app type.  Engage default behaviour.
                if (is_upstream(src)) {
                    net_send_down((app_header_t*)app_pkt, app_pkt + sizeof(app_header_t));
                }
                else {
                    net_send_up((app_header_t*)app_pkt, app_pkt + sizeof(app_header_t));
                }
            }

            break;
        }
    }
}

void exec_blackout() {
    NetFrame out = {};
    out.head.version = (NETWORK_TYPE | NETWORK_VERSION);
    out.head.source = node.id;
    out.head.control = CONTROL_BLACKOUT;

    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (i == LINK_UP)
            continue;

        if (node.link_table.usage & (1ul << i)) {
            out.head.destination = node.link_table.entry[i].id;
            out.head.checksum = pak_checksum(&out);
            net_send_raw(&out);
        }
    }

    ESP_LOGI(TAG, "Blacking out...");
    // NOTE: It would generally be in very bad form to delay(..) within a function
    //  used by timer / esp-now callbacks.  However, since our next action is to
    //  restart the device, we can get away with this here for now.
    vTaskDelay(2000 / portTICK_RATE_MS);
    esp_restart();
}

/*
* Predicate method.  Returns non-zero if true.
*/
int has_uplink(const LinkTable* table) {
    assert(table != NULL);

    if (table->usage & (1ul << LINK_UP)) {
        return 1;
    }
    return 0;
}

/*
* Method checks link table.  If there are available down-stream entries, it
*  returns the index associated with that entry.  Returns negative if there
*  are no down-stream links available.
*/
int has_available_downlinks(const LinkTable* table) {
    assert(table != NULL);

    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (i == LINK_UP) {
            continue;
        }
        if (!(table->usage & (1ul << i)))
            return i;
    }
    return -1;
}

/*
* Method returns 0 on success, non-zero otherwise.
*/
int form_uplink(LinkTable* table, const uint8_t* mac, NodeId id) {
    assert(table != NULL);
    assert(mac != NULL);
    assert(id > 0);

    if (table->usage & (1ul << LINK_UP)) {
        ESP_LOGE(TAG, "Up-stream virtual link already established.");
        return -1;
    }

    table->usage |= (1ul << LINK_UP);
    table->entry[LINK_UP].id = id;
    memcpy(table->entry[LINK_UP].mac, mac, 6);

    uint64_t wnd = PERIOD_UP_STATUS + (esp_random() % WINDOW_UP_STATUS);
    if (esp_timer_start_once(table->entry[LINK_UP].timer, wnd) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start up-stream status timer.");
        return -2;
    }

    return 0;
}

/*
* Method returns 0 on success, non-zero otherwise.
*/
int form_downlink(LinkTable* table, const uint8_t* mac, NodeId id) {
    assert(table != NULL);
    assert(mac != NULL);
    assert(id > 0);

    int x = has_available_downlinks(table);
    if (x < 0) {
        ESP_LOGE(TAG, "Cannot form down-stream link, link table full.");
        return -1;
    }

    table->usage |= (1ul << x);
    table->entry[x].id = id;
    memcpy(table->entry[x].mac, mac, 6);

    if (esp_timer_start_once(table->entry[x].timer, TIMEOUT_LINK_DECAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start down-stream link decay timer.");
        return -2;
    }

    return 0;
}

/*
* Method returns zero if packet is obviously invalid or malformed.
*/
int valid_packet(const uint8_t* mac, const uint8_t* data, int len) {
    const NetFrame* frame = (const NetFrame*)data;


    if (len != sizeof(NetFrame)) {
        return 0;
    }
    
    if (frame->head.version != (NETWORK_TYPE | NETWORK_VERSION)) {
        return 0;
    }

    if (frame->head.checksum != pak_checksum(frame)) {
        return 0;
    }

    return 1;
}
/*
* Method checks whether the (MAC, node-id) pair matches an existing link in
*  the link table.
*/
int valid_link(const uint8_t* mac, NodeId id) {
    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (node.link_table.usage & (1ul << i)) {
            int match = 1;
            if (!cmp_mac(mac, node.link_table.entry[i].mac)) {
                match = 0;
            }
            if (node.link_table.entry[i].id != id) {
                match = 0;
            }
            if (match) {
                return 1;
            }
        }
    }
    return 0;
}

/*
* Predicate method, returns non-zero if the node-id is a node to which we are
*  virtually linked, upstream or down.
*/
int is_linked(NodeId id) {
    return ((is_upstream(id) | is_downstream(id)) != 0 ? 1 : 0);
}

/*
* Predicate method, returns non-zero if node-id is the up-stream link for
*  this node.
*/
int is_upstream(NodeId id) {
    assert(id != 0);

    return (id == node.link_table.entry[LINK_UP].id ? 1 : 0);
}

/*
* Predicate method, returns non-zero if node-id is a down-stream link for
*  this node.
*/
int is_downstream(NodeId id) {
    assert(id != 0);

    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (i == LINK_UP)
            continue;

        if (node.link_table.usage & (1ul << i) && node.link_table.entry[i].id == id) {
            return 1;
        }
    }
    return 0;
}

/*
* Predicate method, returns non-zero if the two mac addresses provided
*  are identical.
*/
int cmp_mac(const uint8_t* mac_a, const uint8_t* mac_b) {
    assert(mac_a != NULL);
    assert(mac_b != NULL);

    if (mac_a == mac_b) {
        ESP_LOGW(TAG, "Warning: cmp_mac(..) called on identical pointers.");
    }

    for (int i = 0; i < 6; ++i) {
        if (mac_a[i] != mac_b[i])
            return 0;
    }
    return 1;
}

uint8_t pak_checksum(const NetFrame* frame) {
    int offset_check = 3;

    uint8_t  balance = 0;
    const uint8_t* work = (const uint8_t*)frame;

    for (int i = 0; i < sizeof(NetFrame); ++i) {
        if (i != offset_check) {
            balance = balance ^ work[i];
        }
    }
    return balance;
}

void init_sys() {
    esp_now_peer_info_t peerInfo = {};

    ESP_ERROR_CHECK(nvs_flash_init());        // initialize NVS

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing ESP-NOW");
        return;
    }
    esp_now_register_recv_cb(espnow_recv);

    //  register the broadcast address
    memcpy(peerInfo.peer_addr, link_broadcast.mac, 6);
    peerInfo.channel = 0;
    peerInfo.ifidx = ESP_IF_WIFI_STA;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer");
        return;
    }

    // Initialize the outbound packet queue.
    outbound = xQueueCreate(16, sizeof(NetFrame));
    if (!outbound) {
        ESP_LOGE(TAG, "Failed to create outbound packet queue.");
        return;
    }

    // Zero-initialize the node state.
    memset(&node, 0, sizeof(NodeState));

    // Create the worker task which actually transmits outbound packets.
    xTaskCreatePinnedToCore(
        worker_send,
        "svc_outbound",
        2048,
        NULL,
        6,
        &node.svc_outbound,
        1);

    ESP_LOGI(TAG, "Initialized network layer.");
}

void init_node(NodeState* node, NodeId id) {
    // NOTE: Method assumes node has ALREADY been zero-initialized.

    esp_timer_create_args_t timer_init = {};

    init_table(&node->link_table);
    init_hooks(&node->app_table);

    // Pick a random initial identifier.
    node->id = id;
    node->loc_ident = esp_random() % 256;
    

    // Set up the timers associated with the network layer.
    timer_init.callback = timer_cb_pending_link;
    timer_init.arg = NULL;
    timer_init.dispatch_method = ESP_TIMER_TASK;
    timer_init.name = "LinkProposal";
    if (esp_timer_create(&timer_init, &node->pending_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer.");
        return;
    }

    timer_init.callback = timer_cb_locating;
    timer_init.arg = NULL;
    timer_init.dispatch_method = ESP_TIMER_TASK;
    timer_init.name = "Locate";
    if (esp_timer_create(&timer_init, &node->loc_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer.");
        return;
    }

    timer_init.callback = timer_cb_up_status;
    timer_init.arg = NULL;
    timer_init.dispatch_method = ESP_TIMER_TASK;
    timer_init.name = "UpStatus";
    if (esp_timer_create(&timer_init, &node->status_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer.");
        return;
    }

    timer_init.callback = timer_cb_join;
    timer_init.arg = NULL;
    timer_init.dispatch_method = ESP_TIMER_TASK;
    timer_init.name = "NetJoin";
    if (esp_timer_create(&timer_init, &node->join_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer.");
        return;
    }
}

void init_table(LinkTable* table) {
    // NOTE: Method assumes table has ALREADY been zero-initialized.

    esp_timer_create_args_t timer_init = {};
    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (i == 0) {
            timer_init.callback = timer_cb_upstream;
            timer_init.name = "UpStreamDecay";
        }
        else {
            timer_init.callback = timer_cb_downstream;
            timer_init.name = "DownStreamDecay";
        }
        // NOTE: We stash the INDEX of the relevant entry in the timer cb argument.
        timer_init.arg = (void*)i;
        timer_init.dispatch_method = ESP_TIMER_TASK;

        if (esp_timer_create(&timer_init, &(table->entry[i].timer)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create virtual link timer.");
            return;
        }
    }
}

void init_hooks(AppTable* table) {
    // NOTE: Method assumes table has ALREADY been zero-initialized.

    table->lock = xSemaphoreCreateBinary();
    if (table->lock == NULL) {
        ESP_LOGE(TAG, "Failed to initialize app table semaphore.");
        return;
    }
    xSemaphoreGive(table->lock);
}

/*
* Method looks up the MAC address associated with a node-id in the link table.
*  Note that this method will also check the broadcast id and any pending link
*  node-id.
* Returns NULL on failure.
*/
const uint8_t* find_mac(NodeId id) {
    if (id == link_broadcast.id) {
        return link_broadcast.mac;
    }
    else if (id == node.pending_id) {
        return node.pending_mac;
    }

    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (node.link_table.usage & (1ul << i) && node.link_table.entry[i].id == id) {
            return node.link_table.entry[i].mac;
        }
    }
    return NULL;
}

/*
* Method looks up the node-id associated with a MAC address in the link table.
*  Note that method also checks broadcast MAC and pending link MAC.
* Returns 0 on failure.
*/
NodeId find_id(const uint8_t* mac) {
    if (cmp_mac(mac, link_broadcast.mac)) {
        return link_broadcast.id;
    }
    else if (cmp_mac(mac, node.pending_mac)) {
        return node.pending_id;
    }
    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (node.link_table.usage & (1ul << i)) {
            if (cmp_mac(mac, node.link_table.entry[i].mac)) {
                return node.link_table.entry[i].id;
            }
        }
    }
    return 0;
}

/*
* Method looks up the link table entry associated with a node-id.
* Returns NULL on failure.
*/
LinkEntry* find_entry(NodeId id) {
    for (int i = 0; i < LINK_TABLE_SIZE; ++i) {
        if (node.link_table.usage & (1ul << i) && node.link_table.entry[i].id == id) {
            return node.link_table.entry + i;
        }
    }
    return NULL;
}

/*
* Method looks up the inbound packet queue associated with an app-id.
* Returns NULL on failure.
*/
QueueHandle_t find_app(uint16_t app_id) {
    if (app_id == 0)
        return NULL;

    QueueHandle_t result = NULL;

    if (xSemaphoreTake(node.app_table.lock, WAIT_LOCK) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire semaphore for app table lookup.");
        return NULL;
    }

    for (int i = 0; i < 32; ++i) {
        uint32_t mask = (1ul << i);
        if (node.app_table.usage & mask && node.app_table.apps[i].id == app_id) {
            result = node.app_table.apps[i].inbound;
            break;
        }
    }
    xSemaphoreGive(node.app_table.lock);
    return result;
}


void net_send_raw(NetFrame* frame) {
    assert(frame != NULL);

    // Simple validation -- any outbound packets must have as a destination
    //  a node-id associated with one of our virtual links, or the broadcast
    //  address.
    assert(is_linked(frame->head.destination) || 
        frame->head.destination == link_broadcast.id ||
        frame->head.destination == node.pending_id);

    if (xQueueSend(outbound, frame, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send packet -- outbound queue full.");
    }
}

/*
* NOTE: This method requires that the packet be validated BEFORE it is pushed
*  to the outbound queue.  All items on the outbound queue are assumed to be valid.
*/
void worker_send(void* param) {
    NetFrame packet = {};
    while (1) {
        while (xQueueReceive(outbound, &packet, UINT32_MAX) != pdTRUE) {
            // Spin.
        }

        // Add a minor random delay to packet transmission to mitigate spiky traffic.
        vTaskDelay(((esp_random() % WINDOW_SEND) / 1000) / portTICK_RATE_MS);

        if (esp_now_send(find_mac(packet.head.destination), (const uint8_t*)&packet, sizeof(NetFrame)) != ESP_OK) {
            ESP_LOGE(TAG, "Packet send failure.");
        }
    }
}
