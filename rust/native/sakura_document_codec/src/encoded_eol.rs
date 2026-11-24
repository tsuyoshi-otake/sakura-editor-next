//! Comparison-only scan for end-of-line markers in UTF-16 and UTF-32 regions.
//!
//! This reproduces what the C++ file loader does for the four non-byte
//! encodings: it steps over complete units from a start offset, recognizes CR,
//! LF and (when extended EOLs are on) NEL, LS and PS, and groups a CR with an
//! immediately following complete LF unit into one marker. It never reads a
//! partial final unit and never allocates.

use core::iter::FusedIterator;

const LF: u32 = 0x0A;
const CR: u32 = 0x0D;
const NEL: u32 = 0x85;
const LS: u32 = 0x2028;
const PS: u32 = 0x2029;

/// The encoding of a region, numbered like the C++ `EEncodingTrait` values.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EncodedEolFormat {
    /// UTF-16 little endian, two-byte units.
    Utf16Le = 2,
    /// UTF-16 big endian, two-byte units.
    Utf16Be = 3,
    /// UTF-32 little endian, four-byte units.
    Utf32Le = 4,
    /// UTF-32 big endian, four-byte units.
    Utf32Be = 5,
}

impl EncodedEolFormat {
    /// Returns the format for a wire number, or `None` for any other value.
    pub const fn from_wire(value: u32) -> Option<Self> {
        match value {
            2 => Some(Self::Utf16Le),
            3 => Some(Self::Utf16Be),
            4 => Some(Self::Utf32Le),
            5 => Some(Self::Utf32Be),
            _ => None,
        }
    }

    /// Returns the width in bytes of one code unit.
    pub const fn unit_bytes(self) -> usize {
        match self {
            Self::Utf16Le | Self::Utf16Be => 2,
            Self::Utf32Le | Self::Utf32Be => 4,
        }
    }

    /// Reads the unit that starts at `offset` when it lies completely inside `region`.
    fn unit_at(self, region: &[u8], offset: usize) -> Option<u32> {
        match self {
            Self::Utf16Le => {
                chunk(region, offset).map(|bytes| u32::from(u16::from_le_bytes(bytes)))
            }
            Self::Utf16Be => {
                chunk(region, offset).map(|bytes| u32::from(u16::from_be_bytes(bytes)))
            }
            Self::Utf32Le => chunk(region, offset).map(u32::from_le_bytes),
            Self::Utf32Be => chunk(region, offset).map(u32::from_be_bytes),
        }
    }
}

/// Copies `N` bytes starting at `offset` when all of them lie inside `region`.
fn chunk<const N: usize>(region: &[u8], offset: usize) -> Option<[u8; N]> {
    region.get(offset..offset.checked_add(N)?)?.try_into().ok()
}

/// The kind of an end-of-line marker, numbered like the C++ `EEolType` values.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EolKind {
    /// CR immediately followed by a complete LF unit, reported as one marker.
    CrLf = 1,
    /// LF (U+000A).
    Lf = 2,
    /// CR (U+000D) that is not followed by a complete LF unit.
    Cr = 3,
    /// NEL (U+0085), recognized only when extended EOLs are on.
    Nel = 4,
    /// LS (U+2028), recognized only when extended EOLs are on.
    Ls = 5,
    /// PS (U+2029), recognized only when extended EOLs are on.
    Ps = 6,
}

/// One marker found by a scan. Offsets count bytes from the start of the region.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EolRecord {
    /// The byte offset of the first unit of the marker.
    pub offset: usize,
    /// The byte width of the marker: one unit, or two units for CR LF.
    pub width: usize,
    /// The kind of the marker.
    pub kind: EolKind,
}

/// Why a scan request was rejected before any unit was read.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EncodedEolScanError {
    /// The start offset lies after the end of the region.
    StartAfterEnd,
    /// The start offset lies inside the region but not on a unit boundary.
    StartNotOnUnitBoundary,
}

/// The markers of one scan in ascending offset order, found lazily.
#[derive(Clone, Debug)]
pub struct EncodedEolScan<'a> {
    region: &'a [u8],
    format: EncodedEolFormat,
    extended_eol: bool,
    next: usize,
}

/// Starts a scan of `region` at byte offset `start`; the end of the region is its length.
///
/// `start` may equal the region length, which yields no markers whatever its
/// alignment. A larger `start` is [`EncodedEolScanError::StartAfterEnd`]; a
/// `start` inside the region that is not a multiple of the unit width is
/// [`EncodedEolScanError::StartNotOnUnitBoundary`]. When `extended_eol` is
/// false only CR, LF and CR LF are markers; when true NEL, LS and PS are too.
pub fn scan_encoded_eol(
    region: &[u8],
    format: EncodedEolFormat,
    extended_eol: bool,
    start: usize,
) -> Result<EncodedEolScan<'_>, EncodedEolScanError> {
    if start > region.len() {
        return Err(EncodedEolScanError::StartAfterEnd);
    }
    if start < region.len() && !start.is_multiple_of(format.unit_bytes()) {
        return Err(EncodedEolScanError::StartNotOnUnitBoundary);
    }
    Ok(EncodedEolScan {
        region,
        format,
        extended_eol,
        next: start,
    })
}

impl Iterator for EncodedEolScan<'_> {
    type Item = EolRecord;

    fn next(&mut self) -> Option<EolRecord> {
        let unit = self.format.unit_bytes();
        let mut offset = self.next;
        while let Some(value) = self.format.unit_at(self.region, offset) {
            let (kind, width) = match value {
                LF => (EolKind::Lf, unit),
                CR if self.format.unit_at(self.region, offset + unit) == Some(LF) => {
                    (EolKind::CrLf, 2 * unit)
                }
                CR => (EolKind::Cr, unit),
                NEL if self.extended_eol => (EolKind::Nel, unit),
                LS if self.extended_eol => (EolKind::Ls, unit),
                PS if self.extended_eol => (EolKind::Ps, unit),
                _ => {
                    offset += unit;
                    continue;
                }
            };
            self.next = offset + width;
            return Some(EolRecord {
                offset,
                width,
                kind,
            });
        }
        self.next = self.region.len();
        None
    }
}

impl FusedIterator for EncodedEolScan<'_> {}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use std::vec::Vec;

    const ALL_FORMATS: [EncodedEolFormat; 4] = [
        EncodedEolFormat::Utf16Le,
        EncodedEolFormat::Utf16Be,
        EncodedEolFormat::Utf32Le,
        EncodedEolFormat::Utf32Be,
    ];

    fn record(offset: usize, width: usize, kind: EolKind) -> EolRecord {
        EolRecord {
            offset,
            width,
            kind,
        }
    }

    /// Encodes unit values with the width and byte order of `format`.
    fn encode(format: EncodedEolFormat, units: &[u32]) -> Vec<u8> {
        let mut bytes = Vec::new();
        for &unit in units {
            match format {
                EncodedEolFormat::Utf16Le => bytes.extend_from_slice(&(unit as u16).to_le_bytes()),
                EncodedEolFormat::Utf16Be => bytes.extend_from_slice(&(unit as u16).to_be_bytes()),
                EncodedEolFormat::Utf32Le => bytes.extend_from_slice(&unit.to_le_bytes()),
                EncodedEolFormat::Utf32Be => bytes.extend_from_slice(&unit.to_be_bytes()),
            }
        }
        bytes
    }

    fn scan(
        region: &[u8],
        format: EncodedEolFormat,
        extended_eol: bool,
        start: usize,
    ) -> Vec<EolRecord> {
        scan_encoded_eol(region, format, extended_eol, start)
            .expect("the start offset is valid")
            .collect()
    }

    #[test]
    fn wire_numbers_match_the_cpp_enumerations() {
        assert_eq!(EncodedEolFormat::Utf16Le as u32, 2);
        assert_eq!(EncodedEolFormat::Utf16Be as u32, 3);
        assert_eq!(EncodedEolFormat::Utf32Le as u32, 4);
        assert_eq!(EncodedEolFormat::Utf32Be as u32, 5);
        for format in ALL_FORMATS {
            assert_eq!(EncodedEolFormat::from_wire(format as u32), Some(format));
        }
        for value in [0, 1, 6, 7, 255, u32::MAX] {
            assert_eq!(EncodedEolFormat::from_wire(value), None, "{value}");
        }
        assert_eq!(EolKind::CrLf as u32, 1);
        assert_eq!(EolKind::Lf as u32, 2);
        assert_eq!(EolKind::Cr as u32, 3);
        assert_eq!(EolKind::Nel as u32, 4);
        assert_eq!(EolKind::Ls as u32, 5);
        assert_eq!(EolKind::Ps as u32, 6);
    }

    #[test]
    fn unit_width_follows_the_format() {
        assert_eq!(EncodedEolFormat::Utf16Le.unit_bytes(), 2);
        assert_eq!(EncodedEolFormat::Utf16Be.unit_bytes(), 2);
        assert_eq!(EncodedEolFormat::Utf32Le.unit_bytes(), 4);
        assert_eq!(EncodedEolFormat::Utf32Be.unit_bytes(), 4);
    }

    #[test]
    fn units_use_the_byte_order_of_the_format() {
        use EncodedEolFormat::{Utf16Be, Utf16Le, Utf32Be, Utf32Le};

        assert_eq!(
            scan(&[0x0A, 0x00], Utf16Le, false, 0),
            [record(0, 2, EolKind::Lf)]
        );
        assert!(scan(&[0x00, 0x0A], Utf16Le, false, 0).is_empty());
        assert_eq!(
            scan(&[0x00, 0x0A], Utf16Be, false, 0),
            [record(0, 2, EolKind::Lf)]
        );
        assert!(scan(&[0x0A, 0x00], Utf16Be, false, 0).is_empty());
        assert_eq!(
            scan(&[0x0A, 0x00, 0x00, 0x00], Utf32Le, false, 0),
            [record(0, 4, EolKind::Lf)]
        );
        assert!(scan(&[0x00, 0x00, 0x00, 0x0A], Utf32Le, false, 0).is_empty());
        assert_eq!(
            scan(&[0x00, 0x00, 0x00, 0x0A], Utf32Be, false, 0),
            [record(0, 4, EolKind::Lf)]
        );
        assert!(scan(&[0x0A, 0x00, 0x00, 0x00], Utf32Be, false, 0).is_empty());
    }

    #[test]
    fn markers_are_reported_with_byte_offsets_in_every_format() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let region = encode(format, &[0x41, LF, 0x42, CR, 0x43, CR, LF, LF]);
            assert_eq!(
                scan(&region, format, false, 0),
                [
                    record(unit, unit, EolKind::Lf),
                    record(3 * unit, unit, EolKind::Cr),
                    record(5 * unit, 2 * unit, EolKind::CrLf),
                    record(7 * unit, unit, EolKind::Lf),
                ],
                "{format:?}"
            );
        }
    }

    #[test]
    fn byte_swapped_and_wide_lookalikes_are_not_delimiters() {
        for format in [EncodedEolFormat::Utf16Le, EncodedEolFormat::Utf16Be] {
            let region = encode(
                format,
                &[
                    0x0A00, 0x0D00, 0x8500, 0x0A0D, 0x0D0A, 0x2027, 0x202A, 0x2800, 0x2900,
                ],
            );
            assert!(scan(&region, format, true, 0).is_empty(), "{format:?}");
        }
        for format in [EncodedEolFormat::Utf32Le, EncodedEolFormat::Utf32Be] {
            let region = encode(
                format,
                &[
                    0x0001_000A,
                    0x0001_000D,
                    0x0001_0085,
                    0x0001_2028,
                    0x0001_2029,
                    0x0A00_0000,
                    0x0D00_0000,
                ],
            );
            assert!(scan(&region, format, true, 0).is_empty(), "{format:?}");
        }
    }

    #[test]
    fn cr_pairs_only_with_a_complete_following_lf() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            assert_eq!(
                scan(&encode(format, &[CR, LF]), format, false, 0),
                [record(0, 2 * unit, EolKind::CrLf)]
            );
            assert_eq!(
                scan(&encode(format, &[CR, CR, LF]), format, false, 0),
                [
                    record(0, unit, EolKind::Cr),
                    record(unit, 2 * unit, EolKind::CrLf)
                ]
            );
            assert_eq!(
                scan(&encode(format, &[CR, 0x41]), format, false, 0),
                [record(0, unit, EolKind::Cr)]
            );
            assert_eq!(
                scan(&encode(format, &[LF, CR]), format, false, 0),
                [
                    record(0, unit, EolKind::Lf),
                    record(unit, unit, EolKind::Cr)
                ]
            );
            let lf = encode(format, &[LF]);
            for kept in 1..unit {
                let mut region = encode(format, &[CR]);
                region.extend_from_slice(&lf[..kept]);
                assert_eq!(
                    scan(&region, format, false, 0),
                    [record(0, unit, EolKind::Cr)],
                    "{format:?} kept={kept}"
                );
            }
        }
    }

    #[test]
    fn extended_markers_need_the_flag() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let region = encode(format, &[NEL, 0x41, LS, PS]);
            assert!(scan(&region, format, false, 0).is_empty(), "{format:?}");
            assert_eq!(
                scan(&region, format, true, 0),
                [
                    record(0, unit, EolKind::Nel),
                    record(2 * unit, unit, EolKind::Ls),
                    record(3 * unit, unit, EolKind::Ps),
                ],
                "{format:?}"
            );
        }
    }

    #[test]
    fn a_partial_final_unit_is_never_read() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let lf = encode(format, &[LF]);
            for kept in 1..unit {
                let mut region = lf.clone();
                region.extend_from_slice(&lf[..kept]);
                assert_eq!(
                    scan(&region, format, false, 0),
                    [record(0, unit, EolKind::Lf)],
                    "{format:?} kept={kept}"
                );
                assert!(scan(&lf[..kept], format, false, 0).is_empty());
            }
        }
    }

    #[test]
    fn start_must_be_inside_the_region_and_on_a_unit_boundary() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let region = encode(format, &[LF, 0x41, LF]);
            let len = region.len();
            for start in [len + 1, len + unit, usize::MAX] {
                assert_eq!(
                    scan_encoded_eol(&region, format, false, start).unwrap_err(),
                    EncodedEolScanError::StartAfterEnd,
                    "{format:?} start={start}"
                );
            }
            for misaligned in 1..unit {
                for base in [0, unit] {
                    assert_eq!(
                        scan_encoded_eol(&region, format, false, base + misaligned).unwrap_err(),
                        EncodedEolScanError::StartNotOnUnitBoundary,
                        "{format:?} start={}",
                        base + misaligned
                    );
                }
            }
            assert!(scan(&region, format, false, len).is_empty());
            assert_eq!(
                scan(&region, format, false, 2 * unit),
                [record(2 * unit, unit, EolKind::Lf)]
            );
        }
    }

    #[test]
    fn a_start_equal_to_the_end_is_valid_even_inside_a_partial_unit() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let mut region = encode(format, &[LF]);
            region.push(0x41);
            assert!(scan(&region, format, false, region.len()).is_empty());
            assert!(scan(&region, format, false, unit).is_empty());
            assert!(scan_encoded_eol(&[], format, true, 0)
                .unwrap()
                .next()
                .is_none());
            assert_eq!(
                scan_encoded_eol(&[], format, true, 1).unwrap_err(),
                EncodedEolScanError::StartAfterEnd
            );
        }
    }

    #[test]
    fn offsets_count_from_the_region_start_not_from_the_scan_start() {
        for format in ALL_FORMATS {
            let unit = format.unit_bytes();
            let region = encode(format, &[0x41, LF, 0x42, LF]);
            assert_eq!(
                scan(&region, format, false, 2 * unit),
                [record(3 * unit, unit, EolKind::Lf)]
            );
        }
    }

    #[test]
    fn the_iterator_is_fused_after_the_last_marker() {
        let region = encode(EncodedEolFormat::Utf16Le, &[LF]);
        let mut scan = scan_encoded_eol(&region, EncodedEolFormat::Utf16Le, false, 0).unwrap();
        assert_eq!(scan.next(), Some(record(0, 2, EolKind::Lf)));
        for _ in 0..3 {
            assert_eq!(scan.next(), None);
        }
    }
}
