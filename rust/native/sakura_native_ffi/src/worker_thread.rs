//! Rust-owned worker threads.
//!
//! Three C++ owners each used to hold a `std::thread` member, raise their own
//! stopping flag, wake their own condition variable, and then remember to join
//! in teardown.  The join was a convention restated by hand in every owner, and
//! an owner that forgot it left a thread running against destroyed state.
//!
//! Here Rust owns the `JoinHandle` and joins it in `Drop`.  The C++ owner holds
//! only a move-only handle whose destructor releases the token, so joining is
//! what destruction *is* rather than a step teardown has to remember.  The only
//! way to keep a worker alive is to keep its handle alive.
//!
//! Two values cross the ABI: the entry point and an opaque context address.
//! Rust never dereferences the context.  It moves the address to the worker
//! thread, hands it back to the entry point once, and otherwise treats it as an
//! integer.

use std::collections::BTreeMap;
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::thread::{Builder, JoinHandle};

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
}

/// The body one worker runs exactly once, on its own thread.
///
/// The C++ owner declares its entry point `noexcept`.  The pointer is a plain
/// `extern "C"` function, so an exception that still escaped would terminate at
/// the boundary instead of unwinding into Rust.
pub type SakuraWorkerThreadEntryV1 = unsafe extern "C" fn(context: *mut c_void);

/// An opaque C++ owner address.
///
/// Rust stores it, moves it to the worker thread, and passes it back to the
/// entry point.  It is never dereferenced, and it never outlives the join,
/// because the owning C++ handle joins from its own destructor while the
/// pointed-to storage is still alive.
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
// Moving it to the worker thread is precisely what the caller asked for, and
// the caller keeps the pointed-to owner alive until the matching join returns.
unsafe impl Send for WorkerContext {}

/// One started worker whose lifetime is exactly this value's lifetime.
///
/// `Drop` joins, so releasing a worker and joining it are the same event and
/// cannot drift apart.
struct WorkerThread {
    handle: Option<JoinHandle<()>>,
}

impl Drop for WorkerThread {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            // The C++ owner raises its own stop before releasing the handle.  A
            // worker that ignored that signal blocks here rather than running on
            // against destroyed state, which is the failure we want to have.
            let _ = handle.join();
        }
    }
}

struct WorkerRegistry {
    next_token: u64,
    workers: BTreeMap<u64, WorkerThread>,
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
        // SAFETY: The address/range check above proves that the caller-owned
        // token slot is writable for this call.
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
/// only for this call.  `entry` must be safe to call once from another thread
/// with `context`, and `context` must stay valid until the matching
/// [`sakura_worker_thread_join_v1`] returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_worker_thread_start_v1(
    entry: Option<SakuraWorkerThreadEntryV1>,
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
                // from another thread with this context, and that the context
                // outlives the join that releases this thread.
                unsafe { entry(context.address()) };
            });
        let Ok(handle) = spawned else {
            return SakuraWorkerThreadStatus::SpawnFailed;
        };
        lock_workers().workers.insert(
            worker_token,
            WorkerThread {
                handle: Some(handle),
            },
        );
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

/// Joins the worker named by `*token` and clears the slot.
///
/// The slot is cleared before the join blocks, so the caller's handle is
/// already released no matter how long the worker takes to notice its stop.
///
/// # Safety
///
/// `token` must address caller-owned writable storage holding one token from
/// [`sakura_worker_thread_start_v1`], borrowed only for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_worker_thread_join_v1(token: *mut u64) -> SakuraWorkerThreadStatus {
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
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
        let Some(worker) = lock_workers().workers.remove(&worker_token) else {
            return SakuraWorkerThreadStatus::NotFound;
        };
        // SAFETY: The token slot remains caller-owned for this call.
        unsafe { token.write(0) };
        drop(worker);
        SakuraWorkerThreadStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => SakuraWorkerThreadStatus::InternalError,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

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

    unsafe extern "C" fn enter_and_return(context: *mut c_void) {
        // SAFETY: Every caller below passes the address of a live `Observed`
        // that outlives the join.
        let observed = unsafe { &*context.cast::<Observed>() };
        observed.entered.fetch_add(1, Ordering::SeqCst);
        observed.finished.fetch_add(1, Ordering::SeqCst);
    }

    unsafe extern "C" fn spin_until_stopped(context: *mut c_void) {
        // SAFETY: Every caller below passes the address of a live `Observed`
        // that outlives the join.
        let observed = unsafe { &*context.cast::<Observed>() };
        observed.entered.fetch_add(1, Ordering::SeqCst);
        while observed.stop.load(Ordering::SeqCst) == 0 {
            std::thread::yield_now();
        }
        observed.finished.fetch_add(1, Ordering::SeqCst);
    }

    type Status = SakuraWorkerThreadStatus;

    fn start(entry: SakuraWorkerThreadEntryV1, observed: &Observed, token: &mut u64) -> Status {
        // SAFETY: `observed` outlives every join in these tests, and `token` is
        // initialized local storage borrowed only for this call.
        unsafe { sakura_worker_thread_start_v1(Some(entry), observed.context(), token) }
    }

    fn join(token: &mut u64) -> Status {
        // SAFETY: `token` is initialized local storage borrowed only for this
        // call.
        unsafe { sakura_worker_thread_join_v1(token) }
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
        let status =
            unsafe { sakura_worker_thread_start_v1(None, std::ptr::null_mut(), &mut token) };
        assert_eq!(Status::InvalidArgument, status);
        assert_eq!(0, token, "a refused start must leave an empty handle");
    }

    #[test]
    fn a_start_without_a_token_slot_is_refused() {
        let observed = Observed::new();
        // SAFETY: The null token slot is exactly what this case exercises, and
        // `observed` outlives the call.
        let status = unsafe {
            sakura_worker_thread_start_v1(
                Some(enter_and_return),
                observed.context(),
                std::ptr::null_mut(),
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

        // SAFETY: The null token slot is exactly what this case exercises.
        let status = unsafe { sakura_worker_thread_join_v1(std::ptr::null_mut()) };
        assert_eq!(Status::InvalidArgument, status);
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
}
