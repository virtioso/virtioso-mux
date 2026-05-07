// Wire protocol for virtioso-muxd — escape-sequence framing.
//
// Uses the same 0xfe/0xfd escape scheme as the CAmkES inner mux so that
// tcu_muxer's -A mode handles both contexts without a separate parser.
// See docs/architecture/virtioso-mux-wire-protocol.md for full details.
//
// Data frame:
//   [0xfe] [stream_id] [data bytes; 0xfe in data → 0xfe 0xfe] [0xfe] [0x00]
//
// Control frame:
//   [0xfe] [0xfd] [ctrl_type] [len_hi] [len_lo] [payload: len bytes]
//
// Valid stream IDs: 0x01–0xfc.  0xfd and 0xfe are reserved as protocol bytes.

/// Escape byte — begins a stream-switch or control record.
pub const ESC: u8 = 0xfe;
/// After ESC: return cursor to unframed/default state.
pub const ESC_DEFAULT: u8 = 0x00;
/// After ESC: enter control channel.
pub const ESC_CONTROL: u8 = 0xfd;

/// Control type: new stream connected (virtioso-muxd → demuxer).
/// Payload: [stream_id: u8] [name bytes…]
pub const CTRL_CONNECTED: u8 = 0x01;
/// Control type: stream disconnected (virtioso-muxd → demuxer).
/// Payload: [stream_id: u8]
pub const CTRL_DISCONNECTED: u8 = 0x04;

// 0x02 = STREAM_REGISTRY (CAmkES), 0x03 = DOWNLINK_ACK (CAmkES) — not used here.

/// Encode one data frame for `stream_id` carrying `payload`.
/// Any 0xfe byte in `payload` is escaped to 0xfe 0xfe.
pub fn encode_data_frame(stream_id: u8, payload: &[u8], out: &mut Vec<u8>) {
    debug_assert!(stream_id >= 0x01 && stream_id <= 0xfc,
        "stream_id {stream_id:#x} is reserved (must be 0x01–0xfc)");
    out.push(ESC);
    out.push(stream_id);
    for &b in payload {
        if b == ESC {
            out.push(ESC); // escape prefix for literal 0xfe
        }
        out.push(b);
    }
    out.push(ESC);
    out.push(ESC_DEFAULT);
}

/// Encode a control frame (CTRL_CONNECTED or CTRL_DISCONNECTED).
/// Payload layout: [stream_id: u8] [name bytes…]
pub fn encode_control_frame(ctrl_type: u8, stream_id: u8, name: &str, out: &mut Vec<u8>) {
    let name_bytes = name.as_bytes();
    let payload_len = 1 + name_bytes.len(); // stream_id byte + name
    let payload_len = payload_len.min(u16::MAX as usize);
    let len_hi = (payload_len >> 8) as u8;
    let len_lo = payload_len as u8;
    out.push(ESC);
    out.push(ESC_CONTROL);
    out.push(ctrl_type);
    out.push(len_hi);
    out.push(len_lo);
    out.push(stream_id);
    let name_len = payload_len - 1;
    out.extend_from_slice(&name_bytes[..name_len]);
}
