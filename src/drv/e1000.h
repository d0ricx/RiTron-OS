#ifndef KERNEL_E1000_H
#define KERNEL_E1000_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    E1000_OK,
    E1000_NOT_FOUND,
    E1000_DEVICE_ERROR,
    E1000_DHCP_TIMEOUT,
    E1000_DHCP_REJECTED
} E1000_RESULT;

typedef struct {
    /* IPv4 addresses use network byte order in the four low-to-high bytes. */
    uint8_t mac_address[6];
    uint32_t ipv4_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint32_t lease_seconds;
} E1000_NETWORK_CONFIG;

typedef void (*E1000_STALL)(size_t);

#ifdef __cplusplus
extern "C" {
#endif

/* Discover and initialize one supported Intel e1000 PCI device. */
E1000_RESULT e1000_initialize(void);
/* Perform the bounded DHCP discover/request exchange using the caller's timer. */
E1000_RESULT e1000_dhcp_acquire(E1000_STALL stall);
bool e1000_is_initialized(void);
bool e1000_has_ipv4_address(void);
const E1000_NETWORK_CONFIG *e1000_network_config(void);
/* Copy configuration to caller storage to avoid exposing mutable driver state. */
bool e1000_copy_network_config(E1000_NETWORK_CONFIG *config);
/*
 * Frame calls are synchronous and use caller-owned buffers. Send copies bytes
 * to a TX descriptor; receive polls the RX ring and reports one complete frame.
 */
bool e1000_send_frame(const uint8_t *frame, size_t length);
bool e1000_receive_frame(uint8_t *frame, size_t capacity, size_t *length);

#ifdef __cplusplus
}
#endif

#endif
