// Parsed body framing is kept separate from transport so it can be host-tested.
pub struct HttpResponse {
    pub status: u16,
    pub body_start: usize,
    pub content_length: Option<usize>,
    pub chunked: bool,
}

pub fn build_request(buffer: &mut [u8], hostname: &[u8], port: u16, path: &[u8]) -> Option<usize> {
    // Reject header injection and request-target whitespace before serialization.
    if hostname.is_empty()
        || path.is_empty()
        || path[0] != b'/'
        || hostname
            .iter()
            .any(|byte| !byte.is_ascii_alphanumeric() && !b".-:".contains(byte))
        || path.iter().any(|byte| *byte <= 0x20 || *byte > 0x7E)
    {
        return None;
    }
    let mut offset = 0;
    append(buffer, &mut offset, b"GET ")?;
    append(buffer, &mut offset, path)?;
    append(buffer, &mut offset, b" HTTP/1.1\r\nHost: ")?;
    append(buffer, &mut offset, hostname)?;
    if port != 80 {
        append(buffer, &mut offset, b":")?;
        append_decimal(buffer, &mut offset, port as u32)?;
    }
    append(
        buffer,
        &mut offset,
        b"\r\nConnection: close\r\nUser-Agent: Daneel-RiTron/0.2\r\nAccept: */*\r\n\r\n",
    )?;
    Some(offset)
}

fn append(buffer: &mut [u8], offset: &mut usize, bytes: &[u8]) -> Option<()> {
    let end = offset.checked_add(bytes.len())?;
    if end > buffer.len() {
        return None;
    }
    buffer[*offset..end].copy_from_slice(bytes);
    *offset = end;
    Some(())
}

fn append_decimal(buffer: &mut [u8], offset: &mut usize, mut value: u32) -> Option<()> {
    let mut digits = [0u8; 10];
    let mut count = 0;
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
        append(buffer, offset, &digits[count..count + 1])?;
    }
    Some(())
}

fn equals_ascii_case_insensitive(left: &[u8], right: &[u8]) -> bool {
    left.len() == right.len()
        && left
            .iter()
            .zip(right)
            .all(|(a, b)| a.eq_ignore_ascii_case(b))
}

pub fn parse_response(response: &[u8]) -> Option<HttpResponse> {
    let header_end = response
        .windows(4)
        .position(|window| window == b"\r\n\r\n")?
        + 4;
    let first_line_end = response.windows(2).position(|window| window == b"\r\n")?;
    let status_line = &response[..first_line_end];
    if !(status_line.starts_with(b"HTTP/1.0 ") || status_line.starts_with(b"HTTP/1.1 "))
        || status_line.len() < 12
        || !status_line[9..12].iter().all(u8::is_ascii_digit)
    {
        return None;
    }
    let status = ((status_line[9] - b'0') as u16) * 100
        + ((status_line[10] - b'0') as u16) * 10
        + (status_line[11] - b'0') as u16;
    // Header names are case-insensitive; repeated lengths must agree exactly.
    let mut content_length = None;
    let mut chunked = false;
    let mut transfer_encoding_seen = false;
    let mut cursor = first_line_end + 2;
    while cursor + 1 < header_end {
        let line_end = response[cursor..header_end]
            .windows(2)
            .position(|window| window == b"\r\n")?
            + cursor;
        if line_end == cursor {
            break;
        }
        let line = &response[cursor..line_end];
        let colon = line.iter().position(|byte| *byte == b':')?;
        let name = &line[..colon];
        let value = trim_ascii_whitespace(&line[colon + 1..]);
        if equals_ascii_case_insensitive(name, b"content-length") {
            let parsed = parse_decimal(value)?;
            if content_length.map_or(false, |previous| previous != parsed) {
                return None;
            }
            content_length = Some(parsed);
        } else if equals_ascii_case_insensitive(name, b"transfer-encoding") {
            if transfer_encoding_seen || !equals_ascii_case_insensitive(value, b"chunked") {
                return None;
            }
            transfer_encoding_seen = true;
            chunked = true;
        }
        cursor = line_end + 2;
    }
    if chunked && content_length.is_some() {
        return None;
    }
    Some(HttpResponse {
        status,
        body_start: header_end,
        content_length,
        chunked,
    })
}

fn trim_ascii_whitespace(mut bytes: &[u8]) -> &[u8] {
    while bytes.first().map_or(false, u8::is_ascii_whitespace) {
        bytes = &bytes[1..];
    }
    while bytes.last().map_or(false, u8::is_ascii_whitespace) {
        bytes = &bytes[..bytes.len() - 1];
    }
    bytes
}

fn parse_decimal(bytes: &[u8]) -> Option<usize> {
    if bytes.is_empty() {
        return None;
    }
    let mut value = 0usize;
    for byte in bytes {
        if !byte.is_ascii_digit() {
            return None;
        }
        value = value.checked_mul(10)?.checked_add((byte - b'0') as usize)?;
    }
    Some(value)
}

pub fn decode_chunked_in_place(buffer: &mut [u8], start: usize, end: usize) -> Option<usize> {
    if start > end || end > buffer.len() {
        return None;
    }
    let mut read = start;
    let mut write = start;
    loop {
        // Chunk framing is stripped in place; output never grows beyond input.
        let line_end = find_crlf(buffer, read, end)?;
        let line = &buffer[read..line_end];
        let size_text = line.split(|byte| *byte == b';').next()?;
        let size = parse_hex(size_text)?;
        read = line_end + 2;
        if size == 0 {
            // Validate trailers and require the terminator to consume the response.
            loop {
                let trailer_end = find_crlf(buffer, read, end)?;
                if trailer_end == read {
                    return (trailer_end + 2 == end).then_some(write - start);
                }
                if !buffer[read..trailer_end].contains(&b':') {
                    return None;
                }
                read = trailer_end + 2;
            }
        }
        let data_end = read.checked_add(size)?;
        let next = data_end.checked_add(2)?;
        if next > end || buffer[data_end..next] != *b"\r\n" {
            return None;
        }
        buffer.copy_within(read..data_end, write);
        write += size;
        read = next;
    }
}

fn find_crlf(buffer: &[u8], start: usize, end: usize) -> Option<usize> {
    if start > end || end > buffer.len() {
        return None;
    }
    buffer[start..end]
        .windows(2)
        .position(|window| window == b"\r\n")
        .map(|offset| start + offset)
}

fn parse_hex(bytes: &[u8]) -> Option<usize> {
    if bytes.is_empty() {
        return None;
    }
    let mut value = 0usize;
    for byte in bytes {
        let digit = match byte {
            b'0'..=b'9' => (byte - b'0') as usize,
            b'a'..=b'f' => (byte - b'a' + 10) as usize,
            b'A'..=b'F' => (byte - b'A' + 10) as usize,
            _ => return None,
        };
        value = value.checked_mul(16)?.checked_add(digit)?;
    }
    Some(value)
}

#[cfg(test)]
mod tests {
    use super::{build_request, decode_chunked_in_place, parse_response};

    #[test]
    fn emits_http_11_request_with_hostname_and_close() {
        let mut buffer = [0u8; 256];
        let length = build_request(&mut buffer, b"example.test", 8080, b"/hello").unwrap();
        assert_eq!(
            &buffer[..length],
            b"GET /hello HTTP/1.1\r\nHost: example.test:8080\r\nConnection: close\r\nUser-Agent: Daneel-RiTron/0.2\r\nAccept: */*\r\n\r\n"
        );
        assert!(build_request(&mut buffer, b"bad\r\nhost", 80, b"/").is_none());
    }

    #[test]
    fn reads_content_length_case_insensitively() {
        let response = b"HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: 5\r\n\r\nhello";
        let parsed = parse_response(response).unwrap();
        assert_eq!(parsed.status, 200);
        assert_eq!(&response[parsed.body_start..], b"hello");
        assert_eq!(parsed.content_length, Some(5));
        assert!(!parsed.chunked);
    }

    #[test]
    fn rejects_ambiguous_or_invalid_response_lengths() {
        assert!(parse_response(
            b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\nx"
        )
        .is_none());
        assert!(parse_response(b"HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n").is_none());
        assert!(parse_response(
            b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"
        )
        .is_none());
        assert!(
            parse_response(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n")
                .is_none()
        );
    }

    #[test]
    fn decodes_chunked_body_and_trailers() {
        let mut buffer = *b"4;ok=yes\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: yes\r\n\r\n";
        let length = buffer.len();
        assert_eq!(decode_chunked_in_place(&mut buffer, 0, length), Some(9));
        assert_eq!(&buffer[..9], b"Wikipedia");
    }

    #[test]
    fn rejects_incomplete_chunk() {
        let mut buffer = *b"4\r\nabc";
        let length = buffer.len();
        assert_eq!(decode_chunked_in_place(&mut buffer, 0, length), None);
    }

    #[test]
    fn rejects_trailing_bytes_after_chunk_terminator() {
        let mut buffer = *b"0\r\n\r\nstale";
        let length = buffer.len();
        assert_eq!(decode_chunked_in_place(&mut buffer, 0, length), None);
    }
}
