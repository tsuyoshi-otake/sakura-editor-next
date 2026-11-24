mod corpus;

use corpus::{corpus_path, load_cases, parse_corpus, Case};
use sakura_document_codec::{
    scan_encoded_eol, EncodedEolFormat, EncodedEolScanError, EolKind, EolRecord,
};
use std::collections::BTreeSet;
use std::fs;

const ALL_FORMATS: [EncodedEolFormat; 4] = [
    EncodedEolFormat::Utf16Le,
    EncodedEolFormat::Utf16Be,
    EncodedEolFormat::Utf32Le,
    EncodedEolFormat::Utf32Be,
];
const LF: u32 = 0x0A;
const CR: u32 = 0x0D;
const NEL: u32 = 0x85;
const LS: u32 = 0x2028;
const PS: u32 = 0x2029;
const UTF16_ALPHABET: [u32; 10] = [LF, CR, NEL, LS, PS, 0x41, 0, 0x0A0D, 0x0D0A, 0xD800];
const UTF32_ALPHABET: [u32; 9] = [LF, CR, NEL, LS, PS, 0x41, 0, 0x0001_000A, 0x0001_0085];

fn scan(
    bytes: &[u8],
    format: EncodedEolFormat,
    extended_eol: bool,
    start: usize,
) -> Vec<EolRecord> {
    scan_encoded_eol(bytes, format, extended_eol, start)
        .expect("the start offset is valid")
        .collect()
}

#[test]
fn corpus_is_well_formed_and_covers_the_required_classes() {
    let cases = load_cases().expect("the shared corpus is valid");
    assert_eq!(cases.len(), 424);
    let classes: BTreeSet<_> = cases.iter().map(|case| case.class.as_str()).collect();
    assert_eq!(
        classes,
        BTreeSet::from([
            "basic",
            "empty",
            "extended",
            "lookalike",
            "no-eol",
            "positions",
            "runs",
            "short-tail",
            "start",
            "sub-unit",
            "surrogate",
            "truncated-pair",
            "utf32-high-bits",
        ])
    );
    let formats: BTreeSet<_> = cases.iter().map(|case| case.format as u32).collect();
    assert_eq!(
        formats,
        BTreeSet::from(ALL_FORMATS.map(|format| format as u32))
    );
    let flags: BTreeSet<_> = cases.iter().map(|case| case.extended_eol).collect();
    assert_eq!(flags, BTreeSet::from([false, true]));
    assert_eq!(cases.iter().filter(|case| case.has_tail).count(), 104);
    assert_eq!(cases.iter().filter(|case| case.id == "crlf").count(), 8);

    for case in &cases {
        let unit = case.format.unit_bytes();
        let complete_bytes = case.bytes.len() / unit * unit;
        assert!(case.start.is_multiple_of(unit), "{}", case.describe());
        assert!(case.start <= complete_bytes, "{}", case.describe());
        let mut previous_end = case.start;
        for record in &case.records {
            let width = if record.kind == EolKind::CrLf {
                2 * unit
            } else {
                unit
            };
            assert_eq!(record.width, width, "{}", case.describe());
            assert!(record.offset.is_multiple_of(unit), "{}", case.describe());
            assert!(record.offset >= previous_end, "{}", case.describe());
            previous_end = record.offset + record.width;
            assert!(previous_end <= complete_bytes, "{}", case.describe());
        }
    }
}

#[test]
fn loader_rejects_malformed_corpus_text() {
    const HEADER: &str = "id\tclass\tformats\tstart\tunits\trecords_off\trecords_on";
    const GOOD_ROW: &str = "fixture\tbasic\tall\t0\tLF\t0:1:LF\t=";
    const PARTIAL_REASON: &str = "partial unit must be the last token and specify 1 to 3 bytes";
    const RANGE_REASON: &str = "record is out of order, overlapping or outside the scan range";

    fn expect_error(text: &str, reason: &str) {
        let error = parse_corpus(text, "origin").expect_err("the corpus must be rejected");
        assert!(
            error.ends_with(&format!(": {reason}")),
            "expected {reason:?}, got {error:?} for {text:?}"
        );
    }

    expect_error("# comment\n\n", "missing header");
    expect_error("wrong header\n", "invalid header");
    expect_error(
        &format!("{HEADER}\n{GOOD_ROW}\n{GOOD_ROW}\n"),
        "duplicate id: fixture",
    );

    let malformed_rows = [
        (
            "fixture\tbasic\tall\t0\tLF\t0:1:LF",
            "expected seven tab-separated fields",
        ),
        ("\tbasic\tall\t0\tLF\t-\t=", "invalid id"),
        ("Fixture\tbasic\tall\t0\tLF\t-\t=", "invalid id"),
        ("fixture\t\tall\t0\tLF\t-\t=", "empty class"),
        ("fixture\tbasic\tutf8\t0\tLF\t-\t=", "unknown format: utf8"),
        ("fixture\tbasic\tall\t+1\tLF\t-\t=", "invalid start"),
        ("fixture\tbasic\tall\t-1\tLF\t-\t=", "invalid start"),
        ("fixture\tbasic\tall\tx\tLF\t-\t=", "invalid start"),
        ("fixture\tbasic\tall\t\tLF\t-\t=", "invalid start"),
        ("fixture\tbasic\tall\t 1\tLF\t-\t=", "invalid start"),
        (
            "fixture\tbasic\tall\t2\tLF\t-\t=",
            "start is past the last complete unit",
        ),
        ("fixture\tbasic\tall\t0\tX\t-\t=", "invalid unit token: X"),
        ("fixture\tbasic\tall\t0\tA  B\t-\t=", "invalid unit token: "),
        (
            "fixture\tbasic\tall\t0\tu12\t-\t=",
            "invalid unit token: u12",
        ),
        (
            "fixture\tbasic\tall\t0\tU0000000A\t-\t=",
            "32-bit unit in a UTF-16 format",
        ),
        (
            "fixture\tbasic\tutf16le\t0\tU0000000A\t-\t=",
            "32-bit unit in a UTF-16 format",
        ),
        (
            "fixture\tbasic\tall\t0\tu+00A\t-\t=",
            "invalid 16-bit hex unit",
        ),
        (
            "fixture\tbasic\tutf32\t0\tU0x00000A\t-\t=",
            "invalid 32-bit hex unit",
        ),
        ("fixture\tbasic\tall\t0\tA~1 B\t-\t=", PARTIAL_REASON),
        ("fixture\tbasic\tall\t0\tA~0\t-\t=", PARTIAL_REASON),
        ("fixture\tbasic\tall\t0\tA~4\t-\t=", PARTIAL_REASON),
        ("fixture\tbasic\tall\t0\tA~12\t-\t=", PARTIAL_REASON),
        (
            "fixture\tbasic\tutf16\t0\tA~2\t-\t=",
            "partial unit is too wide for the format",
        ),
        (
            "fixture\tbasic\tall\t0\tLF\t0:1\t=",
            "expected offset:width:KIND",
        ),
        (
            "fixture\tbasic\tall\t0\tLF\t+0:1:LF\t=",
            "invalid record offset",
        ),
        (
            "fixture\tbasic\tall\t0\tLF\t0: 1:LF\t=",
            "invalid record width",
        ),
        (
            "fixture\tbasic\tall\t0\tLF\t0:1:XX\t=",
            "unknown record kind: XX",
        ),
        (
            "fixture\tbasic\tall\t0\tCR LF\t0:1:CRLF\t=",
            "record width does not match its kind",
        ),
        (
            "fixture\tbasic\tall\t0\tLF LF\t1:1:LF;0:1:LF\t=",
            RANGE_REASON,
        ),
        (
            "fixture\tbasic\tall\t0\tCR LF\t0:2:CRLF;1:1:LF\t=",
            RANGE_REASON,
        ),
        ("fixture\tbasic\tall\t0\tLF LF\t5:1:LF\t=", RANGE_REASON),
        (
            "fixture\tbasic\tall\t0\tLF\t=\t=",
            "expected offset:width:KIND",
        ),
    ];
    for (row, reason) in malformed_rows {
        expect_error(&format!("{HEADER}\n{row}\n"), reason);
    }
    let error = parse_corpus(&format!("# comment\n{HEADER}\nbad row\n"), "origin")
        .expect_err("the third line is malformed");
    assert_eq!(error, "origin:3: expected seven tab-separated fields");

    let path = corpus_path();
    let text = fs::read_to_string(&path).expect("the shared corpus is readable");
    let crlf_text = text.replace("\r\n", "\n").replace('\n', "\r\n");
    assert_eq!(
        parse_corpus(&crlf_text, &path.display().to_string()).expect("CRLF is supported"),
        load_cases().expect("the shared corpus is valid")
    );
}

#[test]
fn direct_scan_matches_the_corpus() {
    for case in load_cases().expect("the shared corpus is valid") {
        assert_eq!(
            scan(&case.bytes, case.format, case.extended_eol, case.start),
            case.records,
            "{}",
            case.describe()
        );
    }
}

fn expect_replay(case: &Case, single: &[EolRecord], splits: &[usize]) {
    let mut replayed = Vec::new();
    let mut start = 0;
    for &end in splits {
        replayed.extend(scan(
            &case.bytes[..end],
            case.format,
            case.extended_eol,
            start,
        ));
        start = end;
    }
    replayed.extend(scan(&case.bytes, case.format, case.extended_eol, start));
    assert_eq!(
        replayed.as_slice(),
        single,
        "{} splits={splits:?}",
        case.describe()
    );
}

#[test]
fn splitting_at_line_starts_replays_the_single_scan() {
    for case in load_cases().expect("the shared corpus is valid") {
        let single = scan(&case.bytes, case.format, case.extended_eol, 0);
        let line_starts: Vec<_> = std::iter::once(0)
            .chain(single.iter().map(|record| record.offset + record.width))
            .collect();
        for (index, &first) in line_starts.iter().enumerate() {
            expect_replay(&case, &single, &[first]);
            for &second in &line_starts[index..] {
                expect_replay(&case, &single, &[first, second]);
            }
        }
    }
}

#[test]
fn a_start_inside_a_cr_lf_pair_scans_the_lf_alone() {
    // Callers must partition on line starts. The C++ planner does so.
    let mut checked = 0;
    for case in load_cases()
        .expect("the shared corpus is valid")
        .into_iter()
        .filter(|case| case.id == "crlf")
    {
        let unit = case.format.unit_bytes();
        assert_eq!(
            scan(&case.bytes, case.format, case.extended_eol, 2 * unit),
            [EolRecord {
                offset: 2 * unit,
                width: unit,
                kind: EolKind::Lf,
            }],
            "{}",
            case.describe()
        );
        assert_eq!(
            scan(&case.bytes, case.format, case.extended_eol, unit),
            [EolRecord {
                offset: unit,
                width: 2 * unit,
                kind: EolKind::CrLf,
            }],
            "{}",
            case.describe()
        );
        checked += 1;
    }
    assert_eq!(checked, 8);
}

struct ModelRegion {
    units: Vec<u32>,
    len: usize,
    width: usize,
}

impl ModelRegion {
    fn new(bytes: &[u8], format: EncodedEolFormat) -> Self {
        let width = match format {
            EncodedEolFormat::Utf16Le | EncodedEolFormat::Utf16Be => 2,
            EncodedEolFormat::Utf32Le | EncodedEolFormat::Utf32Be => 4,
        };
        let units = bytes
            .chunks_exact(width)
            .map(|chunk| match format {
                EncodedEolFormat::Utf16Le => u32::from(u16::from_le_bytes([chunk[0], chunk[1]])),
                EncodedEolFormat::Utf16Be => u32::from(u16::from_be_bytes([chunk[0], chunk[1]])),
                EncodedEolFormat::Utf32Le => {
                    u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]])
                }
                EncodedEolFormat::Utf32Be => {
                    u32::from_be_bytes([chunk[0], chunk[1], chunk[2], chunk[3]])
                }
            })
            .collect();
        Self {
            units,
            len: bytes.len(),
            width,
        }
    }

    fn scan(
        &self,
        extended_eol: bool,
        start: usize,
    ) -> Result<Vec<EolRecord>, EncodedEolScanError> {
        if start > self.len {
            return Err(EncodedEolScanError::StartAfterEnd);
        }
        let mut records = Vec::new();
        if start == self.len {
            return Ok(records);
        }
        if !start.is_multiple_of(self.width) {
            return Err(EncodedEolScanError::StartNotOnUnitBoundary);
        }

        let mut index = start / self.width;
        while index < self.units.len() {
            let value = self.units[index];
            let (kind, step) = if value == LF {
                (EolKind::Lf, 1)
            } else if value == CR {
                if self.units.get(index + 1) == Some(&LF) {
                    (EolKind::CrLf, 2)
                } else {
                    (EolKind::Cr, 1)
                }
            } else if extended_eol && value == NEL {
                (EolKind::Nel, 1)
            } else if extended_eol && value == LS {
                (EolKind::Ls, 1)
            } else if extended_eol && value == PS {
                (EolKind::Ps, 1)
            } else {
                index += 1;
                continue;
            };
            records.push(EolRecord {
                offset: index * self.width,
                width: step * self.width,
                kind,
            });
            index += step;
        }
        Ok(records)
    }
}

fn alphabet(format: EncodedEolFormat) -> &'static [u32] {
    match format {
        EncodedEolFormat::Utf16Le | EncodedEolFormat::Utf16Be => &UTF16_ALPHABET,
        EncodedEolFormat::Utf32Le | EncodedEolFormat::Utf32Be => &UTF32_ALPHABET,
    }
}

fn append_unit(bytes: &mut Vec<u8>, format: EncodedEolFormat, value: u32) {
    match format {
        EncodedEolFormat::Utf16Le => bytes.extend_from_slice(&(value as u16).to_le_bytes()),
        EncodedEolFormat::Utf16Be => bytes.extend_from_slice(&(value as u16).to_be_bytes()),
        EncodedEolFormat::Utf32Le => bytes.extend_from_slice(&value.to_le_bytes()),
        EncodedEolFormat::Utf32Be => bytes.extend_from_slice(&value.to_be_bytes()),
    }
}

fn advance_odometer(digits: &mut [usize], radix: usize) -> bool {
    for digit in digits.iter_mut().rev() {
        *digit += 1;
        if *digit < radix {
            return true;
        }
        *digit = 0;
    }
    false
}

#[test]
fn scan_matches_an_independent_model_on_every_small_region() {
    let mut compared = 0usize;
    let mut with_records = 0usize;
    let mut rejected = 0usize;
    for format in ALL_FORMATS {
        let values = alphabet(format);
        let width = format.unit_bytes();
        let mut encoded_lf = Vec::new();
        append_unit(&mut encoded_lf, format, LF);
        let mut bytes = Vec::with_capacity(5 * width);
        for length in 0..=4 {
            let mut digits = [0; 4];
            loop {
                bytes.clear();
                for &digit in &digits[..length] {
                    append_unit(&mut bytes, format, values[digit]);
                }
                let complete_bytes = bytes.len();
                for tail in 0..width {
                    bytes.truncate(complete_bytes);
                    bytes.extend_from_slice(&encoded_lf[..tail]);
                    // Decode once and reuse the complete units for every scan request.
                    let model = ModelRegion::new(&bytes, format);
                    for extended_eol in [false, true] {
                        for start in 0..=bytes.len() + 2 {
                            let actual = scan_encoded_eol(&bytes, format, extended_eol, start)
                                .map(Iterator::collect::<Vec<_>>);
                            match &actual {
                                Ok(records) if !records.is_empty() => with_records += 1,
                                Err(_) => rejected += 1,
                                Ok(_) => {}
                            }
                            compared += 1;
                            assert_eq!(
                                actual,
                                model.scan(extended_eol, start),
                                "{format:?} bytes={bytes:?} extended={extended_eol} start={start}"
                            );
                        }
                    }
                }
                if !advance_odometer(&mut digits[..length], values.len()) {
                    break;
                }
            }
        }
    }
    // 2 * 501_234 (UTF-16 formats) + 2 * 1_180_980 (UTF-32 formats) scan requests.
    assert_eq!(compared, 3_364_428);
    assert!(with_records > 0 && rejected > 0);
}

#[test]
fn scan_matches_an_independent_model_on_pseudo_random_regions() {
    let mut state = 0xA76B_19F0_2EC4_58D3u64;
    let mut random_below = |limit: usize| -> usize {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        (state % limit as u64) as usize
    };
    for iteration in 0..4000 {
        let format = ALL_FORMATS[random_below(ALL_FORMATS.len())];
        let extended_eol = random_below(2) != 0;
        let units = random_below(49);
        let width = format.unit_bytes();
        let values = alphabet(format);
        let mut bytes = Vec::with_capacity((units + 1) * width);
        for _ in 0..units {
            append_unit(&mut bytes, format, values[random_below(values.len())]);
        }
        let end = bytes.len() + random_below(width);
        append_unit(&mut bytes, format, LF);
        bytes.truncate(end);

        let start = random_below(bytes.len() + 3);
        let model = ModelRegion::new(&bytes, format);
        let actual =
            scan_encoded_eol(&bytes, format, extended_eol, start).map(Iterator::collect::<Vec<_>>);
        assert_eq!(
            actual,
            model.scan(extended_eol, start),
            "iteration={iteration} {format:?} bytes={bytes:?} extended={extended_eol} start={start}"
        );
    }
}
