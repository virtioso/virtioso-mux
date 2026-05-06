use std::os::unix::fs::PermissionsExt;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixListener;
use tokio::sync::{mpsc, Mutex};
use clap::Parser;
use virtioso_mux_proto as proto;

const DEFAULT_SOCKET: &str = "/run/virtioso-mux/control.sock";
const MAX_READ: usize = 4096;

#[derive(Parser)]
#[command(about = "Virtioso stream multiplexer daemon")]
struct Args {
    /// Sink: uart:/dev/ttyS1, tcp:addr:port, or file:/path
    #[arg(long, env = "VIRTIOSO_MUX_SINK", default_value = "uart:/dev/ttyS1")]
    sink: String,

    /// Unix socket path to listen on
    #[arg(long, env = "VIRTIOSO_MUX_SOCKET", default_value = DEFAULT_SOCKET)]
    socket: String,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let sink = match open_sink(&args.sink).await {
        Ok(s) => s,
        Err(e) => {
            eprintln!("virtioso-muxd: cannot open sink {}: {e}", args.sink);
            std::process::exit(1);
        }
    };

    // Single channel serialises all frame writes to the sink.
    let (tx, rx) = mpsc::channel::<Vec<u8>>(256);
    tokio::spawn(sink_task(rx, sink));

    let id_pool = Arc::new(Mutex::new(IdPool::new()));

    if let Some(parent) = std::path::Path::new(&args.socket).parent() {
        std::fs::create_dir_all(parent).ok();
    }
    let _ = std::fs::remove_file(&args.socket);
    let listener = match UnixListener::bind(&args.socket) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("virtioso-muxd: cannot bind {}: {e}", args.socket);
            std::process::exit(1);
        }
    };
    std::fs::set_permissions(&args.socket, std::fs::Permissions::from_mode(0o666)).ok();

    eprintln!("virtioso-muxd: listening on {} sink={}", args.socket, args.sink);

    loop {
        match listener.accept().await {
            Ok((conn, _)) => {
                tokio::spawn(handle_client(conn, tx.clone(), id_pool.clone()));
            }
            Err(e) => {
                eprintln!("virtioso-muxd: accept error: {e}");
            }
        }
    }
}

async fn handle_client(
    mut conn: tokio::net::UnixStream,
    tx: mpsc::Sender<Vec<u8>>,
    id_pool: Arc<Mutex<IdPool>>,
) {
    // Enrollment: [name_len: u8] [name bytes]
    let name_len = match conn.read_u8().await {
        Ok(n) if n > 0 => n as usize,
        _ => return,
    };
    let mut name_buf = vec![0u8; name_len];
    if conn.read_exact(&mut name_buf).await.is_err() {
        return;
    }
    let name = String::from_utf8_lossy(&name_buf).into_owned();

    let id = id_pool.lock().await.alloc();

    if conn.write_u8(id).await.is_err() {
        id_pool.lock().await.free(id);
        return;
    }

    let mut frame = Vec::new();
    proto::encode_control_frame(proto::CTRL_CONNECTED, id, &name, &mut frame);
    let _ = tx.send(frame).await;

    eprintln!("virtioso-muxd: [{id}] connected: {name}");

    let mut buf = vec![0u8; MAX_READ];
    loop {
        match conn.read(&mut buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                let mut frame = Vec::new();
                proto::encode_data_frame(id, &buf[..n], &mut frame);
                if tx.send(frame).await.is_err() {
                    break;
                }
            }
        }
    }

    let mut frame = Vec::new();
    proto::encode_control_frame(proto::CTRL_DISCONNECTED, id, &name, &mut frame);
    let _ = tx.send(frame).await;

    id_pool.lock().await.free(id);
    eprintln!("virtioso-muxd: [{id}] disconnected: {name}");
}

async fn sink_task(
    mut rx: mpsc::Receiver<Vec<u8>>,
    mut sink: Box<dyn tokio::io::AsyncWrite + Send + Unpin>,
) {
    while let Some(frame) = rx.recv().await {
        if sink.write_all(&frame).await.is_err() {
            eprintln!("virtioso-muxd: sink write error, stopping");
            break;
        }
    }
}

async fn open_sink(
    spec: &str,
) -> Result<Box<dyn tokio::io::AsyncWrite + Send + Unpin>, Box<dyn std::error::Error>> {
    if let Some(path) = spec.strip_prefix("uart:") {
        let f = tokio::fs::OpenOptions::new().write(true).open(path).await?;
        Ok(Box::new(f))
    } else if let Some(path) = spec.strip_prefix("file:") {
        let f = tokio::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)
            .await?;
        Ok(Box::new(f))
    } else if let Some(addr) = spec.strip_prefix("tcp:") {
        let listener = tokio::net::TcpListener::bind(addr).await?;
        eprintln!("virtioso-muxd: waiting for demuxer on {addr}...");
        let (stream, peer) = listener.accept().await?;
        eprintln!("virtioso-muxd: demuxer connected from {peer}");
        Ok(Box::new(stream))
    } else {
        Err(format!("unknown sink spec '{spec}' — use uart:, file:, or tcp:").into())
    }
}

struct IdPool {
    next: u8,
    // Tracks which IDs are currently in use to handle wrap-around correctly.
    used: [bool; 256],
}

impl IdPool {
    fn new() -> Self {
        let mut used = [false; 256];
        used[0] = true; // 0 is reserved for control frames
        Self { next: 1, used }
    }

    fn alloc(&mut self) -> u8 {
        let start = self.next;
        loop {
            let id = self.next;
            self.next = self.next.wrapping_add(1);
            if self.next == 0 {
                self.next = 1;
            }
            if !self.used[id as usize] {
                self.used[id as usize] = true;
                return id;
            }
            if self.next == start {
                // All 255 slots occupied — pathological, return 0 (rejected by client).
                return 0;
            }
        }
    }

    fn free(&mut self, id: u8) {
        if id != 0 {
            self.used[id as usize] = false;
        }
    }
}
