use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use wasmtime::component::{Component, Linker};
use wasmtime::{Config, Engine, Store, StoreLimits, StoreLimitsBuilder};

mod runtime_v2;

wasmtime::component::bindgen!({
    path: "../wit/senp-extension.wit",
    world: "extension",
});

use exports::sakura::senp::editor_decorations::{DecorationRequest, VisibleLine};

const MAX_FRAME_BYTES: usize = 4 * 1024 * 1024;
const MAX_COMPONENT_BYTES: u64 = 32 * 1024 * 1024;
/// Fuel one guest call may burn before the payload it carries is counted. It is
/// what an activation, a tree page or a command costs, and it is the whole
/// budget of a call that carries no page.
const BASE_FUEL: u64 = 10_000_000;
/// Fuel added for each byte of payload a call carries. A tool completion holds a
/// whole page of an answer and the guest parses and renders every byte of it, so
/// the budget follows the page rather than every call being handed the largest
/// page's budget. Measured against real GitHub pages, the densest of them cost
/// about 710 fuel per byte; this is that with room above it.
const FUEL_PER_PAYLOAD_BYTE: u64 = 1024;
const EPOCH_TICK_MILLISECONDS: u64 = 10;
const CALL_DEADLINE_TICKS: u64 = 20;

/// Fuel for one call carrying `payload_bytes` of payload. Saturating rather than
/// wrapping: a payload big enough to overflow this is one the frame bound has
/// already refused, and a wrapped budget would be a small one.
fn call_fuel(payload_bytes: usize) -> u64 {
    BASE_FUEL.saturating_add(FUEL_PER_PAYLOAD_BYTE.saturating_mul(payload_bytes as u64))
}

#[derive(Debug, Deserialize)]
#[serde(
    tag = "type",
    rename_all = "camelCase",
    rename_all_fields = "camelCase",
    deny_unknown_fields
)]
enum Request {
    Hello {
        protocol: u32,
    },
    Decorate {
        revision: u64,
        tab_size: u32,
        lines: Vec<InputLine>,
    },
    Shutdown,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct InputLine {
    line: u32,
    text: String,
}

#[derive(Debug, Serialize)]
#[serde(tag = "type", rename_all = "camelCase")]
enum Response {
    Hello {
        protocol: u32,
        abi: &'static str,
    },
    Decorations {
        revision: u64,
        slots: Vec<OutputSlot>,
    },
    Error {
        code: &'static str,
        detail: String,
    },
    Shutdown,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct OutputSlot {
    line: u32,
    visual_start: u32,
    visual_length: u32,
    depth: u32,
}

struct HostState {
    limits: StoreLimits,
}

struct Runtime {
    store: Store<HostState>,
    bindings: Extension,
    epoch_stop: Arc<AtomicBool>,
    epoch_thread: Option<thread::JoinHandle<()>>,
}

impl Runtime {
    fn load(
        component_path: PathBuf,
        expected_sha256: &str,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        let component_bytes = read_verified_component(component_path, expected_sha256)?;
        let mut config = Config::new();
        config.wasm_component_model(true);
        config.consume_fuel(true);
        config.epoch_interruption(true);
        let engine = Engine::new(&config)?;
        let component = Component::from_binary(&engine, &component_bytes)?;
        let linker = Linker::new(&engine);
        let limits = StoreLimitsBuilder::new()
            .memory_size(32 * 1024 * 1024)
            .instances(1)
            .tables(4)
            .build();
        let mut store = Store::new(&engine, HostState { limits });
        store.limiter(|state| &mut state.limits);
        store.set_fuel(call_fuel(0))?;
        store.set_epoch_deadline(CALL_DEADLINE_TICKS);
        let bindings = Extension::instantiate(&mut store, &component, &linker)?;
        let epoch_stop = Arc::new(AtomicBool::new(false));
        let epoch_thread_stop = Arc::clone(&epoch_stop);
        let epoch_engine = engine.clone();
        let epoch_thread = thread::spawn(move || {
            while !epoch_thread_stop.load(Ordering::Acquire) {
                thread::sleep(Duration::from_millis(EPOCH_TICK_MILLISECONDS));
                epoch_engine.increment_epoch();
            }
        });
        Ok(Self {
            store,
            bindings,
            epoch_stop,
            epoch_thread: Some(epoch_thread),
        })
    }

    fn decorate(&mut self, revision: u64, tab_size: u32, lines: Vec<InputLine>) -> Response {
        let payload = lines.iter().map(|line| line.text.len()).sum();
        if let Err(error) = self.store.set_fuel(call_fuel(payload)) {
            return Response::Error {
                code: "runtimeUnavailable",
                detail: error.to_string(),
            };
        }
        self.store.set_epoch_deadline(CALL_DEADLINE_TICKS);
        let request = DecorationRequest {
            revision,
            tab_size,
            lines: lines
                .into_iter()
                .map(|line| VisibleLine {
                    line: line.line,
                    text: line.text,
                })
                .collect(),
        };
        match self
            .bindings
            .sakura_senp_editor_decorations()
            .call_decorate(&mut self.store, &request)
        {
            Ok(Ok(slots)) => Response::Decorations {
                revision,
                slots: slots
                    .into_iter()
                    .map(|slot| OutputSlot {
                        line: slot.line,
                        visual_start: slot.visual_start,
                        visual_length: slot.visual_length,
                        depth: slot.depth,
                    })
                    .collect(),
            },
            Ok(Err(error)) => Response::Error {
                code: "extensionRejected",
                detail: format!("{error:?}"),
            },
            Err(error) => Response::Error {
                code: "extensionTrap",
                detail: error.to_string(),
            },
        }
    }
}

fn is_lower_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn verify_component_bytes(
    bytes: Vec<u8>,
    expected_sha256: &str,
) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    if !is_lower_sha256(expected_sha256) {
        return Err("component SHA-256 must be 64 lowercase hexadecimal characters".into());
    }
    let actual = format!("{:x}", Sha256::digest(&bytes));
    if actual != expected_sha256 {
        return Err("component SHA-256 mismatch".into());
    }
    Ok(bytes)
}

fn read_verified_component(
    component_path: PathBuf,
    expected_sha256: &str,
) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    let mut component = File::open(component_path)?;
    let initial_len = component.metadata()?.len();
    if initial_len > MAX_COMPONENT_BYTES {
        return Err("component exceeds 32 MiB".into());
    }
    let mut bytes = Vec::with_capacity(initial_len as usize);
    Read::by_ref(&mut component)
        .take(MAX_COMPONENT_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_COMPONENT_BYTES {
        return Err("component exceeds 32 MiB".into());
    }
    verify_component_bytes(bytes, expected_sha256)
}

impl Drop for Runtime {
    fn drop(&mut self) {
        self.epoch_stop.store(true, Ordering::Release);
        if let Some(thread) = self.epoch_thread.take() {
            let _ = thread.join();
        }
    }
}

fn read_frame(input: &mut impl Read) -> io::Result<Option<Vec<u8>>> {
    let mut length = [0u8; 4];
    match input.read_exact(&mut length) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error),
    }
    let length = u32::from_le_bytes(length) as usize;
    if length == 0 || length > MAX_FRAME_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame size outside protocol limits",
        ));
    }
    let mut bytes = vec![0; length];
    input.read_exact(&mut bytes)?;
    Ok(Some(bytes))
}

fn write_frame(output: &mut impl Write, response: &Response) -> io::Result<()> {
    let bytes = serde_json::to_vec(response).map_err(io::Error::other)?;
    let length = u32::try_from(bytes.len())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "response too large"))?;
    output.write_all(&length.to_le_bytes())?;
    output.write_all(&bytes)?;
    output.flush()
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args_os().skip(1);
    if arguments.next().as_deref() != Some(std::ffi::OsStr::new("--component")) {
        return Err(
            "usage: sakura-senp-host --component <extension.wasm> --component-sha256 <sha256> [--protocol 2]"
                .into(),
        );
    }
    let component = arguments
        .next()
        .map(PathBuf::from)
        .ok_or("missing component path")?;
    if arguments.next().as_deref() != Some(std::ffi::OsStr::new("--component-sha256")) {
        return Err("missing component SHA-256 option".into());
    }
    let component_sha256 = arguments
        .next()
        .ok_or("missing component SHA-256")?
        .into_string()
        .map_err(|_| "component SHA-256 is not Unicode")?;
    match arguments.next() {
        None => {} // Existing invocation selects v1 explicitly by its default.
        Some(option) if option == "--protocol" => {
            if arguments.next().as_deref() != Some(std::ffi::OsStr::new("2"))
                || arguments.next().is_some()
            {
                return Err("unsupported protocol option".into());
            }
            return runtime_v2::run(component, &component_sha256);
        }
        Some(_) => return Err("unexpected argument".into()),
    }
    let mut runtime = Runtime::load(component, &component_sha256)?;
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    while let Some(frame) = read_frame(&mut input)? {
        let request: Request = match serde_json::from_slice(&frame) {
            Ok(request) => request,
            Err(error) => {
                write_frame(
                    &mut output,
                    &Response::Error {
                        code: "invalidRequest",
                        detail: error.to_string(),
                    },
                )?;
                continue;
            }
        };
        let response = match request {
            Request::Hello { protocol: 1 } => Response::Hello {
                protocol: 1,
                abi: "sakura:senp/extension@1.0.0",
            },
            Request::Hello { .. } => Response::Error {
                code: "unsupportedProtocol",
                detail: "only protocol 1 is supported".into(),
            },
            Request::Decorate {
                revision,
                tab_size,
                lines,
            } => runtime.decorate(revision, tab_size, lines),
            Request::Shutdown => {
                write_frame(&mut output, &Response::Shutdown)?;
                break;
            }
        };
        write_frame(&mut output, &response)?;
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn call_fuel_follows_the_page_a_completion_carries() {
        use sakura_senp_host::effect_protocol as wire;

        let empty = wire::Event::Cancel(wire::Cancel {
            operation_id: "op".into(),
        });
        assert_eq!(empty.payload_bytes(), 0);
        assert_eq!(call_fuel(empty.payload_bytes()), BASE_FUEL);

        // The largest completion the wire admits has to be affordable, or the
        // guest traps on a page the protocol just accepted. The rate is measured
        // against real GitHub pages, whose densest cost about 710 fuel per byte.
        let page = wire::Event::ToolCompleted(wire::ToolCompleted {
            read_id: "read".into(),
            status: wire::CompletionStatus::Succeeded,
            data: "x".repeat(wire::MAX_TOOL_DATA_BYTES),
            message: String::new(),
        });
        assert_eq!(page.payload_bytes(), wire::MAX_TOOL_DATA_BYTES);
        assert!(call_fuel(page.payload_bytes()) > 710 * wire::MAX_TOOL_DATA_BYTES as u64);

        // A length no frame could ever hold still yields a budget, not a wrap to
        // a small one.
        assert_eq!(call_fuel(usize::MAX), u64::MAX);
    }

    #[test]
    fn verified_component_returns_the_exact_hashed_bytes() {
        let bytes = b"component bytes".to_vec();
        let expected = format!("{:x}", Sha256::digest(&bytes));
        assert_eq!(
            verify_component_bytes(bytes.clone(), &expected).unwrap(),
            bytes
        );
    }

    #[test]
    fn verified_component_rejects_mismatch_and_noncanonical_digest() {
        let bytes = b"component bytes".to_vec();
        let expected = format!("{:x}", Sha256::digest(&bytes));
        assert!(verify_component_bytes(bytes.clone(), &"0".repeat(64)).is_err());
        assert!(verify_component_bytes(bytes, &expected.to_uppercase()).is_err());
    }

    #[test]
    fn decoration_request_uses_the_public_camel_case_protocol() {
        let request: Request = serde_json::from_str(
            r#"{"type":"decorate","revision":7,"tabSize":4,"lines":[{"line":11,"text":"        value"}]}"#,
        )
        .unwrap();

        match request {
            Request::Decorate {
                revision,
                tab_size,
                lines,
            } => {
                assert_eq!(revision, 7);
                assert_eq!(tab_size, 4);
                assert_eq!(lines.len(), 1);
                assert_eq!(lines[0].line, 11);
                assert_eq!(lines[0].text, "        value");
            }
            other => panic!("unexpected request: {other:?}"),
        }
    }

    #[test]
    #[ignore = "requires the componentized sakura-indent-rainbow build artifact"]
    fn component_executes_without_wasi_or_other_ambient_imports() {
        let component = std::env::var_os("SAKURA_SENP_TEST_COMPONENT")
            .expect("SAKURA_SENP_TEST_COMPONENT must name the built Indent Rainbow component");
        let component = PathBuf::from(component);
        let expected_sha256 = format!("{:x}", Sha256::digest(std::fs::read(&component).unwrap()));
        let mut runtime = Runtime::load(component, &expected_sha256).unwrap();
        let response = runtime.decorate(
            19,
            4,
            vec![InputLine {
                line: 3,
                text: "\t  value".into(),
            }],
        );
        match response {
            Response::Decorations { revision, slots } => {
                assert_eq!(revision, 19);
                assert_eq!(slots.len(), 2);
                assert_eq!(slots[0].visual_start, 0);
                assert_eq!(slots[0].visual_length, 4);
                assert_eq!(slots[0].depth, 0);
                assert_eq!(slots[1].visual_start, 4);
                assert_eq!(slots[1].visual_length, 2);
                assert_eq!(slots[1].depth, 1);
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }
}
