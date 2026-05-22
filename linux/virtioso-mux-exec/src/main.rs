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

    // Monitor child exit on a background task so we can race it against the pipe drain.
    // If a grandchild process inherits a pipe fd and outlives the child, the pipe reader
    // tasks never see EOF.  We detect child exit and drain for at most 2 s then stop.
    let (exit_tx, exit_rx) = tokio::sync::oneshot::channel::<std::io::Result<std::process::ExitStatus>>();
    tokio::spawn(async move {
        let _ = exit_tx.send(child.wait().await);
    });

    let exit_code = drain_with_child_exit(&mut conn, &mut rx, exit_rx).await;

    drop(conn); // signals CTRL_DISCONNECTED on daemon side
    std::process::exit(exit_code);
}

async fn drain_with_child_exit(
    conn: &mut UnixStream,
    rx: &mut mpsc::Receiver<Vec<u8>>,
    mut exit_rx: tokio::sync::oneshot::Receiver<std::io::Result<std::process::ExitStatus>>,
) -> i32 {
    loop {
        tokio::select! {
            biased;
            data = rx.recv() => {
                match data {
                    Some(data) => {
                        if conn.write_all(&data).await.is_err() {
                            eprintln!("virtioso-mux-exec: muxd socket write failed");
                            return 1;
                        }
                    }
                    None => {
                        // Both pipe reader tasks finished cleanly; collect exit status.
                        return match exit_rx.await {
                            Ok(Ok(s)) => s.code().unwrap_or(1),
                            _ => 1,
                        };
                    }
                }
            }
            result = &mut exit_rx => {
                // Child exited before both pipe readers finished (grandchild holding pipe?).
                // Drain remaining buffered output for up to 2 s, then stop.
                let code = match result {
                    Ok(Ok(s)) => s.code().unwrap_or(1),
                    _ => 1,
                };
                let deadline = tokio::time::Instant::now()
                    + tokio::time::Duration::from_secs(2);
                loop {
                    match tokio::time::timeout_at(deadline, rx.recv()).await {
                        Ok(Some(data)) => { let _ = conn.write_all(&data).await; }
                        _ => break,
                    }
                }
                return code;
            }
        }
    }
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
