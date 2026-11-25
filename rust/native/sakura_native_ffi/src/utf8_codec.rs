//! Comparison-only UTF-8/CESU-8 conversion C ABI (#443). C++ stays `legacy-production`.
//!
//! This module owns the ABI boundary of both directions and delegates the conversion to the document codec. Decode
//! reads bytes and produces UTF-16 units, encode reads UTF-16 units and produces bytes. Both run one checked
//! skeleton, and `Conversion` is the only part in which they differ. No pointer is retained after a call. Measure
//! and write are independent calls.
//!
//! Checks proceed from the output range and header to the request range, header, variant and length. Write next
//! checks the buffer range and header, the capacity arithmetic and the array range, in that order. Both calls then
//! check the input range, the writable overlaps and the input bound. Measure converts and reports. Write measures,
//! refuses a capacity below the measured length, stages the complete output in memory reserved for exactly that
//! length, and publishes the output first and the result descriptor last. Caller-contract failures return
//! InvalidArgument, size failures InvalidCapacity, and caught panics or a failed reservation InternalError. Every
//! failure leaves caller bytes unchanged.

use std::mem::size_of;
use std::{ptr, slice};

use sakura_document_codec::{
    utf8_decode_measure, utf8_decode_write, utf8_encode_measure, utf8_encode_write, OutputTooSmall,
    Utf8Variant,
};

use crate::document_codec_abi::{
    catch_status, read_descriptor, validate_header, SakuraDocumentCodecStatus,
    DOCUMENT_CODEC_ABI_VERSION_V1,
};
use crate::ffi_pointer::{checked_slice_range, overlaps, overlaps_any, AddressRange};

/// Longest input (bytes to decode, UTF-16 units to encode) one call accepts: the `int` length domain of the C++
/// converters, not an adoption threshold.
pub const UTF8_CODEC_MAX_INPUT_V1: u64 = 2_147_483_647;

/// One conversion request. `Unit` is `u8` for decode, where `length` counts bytes, and `u16` for encode, where
/// `length` counts UTF-16 units. `variant` uses the `ECodeType` numbers (4 UTF-8, 7 CESU-8). A null `data` is valid
/// only with `length` 0. The C header names the same fields `length_bytes` and `length_units`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecUtf8RequestV1<Unit> {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *const Unit,
    pub length: u64,
    pub variant: u32,
    pub reserved0: u32,
    pub reserved: [u64; 2],
}

/// Caller-owned output array of `capacity` elements: UTF-16 units for decode, bytes for encode. A null `data` is
/// valid only when `capacity` is 0. The candidate never allocates for the caller. A write with too little capacity
/// writes nothing. The C header names the pointer `units` or `bytes` and the count `capacity_units` or
/// `capacity_bytes`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecUtf8BufferV1<Unit> {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *mut Unit,
    pub capacity: u64,
    pub reserved: [u64; 2],
}

/// Result of both calls. `output_length` counts output elements. `lossy` is 0 or 1: it is always 0 for decode, and
/// 1 for an encode that replaced an unpaired surrogate. There is no loss count or position.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraDocumentCodecUtf8ResultV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub output_length: u64,
    pub lossy: u32,
    pub reserved0: u32,
    pub reserved: [u64; 1],
}

pub type SakuraDocumentCodecUtf8DecodeRequestV1 = SakuraDocumentCodecUtf8RequestV1<u8>;
pub type SakuraDocumentCodecUtf8EncodeRequestV1 = SakuraDocumentCodecUtf8RequestV1<u16>;
pub type SakuraDocumentCodecUtf8DecodeBufferV1 = SakuraDocumentCodecUtf8BufferV1<u16>;
pub type SakuraDocumentCodecUtf8EncodeBufferV1 = SakuraDocumentCodecUtf8BufferV1<u8>;

use SakuraDocumentCodecStatus::{InternalError, InvalidArgument, InvalidCapacity};

/// What a conversion produced: the output length in output elements and whether it lost input.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Converted {
    length: usize,
    lossy: bool,
}

/// The part in which the two directions differ: the element types and the two document-codec calls.
trait Conversion {
    /// Element of the request: a byte to decode, a UTF-16 unit to encode.
    type Input: Copy;
    /// Element of the output: a UTF-16 unit when decoding, a byte when encoding.
    type Output: Copy + Default;

    fn measure(input: &[Self::Input], variant: Utf8Variant) -> Converted;

    /// Writes the whole output into `output`, which the caller sized from `measure`.
    fn write(
        input: &[Self::Input],
        variant: Utf8Variant,
        output: &mut [Self::Output],
    ) -> Result<Converted, OutputTooSmall>;
}

struct Decode;
struct Encode;

impl Conversion for Decode {
    type Input = u8;
    type Output = u16;

    fn measure(input: &[u8], variant: Utf8Variant) -> Converted {
        Converted {
            length: utf8_decode_measure(input, variant),
            lossy: false,
        }
    }

    fn write(
        input: &[u8],
        variant: Utf8Variant,
        output: &mut [u16],
    ) -> Result<Converted, OutputTooSmall> {
        utf8_decode_write(input, variant, output).map(|length| Converted {
            length,
            lossy: false,
        })
    }
}

impl Conversion for Encode {
    type Input = u16;
    type Output = u8;

    fn measure(input: &[u16], variant: Utf8Variant) -> Converted {
        let encoded = utf8_encode_measure(input, variant);
        Converted {
            length: encoded.length,
            lossy: encoded.lossy,
        }
    }

    fn write(
        input: &[u16],
        variant: Utf8Variant,
        output: &mut [u8],
    ) -> Result<Converted, OutputTooSmall> {
        utf8_encode_write(input, variant, output).map(|encoded| Converted {
            length: encoded.length,
            lossy: encoded.lossy,
        })
    }
}

struct ValidatedRequest<Unit> {
    data: *const Unit,
    length: usize,
    variant: Utf8Variant,
    descriptor_range: AddressRange,
}

struct ValidatedBuffer<Unit> {
    data: *mut Unit,
    capacity: usize,
    descriptor_range: AddressRange,
    range: AddressRange,
}

const FAULT_MEASURE_VALIDATED: u8 = 1;
const FAULT_MEASURE_CONVERTED: u8 = 2;
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
        panic!("forced UTF-8 codec export panic");
    }
}

#[cfg(not(test))]
#[inline(always)]
fn maybe_test_panic(_point: u8) {}

#[cfg(test)]
fn reserve_output<T>(staged: &mut Vec<T>, length: usize) -> Result<(), SakuraDocumentCodecStatus> {
    if FORCED_FAULT.with(Cell::get) == FAULT_STAGING_RESERVATION {
        return Err(InternalError);
    }
    staged.try_reserve_exact(length).map_err(|_| InternalError)
}

#[cfg(not(test))]
fn reserve_output<T>(staged: &mut Vec<T>, length: usize) -> Result<(), SakuraDocumentCodecStatus> {
    staged.try_reserve_exact(length).map_err(|_| InternalError)
}

fn validate_output(
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> Result<AddressRange, SakuraDocumentCodecStatus> {
    let (descriptor, range) = read_descriptor(output.cast_const())?;
    validate_header::<SakuraDocumentCodecUtf8ResultV1>(
        descriptor.struct_size,
        descriptor.abi_version,
        descriptor.reserved0 == 0 && descriptor.reserved == [0; 1],
    )?;
    Ok(range)
}

fn validate_request<Unit: Copy>(
    request: *const SakuraDocumentCodecUtf8RequestV1<Unit>,
) -> Result<ValidatedRequest<Unit>, SakuraDocumentCodecStatus> {
    let (request, descriptor_range) = read_descriptor(request)?;
    validate_header::<SakuraDocumentCodecUtf8RequestV1<Unit>>(
        request.struct_size,
        request.abi_version,
        request.reserved0 == 0 && request.reserved == [0; 2],
    )?;
    let variant = Utf8Variant::from_wire(request.variant).ok_or(InvalidArgument)?;
    if request.length > UTF8_CODEC_MAX_INPUT_V1 {
        return Err(InvalidArgument);
    }
    let length = usize::try_from(request.length).map_err(|_| InvalidArgument)?;
    Ok(ValidatedRequest {
        data: request.data,
        length,
        variant,
        descriptor_range,
    })
}

fn validate_buffer<Unit: Copy>(
    buffer: *const SakuraDocumentCodecUtf8BufferV1<Unit>,
) -> Result<ValidatedBuffer<Unit>, SakuraDocumentCodecStatus> {
    let (buffer, descriptor_range) = read_descriptor(buffer)?;
    validate_header::<SakuraDocumentCodecUtf8BufferV1<Unit>>(
        buffer.struct_size,
        buffer.abi_version,
        buffer.reserved == [0; 2],
    )?;
    let capacity = usize::try_from(buffer.capacity).map_err(|_| InvalidCapacity)?;
    let byte_capacity = capacity
        .checked_mul(size_of::<Unit>())
        .ok_or(InvalidCapacity)?;
    if byte_capacity > isize::MAX.cast_unsigned() {
        return Err(InvalidCapacity);
    }
    // An empty array ignores its address, as an empty input does.
    let range = checked_slice_range(buffer.data.cast_const(), capacity).ok_or(InvalidArgument)?;
    Ok(ValidatedBuffer {
        data: buffer.data,
        capacity,
        descriptor_range,
        range,
    })
}

fn input_slice<Unit>(request: &ValidatedRequest<Unit>) -> &[Unit] {
    if request.length == 0 {
        return &[];
    }
    // SAFETY: The input address and length were validated, and the alias check
    // excluded all writes to it. The ABI requires readable initialized elements
    // in one allocation, stable for the duration of the call.
    unsafe { slice::from_raw_parts(request.data, request.length) }
}

fn wire_result(
    converted: Converted,
) -> Result<SakuraDocumentCodecUtf8ResultV1, SakuraDocumentCodecStatus> {
    Ok(SakuraDocumentCodecUtf8ResultV1 {
        struct_size: u32::try_from(size_of::<SakuraDocumentCodecUtf8ResultV1>())
            .map_err(|_| InternalError)?,
        abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
        output_length: u64::try_from(converted.length).map_err(|_| InternalError)?,
        lossy: u32::from(converted.lossy),
        reserved0: 0,
        reserved: [0; 1],
    })
}

fn measure_impl<C: Conversion>(
    request: *const SakuraDocumentCodecUtf8RequestV1<C::Input>,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> Result<(), SakuraDocumentCodecStatus> {
    let output_range = validate_output(output)?;
    let request = validate_request(request)?;
    let input_range = checked_slice_range(request.data, request.length).ok_or(InvalidArgument)?;
    if overlaps_any(output_range, &[request.descriptor_range, input_range]) {
        return Err(InvalidArgument);
    }
    maybe_test_panic(FAULT_MEASURE_VALIDATED);

    let result = wire_result(C::measure(input_slice(&request), request.variant))?;
    maybe_test_panic(FAULT_MEASURE_CONVERTED);
    // SAFETY: The validated output slot is writable by contract and does not
    // overlap a read range. The result is complete and no fallible work follows.
    unsafe { output.write(result) };
    Ok(())
}

fn write_impl<C: Conversion>(
    request: *const SakuraDocumentCodecUtf8RequestV1<C::Input>,
    buffer: *const SakuraDocumentCodecUtf8BufferV1<C::Output>,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> Result<(), SakuraDocumentCodecStatus> {
    let output_range = validate_output(output)?;
    let request = validate_request(request)?;
    let buffer = validate_buffer(buffer)?;
    let input_range = checked_slice_range(request.data, request.length).ok_or(InvalidArgument)?;
    let read_ranges = [
        request.descriptor_range,
        buffer.descriptor_range,
        input_range,
    ];
    if overlaps(output_range, buffer.range)
        || overlaps_any(output_range, &read_ranges)
        || overlaps_any(buffer.range, &read_ranges)
    {
        return Err(InvalidArgument);
    }
    maybe_test_panic(FAULT_WRITE_VALIDATED);

    let input = input_slice(&request);
    let required = C::measure(input, request.variant).length;
    if required > buffer.capacity {
        return Err(InvalidCapacity);
    }
    let mut staged = Vec::new();
    reserve_output(&mut staged, required)?;
    staged.resize(required, C::Output::default());
    let converted = C::write(input, request.variant, &mut staged).map_err(|_| InternalError)?;
    let result = wire_result(converted)?;
    maybe_test_panic(FAULT_WRITE_STAGED);
    if !staged.is_empty() {
        // SAFETY: Capacity covers the complete staged output and the checked
        // caller range is writable by contract. The owned Vec cannot overlap it.
        unsafe { ptr::copy_nonoverlapping(staged.as_ptr(), buffer.data, staged.len()) };
    }
    // SAFETY: The checked output slot is writable by contract, and all reads
    // and the non-overlapping output copy are complete.
    unsafe { output.write(result) };
    Ok(())
}

/// Measures the UTF-16 units a UTF-8 or CESU-8 byte region decodes to, without allocating or retaining caller
/// pointers.
///
/// The result's length and lossy fields are ignored on entry. On success `output_length` is the exact unit count a
/// write needs, and `lossy` is 0: decode keeps every byte, giving an unreadable one the unit `0xDC00 + byte`.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call. A nonempty input region must be
/// readable and stable in one allocation. The result descriptor must be writable. Success requires it to be
/// disjoint from both input ranges.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_utf8_decode_measure_v1(
    request: *const SakuraDocumentCodecUtf8DecodeRequestV1,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| measure_impl::<Decode>(request, output))
}

/// Decodes independently of measure and publishes the complete unit array on success.
///
/// The units are the same ones measure counts. A capacity below that count writes nothing and returns
/// InvalidCapacity. The unused suffix stays intact.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call. A nonempty input region must be
/// readable and stable in one allocation. The result descriptor and any nonempty unit array must be writable and
/// disjoint from each other and from every input range for a successful call. The unit array must be 2-byte
/// aligned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_utf8_decode_write_v1(
    request: *const SakuraDocumentCodecUtf8DecodeRequestV1,
    buffer: *const SakuraDocumentCodecUtf8DecodeBufferV1,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| write_impl::<Decode>(request, buffer, output))
}

/// Measures the bytes a UTF-16 unit region encodes to as UTF-8 or CESU-8, without allocating or retaining caller
/// pointers.
///
/// The result's length and lossy fields are ignored on entry. On success `output_length` is the exact byte count a
/// write needs, and `lossy` is 1 when an unpaired surrogate outside U+DC00..U+DCFF becomes `?`.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call. A nonempty input region must be
/// 2-byte aligned, readable and stable in one allocation. The result descriptor must be writable. Success
/// requires it to be disjoint from both input ranges.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_utf8_encode_measure_v1(
    request: *const SakuraDocumentCodecUtf8EncodeRequestV1,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| measure_impl::<Encode>(request, output))
}

/// Encodes independently of measure and publishes the complete byte array on success.
///
/// The bytes and the loss flag are the ones measure reports. A capacity below the byte count writes nothing and
/// returns InvalidCapacity. The unused suffix stays intact.
///
/// # Safety
///
/// Descriptors at accepted addresses must be initialized and live for the call. A nonempty input region must be
/// 2-byte aligned, readable and stable in one allocation. The result descriptor and any nonempty byte array must
/// be writable and disjoint from each other and from every input range for a successful call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_document_codec_utf8_encode_write_v1(
    request: *const SakuraDocumentCodecUtf8EncodeRequestV1,
    buffer: *const SakuraDocumentCodecUtf8EncodeBufferV1,
    output: *mut SakuraDocumentCodecUtf8ResultV1,
) -> SakuraDocumentCodecStatus {
    catch_status(|| write_impl::<Encode>(request, buffer, output))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fmt::Debug;
    use std::mem::{align_of, offset_of};

    type Status = SakuraDocumentCodecStatus;
    type Request<Unit> = SakuraDocumentCodecUtf8RequestV1<Unit>;
    type Buffer<Unit> = SakuraDocumentCodecUtf8BufferV1<Unit>;
    type Outcome = SakuraDocumentCodecUtf8ResultV1;

    const UTF8: Utf8Variant = Utf8Variant::Utf8;
    const CESU8: Utf8Variant = Utf8Variant::Cesu8;
    const VARIANTS: [Utf8Variant; 2] = [UTF8, CESU8];
    const SENTINEL_LENGTH: u64 = 0xA5A5_A5A5_A5A5_A5A5;
    const SENTINEL_LOSSY: u32 = 0x5A5A_5A5A;

    /// The real exports of one direction, the element that fills every untouched slot, one nonempty conversion
    /// that both variants agree on, and the document codec read directly as the reference.
    trait Exports:
        Conversion<Input: Debug + Eq + From<u8> + 'static, Output: Debug + Eq + 'static>
    {
        const SENTINEL: Self::Output;
        const SAMPLE_INPUT: &'static [Self::Input];
        const SAMPLE_OUTPUT: &'static [Self::Output];

        fn measure_export(request: *const Request<Self::Input>, output: *mut Outcome) -> Status;
        fn write_export(
            request: *const Request<Self::Input>,
            buffer: *const Buffer<Self::Output>,
            output: *mut Outcome,
        ) -> Status;
        fn reference(input: &[Self::Input], variant: Utf8Variant) -> (Vec<Self::Output>, bool);
        fn input_from_bytes(bytes: &[u8]) -> Vec<Self::Input>;
        fn output_from_bytes(bytes: &[u8]) -> Vec<Self::Output>;
    }

    fn little_endian_units(bytes: &[u8]) -> Vec<u16> {
        bytes
            .chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect()
    }

    impl Exports for Decode {
        const SENTINEL: u16 = 0xA5A5;
        const SAMPLE_INPUT: &'static [u8] = &[0xE3, 0x81, 0x82];
        const SAMPLE_OUTPUT: &'static [u16] = &[0x3042];

        fn measure_export(request: *const Request<u8>, output: *mut Outcome) -> Status {
            // SAFETY: The test supplies live initialized storage for every accessed
            // descriptor and input. Bad addresses exercise validation before access.
            unsafe { sakura_document_codec_utf8_decode_measure_v1(request, output) }
        }

        fn write_export(
            request: *const Request<u8>,
            buffer: *const Buffer<u16>,
            output: *mut Outcome,
        ) -> Status {
            // SAFETY: The test supplies live storage for all accessed ranges.
            // Intentionally invalid addresses or overlaps must be rejected.
            unsafe { sakura_document_codec_utf8_decode_write_v1(request, buffer, output) }
        }

        fn reference(input: &[u8], variant: Utf8Variant) -> (Vec<u16>, bool) {
            let mut units = vec![0; utf8_decode_measure(input, variant)];
            assert_eq!(
                utf8_decode_write(input, variant, &mut units),
                Ok(units.len())
            );
            (units, false)
        }

        fn input_from_bytes(bytes: &[u8]) -> Vec<u8> {
            bytes.to_vec()
        }

        fn output_from_bytes(bytes: &[u8]) -> Vec<u16> {
            little_endian_units(bytes)
        }
    }

    impl Exports for Encode {
        const SENTINEL: u8 = 0xA5;
        const SAMPLE_INPUT: &'static [u16] = &[0x3042];
        const SAMPLE_OUTPUT: &'static [u8] = &[0xE3, 0x81, 0x82];

        fn measure_export(request: *const Request<u16>, output: *mut Outcome) -> Status {
            // SAFETY: The test supplies live initialized storage for every accessed
            // descriptor and input. Bad addresses exercise validation before access.
            unsafe { sakura_document_codec_utf8_encode_measure_v1(request, output) }
        }

        fn write_export(
            request: *const Request<u16>,
            buffer: *const Buffer<u8>,
            output: *mut Outcome,
        ) -> Status {
            // SAFETY: The test supplies live storage for all accessed ranges.
            // Intentionally invalid addresses or overlaps must be rejected.
            unsafe { sakura_document_codec_utf8_encode_write_v1(request, buffer, output) }
        }

        fn reference(input: &[u16], variant: Utf8Variant) -> (Vec<u8>, bool) {
            let measured = utf8_encode_measure(input, variant);
            let mut bytes = vec![0; measured.length];
            assert_eq!(utf8_encode_write(input, variant, &mut bytes), Ok(measured));
            (bytes, measured.lossy)
        }

        fn input_from_bytes(bytes: &[u8]) -> Vec<u16> {
            little_endian_units(bytes)
        }

        fn output_from_bytes(bytes: &[u8]) -> Vec<u8> {
            bytes.to_vec()
        }
    }

    fn bytes<T: Copy>(value: &T) -> Vec<u8> {
        // SAFETY: Every descriptor under test is repr(C) without padding, so all
        // of its bytes are initialized.
        unsafe { slice::from_raw_parts(ptr::from_ref(value).cast::<u8>(), size_of::<T>()) }.to_vec()
    }

    fn request<C: Exports>(data: &[C::Input], variant: Utf8Variant) -> Request<C::Input> {
        Request {
            struct_size: u32::try_from(size_of::<Request<C::Input>>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            data: data.as_ptr(),
            length: u64::try_from(data.len()).unwrap(),
            variant: variant as u32,
            reserved0: 0,
            reserved: [0; 2],
        }
    }

    fn buffer<C: Exports>(elements: &mut [C::Output]) -> Buffer<C::Output> {
        Buffer {
            struct_size: u32::try_from(size_of::<Buffer<C::Output>>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            data: elements.as_mut_ptr(),
            capacity: u64::try_from(elements.len()).unwrap(),
            reserved: [0; 2],
        }
    }

    fn output() -> Outcome {
        Outcome {
            struct_size: u32::try_from(size_of::<Outcome>()).unwrap(),
            abi_version: DOCUMENT_CODEC_ABI_VERSION_V1,
            output_length: SENTINEL_LENGTH,
            lossy: SENTINEL_LOSSY,
            reserved0: 0,
            reserved: [0; 1],
        }
    }

    fn assert_outcome(value: &Outcome, length: usize, lossy: bool) {
        assert_eq!(value.struct_size, 32);
        assert_eq!(value.abi_version, 1);
        assert_eq!(value.output_length, u64::try_from(length).unwrap());
        assert_eq!(value.lossy, u32::from(lossy));
        assert_eq!(value.reserved0, 0);
        assert_eq!(value.reserved, [0; 1]);
    }

    fn assert_measure_failure<C: Exports>(
        request: &Request<C::Input>,
        mut result: Outcome,
        expected: Status,
    ) {
        let request_before = bytes(request);
        let result_before = bytes(&result);
        assert_eq!(C::measure_export(request, &raw mut result), expected);
        assert_eq!(bytes(&result), result_before);
        assert_eq!(bytes(request), request_before);
    }

    fn assert_write_failure<C: Exports>(
        request: &Request<C::Input>,
        elements: &mut [C::Output],
        configure: impl FnOnce(&mut Buffer<C::Output>, &mut Outcome),
        expected: Status,
    ) {
        let mut buffer = buffer::<C>(elements);
        let mut result = output();
        configure(&mut buffer, &mut result);
        let request_before = bytes(request);
        let buffer_before = bytes(&buffer);
        let result_before = bytes(&result);
        assert_eq!(
            C::write_export(request, &raw const buffer, &raw mut result),
            expected
        );
        assert_eq!(bytes(&result), result_before);
        assert_eq!(bytes(&buffer), buffer_before);
        assert_eq!(bytes(request), request_before);
        assert!(elements.iter().all(|&element| element == C::SENTINEL));
    }

    fn assert_both_fail<C: Exports>(request: &Request<C::Input>, expected: Status) {
        assert_measure_failure::<C>(request, output(), expected);
        assert_write_failure::<C>(request, &mut [C::SENTINEL; 4], |_, _| {}, expected);
    }

    /// Measure reports the expected output, a write with exactly that capacity and with spare capacity publishes
    /// it and the same result and leaves the spare slots alone, and a write one slot short changes nothing.
    fn assert_success<C: Exports>(
        request: &Request<C::Input>,
        expected: &[C::Output],
        lossy: bool,
    ) {
        let request_before = bytes(request);
        let mut measured = output();
        assert_eq!(C::measure_export(request, &raw mut measured), Status::Ok);
        assert_outcome(&measured, expected.len(), lossy);
        assert_eq!(bytes(request), request_before);

        for capacity in [expected.len(), expected.len() + 3] {
            let mut elements = vec![C::SENTINEL; capacity];
            let buffer = buffer::<C>(&mut elements);
            let buffer_before = bytes(&buffer);
            let mut written = output();
            assert_eq!(
                C::write_export(request, &raw const buffer, &raw mut written),
                Status::Ok
            );
            assert_eq!(bytes(&written), bytes(&measured));
            assert_eq!(bytes(request), request_before);
            assert_eq!(bytes(&buffer), buffer_before);
            let (prefix, suffix) = elements.split_at(expected.len());
            assert_eq!(prefix, expected);
            assert!(suffix.iter().all(|&element| element == C::SENTINEL));
        }
        if !expected.is_empty() {
            let mut elements = vec![C::SENTINEL; expected.len() - 1];
            assert_write_failure::<C>(request, &mut elements, |_, _| {}, InvalidCapacity);
        }
    }

    /// Every sequence of up to `max_length` symbols over `alphabet`, shortest first.
    fn for_each_sequence<T: Copy>(alphabet: &[T], max_length: u32, mut visit: impl FnMut(&[T])) {
        let radix = alphabet.len();
        let mut sequence = Vec::new();
        for length in 0..=max_length {
            for mut code in 0..radix.pow(length) {
                sequence.clear();
                for _ in 0..length {
                    sequence.push(alphabet[code % radix]);
                    code /= radix;
                }
                visit(&sequence);
            }
        }
    }

    const DECODE_BOTH: [(&[u8], &[u16]); 7] = [
        (&[], &[]),
        (&[0x41, 0x00, 0x7F], &[0x0041, 0x0000, 0x007F]),
        (&[0xC3, 0xA9], &[0x00E9]),
        (&[0xE3, 0x81, 0x82], &[0x3042]),
        (&[0xFF, 0xC0, 0x80], &[0xDCFF, 0xDCC0, 0xDC80]),
        (&[0xE3, 0x81], &[0xDCE3, 0xDC81]),
        (&[0xED, 0xA0, 0x80], &[0xDCED, 0xDCA0, 0xDC80]),
    ];
    /// Bytes, the UTF-8 units and the CESU-8 units.
    const DECODE_SPLIT: [(&[u8], &[u16], &[u16]); 2] = [
        (
            &[0xF0, 0x9F, 0x98, 0x80],
            &[0xD83D, 0xDE00],
            &[0xDCF0, 0xDC9F, 0xDC98, 0xDC80],
        ),
        (
            &[0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80],
            &[0xDCED, 0xDCA0, 0xDCBD, 0xDCED, 0xDCB8, 0xDC80],
            &[0xD83D, 0xDE00],
        ),
    ];
    /// Units, the bytes both variants write and whether that is lossy.
    const ENCODE_BOTH: [(&[u16], &[u8], bool); 7] = [
        (&[], &[], false),
        (
            &[0x0041, 0x0000, 0x007F, 0x00E9, 0x3042],
            &[0x41, 0x00, 0x7F, 0xC3, 0xA9, 0xE3, 0x81, 0x82],
            false,
        ),
        (&[0xDC80], &[0x80], false),
        (&[0xDCFF], &[0xFF], false),
        (&[0xD800], &[0x3F], true),
        (&[0xDD00], &[0x3F], true),
        (&[0xD83D, 0x0041], &[0x3F, 0x41], true),
    ];
    /// Units, the UTF-8 bytes and the CESU-8 bytes of a pair.
    const ENCODE_SPLIT: (&[u16], &[u8], &[u8]) = (
        &[0xD83D, 0xDE00],
        &[0xF0, 0x9F, 0x98, 0x80],
        &[0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80],
    );

    #[test]
    fn abi_layout_matches_the_native_header() {
        macro_rules! layout {
            ($ty:ty, $size:literal, $align:literal, $($field:ident : $offset:literal),+ $(,)?) => {
                assert_eq!(size_of::<$ty>(), $size);
                assert_eq!(align_of::<$ty>(), $align);
                $(assert_eq!(offset_of!($ty, $field), $offset);)+
            };
        }

        layout!(
            Request<u8>, 48, 8, struct_size: 0, abi_version: 4, data: 8, length: 16,
            variant: 24, reserved0: 28, reserved: 32,
        );
        layout!(
            Request<u16>, 48, 8, struct_size: 0, abi_version: 4, data: 8, length: 16,
            variant: 24, reserved0: 28, reserved: 32,
        );
        layout!(
            Buffer<u16>, 40, 8, struct_size: 0, abi_version: 4, data: 8, capacity: 16,
            reserved: 24,
        );
        layout!(
            Buffer<u8>, 40, 8, struct_size: 0, abi_version: 4, data: 8, capacity: 16,
            reserved: 24,
        );
        layout!(
            Outcome, 32, 8, struct_size: 0, abi_version: 4, output_length: 8, lossy: 16,
            reserved0: 20, reserved: 24,
        );
        assert_eq!(UTF8_CODEC_MAX_INPUT_V1, 2_147_483_647);
        assert_eq!(VARIANTS.map(|variant| variant as u32), [4, 7]);
    }

    #[test]
    fn literal_decode_rows_hold_in_both_variants() {
        for variant in VARIANTS {
            for (input, units) in DECODE_BOTH {
                assert_success::<Decode>(&request::<Decode>(input, variant), units, false);
            }
        }
        for (input, utf8_units, cesu8_units) in DECODE_SPLIT {
            assert_success::<Decode>(&request::<Decode>(input, UTF8), utf8_units, false);
            assert_success::<Decode>(&request::<Decode>(input, CESU8), cesu8_units, false);
        }
    }

    #[test]
    fn literal_encode_rows_hold_in_both_variants() {
        for variant in VARIANTS {
            for (units, expected, lossy) in ENCODE_BOTH {
                assert_success::<Encode>(&request::<Encode>(units, variant), expected, lossy);
            }
        }
        let (units, utf8_bytes, cesu8_bytes) = ENCODE_SPLIT;
        assert_success::<Encode>(&request::<Encode>(units, UTF8), utf8_bytes, false);
        assert_success::<Encode>(&request::<Encode>(units, CESU8), cesu8_bytes, false);
    }

    fn zero_length_input_ignores_its_address<C: Exports>() {
        for address in [0, 1, 3, usize::MAX] {
            let mut request = request::<C>(&[], UTF8);
            request.data = ptr::without_provenance(address);
            assert_success::<C>(&request, &[], false);
        }
    }

    #[test]
    fn zero_length_input_ignores_its_address_in_both_directions() {
        zero_length_input_ignores_its_address::<Decode>();
        zero_length_input_ignores_its_address::<Encode>();
    }

    fn variant_numbers_and_length_bound<C: Exports>() {
        let valid = request::<C>(C::SAMPLE_INPUT, UTF8);
        for variant in [0, 1, 3, 5, 6, 8, 0x8000_0004, u32::MAX] {
            assert_both_fail::<C>(&Request { variant, ..valid }, InvalidArgument);
        }
        assert_success::<C>(
            &Request {
                variant: 4,
                ..valid
            },
            C::SAMPLE_OUTPUT,
            false,
        );
        assert_success::<C>(
            &Request {
                variant: 7,
                ..valid
            },
            C::SAMPLE_OUTPUT,
            false,
        );

        let phantom = ptr::without_provenance::<C::Input>(2);
        let at_bound = Request {
            data: phantom,
            length: UTF8_CODEC_MAX_INPUT_V1,
            ..valid
        };
        // Validation accepts the bound itself: only the injected fault, which fires after
        // validation and before the first access to the input, stops these two calls.
        {
            let _fault = FaultGuard::inject(FAULT_MEASURE_VALIDATED);
            assert_measure_failure::<C>(&at_bound, output(), InternalError);
        }
        {
            let _fault = FaultGuard::inject(FAULT_WRITE_VALIDATED);
            assert_write_failure::<C>(&at_bound, &mut [C::SENTINEL; 4], |_, _| {}, InternalError);
        }
        for length in [UTF8_CODEC_MAX_INPUT_V1 + 1, 1 << 32, 1 << 63, u64::MAX] {
            assert_both_fail::<C>(&Request { length, ..at_bound }, InvalidArgument);
        }
    }

    #[test]
    fn variant_numbers_and_the_length_bound_are_enforced_in_both_directions() {
        variant_numbers_and_length_bound::<Decode>();
        variant_numbers_and_length_bound::<Encode>();
    }

    fn malformed_headers<C: Exports>() {
        let valid = request::<C>(C::SAMPLE_INPUT, UTF8);
        // Each row changes one header field of a 48-byte request: size, version, reserved0, reserved.
        for (struct_size, abi_version, reserved0, reserved) in [
            (47, 1, 0, [0, 0]),
            (49, 1, 0, [0, 0]),
            (0, 1, 0, [0, 0]),
            (48, 0, 0, [0, 0]),
            (48, 2, 0, [0, 0]),
            (48, u32::MAX, 0, [0, 0]),
            (48, 1, 1, [0, 0]),
            (48, 1, u32::MAX, [0, 0]),
            (48, 1, 0, [1, 0]),
            (48, 1, 0, [0, 1]),
            (48, 1, 0, [u64::MAX, u64::MAX]),
        ] {
            let request = Request {
                struct_size,
                abi_version,
                reserved0,
                reserved,
                ..valid
            };
            assert_both_fail::<C>(&request, InvalidArgument);
        }
        // The same for the 32-byte result: size, version, reserved0, reserved.
        for (struct_size, abi_version, reserved0, reserved) in [
            (31, 1, 0, [0]),
            (33, 1, 0, [0]),
            (0, 1, 0, [0]),
            (32, 0, 0, [0]),
            (32, 2, 0, [0]),
            (32, u32::MAX, 0, [0]),
            (32, 1, 1, [0]),
            (32, 1, u32::MAX, [0]),
            (32, 1, 0, [1]),
            (32, 1, 0, [u64::MAX]),
        ] {
            let bad = Outcome {
                struct_size,
                abi_version,
                reserved0,
                reserved,
                ..output()
            };
            assert_measure_failure::<C>(&valid, bad, InvalidArgument);
            assert_write_failure::<C>(
                &valid,
                &mut [C::SENTINEL; 4],
                |_, result| *result = bad,
                InvalidArgument,
            );
        }
        // And for the 40-byte buffer: size, version, reserved.
        for (struct_size, abi_version, reserved) in [
            (39, 1, [0, 0]),
            (41, 1, [0, 0]),
            (0, 1, [0, 0]),
            (40, 0, [0, 0]),
            (40, 2, [0, 0]),
            (40, u32::MAX, [0, 0]),
            (40, 1, [1, 0]),
            (40, 1, [0, 1]),
            (40, 1, [u64::MAX, u64::MAX]),
        ] {
            assert_write_failure::<C>(
                &valid,
                &mut [C::SENTINEL; 4],
                |buffer, _| {
                    buffer.struct_size = struct_size;
                    buffer.abi_version = abi_version;
                    buffer.reserved = reserved;
                },
                InvalidArgument,
            );
        }
    }

    #[test]
    fn malformed_headers_preserve_all_bytes_in_both_directions() {
        malformed_headers::<Decode>();
        malformed_headers::<Encode>();
    }

    fn bad_pointers<C: Exports>() {
        let valid = request::<C>(C::SAMPLE_INPUT, UTF8);
        let valid_before = bytes(&valid);
        let descriptor_addresses = [0, 1, 4, isize::MAX as usize - 7, usize::MAX - 7];
        for address in descriptor_addresses {
            let request_ptr = ptr::without_provenance::<Request<C::Input>>(address);
            let output_ptr = ptr::without_provenance_mut::<Outcome>(address);
            let buffer_ptr = ptr::without_provenance::<Buffer<C::Output>>(address);

            let mut result = output();
            let result_before = bytes(&result);
            assert_eq!(
                C::measure_export(request_ptr, &raw mut result),
                InvalidArgument
            );
            assert_eq!(C::measure_export(&valid, output_ptr), InvalidArgument);
            let mut elements = [C::SENTINEL; 4];
            let buffer = buffer::<C>(&mut elements);
            let buffer_before = bytes(&buffer);
            assert_eq!(
                C::write_export(request_ptr, &raw const buffer, &raw mut result),
                InvalidArgument
            );
            assert_eq!(
                C::write_export(&valid, buffer_ptr, &raw mut result),
                InvalidArgument
            );
            assert_eq!(
                C::write_export(&valid, &raw const buffer, output_ptr),
                InvalidArgument
            );
            assert_eq!(bytes(&result), result_before);
            assert_eq!(bytes(&buffer), buffer_before);
            assert_eq!(elements, [C::SENTINEL; 4]);
            assert_eq!(bytes(&valid), valid_before);
        }

        let input_size = size_of::<C::Input>();
        let mut bad_inputs = vec![0, isize::MAX as usize + 1 - input_size];
        if input_size > 1 {
            bad_inputs.push(1);
        }
        for address in bad_inputs {
            let mut request = valid;
            request.data = ptr::without_provenance(address);
            assert_both_fail::<C>(&request, InvalidArgument);
        }

        let output_size = size_of::<C::Output>();
        let mut bad_arrays = vec![0, isize::MAX as usize + 1 - output_size];
        if output_size > 1 {
            bad_arrays.push(1);
        }
        for address in bad_arrays {
            assert_write_failure::<C>(
                &valid,
                &mut [C::SENTINEL; 4],
                |buffer, _| buffer.data = ptr::without_provenance_mut(address),
                InvalidArgument,
            );
        }
    }

    #[test]
    fn bad_pointers_are_rejected_before_any_access_in_both_directions() {
        bad_pointers::<Decode>();
        bad_pointers::<Encode>();
    }

    fn capacity_rules<C: Exports>() {
        let empty = request::<C>(&[], UTF8);
        for unused in [ptr::null_mut(), ptr::without_provenance_mut(usize::MAX)] {
            let mut buffer = buffer::<C>(&mut []);
            buffer.data = unused;
            let buffer_before = bytes(&buffer);
            let mut result = output();
            assert_eq!(
                C::write_export(&empty, &raw const buffer, &raw mut result),
                Status::Ok
            );
            assert_outcome(&result, 0, false);
            assert_eq!(bytes(&buffer), buffer_before);
        }

        let request = request::<C>(C::SAMPLE_INPUT, UTF8);
        for short in 0..C::SAMPLE_OUTPUT.len() {
            assert_write_failure::<C>(
                &request,
                &mut vec![C::SENTINEL; short],
                |_, _| {},
                InvalidCapacity,
            );
        }
        assert_write_failure::<C>(
            &request,
            &mut [],
            |buffer, _| buffer.data = ptr::null_mut(),
            InvalidCapacity,
        );
        assert_success::<C>(&request, C::SAMPLE_OUTPUT, false);

        // The byte count overflows (u64::MAX), exceeds isize::MAX, or wraps around to a small value
        // ((1 << 63) + 1 two-byte units make 2 bytes). The capacity decides before the pointer is read.
        let output_size = u64::try_from(size_of::<C::Output>()).unwrap();
        for capacity in [u64::MAX, isize::MAX as u64 / output_size + 1, (1 << 63) + 1] {
            assert_write_failure::<C>(
                &request,
                &mut [C::SENTINEL; 2],
                |buffer, _| {
                    buffer.capacity = capacity;
                    buffer.data = ptr::null_mut();
                },
                InvalidCapacity,
            );
        }
        // A byte count that fits in isize::MAX but cannot start at a real address is a range failure.
        assert_write_failure::<C>(
            &request,
            &mut [C::SENTINEL; 4],
            |buffer, _| buffer.capacity = isize::MAX as u64 / output_size,
            InvalidArgument,
        );
    }

    #[test]
    fn capacity_errors_are_atomic_and_spare_slots_stay_untouched_in_both_directions() {
        capacity_rules::<Decode>();
        capacity_rules::<Encode>();
    }

    fn first_invalid_check_decides<C: Exports>() {
        let valid = request::<C>(C::SAMPLE_INPUT, UTF8);
        let overflow = |buffer: &mut Buffer<C::Output>| {
            buffer.capacity = u64::MAX;
            buffer.data = ptr::null_mut();
        };
        // The output header comes first.
        assert_write_failure::<C>(
            &valid,
            &mut [C::SENTINEL; 1],
            |buffer, result| {
                overflow(buffer);
                result.abi_version = 0;
            },
            InvalidArgument,
        );
        // Then the request header, variant and length, before the buffer is examined.
        for bad in [
            Request {
                variant: 5,
                ..valid
            },
            Request {
                length: u64::MAX,
                ..valid
            },
            Request {
                reserved0: 1,
                ..valid
            },
        ] {
            assert_write_failure::<C>(
                &bad,
                &mut [C::SENTINEL; 1],
                |buffer, _| overflow(buffer),
                InvalidArgument,
            );
        }
        // The buffer header and capacity arithmetic come before the input and array ranges.
        assert_write_failure::<C>(
            &valid,
            &mut [C::SENTINEL; 1],
            |buffer, _| {
                overflow(buffer);
                buffer.abi_version = 0;
            },
            InvalidArgument,
        );
        let null_input = Request {
            data: ptr::null(),
            ..valid
        };
        assert_write_failure::<C>(
            &null_input,
            &mut [C::SENTINEL; 1],
            |buffer, _| overflow(buffer),
            InvalidCapacity,
        );
        // The ranges and overlaps come before the capacity refusal.
        assert_write_failure::<C>(&null_input, &mut [], |_, _| {}, InvalidArgument);
        assert_write_failure::<C>(
            &valid,
            &mut [],
            |buffer, _| buffer.data = ptr::without_provenance_mut(1),
            InvalidCapacity,
        );
    }

    #[test]
    fn the_first_invalid_check_decides_the_status_in_both_directions() {
        first_invalid_check_decides::<Decode>();
        first_invalid_check_decides::<Encode>();
    }

    const ARENA_SIZE: usize = 1280;
    const ARENA_FILL: u8 = 0xA5;

    #[repr(C, align(8))]
    struct Arena([u8; ARENA_SIZE]);

    /// Byte offsets inside one arena of everything a call touches. Every offset is a multiple of 8, so every
    /// placement is aligned for every element type.
    #[derive(Clone, Copy)]
    struct Placement {
        request: usize,
        buffer: usize,
        output: usize,
        input: usize,
        input_elements: usize,
        array: usize,
        array_elements: usize,
    }

    /// Nothing touches anything else. The array holds 48 elements, the most that 16 input elements can convert
    /// to (three bytes per UTF-16 unit when encoding).
    const APART: Placement = Placement {
        request: 0,
        buffer: 256,
        output: 512,
        input: 768,
        input_elements: 16,
        array: 1024,
        array_elements: 48,
    };

    fn put<T>(base: *mut u8, offset: usize, value: T) {
        // SAFETY: Every offset used here is a multiple of 8 inside the arena, and
        // the value ends before the arena does.
        unsafe { base.add(offset).cast::<T>().write(value) }
    }

    fn snapshot(base: *const u8) -> Vec<u8> {
        // SAFETY: The arena is initialized and outlives every call that uses it.
        unsafe { slice::from_raw_parts(base, ARENA_SIZE) }.to_vec()
    }

    fn le_u64(arena: &[u8], at: usize) -> u64 {
        u64::from_le_bytes(arena[at..at + 8].try_into().unwrap())
    }

    fn le_u32(arena: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(arena[at..at + 4].try_into().unwrap())
    }

    /// Runs one call over objects placed in a fresh arena whose input holds the letters A, B, ... repeating and
    /// whose other bytes are `ARENA_FILL`. Returns the status and the arena before and after the call.
    fn run_placed<C: Exports>(
        placement: Placement,
        write: bool,
        tweak: impl FnOnce(&mut Request<C::Input>, &mut Buffer<C::Output>),
    ) -> (Status, Vec<u8>, Vec<u8>) {
        let mut arena = Arena([ARENA_FILL; ARENA_SIZE]);
        let base = (&raw mut arena).cast::<u8>();
        for index in 0..placement.input_elements {
            let letter = C::Input::from(b'A' + u8::try_from(index % 26).unwrap());
            put(
                base,
                placement.input + index * size_of::<C::Input>(),
                letter,
            );
        }
        let mut request = Request::<C::Input> {
            struct_size: 48,
            abi_version: 1,
            data: base.wrapping_add(placement.input).cast_const().cast(),
            length: u64::try_from(placement.input_elements).unwrap(),
            variant: UTF8 as u32,
            reserved0: 0,
            reserved: [0; 2],
        };
        let mut buffer = Buffer::<C::Output> {
            struct_size: 40,
            abi_version: 1,
            data: base.wrapping_add(placement.array).cast(),
            capacity: u64::try_from(placement.array_elements).unwrap(),
            reserved: [0; 2],
        };
        tweak(&mut request, &mut buffer);
        // A descriptor written later wins where two descriptors share bytes.
        put(base, placement.output, output());
        put(base, placement.buffer, buffer);
        put(base, placement.request, request);
        let before = snapshot(base);
        let request_at = base
            .wrapping_add(placement.request)
            .cast_const()
            .cast::<Request<C::Input>>();
        let buffer_at = base
            .wrapping_add(placement.buffer)
            .cast_const()
            .cast::<Buffer<C::Output>>();
        let output_at = base.wrapping_add(placement.output).cast::<Outcome>();
        let status = if write {
            C::write_export(request_at, buffer_at, output_at)
        } else {
            C::measure_export(request_at, output_at)
        };
        (status, before, snapshot(base))
    }

    /// Asserts that a placement is refused with every arena byte unchanged, or accepted with the result and the
    /// output the codec gives for the input as the arena held it.
    fn check_placed<C: Exports>(
        placement: Placement,
        write: bool,
        refused: bool,
        tweak: impl FnOnce(&mut Request<C::Input>, &mut Buffer<C::Output>),
    ) {
        let (status, before, after) = run_placed::<C>(placement, write, tweak);
        if refused {
            assert_eq!(status, InvalidArgument);
            assert_eq!(after, before);
            return;
        }
        assert_eq!(status, Status::Ok);
        let region_end = placement.input + placement.input_elements * size_of::<C::Input>();
        let input = C::input_from_bytes(&before[placement.input..region_end]);
        let (expected, lossy) = C::reference(&input, UTF8);
        assert_eq!(
            le_u64(&after, placement.output + 8),
            u64::try_from(expected.len()).unwrap()
        );
        assert_eq!(le_u32(&after, placement.output + 16), u32::from(lossy));
        if write {
            let array_end = placement.array + expected.len() * size_of::<C::Output>();
            assert_eq!(
                C::output_from_bytes(&after[placement.array..array_end]),
                expected
            );
            let spare =
                array_end..placement.array + placement.array_elements * size_of::<C::Output>();
            assert_eq!(after[spare.clone()], before[spare]);
        }
    }

    fn overlap_rules<C: Exports>() {
        let region = APART.input_elements * size_of::<C::Input>();
        let array = APART.array_elements * size_of::<C::Output>();
        let meets =
            |a: usize, a_len: usize, b: usize, b_len: usize| a.max(b) < (a + a_len).min(b + b_len);
        // The offsets, in steps of 8, at which an object of `len` bytes abuts, overlaps or covers the fixed range.
        let around = |fixed: usize, fixed_len: usize, len: usize| {
            let mut at = vec![fixed + fixed_len, fixed + fixed_len - 8, fixed, fixed + 8];
            if fixed >= len {
                at.push(fixed - len);
            }
            if fixed + 8 >= len {
                at.push(fixed + 8 - len);
            }
            at.sort_unstable();
            at.dedup();
            at
        };
        for write in [false, true] {
            for at in around(APART.input, region, 32) {
                let refused = meets(at, 32, APART.input, region);
                check_placed::<C>(
                    Placement {
                        output: at,
                        ..APART
                    },
                    write,
                    refused,
                    |_, _| {},
                );
            }
            // The input is read only, so it may share bytes with the descriptors that are read.
            for input in [APART.request, APART.buffer - 8] {
                check_placed::<C>(Placement { input, ..APART }, write, false, |_, _| {});
            }
        }

        // A request placed over the ignored fields of the result stays valid with no input, so the overlap itself
        // is what is refused.
        let no_input = |request: &mut Request<C::Input>| {
            request.data = ptr::null();
            request.length = 0;
        };
        let over_the_result = Placement {
            output: 0,
            request: 8,
            ..APART
        };
        check_placed::<C>(over_the_result, false, true, |request, _| no_input(request));
        check_placed::<C>(over_the_result, true, true, |request, _| no_input(request));
        // The same for a buffer descriptor with no array.
        let buffer_over_the_result = Placement {
            output: 0,
            buffer: 8,
            request: 64,
            ..APART
        };
        check_placed::<C>(buffer_over_the_result, true, true, |request, buffer| {
            no_input(request);
            buffer.data = ptr::null_mut();
            buffer.capacity = 0;
        });

        // The array is written, so it may share bytes with nothing else.
        for at in around(APART.array, array, 32) {
            let refused = meets(at, 32, APART.array, array);
            check_placed::<C>(
                Placement {
                    output: at,
                    ..APART
                },
                true,
                refused,
                |_, _| {},
            );
        }
        for (fixed, fixed_len) in [
            (APART.input, region),
            (APART.request, 48),
            (APART.buffer, 40),
        ] {
            for at in around(fixed, fixed_len, array) {
                let refused = meets(at, array, fixed, fixed_len);
                check_placed::<C>(Placement { array: at, ..APART }, true, refused, |_, _| {});
            }
        }

        // An overlap is refused before a short capacity is, so the status stays InvalidArgument.
        check_placed::<C>(
            Placement {
                output: APART.array,
                ..APART
            },
            true,
            true,
            |_, buffer| buffer.capacity = 1,
        );
    }

    #[test]
    fn writable_ranges_cannot_overlap_anything_and_read_only_input_may_share_descriptors() {
        overlap_rules::<Decode>();
        overlap_rules::<Encode>();
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

    fn every_fault_leaves_caller_bytes_unchanged<C: Exports>() {
        let request = request::<C>(C::SAMPLE_INPUT, UTF8);
        for point in [FAULT_MEASURE_VALIDATED, FAULT_MEASURE_CONVERTED] {
            {
                let _fault = FaultGuard::inject(point);
                assert_measure_failure::<C>(&request, output(), InternalError);
            }
            assert_success::<C>(&request, C::SAMPLE_OUTPUT, false);
        }
        for point in [
            FAULT_WRITE_VALIDATED,
            FAULT_WRITE_STAGED,
            FAULT_STAGING_RESERVATION,
        ] {
            {
                let _fault = FaultGuard::inject(point);
                assert_write_failure::<C>(
                    &request,
                    &mut [C::SENTINEL; 4],
                    |_, _| {},
                    InternalError,
                );
            }
            assert_success::<C>(&request, C::SAMPLE_OUTPUT, false);
        }
    }

    #[test]
    fn every_fault_leaves_caller_bytes_unchanged_and_allows_retry() {
        every_fault_leaves_caller_bytes_unchanged::<Decode>();
        every_fault_leaves_caller_bytes_unchanged::<Encode>();
    }

    #[test]
    fn export_source_contract_contains_panic_containment_for_each_export() {
        let source = include_str!("utf8_codec.rs");
        let (production, _) = source.split_once("mod tests {").unwrap();
        assert_eq!(production.matches("pub unsafe extern \"C\" fn ").count(), 4);
        assert_eq!(production.matches("catch_status(|| ").count(), 4);
        assert!(!production.contains("catch_unwind("));
        assert!(!production.contains("unwrap("));
        assert!(!production.contains("expect("));
    }

    /// The exports against the document codec read directly, on every sequence of up to `max_length` symbols, in
    /// both variants. Returns how many conversions were compared.
    fn compare_with_codec<C: Exports>(alphabet: &[C::Input], max_length: u32) -> usize {
        let mut compared = 0;
        for variant in VARIANTS {
            for_each_sequence(alphabet, max_length, |input| {
                let (expected, lossy) = C::reference(input, variant);
                assert_success::<C>(&request::<C>(input, variant), &expected, lossy);
                compared += 1;
            });
        }
        compared
    }

    #[test]
    fn decode_exports_match_the_codec_on_every_short_byte_sequence() {
        const BOUNDARY_BYTES: [u8; 12] = [
            0x00, 0x41, 0x7F, 0x80, 0xBF, 0xC2, 0xDF, 0xE0, 0xED, 0xF0, 0xF4, 0xFF,
        ];
        const SURROGATE_FORM_BYTES: [u8; 6] = [0xED, 0xA0, 0xAF, 0xB0, 0xBF, 0x41];
        assert_eq!(compare_with_codec::<Decode>(&BOUNDARY_BYTES, 4), 45_242);
        assert_eq!(
            compare_with_codec::<Decode>(&SURROGATE_FORM_BYTES, 6),
            111_974
        );
        let every_byte: Vec<u8> = (0..=u8::MAX).collect();
        assert_eq!(compare_with_codec::<Decode>(&every_byte, 1), 2 * 257);
    }

    #[test]
    fn encode_exports_match_the_codec_on_every_short_unit_sequence() {
        const BOUNDARY_UNITS: [u16; 12] = [
            0x0000, 0x0041, 0x07FF, 0x0800, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDCFF, 0xDD00, 0xDFFF,
            0xFFFF,
        ];
        const SURROGATE_UNITS: [u16; 5] = [0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0x0041];
        assert_eq!(compare_with_codec::<Encode>(&BOUNDARY_UNITS, 4), 45_242);
        assert_eq!(compare_with_codec::<Encode>(&SURROGATE_UNITS, 5), 7_812);
        let every_unit: Vec<u16> = (0..=u16::MAX).collect();
        assert_eq!(compare_with_codec::<Encode>(&every_unit, 1), 2 * 65_537);
    }
}
