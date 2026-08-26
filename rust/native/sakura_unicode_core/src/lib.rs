#![no_std]
#![deny(clippy::all)]
#![deny(missing_docs)]

//! Strict, allocation-free UTF-8 primitives for Unicode scalar values.
//!
//! This crate deliberately implements only the Unicode scalar-value UTF-8
//! contract. It does not decode CESU-8, preserve binary surrogate code units,
//! replace malformed input, or perform JSON string escaping. Those are
//! separate policies and must be selected by their owning subsystem.

/// The largest number of bytes used by one strict UTF-8 scalar value.
pub const MAX_UTF8_BYTES: usize = 4;

/// The reason a strict UTF-8 operation was rejected.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Utf8ErrorKind {
    /// The input ended before the current scalar value was complete.
    UnexpectedEnd,
    /// The first byte is not a legal strict UTF-8 lead byte.
    InvalidLeadByte,
    /// A byte following a lead byte is not a continuation byte.
    InvalidContinuationByte,
    /// The byte sequence uses more bytes than the scalar value requires.
    OverlongEncoding,
    /// The decoded value is in the UTF-16 surrogate range.
    SurrogateCodePoint,
    /// The decoded value is greater than U+10FFFF.
    CodePointOutOfRange,
    /// The destination supplied to the encoder is too short.
    OutputTooSmall,
}

/// A strict UTF-8 error with the byte offset at which the error was detected.
///
/// The offset is relative to the byte slice passed to the operation. For an
/// error caused by a missing byte, it is equal to the slice length. For a
/// validation error in a larger input, [`validate_utf8`] reports the absolute
/// offset in that larger input.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Utf8Error {
    /// The category of the validation or encoding failure.
    pub kind: Utf8ErrorKind,
    /// The byte offset associated with the failure.
    pub offset: usize,
}

impl Utf8Error {
    const fn new(kind: Utf8ErrorKind, offset: usize) -> Self {
        Self { kind, offset }
    }

    fn shifted(self, base: usize) -> Self {
        Self {
            kind: self.kind,
            offset: base.saturating_add(self.offset),
        }
    }
}

/// The result of decoding one scalar value from the beginning of a byte slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DecodedCodePoint {
    /// The Unicode scalar value as a `u32`.
    pub code_point: u32,
    /// The number of bytes consumed by the scalar value.
    pub width: usize,
}

/// Returns whether a value is a Unicode scalar value.
///
/// UTF-16 surrogate code points are intentionally excluded even though they
/// are Unicode code points in the broad terminology used by some APIs.
pub const fn is_scalar_value(value: u32) -> bool {
    value <= 0x10ffff && (value < 0xd800 || value > 0xdfff)
}

/// Validates a Unicode scalar value without allocating or changing state.
pub const fn validate_scalar_value(value: u32) -> Result<(), Utf8Error> {
    if value > 0x10ffff {
        Err(Utf8Error::new(Utf8ErrorKind::CodePointOutOfRange, 0))
    } else if value >= 0xd800 && value <= 0xdfff {
        Err(Utf8Error::new(Utf8ErrorKind::SurrogateCodePoint, 0))
    } else {
        Ok(())
    }
}

/// Returns the strict UTF-8 width for a Unicode scalar value.
pub const fn encoded_len(code_point: u32) -> Result<usize, Utf8Error> {
    match validate_scalar_value(code_point) {
        Ok(()) if code_point <= 0x7f => Ok(1),
        Ok(()) if code_point <= 0x7ff => Ok(2),
        Ok(()) if code_point <= 0xffff => Ok(3),
        Ok(()) => Ok(4),
        Err(error) => Err(error),
    }
}

#[inline]
fn unexpected_end(length: usize) -> Utf8Error {
    Utf8Error::new(Utf8ErrorKind::UnexpectedEnd, length)
}

#[inline]
fn continuation_byte(input: &[u8], index: usize) -> Result<u8, Utf8Error> {
    let byte = match input.get(index) {
        Some(&byte) => byte,
        None => return Err(unexpected_end(input.len())),
    };
    if byte & 0xc0 != 0x80 {
        Err(Utf8Error::new(
            Utf8ErrorKind::InvalidContinuationByte,
            index,
        ))
    } else {
        Ok(byte)
    }
}

/// Decodes one strict UTF-8 scalar value from the beginning of `input`.
///
/// The function consumes exactly one scalar value and does not require the
/// slice to end after that value. Use [`validate_utf8`] when the entire slice
/// must be valid. Surrogates, overlong encodings, invalid lead bytes, invalid
/// continuation bytes, and code points above U+10FFFF are rejected.
pub fn decode_code_point(input: &[u8]) -> Result<DecodedCodePoint, Utf8Error> {
    let first = match input.first() {
        Some(&byte) => byte,
        None => return Err(unexpected_end(0)),
    };

    match first {
        0x00..=0x7f => Ok(DecodedCodePoint {
            code_point: first as u32,
            width: 1,
        }),
        0xc2..=0xdf => {
            let second = continuation_byte(input, 1)?;
            let code_point = ((first as u32 & 0x1f) << 6) | (second as u32 & 0x3f);
            Ok(DecodedCodePoint {
                code_point,
                width: 2,
            })
        }
        0xe0..=0xef => {
            let second = continuation_byte(input, 1)?;
            let second_payload = second & 0x3f;
            if first == 0xe0 && second_payload < 0x20 {
                return Err(Utf8Error::new(Utf8ErrorKind::OverlongEncoding, 1));
            }
            if first == 0xed && second_payload >= 0x20 {
                return Err(Utf8Error::new(Utf8ErrorKind::SurrogateCodePoint, 1));
            }
            let third = continuation_byte(input, 2)?;
            let code_point = ((first as u32 & 0x0f) << 12)
                | ((second as u32 & 0x3f) << 6)
                | (third as u32 & 0x3f);
            Ok(DecodedCodePoint {
                code_point,
                width: 3,
            })
        }
        0xf0..=0xf4 => {
            let second = continuation_byte(input, 1)?;
            let second_payload = second & 0x3f;
            if first == 0xf0 && second_payload < 0x10 {
                return Err(Utf8Error::new(Utf8ErrorKind::OverlongEncoding, 1));
            }
            if first == 0xf4 && second_payload > 0x0f {
                return Err(Utf8Error::new(Utf8ErrorKind::CodePointOutOfRange, 1));
            }
            let third = continuation_byte(input, 2)?;
            let fourth = continuation_byte(input, 3)?;
            let code_point = ((first as u32 & 0x07) << 18)
                | ((second as u32 & 0x3f) << 12)
                | ((third as u32 & 0x3f) << 6)
                | (fourth as u32 & 0x3f);
            Ok(DecodedCodePoint {
                code_point,
                width: 4,
            })
        }
        _ => Err(Utf8Error::new(Utf8ErrorKind::InvalidLeadByte, 0)),
    }
}

/// Validates every byte in a strict UTF-8 slice.
///
/// On failure, [`Utf8Error::offset`] is absolute relative to `input`, not
/// relative to the scalar value currently being decoded.
pub fn validate_utf8(input: &[u8]) -> Result<(), Utf8Error> {
    let mut offset = 0usize;
    while offset < input.len() {
        let decoded = match decode_code_point(&input[offset..]) {
            Ok(decoded) => decoded,
            Err(error) => return Err(error.shifted(offset)),
        };
        offset = offset.saturating_add(decoded.width);
    }
    Ok(())
}

/// Encodes one Unicode scalar value into the beginning of `output`.
///
/// The return value is the number of bytes written. The function reports an
/// invalid scalar before checking output capacity, so malformed Unicode is
/// never hidden by a short destination buffer.
pub fn encode_code_point(code_point: u32, output: &mut [u8]) -> Result<usize, Utf8Error> {
    let width = encoded_len(code_point)?;
    if output.len() < width {
        return Err(Utf8Error::new(Utf8ErrorKind::OutputTooSmall, 0));
    }

    match width {
        1 => {
            output[0] = code_point as u8;
        }
        2 => {
            output[0] = 0xc0 | ((code_point >> 6) as u8);
            output[1] = 0x80 | (code_point as u8 & 0x3f);
        }
        3 => {
            output[0] = 0xe0 | ((code_point >> 12) as u8);
            output[1] = 0x80 | ((code_point >> 6) as u8 & 0x3f);
            output[2] = 0x80 | (code_point as u8 & 0x3f);
        }
        4 => {
            output[0] = 0xf0 | ((code_point >> 18) as u8);
            output[1] = 0x80 | ((code_point >> 12) as u8 & 0x3f);
            output[2] = 0x80 | ((code_point >> 6) as u8 & 0x3f);
            output[3] = 0x80 | (code_point as u8 & 0x3f);
        }
        _ => return Err(Utf8Error::new(Utf8ErrorKind::CodePointOutOfRange, 0)),
    }
    Ok(width)
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;

    fn encode(value: u32) -> ([u8; MAX_UTF8_BYTES], usize) {
        let mut bytes = [0; MAX_UTF8_BYTES];
        let width = encode_code_point(value, &mut bytes).unwrap();
        (bytes, width)
    }

    #[test]
    fn scalar_predicate_and_width_cover_boundaries() {
        assert!(is_scalar_value(0));
        assert!(is_scalar_value(0xd7ff));
        assert!(!is_scalar_value(0xd800));
        assert!(!is_scalar_value(0xdfff));
        assert!(is_scalar_value(0xe000));
        assert!(is_scalar_value(0x10ffff));
        assert!(!is_scalar_value(0x110000));

        assert_eq!(encoded_len(0x7f), Ok(1));
        assert_eq!(encoded_len(0x80), Ok(2));
        assert_eq!(encoded_len(0x7ff), Ok(2));
        assert_eq!(encoded_len(0x800), Ok(3));
        assert_eq!(encoded_len(0xffff), Ok(3));
        assert_eq!(encoded_len(0x10000), Ok(4));
        assert_eq!(encoded_len(0x10ffff), Ok(4));
        assert_eq!(
            encoded_len(0xd800),
            Err(Utf8Error {
                kind: Utf8ErrorKind::SurrogateCodePoint,
                offset: 0
            })
        );
        assert_eq!(
            encoded_len(0x110000),
            Err(Utf8Error {
                kind: Utf8ErrorKind::CodePointOutOfRange,
                offset: 0
            })
        );
    }

    #[test]
    fn all_scalar_values_round_trip_without_allocation() {
        let mut value = 0u32;
        while value <= 0x10ffff {
            if is_scalar_value(value) {
                let (encoded, width) = encode(value);
                let decoded = decode_code_point(&encoded[..width]).unwrap();
                assert_eq!(decoded.code_point, value);
                assert_eq!(decoded.width, width);
                assert_eq!(validate_utf8(&encoded[..width]), Ok(()));
            }
            value += 1;
        }
    }

    #[test]
    fn known_boundaries_have_canonical_encodings() {
        let expected = [
            (0x00, [0x00, 0, 0, 0], 1),
            (0x7f, [0x7f, 0, 0, 0], 1),
            (0x80, [0xc2, 0x80, 0, 0], 2),
            (0x7ff, [0xdf, 0xbf, 0, 0], 2),
            (0x800, [0xe0, 0xa0, 0x80, 0], 3),
            (0xffff, [0xef, 0xbf, 0xbf, 0], 3),
            (0x10000, [0xf0, 0x90, 0x80, 0x80], 4),
            (0x10ffff, [0xf4, 0x8f, 0xbf, 0xbf], 4),
        ];
        for (value, expected_bytes, expected_width) in expected {
            let (actual, width) = encode(value);
            assert_eq!(actual, expected_bytes);
            assert_eq!(width, expected_width);
            assert_eq!(
                decode_code_point(&actual[..width]).unwrap().code_point,
                value
            );
        }
    }

    #[test]
    fn ascii_is_exhaustive_and_one_byte_wide() {
        let mut byte = 0u16;
        while byte <= 0x7f {
            let input = [byte as u8];
            assert_eq!(
                decode_code_point(&input),
                Ok(DecodedCodePoint {
                    code_point: byte as u32,
                    width: 1
                })
            );
            assert_eq!(validate_utf8(&input), Ok(()));
            byte += 1;
        }
    }

    #[test]
    fn malformed_leads_are_rejected() {
        for byte in 0x80u8..=0xbf {
            assert_eq!(
                decode_code_point(&[byte]).unwrap_err(),
                Utf8Error {
                    kind: Utf8ErrorKind::InvalidLeadByte,
                    offset: 0
                }
            );
        }
        for byte in 0xc0u8..=0xc1 {
            assert_eq!(
                decode_code_point(&[byte]).unwrap_err().kind,
                Utf8ErrorKind::InvalidLeadByte
            );
        }
        for byte in 0xf5u8..=0xff {
            assert_eq!(
                decode_code_point(&[byte]).unwrap_err().kind,
                Utf8ErrorKind::InvalidLeadByte
            );
        }
    }

    #[test]
    fn truncation_reports_the_end_offset() {
        let inputs: [&[u8]; 6] = [
            &[0xc2],
            &[0xe2],
            &[0xe2, 0x82],
            &[0xf0],
            &[0xf0, 0x90],
            &[0xf0, 0x90, 0x80],
        ];
        for input in inputs {
            let error = decode_code_point(input).unwrap_err();
            assert_eq!(error.kind, Utf8ErrorKind::UnexpectedEnd);
            assert_eq!(error.offset, input.len());
            assert_eq!(validate_utf8(input), Err(error));
        }
    }

    #[test]
    fn invalid_continuations_report_their_byte_offset() {
        let cases = [
            ([0xc2, 0x20, 0, 0], 2usize, 1usize),
            ([0xe1, 0x80, 0x20, 0], 3usize, 2usize),
            ([0xe1, 0x20, 0x80, 0], 3usize, 1usize),
            ([0xf1, 0x80, 0x80, 0x20], 4usize, 3usize),
            ([0xf1, 0x80, 0x20, 0x80], 4usize, 2usize),
            ([0xf1, 0x20, 0x80, 0x80], 4usize, 1usize),
        ];
        for (bytes, width, offset) in cases {
            let error = decode_code_point(&bytes[..width]).unwrap_err();
            assert_eq!(error.kind, Utf8ErrorKind::InvalidContinuationByte);
            assert_eq!(error.offset, offset);
        }
    }

    #[test]
    fn overlong_and_non_scalar_sequences_are_rejected() {
        let cases = [
            (&[0xe0, 0x80, 0x80][..], Utf8ErrorKind::OverlongEncoding, 1),
            (&[0xe0, 0x9f, 0xbf][..], Utf8ErrorKind::OverlongEncoding, 1),
            (
                &[0xf0, 0x80, 0x80, 0x80][..],
                Utf8ErrorKind::OverlongEncoding,
                1,
            ),
            (
                &[0xf0, 0x8f, 0xbf, 0xbf][..],
                Utf8ErrorKind::OverlongEncoding,
                1,
            ),
            (
                &[0xed, 0xa0, 0x80][..],
                Utf8ErrorKind::SurrogateCodePoint,
                1,
            ),
            (
                &[0xed, 0xbf, 0xbf][..],
                Utf8ErrorKind::SurrogateCodePoint,
                1,
            ),
            (
                &[0xf4, 0x90, 0x80, 0x80][..],
                Utf8ErrorKind::CodePointOutOfRange,
                1,
            ),
        ];
        for (input, kind, offset) in cases {
            let error = decode_code_point(input).unwrap_err();
            assert_eq!(error.kind, kind);
            assert_eq!(error.offset, offset);
            assert_eq!(validate_utf8(input), Err(error));
        }
    }

    #[test]
    fn validation_error_offsets_are_absolute() {
        let input = [b'a', b'b', 0xe2, 0x82, b'c'];
        assert_eq!(
            validate_utf8(&input),
            Err(Utf8Error {
                kind: Utf8ErrorKind::InvalidContinuationByte,
                offset: 4
            })
        );

        let input = [b'a', 0xe2, 0x82];
        assert_eq!(
            validate_utf8(&input),
            Err(Utf8Error {
                kind: Utf8ErrorKind::UnexpectedEnd,
                offset: 3
            })
        );
    }

    #[test]
    fn encoder_rejects_invalid_scalars_and_short_output() {
        let mut output = [0u8; 4];
        assert_eq!(
            encode_code_point(0xd800, &mut output),
            Err(Utf8Error {
                kind: Utf8ErrorKind::SurrogateCodePoint,
                offset: 0
            })
        );
        assert_eq!(
            encode_code_point(0x110000, &mut output),
            Err(Utf8Error {
                kind: Utf8ErrorKind::CodePointOutOfRange,
                offset: 0
            })
        );
        assert_eq!(
            encode_code_point(0x80, &mut []),
            Err(Utf8Error {
                kind: Utf8ErrorKind::OutputTooSmall,
                offset: 0
            })
        );
        assert_eq!(
            encode_code_point(0x10000, &mut [0; 3]),
            Err(Utf8Error {
                kind: Utf8ErrorKind::OutputTooSmall,
                offset: 0
            })
        );
        assert_eq!(encode_code_point(0x41, &mut output), Ok(1));
        assert_eq!(output[0], 0x41);
    }

    fn next_state(state: &mut u64) -> u8 {
        *state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        (*state >> 24) as u8
    }

    #[test]
    fn deterministic_byte_property_matches_std_utf8() {
        let mut state = 0x7a31_5c9d_1122_3344u64;
        let mut bytes = [0u8; 64];
        let mut case = 0usize;
        while case < 20_000 {
            let length = (next_state(&mut state) as usize) % bytes.len();
            let mut index = 0usize;
            while index < length {
                bytes[index] = next_state(&mut state);
                index += 1;
            }
            let ours = validate_utf8(&bytes[..length]).is_ok();
            let standard = std::str::from_utf8(&bytes[..length]).is_ok();
            assert_eq!(ours, standard, "case {case}, length {length}");
            case += 1;
        }
    }

    #[test]
    fn valid_sequences_can_have_trailing_scalars_for_one_value_decode() {
        let input = [0xc2, 0xa2, b'X'];
        assert_eq!(
            decode_code_point(&input),
            Ok(DecodedCodePoint {
                code_point: 0xa2,
                width: 2
            })
        );
        assert_eq!(validate_utf8(&input), Ok(()));
    }
}
