// Wire protocol for virtioso-muxd.
//
// Frame layout:
//   [0xfe magic: u8] [stream_id: u8] [payload_len: u16 LE] [payload: payload_len bytes]
//
// stream_id 0 is reserved for control frames.
// All other stream IDs carry application data.

pub const FRAME_MAGIC: u8 = 0xfe;
pub const CONTROL_STREAM_ID: u8 = 0x00;

/// Control frame types carried in the first byte of a control frame payload.
pub const CTRL_CONNECTED: u8 = 0x01;
pub const CTRL_DISCONNECTED: u8 = 0x02;

/// Enrollment request: mux-exec → muxd, before any data.
/// Layout: [name_len: u8] [name bytes: name_len]
/// Response: [stream_id: u8]  (0 = rejected)

pub fn encode_data_frame(stream_id: u8, payload: &[u8], out: &mut Vec<u8>) {
    debug_assert_ne!(stream_id, CONTROL_STREAM_ID);
    let len = payload.len().min(u16::MAX as usize);
    out.push(FRAME_MAGIC);
    out.push(stream_id);
    out.extend_from_slice(&(len as u16).to_le_bytes());
    out.extend_from_slice(&payload[..len]);
}

/// Control frame payload: [ctrl_type: u8] [stream_id: u8] [name bytes (no NUL)]
pub fn encode_control_frame(ctrl_type: u8, stream_id: u8, name: &str, out: &mut Vec<u8>) {
    let name_bytes = name.as_bytes();
    let payload_len = (2 + name_bytes.len()).min(u16::MAX as usize);
    out.push(FRAME_MAGIC);
    out.push(CONTROL_STREAM_ID);
    out.extend_from_slice(&(payload_len as u16).to_le_bytes());
    out.push(ctrl_type);
    out.push(stream_id);
    out.extend_from_slice(&name_bytes[..payload_len - 2]);
}
