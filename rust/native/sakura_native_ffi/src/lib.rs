#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(improper_ctypes_definitions)]
#![deny(clippy::missing_safety_doc)]
#![deny(clippy::undocumented_unsafe_blocks)]

use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::sync::OnceLock;

use sakura_simd as native_simd;

mod output_provider;
mod output_shadow;
mod uri_candidate;

const ABI_VERSION_V1: u32 = 1;
const POLICY_COUNT_V1: usize = 3;

const CPU_FEATURE_AVX: u64 = 1 << 0;
const CPU_FEATURE_AVX2: u64 = 1 << 1;
const CPU_FEATURE_AVX512F: u64 = 1 << 2;
const CPU_FEATURE_AVX512BW: u64 = 1 << 3;
const KNOWN_CPU_FEATURES: u64 =
    CPU_FEATURE_AVX | CPU_FEATURE_AVX2 | CPU_FEATURE_AVX512F | CPU_FEATURE_AVX512BW;

const OS_STATE_XMM: u64 = 1 << 0;
const OS_STATE_YMM: u64 = 1 << 1;
const OS_STATE_OPMASK: u64 = 1 << 2;
const OS_STATE_ZMM_HI256: u64 = 1 << 3;
const OS_STATE_HI16_ZMM: u64 = 1 << 4;
const KNOWN_OS_STATES: u64 =
    OS_STATE_XMM | OS_STATE_YMM | OS_STATE_OPMASK | OS_STATE_ZMM_HI256 | OS_STATE_HI16_ZMM;

const OPERATION_FIND_CR_OR_LF_UTF16: u32 = 1;
const OPERATION_FIND_MARKDOWN_SPECIAL_UTF16: u32 = 2;
const OPERATION_FIND_CHAR_UTF16: u32 = 3;

const IMPLEMENTATION_CPP_AVX128: u32 = 1;
const IMPLEMENTATION_CPP_AVX2: u32 = 2;
const IMPLEMENTATION_CPP_AVX512BW: u32 = 3;
const IMPLEMENTATION_RUST_AVX128: u32 = 101;
const IMPLEMENTATION_RUST_AVX2: u32 = 102;
const IMPLEMENTATION_RUST_AVX512BW: u32 = 103;

/// Typed result returned by every native ABI export.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraStatus {
    Ok = 0,
    InvalidArgument = 1,
    Unsupported = 2,
    NotInitialized = 3,
    ConflictingInitialization = 4,
    InternalError = 5,
}

/// Raw CPU and OS extended-state capabilities detected by the C++ owner.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SakuraCpuCapabilitiesV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub raw_feature_bits: u64,
    pub os_extended_state_bits: u64,
    pub reserved: [u64; 4],
}

/// One operation-specific implementation and break-even policy selected by C++.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SakuraOperationPolicyV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub operation_id: u32,
    pub implementation_id: u32,
    pub minimum_length: u64,
    pub reserved: [u64; 3],
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Snapshot {
    capabilities: SakuraCpuCapabilitiesV1,
    policies: [SakuraOperationPolicyV1; POLICY_COUNT_V1],
}

struct InitializationCell {
    snapshot: OnceLock<Snapshot>,
}

impl InitializationCell {
    const fn new() -> Self {
        Self {
            snapshot: OnceLock::new(),
        }
    }

    fn initialize(&self, snapshot: Snapshot) -> SakuraStatus {
        if let Some(existing) = self.snapshot.get() {
            return if *existing == snapshot {
                SakuraStatus::Ok
            } else {
                SakuraStatus::ConflictingInitialization
            };
        }

        match self.snapshot.set(snapshot) {
            Ok(()) => SakuraStatus::Ok,
            Err(racing_snapshot) => {
                if self.snapshot.get() == Some(&racing_snapshot) {
                    SakuraStatus::Ok
                } else {
                    SakuraStatus::ConflictingInitialization
                }
            }
        }
    }
}

static INITIALIZATION: InitializationCell = InitializationCell::new();

#[derive(Clone, Copy)]
enum RequiredIsa {
    Avx128,
    Avx2,
    Avx512Bw,
}

fn has_all(value: u64, required: u64) -> bool {
    value & required == required
}

fn supports_isa(capabilities: &SakuraCpuCapabilitiesV1, required: RequiredIsa) -> bool {
    let avx = has_all(capabilities.raw_feature_bits, CPU_FEATURE_AVX)
        && has_all(
            capabilities.os_extended_state_bits,
            OS_STATE_XMM | OS_STATE_YMM,
        );
    match required {
        RequiredIsa::Avx128 => avx,
        RequiredIsa::Avx2 => avx && has_all(capabilities.raw_feature_bits, CPU_FEATURE_AVX2),
        RequiredIsa::Avx512Bw => {
            avx && has_all(
                capabilities.raw_feature_bits,
                CPU_FEATURE_AVX2 | CPU_FEATURE_AVX512F | CPU_FEATURE_AVX512BW,
            ) && has_all(
                capabilities.os_extended_state_bits,
                OS_STATE_OPMASK | OS_STATE_ZMM_HI256 | OS_STATE_HI16_ZMM,
            )
        }
    }
}

fn implementation_isa(implementation_id: u32) -> Option<RequiredIsa> {
    match implementation_id {
        IMPLEMENTATION_CPP_AVX128 | IMPLEMENTATION_RUST_AVX128 => Some(RequiredIsa::Avx128),
        IMPLEMENTATION_CPP_AVX2 | IMPLEMENTATION_RUST_AVX2 => Some(RequiredIsa::Avx2),
        IMPLEMENTATION_CPP_AVX512BW | IMPLEMENTATION_RUST_AVX512BW => Some(RequiredIsa::Avx512Bw),
        _ => None,
    }
}

fn validate_snapshot(snapshot: &Snapshot) -> SakuraStatus {
    let capabilities = &snapshot.capabilities;
    if capabilities.struct_size as usize != size_of::<SakuraCpuCapabilitiesV1>()
        || capabilities.abi_version != ABI_VERSION_V1
        || capabilities.reserved != [0; 4]
        || capabilities.raw_feature_bits & !KNOWN_CPU_FEATURES != 0
        || capabilities.os_extended_state_bits & !KNOWN_OS_STATES != 0
    {
        return SakuraStatus::InvalidArgument;
    }

    let mut seen_operations = 0_u8;
    for policy in &snapshot.policies {
        if policy.struct_size as usize != size_of::<SakuraOperationPolicyV1>()
            || policy.abi_version != ABI_VERSION_V1
            || policy.reserved != [0; 3]
        {
            return SakuraStatus::InvalidArgument;
        }
        let operation_bit = match policy.operation_id {
            OPERATION_FIND_CR_OR_LF_UTF16 => 1,
            OPERATION_FIND_MARKDOWN_SPECIAL_UTF16 => 2,
            OPERATION_FIND_CHAR_UTF16 => 4,
            _ => return SakuraStatus::InvalidArgument,
        };
        if seen_operations & operation_bit != 0 {
            return SakuraStatus::InvalidArgument;
        }
        seen_operations |= operation_bit;

        let Some(required_isa) = implementation_isa(policy.implementation_id) else {
            return SakuraStatus::Unsupported;
        };
        if !supports_isa(capabilities, required_isa) {
            return SakuraStatus::Unsupported;
        }
    }

    if seen_operations != 0b111 {
        SakuraStatus::InvalidArgument
    } else {
        SakuraStatus::Ok
    }
}

fn is_aligned<T>(pointer: *const T) -> bool {
    (pointer as usize).is_multiple_of(align_of::<T>())
}

unsafe fn copy_snapshot(
    capabilities: *const SakuraCpuCapabilitiesV1,
    policies: *const SakuraOperationPolicyV1,
    policy_count: u64,
) -> Result<Snapshot, SakuraStatus> {
    if capabilities.is_null()
        || !is_aligned(capabilities)
        || policies.is_null()
        || !is_aligned(policies)
        || policy_count != POLICY_COUNT_V1 as u64
    {
        return Err(SakuraStatus::InvalidArgument);
    }

    let policy_count = usize::try_from(policy_count).map_err(|_| SakuraStatus::InvalidArgument)?;
    // SAFETY: The ABI contract requires both pointers to reference initialized,
    // immutable values for this call. Alignment, nullability, and the bounded
    // policy count were checked above; values are copied before returning.
    let capabilities = unsafe { capabilities.read() };
    // SAFETY: See the pointer contract above. The count is exactly three.
    let policies = unsafe { slice::from_raw_parts(policies, policy_count) };
    let policies = [policies[0], policies[1], policies[2]];
    Ok(Snapshot {
        capabilities,
        policies,
    })
}

fn catch_status(operation: impl FnOnce() -> SakuraStatus) -> SakuraStatus {
    catch_unwind(AssertUnwindSafe(operation)).unwrap_or(SakuraStatus::InternalError)
}

/// Initializes the final native Rust library with C++-owned CPU facts and policy.
///
/// # Safety
///
/// `capabilities` must point to one immutable initialized V1 structure and
/// `policies` must point to exactly `policy_count` immutable initialized V1
/// structures for the duration of the call. The function copies all data and
/// retains no caller pointer. Identical repeated initialization is idempotent;
/// different contents are rejected.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_native_initialize_v1(
    capabilities: *const SakuraCpuCapabilitiesV1,
    policies: *const SakuraOperationPolicyV1,
    policy_count: u64,
) -> SakuraStatus {
    catch_status(|| {
        // SAFETY: The caller owns the pointer/lifetime contract documented above.
        let snapshot = match unsafe { copy_snapshot(capabilities, policies, policy_count) } {
            Ok(snapshot) => snapshot,
            Err(status) => return status,
        };
        let status = validate_snapshot(&snapshot);
        if status != SakuraStatus::Ok {
            return status;
        }
        INITIALIZATION.initialize(snapshot)
    })
}

fn validate_span(data: *const u16, length: u64) -> Result<usize, SakuraStatus> {
    let length = usize::try_from(length).map_err(|_| SakuraStatus::InvalidArgument)?;
    if length == 0 {
        return Ok(0);
    }
    if data.is_null() || !is_aligned(data) {
        return Err(SakuraStatus::InvalidArgument);
    }
    let byte_length = length
        .checked_mul(size_of::<u16>())
        .filter(|&bytes| bytes <= isize::MAX as usize)
        .ok_or(SakuraStatus::InvalidArgument)?;
    (data as usize)
        .checked_add(byte_length)
        .ok_or(SakuraStatus::InvalidArgument)?;
    Ok(length)
}

fn validate_byte_span(data: *const u8, length: u64) -> Result<usize, SakuraStatus> {
    let length = usize::try_from(length).map_err(|_| SakuraStatus::InvalidArgument)?;
    if length == 0 {
        return Ok(0);
    }
    if data.is_null() {
        return Err(SakuraStatus::InvalidArgument);
    }
    if length > isize::MAX as usize {
        return Err(SakuraStatus::InvalidArgument);
    }
    (data as usize)
        .checked_add(length)
        .ok_or(SakuraStatus::InvalidArgument)?;
    Ok(length)
}

unsafe fn run_scan(
    initialization: &InitializationCell,
    required_isa: RequiredIsa,
    data: *const u16,
    length: u64,
    result_index: *mut u64,
    operation: impl FnOnce(*const u16, usize) -> usize,
) -> SakuraStatus {
    if result_index.is_null() || !is_aligned(result_index.cast_const()) {
        return SakuraStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null, aligned, and caller-owned for this
    // call. Publishing the sentinel first makes every later failure fail closed.
    unsafe { result_index.write(length) };

    let Some(snapshot) = initialization.snapshot.get() else {
        return SakuraStatus::NotInitialized;
    };
    if !supports_isa(&snapshot.capabilities, required_isa) {
        return SakuraStatus::Unsupported;
    }
    let length_usize = match validate_span(data, length) {
        Ok(length) => length,
        Err(status) => return status,
    };
    let result = operation(data, length_usize);
    if result > length_usize {
        return SakuraStatus::InternalError;
    }
    // SAFETY: The output pointer remains valid for the duration of this call.
    unsafe { result_index.write(result as u64) };
    SakuraStatus::Ok
}

unsafe fn run_byte_scan(
    initialization: &InitializationCell,
    required_isa: RequiredIsa,
    data: *const u8,
    length: u64,
    result_index: *mut u64,
    operation: impl FnOnce(*const u8, usize) -> usize,
) -> SakuraStatus {
    if result_index.is_null() || !is_aligned(result_index.cast_const()) {
        return SakuraStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null, aligned, and caller-owned for
    // this call. Publishing the sentinel first makes every later failure fail
    // closed, including invalid input and unsupported CPU/OS state.
    unsafe { result_index.write(length) };

    let Some(snapshot) = initialization.snapshot.get() else {
        return SakuraStatus::NotInitialized;
    };
    if !supports_isa(&snapshot.capabilities, required_isa) {
        return SakuraStatus::Unsupported;
    }
    let length_usize = match validate_byte_span(data, length) {
        Ok(length) => length,
        Err(status) => return status,
    };
    let result = operation(data, length_usize);
    if result > length_usize {
        return SakuraStatus::InternalError;
    }
    // SAFETY: The output pointer remains valid for the duration of this call.
    unsafe { result_index.write(result as u64) };
    SakuraStatus::Ok
}

macro_rules! export_scan {
    ($name:ident, $required:expr, $kernel:path) => {
        #[unsafe(no_mangle)]
        #[doc = "Runs one initialized, capability-checked UTF-16 scan."]
        ///
        /// # Safety
        ///
        /// For nonzero `length`, `data` must reference that many initialized
        /// immutable UTF-16 code units. `result_index` must reference writable
        /// `u64` storage. No pointer is retained. The result unit is UTF-16 code
        /// units, and a not-found result equals `length`.
        pub unsafe extern "C" fn $name(
            data: *const u16,
            length: u64,
            result_index: *mut u64,
        ) -> SakuraStatus {
            catch_status(|| {
                let operation = |data, length| {
                    // SAFETY: `run_scan` invokes this closure only after the
                    // immutable C++ snapshot proves the required ISA and the
                    // raw span has passed validation.
                    unsafe { $kernel(data, length) }
                };
                // SAFETY: The export's caller contract is forwarded to the
                // common validator. The ISA kernel runs only after capability
                // and span validation.
                unsafe {
                    run_scan(
                        &INITIALIZATION,
                        $required,
                        data,
                        length,
                        result_index,
                        operation,
                    )
                }
            })
        }
    };
}

macro_rules! export_find_char {
    ($name:ident, $required:expr, $kernel:path) => {
        #[unsafe(no_mangle)]
        #[doc = "Runs one initialized, capability-checked UTF-16 character scan."]
        ///
        /// # Safety
        ///
        /// The pointer, lifetime, output, and result-unit contract is identical
        /// to the other V2 UTF-16 scan exports. `target` is one raw UTF-16 code
        /// unit, so unpaired surrogate values remain ordinary searchable data.
        pub unsafe extern "C" fn $name(
            data: *const u16,
            length: u64,
            target: u16,
            result_index: *mut u64,
        ) -> SakuraStatus {
            catch_status(|| {
                let operation = |data, length| {
                    // SAFETY: The common validator proves span and ISA before
                    // it invokes this closure.
                    unsafe { $kernel(data, length, target) }
                };
                // SAFETY: See the common scan export contract above.
                unsafe {
                    run_scan(
                        &INITIALIZATION,
                        $required,
                        data,
                        length,
                        result_index,
                        operation,
                    )
                }
            })
        }
    };
}

macro_rules! export_byte_candidate {
    ($name:ident, $required:expr, $kernel:path) => {
        #[unsafe(no_mangle)]
        #[doc = "Runs one initialized, capability-checked byte CR/LF candidate scan."]
        ///
        /// # Safety
        ///
        /// For nonzero `length`, `data` must reference that many initialized
        /// immutable bytes. `result_index` must reference writable `u64`
        /// storage. No pointer is retained. The result unit is bytes, and a
        /// not-found result equals `length`. This candidate does not add an
        /// operation-policy slot to ABI V1; the existing three UTF-16 policy
        /// entries remain fixed.
        pub unsafe extern "C" fn $name(
            data: *const u8,
            length: u64,
            result_index: *mut u64,
        ) -> SakuraStatus {
            catch_status(|| {
                let operation = |data, length| {
                    // SAFETY: `run_byte_scan` invokes this closure only after
                    // the immutable C++ snapshot proves the required ISA and
                    // the raw byte span has passed validation.
                    unsafe { $kernel(data, length) }
                };
                // SAFETY: The export's caller contract is forwarded to the
                // common byte validator. The ISA kernel runs only after
                // capability and span validation.
                unsafe {
                    run_byte_scan(
                        &INITIALIZATION,
                        $required,
                        data,
                        length,
                        result_index,
                        operation,
                    )
                }
            })
        }
    };
}

#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_cr_or_lf_avx128_v2,
    RequiredIsa::Avx128,
    native_simd::sakura_utf16_find_cr_or_lf_avx128_v1
);
#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_markdown_special_avx128_v2,
    RequiredIsa::Avx128,
    native_simd::sakura_utf16_find_markdown_special_avx128_v1
);
#[cfg(target_arch = "x86_64")]
export_find_char!(
    sakura_utf16_find_char_avx128_v2,
    RequiredIsa::Avx128,
    native_simd::sakura_utf16_find_char_avx128_v1
);

#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_cr_or_lf_avx2_v2,
    RequiredIsa::Avx2,
    native_simd::sakura_utf16_find_cr_or_lf_avx2_v1
);
#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_markdown_special_avx2_v2,
    RequiredIsa::Avx2,
    native_simd::sakura_utf16_find_markdown_special_avx2_v1
);
#[cfg(target_arch = "x86_64")]
export_find_char!(
    sakura_utf16_find_char_avx2_v2,
    RequiredIsa::Avx2,
    native_simd::sakura_utf16_find_char_avx2_v1
);

#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_cr_or_lf_avx512bw_v2,
    RequiredIsa::Avx512Bw,
    native_simd::sakura_utf16_find_cr_or_lf_avx512bw_v1
);
#[cfg(target_arch = "x86_64")]
export_scan!(
    sakura_utf16_find_markdown_special_avx512bw_v2,
    RequiredIsa::Avx512Bw,
    native_simd::sakura_utf16_find_markdown_special_avx512bw_v1
);
#[cfg(target_arch = "x86_64")]
export_find_char!(
    sakura_utf16_find_char_avx512bw_v2,
    RequiredIsa::Avx512Bw,
    native_simd::sakura_utf16_find_char_avx512bw_v1
);

#[cfg(target_arch = "x86_64")]
export_byte_candidate!(
    sakura_byte_find_cr_or_lf_avx128_candidate_v1,
    RequiredIsa::Avx128,
    native_simd::sakura_byte_find_cr_or_lf_avx128_candidate_v1
);
#[cfg(target_arch = "x86_64")]
export_byte_candidate!(
    sakura_byte_find_cr_or_lf_avx2_candidate_v1,
    RequiredIsa::Avx2,
    native_simd::sakura_byte_find_cr_or_lf_avx2_candidate_v1
);
#[cfg(target_arch = "x86_64")]
export_byte_candidate!(
    sakura_byte_find_cr_or_lf_avx512bw_candidate_v1,
    RequiredIsa::Avx512Bw,
    native_simd::sakura_byte_find_cr_or_lf_avx512bw_candidate_v1
);

#[cfg(test)]
mod tests {
    use super::*;

    fn capabilities(raw: u64, os: u64) -> SakuraCpuCapabilitiesV1 {
        SakuraCpuCapabilitiesV1 {
            struct_size: size_of::<SakuraCpuCapabilitiesV1>() as u32,
            abi_version: ABI_VERSION_V1,
            raw_feature_bits: raw,
            os_extended_state_bits: os,
            reserved: [0; 4],
        }
    }

    fn full_capabilities() -> SakuraCpuCapabilitiesV1 {
        capabilities(KNOWN_CPU_FEATURES, KNOWN_OS_STATES)
    }

    fn policy(
        operation_id: u32,
        implementation_id: u32,
        minimum_length: u64,
    ) -> SakuraOperationPolicyV1 {
        SakuraOperationPolicyV1 {
            struct_size: size_of::<SakuraOperationPolicyV1>() as u32,
            abi_version: ABI_VERSION_V1,
            operation_id,
            implementation_id,
            minimum_length,
            reserved: [0; 3],
        }
    }

    fn policies(implementation_id: u32) -> [SakuraOperationPolicyV1; POLICY_COUNT_V1] {
        [
            policy(OPERATION_FIND_CR_OR_LF_UTF16, implementation_id, 8),
            policy(OPERATION_FIND_MARKDOWN_SPECIAL_UTF16, implementation_id, 6),
            policy(OPERATION_FIND_CHAR_UTF16, implementation_id, 16),
        ]
    }

    fn snapshot(
        capabilities: SakuraCpuCapabilitiesV1,
        policies: [SakuraOperationPolicyV1; POLICY_COUNT_V1],
    ) -> Snapshot {
        Snapshot {
            capabilities,
            policies,
        }
    }

    #[test]
    fn abi_layout_is_fixed() {
        assert_eq!(56, size_of::<SakuraCpuCapabilitiesV1>());
        assert_eq!(48, size_of::<SakuraOperationPolicyV1>());
        assert_eq!(4, size_of::<SakuraStatus>());
    }

    #[test]
    fn null_size_version_reserved_and_policy_count_are_rejected() {
        let valid_capabilities = full_capabilities();
        let valid_policies = policies(IMPLEMENTATION_CPP_AVX512BW);
        // SAFETY: These calls intentionally exercise pointer validation without
        // dereferencing the rejected null arguments.
        unsafe {
            assert_eq!(
                Err(SakuraStatus::InvalidArgument),
                copy_snapshot(std::ptr::null(), valid_policies.as_ptr(), 3)
            );
            assert_eq!(
                Err(SakuraStatus::InvalidArgument),
                copy_snapshot(&valid_capabilities, std::ptr::null(), 3)
            );
            assert_eq!(
                Err(SakuraStatus::InvalidArgument),
                copy_snapshot(&valid_capabilities, valid_policies.as_ptr(), 2)
            );
        }

        let mut invalid = snapshot(valid_capabilities, valid_policies);
        invalid.capabilities.struct_size = 0;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
        invalid = snapshot(valid_capabilities, valid_policies);
        invalid.capabilities.abi_version += 1;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
        invalid = snapshot(valid_capabilities, valid_policies);
        invalid.capabilities.reserved[0] = 1;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
        invalid = snapshot(valid_capabilities, valid_policies);
        invalid.policies[0].struct_size = 0;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
        invalid = snapshot(valid_capabilities, valid_policies);
        invalid.policies[0].abi_version += 1;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
        invalid = snapshot(valid_capabilities, valid_policies);
        invalid.policies[0].reserved[0] = 1;
        assert_eq!(SakuraStatus::InvalidArgument, validate_snapshot(&invalid));
    }

    #[test]
    fn unsupported_capability_policy_pairs_are_rejected() {
        let avx_only = capabilities(CPU_FEATURE_AVX, OS_STATE_XMM | OS_STATE_YMM);
        assert_eq!(
            SakuraStatus::Unsupported,
            validate_snapshot(&snapshot(avx_only, policies(IMPLEMENTATION_RUST_AVX2)))
        );
        let no_zmm = capabilities(KNOWN_CPU_FEATURES, OS_STATE_XMM | OS_STATE_YMM);
        assert_eq!(
            SakuraStatus::Unsupported,
            validate_snapshot(&snapshot(no_zmm, policies(IMPLEMENTATION_RUST_AVX512BW),))
        );
        assert_eq!(
            SakuraStatus::Unsupported,
            validate_snapshot(&snapshot(full_capabilities(), policies(999)))
        );
    }

    #[test]
    fn duplicate_unknown_and_missing_operations_are_rejected() {
        let mut duplicate = policies(IMPLEMENTATION_CPP_AVX128);
        duplicate[1].operation_id = duplicate[0].operation_id;
        assert_eq!(
            SakuraStatus::InvalidArgument,
            validate_snapshot(&snapshot(full_capabilities(), duplicate))
        );
        let mut unknown = policies(IMPLEMENTATION_CPP_AVX128);
        unknown[1].operation_id = 99;
        assert_eq!(
            SakuraStatus::InvalidArgument,
            validate_snapshot(&snapshot(full_capabilities(), unknown))
        );
    }

    #[test]
    fn identical_initialization_is_idempotent_and_conflict_is_rejected() {
        let cell = InitializationCell::new();
        let original = snapshot(full_capabilities(), policies(IMPLEMENTATION_CPP_AVX512BW));
        assert_eq!(SakuraStatus::Ok, cell.initialize(original));
        assert_eq!(SakuraStatus::Ok, cell.initialize(original));
        let mut different = original;
        different.policies[0].minimum_length += 1;
        assert_eq!(
            SakuraStatus::ConflictingInitialization,
            cell.initialize(different)
        );
    }

    #[test]
    fn simd_before_initialization_fails_closed() {
        let cell = InitializationCell::new();
        let data = [b'x' as u16];
        let mut result = 99_u64;
        let status = catch_status(|| {
            // SAFETY: The local pointers are valid for this call.
            unsafe {
                run_scan(
                    &cell,
                    RequiredIsa::Avx128,
                    data.as_ptr(),
                    data.len() as u64,
                    &mut result,
                    |_, _| 0,
                )
            }
        });
        assert_eq!(SakuraStatus::NotInitialized, status);
        assert_eq!(data.len() as u64, result);
    }

    #[test]
    fn scan_capability_gate_and_invalid_span_fail_closed() {
        let cell = InitializationCell::new();
        let snapshot = snapshot(
            capabilities(CPU_FEATURE_AVX, OS_STATE_XMM | OS_STATE_YMM),
            policies(IMPLEMENTATION_CPP_AVX128),
        );
        assert_eq!(SakuraStatus::Ok, cell.initialize(snapshot));
        let data = [b'x' as u16];
        let mut result = 0_u64;
        // SAFETY: The valid local pointers satisfy the common scan contract.
        let unsupported = unsafe {
            run_scan(
                &cell,
                RequiredIsa::Avx2,
                data.as_ptr(),
                1,
                &mut result,
                |_, _| 0,
            )
        };
        assert_eq!(SakuraStatus::Unsupported, unsupported);
        assert_eq!(1, result);
        // SAFETY: The null input is rejected before dereference.
        let invalid = unsafe {
            run_scan(
                &cell,
                RequiredIsa::Avx128,
                std::ptr::null(),
                1,
                &mut result,
                |_, _| 0,
            )
        };
        assert_eq!(SakuraStatus::InvalidArgument, invalid);
        assert_eq!(1, result);
    }

    #[test]
    fn byte_scan_capability_gate_and_invalid_span_fail_closed() {
        let cell = InitializationCell::new();
        let snapshot = snapshot(
            capabilities(CPU_FEATURE_AVX, OS_STATE_XMM | OS_STATE_YMM),
            policies(IMPLEMENTATION_CPP_AVX128),
        );
        assert_eq!(SakuraStatus::Ok, cell.initialize(snapshot));

        let data = [b'x', b'\r'];
        let mut result = 0_u64;
        // SAFETY: The local pointers satisfy the byte scan contract.
        let found = unsafe {
            run_byte_scan(
                &cell,
                RequiredIsa::Avx128,
                data.as_ptr(),
                data.len() as u64,
                &mut result,
                |pointer, length| {
                    // SAFETY: The validator established a valid immutable
                    // byte span before invoking this test operation.
                    let input = slice::from_raw_parts(pointer, length);
                    if input[0] == b'\r' {
                        0
                    } else {
                        1
                    }
                },
            )
        };
        assert_eq!(SakuraStatus::Ok, found);
        assert_eq!(1, result);

        // The required ISA gate runs before input validation and still leaves
        // the output at its length sentinel.
        // SAFETY: The local pointers and operation satisfy the byte scan
        // contract; the test intentionally requests an unsupported ISA.
        let unsupported = unsafe {
            run_byte_scan(
                &cell,
                RequiredIsa::Avx2,
                data.as_ptr(),
                data.len() as u64,
                &mut result,
                |_, _| 0,
            )
        };
        assert_eq!(SakuraStatus::Unsupported, unsupported);
        assert_eq!(data.len() as u64, result);

        // Byte input accepts every alignment, but null non-empty spans and
        // overflowing addresses fail closed before dereference.
        // SAFETY: The null input is intentionally rejected before dereference.
        let invalid = unsafe {
            run_byte_scan(
                &cell,
                RequiredIsa::Avx128,
                std::ptr::null(),
                1,
                &mut result,
                |_, _| 0,
            )
        };
        assert_eq!(SakuraStatus::InvalidArgument, invalid);
        assert_eq!(1, result);

        // SAFETY: The overflowing pointer is intentionally rejected before
        // dereference.
        let overflowing = unsafe {
            run_byte_scan(
                &cell,
                RequiredIsa::Avx128,
                usize::MAX as *const u8,
                1,
                &mut result,
                |_, _| 0,
            )
        };
        assert_eq!(SakuraStatus::InvalidArgument, overflowing);
        assert_eq!(1, result);

        // SAFETY: A null output pointer is intentionally rejected before any
        // output write.
        assert_eq!(SakuraStatus::InvalidArgument, unsafe {
            run_byte_scan(
                &cell,
                RequiredIsa::Avx128,
                data.as_ptr(),
                data.len() as u64,
                std::ptr::null_mut(),
                |_, _| 0,
            )
        });
    }

    #[test]
    fn caught_panic_becomes_typed_internal_error() {
        assert_eq!(
            SakuraStatus::InternalError,
            catch_status(|| panic!("contained test panic"))
        );
    }
}
