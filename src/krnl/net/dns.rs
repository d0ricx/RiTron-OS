const DNS_HEADER_LENGTH: usize = 12;

// Shell endpoints are ASCII strings; numeric IPv4 avoids a DNS round trip.
pub fn parse_ipv4(address: &[u8]) -> Option<u32> {
    let mut value = 0u32;
    let mut octets = 0;
    let mut digits = 0;
    let mut octet = 0u32;
    for byte in address.iter().copied().chain(core::iter::once(b'.')) {
        if byte.is_ascii_digit() {
            octet = octet.checked_mul(10)?.checked_add((byte - b'0') as u32)?;
            if octet > 255 {
                return None;
            }
            digits += 1;
        } else if byte == b'.' {
            if digits == 0 || octets >= 4 {
                return None;
            }
            value = (value << 8) | octet;
            octets += 1;
            digits = 0;
            octet = 0;
        } else {
            return None;
        }
    }
    if octets == 4 {
        Some(value)
    } else {
        None
    }
}

fn normalized_name(name: &[u8]) -> Option<&[u8]> {
    // DNS wire labels are limited to 63 octets and the full name to 253.
    let name = if name.last() == Some(&b'.') {
        &name[..name.len() - 1]
    } else {
        name
    };
    if name.is_empty() || name.len() > 253 {
        return None;
    }
    let mut label_length = 0usize;
    let mut label_start = true;
    let mut previous = 0;
    for byte in name.iter().copied() {
        if byte == b'.' {
            if label_length == 0 || previous == b'-' {
                return None;
            }
            label_length = 0;
            label_start = true;
        } else if byte.is_ascii_alphanumeric() || byte == b'-' {
            if label_start && byte == b'-' {
                return None;
            }
            label_length += 1;
            if label_length > 63 {
                return None;
            }
            label_start = false;
        } else {
            return None;
        }
        previous = byte;
    }
    if label_length == 0 || previous == b'-' {
        return None;
    }
    Some(name)
}

pub fn build_query(id: u16, hostname: &[u8], output: &mut [u8]) -> Option<usize> {
    let hostname = normalized_name(hostname)?;
    if output.len() < DNS_HEADER_LENGTH {
        return None;
    }
    output[..DNS_HEADER_LENGTH].fill(0);
    // Standard recursive query: one IN-class A question, no optional sections.
    output[0] = (id >> 8) as u8;
    output[1] = id as u8;
    output[2] = 1;
    output[5] = 1;

    let mut offset = DNS_HEADER_LENGTH;
    let mut label_start = 0;
    for index in 0..=hostname.len() {
        if index == hostname.len() || hostname[index] == b'.' {
            let label = &hostname[label_start..index];
            if offset + label.len() + 1 + 4 > output.len() {
                return None;
            }
            output[offset] = label.len() as u8;
            offset += 1;
            for byte in label {
                output[offset] = byte.to_ascii_lowercase();
                offset += 1;
            }
            label_start = index + 1;
        }
    }
    output[offset] = 0;
    offset += 1;
    output[offset..offset + 4].copy_from_slice(&[0, 1, 0, 1]);
    Some(offset + 4)
}

fn decode_name(packet: &[u8], start: usize, output: &mut [u8; 254]) -> Option<(usize, usize)> {
    if start >= packet.len() {
        return None;
    }
    let mut cursor = start;
    let mut consumed = 0usize;
    let mut jumped = false;
    let mut written = 0usize;
    let mut steps = 0usize;
    loop {
        if cursor >= packet.len() || steps > packet.len() {
            return None;
        }
        steps += 1;
        let length = packet[cursor];
        if length & 0xC0 == 0xC0 {
            if cursor + 1 >= packet.len() {
                return None;
            }
            let pointer = (((length & 0x3F) as usize) << 8) | packet[cursor + 1] as usize;
            if pointer >= packet.len() {
                return None;
            }
            if !jumped {
                // The caller advances past the pointer, not to the pointed-to name.
                consumed = cursor + 2 - start;
                jumped = true;
            }
            cursor = pointer;
            continue;
        }
        if length & 0xC0 != 0 || length > 63 {
            return None;
        }
        cursor += 1;
        if length == 0 {
            if !jumped {
                consumed = cursor - start;
            }
            return Some((written, consumed));
        }
        // Label bytes may not cross the received DNS message boundary.
        let end = cursor.checked_add(length as usize)?;
        if end > packet.len()
            || written + length as usize + usize::from(written != 0) > output.len()
        {
            return None;
        }
        if written != 0 {
            output[written] = b'.';
            written += 1;
        }
        for byte in &packet[cursor..end] {
            output[written] = byte.to_ascii_lowercase();
            written += 1;
        }
        cursor = end;
        if !jumped {
            consumed = cursor - start;
        }
    }
}

pub fn parse_response(packet: &[u8], id: u16, hostname: &[u8]) -> Option<u32> {
    let hostname = normalized_name(hostname)?;
    if packet.len() < DNS_HEADER_LENGTH
        || packet[0] != (id >> 8) as u8
        || packet[1] != id as u8
        || packet[2] & 0x80 == 0
        || packet[2] & 0x78 != 0
        || packet[2] & 0x02 != 0
        || packet[3] & 0x0F != 0
    {
        return None;
    }
    let questions = u16::from_be_bytes([packet[4], packet[5]]);
    let answers = u16::from_be_bytes([packet[6], packet[7]]);
    if questions != 1 || answers == 0 {
        return None;
    }
    // Follow CNAMEs only within this answer section; no extra DNS transaction.
    let mut current_name = [0u8; 254];
    for (destination, source) in current_name.iter_mut().zip(hostname) {
        *destination = source.to_ascii_lowercase();
    }
    let mut current_name_length = hostname.len();
    let mut offset = DNS_HEADER_LENGTH;
    let mut question_name = [0u8; 254];
    let (question_length, question_consumed) = decode_name(packet, offset, &mut question_name)?;
    if question_length != hostname.len()
        || !question_name[..question_length]
            .iter()
            .zip(hostname)
            .all(|(left, right)| left.eq_ignore_ascii_case(right))
    {
        return None;
    }
    offset = offset.checked_add(question_consumed)?;
    if offset + 4 > packet.len() || packet[offset..offset + 4] != [0, 1, 0, 1] {
        return None;
    }
    offset += 4;

    for _ in 0..answers {
        let mut answer_name = [0u8; 254];
        let (answer_name_length, consumed) = decode_name(packet, offset, &mut answer_name)?;
        offset = offset.checked_add(consumed)?;
        if offset + 10 > packet.len() {
            return None;
        }
        let record_type = u16::from_be_bytes([packet[offset], packet[offset + 1]]);
        let class = u16::from_be_bytes([packet[offset + 2], packet[offset + 3]]);
        let data_length = u16::from_be_bytes([packet[offset + 8], packet[offset + 9]]) as usize;
        offset += 10;
        let data_end = offset.checked_add(data_length)?;
        if data_end > packet.len() {
            return None;
        }
        let owner_matches = answer_name_length == current_name_length
            && answer_name[..answer_name_length] == current_name[..current_name_length];
        // A records are accepted only for the queried name or its in-packet alias.
        if owner_matches && record_type == 1 && class == 1 && data_length == 4 {
            return Some(
                ((packet[offset] as u32) << 24)
                    | ((packet[offset + 1] as u32) << 16)
                    | ((packet[offset + 2] as u32) << 8)
                    | packet[offset + 3] as u32,
            );
        }
        if owner_matches && record_type == 5 && class == 1 {
            let mut canonical_name = [0u8; 254];
            let (canonical_length, consumed) = decode_name(packet, offset, &mut canonical_name)?;
            if consumed != data_length {
                return None;
            }
            current_name[..canonical_length].copy_from_slice(&canonical_name[..canonical_length]);
            current_name_length = canonical_length;
        }
        offset = data_end;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::{build_query, parse_ipv4, parse_response};

    #[test]
    fn parses_ipv4_without_truncating_invalid_octets() {
        assert_eq!(parse_ipv4(b"10.0.2.3"), Some(0x0A000203));
        assert_eq!(parse_ipv4(b"10.0.256.3"), None);
        assert_eq!(parse_ipv4(b"10.0.2"), None);
        assert_eq!(parse_ipv4(b"10..2.3"), None);
    }

    #[test]
    fn builds_dns_a_query() {
        let mut packet = [0u8; 64];
        let length = build_query(0x1234, b"Example.com", &mut packet).unwrap();
        assert_eq!(&packet[..2], &[0x12, 0x34]);
        assert_eq!(&packet[12..25], b"\x07example\x03com\x00");
        assert_eq!(&packet[length - 4..length], &[0, 1, 0, 1]);
        assert!(build_query(1, b"-bad.example", &mut packet).is_none());
    }

    #[test]
    fn resolves_compressed_dns_a_response() {
        let mut response = [0u8; 64];
        let question_length = build_query(0x3412, b"example.test", &mut response).unwrap();
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0;
        response[7] = 1;
        response[question_length] = 0xC0;
        response[question_length + 1] = 0x0C;
        response[question_length + 2..question_length + 12]
            .copy_from_slice(&[0, 1, 0, 1, 0, 0, 0, 30, 0, 4]);
        response[question_length + 12..question_length + 16].copy_from_slice(&[203, 0, 113, 9]);
        assert_eq!(
            parse_response(&response[..question_length + 16], 0x3412, b"example.test"),
            Some(0xCB007109)
        );
    }

    #[test]
    fn parses_qemu_user_network_dns_response() {
        let response = [
            0x00, 0x01, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x07, b'e',
            b'x', b'a', b'm', b'p', b'l', b'e', 0x03, b'c', b'o', b'm', 0x00, 0x00, 0x01, 0x00,
            0x01, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x29, 0x00, 0x04, 0xAC,
            0x42, 0x93, 0xF3, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x29, 0x00,
            0x04, 0x68, 0x14, 0x17, 0x9A,
        ];
        assert_eq!(
            parse_response(&response, 1, b"example.com"),
            Some(0xAC4293F3)
        );
    }

    #[test]
    fn follows_cname_within_the_answer_section() {
        let mut response = [0u8; 128];
        let mut offset = build_query(0x4567, b"example.test", &mut response).unwrap();
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0;
        response[7] = 2;

        response[offset..offset + 2].copy_from_slice(&[0xC0, 0x0C]);
        response[offset + 2..offset + 10].copy_from_slice(&[0, 5, 0, 1, 0, 0, 0, 30]);
        response[offset + 10..offset + 12].copy_from_slice(&[0, 19]);
        offset += 12;
        let canonical_name_offset = offset;
        response[offset..offset + 19].copy_from_slice(b"\x04edge\x07example\x04test\x00");
        offset += 19;

        response[offset..offset + 2].copy_from_slice(&[
            0xC0 | ((canonical_name_offset >> 8) as u8),
            canonical_name_offset as u8,
        ]);
        response[offset + 2..offset + 12].copy_from_slice(&[0, 1, 0, 1, 0, 0, 0, 30, 0, 4]);
        response[offset + 12..offset + 16].copy_from_slice(&[192, 0, 2, 45]);
        offset += 16;
        assert_eq!(
            parse_response(&response[..offset], 0x4567, b"example.test"),
            Some(0xC000022D)
        );
    }

    #[test]
    fn ignores_an_a_record_for_an_unrelated_owner() {
        let mut response = [0u8; 64];
        let question_length = build_query(0x1357, b"example.test", &mut response).unwrap();
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0;
        response[7] = 1;
        response[question_length] = 1;
        response[question_length + 1] = b'x';
        response[question_length + 2] = 0;
        response[question_length + 3..question_length + 13]
            .copy_from_slice(&[0, 1, 0, 1, 0, 0, 0, 30, 0, 4]);
        response[question_length + 13..question_length + 17].copy_from_slice(&[203, 0, 113, 9]);
        assert_eq!(
            parse_response(&response[..question_length + 17], 0x1357, b"example.test"),
            None
        );
    }

    #[test]
    fn rejects_dns_compression_pointer_loops() {
        let mut response = [0u8; 32];
        response[0..2].copy_from_slice(&[0, 7]);
        response[2] = 0x81;
        response[4..6].copy_from_slice(&[0, 1]);
        response[6..8].copy_from_slice(&[0, 1]);
        response[12..14].copy_from_slice(&[0xC0, 12]);
        assert_eq!(parse_response(&response, 7, b"example.test"), None);
    }
}
