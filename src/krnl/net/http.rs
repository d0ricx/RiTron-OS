//! Synchronous plain-HTTP client used by the kernel's `httpget` command.
//!
//! This is intentionally a small, single-connection implementation for the
//! polled e1000 driver. It does not provide DNS, TLS, IP fragmentation, or a
//! general-purpose TCP retransmission/reassembly layer.
#![no_std]

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
const TCP_FIN: u8 = 0x01;
const TCP_SYN: u8 = 0x02;
const TCP_RST: u8 = 0x04;
const TCP_PSH: u8 = 0x08;
const TCP_ACK: u8 = 0x10;

const HTTP_CLIENT_OK: i32 = 0;
const HTTP_CLIENT_INVALID_ARGUMENT: i32 = 1;
const HTTP_CLIENT_NO_NETWORK: i32 = 2;
const HTTP_CLIENT_ARP_TIMEOUT: i32 = 3;
const HTTP_CLIENT_TCP_TIMEOUT: i32 = 4;
const HTTP_CLIENT_CONNECTION_RESET: i32 = 5;
const HTTP_CLIENT_BAD_RESPONSE: i32 = 6;
const HTTP_CLIENT_RESPONSE_TOO_LARGE: i32 = 7;
const HTTP_CLIENT_TRANSMIT_ERROR: i32 = 8;

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

// Kernel shell requests are serialized, so one mutable ephemeral-port counter suffices.
static mut NEXT_EPHEMERAL_PORT: u16 = 49152;

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

// Ethernet frames cross the C ABI synchronously; the driver copies TX data before returning.
fn send_frame(frame: &[u8]) -> bool {
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

fn resolve_arp(config: &NetworkConfig, target: u32, stall: Stall) -> Option<[u8; 6]> {
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
                return Some(address);
            }
        }
        stall(10_000);
        tick += 1;
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
    // Reject unrelated or malformed frames before exposing any packet offsets.
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

fn append_decimal(buffer: &mut [u8], mut value: u8, mut offset: usize) -> usize {
    let mut digits = [0u8; 3];
    let mut count = 0;
    loop {
        digits[count] = b'0' + value % 10;
        count += 1;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    while count > 0 {
        count -= 1;
        buffer[offset] = digits[count];
        offset += 1;
    }
    offset
}

fn build_http_request(
    buffer: &mut [u8; MAX_HTTP_REQUEST_LENGTH],
    remote_ip: u32,
    port: u16,
    path: &[u8],
) -> Option<usize> {
    // Limit paths to printable ASCII and form an HTTP/1.0 request without allocation.
    if path.is_empty() || path[0] != b'/' {
        return None;
    }
    let mut offset = 0;
    for byte in b"GET " {
        buffer[offset] = *byte;
        offset += 1;
    }
    for byte in path {
        if *byte <= 0x20 || *byte > 0x7E {
            return None;
        }
        buffer[offset] = *byte;
        offset += 1;
    }
    for byte in b" HTTP/1.0\r\nHost: " {
        buffer[offset] = *byte;
        offset += 1;
    }
    for shift in [24, 16, 8, 0] {
        offset = append_decimal(buffer, (remote_ip >> shift) as u8, offset);
        if shift != 0 {
            buffer[offset] = b'.';
            offset += 1;
        }
    }
    if port != 80 {
        for byte in b":" {
            buffer[offset] = *byte;
            offset += 1;
        }
        let mut digits = [0u8; 5];
        let mut count = 0;
        let mut value = port;
        loop {
            digits[count] = b'0' + (value % 10) as u8;
            count += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        while count > 0 {
            count -= 1;
            buffer[offset] = digits[count];
            offset += 1;
        }
    }
    for byte in b"\r\nConnection: close\r\nUser-Agent: Daneel's-RiTron/0.1\r\n\r\n" {
        buffer[offset] = *byte;
        offset += 1;
    }
    if offset > buffer.len() {
        None
    } else {
        Some(offset)
    }
}

fn parse_http_status(response: &[u8]) -> Option<(u16, usize)> {
    // Only the status line and header terminator are interpreted; body starts after CRLFCRLF.
    let mut header_end = None;
    let mut offset = 0;
    while offset + 3 < response.len() {
        if response[offset] == b'\r'
            && response[offset + 1] == b'\n'
            && response[offset + 2] == b'\r'
            && response[offset + 3] == b'\n'
        {
            header_end = Some(offset + 4);
            break;
        }
        offset += 1;
    }
    let body_start = header_end?;
    if response.len() < 12
        || !response.starts_with(b"HTTP/")
        || response[8] != b' '
        || !response[9].is_ascii_digit()
        || !response[10].is_ascii_digit()
        || !response[11].is_ascii_digit()
    {
        return None;
    }
    let status = ((response[9] - b'0') as u16) * 100
        + ((response[10] - b'0') as u16) * 10
        + (response[11] - b'0') as u16;
    Some((status, body_start))
}

fn route_address(config: &NetworkConfig, destination: u32) -> Option<u32> {
    // Use the destination directly on-link, otherwise ARP for the configured gateway.
    if destination == 0 || destination == u32::MAX {
        return None;
    }
    if config.subnet_mask == 0
        || destination & config.subnet_mask == config.ipv4_address & config.subnet_mask
    {
        Some(destination)
    } else if config.gateway != 0 {
        Some(config.gateway)
    } else {
        None
    }
}

unsafe fn http_get(
    remote_ip: u32,
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
    if remote_ip == 0
        || path_pointer.is_null()
        || output_pointer.is_null()
        || output_length.is_null()
        || http_status.is_null()
        || output_capacity < MAX_HTTP_REQUEST_LENGTH
        || path_length == 0
        || path_length > 400
        || port == 0
    {
        return HTTP_CLIENT_INVALID_ARGUMENT;
    }
    // Caller guarantees these regions are valid for their declared lengths for this call.
    let path = slice::from_raw_parts(path_pointer, path_length);
    let output = slice::from_raw_parts_mut(output_pointer, output_capacity);
    let output_length = &mut *output_length;
    let http_status = &mut *http_status;
    *output_length = 0;
    *http_status = 0;

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
    let mut client_sequence = initial_sequence;
    let mut server_sequence = 0u32;
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
            client_sequence,
            0,
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
                    return HTTP_CLIENT_CONNECTION_RESET;
                }
                if tcp.flags & (TCP_SYN | TCP_ACK) == (TCP_SYN | TCP_ACK)
                    && tcp.acknowledgement == client_sequence.wrapping_add(1)
                {
                    server_sequence = tcp.sequence.wrapping_add(1);
                    client_sequence = client_sequence.wrapping_add(1);
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
        client_sequence,
        server_sequence,
        TCP_ACK,
        &[],
    ) {
        return HTTP_CLIENT_TRANSMIT_ERROR;
    }

    // Send one complete request segment; the implementation is not a streaming TCP stack.
    let mut request = [0u8; MAX_HTTP_REQUEST_LENGTH];
    let request_length = match build_http_request(&mut request, remote_ip, port, path) {
        Some(length) => length,
        None => return HTTP_CLIENT_INVALID_ARGUMENT,
    };
    let request_sequence = client_sequence;
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
        client_sequence,
        server_sequence,
        TCP_ACK | TCP_PSH,
        &request[..request_length],
    ) {
        return HTTP_CLIENT_TRANSMIT_ERROR;
    }
    client_sequence = client_sequence.wrapping_add(request_length as u32);

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
                return HTTP_CLIENT_CONNECTION_RESET;
            }
            let acknowledged_bytes = tcp.acknowledgement.wrapping_sub(request_sequence);
            if tcp.acknowledgement == client_sequence
                || (acknowledged_bytes >= request_length as u32 && acknowledged_bytes < 0x8000_0000)
            {
                request_acknowledged = true;
            }
            if tcp.payload_length != 0 {
                if tcp.sequence == server_sequence {
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
                    server_sequence = server_sequence.wrapping_add(tcp.payload_length as u32);
                }
                if !send_tcp(
                    &config,
                    &destination_mac,
                    remote_ip,
                    local_port,
                    port,
                    client_sequence,
                    server_sequence,
                    TCP_ACK,
                    &[],
                ) {
                    return HTTP_CLIENT_TRANSMIT_ERROR;
                }
            }
            if tcp.flags & TCP_FIN != 0
                && tcp.sequence.wrapping_add(tcp.payload_length as u32) == server_sequence
            {
                server_sequence = server_sequence.wrapping_add(1);
                server_closed = true;
                let _ = send_tcp(
                    &config,
                    &destination_mac,
                    remote_ip,
                    local_port,
                    port,
                    client_sequence,
                    server_sequence,
                    TCP_ACK,
                    &[],
                );
                break;
            }
        } else {
            stall(10_000);
            elapsed += 1;
        }
        if !request_acknowledged && elapsed != 0 && elapsed % RETRY_TICKS == 0 {
            if !send_tcp(
                &config,
                &destination_mac,
                remote_ip,
                local_port,
                port,
                request_sequence,
                server_sequence,
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

    // Compact the response body over its headers so callers receive body-only bytes.
    let (status, body_start) = match parse_http_status(&output[..response_length]) {
        Some(parsed) => parsed,
        None => return HTTP_CLIENT_BAD_RESPONSE,
    };
    let body_length = response_length - body_start;
    for index in 0..body_length {
        output[index] = output[body_start + index];
    }
    *output_length = body_length;
    *http_status = status;

    let _ = send_tcp(
        &config,
        &destination_mac,
        remote_ip,
        local_port,
        port,
        client_sequence,
        server_sequence,
        TCP_FIN | TCP_ACK,
        &[],
    );
    HTTP_CLIENT_OK
}

#[no_mangle]
/// C ABI entry point; output_length and http_status are set to zero on entry.
pub unsafe extern "C" fn daneels_ritron_http_get(
    remote_ip: u32,
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
        remote_ip,
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
