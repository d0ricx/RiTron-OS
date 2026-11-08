#include "e1000.h"

#include <stdbool.h>

// This driver currently supports a narrow set of Intel devices and a single
// polled descriptor ring pair; it does not allocate DMA memory dynamically.
#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC
#define PCI_COMMAND_OFFSET 0x04
#define PCI_BAR0_OFFSET 0x10
#define PCI_HEADER_TYPE_OFFSET 0x0E
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_82540EM 0x100E
#define E1000_DEVICE_82545EM 0x100F
#define E1000_DEVICE_82574L 0x10D3
#define E1000_DEVICE_82573E 0x108B

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RAL 0x5400
#define REG_RAH 0x5404
#define REG_MTA 0x5200
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818

#define CTRL_RST 0x04000000
#define CTRL_SLU 0x00000040
#define RCTL_EN 0x00000002
#define RCTL_BAM 0x00008000
#define RCTL_SECRC 0x04000000
#define TCTL_EN 0x00000002
#define TCTL_PSP 0x00000008
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12
#define DESCRIPTOR_DONE 0x01
#define DESCRIPTOR_EOP 0x01
#define DESCRIPTOR_IFCS 0x02
#define DESCRIPTOR_RS 0x08
#define RX_RING_COUNT 8
#define RX_BUFFER_SIZE 2048
#define MAX_FRAME_SIZE 1514
#define DHCP_PACKET_CAPACITY 512
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_MAGIC 0x63825363
#define DHCP_TIMEOUT_TICKS 200
#define DHCP_ATTEMPTS 3

typedef struct __attribute__((packed, aligned(16))) {
    /* Hardware-visible RX descriptor layout; status is written by the NIC. */
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    volatile uint8_t status;
    uint8_t errors;
    uint16_t special;
} E1000_RX_DESCRIPTOR;

typedef struct __attribute__((packed, aligned(16))) {
    /* Hardware-visible TX descriptor layout; status reports completion. */
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    volatile uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} E1000_TX_DESCRIPTOR;

typedef struct {
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} E1000_PCI_DEVICE;

static E1000_RX_DESCRIPTOR ReceiveDescriptors[RX_RING_COUNT]
    __attribute__((aligned(128)));
static uint8_t ReceiveBuffers[RX_RING_COUNT][RX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static E1000_TX_DESCRIPTOR TransmitDescriptors[RX_RING_COUNT]
    __attribute__((aligned(128)));
static uint8_t TransmitBuffers[RX_RING_COUNT][MAX_FRAME_SIZE]
    __attribute__((aligned(16)));
static uint8_t DhcpPacket[DHCP_PACKET_CAPACITY];
static uint8_t ReceivedPacket[RX_BUFFER_SIZE];
static volatile uint32_t *Registers;
static size_t ReceiveIndex;
static size_t TransmitIndex;
static bool Initialized;
static bool HasAddress;
static E1000_NETWORK_CONFIG NetworkConfig;

// PCI configuration mechanism #1, used here to scan bus 0 only.
static void port_out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t port_in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | (offset & 0xFC);
    port_out32(PCI_CONFIG_ADDRESS, address);
    return port_in32(PCI_CONFIG_DATA);
}

static bool pci_find_device(E1000_PCI_DEVICE *found) {
    for (uint8_t device = 0; device < 32; ++device) {
        uint32_t identity = pci_read32(0, device, 0, 0);
        uint16_t vendor = (uint16_t)identity;
        uint16_t device_id = (uint16_t)(identity >> 16);
        uint8_t function_count;

        if (vendor == 0xFFFF) {
            continue;
        }
        // Multifunction PCI devices expose up to eight function numbers.
        function_count =
            (pci_read32(0, device, 0, PCI_HEADER_TYPE_OFFSET) >> 16) &
                    0x80
                ? 8
                : 1;
        for (uint8_t function = 0; function < function_count; ++function) {
            identity = function == 0
                           ? identity
                           : pci_read32(0, device, function, 0);
            vendor = (uint16_t)identity;
            device_id = (uint16_t)(identity >> 16);
            if (vendor != E1000_VENDOR_ID ||
                (device_id != E1000_DEVICE_82540EM &&
                 device_id != E1000_DEVICE_82545EM &&
                 device_id != E1000_DEVICE_82574L &&
                 device_id != E1000_DEVICE_82573E)) {
                continue;
            }
            found->device_id = device_id;
            found->bus = 0;
            found->device = device;
            found->function = function;
            return true;
        }
    }
    return false;
}

static uint32_t mmio_read(uint32_t offset) {
    return Registers[offset / sizeof(uint32_t)];
}

static void mmio_write(uint32_t offset, uint32_t value) {
    Registers[offset / sizeof(uint32_t)] = value;
}

static bool eeprom_read_word(uint8_t address, uint16_t *value) {
    uint32_t command = (1u << 1) | ((uint32_t)address << 8);
    mmio_write(REG_EERD, command);
    // Poll the EEPROM done bit with a finite retry bound during early boot.
    for (uint32_t retry = 0; retry < 100000; ++retry) {
        uint32_t result = mmio_read(REG_EERD);
        if ((result & (1u << 4)) != 0) {
            *value = (uint16_t)(result >> 16);
            return true;
        }
    }
    return false;
}

static bool mac_address_valid(void) {
    bool nonzero = false;
    bool all_ones = true;

    for (size_t index = 0; index < sizeof(NetworkConfig.mac_address); ++index) {
        uint8_t value = NetworkConfig.mac_address[index];
        nonzero = nonzero || value != 0;
        all_ones = all_ones && value == 0xFF;
    }
    return nonzero && !all_ones &&
           (NetworkConfig.mac_address[0] & 1) == 0;
}

static bool load_mac_address(void) {
    uint32_t low = mmio_read(REG_RAL);
    uint32_t high = mmio_read(REG_RAH);

    NetworkConfig.mac_address[0] = (uint8_t)low;
    NetworkConfig.mac_address[1] = (uint8_t)(low >> 8);
    NetworkConfig.mac_address[2] = (uint8_t)(low >> 16);
    NetworkConfig.mac_address[3] = (uint8_t)(low >> 24);
    NetworkConfig.mac_address[4] = (uint8_t)high;
    NetworkConfig.mac_address[5] = (uint8_t)(high >> 8);
    // Some devices expose an invalid reset value in RAL/RAH; fall back to EEPROM.
    if (mac_address_valid()) {
        return true;
    }

    for (uint8_t index = 0; index < 3; ++index) {
        uint16_t word;
        if (!eeprom_read_word((uint8_t)(index), &word)) {
            return false;
        }
        NetworkConfig.mac_address[index * 2] = (uint8_t)word;
        NetworkConfig.mac_address[index * 2 + 1] = (uint8_t)(word >> 8);
    }
    return mac_address_valid();
}

static bool initialize_rings(void) {
    uintptr_t receive_address = (uintptr_t)ReceiveDescriptors;
    uintptr_t transmit_address = (uintptr_t)TransmitDescriptors;

    // This driver programs only the low DMA base registers, so buffers must fit below 4 GiB.
    if ((receive_address >> 32) != 0 || (transmit_address >> 32) != 0) {
        return false;
    }
    ReceiveIndex = 0;
    TransmitIndex = 0;
    for (size_t index = 0; index < RX_RING_COUNT; ++index) {
        ReceiveDescriptors[index].address =
            (uint64_t)(uintptr_t)ReceiveBuffers[index];
        ReceiveDescriptors[index].length = 0;
        ReceiveDescriptors[index].checksum = 0;
        ReceiveDescriptors[index].status = 0;
        ReceiveDescriptors[index].errors = 0;
        ReceiveDescriptors[index].special = 0;
    }
    // Give RX all descriptors initially; RDT is the last available ring entry.
    mmio_write(REG_RDBAL, (uint32_t)receive_address);
    mmio_write(REG_RDBAH, 0);
    mmio_write(REG_RDLEN, sizeof(ReceiveDescriptors));
    mmio_write(REG_RDH, 0);
    mmio_write(REG_RDT, RX_RING_COUNT - 1);

    for (size_t index = 0; index < RX_RING_COUNT; ++index) {
        TransmitDescriptors[index].address =
            (uint64_t)(uintptr_t)TransmitBuffers[index];
        TransmitDescriptors[index].length = 0;
        TransmitDescriptors[index].checksum_offset = 0;
        TransmitDescriptors[index].command = 0;
        TransmitDescriptors[index].status = DESCRIPTOR_DONE;
        TransmitDescriptors[index].checksum_start = 0;
        TransmitDescriptors[index].special = 0;
    }
    mmio_write(REG_TDBAL, (uint32_t)transmit_address);
    mmio_write(REG_TDBAH, 0);
    mmio_write(REG_TDLEN, sizeof(TransmitDescriptors));
    mmio_write(REG_TDH, 0);
    mmio_write(REG_TDT, 0);
    return true;
}

E1000_RESULT e1000_initialize(void) {
    E1000_PCI_DEVICE device;
    uint32_t bar;
    uint32_t command;
    uint16_t pci_command;
    uint32_t memory_type;

    // Reset published state first so partial initialization is never reported usable.
    Initialized = false;
    HasAddress = false;
    if (!pci_find_device(&device)) {
        return E1000_NOT_FOUND;
    }
    bar = pci_read32(device.bus, device.device, device.function,
                     PCI_BAR0_OFFSET);
    memory_type = (bar >> 1) & 3;
    if (bar == 0 || (bar & 1) != 0 ||
        (memory_type != 0 && memory_type != 2)) {
        return E1000_DEVICE_ERROR;
    }
    // Decode an MMIO BAR, including its high dword when the BAR is 64-bit.
    uintptr_t base = (uintptr_t)(bar & ~0x0Fu);
    if (((bar >> 1) & 3) == 2) {
        base |= (uintptr_t)pci_read32(device.bus, device.device,
                                      device.function, PCI_BAR0_OFFSET + 4)
                << 32;
    }
    if (base == 0) {
        return E1000_DEVICE_ERROR;
    }

    command = pci_read32(device.bus, device.device, device.function,
                         PCI_COMMAND_OFFSET);
    pci_command = (uint16_t)command | 0x0006;
    port_out32(PCI_CONFIG_ADDRESS,
               0x80000000u | ((uint32_t)device.bus << 16) |
                   ((uint32_t)device.device << 11) |
                   ((uint32_t)device.function << 8) | PCI_COMMAND_OFFSET);
    port_out16(PCI_CONFIG_DATA, pci_command);

    Registers = (volatile uint32_t *)base;
    // Reset must complete before any device registers or descriptor rings are reused.
    mmio_write(REG_CTRL, CTRL_RST);
    for (uint32_t retry = 0; retry < 1000000; ++retry) {
        if ((mmio_read(REG_CTRL) & CTRL_RST) == 0) {
            break;
        }
        if (retry == 999999) {
            Registers = NULL;
            return E1000_DEVICE_ERROR;
        }
    }
    mmio_write(REG_IMC, 0xFFFFFFFF);
    (void)mmio_read(REG_STATUS);
    if (!load_mac_address()) {
        Registers = NULL;
        return E1000_DEVICE_ERROR;
    }
    if (!initialize_rings()) {
        Registers = NULL;
        return E1000_DEVICE_ERROR;
    }
    for (uint32_t index = 0; index < 128; ++index) {
        mmio_write(REG_MTA + index * sizeof(uint32_t), 0);
    }
    mmio_write(REG_CTRL, CTRL_SLU);
    mmio_write(REG_TCTL,
               TCTL_EN | TCTL_PSP | (0x10u << TCTL_CT_SHIFT) |
                   (0x40u << TCTL_COLD_SHIFT));
    mmio_write(REG_TIPG, 0x0060200A);
    mmio_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    Initialized = true;
    return E1000_OK;
}

static uint16_t read_be16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint16_t internet_checksum(const uint8_t *data, size_t length) {
    uint32_t sum = 0;

    while (length >= 2) {
        sum += read_be16(data);
        data += 2;
        length -= 2;
    }
    if (length != 0) {
        sum += (uint16_t)data[0] << 8;
    }
    // Fold one's-complement carries before complementing the 16-bit result.
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static bool transmit_packet(const uint8_t *packet, size_t length) {
    // A descriptor is reusable only after hardware reports completion.
    E1000_TX_DESCRIPTOR *descriptor = &TransmitDescriptors[TransmitIndex];
    uint8_t *buffer = TransmitBuffers[TransmitIndex];

    if (!Initialized || packet == NULL || length > sizeof(TransmitBuffers[0]) ||
        length < 14 ||
        descriptor->status != DESCRIPTOR_DONE) {
        return false;
    }
    // Copy into the descriptor's stable DMA buffer before notifying hardware.
    for (size_t index = 0; index < length; ++index) {
        buffer[index] = packet[index];
    }
    descriptor->length = (uint16_t)length;
    descriptor->command = DESCRIPTOR_EOP | DESCRIPTOR_IFCS | DESCRIPTOR_RS;
    __asm__ volatile("" : : : "memory");
    descriptor->status = 0;
    TransmitIndex = (TransmitIndex + 1) % RX_RING_COUNT;
    mmio_write(REG_TDT, (uint32_t)TransmitIndex);
    for (uint32_t retry = 0; retry < 1000000; ++retry) {
        __asm__ volatile("" : : : "memory");
        if ((descriptor->status & DESCRIPTOR_DONE) != 0) {
            return true;
        }
    }
    return false;
}

static bool receive_packet(size_t *length) {
    E1000_RX_DESCRIPTOR *descriptor = &ReceiveDescriptors[ReceiveIndex];

    __asm__ volatile("" : : : "memory");
    // The NIC owns the descriptor until DD is set; recycle it after copying data.
    if ((descriptor->status & DESCRIPTOR_DONE) == 0) {
        return false;
    }
    *length = descriptor->length;
    if (*length > sizeof(ReceivedPacket) || descriptor->errors != 0) {
        *length = 0;
    } else {
        for (size_t index = 0; index < *length; ++index) {
            ReceivedPacket[index] = ReceiveBuffers[ReceiveIndex][index];
        }
    }
    descriptor->length = 0;
    descriptor->checksum = 0;
    descriptor->errors = 0;
    descriptor->status = 0;
    ReceiveIndex = (ReceiveIndex + 1) % RX_RING_COUNT;
    mmio_write(REG_RDT, (uint32_t)((ReceiveIndex + RX_RING_COUNT - 1) %
                                  RX_RING_COUNT));
    return *length != 0;
}

static size_t build_dhcp_packet(uint8_t *packet, uint8_t message_type,
                                uint32_t transaction_id,
                                uint32_t requested_address,
                                uint32_t server_identifier) {
    const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF};
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    uint8_t *options;
    size_t dhcp_length;
    size_t udp_length;
    size_t ip_length;
    uint16_t checksum;

    // DHCP starts as a broadcast Ethernet/IPv4/UDP/BOOTP packet with padded options.
    for (size_t index = 0; index < 14 + 20 + 8 + 300; ++index) {
        packet[index] = 0;
    }
    for (size_t index = 0; index < 6; ++index) {
        packet[index] = broadcast_mac[index];
        packet[6 + index] = NetworkConfig.mac_address[index];
    }
    write_be16(packet + 12, 0x0800);
    ip[0] = 0x45;
    ip[1] = 0;
    ip[4] = 0;
    ip[5] = 0;
    ip[6] = 0x40;
    ip[7] = 0;
    ip[8] = 64;
    ip[9] = 17;
    ip[12] = 0;
    ip[13] = 0;
    ip[14] = 0;
    ip[15] = 0;
    ip[16] = 255;
    ip[17] = 255;
    ip[18] = 255;
    ip[19] = 255;
    write_be16(udp, 68);
    write_be16(udp + 2, 67);
    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    write_be32(bootp + 4, transaction_id);
    bootp[10] = 0x80;
    for (size_t index = 0; index < 6; ++index) {
        bootp[28 + index] = NetworkConfig.mac_address[index];
    }
    write_be32(bootp + 236, DHCP_MAGIC);
    options = bootp + 240;
    options[0] = 53;
    options[1] = 1;
    options[2] = message_type;
    options[3] = 55;
    options[4] = 6;
    options[5] = 1;
    options[6] = 3;
    options[7] = 6;
    options[8] = 51;
    options[9] = 54;
    options[10] = 58;
    options[11] = 59;
    size_t offset = 12;
    if (message_type == DHCP_REQUEST) {
        options[offset++] = 50;
        options[offset++] = 4;
        write_be32(options + offset, requested_address);
        offset += 4;
        options[offset++] = 54;
        options[offset++] = 4;
        write_be32(options + offset, server_identifier);
        offset += 4;
    }
    options[offset++] = 255;
    dhcp_length = 240 + offset;
    udp_length = 8 + dhcp_length;
    ip_length = 20 + udp_length;
    write_be16(udp + 4, (uint16_t)udp_length);
    write_be16(ip + 2, (uint16_t)ip_length);
    checksum = internet_checksum(ip, 20);
    write_be16(ip + 10, checksum);
    return 14 + ip_length;
}

typedef struct {
    uint8_t message_type;
    uint32_t offered_address;
    uint32_t server_identifier;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint32_t lease_seconds;
} DHCP_RESPONSE;

static bool parse_dhcp_response(const uint8_t *packet, size_t length,
                                uint32_t transaction_id,
                                DHCP_RESPONSE *response) {
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *bootp;
    const uint8_t *options;
    size_t ip_header_length;
    size_t bootp_length;
    size_t option_offset;
    size_t udp_length;

    // Validate each enclosing length before reading its nested DHCP fields.
    if (length < 14 + 20 + 8 + 240 || read_be16(packet + 12) != 0x0800) {
        return false;
    }
    ip = packet + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != 17) {
        return false;
    }
    ip_header_length = (ip[0] & 0x0F) * 4;
    if (ip_header_length < 20 || length < 14 + ip_header_length + 8) {
        return false;
    }
    udp = ip + ip_header_length;
    if (read_be16(udp) != 67 || read_be16(udp + 2) != 68) {
        return false;
    }
    udp_length = read_be16(udp + 4);
    if (udp_length < 8 + 240 ||
        length < 14 + ip_header_length + udp_length) {
        return false;
    }
    bootp = udp + 8;
    bootp_length = udp_length - 8;
    if (bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6 ||
        read_be32(bootp + 4) != transaction_id ||
        read_be32(bootp + 236) != DHCP_MAGIC) {
        return false;
    }
    for (size_t index = 0; index < 6; ++index) {
        if (bootp[28 + index] != NetworkConfig.mac_address[index]) {
            return false;
        }
    }
    response->offered_address = read_be32(bootp + 16);
    response->server_identifier = 0;
    response->subnet_mask = 0;
    response->gateway = 0;
    response->dns_server = 0;
    response->lease_seconds = 0;
    options = bootp + 240;
    option_offset = 0;
    response->message_type = 0;
    while (option_offset < bootp_length - 240) {
        uint8_t option = options[option_offset++];
        uint8_t option_length;

        if (option == 0) {
            continue;
        }
        if (option == 255) {
            break;
        }
        if (option_offset >= bootp_length - 240) {
            return false;
        }
        option_length = options[option_offset++];
        if (option_length > bootp_length - 240 - option_offset) {
            return false;
        }
        const uint8_t *value = options + option_offset;
        if (option == 53 && option_length == 1) {
            response->message_type = value[0];
        } else if (option == 54 && option_length == 4) {
            response->server_identifier = read_be32(value);
        } else if (option == 1 && option_length == 4) {
            response->subnet_mask = read_be32(value);
        } else if (option == 3 && option_length >= 4) {
            response->gateway = read_be32(value);
        } else if (option == 6 && option_length >= 4) {
            response->dns_server = read_be32(value);
        } else if (option == 51 && option_length == 4) {
            response->lease_seconds = read_be32(value);
        }
        option_offset += option_length;
    }
    return response->message_type != 0;
}

static bool wait_for_dhcp_message(uint32_t transaction_id,
                                  uint8_t expected_message,
                                  E1000_STALL stall,
                                  DHCP_RESPONSE *response) {
    // Ignore unrelated frames and keep polling only for this transaction's reply.
    for (uint32_t tick = 0; tick < DHCP_TIMEOUT_TICKS; ++tick) {
        size_t packet_length;
        if (receive_packet(&packet_length) &&
            parse_dhcp_response(ReceivedPacket, packet_length, transaction_id,
                                response)) {
            if (response->message_type == DHCP_NAK) {
                return false;
            }
            if (response->message_type == expected_message) {
                return true;
            }
        }
        stall(10000);
    }
    return false;
}

E1000_RESULT e1000_dhcp_acquire(E1000_STALL stall) {
    DHCP_RESPONSE response;
    uint32_t transaction_id;

    if (!Initialized || stall == NULL) {
        return E1000_DEVICE_ERROR;
    }
    HasAddress = false;
    // Derive a repeatable transaction ID from the MAC address for reply matching.
    transaction_id = 0x44525431;
    for (size_t index = 0; index < 6; ++index) {
        transaction_id = ((transaction_id << 5) | (transaction_id >> 27)) ^
                         NetworkConfig.mac_address[index];
    }
    // Each attempt performs the standard DISCOVER/OFFER then REQUEST/ACK exchange.
    for (uint32_t attempt = 0; attempt < DHCP_ATTEMPTS; ++attempt) {
        size_t packet_length =
            build_dhcp_packet(DhcpPacket, DHCP_DISCOVER,
                              transaction_id + attempt, 0, 0);
        if (!transmit_packet(DhcpPacket, packet_length) ||
            !wait_for_dhcp_message(transaction_id + attempt, DHCP_OFFER,
                                   stall, &response)) {
            continue;
        }
        if (response.offered_address == 0 ||
            response.server_identifier == 0) {
            continue;
        }
        packet_length = build_dhcp_packet(
            DhcpPacket, DHCP_REQUEST, transaction_id + attempt,
            response.offered_address, response.server_identifier);
        if (!transmit_packet(DhcpPacket, packet_length)) {
            continue;
        }
        if (!wait_for_dhcp_message(transaction_id + attempt, DHCP_ACK, stall,
                                   &response)) {
            continue;
        }
        if (response.offered_address == 0) {
            continue;
        }
        NetworkConfig.ipv4_address = response.offered_address;
        NetworkConfig.subnet_mask = response.subnet_mask;
        NetworkConfig.gateway = response.gateway;
        NetworkConfig.dns_server = response.dns_server;
        NetworkConfig.lease_seconds = response.lease_seconds;
        HasAddress = true;
        return E1000_OK;
    }
    return E1000_DHCP_TIMEOUT;
}

bool e1000_is_initialized(void) {
    return Initialized;
}

bool e1000_has_ipv4_address(void) {
    return HasAddress;
}

bool e1000_copy_network_config(E1000_NETWORK_CONFIG *config) {
    if (!HasAddress || config == NULL) {
        return false;
    }
    *config = NetworkConfig;
    return true;
}

const E1000_NETWORK_CONFIG *e1000_network_config(void) {
    return &NetworkConfig;
}

bool e1000_send_frame(const uint8_t *frame, size_t length) {
    return transmit_packet(frame, length);
}

bool e1000_receive_frame(uint8_t *frame, size_t capacity, size_t *length) {
    size_t received_length;

    // Never partially copy a frame into an undersized caller buffer.
    if (frame == NULL || length == NULL || !receive_packet(&received_length) ||
        received_length > capacity) {
        return false;
    }
    for (size_t index = 0; index < received_length; ++index) {
        frame[index] = ReceivedPacket[index];
    }
    *length = received_length;
    return true;
}
