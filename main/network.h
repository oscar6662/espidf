/*
 *  Network interface to Applications  -- TOL103M
 */

#ifndef INCL_NETWORK_H
#define INCL_NETWORK_H

typedef struct  {
    uint16_t type;         /* application id */
    uint8_t  len;          /* length         */ 
    uint8_t  reserved[5];  /* do not use     */
} app_header_t;

/*
 * Note: app_id > 0
 */ 
int  net_init(           uint8_t node_id, int isDebugRoot);
int  net_register_app(   uint16_t app_id);
int  net_unregister_app( uint16_t app_id);
int  net_send_up(  const app_header_t *head, const uint8_t *data);
int  net_send_down(const app_header_t *head, const uint8_t *data);

#define NET_MAX_PAYLOAD  128

// Blocks until viable packet is available, or timeout occurs.
// - data pointer to an array with size NET_MAX_PAYLOAD (or more)
// - received length is in the header data
// - returns zero on success, otherwise -1
// - negative timeout means to wait until packet is available
int net_receive(uint16_t app_id, app_header_t *h, uint8_t *data, int32_t timeout);

    
#endif