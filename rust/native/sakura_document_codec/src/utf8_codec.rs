//! Comparison-only one-shot conversion between UTF-8 or CESU-8 bytes and UTF-16 code units.
//!
//! This reproduces what the C++ converters `CUtf8::Utf8ToUni` and `CUtf8::UniToUtf8` do with one
//! whole buffer, loss policy included, so that the two can be compared. The policy is frozen as
//! rows in `rust/native/fixtures/utf8-codec-corpus.tsv`:
//!
//! * Decoding never loses input. A byte that does not start a complete valid sequence becomes the
//!   binary unit `0xDC00 + byte` and the scan resumes at the next byte. UTF-8 reads exactly the
//!   strict sequences of `sakura-unicode-core`. CESU-8 reads the same one-, two- and three-byte
//!   sequences, never a four-byte form, and a surrogate pair only as a three-byte high half
//!   directly followed by a three-byte low half.
//! * Encoding writes a surrogate pair as one four-byte form (UTF-8) or as two three-byte forms
//!   (CESU-8) and every other unit as the form of its value. A lone unit in `U+DC00..=U+DCFF` is
//!   written as its low eight bits, which restores a binary byte. Any other lone surrogate is
//!   written as `?` and makes the result lossy.
//!
//! Each direction has a measure call, which returns the exact output length, and a write call,
//! which checks the capacity first and so writes either the whole output or nothing. Decoding
//! never produces more units than it has bytes, and encoding never produces more than three bytes
//! per unit. Nothing allocates, and no input makes a call panic: every range is checked before it
//! is sliced.

use core::ops::RangeInclusive;
use sakura_unicode_core::{decode_code_point, encode_code_point};

/// The first binary unit: a byte `b` that starts no valid sequence is read as `0xDC00 + b`.
const BINARY_BASE: u16 = 0xDC00;
/// What an unpaired surrogate outside `U+DC00..=U+DCFF` is written as.
const REPLACEMENT: u8 = b'?';
/// The bytes of one CESU-8 surrogate pair: a three-byte high half and a three-byte low half.
const CESU8_PAIR_BYTES: usize = 6;
/// The most bytes one encoding step writes: the two three-byte forms of a CESU-8 pair.
const MAX_STEP_BYTES: usize = 6;

/// The byte form of a conversion, numbered like the C++ `ECodeType` values.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Utf8Variant {
    /// UTF-8 (`CODE_UTF8`): a supplementary scalar value is one four-byte form.
    Utf8 = 4,
    /// CESU-8 (`CODE_CESU8`): a supplementary scalar value is two three-byte surrogate forms.
    Cesu8 = 7,
}

impl Utf8Variant {
    /// Returns the variant for a wire number, or `None` for any other value.
    pub const fn from_wire(value: u32) -> Option<Self> {
        match value {
            4 => Some(Self::Utf8),
            7 => Some(Self::Cesu8),
            _ => None,
        }
    }
}

/// A write was refused because the destination cannot hold the whole output. Nothing was written.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OutputTooSmall {
    /// The number of elements the output needs.
    pub required: usize,
}

/// The outcome of an encoding: the length of the output and whether it lost information.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Utf8Encoded {
    /// The number of bytes of the output.
    pub length: usize,
    /// Whether an unpaired surrogate outside `U+DC00..=U+DCFF` was written as `?`.
    pub lossy: bool,
}

/// The destination of a scan. It stores what fits in `slots` and counts everything pushed, so a scan
/// into an empty slice measures and a scan into a slice of the measured length writes.
struct Output<'a, T> {
    slots: &'a mut [T],
    count: usize,
}

impl<'a, T> Output<'a, T> {
    fn new(slots: &'a mut [T]) -> Self {
        Self { slots, count: 0 }
    }

    fn push(&mut self, element: T) {
        if let Some(slot) = self.slots.get_mut(self.count) {
            *slot = element;
        }
        self.count += 1;
    }
}

/// One step of decoding: one or two units and the number of bytes they were read from.
struct DecodeStep {
    units: [u16; 2],
    unit_count: usize,
    consumed: usize,
}

impl DecodeStep {
    fn one(unit: u16, consumed: usize) -> Self {
        Self {
            units: [unit, 0],
            unit_count: 1,
            consumed,
        }
    }

    fn two(high: u16, low: u16, consumed: usize) -> Self {
        Self {
            units: [high, low],
            unit_count: 2,
            consumed,
        }
    }

    fn units(&self) -> &[u16] {
        &self.units[..self.unit_count]
    }
}

/// Reads the unit or pair of units that starts `rest`, whose first byte is `first`.
fn decode_step(first: u8, rest: &[u8], variant: Utf8Variant) -> DecodeStep {
    if variant == Utf8Variant::Cesu8 {
        if let Some((high, low)) = cesu8_pair(rest) {
            return DecodeStep::two(high, low, CESU8_PAIR_BYTES);
        }
    }
    match decode_code_point(rest) {
        // CESU-8 has no four-byte form, so the lead byte of one is a binary byte there.
        Ok(decoded) if decoded.width < 4 || variant == Utf8Variant::Utf8 => {
            scalar_units(decoded.code_point, decoded.width)
        }
        _ => DecodeStep::one(BINARY_BASE + u16::from(first), 1),
    }
}

/// The UTF-16 units of a scalar value read from `consumed` bytes.
fn scalar_units(code_point: u32, consumed: usize) -> DecodeStep {
    match code_point.checked_sub(0x1_0000) {
        // A scalar value below U+10000 fits in one unit exactly.
        None => DecodeStep::one(code_point as u16, consumed),
        Some(offset) => DecodeStep::two(
            0xD800 + (offset >> 10) as u16,
            0xDC00 + (offset & 0x3FF) as u16,
            consumed,
        ),
    }
}

/// Reads a surrogate pair written as two three-byte forms, high half first.
fn cesu8_pair(rest: &[u8]) -> Option<(u16, u16)> {
    let (high, after_high) = rest.split_first_chunk::<3>()?;
    let (low, _) = after_high.split_first_chunk::<3>()?;
    Some((
        surrogate_unit(high, 0xA0..=0xAF)?,
        surrogate_unit(low, 0xB0..=0xBF)?,
    ))
}

/// The unit of the three-byte surrogate form `ED middle tail` whose middle byte lies in `middles`:
/// `A0..=AF` for a high surrogate and `B0..=BF` for a low one.
fn surrogate_unit(form: &[u8; 3], middles: RangeInclusive<u8>) -> Option<u16> {
    match *form {
        [0xED, middle, tail @ 0x80..=0xBF] if middles.contains(&middle) => {
            Some(0xD000 | (u16::from(middle & 0x3F) << 6) | u16::from(tail & 0x3F))
        }
        _ => None,
    }
}

/// One step of encoding: the bytes of one unit or pair, the units they came from, and whether a
/// unit was replaced.
struct EncodeStep {
    bytes: [u8; MAX_STEP_BYTES],
    byte_count: usize,
    consumed: usize,
    lossy: bool,
}

impl EncodeStep {
    fn replacement(consumed: usize) -> Self {
        Self {
            bytes: [REPLACEMENT, 0, 0, 0, 0, 0],
            byte_count: 1,
            consumed,
            lossy: true,
        }
    }

    fn binary(byte: u8) -> Self {
        Self {
            bytes: [byte, 0, 0, 0, 0, 0],
            byte_count: 1,
            consumed: 1,
            lossy: false,
        }
    }

    fn bytes(&self) -> &[u8] {
        &self.bytes[..self.byte_count]
    }
}

/// Writes the unit `first`, which is followed by `tail`.
fn encode_step(first: u16, tail: &[u16], variant: Utf8Variant) -> EncodeStep {
    match (first, tail.first()) {
        (0xD800..=0xDBFF, Some(&low @ 0xDC00..=0xDFFF)) => pair_step(first, low, variant),
        (0xDC00..=0xDCFF, _) => {
            let [low_byte, _] = first.to_le_bytes();
            EncodeStep::binary(low_byte)
        }
        // A scalar value on its own, or an unpaired surrogate, which `scalar_step` replaces.
        _ => scalar_step(u32::from(first), 1),
    }
}

/// Writes a high and a low surrogate as one scalar value or as two surrogate forms.
fn pair_step(high: u16, low: u16, variant: Utf8Variant) -> EncodeStep {
    match variant {
        Utf8Variant::Utf8 => {
            let scalar = 0x1_0000 + ((u32::from(high) - 0xD800) << 10) + (u32::from(low) - 0xDC00);
            scalar_step(scalar, 2)
        }
        Utf8Variant::Cesu8 => {
            let [high_0, high_1, high_2] = three_byte_form(high);
            let [low_0, low_1, low_2] = three_byte_form(low);
            EncodeStep {
                bytes: [high_0, high_1, high_2, low_0, low_1, low_2],
                byte_count: CESU8_PAIR_BYTES,
                consumed: 2,
                lossy: false,
            }
        }
    }
}

/// Writes a scalar value that came from `consumed` units. The strict encoder refuses a value that
/// is not a scalar value, and here only an unpaired surrogate can be one (the buffer holds the
/// longest form and no value is above `U+10FFFF`), so a refusal is written as `?` and is lossy.
fn scalar_step(scalar: u32, consumed: usize) -> EncodeStep {
    let mut bytes = [0; MAX_STEP_BYTES];
    match encode_code_point(scalar, &mut bytes) {
        Ok(byte_count) => EncodeStep {
            bytes,
            byte_count,
            consumed,
            lossy: false,
        },
        Err(_) => EncodeStep::replacement(consumed),
    }
}

/// The three-byte form of a UTF-16 unit, whatever its value.
fn three_byte_form(unit: u16) -> [u8; 3] {
    [
        0xE0 | (unit >> 12) as u8,
        0x80 | ((unit >> 6) & 0x3F) as u8,
        0x80 | (unit & 0x3F) as u8,
    ]
}

fn scan_decode(bytes: &[u8], variant: Utf8Variant, output: &mut Output<'_, u16>) {
    let mut rest = bytes;
    while let Some(&first) = rest.first() {
        let step = decode_step(first, rest, variant);
        for &unit in step.units() {
            output.push(unit);
        }
        // A step never consumes more than `rest` holds.
        rest = &rest[step.consumed..];
    }
}

fn scan_encode(units: &[u16], variant: Utf8Variant, output: &mut Output<'_, u8>) -> bool {
    let mut lossy = false;
    let mut rest = units;
    while let Some((&first, tail)) = rest.split_first() {
        let step = encode_step(first, tail, variant);
        for &byte in step.bytes() {
            output.push(byte);
        }
        lossy |= step.lossy;
        // A step consumes the unit it starts at and, for a pair, the unit in `tail` after it.
        rest = &rest[step.consumed..];
    }
    lossy
}

/// Returns how many UTF-16 units `bytes` decode to.
pub fn utf8_decode_measure(bytes: &[u8], variant: Utf8Variant) -> usize {
    let mut output = Output::new(&mut []);
    scan_decode(bytes, variant, &mut output);
    output.count
}

/// Decodes `bytes` into the start of `units` and returns the number of units written.
///
/// When `units` is shorter than [`utf8_decode_measure`] reports, nothing is written and the error
/// carries the required length. A longer `units` keeps its unused suffix unchanged.
pub fn utf8_decode_write(
    bytes: &[u8],
    variant: Utf8Variant,
    units: &mut [u16],
) -> Result<usize, OutputTooSmall> {
    let required = utf8_decode_measure(bytes, variant);
    if units.len() < required {
        return Err(OutputTooSmall { required });
    }
    let (target, _unused) = units.split_at_mut(required);
    scan_decode(bytes, variant, &mut Output::new(target));
    Ok(required)
}

/// Returns how many bytes `units` encode to and whether the encoding loses information.
pub fn utf8_encode_measure(units: &[u16], variant: Utf8Variant) -> Utf8Encoded {
    let mut output = Output::new(&mut []);
    let lossy = scan_encode(units, variant, &mut output);
    Utf8Encoded {
        length: output.count,
        lossy,
    }
}

/// Encodes `units` into the start of `bytes` and returns the length and loss of the output.
///
/// When `bytes` is shorter than [`utf8_encode_measure`] reports, nothing is written and the error
/// carries the required length. A longer `bytes` keeps its unused suffix unchanged.
pub fn utf8_encode_write(
    units: &[u16],
    variant: Utf8Variant,
    bytes: &mut [u8],
) -> Result<Utf8Encoded, OutputTooSmall> {
    let measured = utf8_encode_measure(units, variant);
    if bytes.len() < measured.length {
        return Err(OutputTooSmall {
            required: measured.length,
        });
    }
    let (target, _unused) = bytes.split_at_mut(measured.length);
    scan_encode(units, variant, &mut Output::new(target));
    Ok(measured)
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use std::vec;
    use std::vec::Vec;

    const BOTH: [Utf8Variant; 2] = [Utf8Variant::Utf8, Utf8Variant::Cesu8];

    fn decode(bytes: &[u8], variant: Utf8Variant) -> Vec<u16> {
        let mut units = vec![0; utf8_decode_measure(bytes, variant)];
        assert_eq!(
            utf8_decode_write(bytes, variant, &mut units),
            Ok(units.len())
        );
        units
    }

    fn encode(units: &[u16], variant: Utf8Variant) -> (Vec<u8>, bool) {
        let measured = utf8_encode_measure(units, variant);
        let mut bytes = vec![0; measured.length];
        assert_eq!(utf8_encode_write(units, variant, &mut bytes), Ok(measured));
        (bytes, measured.lossy)
    }

    #[test]
    fn variants_are_numbered_like_the_cpp_code_types() {
        assert_eq!(Utf8Variant::Utf8 as u32, 4);
        assert_eq!(Utf8Variant::Cesu8 as u32, 7);
        for value in (0..=0x100).chain([u32::MAX - 1, u32::MAX]) {
            let expected = match value {
                4 => Some(Utf8Variant::Utf8),
                7 => Some(Utf8Variant::Cesu8),
                _ => None,
            };
            assert_eq!(
                Utf8Variant::from_wire(value),
                expected,
                "wire value {value}"
            );
        }
    }

    #[test]
    fn output_stores_what_fits_and_counts_everything() {
        let mut slots = [0u8; 2];
        let mut output = Output::new(&mut slots);
        for element in [7, 8, 9, 10] {
            output.push(element);
        }
        assert_eq!(output.count, 4);
        assert_eq!(slots, [7, 8]);

        let mut nothing: [u16; 0] = [];
        let mut empty = Output::new(&mut nothing);
        empty.push(1);
        assert_eq!(empty.count, 1);
    }

    #[test]
    fn decoding_has_literal_results_in_both_variants() {
        // A supplementary scalar is a pair in UTF-8 and four binary units in CESU-8.
        let emoji = [0xF0, 0x9F, 0x98, 0x80];
        assert_eq!(decode(&emoji, Utf8Variant::Utf8), [0xD83D, 0xDE00]);
        assert_eq!(
            decode(&emoji, Utf8Variant::Cesu8),
            [0xDCF0, 0xDC9F, 0xDC98, 0xDC80]
        );
        // Two three-byte surrogate forms are a pair in CESU-8 and six binary units in UTF-8.
        let pair = [0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80];
        assert_eq!(decode(&pair, Utf8Variant::Cesu8), [0xD83D, 0xDE00]);
        assert_eq!(
            decode(&pair, Utf8Variant::Utf8),
            [0xDCED, 0xDCA0, 0xDCBD, 0xDCED, 0xDCB8, 0xDC80]
        );
        for variant in BOTH {
            assert_eq!(decode(&[], variant), []);
            assert_eq!(decode(b"A\0z", variant), [0x41, 0, 0x7A]);
            assert_eq!(decode(&[0xE3, 0x81, 0x82], variant), [0x3042]);
            // A truncated sequence resumes at the next byte.
            assert_eq!(decode(&[0xE3, 0x81, 0x41], variant), [0xDCE3, 0xDC81, 0x41]);
        }
    }

    #[test]
    fn encoding_has_literal_results_in_both_variants() {
        let pair = [0xD83D, 0xDE00];
        assert_eq!(
            encode(&pair, Utf8Variant::Utf8),
            (vec![0xF0, 0x9F, 0x98, 0x80], false)
        );
        assert_eq!(
            encode(&pair, Utf8Variant::Cesu8),
            (vec![0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80], false)
        );
        for variant in BOTH {
            assert_eq!(encode(&[], variant), (vec![], false));
            assert_eq!(
                encode(&[0x41, 0xE9, 0x3042], variant),
                (vec![0x41, 0xC3, 0xA9, 0xE3, 0x81, 0x82], false)
            );
            // A binary unit restores its byte; any other lone surrogate is `?` and lossy.
            assert_eq!(encode(&[0xDC80], variant), (vec![0x80], false));
            assert_eq!(encode(&[0xD800, 0x41], variant), (vec![b'?', 0x41], true));
            assert_eq!(encode(&[0xDD00], variant), (vec![b'?'], true));
        }
    }

    #[test]
    fn a_write_with_too_little_room_changes_nothing() {
        const SENTINEL: u16 = 0xA5A5;
        let bytes = [0x41, 0xE3, 0x81, 0x82];
        for variant in BOTH {
            let mut units = [SENTINEL; 8];
            assert_eq!(
                utf8_decode_write(&bytes, variant, &mut units[..1]),
                Err(OutputTooSmall { required: 2 })
            );
            assert_eq!(units, [SENTINEL; 8]);
            // Exactly enough room writes the output and leaves the rest alone.
            assert_eq!(utf8_decode_write(&bytes, variant, &mut units), Ok(2));
            assert_eq!(units[..3], [0x41, 0x3042, SENTINEL]);

            let mut out = [0xA5u8; 8];
            let units = [0x41, 0x3042];
            assert_eq!(
                utf8_encode_write(&units, variant, &mut out[..3]),
                Err(OutputTooSmall { required: 4 })
            );
            assert_eq!(out, [0xA5; 8]);
            assert_eq!(
                utf8_encode_write(&units, variant, &mut out),
                Ok(Utf8Encoded {
                    length: 4,
                    lossy: false
                })
            );
            assert_eq!(out, [0x41, 0xE3, 0x81, 0x82, 0xA5, 0xA5, 0xA5, 0xA5]);
        }
    }
}
