//! Comparison-only encoded-EOL C ABI. C++ stays `legacy-production`.
//!
//! This module owns the ABI boundary and delegates scanning to the document
//! codec. No pointer is retained after a call. Measure and write are independent.
//!
//! Checks proceed from the output range and header to the request range,
//! header, format, flag, length and start. Write next checks the buffer range
//! and header, capacity arithmetic and records range, in that order. Both
//! calls then check the region range, writable overlaps and scanner start.
//! Then measure counts or write stages up to 1,048,576 markers. Write checks
//! caller capacity and publishes records first, followed by the output descriptor.
//! Caller-contract failures return InvalidArgument, size failures InvalidCapacity,
//! and caught panics, failed reservations or unrepresentable results InternalError.
//! Every failure leaves caller bytes unchanged (no poisoning, unlike the URI candidate).

use std::mem::size_of;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::{ptr, slice};

use sakura_document_codec::{scan_encoded_eol, EncodedEolFormat, EncodedEolScan, EolRecord};

use crate::ffi_pointer::{checked_address_range, checked_slice_range, overlaps, AddressRange};

pub const DOCUMENT_CODEC_ABI_VERSION_V1: u32 = 1;

/// Comparison-only memory bound of 16 MiB of staged records, not an adoption threshold.
pub const ENCODED_EOL_MAX_RECORDS_V1: u64 = 1_048_576;

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraDocumentCodecStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidCapacity = 2,
    InternalError = 12,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SakuraDocumentCodecEolRecordV1 {
    pub delimiter_byte_offset: u64,
    pub byte_width: u32,
    pub kind: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecEolRequestV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *const u8,
    pub length_bytes: u64,
    pub start_byte_offset: u64,
    pub format: u32,
    pub extended_eol: u32,
    pub reserved: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecEolBufferV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub records: *mut SakuraDocumentCodecEolRecordV1,
    pub capacity_records: u64,
    pub reserved: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecEolMeasureV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub record_count: u64,
    pub finish_byte_offset: u64,
    pub reserved: [u64; 2],
}

use SakuraDocumentCodecStatus::{InternalError, InvalidArgument, InvalidCapacity};

struct ValidatedRequest {
    data: *const u8,
    length: usize,
    finish_byte_offset: u64,
    start: usize,
    format: EncodedEolFormat,
    extended_eol: bool,
    descriptor_range: AddressRange,
}

struct ValidatedBuffer {
    records: *mut SakuraDocumentCodecEolRecordV1,
    capacity: usize,
    descriptor_range: AddressRange,
    records_range: AddressRange,
}

const FAULT_MEASURE_VALIDATED: u8 = 1;
const FAULT_MEASURE_COUNTED: u8 = 2;
const FAULT_WRITE_VALIDATED: u8 = 3;
const FAULT_WRITE_STAGED: u8 = 4;
#[cfg(test)]
const FAULT_STAGING_RESERVATION: u8 = 5;

#[cfg(test)]
use std::cell::Cell;

#[cfg(test)]
thread_local! {
    static FORCED_FAULT: Cell<u8> = const { Cell::new(0) };
}

#[cfg(test)]
#[inline(never)]
fn maybe_test_panic(point: u8) {
    if FORCED_FAULT.with(Cell::get) == point {
        panic!("forced encoded-EOL export panic");
    }
}

#[cfg(not(test))]
#[inline(always)]
fn maybe_test_panic(_point: u8) {}

#[cfg(test)]
fn reserve_record(
    staged: &mut Vec<SakuraDocumentCodecEolRecordV1>,
) -> Result<(), SakuraDocumentCodecStatus> {
    if FORCED_FAULT.with(Cell::get) == FAULT_STAGING_RESERVATION {
        return Err(InternalError);
    }
    staged.try_reserve(1).map_err(|_| InternalError)
}

#[cfg(not(test))]
fn reserve_record(
    staged: &mut Vec<SakuraDocumentCodecEolRecordV1>,
) -> Result<(), SakuraDocumentCodecStatus> {
    staged.try_reserve(1).map_err(|_| InternalError)
}

fn catch_status<F>(operation: F) -> SakuraDocumentCodecStatus
where
    F: FnOnce() -> Result<(), SakuraDocumentCodecStatus>,
{
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(())) => SakuraDocumentCodecStatus::Ok,
        Ok(Err(status)) => status,
        Err(_) => InternalError,
    }
}

fn read_descriptor<T: Copy>(
    pointer: *const T,
) -> Result<(T, AddressRange), SakuraDocumentCodecStatus> {
    let range = checked_address_range(pointer).ok_or(InvalidArgument)?;
    // SAFETY: The address range was checked. The ABI requires that descriptors
    // at accepted addresses are fully initialized and readable during the call.
    Ok((unsafe { pointer.read() }, range))
}

fn validate_header<T>(
    struct_size: u32,
    abi_version: u32,
    reserved: [u64; 2],
) -> Result<(), SakuraDocumentCodecStatus> {
    if u32::try_from(size_of::<T>()) != Ok(struct_size)
        || abi_version != DOCUMENT_CODEC_ABI_VERSION_V1
        || reserved != [0; 2]
    {
        return Err(InvalidArgument);
    }
    Ok(())
}

fn validate_output(
    output: *mut SakuraDocumentCodecEolMeasureV1,
) -> Result<AddressRange, SakuraDocumentCodecStatus> {
    let (descriptor, range) = read_descriptor(output.cast_const())?;
    validate_header::<SakuraDocumentCodecEolMeasureV1>(
        descriptor.struct_size,
        descriptor.abi_version,
        descriptor.reserved,
    )?;
    Ok(range)
}

fn validate_request(
    request: *const SakuraDocumentCodecEolRequestV1,
) -> Result<ValidatedRequest, SakuraDocumentCodecStatus> {
    let (request, descriptor_range) = read_descriptor(request)?;
    validate_header::<SakuraDocumentCodecEolRequestV1>(
        request.struct_size,
        request.abi_version,
        request.reserved,
    )?;
    let format = EncodedEolFormat::from_wire(request.format).ok_or(InvalidArgument)?;
    let extended_eol = match request.extended_eol {
        0 => false,
        1 => true,
        _ => return Err(InvalidArgument),
    };
    let length = usize::try_from(request.length_bytes).map_err(|_| InvalidArgument)?;
    if length > isize::MAX.cast_unsigned() {
        return Err(InvalidArgument);
    }
    let start = usize::try_from(request.start_byte_offset).map_err(|_| InvalidArgument)?;
    if start > length {
        return Err(InvalidArgument);
    }
    Ok(ValidatedRequest {
        data: request.data,
        length,
        finish_byte_offset: request.length_bytes,
        start,
        format,
        extended_eol,
        descriptor_range,
    })
}

fn validate_buffer(
    buffer: *const SakuraDocumentCodecEolBufferV1,
) -> Result<ValidatedBuffer, SakuraDocumentCodecStatus> {
    let (buffer, descriptor_range) = read_descriptor(buffer)?;
    validate_header::<SakuraDocumentCodecEolBufferV1>(
        buffer.struct_size,
        buffer.abi_version,
        buffer.reserved,
    )?;
    let capacity = usize::try_from(buffer.capacity_records).map_err(|_| InvalidCapacity)?;
    let byte_capacity = capacity
        .checked_mul(size_of::<SakuraDocumentCodecEolRecordV1>())
        .ok_or(InvalidCapacity)?;
    if byte_capacity > isize::MAX.cast_unsigned() {
        return Err(InvalidCapacity);
    }
    let (records, records_range) = if capacity == 0 {
        (ptr::null_mut(), AddressRange { start: 0, end: 0 })
    } else {
        let range =
            checked_slice_range(buffer.records.cast_const(), capacity).ok_or(InvalidArgument)?;
        (buffer.records, range)
    };
    Ok(ValidatedBuffer {
        records,
        capacity,
        descriptor_range,
        records_range,
    })
}

fn overlaps_any(range: AddressRange, others: &[AddressRange]) -> bool {
    others.iter().any(|&other| overlaps(range, other))
}

fn validated_scan(
    request: &ValidatedRequest,
) -> Result<EncodedEolScan<'_>, SakuraDocumentCodecStatus> {
    let region = if request.length == 0 {
        &[]
    } else {
        // SAFETY: The region address and length were validated, and the alias
        // check excluded all writes to it. The ABI requires readable initialized
        // bytes in one allocation, stable for the duration of the call.
        unsafe { slice::from_raw_parts(request.data, request.length) }
    };
    scan_encoded_eol(region, request.format, request.extended_eol, request.start)
        .map_err(|_| InvalidArgument)
}

fn measure_result(
    record_count: u64,
    finish_byte_offset: u64,
) -> Result<SakuraDocumentCodecEolMeasureV1, SakuraDocumentCodecStatus> {
    Ok(SakuraDocumentCodecEolMeasureV1 {
        struct_size: u32::try_from(size_of::<SakuraDocumentCodecEolMeasureV1>())
            .map_err(|_| InternalError)?,
        abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
        record_count,
        finish_byte_offset,
        reserved: [0; 2],
    })
}

fn wire_record(
    record: EolRecord,
) -> Result<SakuraDocumentCodecEolRecordV1, SakuraDocumentCodecStatus> {
    Ok(SakuraDocumentCodecEolRecordV1 {
        delimiter_byte_offset: u64::try_from(record.offset).map_err(|_| InternalError)?,
        byte_width: u32::try_from(record.width).map_err(|_| InternalError)?,
        kind: record.kind as u32,
    })
}

fn measure_impl(
    request: *const SakuraDocumentCodecEolRequestV1,
    output: *mut SakuraDocumentCodecEolMeasureV1,
) -> Result<(), SakuraDocumentCodecStatus> {
    let output_range = validate_output(output)?;
    let request = validate_request(request)?;
    let region_range = checked_slice_range(request.data, request.length).ok_or(InvalidArgument)?;
    if overlaps_any(output_range, &[request.descriptor_range, region_range]) {
        return Err(InvalidArgument);
    }
    let scan = validated_scan(&request)?;
    maybe_test_panic(FAULT_MEASURE_VALIDATED);

    let mut record_count = 0;
    for _ in scan {
        if record_count == ENCODED_EOL_MAX_RECORDS_V1 {
            return Err(InvalidCapacity);
        }
        record_count += 1;
    }
    let result = measure_result(record_count, request.finish_byte_offset)?;
    maybe_test_panic(FAULT_MEASURE_COUNTED);
    // SAFETY: The validated output slot is writable by contract and does not
    // overlap a read range. The result is complete and no fallible work follows.
    unsafe { output.write(result) };
    Ok(())
}

fn write_impl(
    request: *const SakuraDocumentCodecEolRequestV1,
    buffer: *const SakuraDocumentCodecEolBufferV1,
    output: *mut SakuraDocumentCodecEolMeasureV1,
) -> Result<(), SakuraDocumentCodecStatus> {
    let output_range = validate_output(output)?;
    let request = validate_request(request)?;
    let buffer = validate_buffer(buffer)?;
    let region_range = checked_slice_range(request.data, request.length).ok_or(InvalidArgument)?;
    let read_ranges = [
        request.descriptor_range,
        buffer.descriptor_range,
        region_range,
    ];
    if overlaps(output_range, buffer.records_range)
        || overlaps_any(output_range, &read_ranges)
        || overlaps_any(buffer.records_range, &read_ranges)
    {
        return Err(InvalidArgument);
    }
    let scan = validated_scan(&request)?;
    maybe_test_panic(FAULT_WRITE_VALIDATED);

    let ceiling = usize::try_from(ENCODED_EOL_MAX_RECORDS_V1).map_err(|_| InternalError)?;
    let mut staged = Vec::new();
    for record in scan {
        if staged.len() == ceiling {
            return Err(InvalidCapacity);
        }
        if staged.len() == staged.capacity() {
            reserve_record(&mut staged)?;
        }
        staged.push(wire_record(record)?);
    }
    if staged.len() > buffer.capacity {
        return Err(InvalidCapacity);
    }
    let record_count = u64::try_from(staged.len()).map_err(|_| InternalError)?;
    let result = measure_result(record_count, request.finish_byte_offset)?;
    maybe_test_panic(FAULT_WRITE_STAGED);
    if !staged.is_empty() {
        // SAFETY: Capacity covers the complete staging array and the checked
        // caller range is writable by contract. The owned Vec cannot overlap it.
        unsafe { ptr::copy_nonoverlapping(staged.as_ptr(), buffer.records, staged.len()) };
    }
    // SAFETY: The checked output slot is writable by contract, and all reads
    // and the non-overlapping records copy are complete.
    unsafe { output.write(result) };
    Ok(())
}

/// Counts encoded-EOL markers without allocating or retaining caller pointers.
///
/// The output's count and finish fields are ignored on entry. On success its
/// finish offset is the exclusive region end, including trailing content.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call.
/// The input region must be readable and stable in one allocation when nonempty.
/// The output descriptor must be writable. Success requires it to be disjoint
/// from both input ranges.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_scan_eol_measure_v1(
    request: *const SakuraDocumentCodecEolRequestV1,
    output: *mut SakuraDocumentCodecEolMeasureV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| measure_impl(request, output))
}

/// Scans independently of measure and publishes a complete record array on success.
///
/// Records have increasing offsets from the start of the region. Widths are
/// bytes, with CR LF counted as one two-unit marker. The unused suffix stays intact.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call.
/// A nonempty input region must be readable and stable in one allocation.
/// The output descriptor and any nonempty records array must be writable and
/// disjoint from each other and from every input range for a successful call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_scan_eol_write_v1(
    request: *const SakuraDocumentCodecEolRequestV1,
    buffer: *const SakuraDocumentCodecEolBufferV1,
    output: *mut SakuraDocumentCodecEolMeasureV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| write_impl(request, buffer, output))
}

#[cfg(test)]
mod tests {
    use super::*;
    use sakura_document_codec::EolKind;
    use std::mem::{align_of, offset_of};

    type Request = SakuraDocumentCodecEolRequestV1;
    type Buffer = SakuraDocumentCodecEolBufferV1;
    type Measure = SakuraDocumentCodecEolMeasureV1;
    type Record = SakuraDocumentCodecEolRecordV1;
    type Status = SakuraDocumentCodecStatus;

    const FORMATS: [EncodedEolFormat; 4] = [
        EncodedEolFormat::Utf16Le,
        EncodedEolFormat::Utf16Be,
        EncodedEolFormat::Utf32Le,
        EncodedEolFormat::Utf32Be,
    ];
    const SENTINEL_RECORD: Record = record(u64::MAX, u32::MAX, u32::MAX - 1);
    const SENTINEL_COUNT: u64 = 0xA5A5_A5A5_A5A5_A5A5;
    const SENTINEL_FINISH: u64 = 0x5A5A_5A5A_5A5A_5A5A;
    const PARTIAL_TAIL: [u8; 3] = [0x0D, 0x00, 0x00];

    macro_rules! impl_descriptor_bytes {
        ($descriptor:ty) => {
            impl $descriptor {
                fn bytes(&self) -> [u8; size_of::<Self>()] {
                    assert_eq!(size_of::<usize>(), 8);
                    // SAFETY: On x64 these repr(C) descriptors have no padding.
                    // The array read covers only the initialized descriptor bytes.
                    unsafe { ptr::from_ref(self).cast::<[u8; size_of::<Self>()]>().read() }
                }
            }
        };
    }

    impl_descriptor_bytes!(Request);
    impl_descriptor_bytes!(Buffer);
    impl_descriptor_bytes!(Measure);

    fn request(data: &[u8], format: EncodedEolFormat, extended_eol: bool, start: usize) -> Request {
        Request {
            struct_size: u32::try_from(size_of::<Request>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            data: data.as_ptr(),
            length_bytes: u64::try_from(data.len()).unwrap(),
            start_byte_offset: u64::try_from(start).unwrap(),
            format: format as u32,
            extended_eol: u32::from(extended_eol),
            reserved: [0; 2],
        }
    }

    fn buffer(records: &mut [Record]) -> Buffer {
        Buffer {
            struct_size: u32::try_from(size_of::<Buffer>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            records: records.as_mut_ptr(),
            capacity_records: u64::try_from(records.len()).unwrap(),
            reserved: [0; 2],
        }
    }

    fn output() -> Measure {
        Measure {
            struct_size: u32::try_from(size_of::<Measure>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            record_count: SENTINEL_COUNT,
            finish_byte_offset: SENTINEL_FINISH,
            reserved: [0; 2],
        }
    }

    const fn record(delimiter_byte_offset: u64, byte_width: u32, kind: u32) -> Record {
        Record {
            delimiter_byte_offset,
            byte_width,
            kind,
        }
    }

    fn measure(request: *const Request, output: *mut Measure) -> Status {
        // SAFETY: The test supplies live initialized storage for every accessed
        // descriptor and region. Bad addresses exercise validation before access.
        unsafe { sakura_document_codec_scan_eol_measure_v1(request, output) }
    }

    fn write(request: *const Request, buffer: *const Buffer, output: *mut Measure) -> Status {
        // SAFETY: The test supplies live storage for all accessed ranges.
        // Intentionally invalid addresses or overlaps must be rejected.
        unsafe { sakura_document_codec_scan_eol_write_v1(request, buffer, output) }
    }

    fn assert_summary(value: &Measure, count: usize, finish: u64) {
        assert_eq!(value.struct_size, 40);
        assert_eq!(value.abi_version, 1);
        assert_eq!(value.record_count, u64::try_from(count).unwrap());
        assert_eq!(value.finish_byte_offset, finish);
        assert_eq!(value.reserved, [0; 2]);
    }

    fn assert_measure_failure(request: &Request, mut result: Measure, expected: Status) {
        let request_before = request.bytes();
        let result_before = result.bytes();
        assert_eq!(measure(request, &raw mut result), expected);
        assert_eq!(result.bytes(), result_before);
        assert_eq!(request.bytes(), request_before);
    }

    fn assert_write_failure(
        request: &Request,
        records: &mut [Record],
        configure: impl FnOnce(&mut Buffer, &mut Measure),
        expected: Status,
    ) {
        let mut buffer = buffer(records);
        let mut result = output();
        configure(&mut buffer, &mut result);
        let request_before = request.bytes();
        let buffer_before = buffer.bytes();
        let result_before = result.bytes();
        assert_eq!(write(request, &raw const buffer, &raw mut result), expected);
        assert_eq!(result.bytes(), result_before);
        assert_eq!(buffer.bytes(), buffer_before);
        assert_eq!(request.bytes(), request_before);
        assert!(records.iter().all(|&value| value == SENTINEL_RECORD));
    }

    fn assert_both_fail(request: &Request, expected: Status) {
        assert_measure_failure(request, output(), expected);
        assert_write_failure(request, &mut [SENTINEL_RECORD; 4], |_, _| {}, expected);
    }

    fn assert_success(request: &Request, expected: &[Record], records: &mut [Record]) {
        let request_before = request.bytes();
        let mut measured = output();
        assert_eq!(measure(request, &raw mut measured), Status::Ok);
        assert_summary(&measured, expected.len(), request.length_bytes);
        assert_eq!(request.bytes(), request_before);

        let buffer = buffer(records);
        let buffer_before = buffer.bytes();
        let mut written = output();
        assert_eq!(
            write(request, &raw const buffer, &raw mut written),
            Status::Ok
        );
        assert_eq!(written.bytes(), measured.bytes());
        assert_eq!(request.bytes(), request_before);
        assert_eq!(buffer.bytes(), buffer_before);
        let (prefix, suffix) = records.split_at(expected.len());
        assert_eq!(prefix, expected);
        assert!(suffix.iter().all(|&value| value == SENTINEL_RECORD));
    }

    fn push_unit(region: &mut Vec<u8>, format: EncodedEolFormat, unit: u32) {
        match format {
            EncodedEolFormat::Utf16Le => {
                region.extend_from_slice(&u16::try_from(unit).unwrap().to_le_bytes());
            }
            EncodedEolFormat::Utf16Be => {
                region.extend_from_slice(&u16::try_from(unit).unwrap().to_be_bytes());
            }
            EncodedEolFormat::Utf32Le => region.extend_from_slice(&unit.to_le_bytes()),
            EncodedEolFormat::Utf32Be => region.extend_from_slice(&unit.to_be_bytes()),
        }
    }

    fn encode(format: EncodedEolFormat, units: &[u32]) -> Vec<u8> {
        let mut region = Vec::new();
        for &unit in units {
            push_unit(&mut region, format, unit);
        }
        region
    }

    fn scanner_records(scan: EncodedEolScan<'_>) -> Vec<Record> {
        scan.map(|marker| {
            record(
                u64::try_from(marker.offset).unwrap(),
                u32::try_from(marker.width).unwrap(),
                marker.kind as u32,
            )
        })
        .collect()
    }

    #[test]
    fn abi_layout_matches_the_native_header() {
        macro_rules! layout {
            ($ty:ty, $size:literal, $align:literal, $($field:ident : $offset:literal),+ $(,)?) => {
                assert_eq!(size_of::<$ty>(), $size);
                assert_eq!(align_of::<$ty>(), $align);
                $(assert_eq!(offset_of!($ty, $field), $offset);)+
            };
        }

        layout!(Record, 16, 8, delimiter_byte_offset: 0, byte_width: 8, kind: 12);
        layout!(
            Request, 56, 8, struct_size: 0, abi_version: 4, data: 8,
            length_bytes: 16, start_byte_offset: 24, format: 32, extended_eol: 36,
            reserved: 40,
        );
        layout!(
            Buffer, 40, 8, struct_size: 0, abi_version: 4, records: 8,
            capacity_records: 16, reserved: 24,
        );
        layout!(
            Measure, 40, 8, struct_size: 0, abi_version: 4, record_count: 8,
            finish_byte_offset: 16, reserved: 24,
        );
        assert_eq!(Status::Ok as u32, 0);
        assert_eq!(InvalidArgument as u32, 1);
        assert_eq!(InvalidCapacity as u32, 2);
        assert_eq!(InternalError as u32, 12);
        assert_eq!(DOCUMENT_CODEC_ABI_VERSION_V1, 1);
        assert_eq!(ENCODED_EOL_MAX_RECORDS_V1, 1_048_576);
        assert_eq!(FORMATS.map(|format| format as u32), [2, 3, 4, 5]);
        assert_eq!(
            [
                EolKind::CrLf,
                EolKind::Lf,
                EolKind::Cr,
                EolKind::Nel,
                EolKind::Ls,
                EolKind::Ps,
            ]
            .map(|kind| kind as u32),
            [1, 2, 3, 4, 5, 6],
        );
    }

    #[test]
    fn mixed_regions_have_literal_results_for_every_format_and_flag() {
        const UTF16: [Record; 6] = [
            record(2, 2, 3),
            record(6, 2, 2),
            record(8, 4, 1),
            record(12, 2, 4),
            record(14, 2, 5),
            record(16, 2, 6),
        ];
        const UTF32: [Record; 6] = [
            record(4, 4, 3),
            record(12, 4, 2),
            record(16, 8, 1),
            record(24, 4, 4),
            record(28, 4, 5),
            record(32, 4, 6),
        ];

        for format in FORMATS {
            let mut region = encode(
                format,
                &[
                    0x41, 0x0D, 0x42, 0x0A, 0x0D, 0x0A, 0x85, 0x2028, 0x2029, 0x43,
                ],
            );
            region.push(0x0A);
            let expected = match format {
                EncodedEolFormat::Utf16Le | EncodedEolFormat::Utf16Be => &UTF16,
                EncodedEolFormat::Utf32Le | EncodedEolFormat::Utf32Be => &UTF32,
            };
            for extended in [false, true] {
                let count = if extended { 6 } else { 3 };
                assert_success(
                    &request(&region, format, extended, 0),
                    &expected[..count],
                    &mut [SENTINEL_RECORD; 7],
                );
            }
        }
    }

    #[test]
    fn byte_start_obeys_the_scanner_boundaries() {
        for format in FORMATS {
            let unit = format.unit_bytes();
            let mut region = encode(format, &[0x0D, 0x0A]);
            let pair_length = region.len();
            let lf = record(
                u64::try_from(unit).unwrap(),
                u32::try_from(unit).unwrap(),
                2,
            );
            assert_success(
                &request(&region, format, false, unit),
                &[lf],
                &mut [SENTINEL_RECORD; 2],
            );

            for tail in 0..unit {
                region.truncate(pair_length);
                region.extend_from_slice(&PARTIAL_TAIL[..tail]);
                assert_success(&request(&region, format, false, region.len()), &[], &mut []);
                assert_both_fail(
                    &request(&region, format, false, region.len() + 1),
                    InvalidArgument,
                );
                assert_both_fail(&request(&region, format, false, 1), InvalidArgument);
            }
        }
    }

    #[test]
    fn zero_length_region_ignores_its_address() {
        let mut request = request(&[], EncodedEolFormat::Utf16Le, false, 0);
        for data in [ptr::null(), ptr::without_provenance::<u8>(usize::MAX)] {
            request.data = data;
            assert_success(&request, &[], &mut []);
        }
        request.data = ptr::null();
        request.length_bytes = 1;
        assert_both_fail(&request, InvalidArgument);
    }

    fn bad_headers(size: u32) -> [(u32, u32, [u64; 2]); 4] {
        [
            (size - 1, 1, [0, 0]),
            (size, 2, [0, 0]),
            (size, 1, [1, 0]),
            (size, 1, [0, 1]),
        ]
    }

    #[test]
    fn malformed_headers_and_request_fields_preserve_all_bytes() {
        let valid = request(&[0x0A, 0x00], EncodedEolFormat::Utf16Le, false, 0);
        for header in bad_headers(56) {
            let mut request = valid;
            (request.struct_size, request.abi_version, request.reserved) = header;
            assert_both_fail(&request, InvalidArgument);
        }
        for header in bad_headers(40) {
            let mut measured = output();
            (
                measured.struct_size,
                measured.abi_version,
                measured.reserved,
            ) = header;
            assert_measure_failure(&valid, measured, InvalidArgument);
            assert_write_failure(
                &valid,
                &mut [SENTINEL_RECORD; 4],
                |_, result| {
                    (result.struct_size, result.abi_version, result.reserved) = header;
                },
                InvalidArgument,
            );
            assert_write_failure(
                &valid,
                &mut [SENTINEL_RECORD; 4],
                |buffer, _| {
                    (buffer.struct_size, buffer.abi_version, buffer.reserved) = header;
                },
                InvalidArgument,
            );
        }
        for format in [0, 1, 6, u32::MAX] {
            assert_both_fail(&Request { format, ..valid }, InvalidArgument);
        }
        for extended_eol in [2, u32::MAX] {
            assert_both_fail(
                &Request {
                    extended_eol,
                    ..valid
                },
                InvalidArgument,
            );
        }
        for length_bytes in [isize::MAX as u64 + 1, u64::MAX] {
            assert_both_fail(
                &Request {
                    length_bytes,
                    ..valid
                },
                InvalidArgument,
            );
        }
        assert_both_fail(
            &Request {
                start_byte_offset: u64::MAX,
                ..valid
            },
            InvalidArgument,
        );
    }

    #[test]
    fn bad_pointers_are_rejected_before_any_access() {
        #[derive(Clone, Copy)]
        enum Target {
            Request,
            Buffer,
            Output,
            Records,
        }
        let request = request(&[0x0A, 0x00], EncodedEolFormat::Utf16Le, false, 0);
        let request_before = request.bytes();
        let mut aligned = [0xA5A5_A5A5_A5A5_A5A5_u64; 8];
        let aligned_before = aligned;
        let unaligned = aligned.as_mut_ptr().cast::<u8>().wrapping_add(1);
        let wrapping = ptr::without_provenance_mut::<u8>(usize::MAX - 7);

        for bad in [ptr::null_mut(), unaligned, wrapping] {
            for target in [Target::Request, Target::Output] {
                let mut result = output();
                let before = result.bytes();
                let (req, out) = match target {
                    Target::Request => (bad.cast_const().cast(), &raw mut result),
                    Target::Output => (&raw const request, bad.cast()),
                    _ => unreachable!(),
                };
                assert_eq!(measure(req, out), InvalidArgument);
                assert_eq!(result.bytes(), before);
                assert_eq!(request.bytes(), request_before);
            }

            for target in [
                Target::Request,
                Target::Buffer,
                Target::Output,
                Target::Records,
            ] {
                let mut records = [SENTINEL_RECORD; 2];
                let mut buffer = buffer(&mut records);
                let mut result = output();
                if matches!(target, Target::Records) {
                    buffer.records = bad.cast();
                }
                let buffer_before = buffer.bytes();
                let result_before = result.bytes();
                let req = match target {
                    Target::Request => bad.cast_const().cast(),
                    _ => &raw const request,
                };
                let buf = match target {
                    Target::Buffer => bad.cast_const().cast(),
                    _ => &raw const buffer,
                };
                let out = match target {
                    Target::Output => bad.cast(),
                    _ => &raw mut result,
                };
                assert_eq!(write(req, buf, out), InvalidArgument);
                assert_eq!(result.bytes(), result_before);
                assert_eq!(buffer.bytes(), buffer_before);
                assert_eq!(request.bytes(), request_before);
                assert_eq!(records, [SENTINEL_RECORD; 2]);
            }
        }
        assert_eq!(aligned, aligned_before);
    }

    fn word_pair(first: u32, second: u32) -> u64 {
        let mut bytes = [0; 8];
        bytes[..4].copy_from_slice(&first.to_ne_bytes());
        bytes[4..].copy_from_slice(&second.to_ne_bytes());
        u64::from_ne_bytes(bytes)
    }

    #[test]
    fn output_cannot_overlap_the_region_or_either_input_descriptor() {
        let format = EncodedEolFormat::Utf16Le;
        let mut result = output();
        let result_before = result.bytes();
        let result_ptr = &raw mut result;
        let mut aliased_request = request(&[], format, false, 0);
        aliased_request.data = result_ptr.cast_const().cast();
        aliased_request.length_bytes = 40;
        let aliased_request_before = aliased_request.bytes();
        assert_eq!(measure(&aliased_request, result_ptr), InvalidArgument);
        assert_eq!(result.bytes(), result_before);
        assert_eq!(aliased_request.bytes(), aliased_request_before);

        let result_ptr = &raw mut result;
        aliased_request.data = result_ptr.cast_const().cast();
        let aliased_request_before = aliased_request.bytes();
        let mut records = [SENTINEL_RECORD; 20];
        let buffer = buffer(&mut records);
        let buffer_before = buffer.bytes();
        assert_eq!(
            write(&aliased_request, &buffer, result_ptr),
            InvalidArgument
        );
        assert_eq!(result.bytes(), result_before);
        assert_eq!(aliased_request.bytes(), aliased_request_before);
        assert_eq!(buffer.bytes(), buffer_before);
        assert_eq!(records, [SENTINEL_RECORD; 20]);

        // Place the request header in the ignored finish field of the output.
        // Its null data pointer and zero length keep both overlapping headers valid.
        let mut output_and_request = [
            word_pair(40, 1),
            SENTINEL_COUNT,
            word_pair(56, 1),
            0,
            0,
            0,
            word_pair(2, 0),
            0,
            0,
        ];
        let before = output_and_request;
        let words = output_and_request.as_mut_ptr();
        assert_eq!(
            measure(words.wrapping_add(2).cast_const().cast(), words.cast()),
            InvalidArgument,
        );
        assert_eq!(output_and_request, before);
        let words = output_and_request.as_mut_ptr();
        assert_eq!(
            write(
                words.wrapping_add(2).cast_const().cast(),
                &buffer,
                words.cast()
            ),
            InvalidArgument,
        );
        assert_eq!(output_and_request, before);
        assert_eq!(buffer.bytes(), buffer_before);
        assert_eq!(records, [SENTINEL_RECORD; 20]);

        // The buffer header can likewise share the ignored finish field.
        let mut output_and_buffer = [
            word_pair(40, 1),
            SENTINEL_COUNT,
            word_pair(40, 1),
            0,
            0,
            0,
            0,
        ];
        let before = output_and_buffer;
        let words = output_and_buffer.as_mut_ptr();
        let request = request(&[], format, false, 0);
        let request_before = request.bytes();
        assert_eq!(
            write(
                &request,
                words.wrapping_add(2).cast_const().cast(),
                words.cast()
            ),
            InvalidArgument,
        );
        assert_eq!(output_and_buffer, before);
        assert_eq!(request.bytes(), request_before);
    }

    #[test]
    fn records_cannot_overlap_any_input_or_output_range() {
        let format = EncodedEolFormat::Utf16Le;
        let mut records = [SENTINEL_RECORD; 2];
        let mut buffer = buffer(&mut records);
        let mut request = request(&[], format, false, 0);
        let mut result = output();
        let result_before = result.bytes();

        request.data = buffer.records.cast_const().cast();
        request.length_bytes = 2;
        let request_before = request.bytes();
        let buffer_before = buffer.bytes();
        assert_eq!(write(&request, &buffer, &raw mut result), InvalidArgument);
        assert_eq!(result.bytes(), result_before);
        assert_eq!(request.bytes(), request_before);
        assert_eq!(buffer.bytes(), buffer_before);
        assert_eq!(records, [SENTINEL_RECORD; 2]);

        request.data = ptr::null();
        request.length_bytes = 0;
        let request_before = request.bytes();
        let result_ptr = &raw mut result;
        buffer.records = result_ptr.cast();
        buffer.capacity_records = 1;
        let buffer_before = buffer.bytes();
        assert_eq!(write(&request, &buffer, result_ptr), InvalidArgument);
        assert_eq!(result.bytes(), result_before);
        assert_eq!(request.bytes(), request_before);
        assert_eq!(buffer.bytes(), buffer_before);

        let request_ptr = &raw mut request;
        buffer.records = request_ptr.cast();
        let buffer_before = buffer.bytes();
        assert_eq!(
            write(request_ptr, &buffer, &raw mut result),
            InvalidArgument
        );
        assert_eq!(result.bytes(), result_before);
        assert_eq!(request.bytes(), request_before);
        assert_eq!(buffer.bytes(), buffer_before);

        let buffer_ptr = &raw mut buffer;
        buffer.records = buffer_ptr.cast();
        let buffer_before = buffer.bytes();
        assert_eq!(
            write(&request, buffer_ptr, &raw mut result),
            InvalidArgument
        );
        assert_eq!(result.bytes(), result_before);
        assert_eq!(request.bytes(), request_before);
        assert_eq!(buffer.bytes(), buffer_before);
        assert_eq!(records, [SENTINEL_RECORD; 2]);
    }

    #[test]
    fn read_only_region_may_overlap_its_request_descriptor() {
        let format = EncodedEolFormat::Utf16Le;
        let mut request = request(&[], format, false, 0);
        let request_ptr = &raw mut request;
        request.data = request_ptr.cast_const().cast();
        request.length_bytes = u64::try_from(size_of::<Request>()).unwrap();
        let before = request.bytes();
        let expected = scan_encoded_eol(&before, format, false, 0).unwrap().count();
        let mut result = output();
        assert_eq!(measure(request_ptr, &raw mut result), Status::Ok);
        assert_summary(&result, expected, request.length_bytes);
        assert_eq!(request.bytes(), before);
    }

    #[test]
    fn capacity_errors_are_atomic_and_suffix_slots_stay_untouched() {
        let format = EncodedEolFormat::Utf16Le;
        let empty = request(&[], format, false, 0);
        for unused in [ptr::null_mut(), ptr::without_provenance_mut(usize::MAX)] {
            let mut buffer = buffer(&mut []);
            buffer.records = unused;
            let buffer_before = buffer.bytes();
            let request_before = empty.bytes();
            let mut result = output();
            assert_eq!(write(&empty, &buffer, &raw mut result), Status::Ok);
            assert_summary(&result, 0, 0);
            assert_eq!(buffer.bytes(), buffer_before);
            assert_eq!(empty.bytes(), request_before);
        }

        let region = [0x0A, 0x00, 0x0A, 0x00];
        let request = request(&region, format, false, 0);
        assert_write_failure(&request, &mut [], |_, _| {}, InvalidCapacity);
        assert_write_failure(
            &request,
            &mut [SENTINEL_RECORD; 1],
            |_, _| {},
            InvalidCapacity,
        );
        let expected = [record(0, 2, 2), record(2, 2, 2)];
        assert_success(&request, &expected, &mut [SENTINEL_RECORD; 2]);
        assert_success(&request, &expected, &mut [SENTINEL_RECORD; 4]);

        // The byte count overflows (u64::MAX), exceeds isize::MAX (isize::MAX / 16 + 1), or wraps around
        // to a small value ((1 << 60) + 1 sixteen-byte records make 16 bytes).
        for capacity in [u64::MAX, isize::MAX as u64 / 16 + 1, (1 << 60) + 1] {
            assert_write_failure(
                &request,
                &mut [SENTINEL_RECORD; 2],
                |buffer, _| {
                    buffer.capacity_records = capacity;
                    buffer.records = ptr::null_mut();
                },
                InvalidCapacity,
            );
        }
    }

    #[test]
    fn the_first_invalid_descriptor_or_range_decides_the_status() {
        let valid = request(&[0x0A, 0x00], EncodedEolFormat::Utf16Le, false, 0);
        assert_write_failure(
            &valid,
            &mut [SENTINEL_RECORD; 1],
            |buffer, result| {
                buffer.capacity_records = u64::MAX;
                result.abi_version = 0;
            },
            InvalidArgument,
        );
        assert_write_failure(
            &Request {
                format: u32::MAX,
                ..valid
            },
            &mut [SENTINEL_RECORD; 1],
            |buffer, _| buffer.capacity_records = u64::MAX,
            InvalidArgument,
        );
        assert_write_failure(
            &Request {
                data: ptr::null(),
                ..valid
            },
            &mut [SENTINEL_RECORD; 1],
            |buffer, _| buffer.capacity_records = u64::MAX,
            InvalidCapacity,
        );
        assert_write_failure(
            &Request {
                start_byte_offset: 1,
                ..valid
            },
            &mut [SENTINEL_RECORD; 1],
            |buffer, _| buffer.capacity_records = u64::MAX,
            InvalidCapacity,
        );
        // The request's own length and start bounds decide before the buffer is examined.
        assert_write_failure(
            &Request {
                length_bytes: isize::MAX as u64 + 1,
                ..valid
            },
            &mut [SENTINEL_RECORD; 1],
            |buffer, _| buffer.capacity_records = u64::MAX,
            InvalidArgument,
        );
        assert_write_failure(
            &Request {
                start_byte_offset: 3,
                ..valid
            },
            &mut [SENTINEL_RECORD; 1],
            |buffer, _| buffer.capacity_records = u64::MAX,
            InvalidArgument,
        );
    }

    #[test]
    fn record_ceiling_is_inclusive_and_overflow_leaves_no_prefix() {
        let ceiling = usize::try_from(ENCODED_EOL_MAX_RECORDS_V1).unwrap();
        let region = [0x0A, 0x00].repeat(ceiling + 1);
        let request_at_limit = request(&region[..2 * ceiling], EncodedEolFormat::Utf16Le, false, 0);
        let mut measured = output();
        assert_eq!(measure(&request_at_limit, &raw mut measured), Status::Ok);
        assert_summary(&measured, ceiling, request_at_limit.length_bytes);

        let mut records = vec![SENTINEL_RECORD; ceiling + 1];
        let exact_buffer = buffer(&mut records[..ceiling]);
        let exact_buffer_before = exact_buffer.bytes();
        let request_before = request_at_limit.bytes();
        let mut written = output();
        assert_eq!(
            write(&request_at_limit, &exact_buffer, &raw mut written),
            Status::Ok
        );
        assert_eq!(written.bytes(), measured.bytes());
        assert_eq!(exact_buffer.bytes(), exact_buffer_before);
        assert_eq!(request_at_limit.bytes(), request_before);
        for index in [0, ceiling / 2, ceiling - 1] {
            assert_eq!(
                records[index],
                record(u64::try_from(index * 2).unwrap(), 2, 2)
            );
        }
        assert_eq!(records[ceiling], SENTINEL_RECORD);

        records.fill(SENTINEL_RECORD);
        let too_many = request(&region, EncodedEolFormat::Utf16Le, false, 0);
        assert_measure_failure(&too_many, output(), InvalidCapacity);
        assert_write_failure(&too_many, &mut records, |_, _| {}, InvalidCapacity);
    }

    struct FaultGuard;

    impl FaultGuard {
        fn inject(point: u8) -> Self {
            FORCED_FAULT.set(point);
            Self
        }
    }

    impl Drop for FaultGuard {
        fn drop(&mut self) {
            FORCED_FAULT.set(0);
        }
    }

    #[test]
    fn every_fault_leaves_caller_bytes_unchanged_and_allows_retry() {
        let region = [0x0A, 0x00];
        let request = request(&region, EncodedEolFormat::Utf16Le, false, 0);
        let expected = [record(0, 2, 2)];
        for point in [FAULT_MEASURE_VALIDATED, FAULT_MEASURE_COUNTED] {
            {
                let _fault = FaultGuard::inject(point);
                assert_measure_failure(&request, output(), InternalError);
            }
            assert_success(&request, &expected, &mut [SENTINEL_RECORD; 2]);
        }
        for point in [
            FAULT_WRITE_VALIDATED,
            FAULT_WRITE_STAGED,
            FAULT_STAGING_RESERVATION,
        ] {
            {
                let _fault = FaultGuard::inject(point);
                assert_write_failure(
                    &request,
                    &mut [SENTINEL_RECORD; 2],
                    |_, _| {},
                    InternalError,
                );
            }
            assert_success(&request, &expected, &mut [SENTINEL_RECORD; 2]);
        }
    }

    #[test]
    fn export_source_contract_contains_panic_containment_for_each_export() {
        let source = include_str!("encoded_eol.rs");
        let (production, _) = source.split_once("mod tests {").unwrap();
        assert_eq!(production.matches("pub unsafe extern \"C\" fn ").count(), 2);
        assert_eq!(production.matches("catch_status(").count(), 2);
        assert_eq!(production.matches("catch_unwind(").count(), 1);
        assert!(!production.contains("unwrap("));
        assert!(!production.contains("expect("));
    }

    #[test]
    fn abi_matches_the_scanner_on_every_small_region() {
        let utf16 = [
            0x000D, 0x000A, 0x0085, 0x2028, 0x2029, 0x0041, 0x0A00, 0xD800,
        ];
        let utf32 = [
            0x0D,
            0x0A,
            0x85,
            0x2028,
            0x2029,
            0x41,
            0x0001_000A,
            0x0A00_0000,
        ];
        let mut compared = 0;
        let mut nonempty_scans = 0;
        let mut rejected_scans = 0;
        for format in FORMATS {
            let unit = format.unit_bytes();
            let (alphabet, comparisons_per_flag) = match format {
                EncodedEolFormat::Utf16Le | EncodedEolFormat::Utf16Be => (&utf16, 9_613),
                EncodedEolFormat::Utf32Le | EncodedEolFormat::Utf32Be => (&utf32, 34_942),
            };
            let radix = alphabet.len();
            for extended in [false, true] {
                let before = compared;
                for unit_count in 0..=3_u32 {
                    for mut code in 0..radix.pow(unit_count) {
                        let mut region = Vec::new();
                        for _ in 0..unit_count {
                            push_unit(&mut region, format, alphabet[code % radix]);
                            code /= radix;
                        }
                        let base_length = region.len();
                        for tail in 0..unit {
                            region.truncate(base_length);
                            region.extend_from_slice(&PARTIAL_TAIL[..tail]);
                            for start in 0..=region.len() + 1 {
                                let request = request(&region, format, extended, start);
                                match scan_encoded_eol(&region, format, extended, start) {
                                    Ok(scan) => {
                                        let expected = scanner_records(scan);
                                        nonempty_scans += usize::from(!expected.is_empty());
                                        assert_success(
                                            &request,
                                            &expected,
                                            &mut [SENTINEL_RECORD; 4],
                                        );
                                    }
                                    Err(_) => {
                                        rejected_scans += 1;
                                        assert_both_fail(&request, InvalidArgument);
                                    }
                                }
                                compared += 1;
                            }
                        }
                    }
                }
                assert_eq!(compared - before, comparisons_per_flag);
            }
        }
        assert_eq!(compared, 178_220);
        assert!(nonempty_scans > 0);
        assert!(rejected_scans > 0);
    }
}
