//! Synchronous plain-HTTP client used by the kernel's `httpget` command.
//!
//! This is a bounded single-request client for the polled e1000 driver. It
//! supports DNS A lookups and plain HTTP/1.1, but not TLS, IP fragmentation, or
//! general-purpose TCP retransmission/reassembly.
//! Protocol codecs live in sibling modules so they can be tested without hardware.
#![no_std]

#[path = "dns.rs"]
mod dns;
#[path = "http_codec.rs"]
mod http_codec;
#[path = "tcp.rs"]
mod tcp;

use core::panic::PanicInfo;
use core::slice;

const ETHERNET_HEADER_LENGTH: usize = 14;
const IPV4_HEADER_LENGTH: usize = 20;
const TCP_HEADER_LENGTH: usize = 20;
const MAX_FRAME_LENGTH: usize = 1514;
const MAX_HTTP_REQUEST_LENGTH: usize = 512;
const RECEIVE_TIMEOUT_TICKS: usize = 1000;
const RETRY_TICKS: usize = 100;
const ARP_TIMEOUT_TICKS: usize = 300;
const HANDSHAKE_ATTEMPTS: usize = 3;
const DNS_TIMEOUT_TICKS: usize = 300;
const DNS_SOURCE_PORT: u16 = 53000;
const NEIGHBOR_CACHE_SIZE: usize = 8;
const NEIGHBOR_CACHE_TTL: u8 = 32;
const TCP_FIN: u8 = 0x01;
const TCP_SYN: u8 = 0x02;
const TCP_RST: u8 = 0x04;
const TCP_PSH: u8 = 0x08;
const TCP_ACK: u8 = 0x10;
const IPV4_PROTOCOL_UDP: u8 = 17;

const HTTP_CLIENT_OK: i32 = 0;
const HTTP_CLIENT_INVALID_ARGUMENT: i32 = 1;
const HTTP_CLIENT_NO_NETWORK: i32 = 2;
const HTTP_CLIENT_ARP_TIMEOUT: i32 = 3;
const HTTP_CLIENT_TCP_TIMEOUT: i32 = 4;
const HTTP_CLIENT_CONNECTION_RESET: i32 = 5;
const HTTP_CLIENT_BAD_RESPONSE: i32 = 6;
const HTTP_CLIENT_RESPONSE_TOO_LARGE: i32 = 7;
const HTTP_CLIENT_TRANSMIT_ERROR: i32 = 8;
const HTTP_CLIENT_DNS_FAILURE: i32 = 9;

#[repr(C)]
// Keep field order and integer widths identical to E1000_NETWORK_CONFIG in C.
struct NetworkConfig {
    mac_address: [u8; 6],
    ipv4_address: u32,
    subnet_mask: u32,
    gateway: u32,
    dns_server: u32,
    lease_seconds: u32,
}

type Stall = extern "C" fn(usize);

// Narrow driver boundary: networking builds packets, while C owns NIC descriptors.
unsafe extern "C" {
    fn e1000_copy_network_config(config: *mut NetworkConfig) -> bool;
    fn e1000_send_frame(frame: *const u8, length: usize) -> bool;
    fn e1000_receive_frame(frame: *mut u8, capacity: usize, length: *mut usize) -> bool;
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // A panic cannot unwind through the C kernel; halt without relying on a runtime.
    loop {
        unsafe {
            core::arch::asm!("cli; hlt", options(nomem, nostack));
        }
    }
}

#[derive(Clone, Copy)]
// Parsed TCP fields refer into the original receive frame for payload access.
struct TcpPacket {
    sequence: u32,
    acknowledgement: u32,
    flags: u8,
    payload_start: usize,
    payload_length: usize,
}

#[derive(Clone, Copy)]
struct NeighborEntry {
    ip_address: u32,
    mac_address: [u8; 6],
    ttl: u8,
}

// Kernel shell requests are serialized, so one mutable ephemeral-port counter suffices.
static mut NEXT_EPHEMERAL_PORT: u16 = 49152;
static mut NEXT_DNS_ID: u16 = 1;
static mut NEIGHBOR_CACHE: [Option<NeighborEntry>; NEIGHBOR_CACHE_SIZE] =
    [None; NEIGHBOR_CACHE_SIZE];

// Packet integers are encoded in network byte order regardless of CPU endianness.
fn read_be16(bytes: &[u8], offset: usize) -> u16 {
    ((bytes[offset] as u16) << 8) | bytes[offset + 1] as u16
}

fn read_be32(bytes: &[u8], offset: usize) -> u32 {
    ((bytes[offset] as u32) << 24)
        | ((bytes[offset + 1] as u32) << 16)
        | ((bytes[offset + 2] as u32) << 8)
        | bytes[offset + 3] as u32
}

fn write_be16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset] = (value >> 8) as u8;
    bytes[offset + 1] = value as u8;
}

fn write_be32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset] = (value >> 24) as u8;
    bytes[offset + 1] = (value >> 16) as u8;
    bytes[offset + 2] = (value >> 8) as u8;
    bytes[offset + 3] = value as u8;
}

fn fold_checksum(mut sum: u32) -> u16 {
    // Internet checksums use one's-complement addition with end-around carry.
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

fn checksum_sum(bytes: &[u8], mut sum: u32) -> u32 {
    let mut offset = 0;
    while offset + 1 < bytes.len() {
        sum += read_be16(bytes, offset) as u32;
        offset += 2;
    }
    // An odd trailing byte occupies the high byte of the final 16-bit word.
    if offset < bytes.len() {
        sum += (bytes[offset] as u32) << 8;
    }
    sum
}

fn internet_checksum(bytes: &[u8]) -> u16 {
    fold_checksum(checksum_sum(bytes, 0))
}

fn tcp_checksum(source: u32, destination: u32, segment: &[u8]) -> u16 {
    // TCP checksum covers the IPv4 pseudo-header followed by the full segment.
    let mut sum = ((source >> 16) & 0xFFFF) + (source & 0xFFFF);
    sum += ((destination >> 16) & 0xFFFF) + (destination & 0xFFFF);
    sum += 6 + segment.len() as u32;
    fold_checksum(checksum_sum(segment, sum))
}

fn udp_checksum(source: u32, destination: u32, datagram: &[u8]) -> u16 {
    let mut sum = ((source >> 16) & 0xFFFF) + (source & 0xFFFF);
    sum += ((destination >> 16) & 0xFFFF) + (destination & 0xFFFF);
    sum += IPV4_PROTOCOL_UDP as u32 + datagram.len() as u32;
    fold_checksum(checksum_sum(datagram, sum))
}

// Ethernet frames cross the C ABI synchronously; the driver copies TX data before returning.
fn send_frame(frame: &[u8]) -> bool {
    // The C driver must consume/copy a frame before this borrow ends.
    unsafe { e1000_send_frame(frame.as_ptr(), frame.len()) }
}

fn receive_frame(frame: &mut [u8; MAX_FRAME_LENGTH]) -> Option<usize> {
    let mut length = 0;
    // The C driver reports false for no frame, invalid arguments, or insufficient capacity.
    if unsafe { e1000_receive_frame(frame.as_mut_ptr(), frame.len(), &mut length) }
        && length <= frame.len()
    {
        Some(length)
    } else {
        None
    }
}

fn send_arp_request(config: &NetworkConfig, target: u32) -> bool {
    // ARP requests are Ethernet broadcasts; the target MAC field is intentionally zero.
    let mut frame = [0u8; 42];
    frame[..6].fill(0xFF);
    frame[6..12].copy_from_slice(&config.mac_address);
    write_be16(&mut frame, 12, 0x0806);
    write_be16(&mut frame, 14, 1);
    write_be16(&mut frame, 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(&mut frame, 20, 1);
    frame[22..28].copy_from_slice(&config.mac_address);
    write_be32(&mut frame, 28, config.ipv4_address);
    write_be32(&mut frame, 38, target);
    send_frame(&frame)
}

fn answer_arp_request(config: &NetworkConfig, frame: &[u8]) {
    // Respond only to well-formed Ethernet/IPv4 ARP requests for our assigned address.
    if frame.len() < 42
        || read_be16(frame, 12) != 0x0806
        || read_be16(frame, 14) != 1
        || read_be16(frame, 16) != 0x0800
        || frame[18] != 6
        || frame[19] != 4
        || read_be16(frame, 20) != 1
        || read_be32(frame, 38) != config.ipv4_address
    {
        return;
    }
    let mut reply = [0u8; 42];
    reply[..6].copy_from_slice(&frame[22..28]);
    reply[6..12].copy_from_slice(&config.mac_address);
    write_be16(&mut reply, 12, 0x0806);
    write_be16(&mut reply, 14, 1);
    write_be16(&mut reply, 16, 0x0800);
    reply[18] = 6;
    reply[19] = 4;
    write_be16(&mut reply, 20, 2);
    reply[22..28].copy_from_slice(&config.mac_address);
    write_be32(&mut reply, 28, config.ipv4_address);
    reply[32..38].copy_from_slice(&frame[22..28]);
    write_be32(&mut reply, 38, read_be32(frame, 28));
    let _ = send_frame(&reply);
}

fn neighbor_cache_slot(ip_address: u32) -> usize {
    (ip_address.wrapping_mul(0x9E3779B1) as usize) % NEIGHBOR_CACHE_SIZE
}

fn read_neighbor_cache(ip_address: u32) -> Option<[u8; 6]> {
    unsafe {
        for slot in 0..NEIGHBOR_CACHE_SIZE {
            let entry = NEIGHBOR_CACHE[slot];
            if let Some(mut candidate) = entry {
                if candidate.ip_address == ip_address {
                    if candidate.ttl == 0 {
                        NEIGHBOR_CACHE[slot] = None;
                        continue;
                    }
                    // This small cache ages on lookup, not wall-clock time.
                    candidate.ttl -= 1;
                    NEIGHBOR_CACHE[slot] = Some(candidate);
                    return Some(candidate.mac_address);
                }
            }
        }
    }
    None
}

fn write_neighbor_cache(ip_address: u32, mac_address: [u8; 6]) {
    unsafe {
        let slot = neighbor_cache_slot(ip_address);
        let entry = NeighborEntry {
            ip_address,
            mac_address,
            ttl: NEIGHBOR_CACHE_TTL,
        };
        NEIGHBOR_CACHE[slot] = Some(entry);
    }
}

fn resolve_arp(config: &NetworkConfig, target: u32, stall: Stall) -> Option<[u8; 6]> {
    if let Some(mac_address) = read_neighbor_cache(target) {
        return Some(mac_address);
    }
    // Resolve the gateway for off-subnet traffic, then wait a bounded number of PIT ticks.
    if target == 0 || !send_arp_request(config, target) {
        return None;
    }
    let mut frame = [0u8; MAX_FRAME_LENGTH];
    let mut tick = 0;
    while tick < ARP_TIMEOUT_TICKS {
        if let Some(length) = receive_frame(&mut frame) {
            let packet = &frame[..length];
            answer_arp_request(config, packet);
            if packet.len() >= 42
                && read_be16(packet, 12) == 0x0806
                && read_be16(packet, 14) == 1
                && read_be16(packet, 16) == 0x0800
                && packet[18] == 6
                && packet[19] == 4
                && read_be16(packet, 20) == 2
                && read_be32(packet, 38) == config.ipv4_address
                && read_be32(packet, 28) == target
                && packet[32..38] == config.mac_address
            {
                let mut address = [0u8; 6];
                address.copy_from_slice(&packet[22..28]);
                write_neighbor_cache(target, address);
                return Some(address);
            }
        }
        stall(10_000);
        tick += 1;
    }
    None
}

fn send_udp(
    config: &NetworkConfig,
    destination_mac: &[u8; 6],
    destination_ip: u32,
    source_port: u16,
    destination_port: u16,
    payload: &[u8],
) -> bool {
    let udp_length = 8 + payload.len();
    let ip_length = IPV4_HEADER_LENGTH + udp_length;
    let frame_length = ETHERNET_HEADER_LENGTH + ip_length;
    if frame_length > MAX_FRAME_LENGTH || udp_length > u16::MAX as usize {
        return false;
    }
    let mut frame = [0u8; MAX_FRAME_LENGTH];
    frame[..6].copy_from_slice(destination_mac);
    frame[6..12].copy_from_slice(&config.mac_address);
    write_be16(&mut frame, 12, 0x0800);
    let ip = ETHERNET_HEADER_LENGTH;
    frame[ip] = 0x45;
    write_be16(&mut frame, ip + 2, ip_length as u16);
    write_be16(&mut frame, ip + 4, 0);
    write_be16(&mut frame, ip + 6, 0x4000);
    frame[ip + 8] = 64;
    frame[ip + 9] = IPV4_PROTOCOL_UDP;
    write_be32(&mut frame, ip + 12, config.ipv4_address);
    write_be32(&mut frame, ip + 16, destination_ip);
    let ip_checksum = internet_checksum(&frame[ip..ip + IPV4_HEADER_LENGTH]);
    write_be16(&mut frame, ip + 10, ip_checksum);

    let udp = ip + IPV4_HEADER_LENGTH;
    write_be16(&mut frame, udp, source_port);
    write_be16(&mut frame, udp + 2, destination_port);
    write_be16(&mut frame, udp + 4, udp_length as u16);
    frame[udp + 8..frame_length].copy_from_slice(payload);
    send_frame(&frame[..frame_length])
}

fn resolve_dns(config: &NetworkConfig, hostname: &[u8], stall: Stall) -> Option<u32> {
    if config.dns_server == 0 {
        return None;
    }
    let id = unsafe {
        let current = NEXT_DNS_ID;
        NEXT_DNS_ID = NEXT_DNS_ID.wrapping_add(1);
        current
    };
    let mut query = [0u8; 512];
    let query_length = dns::build_query(id, hostname, &mut query)?;
    let next_hop = route_address(config, config.dns_server)?;
    let destination_mac = resolve_arp(config, next_hop, stall)?;
    if !send_udp(
        config,
        &destination_mac,
        config.dns_server,
        DNS_SOURCE_PORT,
        53,
        &query[..query_length],
    ) {
        return None;
    }

    let mut frame = [0u8; MAX_FRAME_LENGTH];
    // Poll a bounded window and retry the same transaction ID on timeout.
    for tick in 0..DNS_TIMEOUT_TICKS {
        if tick != 0
            && tick % RETRY_TICKS == 0
            && !send_udp(
                config,
                &destination_mac,
                config.dns_server,
                DNS_SOURCE_PORT,
                53,
                &query[..query_length],
            )
        {
            return None;
        }
        if let Some(length) = receive_frame(&mut frame) {
            let packet = &frame[..length];
            if packet.len() < ETHERNET_HEADER_LENGTH + IPV4_HEADER_LENGTH + 8
                || read_be16(packet, 12) != 0x0800
                || packet[..6] != config.mac_address
            {
                stall(10_000);
                continue;
            }
            // QEMU's DNS proxy can reply from the gateway MAC, not the ARP alias.
            let ip = ETHERNET_HEADER_LENGTH;
            if packet[ip] >> 4 != 4
                || packet[ip] & 0x0F != 5
                || packet[ip + 9] != IPV4_PROTOCOL_UDP
                || internet_checksum(&packet[ip..ip + IPV4_HEADER_LENGTH]) != 0
                || read_be32(packet, ip + 12) != config.dns_server
                || read_be32(packet, ip + 16) != config.ipv4_address
                || read_be16(packet, ip + 6) & 0x3FFF != 0
            {
                stall(10_000);
                continue;
            }
            let ip_length = read_be16(packet, ip + 2) as usize;
            if ip_length < IPV4_HEADER_LENGTH + 8 || ip + ip_length > packet.len() {
                stall(10_000);
                continue;
            }
            let udp = ip + IPV4_HEADER_LENGTH;
            let udp_length = read_be16(packet, udp + 4) as usize;
            if read_be16(packet, udp) != 53
                || read_be16(packet, udp + 2) != DNS_SOURCE_PORT
                || udp_length < 8
                || udp_length > ip_length - IPV4_HEADER_LENGTH
                || (read_be16(packet, udp + 6) != 0
                    && udp_checksum(
                        config.dns_server,
                        config.ipv4_address,
                        &packet[udp..udp + udp_length],
                    ) != 0)
            {
                stall(10_000);
                continue;
            }
            if let Some(address) =
                dns::parse_response(&packet[udp + 8..udp + udp_length], id, hostname)
            {
                return Some(address);
            }
        }
        stall(10_000);
    }
    None
}

fn send_tcp(
    config: &NetworkConfig,
    destination_mac: &[u8; 6],
    destination_ip: u32,
    source_port: u16,
    destination_port: u16,
    sequence: u32,
    acknowledgement: u32,
    flags: u8,
    payload: &[u8],
) -> bool {
    // Construct one Ethernet + fixed IPv4 + fixed TCP header frame (no options).
    let tcp_length = TCP_HEADER_LENGTH + payload.len();
    let ip_length = IPV4_HEADER_LENGTH + tcp_length;
    let frame_length = ETHERNET_HEADER_LENGTH + ip_length;
    if frame_length > MAX_FRAME_LENGTH {
        return false;
    }
    let mut frame = [0u8; MAX_FRAME_LENGTH];
    frame[..6].copy_from_slice(destination_mac);
    frame[6..12].copy_from_slice(&config.mac_address);
    write_be16(&mut frame, 12, 0x0800);

    let ip = ETHERNET_HEADER_LENGTH;
    frame[ip] = 0x45;
    write_be16(&mut frame, ip + 2, ip_length as u16);
    write_be16(&mut frame, ip + 4, (sequence as u16).wrapping_add(1));
    write_be16(&mut frame, ip + 6, 0x4000);
    frame[ip + 8] = 64;
    frame[ip + 9] = 6;
    write_be32(&mut frame, ip + 12, config.ipv4_address);
    write_be32(&mut frame, ip + 16, destination_ip);
    let ip_checksum = internet_checksum(&frame[ip..ip + IPV4_HEADER_LENGTH]);
    write_be16(&mut frame, ip + 10, ip_checksum);

    let tcp = ip + IPV4_HEADER_LENGTH;
    write_be16(&mut frame, tcp, source_port);
    write_be16(&mut frame, tcp + 2, destination_port);
    write_be32(&mut frame, tcp + 4, sequence);
    write_be32(&mut frame, tcp + 8, acknowledgement);
    frame[tcp + 12] = (TCP_HEADER_LENGTH as u8 / 4) << 4;
    frame[tcp + 13] = flags;
    write_be16(&mut frame, tcp + 14, 8192);
    frame[tcp + TCP_HEADER_LENGTH..frame_length].copy_from_slice(payload);
    let checksum = tcp_checksum(
        config.ipv4_address,
        destination_ip,
        &frame[tcp..frame_length],
    );
    write_be16(&mut frame, tcp + 16, checksum);
    send_frame(&frame[..frame_length])
}

fn parse_tcp_packet(
    frame: &[u8],
    config: &NetworkConfig,
    destination_mac: &[u8; 6],
    remote_ip: u32,
    local_port: u16,
    remote_port: u16,
) -> Option<TcpPacket> {
    // Validate enclosing Ethernet/IP lengths before deriving the TCP payload slice.
    if frame.len() < ETHERNET_HEADER_LENGTH + IPV4_HEADER_LENGTH
        || read_be16(frame, 12) != 0x0800
        || frame[..6] != config.mac_address
        || frame[6..12] != destination_mac[..]
    {
        return None;
    }
    let ip = ETHERNET_HEADER_LENGTH;
    // IPv4 options are accounted for when locating TCP; fragmented IPv4 is unsupported.
    if frame[ip] >> 4 != 4 || frame[ip + 9] != 6 {
        return None;
    }
    let ip_header_length = ((frame[ip] & 0x0F) as usize) * 4;
    if ip_header_length < IPV4_HEADER_LENGTH
        || frame.len() < ip + ip_header_length + TCP_HEADER_LENGTH
        || internet_checksum(&frame[ip..ip + ip_header_length]) != 0
        || read_be32(frame, ip + 12) != remote_ip
        || read_be32(frame, ip + 16) != config.ipv4_address
        || read_be16(frame, ip + 6) & 0x3FFF != 0
    {
        return None;
    }
    let ip_total_length = read_be16(frame, ip + 2) as usize;
    if ip_total_length < ip_header_length + TCP_HEADER_LENGTH || ip + ip_total_length > frame.len()
    {
        return None;
    }
    let tcp = ip + ip_header_length;
    let tcp_length = ip_total_length - ip_header_length;
    let tcp_header_length = ((frame[tcp + 12] >> 4) as usize) * 4;
    if tcp_header_length < TCP_HEADER_LENGTH
        || tcp_header_length > tcp_length
        || read_be16(frame, tcp) != remote_port
        || read_be16(frame, tcp + 2) != local_port
        || tcp_checksum(
            remote_ip,
            config.ipv4_address,
            &frame[tcp..tcp + tcp_length],
        ) != 0
    {
        return None;
    }
    Some(TcpPacket {
        sequence: read_be32(frame, tcp + 4),
        acknowledgement: read_be32(frame, tcp + 8),
        flags: frame[tcp + 13],
        payload_start: tcp + tcp_header_length,
        payload_length: tcp_length - tcp_header_length,
    })
}

fn receive_tcp_packet(
    frame: &mut [u8; MAX_FRAME_LENGTH],
    config: &NetworkConfig,
    destination_mac: &[u8; 6],
    remote_ip: u32,
    local_port: u16,
    remote_port: u16,
) -> Option<(usize, TcpPacket)> {
    // Service ARP even while waiting for TCP so neighbors can resolve our MAC.
    let length = receive_frame(frame)?;
    let packet = &frame[..length];
    answer_arp_request(config, packet);
    parse_tcp_packet(
        packet,
        config,
        destination_mac,
        remote_ip,
        local_port,
        remote_port,
    )
    .map(|tcp| (length, tcp))
}

fn build_http_request(
    buffer: &mut [u8; MAX_HTTP_REQUEST_LENGTH],
    hostname: &[u8],
    port: u16,
    path: &[u8],
) -> Option<usize> {
    http_codec::build_request(buffer, hostname, port, path)
}

#[derive(Clone, Copy)]
struct RouteEntry {
    network: u32,
    next_hop: u32,
}

fn route_address(config: &NetworkConfig, destination: u32) -> Option<u32> {
    // Route to a neighbor directly when the destination is on-link; otherwise use the
    // configured default gateway. This is intentionally a simple model for phase 1.
    if destination == 0 || destination == u32::MAX {
        return None;
    }

    if config.subnet_mask != 0 {
        let local_network = config.ipv4_address & config.subnet_mask;
        let candidate = RouteEntry {
            network: local_network,
            next_hop: destination,
        };
        if (destination & config.subnet_mask) == candidate.network {
            return Some(candidate.next_hop);
        }
    }

    // The current DHCP contract exposes one default gateway, not a route table.
    let default_route = RouteEntry {
        network: 0,
        next_hop: config.gateway,
    };
    if default_route.next_hop != 0 {
        Some(default_route.next_hop)
    } else {
        None
    }
}

unsafe fn http_get(
    hostname_pointer: *const u8,
    hostname_length: usize,
    port: u16,
    path_pointer: *const u8,
    path_length: usize,
    output_pointer: *mut u8,
    output_capacity: usize,
    output_length: *mut usize,
    http_status: *mut u16,
    stall: Stall,
) -> i32 {
    // Raw C pointers are accepted only after null/length/capacity checks below.
    if hostname_pointer.is_null()
        || path_pointer.is_null()
        || output_pointer.is_null()
        || output_length.is_null()
        || http_status.is_null()
        || output_capacity < MAX_HTTP_REQUEST_LENGTH
        || hostname_length == 0
        || hostname_length > 253
        || path_length == 0
        || path_length > 400
        || port == 0
    {
        return HTTP_CLIENT_INVALID_ARGUMENT;
    }
    // Caller guarantees these regions are valid and non-overlapping for this call.
    let hostname = slice::from_raw_parts(hostname_pointer, hostname_length);
    let path = slice::from_raw_parts(path_pointer, path_length);
    let output = slice::from_raw_parts_mut(output_pointer, output_capacity);
    let output_length = &mut *output_length;
    let http_status = &mut *http_status;
    *output_length = 0;
    *http_status = 0;

    let mut request = [0u8; MAX_HTTP_REQUEST_LENGTH];
    let request_length = match build_http_request(&mut request, hostname, port, path) {
        Some(length) => length,
        None => return HTTP_CLIENT_INVALID_ARGUMENT,
    };
    let mut config = NetworkConfig {
        mac_address: [0; 6],
        ipv4_address: 0,
        subnet_mask: 0,
        gateway: 0,
        dns_server: 0,
        lease_seconds: 0,
    };
    if !e1000_copy_network_config(&mut config) || config.ipv4_address == 0 {
        return HTTP_CLIENT_NO_NETWORK;
    }
    let remote_ip = match dns::parse_ipv4(hostname) {
        Some(address) => address,
        None => match resolve_dns(&config, hostname, stall) {
            Some(address) => address,
            None => return HTTP_CLIENT_DNS_FAILURE,
        },
    };
    // Resolve link-layer addressing before creating the TCP four-tuple.
    let next_hop = match route_address(&config, remote_ip) {
        Some(address) => address,
        None => return HTTP_CLIENT_INVALID_ARGUMENT,
    };
    let destination_mac = match resolve_arp(&config, next_hop, stall) {
        Some(address) => address,
        None => return HTTP_CLIENT_ARP_TIMEOUT,
    };

    let local_port = unsafe {
        let current = NEXT_EPHEMERAL_PORT;
        NEXT_EPHEMERAL_PORT = if current >= 65534 { 49152 } else { current + 1 };
        current
    };
    let initial_sequence = remote_ip.rotate_left(7) ^ (port as u32) ^ 0x44525431;
    let mut connection = tcp::Connection::new(initial_sequence);
    let mut connected = false;
    let mut frame = [0u8; MAX_FRAME_LENGTH];

    // Retry SYN a bounded number of times; only accept SYN+ACK for our sequence.
    for _ in 0..HANDSHAKE_ATTEMPTS {
        if !send_tcp(
            &config,
            &destination_mac,
            remote_ip,
            local_port,
            port,
            initial_sequence,
            connection.receive_next,
            TCP_SYN,
            &[],
        ) {
            return HTTP_CLIENT_TRANSMIT_ERROR;
        }
        for _ in 0..RETRY_TICKS {
            if let Some((length, tcp)) = receive_tcp_packet(
                &mut frame,
                &config,
                &destination_mac,
                remote_ip,
                local_port,
                port,
            ) {
                if tcp.flags & TCP_RST != 0 {
                    connection.reset();
                    return HTTP_CLIENT_CONNECTION_RESET;
                }
                if tcp.flags & (TCP_SYN | TCP_ACK) == (TCP_SYN | TCP_ACK)
                    && connection.accept_syn_ack(tcp.sequence, tcp.acknowledgement)
                {
                    connected = true;
                    let _ = length;
                    break;
                }
            }
            stall(10_000);
        }
        if connected {
            break;
        }
    }
    if !connected {
        return HTTP_CLIENT_TCP_TIMEOUT;
    }
    if !send_tcp(
        &config,
        &destination_mac,
        remote_ip,
        local_port,
        port,
        connection.send_next,
        connection.receive_next,
        TCP_ACK,
        &[],
    ) {
        return HTTP_CLIENT_TRANSMIT_ERROR;
    }

    // Send one complete HTTP/1.1 request segment; this remains a single-request client.
    let request_sequence = connection.send_next;
    let mut request_acknowledged = false;
    let mut response_length = 0usize;
    let mut server_closed = false;
    let mut elapsed = 0usize;

    if !send_tcp(
        &config,
        &destination_mac,
        remote_ip,
        local_port,
        port,
        connection.send_next,
        connection.receive_next,
        TCP_ACK | TCP_PSH,
        &request[..request_length],
    ) {
        return HTTP_CLIENT_TRANSMIT_ERROR;
    }
    if !connection.send_data(request_length) {
        return HTTP_CLIENT_TCP_TIMEOUT;
    }

    // Accumulate in-order payload only and ACK the next expected server sequence.
    while elapsed < RECEIVE_TIMEOUT_TICKS {
        if let Some((frame_length, tcp)) = receive_tcp_packet(
            &mut frame,
            &config,
            &destination_mac,
            remote_ip,
            local_port,
            port,
        ) {
            if tcp.flags & TCP_RST != 0 {
                connection.reset();
                return HTTP_CLIENT_CONNECTION_RESET;
            }
            if connection.acknowledge(tcp.acknowledgement)
                && tcp::acknowledgement_covers(
                    tcp.acknowledgement,
                    request_sequence,
                    request_length,
                )
            {
                request_acknowledged = true;
            }
            if tcp.payload_length != 0 {
                if connection.expects_segment(tcp.sequence) {
                    if response_length + tcp.payload_length > output.len() {
                        return HTTP_CLIENT_RESPONSE_TOO_LARGE;
                    }
                    let payload_start = tcp.payload_start;
                    let payload_end = payload_start + tcp.payload_length;
                    if payload_end > frame_length {
                        return HTTP_CLIENT_BAD_RESPONSE;
                    }
                    output[response_length..response_length + tcp.payload_length]
                        .copy_from_slice(&frame[payload_start..payload_end]);
                    response_length += tcp.payload_length;
                    if !connection.receive_data(tcp.sequence, tcp.payload_length) {
                        return HTTP_CLIENT_BAD_RESPONSE;
                    }
                }
                if !send_tcp(
                    &config,
                    &destination_mac,
                    remote_ip,
                    local_port,
                    port,
                    connection.send_next,
                    connection.receive_next,
                    TCP_ACK,
                    &[],
                ) {
                    return HTTP_CLIENT_TRANSMIT_ERROR;
                }
            }
            if tcp.flags & TCP_FIN != 0 && connection.receive_fin(tcp.sequence, tcp.payload_length)
            {
                server_closed = true;
                let _ = send_tcp(
                    &config,
                    &destination_mac,
                    remote_ip,
                    local_port,
                    port,
                    connection.send_next,
                    connection.receive_next,
                    TCP_ACK,
                    &[],
                );
                break;
            }
        } else {
            stall(10_000);
        }
        elapsed += 1;
        if !request_acknowledged && elapsed != 0 && elapsed % RETRY_TICKS == 0 {
            if !send_tcp(
                &config,
                &destination_mac,
                remote_ip,
                local_port,
                port,
                request_sequence,
                connection.receive_next,
                TCP_ACK | TCP_PSH,
                &request[..request_length],
            ) {
                return HTTP_CLIENT_TRANSMIT_ERROR;
            }
        }
        if server_closed {
            break;
        }
    }
    if !server_closed {
        return HTTP_CLIENT_TCP_TIMEOUT;
    }

    // Response framing is parsed only after TCP's peer FIN bounds the full message.
    let response = match http_codec::parse_response(&output[..response_length]) {
        Some(parsed) => parsed,
        None => return HTTP_CLIENT_BAD_RESPONSE,
    };
    let received_body_length = response_length - response.body_start;
    let body_length = if response.chunked {
        match http_codec::decode_chunked_in_place(output, response.body_start, response_length) {
            Some(length) => length,
            None => return HTTP_CLIENT_BAD_RESPONSE,
        }
    } else if let Some(expected_length) = response.content_length {
        if received_body_length != expected_length {
            return HTTP_CLIENT_BAD_RESPONSE;
        }
        expected_length
    } else {
        received_body_length
    };
    if body_length > output.len() {
        return HTTP_CLIENT_RESPONSE_TOO_LARGE;
    }
    output.copy_within(response.body_start..response.body_start + body_length, 0);
    *output_length = body_length;
    *http_status = response.status;

    if connection.send_fin() {
        let _ = send_tcp(
            &config,
            &destination_mac,
            remote_ip,
            local_port,
            port,
            connection.send_next.wrapping_sub(1),
            connection.receive_next,
            TCP_FIN | TCP_ACK,
            &[],
        );
    }
    HTTP_CLIENT_OK
}

#[no_mangle]
/// C ABI entry point; output_length and http_status are set to zero on entry.
pub unsafe extern "C" fn daneels_ritron_http_get(
    hostname: *const u8,
    hostname_length: usize,
    port: u16,
    path: *const u8,
    path_length: usize,
    output: *mut u8,
    output_capacity: usize,
    output_length: *mut usize,
    http_status: *mut u16,
    stall: Stall,
) -> i32 {
    http_get(
        hostname,
        hostname_length,
        port,
        path,
        path_length,
        output,
        output_capacity,
        output_length,
        http_status,
        stall,
    )
}
