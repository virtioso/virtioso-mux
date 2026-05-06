use std::process::Stdio;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::process::Command;
use tokio::sync::mpsc;
use clap::Parser;

const DEFAULT_SOCKET: &str = "/run/virtioso-mux/control.sock";

#[derive(Parser)]
#[command(
    about = "Run a command with its output multiplexed via virtioso-muxd",
    trailing_var_arg = true
)]
struct Args {
    /// Stream name as it will appear in demuxed output
    #[arg(long)]
    name: String,

    /// Socket path of the running virtioso-muxd instance
    #[arg(long, default_value = DEFAULT_SOCKET)]
    socket: String,

    /// How long to wait for the socket to appear (seconds)
    #[arg(long, default_value_t = 10)]
    connect_timeout: u64,

    /// Command and arguments to execute
    #[arg(required = true, allow_hyphen_values = true)]
    cmd: Vec<String>,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let mut conn = match connect_with_retry(&args.socket, args.connect_timeout).await {
        Ok(c) => c,
        Err(e) => {
            eprintln!("virtioso-mux-exec: cannot connect to {}: {e}", args.socket);
            std::process::exit(1);
        }
    };

    // Enrollment: send [name_len: u8][name bytes], receive [stream_id: u8]
    let name = args.name.as_bytes();
    if name.len() > u8::MAX as usize {
        eprintln!("virtioso-mux-exec: name too long (max 255 bytes)");
        std::process::exit(1);
    }
    if conn.write_u8(name.len() as u8).await.is_err()
        || conn.write_all(name).await.is_err()
    {
        eprintln!("virtioso-mux-exec: enrollment write failed");
        std::process::exit(1);
    }
    let stream_id = match conn.read_u8().await {
        Ok(0) => {
            eprintln!("virtioso-mux-exec: muxd rejected enrollment (no free stream IDs)");
            std::process::exit(1);
        }
        Ok(id) => id,
        Err(e) => {
            eprintln!("virtioso-mux-exec: enrollment read failed: {e}");
            std::process::exit(1);
        }
    };
    eprintln!("virtioso-mux-exec: enrolled as stream {stream_id} ({})", args.name);

    // Spawn child with stdout and stderr piped.
    let mut child = match Command::new(&args.cmd[0])
        .args(&args.cmd[1..])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            eprintln!("virtioso-mux-exec: cannot spawn {:?}: {e}", args.cmd[0]);
            std::process::exit(1);
        }
    };

    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();

    // Merge stdout and stderr into a single channel, then drain to the muxd socket.
    let (tx, mut rx) = mpsc::channel::<Vec<u8>>(64);

    {
        let tx = tx.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; 4096];
            loop {
                match stdout.read(&mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        if tx.send(buf[..n].to_vec()).await.is_err() {
                            break;
                        }
                    }
                }
            }
        });
    }
    {
        let tx = tx.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; 4096];
            loop {
                match stderr.read(&mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        if tx.send(buf[..n].to_vec()).await.is_err() {
                            break;
                        }
                    }
                }
            }
        });
    }
    drop(tx); // rx closes when both forwarder tasks finish

    while let Some(data) = rx.recv().await {
        if conn.write_all(&data).await.is_err() {
            eprintln!("virtioso-mux-exec: muxd socket write failed");
            break;
        }
    }

    // Closing conn signals CTRL_DISCONNECTED on the daemon side.
    drop(conn);

    let status = match child.wait().await {
        Ok(s) => s,
        Err(e) => {
            eprintln!("virtioso-mux-exec: wait failed: {e}");
            std::process::exit(1);
        }
    };
    std::process::exit(status.code().unwrap_or(1));
}

async fn connect_with_retry(
    path: &str,
    timeout_secs: u64,
) -> Result<UnixStream, Box<dyn std::error::Error>> {
    let deadline = tokio::time::Instant::now()
        + tokio::time::Duration::from_secs(timeout_secs);
    loop {
        match UnixStream::connect(path).await {
            Ok(s) => return Ok(s),
            Err(_) if tokio::time::Instant::now() < deadline => {
                tokio::time::sleep(tokio::time::Duration::from_millis(200)).await;
            }
            Err(e) => return Err(e.into()),
        }
    }
}
