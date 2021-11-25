
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_timer.h>

#define PINODE_ID 0x01

#define NETWORK_TYPE 0x10
#define NETWORK_VERSION 0x01

#define LINK_TABLE_SIZE 4
#define LINK_UP 0

#define INBOUND_QUEUE_SIZE 6

#define LOCATE_SIZE 16

#define WAIT_LOCK ((TickType_t)(10 / portTICK_PERIOD_MS))

// Microsecond timer values.
#define US_FACTOR 1000000

#define PERIOD_LOCATE			(25 * US_FACTOR)
#define WINDOW_LOCATE			(5 * US_FACTOR)
#define TIMEOUT_LOCATE			(1 * US_FACTOR)

#define TIMEOUT_PROPOSE_LINK	(2 * US_FACTOR)
#define TIMEOUT_STATUS			(1 * US_FACTOR)

#define TIMEOUT_LINK_DECAY		(30 * US_FACTOR)

#define PERIOD_UP_STATUS		(15 * US_FACTOR)
#define WINDOW_UP_STATUS		(5 * US_FACTOR)

#define WINDOW_SEND				(10000)

typedef uint8_t NodeId;

typedef struct LinkEntry {
	uint8_t mac[6];
	NodeId id;
	esp_timer_handle_t timer;
} LinkEntry;

typedef struct LinkTable {
	LinkEntry entry[LINK_TABLE_SIZE];
	uint32_t usage;
} LinkTable;

typedef struct AppQueue {
	uint16_t        id;
	QueueHandle_t   inbound;
} AppQueue;

typedef struct AppTable {
	uint32_t            usage;
	AppQueue            apps[32];
	SemaphoreHandle_t   lock;
} AppTable;

typedef struct NodeState {
	int			isRoot;
	NodeId		id;
	LinkTable	link_table;
	AppTable	app_table;
	uint32_t	flags;
	
	uint8_t		loc_ident;
	LinkEntry	loc_response[LOCATE_SIZE];
	uint32_t	loc_count;
	esp_timer_handle_t loc_timer;

	uint8_t				pending_mac[6];
	NodeId				pending_id;
	esp_timer_handle_t	pending_timer;

	esp_timer_handle_t status_timer;
	esp_timer_handle_t join_timer;

	TaskHandle_t svc_outbound;
} NodeState;

#define STATE_LOCATING (1ul << 0)
#define STATE_PENDING_LINK (1ul << 1)
#define STATE_UPLINK_STATUS (1ul << 2)
#define STATE_FROZEN (1ul << 3)

typedef struct NetFrameHeader {
	uint8_t version;
	NodeId source;
	NodeId destination;
	uint8_t checksum;
	uint8_t control;
	uint8_t reserved[11];
} NetFrameHeader;

#define RES_CONTROL 0
#define RES_IDENT 1
#define RES_ORIGIN 1
#define RES_UPSTREAM 2

#define CONTROL_DEFAULT 0
#define CONTROL_LOCATE 1
#define CONTROL_LINK 2
#define CONTROL_STATUS 3
#define CONTROL_MAP 4
#define CONTROL_BLACKOUT 5
#define CONTROL_FREEZE 6

typedef struct NetFrame {
	NetFrameHeader head;
	uint8_t contents[136];
} NetFrame;


// Clearinghouse for internal methods.
void init_sys();
void init_node(NodeState* node, NodeId id);
void init_table(LinkTable* table);
void init_hooks(AppTable* table);

int valid_packet(const uint8_t* mac, const uint8_t* data, int len);
int valid_link(const uint8_t* mac, NodeId node);

int is_linked(NodeId id);
int is_upstream(NodeId id);
int is_downstream(NodeId id);

const uint8_t* find_mac(NodeId id);
NodeId find_id(const uint8_t* mac);
LinkEntry* find_entry(NodeId id);
QueueHandle_t find_app(uint16_t app_id);

int has_uplink(const LinkTable* table);
int has_available_downlinks(const LinkTable* table);
int form_uplink(LinkTable* table, const uint8_t* mac, NodeId id);
int form_downlink(LinkTable* table, const uint8_t* mac, NodeId id);

uint8_t pak_checksum(const NetFrame* frame);

int cmp_mac(const uint8_t* mac_a, const uint8_t* mac_b);

// Packet sending interface?
void net_send_raw(NetFrame* frame);

void worker_send(void* param);

// Control packet handlers.
void exec_blackout();


// Callback methods for various timers.
void timer_cb_pending_link(void* param);
void timer_cb_locating(void* param);
void timer_cb_up_status(void* param);
void timer_cb_upstream(void* param);
void timer_cb_downstream(void* param);
void timer_cb_join(void* param);

void net_table();