#[derive(Clone, Copy, Debug, PartialEq, Eq)]
// This client needs only states for one HTTP exchange and orderly shutdown.
pub enum State {
    SynSent,
    Established,
    CloseWait,
    LastAck,
    Reset,
}

pub struct Connection {
    // send_next is the next sequence number to transmit; SYN consumes one byte.
    pub state: State,
    pub send_unacknowledged: u32,
    pub send_next: u32,
    pub receive_next: u32,
}

impl Connection {
    pub fn new(initial_sequence: u32) -> Self {
        Self {
            state: State::SynSent,
            send_unacknowledged: initial_sequence,
            send_next: initial_sequence.wrapping_add(1),
            receive_next: 0,
        }
    }

    pub fn accept_syn_ack(&mut self, sequence: u32, acknowledgement: u32) -> bool {
        if self.state != State::SynSent || acknowledgement != self.send_next {
            return false;
        }
        self.send_unacknowledged = acknowledgement;
        self.receive_next = sequence.wrapping_add(1);
        self.state = State::Established;
        true
    }

    pub fn acknowledge(&mut self, acknowledgement: u32) -> bool {
        let advance = acknowledgement.wrapping_sub(self.send_unacknowledged);
        let outstanding = self.send_next.wrapping_sub(self.send_unacknowledged);
        // Reject acknowledgements beyond sent data or behind in serial-number space.
        if advance > outstanding || advance >= 0x8000_0000 {
            return false;
        }
        self.send_unacknowledged = acknowledgement;
        true
    }

    pub fn send_data(&mut self, length: usize) -> bool {
        if self.state != State::Established || length > u32::MAX as usize {
            return false;
        }
        self.send_next = self.send_next.wrapping_add(length as u32);
        true
    }

    pub fn expects_segment(&self, sequence: u32) -> bool {
        in_order_segment(self.receive_next, sequence)
    }

    pub fn receive_data(&mut self, sequence: u32, length: usize) -> bool {
        if self.state != State::Established
            || !self.expects_segment(sequence)
            || length > u32::MAX as usize
        {
            return false;
        }
        self.receive_next = self.receive_next.wrapping_add(length as u32);
        true
    }

    pub fn receive_fin(&mut self, sequence: u32, payload_length: usize) -> bool {
        if self.state != State::Established
            || next_receive_sequence(sequence, payload_length, 0) != self.receive_next
        {
            return false;
        }
        // FIN consumes one sequence number after any payload in its segment.
        self.receive_next = self.receive_next.wrapping_add(1);
        self.state = State::CloseWait;
        true
    }

    pub fn send_fin(&mut self) -> bool {
        if self.state != State::CloseWait {
            return false;
        }
        self.send_next = self.send_next.wrapping_add(1);
        self.state = State::LastAck;
        true
    }

    pub fn reset(&mut self) {
        self.state = State::Reset;
    }
}

pub fn acknowledgement_covers(acknowledgement: u32, start: u32, length: usize) -> bool {
    // The client transmits one application segment; only its exact end ACK completes it.
    let acknowledged = acknowledgement.wrapping_sub(start);
    acknowledged == length as u32 && acknowledged < 0x8000_0000
}

pub fn in_order_segment(expected_sequence: u32, sequence: u32) -> bool {
    expected_sequence == sequence
}

pub fn next_receive_sequence(sequence: u32, payload_length: usize, flags: u8) -> u32 {
    sequence
        .wrapping_add(payload_length as u32)
        .wrapping_add(u32::from(flags & 0x02 != 0))
        .wrapping_add(u32::from(flags & 0x01 != 0))
}

#[cfg(test)]
mod tests {
    use super::{
        acknowledgement_covers, in_order_segment, next_receive_sequence, Connection, State,
    };

    #[test]
    fn tracks_sequence_wraparound_and_control_bytes() {
        assert_eq!(next_receive_sequence(u32::MAX, 1, 0), 0);
        assert_eq!(next_receive_sequence(10, 4, 0x03), 16);
    }

    #[test]
    fn recognizes_only_in_order_payload_and_covering_acknowledgements() {
        assert!(in_order_segment(22, 22));
        assert!(!in_order_segment(22, 21));
        assert!(acknowledgement_covers(4, u32::MAX - 2, 7));
        assert!(!acknowledgement_covers(2, 10, 5));
        assert!(!acknowledgement_covers(16, 10, 5));
    }

    #[test]
    fn enforces_connection_state_and_sequence_progression() {
        let mut connection = Connection::new(u32::MAX);
        assert_eq!(connection.state, State::SynSent);
        assert!(!connection.accept_syn_ack(100, 1));
        assert!(connection.accept_syn_ack(100, 0));
        assert_eq!(connection.state, State::Established);
        assert_eq!(connection.send_next, 0);
        assert_eq!(connection.receive_next, 101);
        assert!(connection.send_data(5));
        assert!(!connection.acknowledge(6));
        assert!(connection.acknowledge(5));
        assert!(!connection.receive_data(100, 1));
        assert!(connection.receive_data(101, 2));
        assert!(!connection.receive_fin(104, 0));
        assert!(!connection.receive_fin(103, 2));
        assert!(connection.receive_data(103, 2));
        assert!(connection.receive_fin(103, 2));
        assert_eq!(connection.receive_next, 106);
        assert!(connection.send_fin());
        assert_eq!(connection.state, State::LastAck);
        assert!(!connection.send_data(1));
        connection.reset();
        assert_eq!(connection.state, State::Reset);
    }
}
