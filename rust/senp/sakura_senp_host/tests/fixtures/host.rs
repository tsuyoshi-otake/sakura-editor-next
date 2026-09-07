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

fn main() -> Result<(), Box<dyn std::error::Error>> {
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_secs(30));
        std::process::exit(124);
    });
    let path = std::path::PathBuf::from(
        std::env::args_os()
            .nth(2)
            .ok_or("missing fixture scenario")?,
    );
    let scenario = path
        .file_stem()
        .and_then(|value| value.to_str())
        .ok_or("scenario is not UTF-8")?;
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
