//! Aggregate guest resource budget, shared by the v1 and v2 hosts.
//!
//! `StoreLimits.memory_size` bounds EACH memory separately, so N memories could
//! commit N x 32 MiB; `table_elements` likewise bounds each table alone. This
//! limiter instead charges every approved growth against store-wide sums.
//!
//! Charges are final once approved and are never refunded. The failure
//! callbacks carry no size, and wasmtime 49.0.2 also calls them WITHOUT a
//! preceding approval: `table_grow_failed` when the new table size overflows
//! (`src/runtime/vm/table.rs:642-647`, reachable through `Table::grow` and
//! 64-bit table indices) and `memory_grow_failed` when a 1-byte-page memory
//! would exceed its index range (`src/runtime/vm/memory.rs:643-648`;
//! `allow_growth_to` is always true for 64 KiB pages). A refund keyed to the
//! last approval would therefore hand back an earlier, successful grant, and a
//! guest could repeat that forever. Over-counting after a post-approval OS
//! allocation failure is fail-closed.
use wasmtime::ResourceLimiter;

/// Sum of all linear memories in the store. Every shipped built-in and the
/// sample carry exactly one core memory (17-18 min pages); a WASI adapter
/// would add one, hence the count headroom below rather than the byte budget.
pub const GUEST_MEMORY_TOTAL_BYTES: usize = 32 * 1024 * 1024;
/// Linear memories per store. Observed: 1 per component.
pub const GUEST_MEMORIES: usize = 4;
/// Tables per store. Observed: 1 per component.
pub const GUEST_TABLES: usize = 4;
/// Sum of all table elements. Shipped guests declare 36-106 elements with
/// min == max; each element costs one usize of host memory.
pub const GUEST_TABLE_ELEMENTS: usize = 1_048_576;
/// Instances per store (wit-component glue instantiates within this today).
pub const GUEST_INSTANCES: usize = 1;

#[derive(Default)]
pub struct GuestLimits {
    memory_bytes: usize,
    table_elements: usize,
}

impl ResourceLimiter for GuestLimits {
    fn memory_growing(
        &mut self,
        current: usize,
        desired: usize,
        maximum: Option<usize>,
    ) -> wasmtime::Result<bool> {
        let delta = desired.saturating_sub(current);
        let allow = maximum.is_none_or(|max| desired <= max)
            && delta <= GUEST_MEMORY_TOTAL_BYTES.saturating_sub(self.memory_bytes);
        if allow {
            self.memory_bytes += delta;
        }
        Ok(allow)
    }

    fn table_growing(
        &mut self,
        current: usize,
        desired: usize,
        maximum: Option<usize>,
    ) -> wasmtime::Result<bool> {
        let delta = desired.saturating_sub(current);
        let allow = maximum.is_none_or(|max| desired <= max)
            && delta <= GUEST_TABLE_ELEMENTS.saturating_sub(self.table_elements);
        if allow {
            self.table_elements += delta;
        }
        Ok(allow)
    }

    fn instances(&self) -> usize {
        GUEST_INSTANCES
    }

    fn tables(&self) -> usize {
        GUEST_TABLES
    }

    fn memories(&self) -> usize {
        GUEST_MEMORIES
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasmtime::{Config, Engine, Instance, Module, Store};

    // Hand-encoded core modules (no `wat` crate in the lockfile; section
    // layout documented inline).
    /// `(module (memory 1)(memory 1))` with exports "m0"/"m1".
    const TWO_MEMORIES: &[u8] = &[
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x05, 0x02, 0x00, 0x01, 0x00, 0x01,
        0x07, 0x0b, 0x02, 0x02, 0x6d, 0x30, 0x02, 0x00, 0x02, 0x6d, 0x31, 0x02, 0x01,
    ];
    /// `(module (memory 1 512))` — declared max exactly 32 MiB, exported "m".
    const CAPPED_MEMORY: &[u8] = &[
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x05, 0x01, 0x01, 0x01, 0x80, 0x04,
        0x07, 0x05, 0x01, 0x01, 0x6d, 0x02, 0x00,
    ];
    /// `(module (memory 1) x5)` — over the per-store memory count bound.
    const FIVE_MEMORIES: &[u8] = &[
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x0b, 0x05, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
    ];
    /// `(module (table funcref 1) x5)` — over the per-store table count bound.
    const FIVE_TABLES: &[u8] = &[
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x05, 0x70, 0x00, 0x01, 0x70,
        0x00, 0x01, 0x70, 0x00, 0x01, 0x70, 0x00, 0x01, 0x70, 0x00, 0x01,
    ];
    /// `(module (table funcref 1 2000000))` exported "t".
    const BIG_TABLE: &[u8] = &[
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x04, 0x07, 0x01, 0x70, 0x01, 0x01, 0x80,
        0x89, 0x7a, 0x07, 0x05, 0x01, 0x01, 0x74, 0x01, 0x00,
    ];

    fn limited_store(engine: &Engine) -> Store<GuestLimits> {
        let mut store = Store::new(engine, GuestLimits::default());
        store.limiter(|state| state);
        store
    }

    #[test]
    fn aggregate_memory_bound_charges_across_memories() {
        let mut limits = GuestLimits::default();
        // One memory may use almost the whole budget.
        assert!(limits.memory_growing(0, 30 * 1024 * 1024, None).unwrap());
        // A second memory only gets what is left.
        assert!(!limits.memory_growing(0, 3 * 1024 * 1024, None).unwrap());
        assert!(limits.memory_growing(0, 2 * 1024 * 1024, None).unwrap());
        // Budget is full now.
        assert!(!limits.memory_growing(0, 1, None).unwrap());
        // Declared maximums are enforced even inside the budget.
        let mut fresh = GuestLimits::default();
        assert!(!fresh
            .memory_growing(0, 64 * 1024, Some(64 * 1024 - 1))
            .unwrap());
    }

    #[test]
    fn grow_failed_is_never_refunded() {
        // The trait default keeps charges on failure: a guest that banks a
        // grant then triggers a no-approval failure cannot drain the budget.
        let mut limits = GuestLimits::default();
        assert!(limits.memory_growing(0, 31 * 1024 * 1024, None).unwrap());
        limits
            .memory_grow_failed(wasmtime::Error::msg("simulated type-limit failure"))
            .unwrap();
        assert!(!limits.memory_growing(0, 2 * 1024 * 1024, None).unwrap());

        assert!(limits
            .table_growing(0, GUEST_TABLE_ELEMENTS - 8, None)
            .unwrap());
        limits
            .table_grow_failed(wasmtime::Error::msg("simulated overflow"))
            .unwrap();
        assert!(!limits.table_growing(0, 16, None).unwrap());
    }

    #[test]
    fn element_bound_sums_across_tables() {
        let mut limits = GuestLimits::default();
        assert!(limits
            .table_growing(0, GUEST_TABLE_ELEMENTS - 8, None)
            .unwrap());
        assert!(!limits.table_growing(0, 9, None).unwrap());
        assert!(limits.table_growing(0, 8, None).unwrap());
        assert!(!limits.table_growing(0, 1, None).unwrap());
        // Type maximum still wins inside the budget.
        let mut fresh = GuestLimits::default();
        assert!(!fresh.table_growing(0, 50, Some(49)).unwrap());
    }

    #[test]
    fn real_engine_two_memories_share_one_32mib_budget() {
        let engine = Engine::new(&Config::new()).unwrap();
        let mut store = limited_store(&engine);
        let module = Module::new(&engine, TWO_MEMORIES).unwrap();
        let instance = Instance::new(&mut store, &module, &[]).unwrap();
        let m0 = instance.get_memory(&mut store, "m0").unwrap();
        let m1 = instance.get_memory(&mut store, "m1").unwrap();
        // 20 MiB + 20 MiB > 32 MiB aggregate: second grow must fail, while the
        // first stays within the budget and succeeds (liveness).
        assert!(m0.grow(&mut store, 320).is_ok());
        assert!(m1.grow(&mut store, 320).is_err());
        // Instantiation charged the two min pages; only 190 pages of headroom
        // remain (512 - 2 - 320).
        assert!(m1.grow(&mut store, 190).is_ok());
        assert!(m1.grow(&mut store, 1).is_err());
    }

    #[test]
    fn real_engine_declared_maximum_denial_is_not_charged() {
        // The limiter, not wasmtime, refuses growth past the declared maximum
        // (wasmtime asks the limiter first), and a refused grow charges nothing.
        let engine = Engine::new(&Config::new()).unwrap();
        let mut store = limited_store(&engine);
        let module = Module::new(&engine, CAPPED_MEMORY).unwrap();
        let instance = Instance::new(&mut store, &module, &[]).unwrap();
        let memory = instance.get_memory(&mut store, "m").unwrap();
        // 511 pages = 31.9 MiB.
        assert!(memory.grow(&mut store, 510).is_ok());
        // Past the declared 512-page maximum.
        assert!(memory.grow(&mut store, 2).is_err());
        assert_eq!(store.data().memory_bytes, 511 * 65536);
        // Exactly one page of aggregate headroom remains, no more.
        assert!(memory.grow(&mut store, 1).is_ok());
        assert_eq!(store.data().memory_bytes, 512 * 65536);
    }

    #[test]
    fn real_engine_unapproved_table_failure_keeps_the_charge() {
        // An overflowing size reaches table_grow_failed without a preceding
        // table_growing call (table.rs:642-647). A refund keyed to the last
        // approval would hand back the 1,000,000-element grant made before it.
        let engine = Engine::new(&Config::new()).unwrap();
        let mut store = limited_store(&engine);
        let module = Module::new(&engine, BIG_TABLE).unwrap();
        let instance = Instance::new(&mut store, &module, &[]).unwrap();
        let table = instance.get_table(&mut store, "t").unwrap();
        assert!(table
            .grow(&mut store, 1_000_000, wasmtime::Ref::Func(None))
            .is_ok());
        assert!(table
            .grow(&mut store, u64::MAX, wasmtime::Ref::Func(None))
            .is_err());
        assert_eq!(store.data().table_elements, 1_000_001);
        assert!(table
            .grow(&mut store, 48_576, wasmtime::Ref::Func(None))
            .is_err());
    }

    #[test]
    fn real_engine_table_element_bound_and_counts() {
        let engine = Engine::new(&Config::new()).unwrap();
        let mut store = limited_store(&engine);
        let module = Module::new(&engine, BIG_TABLE).unwrap();
        let instance = Instance::new(&mut store, &module, &[]).unwrap();
        let table = instance.get_table(&mut store, "t").unwrap();
        // Instantiation charged the min element; delta accounting starts there.
        // Declared max is 2,000,000; the aggregate bound stops it first.
        assert!(table
            .grow(&mut store, 1_999_998, wasmtime::Ref::Func(None))
            .is_err());
        // A grow inside both bounds works and is charged.
        assert!(table
            .grow(&mut store, 1_000_000, wasmtime::Ref::Func(None))
            .is_ok());
        assert_eq!(store.data().table_elements, 1_000_001);
        // Remaining aggregate headroom is 48,575: the next grow stays denied.
        assert!(table
            .grow(&mut store, 100_000, wasmtime::Ref::Func(None))
            .is_err());
        assert!(table
            .grow(&mut store, 48_575, wasmtime::Ref::Func(None))
            .is_ok());
    }

    #[test]
    fn real_engine_memory_and_table_count_bounds() {
        let engine = Engine::new(&Config::new()).unwrap();
        // Count bounds apply at instantiation: 5 of either must fail while a
        // two-memory module inside the bounds instantiates. One store per case
        // because instances() is 1.
        let over_mem = Module::new(&engine, FIVE_MEMORIES).unwrap();
        let mut s1 = limited_store(&engine);
        let err = Instance::new(&mut s1, &over_mem, &[]).unwrap_err();
        assert!(err.to_string().contains("memory count"), "{err}");
        let over_tab = Module::new(&engine, FIVE_TABLES).unwrap();
        let mut s2 = limited_store(&engine);
        let err = Instance::new(&mut s2, &over_tab, &[]).unwrap_err();
        assert!(err.to_string().contains("table count"), "{err}");
        let ok = Module::new(&engine, TWO_MEMORIES).unwrap();
        let mut s3 = limited_store(&engine);
        assert!(Instance::new(&mut s3, &ok, &[]).is_ok());
    }
}
