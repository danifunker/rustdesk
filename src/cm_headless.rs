// Headless connection manager for the PowerPC agent build.
//
// Upstream's connection manager (`src/ui/cm.rs`) is a sciter window: it lists
// live sessions and, for an unauthorized peer, shows the accept/reject prompt.
// It is spawned by `server::connection::start_ipc` as a separate `--cm`
// process on every incoming connection, and if that process never answers on
// the `_cm` IPC socket, `start_ipc` bails and the connection is torn down.
//
// So a GUI-free build still needs *something* on `_cm`. This is that: the same
// IPC protocol, no window, and every peer authorized.
//
// On the auth path this is deliberately not a security decision of its own.
// `Connection::on_message` (src/server/connection.rs) already grants access on
// its own before the CM hears anything:
//
//     * correct permanent password -> `send_logon_response()` then
//       `try_start_cm(.., authorized = true)`  -- the CM is told, not asked.
//     * empty or wrong password    -> `try_start_cm(.., authorized = false)`
//       and the CM's answer decides.
//
// The second case is the one this module changes: upstream would wait for a
// human to click Accept; here it replies `Data::Authorize` immediately. That is
// what makes an unattended G5 reachable, and it is why the permanent password
// is not optional -- see `docs/powerpc-mrustc-scope.md`. With no password set,
// this would accept anyone who can reach the port.

use crate::ipc::{new_listener, Connection, Data};
use hbb_common::{
    config::Config,
    futures::StreamExt as _,
    log,
    tokio::{self, sync::mpsc},
    ResultType,
};

/// Refuse to run wide open. A permanent password is the only thing standing
/// between "unattended" and "unauthenticated", so make its absence loud and
/// fatal rather than a warning nobody reads on a machine with no screen.
pub fn check_password_set() -> ResultType<()> {
    if Config::get_password().is_empty() {
        hbb_common::bail!(
            "no permanent password is set, and the headless connection manager \
             authorizes every peer. Set one first:\n\
             \n    rustdesk --password <PASSWORD>\n\
             \n(see docs/powerpc-mrustc-scope.md)"
        );
    }
    Ok(())
}

#[tokio::main(flavor = "current_thread")]
pub async fn start() {
    if let Err(e) = check_password_set() {
        log::error!("{}", e);
        std::process::exit(1);
    }
    log::info!("headless connection manager starting (auto-accept)");
    run().await;
}

async fn run() {
    let mut incoming = match new_listener("_cm").await {
        Ok(i) => i,
        Err(err) => {
            log::error!("failed to listen on the _cm ipc socket: {}", err);
            return;
        }
    };
    while let Some(result) = incoming.next().await {
        match result {
            Ok(stream) => {
                tokio::spawn(async move {
                    if let Err(err) = handle(Connection::new(stream)).await {
                        log::debug!("cm ipc connection ended: {}", err);
                    }
                });
            }
            Err(err) => {
                log::error!("cm ipc accept failed: {}", err);
                break;
            }
        }
    }
}

/// One server-side `Connection`'s worth of CM traffic.
async fn handle(mut stream: Connection) -> ResultType<()> {
    // `Connection` in src/server/connection.rs forwards anything we send here
    // straight into its own event loop, so this channel exists only to keep the
    // shape identical to src/ui/cm.rs (which feeds it from UI callbacks).
    let (_tx, mut rx) = mpsc::unbounded_channel::<Data>();
    loop {
        tokio::select! {
            res = stream.next() => {
                match res {
                    Err(err) => return Err(err.into()),
                    Ok(None) => return Ok(()),
                    Ok(Some(data)) => match data {
                        Data::Login { id, peer_id, name, authorized, keyboard, clipboard, audio, is_file_transfer, .. } => {
                            if authorized {
                                // Password already checked by the connection.
                                log::info!(
                                    "session {} authorized: peer={} name={} \
                                     (keyboard={} clipboard={} audio={} file_transfer={})",
                                    id, peer_id, name, keyboard, clipboard, audio, is_file_transfer
                                );
                            } else {
                                log::info!(
                                    "session {} from peer={} name={} not pre-authorized -- \
                                     auto-accepting (headless)",
                                    id, peer_id, name
                                );
                                stream.send(&Data::Authorize).await?;
                            }
                        }
                        Data::ChatMessage { text } => log::info!("chat from peer: {}", text),
                        // The UI would show a transfer list; nothing to do headless.
                        Data::FS(_) => {}
                        Data::Close => return Ok(()),
                        _ => {}
                    },
                }
            }
            Some(data) = rx.recv() => {
                stream.send(&data).await?;
            }
        }
    }
}
