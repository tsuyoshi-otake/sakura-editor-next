//! Explicit v2 executable path. The native process owner supplies an overall
//! deadline and a job memory limit, including Wasmtime's lifted host values.
use sakura_senp_host::bindings_v2::Extension;
use sakura_senp_host::effect_protocol as wire;
use sakura_senp_host::effect_session::{Guest, GuestFailed, Session};
use std::io::{self, Read, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;
use wasmtime::component::{Component, Linker};
use wasmtime::{Config, Engine, Store, StoreLimitsBuilder};

struct EpochClock {
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}

impl EpochClock {
    fn start(engine: Engine) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let stopped = stop.clone();
        let thread = thread::spawn(move || {
            while !stopped.load(Ordering::Acquire) {
                thread::sleep(Duration::from_millis(super::EPOCH_TICK_MILLISECONDS));
                engine.increment_epoch();
            }
        });
        Self {
            stop,
            thread: Some(thread),
        }
    }
}

impl Drop for EpochClock {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

struct Runtime {
    store: Store<super::HostState>,
    bindings: Extension,
    _clock: EpochClock,
}

impl Runtime {
    fn load(path: PathBuf, digest: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let bytes = super::read_verified_component(path, digest)?;
        let mut config = Config::new();
        config
            .wasm_component_model(true)
            .consume_fuel(true)
            .epoch_interruption(true);
        let engine = Engine::new(&config)?;
        let component = Component::from_binary(&engine, &bytes)?;
        let linker = Linker::new(&engine);
        let limits = StoreLimitsBuilder::new()
            .memory_size(32 * 1024 * 1024)
            .instances(1)
            .tables(4)
            .build();
        let mut store = Store::new(&engine, super::HostState { limits });
        store.limiter(|state| &mut state.limits);
        store.set_fuel(super::MAX_FUEL)?;
        store.set_epoch_deadline(super::CALL_DEADLINE_TICKS);
        // Instantiation can execute Wasm, so the clock must already be running.
        // This RAII local also joins the clock on failed instantiation.
        let clock = EpochClock::start(engine);
        let bindings = Extension::instantiate(&mut store, &component, &linker)?;
        Ok(Self {
            store,
            bindings,
            _clock: clock,
        })
    }

    fn begin_call(&mut self) -> Result<(), GuestFailed> {
        self.store
            .set_fuel(super::MAX_FUEL)
            .map_err(|_| GuestFailed)?;
        self.store.set_epoch_deadline(super::CALL_DEADLINE_TICKS);
        Ok(())
    }
}

impl Guest for Runtime {
    fn activate(&mut self, value: &wire::Activate) -> Result<Vec<wire::Effect>, GuestFailed> {
        self.begin_call()?;
        self.bindings
            .sakura_senp_event_effects()
            .call_activate(&mut self.store, &value.into())
            .map(|effects| effects.into_iter().map(Into::into).collect())
            .map_err(|_| GuestFailed)
    }

    fn on_event(&mut self, value: &wire::EventMessage) -> Result<Vec<wire::Effect>, GuestFailed> {
        self.begin_call()?;
        self.bindings
            .sakura_senp_event_effects()
            .call_on_event(
                &mut self.store,
                &(&value.context).into(),
                &(&value.event).into(),
            )
            .map(|effects| effects.into_iter().map(Into::into).collect())
            .map_err(|_| GuestFailed)
    }

    fn deactivate(&mut self, reason: &wire::StopReason) -> Result<(), GuestFailed> {
        self.begin_call()?;
        self.bindings
            .sakura_senp_event_effects()
            .call_deactivate(&mut self.store, reason.into())
            .map_err(|_| GuestFailed)
    }
}

fn read_frame(input: &mut impl Read) -> io::Result<Option<Vec<u8>>> {
    let mut header = [0; 4];
    // EOF is orderly only BETWEEN frames; truncated prefixes are failures.
    loop {
        match input.read(&mut header[..1]) {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
    }
    input.read_exact(&mut header[1..])?;
    let size = u32::from_le_bytes(header) as usize;
    if size == 0 || size > wire::MAX_FRAME_BYTES {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "v2 frame size"));
    }
    let mut bytes = vec![0; size];
    input.read_exact(&mut bytes)?;
    Ok(Some(bytes))
}

pub(super) fn run(path: PathBuf, digest: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut session = Session::new(Runtime::load(path, digest)?);
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    while let Some(bytes) = read_frame(&mut input)? {
        if let Some(response) = session.receive(&bytes).map_err(|_| "v2 protocol failure")? {
            output.write_all(&(response.len() as u32).to_le_bytes())?;
            output.write_all(&response)?;
            output.flush()?;
        }
        if session.is_stopped() {
            break;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn v2_frame_reader_distinguishes_eof_from_truncation_and_bounds_allocation() {
        assert!(read_frame(&mut &b""[..]).unwrap().is_none());
        for bytes in [
            vec![2],
            vec![2, 0, 0, 0, 42],
            vec![0, 0, 0, 0],
            ((wire::MAX_FRAME_BYTES + 1) as u32).to_le_bytes().to_vec(),
        ] {
            assert!(read_frame(&mut bytes.as_slice()).is_err());
        }
        assert_eq!(
            read_frame(&mut &[2, 0, 0, 0, 42, 43][..]).unwrap(),
            Some(vec![42, 43])
        );
    }
}
