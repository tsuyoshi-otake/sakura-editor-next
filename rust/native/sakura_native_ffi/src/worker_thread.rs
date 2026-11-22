//! Rust-owned, one-shot worker threads.
//!
//! Rust owns each `JoinHandle` until a caller claims its token. A join from
//! another thread waits for the body and reports its redacted outcome. A join
//! from the body itself consumes the token and detaches the worker.
//! Rust passes the opaque context back to the entry point without reading it.

use std::collections::BTreeMap;
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::thread::{self, Builder, JoinHandle};

use crate::ffi_pointer::is_valid_pointer;

/// Tags every worker token, so a handle belonging to another opaque-token
/// family can never be mistaken for a worker even if two registries happen to
/// agree on a small integer.
const WORKER_TOKEN_TAG: u64 = 1_u64 << 62;

/// Names every worker thread this module starts, so a debugger or a crash dump
/// attributes the thread to this boundary rather than to an anonymous spawn.
const WORKER_THREAD_NAME: &str = "sakura-native-worker";

/// Typed result returned by every worker-thread export.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraWorkerThreadStatus {
    Ok = 0,
    InvalidArgument = 1,
    SpawnFailed = 2,
    NotFound = 3,
    InternalError = 4,
    WouldDeadlock = 5,
}

/// Redacted outcome defined by the C++ callback trampoline.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraWorkerBodyOutcome {
    Completed = 0,
    ThrewException = 1,
    ThrewUnknown = 2,
}

/// The body one worker runs exactly once, on its own thread.
///
/// The entry point must contain any exceptions and return a raw outcome code.
/// An undefined code makes join return `InternalError`.
pub type SakuraWorkerThreadEntryV2 = unsafe extern "C" fn(context: *mut c_void) -> u32;

/// An opaque C++ owner address.
///
/// Rust moves it to the worker and passes it back to the entry point.
struct WorkerContext(*mut c_void);

impl WorkerContext {
    /// Returns the opaque address to hand back to the entry point.
    ///
    /// Reading it through a method rather than the tuple field keeps the
    /// capture whole: a closure that touched the field directly would capture
    /// the raw pointer, which is not `Send`, instead of this wrapper.
    fn address(&self) -> *mut c_void {
        self.0
    }
}

// SAFETY: The address is opaque to Rust, which performs no access through it.
// The caller keeps the context valid for the entry point's use, even if the
// worker consumes its token by joining itself.
unsafe impl Send for WorkerContext {}

struct WorkerRegistry {
    next_token: u64,
    workers: BTreeMap<u64, JoinHandle<Option<SakuraWorkerBodyOutcome>>>,
}

static WORKERS: OnceLock<Mutex<WorkerRegistry>> = OnceLock::new();

fn workers() -> &'static Mutex<WorkerRegistry> {
    WORKERS.get_or_init(|| {
        Mutex::new(WorkerRegistry {
            next_token: WORKER_TOKEN_TAG | 1,
            workers: BTreeMap::new(),
        })
    })
}

fn lock_workers() -> MutexGuard<'static, WorkerRegistry> {
    workers()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Reserves the next worker token without publishing anything under it.
///
/// The token is taken before the thread starts.  Reserving afterwards would
/// leave a running body with no reachable handle whenever the reservation
/// failed, and the only honest way to release such a body is a join the caller
/// can no longer ask for.
fn reserve_token() -> Option<u64> {
    let mut registry = lock_workers();
    let token = registry.next_token;
    if token == 0 || token & WORKER_TOKEN_TAG == 0 {
        return None;
    }
    registry.next_token = token.checked_add(1).unwrap_or(0);
    Some(token)
}

fn zero_token_if_valid(token: *mut u64) {
    if is_valid_pointer(token.cast_const()) {
        // SAFETY: The slot passes the address checks and the caller supplies
        // writable storage for the duration of this call.
        unsafe { token.write(0) };
    }
}

/// Starts one Rust-owned worker thread and publishes its token.
///
/// The token slot is cleared before any fallible work, so a caller that ignores
/// the status still observes an empty handle rather than a stale one.
///
/// # Safety
///
/// `token` must address caller-owned writable storage for one `u64`, borrowed
/// only for this call. `entry` must be safe to call once from another thread
/// with `context` and must not unwind.
/// `context` must stay valid for the body even when a self-join detaches it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_worker_thread_start_v2(
    entry: Option<SakuraWorkerThreadEntryV2>,
    context: *mut c_void,
    token: *mut u64,
) -> SakuraWorkerThreadStatus {
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
            return SakuraWorkerThreadStatus::InvalidArgument;
        }
        // SAFETY: The token slot is validated and caller-owned for this call.
        unsafe { token.write(0) };
        let Some(entry) = entry else {
            return SakuraWorkerThreadStatus::InvalidArgument;
        };
        let Some(worker_token) = reserve_token() else {
            return SakuraWorkerThreadStatus::InternalError;
        };
        let context = WorkerContext(context);
        let spawned = Builder::new()
            .name(String::from(WORKER_THREAD_NAME))
            .spawn(move || {
                // SAFETY: The caller promised this entry point is callable once
                // from another thread with this context and does not unwind.
                let raw_outcome = unsafe { entry(context.address()) };
                match raw_outcome {
                    0 => Some(SakuraWorkerBodyOutcome::Completed),
                    1 => Some(SakuraWorkerBodyOutcome::ThrewException),
                    2 => Some(SakuraWorkerBodyOutcome::ThrewUnknown),
                    _ => None,
                }
            });
        let Ok(handle) = spawned else {
            return SakuraWorkerThreadStatus::SpawnFailed;
        };
        lock_workers().workers.insert(worker_token, handle);
        // SAFETY: The token slot remains caller-owned for this call.
        unsafe { token.write(worker_token) };
        SakuraWorkerThreadStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => {
            // A token is published only after the whole start transaction
            // succeeds.  Keep the caller's slot unambiguously empty otherwise.
            zero_token_if_valid(token);
            SakuraWorkerThreadStatus::InternalError
        }
    }
}

/// Claims the worker named by `*token` and clears the slot before waiting.
/// A self-join detaches with `WouldDeadlock`. A Rust worker panic or an undefined
/// callback outcome returns `InternalError`. Only a successful join writes
/// the body's outcome.
///
/// # Safety
///
/// `token` must address caller-owned writable storage holding one token from
/// [`sakura_worker_thread_start_v2`]. `outcome` must address writable storage
/// for one `SakuraWorkerBodyOutcome`. The slots are independent and borrowed
/// exclusively for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_worker_thread_join_v2(
    token: *mut u64,
    outcome: *mut SakuraWorkerBodyOutcome,
) -> SakuraWorkerThreadStatus {
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) || !is_valid_pointer(outcome.cast_const()) {
            return SakuraWorkerThreadStatus::InvalidArgument;
        }
        // SAFETY: The token slot is validated and caller-owned for this call.
        let worker_token = unsafe { token.read() };
        if worker_token == 0 || worker_token & WORKER_TOKEN_TAG == 0 {
            return SakuraWorkerThreadStatus::InvalidArgument;
        }
        // The registry lock is released before the join: a join lasts as long
        // as the worker body runs, and no other owner may be blocked from
        // starting or releasing its own worker for that whole time.
        let handle = lock_workers().workers.remove(&worker_token);
        let Some(handle) = handle else {
            return SakuraWorkerThreadStatus::NotFound;
        };
        // SAFETY: The token slot remains caller-owned for this call.
        unsafe { token.write(0) };
        if handle.thread().id() == thread::current().id() {
            drop(handle);
            return SakuraWorkerThreadStatus::WouldDeadlock;
        }
        let Ok(Some(body_outcome)) = handle.join() else {
            return SakuraWorkerThreadStatus::InternalError;
        };
        // SAFETY: The outcome slot remains caller-owned for this call.
        unsafe { outcome.write(body_outcome) };
        SakuraWorkerThreadStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => SakuraWorkerThreadStatus::InternalError,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::mpsc::{self, Receiver, Sender};
    use std::sync::Barrier;
    use std::time::Duration;

    type Status = SakuraWorkerThreadStatus;
    type Outcome = SakuraWorkerBodyOutcome;

    /// What one test worker reports back.
    ///
    /// The entry point receives this value's address and does the only things a
    /// real owner's worker does: observe its own state and publish progress.
    struct Observed {
        entered: AtomicU64,
        stop: AtomicU64,
        finished: AtomicU64,
    }

    impl Observed {
        fn new() -> Self {
            Self {
                entered: AtomicU64::new(0),
                stop: AtomicU64::new(0),
                finished: AtomicU64::new(0),
            }
        }

        fn context(&self) -> *mut c_void {
            std::ptr::from_ref(self).cast_mut().cast()
        }
    }

    unsafe extern "C" fn enter_and_return(context: *mut c_void) -> u32 {
        // SAFETY: Every caller below passes the address of a live `Observed`
        // that outlives the join.
        let observed = unsafe { &*context.cast::<Observed>() };
        observed.entered.fetch_add(1, Ordering::SeqCst);
        observed.finished.fetch_add(1, Ordering::SeqCst);
        Outcome::Completed as u32
    }

    unsafe extern "C" fn spin_until_stopped(context: *mut c_void) -> u32 {
        // SAFETY: Every caller below passes the address of a live `Observed`
        // that outlives the join.
        let observed = unsafe { &*context.cast::<Observed>() };
        observed.entered.fetch_add(1, Ordering::SeqCst);
        while observed.stop.load(Ordering::SeqCst) == 0 {
            std::thread::yield_now();
        }
        observed.finished.fetch_add(1, Ordering::SeqCst);
        Outcome::Completed as u32
    }

    fn start(entry: SakuraWorkerThreadEntryV2, observed: &Observed, token: &mut u64) -> Status {
        // SAFETY: `observed` outlives every join in these tests, and `token` is
        // initialized local storage borrowed only for this call.
        unsafe { sakura_worker_thread_start_v2(Some(entry), observed.context(), token) }
    }

    fn join(token: &mut u64) -> Status {
        let mut outcome = Outcome::Completed;
        // SAFETY: Both slots are exclusive and valid for this call.
        unsafe { sakura_worker_thread_join_v2(token, &mut outcome) }
    }

    #[test]
    fn a_started_worker_runs_its_entry_point_and_joins() {
        let observed = Observed::new();
        let mut token = 0_u64;
        assert_eq!(Status::Ok, start(enter_and_return, &observed, &mut token));
        assert_ne!(0, token & WORKER_TOKEN_TAG);

        assert_eq!(Status::Ok, join(&mut token));
        assert_eq!(0, token, "the join must release the caller handle");
        assert_eq!(1, observed.entered.load(Ordering::SeqCst));
    }

    #[test]
    fn the_join_waits_for_a_worker_that_is_still_running() {
        let observed = Observed::new();
        let mut token = 0_u64;
        assert_eq!(Status::Ok, start(spin_until_stopped, &observed, &mut token));
        while observed.entered.load(Ordering::SeqCst) == 0 {
            std::thread::yield_now();
        }
        assert_eq!(
            0,
            observed.finished.load(Ordering::SeqCst),
            "the worker must still be running, or this case proves nothing"
        );

        observed.stop.store(1, Ordering::SeqCst);
        assert_eq!(Status::Ok, join(&mut token));
        assert_eq!(
            1,
            observed.finished.load(Ordering::SeqCst),
            "the join must not return before the body completes"
        );
    }

    #[test]
    fn a_second_join_of_the_same_token_finds_nothing() {
        let observed = Observed::new();
        let mut token = 0_u64;
        assert_eq!(Status::Ok, start(enter_and_return, &observed, &mut token));
        let issued = token;
        assert_eq!(Status::Ok, join(&mut token));

        let mut stale = issued;
        assert_eq!(Status::NotFound, join(&mut stale));
        assert_eq!(issued, stale, "a refused join must not clear the slot");
    }

    #[test]
    fn a_start_without_an_entry_point_is_refused() {
        let mut token = u64::MAX;
        // SAFETY: `token` is initialized local storage borrowed only for this
        // call.
        let status = unsafe { sakura_worker_thread_start_v2(None, ptr::null_mut(), &mut token) };
        assert_eq!(Status::InvalidArgument, status);
        assert_eq!(0, token, "a refused start must leave an empty handle");
    }

    #[test]
    fn a_start_without_a_token_slot_is_refused() {
        let observed = Observed::new();
        // SAFETY: The null token slot is exactly what this case exercises, and
        // `observed` outlives the call.
        let status = unsafe {
            sakura_worker_thread_start_v2(
                Some(enter_and_return),
                observed.context(),
                ptr::null_mut(),
            )
        };
        assert_eq!(Status::InvalidArgument, status);
        assert_eq!(
            0,
            observed.entered.load(Ordering::SeqCst),
            "a refused start must not run the body"
        );
    }

    #[test]
    fn a_join_of_an_empty_or_foreign_token_is_refused() {
        let mut empty = 0_u64;
        assert_eq!(Status::InvalidArgument, join(&mut empty));

        let mut untagged = 1_u64;
        assert_eq!(Status::InvalidArgument, join(&mut untagged));

        let mut outcome = Outcome::ThrewUnknown;
        // SAFETY: The null token slot is exactly what this case exercises.
        let status = unsafe { sakura_worker_thread_join_v2(ptr::null_mut(), &mut outcome) };
        assert_eq!(Status::InvalidArgument, status);
        assert_eq!(Outcome::ThrewUnknown, outcome);
    }

    #[test]
    fn every_started_worker_gets_its_own_token() {
        let observed = Observed::new();
        let mut first = 0_u64;
        let mut second = 0_u64;
        assert_eq!(Status::Ok, start(enter_and_return, &observed, &mut first));
        assert_eq!(Status::Ok, start(enter_and_return, &observed, &mut second));
        assert_ne!(first, second);

        assert_eq!(Status::Ok, join(&mut first));
        assert_eq!(Status::Ok, join(&mut second));
        assert_eq!(2, observed.entered.load(Ordering::SeqCst));
    }

    unsafe extern "C" fn return_raw_outcome(context: *mut c_void) -> u32 {
        // SAFETY: The caller keeps an initialized u32 alive through join.
        unsafe { *context.cast::<u32>() }
    }

    #[test]
    fn join_round_trips_every_body_outcome() {
        for (expected, mut outcome) in [
            (Outcome::Completed, Outcome::ThrewUnknown),
            (Outcome::ThrewException, Outcome::Completed),
            (Outcome::ThrewUnknown, Outcome::Completed),
        ] {
            let mut token = 0;
            let raw = expected as u32;
            let context = ptr::from_ref(&raw).cast_mut().cast();
            // SAFETY: The callback only reads `raw`, which outlives join.
            let status = unsafe {
                sakura_worker_thread_start_v2(Some(return_raw_outcome), context, &mut token)
            };
            assert_eq!(Status::Ok, status);
            // SAFETY: Both slots are exclusive and valid for this call.
            assert_eq!(Status::Ok, unsafe {
                sakura_worker_thread_join_v2(&mut token, &mut outcome)
            });
            assert_eq!(expected, outcome);
            assert_eq!(0, token);
        }
    }

    #[test]
    fn an_undefined_callback_outcome_consumes_the_token_without_writing_an_outcome() {
        let raw = 7_u32;
        let context = ptr::from_ref(&raw).cast_mut().cast();
        let mut token = 0;
        // SAFETY: The callback only reads `raw`, which outlives join.
        let status =
            unsafe { sakura_worker_thread_start_v2(Some(return_raw_outcome), context, &mut token) };
        assert_eq!(Status::Ok, status);
        let mut issued = token;

        let mut outcome = Outcome::ThrewUnknown;
        // SAFETY: Both slots are exclusive and valid for this call.
        let status = unsafe { sakura_worker_thread_join_v2(&mut token, &mut outcome) };
        assert_eq!(
            (Status::InternalError, 0, Outcome::ThrewUnknown),
            (status, token, outcome)
        );
        assert_eq!(Status::NotFound, join(&mut issued));
    }

    struct SelfJoin {
        token: Receiver<u64>,
        result: Sender<(Status, u64, Outcome)>,
    }

    unsafe extern "C" fn join_self(context: *mut c_void) -> u32 {
        // SAFETY: A successful start transfers this box to the worker.
        let state = unsafe { Box::from_raw(context.cast::<SelfJoin>()) };
        if let Ok(mut token) = state.token.recv() {
            let mut outcome = Outcome::ThrewUnknown;
            // SAFETY: Both slots are exclusive and valid for this call.
            let status = unsafe { sakura_worker_thread_join_v2(&mut token, &mut outcome) };
            let _ = state.result.send((status, token, outcome));
        }
        Outcome::Completed as u32
    }

    #[test]
    fn self_join_consumes_the_token_without_waiting_or_writing_an_outcome() {
        let (send_token, token) = mpsc::channel();
        let (result, receive_result) = mpsc::channel();
        let context = Box::into_raw(Box::new(SelfJoin { token, result }));
        let mut issued = 0;
        // SAFETY: The worker adopts the live context on success.
        let status =
            unsafe { sakura_worker_thread_start_v2(Some(join_self), context.cast(), &mut issued) };
        if status != Status::Ok {
            // SAFETY: On failure no thread adopted the box.
            drop(unsafe { Box::from_raw(context) });
        }
        assert_eq!(Status::Ok, status);
        send_token.send(issued).unwrap();
        assert_eq!(
            (Status::WouldDeadlock, 0, Outcome::ThrewUnknown),
            receive_result.recv_timeout(Duration::from_secs(5)).unwrap()
        );
        assert_eq!(Status::NotFound, join(&mut issued));
    }

    #[test]
    fn concurrent_joins_report_one_outcome() {
        const CALLERS: usize = 4;
        let observed = Observed::new();
        let mut issued = 0;
        assert_eq!(
            Status::Ok,
            start(spin_until_stopped, &observed, &mut issued)
        );
        let gate = Barrier::new(CALLERS);
        let (report, results) = mpsc::channel();

        let rejected = thread::scope(|scope| {
            for _ in 0..CALLERS {
                let gate = &gate;
                let report = report.clone();
                scope.spawn(move || {
                    let mut token = issued;
                    let mut outcome = Outcome::ThrewUnknown;
                    gate.wait();
                    // SAFETY: Each caller owns independent token and outcome slots.
                    let status = unsafe { sakura_worker_thread_join_v2(&mut token, &mut outcome) };
                    report.send((status, token, outcome)).unwrap();
                });
            }
            // The successful join is waiting on the body here. Losing calls
            // must finish before the worker is released.
            let mut rejected = Vec::new();
            for _ in 1..CALLERS {
                let Ok(result) = results.recv_timeout(Duration::from_secs(5)) else {
                    break;
                };
                rejected.push(result);
            }
            observed.stop.store(1, Ordering::SeqCst);
            rejected
        });

        assert_eq!(CALLERS - 1, rejected.len());
        for result in rejected {
            assert_eq!((Status::NotFound, issued, Outcome::ThrewUnknown), result);
        }
        assert_eq!(
            (Status::Ok, 0, Outcome::Completed),
            results.try_recv().unwrap()
        );
        assert_eq!(1, observed.entered.load(Ordering::SeqCst));
    }

    unsafe extern "C" fn start_and_join_another_worker(context: *mut c_void) -> u32 {
        // SAFETY: The outer caller keeps the observation alive through join.
        let observed = unsafe { &*context.cast::<Observed>() };
        let mut token = 0;
        if start(enter_and_return, observed, &mut token) == Status::Ok
            && join(&mut token) == Status::Ok
        {
            Outcome::Completed as u32
        } else {
            Outcome::ThrewUnknown as u32
        }
    }

    #[test]
    fn a_running_body_can_start_and_join_another_worker() {
        let observed = Observed::new();
        let mut token = 0;
        assert_eq!(
            Status::Ok,
            start(start_and_join_another_worker, &observed, &mut token)
        );

        let mut outcome = Outcome::ThrewUnknown;
        // SAFETY: Both slots are exclusive and valid for this call.
        assert_eq!(Status::Ok, unsafe {
            sakura_worker_thread_join_v2(&mut token, &mut outcome)
        });
        assert_eq!(Outcome::Completed, outcome);
        assert_eq!(1, observed.finished.load(Ordering::SeqCst));
    }

    #[test]
    fn a_rust_worker_panic_returns_internal_error_without_an_outcome() {
        let mut token = reserve_token().unwrap();
        let handle = thread::spawn(|| panic!("worker wrapper panic"));
        lock_workers().workers.insert(token, handle);

        let mut outcome = Outcome::ThrewUnknown;
        // SAFETY: Both slots are exclusive and valid for this call.
        let status = unsafe { sakura_worker_thread_join_v2(&mut token, &mut outcome) };
        assert_eq!(
            (Status::InternalError, 0, Outcome::ThrewUnknown),
            (status, token, outcome)
        );
    }

    #[test]
    fn an_invalid_outcome_slot_does_not_claim_the_token() {
        let observed = Observed::new();
        let mut token = 0;
        assert_eq!(Status::Ok, start(enter_and_return, &observed, &mut token));
        let issued = token;

        // SAFETY: The null outcome slot is exactly what this case exercises.
        let status = unsafe { sakura_worker_thread_join_v2(&mut token, ptr::null_mut()) };
        assert_eq!(Status::InvalidArgument, status);
        assert_eq!(issued, token);
        assert_eq!(Status::Ok, join(&mut token));
    }
}
