// Specify the Windows subsystem to eliminate console window.
// Requires Rust 1.18.
//#![windows_subsystem = "windows"]

use hbb_common::log;
use rustdesk::*;

#[cfg(any(target_os = "android", target_os = "ios"))]
fn main() {
    common::test_rendezvous_server();
    common::test_nat_type();
    #[cfg(target_os = "android")]
    crate::common::check_software_update();
    mobile::Session::start("");
}

#[cfg(not(any(target_os = "android", target_os = "ios", feature = "cli")))]
fn main() {
    let mut args = Vec::new();
    let mut i = 0;
    for arg in std::env::args() {
        if i > 0 {
            args.push(arg);
        }
        i += 1;
    }
    if args.len() > 0 && args[0] == "--version" {
        println!("{}", crate::VERSION);
        return;
    }
    #[cfg(not(feature = "inline"))]
    {
        use hbb_common::env_logger::*;
        init_from_env(Env::default().filter_or(DEFAULT_FILTER_ENV, "info"));
    }
    #[cfg(feature = "inline")]
    {
        let mut path = hbb_common::config::Config::log_path();
        if args.len() > 0 && args[0].starts_with("--") {
            let name = args[0].replace("--", "");
            if !name.is_empty() {
                path.push(name);
            }
        }
        use flexi_logger::*;
        Logger::try_with_env_or_str("debug")
            .map(|x| {
                x.log_to_file(FileSpec::default().directory(path))
                    .format(opt_format)
                    .rotate(
                        Criterion::Age(Age::Day),
                        Naming::Timestamps,
                        Cleanup::KeepLogFiles(6),
                    )
                    .start()
                    .ok();
            })
            .ok();
    }
    if args.is_empty() {
        std::thread::spawn(move || start_server(false, false));
    } else {
        #[cfg(windows)]
        {
            if args[0] == "--uninstall" {
                if let Err(err) = platform::uninstall_me() {
                    log::error!("Failed to uninstall: {}", err);
                }
                return;
            } else if args[0] == "--update" {
                hbb_common::allow_err!(platform::update_me());
                return;
            } else if args[0] == "--reinstall" {
                hbb_common::allow_err!(platform::uninstall_me());
                hbb_common::allow_err!(platform::install_me("desktopicon startmenu",));
                return;
            }
        }
        if args[0] == "--remove" {
            if args.len() == 2 {
                // sleep a while so that process of removed exe exit
                std::thread::sleep(std::time::Duration::from_secs(1));
                std::fs::remove_file(&args[1]).ok();
                return;
            }
        } else if args[0] == "--service" {
            log::info!("start --service");
            start_os_service();
            return;
        } else if args[0] == "--server" {
            log::info!("start --server");
            start_server(true, true);
            return;
        } else if args[0] == "--import-config" {
            if args.len() == 2 {
                hbb_common::config::Config::import(&args[1]);
            }
            return;
        } else if args[0] == "--password" {
            if args.len() == 2 {
                ipc::set_password(args[1].to_owned()).unwrap();
            }
            return;
        }
    }
    ui::start(&mut args[..]);
}

#[cfg(feature = "cli")]
fn main() {
    use clap::App;
    // NOTE: upstream advertised `--server` here but never implemented the arm;
    // the agent build needs it, plus a headless `--cm` and headless config
    // setters (there is no GUI to set the password or the ID server from).
    let args = format!(
        "-p, --port-forward=[PORT-FORWARD-OPTIONS] 'Format: remote-id:local-port:remote-port[:remote-host]'
       -s, --server 'Start the agent (accept incoming connections)'
           --cm 'Run the headless connection manager (spawned by --server)'
           --password=[PASSWORD] 'Set the permanent password and exit'
           --rendezvous-server=[HOST] 'Set the self-hosted ID server (host or host:port) and exit'
           --key=[KEY] 'Set the self-hosted server public key and exit'
           --get-id 'Print this machine''s RustDesk ID and exit'",
    );
    let matches = App::new("rustdesk")
        .version(VERSION)
        .author("CarrieZ Studio<info@rustdesk.com>")
        .about("RustDesk command line tool")
        .args_from_usage(&args)
        .get_matches();
    use hbb_common::env_logger::*;
    init_from_env(Env::default().filter_or(DEFAULT_FILTER_ENV, "info"));

    // ---- headless configuration (no GUI to do this from) --------------------
    use hbb_common::config::Config;
    let mut configured = false;
    if let Some(v) = matches.value_of("password") {
        Config::set_password(v);
        println!("permanent password set");
        configured = true;
    }
    if let Some(v) = matches.value_of("rendezvous-server") {
        // Config::get_rendezvous_server() appends the default port if absent.
        Config::set_option("custom-rendezvous-server".to_owned(), v.to_owned());
        println!("id server set to {}", v);
        configured = true;
    }
    if let Some(v) = matches.value_of("key") {
        Config::set_option("key".to_owned(), v.to_owned());
        println!("server key set");
        configured = true;
    }
    if matches.is_present("get-id") {
        println!("{}", Config::get_id());
        return;
    }
    if configured {
        return;
    }

    if matches.is_present("cm") {
        cm_headless::start();
        return;
    }
    if matches.is_present("server") {
        // Refuse to come up wide open -- the headless CM authorizes everyone.
        if let Err(e) = cm_headless::check_password_set() {
            eprintln!("{}", e);
            std::process::exit(1);
        }
        println!("rustdesk agent starting, id = {}", Config::get_id());
        start_server(true, true);
        return;
    }
    if let Some(p) = matches.value_of("port-forward") {
        let options: Vec<String> = p.split(":").map(|x| x.to_owned()).collect();
        if options.len() < 3 {
            log::error!("Wrong port-forward options");
            return;
        }
        let mut port = 0;
        if let Ok(v) = options[1].parse::<i32>() {
            port = v;
        } else {
            log::error!("Wrong local-port");
            return;
        }
        let mut remote_port = 0;
        if let Ok(v) = options[2].parse::<i32>() {
            remote_port = v;
        } else {
            log::error!("Wrong remote-port");
            return;
        }
        let mut remote_host = "localhost".to_owned();
        if options.len() > 3 {
            remote_host = options[3].clone();
        }
        cli::start_one_port_forward(options[0].clone(), port, remote_host, remote_port);
    }
}
