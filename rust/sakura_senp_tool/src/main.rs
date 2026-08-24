use sakura_senp::{
    decode_hex, install_package, list_installed, list_uninstalled, pack_directory,
    set_extension_enabled, uninstall_built_in, verify_package, TrustPolicy,
};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::Path;

fn usage() -> ! {
    eprintln!("usage: sakura-senp-tool componentize <core.wasm> <component.wasm> | pack <source-dir> <output.senp> [signing-key-hex] | pack-builtin <source-dir> <output.senp> <hash-file> | verify <package.senp> [publisher=public-key-hex] | inspect-builtin <package.senp> <archive-sha256> | install <package.senp> <root> [publisher=public-key-hex] | install-builtin <package.senp> <root> <archive-sha256> | uninstall-builtin <root> <id> <archive-sha256> | install-dev <package.senp> <root> | list <root> | list-uninstalled <root> | set-enabled <root> <id> <true|false>");
    std::process::exit(2);
}

fn publisher_policy(value: Option<String>) -> Result<TrustPolicy, Box<dyn std::error::Error>> {
    match value {
        Some(value) => {
            let (publisher, key) = value
                .split_once('=')
                .ok_or("publisher key must be publisher=hex")?;
            let mut keys = BTreeMap::new();
            keys.insert(publisher.to_owned(), decode_hex::<32>(key)?);
            Ok(TrustPolicy::PublisherKeys(keys))
        }
        None => Ok(TrustPolicy::PublisherKeys(BTreeMap::new())),
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = env::args().skip(1);
    match arguments.next().as_deref() {
        Some("componentize") => {
            let input = arguments.next().unwrap_or_else(|| usage());
            let output = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            let module = fs::read(&input)?;
            let component = wit_component::ComponentEncoder::default()
                .validate(true)
                .module(&module)?
                .encode()?;
            fs::write(output, component)?;
        }
        Some("pack") => {
            let source = arguments.next().unwrap_or_else(|| usage());
            let output = arguments.next().unwrap_or_else(|| usage());
            let key = arguments
                .next()
                .map(|value| decode_hex::<32>(&value))
                .transpose()?;
            if arguments.next().is_some() {
                usage();
            }
            let hash = pack_directory(Path::new(&source), Path::new(&output), key)?;
            println!("{hash}");
        }
        Some("pack-builtin") => {
            let source = arguments.next().unwrap_or_else(|| usage());
            let output = arguments.next().unwrap_or_else(|| usage());
            let hash_file = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            let hash = pack_directory(Path::new(&source), Path::new(&output), None)?;
            fs::write(hash_file, format!("{hash}\n"))?;
            println!("{hash}");
        }
        Some("verify") => {
            let package = arguments.next().unwrap_or_else(|| usage());
            let policy = publisher_policy(arguments.next())?;
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&verify_package(Path::new(&package), &policy)?)?
            );
        }
        Some("inspect-builtin") => {
            let package = arguments.next().unwrap_or_else(|| usage());
            let expected_archive_sha256 = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&verify_package(
                    Path::new(&package),
                    &TrustPolicy::BuiltIn {
                        expected_archive_sha256,
                    }
                )?)?
            );
        }
        Some("install") => {
            let package = arguments.next().unwrap_or_else(|| usage());
            let root = arguments.next().unwrap_or_else(|| usage());
            let policy = publisher_policy(arguments.next())?;
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&install_package(
                    Path::new(&package),
                    Path::new(&root),
                    &policy
                )?)?
            );
        }
        Some("install-builtin") => {
            let package = arguments.next().unwrap_or_else(|| usage());
            let root = arguments.next().unwrap_or_else(|| usage());
            let expected_archive_sha256 = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            let policy = TrustPolicy::BuiltIn {
                expected_archive_sha256,
            };
            println!(
                "{}",
                serde_json::to_string(&install_package(
                    Path::new(&package),
                    Path::new(&root),
                    &policy
                )?)?
            );
        }
        Some("install-dev") => {
            let package = arguments.next().unwrap_or_else(|| usage());
            let root = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&install_package(
                    Path::new(&package),
                    Path::new(&root),
                    &TrustPolicy::DeveloperUnsigned
                )?)?
            );
        }
        Some("uninstall-builtin") => {
            let root = arguments.next().unwrap_or_else(|| usage());
            let id = arguments.next().unwrap_or_else(|| usage());
            let expected_archive_sha256 = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            uninstall_built_in(Path::new(&root), &id, &expected_archive_sha256)?;
        }
        Some("list") => {
            let root = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&list_installed(Path::new(&root))?)?
            );
        }
        Some("list-uninstalled") => {
            let root = arguments.next().unwrap_or_else(|| usage());
            if arguments.next().is_some() {
                usage();
            }
            println!(
                "{}",
                serde_json::to_string(&list_uninstalled(Path::new(&root))?)?
            );
        }
        Some("set-enabled") => {
            let root = arguments.next().unwrap_or_else(|| usage());
            let id = arguments.next().unwrap_or_else(|| usage());
            let enabled = match arguments.next().as_deref() {
                Some("true") => true,
                Some("false") => false,
                _ => usage(),
            };
            if arguments.next().is_some() {
                usage();
            }
            set_extension_enabled(Path::new(&root), &id, enabled)?;
        }
        _ => usage(),
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
