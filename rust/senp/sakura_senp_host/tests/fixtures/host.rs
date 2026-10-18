//! Deliberately faulty native pipe peer, built only with test-fixtures.
//! Never shipped or selected by package metadata. The native test owns its job;
//! the independent watchdog also bounds manual invocation to 30 seconds.
use sakura_senp_host::effect_protocol as wire;
use sakura_senp_host::effect_session::{Guest, GuestFailed, Session};
use std::io::{self, Read, Write};
use std::time::Duration;

struct Fixture(String);
impl Guest for Fixture {
    fn activate(&mut self, _: &wire::Activate) -> Result<Vec<wire::Effect>, GuestFailed> {
        Ok(vec![])
    }
    fn on_event(&mut self, _: &wire::EventMessage) -> Result<Vec<wire::Effect>, GuestFailed> {
        match self.0.as_str() {
            "blocked-read" => std::thread::sleep(Duration::from_secs(30)),
            // One slow-but-healthy guest: the reply lands inside an ordinary
            // invocation deadline but past a fixed short exchange cap.
            "delayed-reply" => std::thread::sleep(Duration::from_millis(1500)),
            "crash" => std::process::exit(66),
            "memory-limit" => {
                let mut allocations = Vec::new();
                for _ in 0..768 {
                    allocations.push(vec![42u8; 1024 * 1024]);
                }
                std::hint::black_box(allocations);
            }
            _ => {}
        }
        Ok(vec![])
    }
    fn deactivate(&mut self, _: &wire::StopReason) -> Result<(), GuestFailed> {
        Ok(())
    }
}

/// Minimal v1 pipe-protocol peer for the C++ decoration worker tests. The
/// v1 host launches `peer --component <module> --component-sha256 <sha>` with
/// no `--protocol` flag, which selects this mode; frames are the same u32-LE
/// length-prefixed JSON envelopes on stdio, decoded strictly so a malformed
/// native frame fails the exchange. `wrong-hello` answers the hello with a
/// non-v1 protocol so the native `Open` must refuse it; `memory-limit`
/// allocates past the 512 MiB job cap on the first `decorate`. Every other
/// scenario answers one slot per requested line, with `depth` set to the
/// line's index and `visualLength` to its UTF-16 length plus one, so the
/// native side can check the frame it built and how slots map back.
fn v1_main(scenario: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    loop {
        let mut length = [0; 4];
        if input.read_exact(&mut length).is_err() {
            break;
        }
        let size = u32::from_le_bytes(length) as usize;
        if size == 0 || size > wire::MAX_FRAME_BYTES {
            return Err("fixture frame size".into());
        }
        let mut bytes = vec![0; size];
        input.read_exact(&mut bytes)?;
        let request: serde_json::Value = serde_json::from_slice(&bytes)?;
        let response = match request["type"].as_str() {
            Some("shutdown") => break,
            Some("hello") => {
                let protocol = if scenario == "wrong-hello" { 2 } else { 1 };
                serde_json::json!({
                    "type": "hello",
                    "protocol": protocol,
                    "abi": "sakura:senp/extension@1.0.0",
                })
            }
            Some("decorate") => {
                if scenario == "memory-limit" {
                    let mut allocations = Vec::new();
                    for _ in 0..768 {
                        allocations.push(vec![42u8; 1024 * 1024]);
                    }
                    std::hint::black_box(allocations);
                }
                let mut slots = Vec::new();
                for line in request["lines"].as_array().ok_or("fixture lines")? {
                    let index = line["line"].as_u64().ok_or("fixture line index")?;
                    let text = line["text"].as_str().ok_or("fixture line text")?;
                    slots.push(serde_json::json!({
                        "line": index,
                        "visualStart": 0,
                        "visualLength": text.encode_utf16().count() + 1,
                        "depth": index,
                    }));
                }
                serde_json::json!({ "type": "decorations", "revision": 1, "slots": slots })
            }
            _ => return Err("fixture v1 request".into()),
        };
        let response = serde_json::to_vec(&response)?;
        output.write_all(&(response.len() as u32).to_le_bytes())?;
        output.write_all(&response)?;
        output.flush()?;
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_secs(30));
        std::process::exit(124);
    });
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    let path = std::path::PathBuf::from(args.get(2).ok_or("missing fixture scenario")?);
    let scenario = path
        .file_stem()
        .and_then(|value| value.to_str())
        .ok_or("scenario is not UTF-8")?;
    // The v1 launch carries no `--protocol` flag; the v2 launch does.
    if args.get(1).and_then(|value| value.to_str()) == Some("--component")
        && !args
            .iter()
            .any(|value| value.to_str() == Some("--protocol"))
    {
        return v1_main(scenario);
    }
    let mut session = Session::new(Fixture(scenario.into()));
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    loop {
        let mut length = [0; 4];
        if input.read_exact(&mut length).is_err() {
            break;
        }
        let size = u32::from_le_bytes(length) as usize;
        if size == 0 || size > wire::MAX_FRAME_BYTES {
            return Err("fixture frame size".into());
        }
        let mut bytes = vec![0; size];
        input.read_exact(&mut bytes)?;
        let request = wire::decode(&bytes)?;
        if matches!(request.body, wire::Message::Event(_)) {
            if scenario == "oversized" {
                output.write_all(&((wire::MAX_FRAME_BYTES + 1) as u32).to_le_bytes())?;
                output.flush()?;
                std::thread::sleep(Duration::from_secs(30));
            }
            if scenario == "partial" {
                output.write_all(&100u32.to_le_bytes())?;
                output.write_all(b"{")?;
                output.flush()?;
                std::thread::sleep(Duration::from_secs(30));
            }
        }
        // A cold start owns the Hello exchange: "delayed-hello" is a peer that
        // verifies and compiles slowly but answers inside the cold-start
        // budget; "blocked-hello" never answers within the test's horizon.
        if matches!(request.body, wire::Message::Hello(_)) {
            if scenario == "delayed-hello" {
                std::thread::sleep(Duration::from_millis(1000));
            }
            if scenario == "blocked-hello" {
                std::thread::sleep(Duration::from_secs(30));
            }
        }
        if let Some(response) = session.receive(&bytes).map_err(|_| "fixture protocol")? {
            output.write_all(&(response.len() as u32).to_le_bytes())?;
            output.write_all(&response)?;
            output.flush()?;
        }
        if scenario == "blocked-write" && matches!(request.body, wire::Message::Activate(_)) {
            std::thread::sleep(Duration::from_secs(30));
        }
        if session.is_stopped() {
            break;
        }
    }
    Ok(())
}
