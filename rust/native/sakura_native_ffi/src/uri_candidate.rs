//! Stateless Rust URI parse/encode shadow candidate.
//!
//! This module deliberately stays inside the final native FFI crate.  It does
//! not own URI identity, Windows path conversion, comparison keys, or any
//! platform state.  Every entry point copies its input and uses caller-owned
//! two-pass output buffers; no pointer is retained after a call.

use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

use sakura_unicode_core::{
    decode_code_point as decode_utf8_code_point, encode_code_point, MAX_UTF8_BYTES,
};

pub const URI_CANDIDATE_ABI_VERSION_V1: u32 = 1;

const POISON_LENGTH: u64 = u64::MAX;
const POISON_FLAG: u32 = u32::MAX;

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraUriCandidateStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidCapacity = 2,
    EmptyInput = 3,
    MissingScheme = 4,
    InvalidScheme = 5,
    InvalidAuthority = 6,
    InvalidPath = 7,
    InvalidQuery = 8,
    InvalidFragment = 9,
    InvalidPercentEncoding = 10,
    InvalidUtf8 = 11,
    InternalError = 12,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraUriCandidateSpanV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *const u16,
    pub length: u64,
    pub reserved: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraUriCandidateComponentsV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub scheme: SakuraUriCandidateSpanV1,
    pub authority: SakuraUriCandidateSpanV1,
    pub path: SakuraUriCandidateSpanV1,
    pub query: SakuraUriCandidateSpanV1,
    pub fragment: SakuraUriCandidateSpanV1,
    pub has_authority: u32,
    pub has_query: u32,
    pub has_fragment: u32,
    pub reserved: u32,
    pub reserved64: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraUriCandidateBufferV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *mut u16,
    pub capacity: u64,
    pub reserved: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraUriCandidateBuffersV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub scheme: SakuraUriCandidateBufferV1,
    pub authority: SakuraUriCandidateBufferV1,
    pub path: SakuraUriCandidateBufferV1,
    pub query: SakuraUriCandidateBufferV1,
    pub fragment: SakuraUriCandidateBufferV1,
    pub serialized: SakuraUriCandidateBufferV1,
    pub reserved64: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraUriCandidateMeasureV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub scheme_length: u64,
    pub authority_length: u64,
    pub path_length: u64,
    pub query_length: u64,
    pub fragment_length: u64,
    pub serialized_length: u64,
    pub has_authority: u32,
    pub has_query: u32,
    pub has_fragment: u32,
    pub reserved: u32,
    pub reserved64: [u64; 2],
}

pub type SakuraUriCandidateOutputV1 = SakuraUriCandidateMeasureV1;

#[derive(Clone, Copy)]
enum Component {
    Authority,
    Path,
    Query,
    Fragment,
}

#[derive(Clone, Debug)]
struct UriValue {
    scheme: Vec<u16>,
    authority: Vec<u16>,
    path: Vec<u16>,
    query: Option<Vec<u16>>,
    fragment: Option<Vec<u16>>,
    has_authority: bool,
    serialized: Vec<u16>,
}

struct CopiedComponents {
    scheme: Vec<u16>,
    authority: Vec<u16>,
    path: Vec<u16>,
    query: Option<Vec<u16>>,
    fragment: Option<Vec<u16>>,
    has_authority: bool,
}

#[derive(Clone, Copy)]
struct AddressRange {
    start: usize,
    end: usize,
}

fn catch_status(operation: impl FnOnce() -> SakuraUriCandidateStatus) -> SakuraUriCandidateStatus {
    catch_unwind(AssertUnwindSafe(operation)).unwrap_or(SakuraUriCandidateStatus::InternalError)
}

fn is_aligned<T>(pointer: *const T) -> bool {
    (pointer as usize).is_multiple_of(align_of::<T>())
}

fn checked_address_range<T>(pointer: *const T) -> Option<AddressRange> {
    if pointer.is_null() || !is_aligned(pointer) {
        return None;
    }
    let start = pointer as usize;
    let end = start.checked_add(size_of::<T>())?;
    (end <= isize::MAX as usize).then_some(AddressRange { start, end })
}

fn checked_slice_range<T>(pointer: *const T, length: usize) -> Option<AddressRange> {
    if length == 0 {
        return Some(AddressRange { start: 0, end: 0 });
    }
    if pointer.is_null() || !is_aligned(pointer) {
        return None;
    }
    let byte_length = length.checked_mul(size_of::<T>())?;
    if byte_length > isize::MAX as usize {
        return None;
    }
    let start = pointer as usize;
    let end = start.checked_add(byte_length)?;
    (end <= isize::MAX as usize).then_some(AddressRange { start, end })
}

fn overlaps(left: AddressRange, right: AddressRange) -> bool {
    left.start != left.end
        && right.start != right.end
        && left.start < right.end
        && right.start < left.end
}

fn validate_header(
    struct_size: u32,
    abi_version: u32,
    reserved: &[u64],
) -> Result<(), SakuraUriCandidateStatus> {
    if struct_size as usize != size_of::<SakuraUriCandidateSpanV1>()
        || abi_version != URI_CANDIDATE_ABI_VERSION_V1
        || reserved.iter().any(|value| *value != 0)
    {
        Err(SakuraUriCandidateStatus::InvalidArgument)
    } else {
        Ok(())
    }
}

fn validate_span_descriptor(
    span: &SakuraUriCandidateSpanV1,
) -> Result<usize, SakuraUriCandidateStatus> {
    validate_header(span.struct_size, span.abi_version, &span.reserved)?;
    let length =
        usize::try_from(span.length).map_err(|_| SakuraUriCandidateStatus::InvalidArgument)?;
    checked_slice_range(span.data, length).ok_or(SakuraUriCandidateStatus::InvalidArgument)?;
    Ok(length)
}

fn copy_span(span: &SakuraUriCandidateSpanV1) -> Result<Vec<u16>, SakuraUriCandidateStatus> {
    let length = validate_span_descriptor(span)?;
    if length == 0 {
        return Ok(Vec::new());
    }
    // SAFETY: The descriptor has an exact ABI header, and the pointer is
    // aligned, non-null, bounded by isize::MAX, and copied before return.
    Ok(unsafe { slice::from_raw_parts(span.data, length) }.to_vec())
}

fn validate_flag(flag: u32) -> Result<bool, SakuraUriCandidateStatus> {
    match flag {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(SakuraUriCandidateStatus::InvalidArgument),
    }
}

fn validate_components_header(
    components: &SakuraUriCandidateComponentsV1,
) -> Result<(), SakuraUriCandidateStatus> {
    if components.struct_size as usize != size_of::<SakuraUriCandidateComponentsV1>()
        || components.abi_version != URI_CANDIDATE_ABI_VERSION_V1
        || components.reserved != 0
        || components.reserved64 != [0; 2]
    {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    validate_flag(components.has_authority)?;
    validate_flag(components.has_query)?;
    validate_flag(components.has_fragment)?;
    Ok(())
}

fn copy_components(
    components: &SakuraUriCandidateComponentsV1,
) -> Result<CopiedComponents, SakuraUriCandidateStatus> {
    validate_components_header(components)?;
    let has_authority = validate_flag(components.has_authority)?;
    let has_query = validate_flag(components.has_query)?;
    let has_fragment = validate_flag(components.has_fragment)?;
    let scheme = copy_span(&components.scheme)?;
    let authority = copy_span(&components.authority)?;
    let path = copy_span(&components.path)?;
    let query = copy_span(&components.query)?;
    let fragment = copy_span(&components.fragment)?;
    if !has_query && !query.is_empty() {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    if !has_fragment && !fragment.is_empty() {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    Ok(CopiedComponents {
        scheme,
        authority,
        path,
        query: has_query.then_some(query),
        fragment: has_fragment.then_some(fragment),
        has_authority,
    })
}

fn is_ascii_alpha(value: u16) -> bool {
    (b'a' as u16..=b'z' as u16).contains(&value) || (b'A' as u16..=b'Z' as u16).contains(&value)
}

fn is_ascii_digit(value: u16) -> bool {
    (b'0' as u16..=b'9' as u16).contains(&value)
}

fn is_scheme_character(value: u16) -> bool {
    is_ascii_alpha(value)
        || is_ascii_digit(value)
        || value == b'+' as u16
        || value == b'-' as u16
        || value == b'.' as u16
}

fn validate_scheme(scheme: &[u16]) -> Result<(), SakuraUriCandidateStatus> {
    if scheme.is_empty()
        || !is_ascii_alpha(scheme[0])
        || !scheme.iter().copied().all(is_scheme_character)
    {
        Err(SakuraUriCandidateStatus::InvalidScheme)
    } else {
        Ok(())
    }
}

fn is_valid_utf16(value: &[u16]) -> bool {
    let mut index = 0;
    while index < value.len() {
        let code_unit = value[index];
        if (0xd800..=0xdbff).contains(&code_unit) {
            if index + 1 >= value.len() || !(0xdc00..=0xdfff).contains(&value[index + 1]) {
                return false;
            }
            index += 2;
        } else if (0xdc00..=0xdfff).contains(&code_unit) {
            return false;
        } else {
            index += 1;
        }
    }
    true
}

fn has_control_character(value: &[u16]) -> bool {
    value
        .iter()
        .copied()
        .any(|character| character <= 0x1f || character == 0x7f)
}

fn validate_component(
    value: &[u16],
    error: SakuraUriCandidateStatus,
) -> Result<(), SakuraUriCandidateStatus> {
    if is_valid_utf16(value) && !has_control_character(value) {
        Ok(())
    } else {
        Err(error)
    }
}

fn to_lower_ascii(value: u16) -> u16 {
    if (b'A' as u16..=b'Z' as u16).contains(&value) {
        value + (b'a' - b'A') as u16
    } else {
        value
    }
}

fn lower_scheme(scheme: &[u16]) -> Vec<u16> {
    scheme.iter().copied().map(to_lower_ascii).collect()
}

fn is_sub_delimiter(value: u16) -> bool {
    matches!(
        value,
        value if value == b'!' as u16
            || value == b'$' as u16
            || value == b'&' as u16
            || value == b'\'' as u16
            || value == b'(' as u16
            || value == b')' as u16
            || value == b'*' as u16
            || value == b'+' as u16
            || value == b',' as u16
            || value == b';' as u16
            || value == b'=' as u16
    )
}

fn is_unreserved(value: u16) -> bool {
    is_ascii_alpha(value)
        || is_ascii_digit(value)
        || value == b'-' as u16
        || value == b'.' as u16
        || value == b'_' as u16
        || value == b'~' as u16
}

fn is_allowed_literal(value: u16, component: Component) -> bool {
    if is_unreserved(value) || is_sub_delimiter(value) {
        return true;
    }
    match component {
        Component::Authority => {
            value == b':' as u16
                || value == b'@' as u16
                || value == b'[' as u16
                || value == b']' as u16
        }
        Component::Path => value == b':' as u16 || value == b'@' as u16 || value == b'/' as u16,
        Component::Query | Component::Fragment => {
            value == b':' as u16
                || value == b'@' as u16
                || value == b'/' as u16
                || value == b'?' as u16
        }
    }
}

fn encode_component(
    input: &[u16],
    component: Component,
) -> Result<Vec<u16>, SakuraUriCandidateStatus> {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut encoded = Vec::new();
    let mut index = 0;
    while index < input.len() {
        let character = input[index];
        if character <= 0x7f && is_allowed_literal(character, component) {
            encoded.push(character);
            index += 1;
            continue;
        }
        let code_point;
        if (0xd800..=0xdbff).contains(&character) {
            let trailing = input[index + 1];
            code_point =
                0x10000 + (((character - 0xd800) as u32) << 10) + (trailing - 0xdc00) as u32;
            index += 2;
        } else {
            code_point = character as u32;
            index += 1;
        }
        let mut bytes = [0_u8; MAX_UTF8_BYTES];
        let width = encode_code_point(code_point, &mut bytes)
            .map_err(|_| SakuraUriCandidateStatus::InternalError)?;
        for byte in bytes[..width].iter().copied() {
            encoded.push(b'%' as u16);
            encoded.push(HEX[(byte >> 4) as usize] as u16);
            encoded.push(HEX[(byte & 0x0f) as usize] as u16);
        }
    }
    Ok(encoded)
}

fn append_encoded(
    output: &mut Vec<u16>,
    input: &[u16],
    component: Component,
) -> Result<(), SakuraUriCandidateStatus> {
    output.extend(encode_component(input, component)?);
    Ok(())
}

fn serialize(
    scheme: &[u16],
    authority: &[u16],
    path: &[u16],
    query: Option<&[u16]>,
    fragment: Option<&[u16]>,
    has_authority: bool,
) -> Result<Vec<u16>, SakuraUriCandidateStatus> {
    let mut result = Vec::new();
    result.extend_from_slice(scheme);
    result.push(b':' as u16);
    if has_authority {
        result.extend([b'/' as u16, b'/' as u16]);
        append_encoded(&mut result, authority, Component::Authority)?;
    }
    append_encoded(&mut result, path, Component::Path)?;
    if let Some(query) = query {
        result.push(b'?' as u16);
        append_encoded(&mut result, query, Component::Query)?;
    }
    if let Some(fragment) = fragment {
        result.push(b'#' as u16);
        append_encoded(&mut result, fragment, Component::Fragment)?;
    }
    Ok(result)
}

fn decode_percent_run(bytes: &[u8], output: &mut Vec<u16>) -> Result<(), SakuraUriCandidateStatus> {
    let mut index = 0;
    while index < bytes.len() {
        let decoded = decode_utf8_code_point(&bytes[index..])
            .map_err(|_| SakuraUriCandidateStatus::InvalidUtf8)?;
        let code_point = decoded.code_point;
        if code_point <= 0xffff {
            output.push(code_point as u16);
        } else {
            let adjusted = code_point - 0x10000;
            output.push(0xd800 | ((adjusted >> 10) as u16));
            output.push(0xdc00 | ((adjusted & 0x3ff) as u16));
        }
        index += decoded.width;
    }
    Ok(())
}

fn hex_value(value: u16) -> Option<u8> {
    if (b'0' as u16..=b'9' as u16).contains(&value) {
        Some((value - b'0' as u16) as u8)
    } else if (b'a' as u16..=b'f' as u16).contains(&value) {
        Some((value - b'a' as u16 + 10) as u8)
    } else if (b'A' as u16..=b'F' as u16).contains(&value) {
        Some((value - b'A' as u16 + 10) as u8)
    } else {
        None
    }
}

fn is_raw_uri_character_invalid(value: u16) -> bool {
    value <= 0x20 || value == 0x7f || value == b'\\' as u16
}

fn decode_raw_component(
    encoded: &[u16],
    component_error: SakuraUriCandidateStatus,
) -> Result<Vec<u16>, SakuraUriCandidateStatus> {
    if !is_valid_utf16(encoded) || encoded.iter().copied().any(is_raw_uri_character_invalid) {
        return Err(component_error);
    }
    let mut decoded = Vec::with_capacity(encoded.len());
    let mut index = 0;
    while index < encoded.len() {
        if encoded[index] != b'%' as u16 {
            decoded.push(encoded[index]);
            index += 1;
            continue;
        }
        let mut bytes = Vec::new();
        while index < encoded.len() && encoded[index] == b'%' as u16 {
            if index + 2 >= encoded.len() {
                return Err(SakuraUriCandidateStatus::InvalidPercentEncoding);
            }
            let high = hex_value(encoded[index + 1]);
            let low = hex_value(encoded[index + 2]);
            let (Some(high), Some(low)) = (high, low) else {
                return Err(SakuraUriCandidateStatus::InvalidPercentEncoding);
            };
            bytes.push((high << 4) | low);
            index += 3;
        }
        decode_percent_run(&bytes, &mut decoded)?;
    }
    Ok(decoded)
}

fn find_character(value: &[u16], start: usize, characters: &[u16]) -> Option<usize> {
    value[start..]
        .iter()
        .position(|character| characters.contains(character))
        .map(|offset| start + offset)
}

fn parse_uri(input: &[u16]) -> Result<UriValue, SakuraUriCandidateStatus> {
    if input.is_empty() {
        return Err(SakuraUriCandidateStatus::EmptyInput);
    }
    let scheme_end = input.iter().position(|character| *character == b':' as u16);
    let first_delimiter = find_character(input, 0, &[b'/' as u16, b'?' as u16, b'#' as u16]);
    if scheme_end.is_none()
        || first_delimiter.is_some_and(|delimiter| scheme_end.is_some_and(|end| end > delimiter))
    {
        return Err(SakuraUriCandidateStatus::MissingScheme);
    }
    let scheme_end = scheme_end.expect("checked above");
    let scheme = &input[..scheme_end];
    validate_scheme(scheme)?;

    let mut position = scheme_end + 1;
    let mut has_authority = false;
    let mut authority = Vec::new();
    if input.get(position..position + 2) == Some(&[b'/' as u16, b'/' as u16]) {
        has_authority = true;
        position += 2;
        let authority_end =
            find_character(input, position, &[b'/' as u16, b'?' as u16, b'#' as u16]);
        let raw_authority = &input[position..authority_end.unwrap_or(input.len())];
        authority =
            decode_raw_component(raw_authority, SakuraUriCandidateStatus::InvalidAuthority)?;
        position = authority_end.unwrap_or(input.len());
    }

    let query_start = input[position..]
        .iter()
        .position(|character| *character == b'?' as u16)
        .map(|offset| position + offset);
    let fragment_start = input[position..]
        .iter()
        .position(|character| *character == b'#' as u16)
        .map(|offset| position + offset);
    let path_end = query_start
        .into_iter()
        .chain(fragment_start)
        .min()
        .unwrap_or(input.len());
    let path = decode_raw_component(
        &input[position..path_end],
        SakuraUriCandidateStatus::InvalidPath,
    )?;

    let query = match (query_start, fragment_start) {
        (Some(start), Some(end)) if start < end => Some(decode_raw_component(
            &input[start + 1..end],
            SakuraUriCandidateStatus::InvalidQuery,
        )?),
        (Some(start), None) => Some(decode_raw_component(
            &input[start + 1..],
            SakuraUriCandidateStatus::InvalidQuery,
        )?),
        _ => None,
    };
    let fragment = fragment_start
        .map(|start| {
            decode_raw_component(
                &input[start + 1..],
                SakuraUriCandidateStatus::InvalidFragment,
            )
        })
        .transpose()?;
    from_components(
        scheme.to_vec(),
        authority,
        path,
        query,
        fragment,
        has_authority,
    )
}

fn from_components(
    scheme: Vec<u16>,
    authority: Vec<u16>,
    path: Vec<u16>,
    query: Option<Vec<u16>>,
    fragment: Option<Vec<u16>>,
    has_authority: bool,
) -> Result<UriValue, SakuraUriCandidateStatus> {
    validate_scheme(&scheme)?;
    validate_component(&authority, SakuraUriCandidateStatus::InvalidAuthority)?;
    if !has_authority && !authority.is_empty() {
        return Err(SakuraUriCandidateStatus::InvalidAuthority);
    }
    if has_authority && !path.is_empty() && path[0] != b'/' as u16 {
        return Err(SakuraUriCandidateStatus::InvalidPath);
    }
    validate_component(&path, SakuraUriCandidateStatus::InvalidPath)?;
    if let Some(query) = &query {
        validate_component(query, SakuraUriCandidateStatus::InvalidQuery)?;
    }
    if let Some(fragment) = &fragment {
        validate_component(fragment, SakuraUriCandidateStatus::InvalidFragment)?;
    }
    let scheme = lower_scheme(&scheme);
    let serialized = serialize(
        &scheme,
        &authority,
        &path,
        query.as_deref(),
        fragment.as_deref(),
        has_authority,
    )?;
    Ok(UriValue {
        scheme,
        authority,
        path,
        query,
        fragment,
        has_authority,
        serialized,
    })
}

fn to_u64(length: usize) -> Result<u64, SakuraUriCandidateStatus> {
    u64::try_from(length).map_err(|_| SakuraUriCandidateStatus::InternalError)
}

fn measure_value(
    value: &UriValue,
) -> Result<SakuraUriCandidateMeasureV1, SakuraUriCandidateStatus> {
    Ok(SakuraUriCandidateMeasureV1 {
        struct_size: size_of::<SakuraUriCandidateMeasureV1>() as u32,
        abi_version: URI_CANDIDATE_ABI_VERSION_V1,
        scheme_length: to_u64(value.scheme.len())?,
        authority_length: to_u64(value.authority.len())?,
        path_length: to_u64(value.path.len())?,
        query_length: to_u64(value.query.as_ref().map_or(0, Vec::len))?,
        fragment_length: to_u64(value.fragment.as_ref().map_or(0, Vec::len))?,
        serialized_length: to_u64(value.serialized.len())?,
        has_authority: u32::from(value.has_authority),
        has_query: u32::from(value.query.is_some()),
        has_fragment: u32::from(value.fragment.is_some()),
        reserved: 0,
        reserved64: [0; 2],
    })
}

fn poison_measure(output: *mut SakuraUriCandidateMeasureV1) {
    // SAFETY: The caller validated this output pointer and its writable range
    // before invoking this helper.
    unsafe {
        output.write(SakuraUriCandidateMeasureV1 {
            struct_size: size_of::<SakuraUriCandidateMeasureV1>() as u32,
            abi_version: URI_CANDIDATE_ABI_VERSION_V1,
            scheme_length: POISON_LENGTH,
            authority_length: POISON_LENGTH,
            path_length: POISON_LENGTH,
            query_length: POISON_LENGTH,
            fragment_length: POISON_LENGTH,
            serialized_length: POISON_LENGTH,
            has_authority: POISON_FLAG,
            has_query: POISON_FLAG,
            has_fragment: POISON_FLAG,
            reserved: 0,
            reserved64: [0; 2],
        });
    }
}

fn validate_measure_output(
    output: *mut SakuraUriCandidateMeasureV1,
) -> Result<(), SakuraUriCandidateStatus> {
    let Some(range) = checked_address_range(output.cast_const()) else {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    };
    // SAFETY: The address/alignment/range check above makes this a bounded ABI
    // read; the caller owns the initialized output descriptor.
    let current = unsafe { output.read() };
    if current.struct_size as usize != size_of::<SakuraUriCandidateMeasureV1>()
        || current.abi_version != URI_CANDIDATE_ABI_VERSION_V1
        || current.reserved != 0
        || current.reserved64 != [0; 2]
    {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    let _ = range;
    poison_measure(output);
    Ok(())
}

fn validate_output(
    output: *mut SakuraUriCandidateOutputV1,
) -> Result<(), SakuraUriCandidateStatus> {
    validate_measure_output(output)
}

fn validate_buffer_descriptor(
    buffer: &SakuraUriCandidateBufferV1,
) -> Result<Option<AddressRange>, SakuraUriCandidateStatus> {
    if buffer.struct_size as usize != size_of::<SakuraUriCandidateBufferV1>()
        || buffer.abi_version != URI_CANDIDATE_ABI_VERSION_V1
        || buffer.reserved != [0; 2]
    {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    let capacity =
        usize::try_from(buffer.capacity).map_err(|_| SakuraUriCandidateStatus::InvalidCapacity)?;
    if capacity == 0 {
        return Ok(None);
    }
    let range = checked_slice_range(buffer.data.cast_const(), capacity)
        .ok_or(SakuraUriCandidateStatus::InvalidArgument)?;
    Ok(Some(range))
}

fn validate_buffers(
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *const SakuraUriCandidateOutputV1,
    value: &UriValue,
) -> Result<[AddressRange; 6], SakuraUriCandidateStatus> {
    let buffers_range =
        checked_address_range(buffers).ok_or(SakuraUriCandidateStatus::InvalidArgument)?;
    let output_range =
        checked_address_range(output).ok_or(SakuraUriCandidateStatus::InvalidArgument)?;
    if overlaps(buffers_range, output_range) {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    // SAFETY: The descriptor address is bounded and the caller owns its
    // initialized bytes for the duration of this call.
    let buffers = unsafe { buffers.read() };
    if buffers.struct_size as usize != size_of::<SakuraUriCandidateBuffersV1>()
        || buffers.abi_version != URI_CANDIDATE_ABI_VERSION_V1
        || buffers.reserved64 != [0; 2]
    {
        return Err(SakuraUriCandidateStatus::InvalidArgument);
    }
    let descriptors = [
        buffers.scheme,
        buffers.authority,
        buffers.path,
        buffers.query,
        buffers.fragment,
        buffers.serialized,
    ];
    let required = [
        value.scheme.len(),
        value.authority.len(),
        value.path.len(),
        value.query.as_ref().map_or(0, Vec::len),
        value.fragment.as_ref().map_or(0, Vec::len),
        value.serialized.len(),
    ];
    let mut ranges = [AddressRange { start: 0, end: 0 }; 6];
    for (index, descriptor) in descriptors.iter().enumerate() {
        let Some(range) = validate_buffer_descriptor(descriptor)? else {
            if required[index] != 0 {
                return Err(SakuraUriCandidateStatus::InvalidCapacity);
            }
            continue;
        };
        let capacity = usize::try_from(descriptor.capacity)
            .map_err(|_| SakuraUriCandidateStatus::InvalidCapacity)?;
        if capacity < required[index] {
            return Err(SakuraUriCandidateStatus::InvalidCapacity);
        }
        if overlaps(range, output_range) || overlaps(range, buffers_range) {
            return Err(SakuraUriCandidateStatus::InvalidArgument);
        }
        ranges[index] = range;
    }
    for left in 0..ranges.len() {
        for right in left + 1..ranges.len() {
            if overlaps(ranges[left], ranges[right]) {
                return Err(SakuraUriCandidateStatus::InvalidArgument);
            }
        }
    }
    Ok(ranges)
}

unsafe fn copy_output_buffer(buffer: &SakuraUriCandidateBufferV1, input: &[u16]) {
    if input.is_empty() {
        return;
    }
    // SAFETY: `validate_buffers` checked pointer alignment, range, capacity,
    // and non-overlap before any buffer is copied.
    unsafe { ptr::copy_nonoverlapping(input.as_ptr(), buffer.data, input.len()) };
}

fn write_value(
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *mut SakuraUriCandidateOutputV1,
    value: &UriValue,
) -> SakuraUriCandidateStatus {
    let measured = match measure_value(value) {
        Ok(measured) => measured,
        Err(status) => return status,
    };
    let _ranges = match validate_buffers(buffers, output.cast_const(), value) {
        Ok(ranges) => ranges,
        Err(status) => return status,
    };
    // SAFETY: `buffers` was validated and copied by `validate_buffers`; reading
    // the descriptors again is safe, and no caller pointer is retained.
    let buffers = unsafe { buffers.read() };
    // SAFETY: All six destinations were validated before any write.  The
    // temporary `UriValue` owns each source, so no input alias can mutate it.
    unsafe {
        copy_output_buffer(&buffers.scheme, &value.scheme);
        copy_output_buffer(&buffers.authority, &value.authority);
        copy_output_buffer(&buffers.path, &value.path);
        if let Some(query) = &value.query {
            copy_output_buffer(&buffers.query, query);
        }
        if let Some(fragment) = &value.fragment {
            copy_output_buffer(&buffers.fragment, fragment);
        }
        copy_output_buffer(&buffers.serialized, &value.serialized);
    }
    // SAFETY: The output pointer was validated and remains writable.  It is
    // published only after all destination buffers were validated and filled.
    unsafe { output.write(measured) };
    SakuraUriCandidateStatus::Ok
}

fn parse_measure_impl(
    input: *const SakuraUriCandidateSpanV1,
    output: *mut SakuraUriCandidateMeasureV1,
) -> SakuraUriCandidateStatus {
    if validate_measure_output(output).is_err() {
        return SakuraUriCandidateStatus::InvalidArgument;
    }
    let Some(input_range) = checked_address_range(input) else {
        return SakuraUriCandidateStatus::InvalidArgument;
    };
    // SAFETY: The descriptor pointer is bounded and points to initialized
    // caller-owned bytes for this call.
    let input = unsafe { input.read() };
    let value = match copy_span(&input).and_then(|input| parse_uri(&input)) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let measured = match measure_value(&value) {
        Ok(measured) => measured,
        Err(status) => return status,
    };
    // SAFETY: The output pointer was validated above and remains writable.
    unsafe { output.write(measured) };
    let _ = input_range;
    SakuraUriCandidateStatus::Ok
}

fn from_components_measure_impl(
    input: *const SakuraUriCandidateComponentsV1,
    output: *mut SakuraUriCandidateMeasureV1,
) -> SakuraUriCandidateStatus {
    if validate_measure_output(output).is_err() {
        return SakuraUriCandidateStatus::InvalidArgument;
    }
    let Some(input_range) = checked_address_range(input) else {
        return SakuraUriCandidateStatus::InvalidArgument;
    };
    // SAFETY: The descriptor pointer is bounded and points to initialized
    // caller-owned bytes for this call.
    let input = unsafe { input.read() };
    let components = match copy_components(&input) {
        Ok(components) => components,
        Err(status) => return status,
    };
    let value = match from_components(
        components.scheme,
        components.authority,
        components.path,
        components.query,
        components.fragment,
        components.has_authority,
    ) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let measured = match measure_value(&value) {
        Ok(measured) => measured,
        Err(status) => return status,
    };
    // SAFETY: The output pointer was validated above and remains writable.
    unsafe { output.write(measured) };
    let _ = input_range;
    SakuraUriCandidateStatus::Ok
}

fn parse_write_impl(
    input: *const SakuraUriCandidateSpanV1,
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *mut SakuraUriCandidateOutputV1,
) -> SakuraUriCandidateStatus {
    if validate_output(output).is_err() {
        return SakuraUriCandidateStatus::InvalidArgument;
    }
    let Some(input_range) = checked_address_range(input) else {
        return SakuraUriCandidateStatus::InvalidArgument;
    };
    // SAFETY: The descriptor pointer is bounded and points to initialized
    // caller-owned bytes for this call.
    let input = unsafe { input.read() };
    let value = match copy_span(&input).and_then(|input| parse_uri(&input)) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let status = write_value(buffers, output, &value);
    let _ = input_range;
    status
}

fn from_components_write_impl(
    input: *const SakuraUriCandidateComponentsV1,
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *mut SakuraUriCandidateOutputV1,
) -> SakuraUriCandidateStatus {
    if validate_output(output).is_err() {
        return SakuraUriCandidateStatus::InvalidArgument;
    }
    let Some(input_range) = checked_address_range(input) else {
        return SakuraUriCandidateStatus::InvalidArgument;
    };
    // SAFETY: The descriptor pointer is bounded and points to initialized
    // caller-owned bytes for this call.
    let input = unsafe { input.read() };
    let components = match copy_components(&input) {
        Ok(components) => components,
        Err(status) => return status,
    };
    let value = match from_components(
        components.scheme,
        components.authority,
        components.path,
        components.query,
        components.fragment,
        components.has_authority,
    ) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let status = write_value(buffers, output, &value);
    let _ = input_range;
    status
}

/// Measures a URI parse result without retaining the input pointer.
///
/// # Safety
///
/// `input` and `output` must point to initialized, caller-owned descriptors
/// for the duration of the call. Their exact V1 headers are validated; the
/// nested input span is copied before the function returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_uri_candidate_parse_measure_v1(
    input: *const SakuraUriCandidateSpanV1,
    output: *mut SakuraUriCandidateMeasureV1,
) -> SakuraUriCandidateStatus {
    catch_status(|| parse_measure_impl(input, output))
}

/// Writes a measured URI parse result into caller-owned UTF-16 buffers.
///
/// # Safety
///
/// `input`, `buffers`, and `output` must point to initialized caller-owned V1
/// descriptors. All input and output spans are validated for nullability,
/// alignment, byte-size and address overflow. No pointer is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_uri_candidate_parse_write_v1(
    input: *const SakuraUriCandidateSpanV1,
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *mut SakuraUriCandidateOutputV1,
) -> SakuraUriCandidateStatus {
    catch_status(|| parse_write_impl(input, buffers, output))
}

/// Measures a validated decoded-component URI construction.
///
/// # Safety
///
/// `input` and `output` must point to initialized, caller-owned V1 descriptors
/// for the duration of the call. Every component is copied before return.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_uri_candidate_from_components_measure_v1(
    input: *const SakuraUriCandidateComponentsV1,
    output: *mut SakuraUriCandidateMeasureV1,
) -> SakuraUriCandidateStatus {
    catch_status(|| from_components_measure_impl(input, output))
}

/// Writes a validated decoded-component URI construction into owned buffers.
///
/// # Safety
///
/// `input`, `buffers`, and `output` must point to initialized caller-owned V1
/// descriptors. All spans and capacities are validated before any destination
/// is written, and no pointer is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_uri_candidate_from_components_write_v1(
    input: *const SakuraUriCandidateComponentsV1,
    buffers: *const SakuraUriCandidateBuffersV1,
    output: *mut SakuraUriCandidateOutputV1,
) -> SakuraUriCandidateStatus {
    catch_status(|| from_components_write_impl(input, buffers, output))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn span(value: &[u16]) -> SakuraUriCandidateSpanV1 {
        SakuraUriCandidateSpanV1 {
            struct_size: size_of::<SakuraUriCandidateSpanV1>() as u32,
            abi_version: URI_CANDIDATE_ABI_VERSION_V1,
            data: value.as_ptr(),
            length: value.len() as u64,
            reserved: [0; 2],
        }
    }

    fn valid_output() -> SakuraUriCandidateMeasureV1 {
        SakuraUriCandidateMeasureV1 {
            struct_size: size_of::<SakuraUriCandidateMeasureV1>() as u32,
            abi_version: URI_CANDIDATE_ABI_VERSION_V1,
            scheme_length: 0,
            authority_length: 0,
            path_length: 0,
            query_length: 0,
            fragment_length: 0,
            serialized_length: 0,
            has_authority: 0,
            has_query: 0,
            has_fragment: 0,
            reserved: 0,
            reserved64: [0; 2],
        }
    }

    #[test]
    fn strict_decode_rejects_overlong_and_cesu8() {
        for text in [
            b"demo://host/%C0%AF".as_slice(),
            b"demo://host/%ED%A0%80".as_slice(),
        ] {
            let utf16: Vec<u16> = text.iter().map(|byte| u16::from(*byte)).collect();
            assert_eq!(
                SakuraUriCandidateStatus::InvalidUtf8,
                parse_uri(&utf16).unwrap_err()
            );
        }
    }

    #[test]
    fn abi_layout_matches_the_native_header() {
        assert_eq!(40, size_of::<SakuraUriCandidateSpanV1>());
        assert_eq!(240, size_of::<SakuraUriCandidateComponentsV1>());
        assert_eq!(40, size_of::<SakuraUriCandidateBufferV1>());
        assert_eq!(264, size_of::<SakuraUriCandidateBuffersV1>());
        assert_eq!(88, size_of::<SakuraUriCandidateMeasureV1>());
    }

    #[test]
    fn invalid_span_and_output_descriptors_fail_closed() {
        let mut output = valid_output();
        let null_input = SakuraUriCandidateSpanV1 {
            struct_size: size_of::<SakuraUriCandidateSpanV1>() as u32,
            abi_version: URI_CANDIDATE_ABI_VERSION_V1,
            data: ptr::null(),
            length: 1,
            reserved: [0; 2],
        };
        // SAFETY: The descriptor itself and output are live; the null span is
        // rejected before a slice is created.
        let status = unsafe { sakura_uri_candidate_parse_measure_v1(&null_input, &mut output) };
        assert_eq!(SakuraUriCandidateStatus::InvalidArgument, status);
        assert_eq!(POISON_LENGTH, output.serialized_length);

        let mut overflow_input = null_input;
        overflow_input.data = std::ptr::NonNull::<u16>::dangling().as_ptr();
        overflow_input.length = u64::MAX;
        output = valid_output();
        // SAFETY: Address arithmetic rejects the impossible length before the
        // dangling sentinel can be dereferenced.
        let status = unsafe { sakura_uri_candidate_parse_measure_v1(&overflow_input, &mut output) };
        assert_eq!(SakuraUriCandidateStatus::InvalidArgument, status);
        assert_eq!(POISON_LENGTH, output.path_length);

        let mut misaligned_input = null_input;
        misaligned_input.data = std::ptr::without_provenance::<u16>(1);
        output = valid_output();
        // SAFETY: Alignment validation rejects this sentinel address before
        // any memory access through it.
        let status =
            unsafe { sakura_uri_candidate_parse_measure_v1(&misaligned_input, &mut output) };
        assert_eq!(SakuraUriCandidateStatus::InvalidArgument, status);

        let valid_input_data: Vec<u16> = "demo:path".encode_utf16().collect();
        let valid_input = span(&valid_input_data);
        output = valid_output();
        output.abi_version += 1;
        output.scheme_length = 77;
        // SAFETY: Both descriptors point to live initialized storage. The bad
        // output header is rejected without treating it as a writable result.
        let status = unsafe { sakura_uri_candidate_parse_measure_v1(&valid_input, &mut output) };
        assert_eq!(SakuraUriCandidateStatus::InvalidArgument, status);
        assert_eq!(77, output.scheme_length);
    }

    #[test]
    fn parse_and_component_encode_preserve_presence_and_serialization() {
        let input: Vec<u16> = "DeMo://Example.Test/a%20b/%C3%A9?x=%2F#%F0%90%90%80"
            .encode_utf16()
            .collect();
        let value = parse_uri(&input).expect("parse");
        assert_eq!(value.scheme, "demo".encode_utf16().collect::<Vec<_>>());
        assert_eq!(
            value.query.as_deref(),
            Some("x=/".encode_utf16().collect::<Vec<_>>().as_slice())
        );
        assert_eq!(
            value.serialized,
            "demo://Example.Test/a%20b/%C3%A9?x=/#%F0%90%90%80"
                .encode_utf16()
                .collect::<Vec<_>>()
        );

        let authority = "\u{0130}".encode_utf16().collect::<Vec<_>>();
        let path = "/a \u{00e9}?#".encode_utf16().collect::<Vec<_>>();
        let query = "q= ?".encode_utf16().collect::<Vec<_>>();
        let fragment = "f# \u{00e9}".encode_utf16().collect::<Vec<_>>();
        let value = from_components(
            "demo".encode_utf16().collect(),
            authority,
            path,
            Some(query),
            Some(fragment),
            true,
        )
        .expect("components");
        assert_eq!(
            value.serialized,
            "demo://%C4%B0/a%20%C3%A9%3F%23?q=%20?#f%23%20%C3%A9"
                .encode_utf16()
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn output_is_poisoned_on_malformed_input() {
        let input: Vec<u16> = b"demo://host/%ZZ"
            .iter()
            .map(|byte| u16::from(*byte))
            .collect();
        let input = span(&input);
        let mut output = valid_output();
        output.scheme_length = 77;
        // SAFETY: Test descriptors point to live local storage.
        let status = unsafe { sakura_uri_candidate_parse_measure_v1(&input, &mut output) };
        assert_eq!(SakuraUriCandidateStatus::InvalidPercentEncoding, status);
        assert_eq!(POISON_LENGTH, output.scheme_length);
        assert_eq!(POISON_FLAG, output.has_query);
    }
}
