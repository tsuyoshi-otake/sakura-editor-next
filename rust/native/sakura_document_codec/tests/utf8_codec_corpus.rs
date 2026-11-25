mod fixture_tsv;
mod utf8_corpus;

use sakura_document_codec::{
    utf8_decode_measure, utf8_decode_write, utf8_encode_measure, utf8_encode_write, OutputTooSmall,
    Utf8Encoded, Utf8Variant,
};
use std::collections::BTreeSet;
use std::fs;
use utf8_corpus::{corpus_path, load_corpus, model, parse_corpus, Variants};

const VARIANTS: [Utf8Variant; 2] = [Utf8Variant::Utf8, Utf8Variant::Cesu8];
/// Elements after the documented output bound that a conversion must leave untouched.
const GUARD: usize = 8;
const UNIT_SENTINEL: u16 = 0xA5A5;
const BYTE_SENTINEL: u8 = 0xA5;

/// Bytes on the edges of the UTF-8 rules: ASCII and NUL, the continuation range, the lead bytes of
/// two-, three- and four-byte forms and the bytes that start no valid form.
const BOUNDARY_BYTES: [u8; 24] = [
    0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xAF, 0xB0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0,
    0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF4, 0xF5,
];
/// Units on the edges of the surrogate rules.
const UNIT_ALPHABET: [u16; 12] = [
    0x0041, 0x07FF, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDC7F, 0xDC80, 0xDCFF, 0xDD00, 0xDFFF, 0xE000,
];
/// The middle and last bytes of a three-byte form and the lead of the second form of a pair, chosen
/// on both sides of every boundary of a CESU-8 surrogate pair.
const FORM_MIDDLES: [u8; 10] = [0x7F, 0x80, 0x9F, 0xA0, 0xA1, 0xAF, 0xB0, 0xB1, 0xBF, 0xC0];
const FORM_TAILS: [u8; 4] = [0x7F, 0x80, 0xBF, 0xC0];
const SECOND_FORM_LEADS: [u8; 4] = [0xED, 0xEC, 0xEE, 0x41];
const FOLLOWERS: [&[u8]; 3] = [&[], &[0x41], &[0x80]];

fn hex_bytes(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".to_owned();
    }
    let tokens: Vec<_> = bytes.iter().map(|byte| format!("{byte:02X}")).collect();
    tokens.join(" ")
}

fn hex_units(units: &[u16]) -> String {
    if units.is_empty() {
        return "-".to_owned();
    }
    let tokens: Vec<_> = units.iter().map(|unit| format!("{unit:04X}")).collect();
    tokens.join(" ")
}

/// Runs the candidate over reused buffers, so that sweeps do not allocate per call, and checks the
/// output bound (one unit per input byte, three bytes per input unit) and the untouched suffix of
/// the destination on every call.
struct Candidate {
    units: Vec<u16>,
    bytes: Vec<u8>,
}

impl Candidate {
    fn new() -> Self {
        Self {
            units: Vec::new(),
            bytes: Vec::new(),
        }
    }

    fn decode(&mut self, input: &[u8], variant: Utf8Variant) -> Result<&[u16], String> {
        let required = utf8_decode_measure(input, variant);
        if required > input.len() {
            return Err(format!(
                "measured {required} units for {} bytes",
                input.len()
            ));
        }
        self.units.clear();
        self.units.resize(input.len() + GUARD, UNIT_SENTINEL);
        match utf8_decode_write(input, variant, &mut self.units[..input.len()]) {
            Ok(written) if written == required => {}
            other => return Err(format!("measured {required} units but wrote {other:?}")),
        }
        if self.units[required..]
            .iter()
            .any(|&unit| unit != UNIT_SENTINEL)
        {
            return Err("changed units after the measured length".to_owned());
        }
        Ok(&self.units[..required])
    }

    fn encode(&mut self, input: &[u16], variant: Utf8Variant) -> Result<(&[u8], bool), String> {
        let measured = utf8_encode_measure(input, variant);
        let bound = input.len() * 3;
        if measured.length > bound {
            return Err(format!(
                "measured {} bytes for {} units",
                measured.length,
                input.len()
            ));
        }
        self.bytes.clear();
        self.bytes.resize(bound + GUARD, BYTE_SENTINEL);
        match utf8_encode_write(input, variant, &mut self.bytes[..bound]) {
            Ok(written) if written == measured => {}
            other => return Err(format!("measured {measured:?} but wrote {other:?}")),
        }
        if self.bytes[measured.length..]
            .iter()
            .any(|&byte| byte != BYTE_SENTINEL)
        {
            return Err("changed bytes after the measured length".to_owned());
        }
        Ok((&self.bytes[..measured.length], measured.lossy))
    }
}

/// A sweep keeps its first few mismatches and then only counts, so one wrong rule does not print a
/// million lines. It also counts the inputs it examined, so a sweep that stops covering what it
/// claims to cover fails.
#[derive(Default)]
struct Sweep {
    examined: usize,
    mismatches: usize,
    first: Vec<String>,
}

impl Sweep {
    fn report(&mut self, description: String) {
        self.mismatches += 1;
        if self.first.len() < 8 {
            self.first.push(description);
        }
    }

    fn finish(self, expected_examined: usize) {
        assert_eq!(
            self.examined, expected_examined,
            "the sweep covered a different number of inputs"
        );
        assert_eq!(
            self.mismatches, 0,
            "{} mismatches, the first ones: {:#?}",
            self.mismatches, self.first
        );
    }
}

fn check_decode(candidate: &mut Candidate, sweep: &mut Sweep, input: &[u8], variant: Utf8Variant) {
    sweep.examined += 1;
    let expected = model::decode(input, variant);
    match candidate.decode(input, variant) {
        Ok(actual) if actual == expected => {}
        Ok(actual) => sweep.report(format!(
            "{variant:?} decode {}: {} instead of {}",
            hex_bytes(input),
            hex_units(actual),
            hex_units(&expected)
        )),
        Err(reason) => sweep.report(format!("{variant:?} decode {}: {reason}", hex_bytes(input))),
    }
}

fn check_encode(candidate: &mut Candidate, sweep: &mut Sweep, input: &[u16], variant: Utf8Variant) {
    sweep.examined += 1;
    let (expected_bytes, expected_lossy) = model::encode(input, variant);
    match candidate.encode(input, variant) {
        Ok((bytes, lossy)) if bytes == expected_bytes && lossy == expected_lossy => {}
        Ok((bytes, lossy)) => sweep.report(format!(
            "{variant:?} encode {}: {} lossy={lossy} instead of {} lossy={expected_lossy}",
            hex_units(input),
            hex_bytes(bytes),
            hex_bytes(&expected_bytes)
        )),
        Err(reason) => sweep.report(format!("{variant:?} encode {}: {reason}", hex_units(input))),
    }
}

/// Decoding loses nothing: every byte that starts no valid sequence is one binary unit, and a
/// binary unit is written back as the byte it came from.
fn check_round_trip(
    candidate: &mut Candidate,
    sweep: &mut Sweep,
    input: &[u8],
    variant: Utf8Variant,
) {
    // A failing decode is already reported by `check_decode`.
    let Ok(units) = candidate.decode(input, variant).map(<[u16]>::to_vec) else {
        return;
    };
    match candidate.encode(&units, variant) {
        Ok((bytes, false)) if bytes == input => {}
        Ok((bytes, lossy)) => sweep.report(format!(
            "{variant:?} round trip {} -> {} -> {} lossy={lossy}",
            hex_bytes(input),
            hex_units(&units),
            hex_bytes(bytes)
        )),
        Err(reason) => sweep.report(format!(
            "{variant:?} round trip {}: {reason}",
            hex_bytes(input)
        )),
    }
}

/// The number of sequences of up to `max_length` elements over an alphabet of `alphabet_length`.
fn sequence_count(alphabet_length: usize, max_length: usize) -> usize {
    (0..=max_length)
        .map(|length| alphabet_length.pow(u32::try_from(length).expect("a small length")))
        .sum()
}

/// Calls `visit` with every sequence of up to `max_length` elements of `alphabet`, the empty one
/// first.
fn for_each_sequence<T: Copy>(alphabet: &[T], max_length: usize, visit: &mut impl FnMut(&[T])) {
    fn extend<T: Copy>(
        alphabet: &[T],
        remaining: usize,
        current: &mut Vec<T>,
        visit: &mut impl FnMut(&[T]),
    ) {
        visit(current);
        if remaining == 0 {
            return;
        }
        for &element in alphabet {
            current.push(element);
            extend(alphabet, remaining - 1, current, visit);
            current.pop();
        }
    }
    extend(
        alphabet,
        max_length,
        &mut Vec::with_capacity(max_length),
        visit,
    );
}

/// Every CESU-8 surrogate pair written as two three-byte forms, with each byte taken on both sides
/// of every boundary of the rule.
fn six_byte_forms() -> Vec<[u8; 6]> {
    let mut forms = Vec::new();
    for &high_middle in &FORM_MIDDLES {
        for &high_tail in &FORM_TAILS {
            for &low_lead in &SECOND_FORM_LEADS {
                for &low_middle in &FORM_MIDDLES {
                    for &low_tail in &FORM_TAILS {
                        forms.push([0xED, high_middle, high_tail, low_lead, low_middle, low_tail]);
                    }
                }
            }
        }
    }
    forms
}

#[test]
fn corpus_is_well_formed_and_covers_the_required_classes() {
    let corpus = load_corpus().expect("the shared corpus is valid");
    // Adding or removing a row is a deliberate change of these numbers.
    assert_eq!(corpus.decode.len(), 130);
    assert_eq!(corpus.encode.len(), 68);

    let decode_classes: BTreeSet<_> = corpus
        .decode
        .iter()
        .map(|case| case.class.as_str())
        .collect();
    assert_eq!(
        decode_classes,
        BTreeSet::from([
            "ascii",
            "bad-continuation",
            "bom",
            "cesu8-lone",
            "cesu8-pair",
            "empty",
            "four-byte",
            "lone-continuation",
            "mixed",
            "non-character",
            "nul",
            "out-of-range",
            "overlong",
            "resume",
            "surrogate-encoding",
            "three-byte",
            "truncated",
            "two-byte",
        ])
    );
    let encode_classes: BTreeSet<_> = corpus
        .encode
        .iter()
        .map(|case| case.class.as_str())
        .collect();
    assert_eq!(
        encode_classes,
        BTreeSet::from([
            "ascii",
            "binary-low",
            "bmp",
            "empty",
            "lone-high-lossy",
            "lone-low-lossy",
            "mixed-lossy",
            "pair",
            "round-trip",
            "surrogate-edges",
        ])
    );

    let count = |variants: Variants, selected: &[Variants]| selected.contains(&variants);
    for (name, rows) in [
        (
            "decode",
            corpus
                .decode
                .iter()
                .map(|case| case.variants)
                .collect::<Vec<_>>(),
        ),
        (
            "encode",
            corpus
                .encode
                .iter()
                .map(|case| case.variants)
                .collect::<Vec<_>>(),
        ),
    ] {
        let expected = if name == "decode" {
            [88, 21, 21]
        } else {
            [40, 14, 14]
        };
        let actual = [Variants::Both, Variants::Utf8, Variants::Cesu8]
            .map(|variants| rows.iter().filter(|&&row| count(row, &[variants])).count());
        assert_eq!(actual, expected, "{name} rows by variants");
    }
    assert_eq!(
        corpus.encode.iter().filter(|case| case.lossy).count(),
        21,
        "lossy encode rows"
    );
    assert!(corpus
        .decode
        .iter()
        .all(|case| case.expected.len() <= case.input.len()));
    assert!(corpus
        .encode
        .iter()
        .all(|case| case.expected.len() <= case.input.len() * 3));
}

#[test]
fn decode_rows_match_the_candidate_and_the_model_in_every_applicable_variant() {
    let corpus = load_corpus().expect("the shared corpus is valid");
    let mut candidate = Candidate::new();
    let mut compared = 0;
    for case in &corpus.decode {
        for variant in case.variants.each() {
            let label = format!("{}/{variant:?}", case.id);
            let actual = candidate
                .decode(&case.input, variant)
                .unwrap_or_else(|reason| panic!("{label}: {reason}"));
            assert_eq!(
                hex_units(actual),
                hex_units(&case.expected),
                "candidate {label}"
            );
            assert_eq!(
                hex_units(&model::decode(&case.input, variant)),
                hex_units(&case.expected),
                "model {label}"
            );
            compared += 1;
        }
    }
    assert_eq!(compared, 88 * 2 + 21 + 21);
}

#[test]
fn encode_rows_match_the_candidate_and_the_model_in_every_applicable_variant() {
    let corpus = load_corpus().expect("the shared corpus is valid");
    let mut candidate = Candidate::new();
    let mut compared = 0;
    for case in &corpus.encode {
        for variant in case.variants.each() {
            let label = format!("{}/{variant:?}", case.id);
            let (actual, lossy) = candidate
                .encode(&case.input, variant)
                .unwrap_or_else(|reason| panic!("{label}: {reason}"));
            assert_eq!(
                hex_bytes(actual),
                hex_bytes(&case.expected),
                "candidate {label}"
            );
            assert_eq!(lossy, case.lossy, "candidate loss {label}");
            let (bytes, model_lossy) = model::encode(&case.input, variant);
            assert_eq!(
                hex_bytes(&bytes),
                hex_bytes(&case.expected),
                "model {label}"
            );
            assert_eq!(model_lossy, case.lossy, "model loss {label}");
            compared += 1;
        }
    }
    assert_eq!(compared, 40 * 2 + 14 + 14);
}

#[test]
fn every_row_is_written_only_when_the_destination_holds_all_of_it() {
    let corpus = load_corpus().expect("the shared corpus is valid");
    for case in &corpus.decode {
        for variant in case.variants.each() {
            let label = format!("{}/{variant:?}", case.id);
            let required = case.expected.len();
            assert_eq!(
                utf8_decode_measure(&case.input, variant),
                required,
                "{label}"
            );
            if required > 0 {
                let mut short = vec![UNIT_SENTINEL; required];
                assert_eq!(
                    utf8_decode_write(&case.input, variant, &mut short[..required - 1]),
                    Err(OutputTooSmall { required }),
                    "{label}"
                );
                assert!(short.iter().all(|&unit| unit == UNIT_SENTINEL), "{label}");
            }
            let mut roomy = vec![UNIT_SENTINEL; required + 3];
            assert_eq!(
                utf8_decode_write(&case.input, variant, &mut roomy),
                Ok(required),
                "{label}"
            );
            assert_eq!(roomy[..required], case.expected, "{label}");
            assert!(
                roomy[required..].iter().all(|&unit| unit == UNIT_SENTINEL),
                "{label}"
            );
        }
    }
    for case in &corpus.encode {
        for variant in case.variants.each() {
            let label = format!("{}/{variant:?}", case.id);
            let required = case.expected.len();
            let outcome = Utf8Encoded {
                length: required,
                lossy: case.lossy,
            };
            assert_eq!(
                utf8_encode_measure(&case.input, variant),
                outcome,
                "{label}"
            );
            if required > 0 {
                let mut short = vec![BYTE_SENTINEL; required];
                assert_eq!(
                    utf8_encode_write(&case.input, variant, &mut short[..required - 1]),
                    Err(OutputTooSmall { required }),
                    "{label}"
                );
                assert!(short.iter().all(|&byte| byte == BYTE_SENTINEL), "{label}");
            }
            let mut roomy = vec![BYTE_SENTINEL; required + 3];
            assert_eq!(
                utf8_encode_write(&case.input, variant, &mut roomy),
                Ok(outcome),
                "{label}"
            );
            assert_eq!(roomy[..required], case.expected, "{label}");
            assert!(
                roomy[required..].iter().all(|&byte| byte == BYTE_SENTINEL),
                "{label}"
            );
        }
    }
}

#[test]
fn empty_outputs_need_no_room() {
    for variant in VARIANTS {
        assert_eq!(utf8_decode_write(&[], variant, &mut []), Ok(0));
        assert_eq!(
            utf8_encode_write(&[], variant, &mut []),
            Ok(Utf8Encoded {
                length: 0,
                lossy: false
            })
        );
        assert_eq!(
            utf8_decode_write(b"A", variant, &mut []),
            Err(OutputTooSmall { required: 1 })
        );
        assert_eq!(
            utf8_encode_write(&[0x41], variant, &mut []),
            Err(OutputTooSmall { required: 1 })
        );
    }
}

#[test]
fn every_byte_and_byte_pair_decodes_like_the_model_and_round_trips() {
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for first in 0..=u8::MAX {
            // `None` stands for the single byte on its own.
            for second in (0..=u8::MAX).map(Some).chain([None]) {
                let mut input = vec![first];
                input.extend(second);
                check_decode(&mut candidate, &mut sweep, &input, variant);
                check_round_trip(&mut candidate, &mut sweep, &input, variant);
            }
        }
    }
    sweep.finish(2 * 256 * 257);
}

#[test]
fn boundary_byte_sequences_decode_like_the_model_and_round_trip() {
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for_each_sequence(&BOUNDARY_BYTES, 4, &mut |input| {
            check_decode(&mut candidate, &mut sweep, input, variant);
            check_round_trip(&mut candidate, &mut sweep, input, variant);
        });
    }
    sweep.finish(2 * sequence_count(BOUNDARY_BYTES.len(), 4));
}

#[test]
fn every_second_and_third_byte_after_a_multi_byte_lead_decodes_like_the_model() {
    const THREE_BYTE_LEADS: [u8; 5] = [0xE0, 0xE1, 0xED, 0xEE, 0xEF];
    const FOUR_BYTE_LEADS: [u8; 4] = [0xF0, 0xF1, 0xF4, 0xF5];
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for lead in THREE_BYTE_LEADS {
            for second in 0..=u8::MAX {
                for third in 0..=u8::MAX {
                    let input = [lead, second, third];
                    check_decode(&mut candidate, &mut sweep, &input, variant);
                    check_round_trip(&mut candidate, &mut sweep, &input, variant);
                }
            }
        }
        for lead in FOUR_BYTE_LEADS {
            for second in 0..=u8::MAX {
                for third in BOUNDARY_BYTES {
                    for fourth in BOUNDARY_BYTES {
                        let input = [lead, second, third, fourth];
                        check_decode(&mut candidate, &mut sweep, &input, variant);
                        check_round_trip(&mut candidate, &mut sweep, &input, variant);
                    }
                }
            }
        }
    }
    sweep.finish(
        2 * (THREE_BYTE_LEADS.len() * 256 * 256
            + FOUR_BYTE_LEADS.len() * 256 * BOUNDARY_BYTES.len() * BOUNDARY_BYTES.len()),
    );
}

#[test]
fn cesu8_surrogate_forms_and_their_truncations_decode_like_the_model() {
    let forms = six_byte_forms();
    assert_eq!(
        forms.len(),
        FORM_MIDDLES.len()
            * FORM_TAILS.len()
            * SECOND_FORM_LEADS.len()
            * FORM_MIDDLES.len()
            * FORM_TAILS.len()
    );
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for form in &forms {
            for length in 0..=form.len() {
                for follower in FOLLOWERS {
                    let mut input = form[..length].to_vec();
                    input.extend_from_slice(follower);
                    check_decode(&mut candidate, &mut sweep, &input, variant);
                    check_round_trip(&mut candidate, &mut sweep, &input, variant);
                }
            }
        }
    }
    sweep.finish(2 * forms.len() * 7 * FOLLOWERS.len());
}

#[test]
fn every_unit_alone_and_between_neighbours_encodes_like_the_model() {
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for unit in 0..=u16::MAX {
            for input in [
                &[unit][..],
                &[0x41, unit],
                &[unit, 0x41],
                &[0xD800, unit],
                &[unit, 0xDC00],
            ] {
                check_encode(&mut candidate, &mut sweep, input, variant);
            }
        }
    }
    sweep.finish(2 * 65_536 * 5);
}

#[test]
fn every_surrogate_pair_encodes_like_the_model() {
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for high in 0xD800..=0xDBFF {
            for low in 0xDC00..=0xDFFF {
                check_encode(&mut candidate, &mut sweep, &[high, low], variant);
            }
        }
    }
    sweep.finish(2 * 1024 * 1024);
}

#[test]
fn unit_sequences_over_the_surrogate_edges_encode_like_the_model() {
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for variant in VARIANTS {
        for_each_sequence(&UNIT_ALPHABET, 5, &mut |input| {
            check_encode(&mut candidate, &mut sweep, input, variant);
        });
    }
    sweep.finish(2 * sequence_count(UNIT_ALPHABET.len(), 5));
}

/// A deterministic generator for the pseudo-random inputs.
struct Lcg(u64);

impl Lcg {
    fn next(&mut self, bound: usize) -> usize {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        usize::try_from(self.0 >> 33).expect("31 bits") % bound
    }
}

#[test]
fn pseudo_random_inputs_match_the_model_and_round_trip() {
    const TOKENS: [&[u8]; 14] = [
        b"A",
        b"\n",
        b"\0",
        &[0xC3, 0xA9],
        &[0xE3, 0x81, 0x82],
        &[0xEF, 0xBF, 0xBF],
        &[0xF0, 0x9F, 0x98, 0x80],
        &[0xF4, 0x8F, 0xBF, 0xBF],
        &[0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80],
        &[0xED, 0xA0, 0xBD],
        &[0xED, 0xB8, 0x80],
        &[0xE3, 0x81],
        &[0x80],
        &[0xFF],
    ];
    const UNIT_TOKENS: [&[u16]; 10] = [
        &[0x41],
        &[0x3042],
        &[0xD83D, 0xDE00],
        &[0xD83D],
        &[0xDE00],
        &[0xDC80],
        &[0xDC41],
        &[0xDFFF, 0xD800],
        &[0xFFFF],
        &[0],
    ];
    let mut random = Lcg(0x5EED_0443);
    let mut candidate = Candidate::new();
    let mut sweep = Sweep::default();
    for _ in 0..4000 {
        let mut bytes = Vec::new();
        let mut units = Vec::new();
        for _ in 0..random.next(48) {
            bytes.extend_from_slice(TOKENS[random.next(TOKENS.len())]);
            units.extend_from_slice(UNIT_TOKENS[random.next(UNIT_TOKENS.len())]);
        }
        for variant in VARIANTS {
            check_decode(&mut candidate, &mut sweep, &bytes, variant);
            check_round_trip(&mut candidate, &mut sweep, &bytes, variant);
            check_encode(&mut candidate, &mut sweep, &units, variant);
        }
    }
    sweep.finish(4000 * 2 * 2);
}

#[test]
fn loader_rejects_malformed_corpus_text() {
    const HEADER: &str = "id\tclass\tdirection\tvariants\tinput\toutput\tlossy";
    const GOOD_ROW: &str = "fixture\tascii\tdecode\tboth\t41\t0041\t0";

    fn expect_error(text: &str, reason: &str) {
        let error = parse_corpus(text, "origin").expect_err("the corpus must be rejected");
        assert!(
            error.ends_with(&format!(": {reason}")),
            "expected {reason:?}, got {error:?} for {text:?}"
        );
    }

    let good = parse_corpus(&format!("{HEADER}\n{GOOD_ROW}\n"), "origin").expect("a valid row");
    assert_eq!(good.decode.len(), 1);
    assert_eq!(good.decode[0].input, [0x41]);
    assert_eq!(good.decode[0].expected, [0x0041]);
    assert_eq!(good.decode[0].variants, Variants::Both);

    expect_error("# comment\n\n", "missing header");
    expect_error("wrong header\n", "invalid header");
    expect_error(
        &format!("{HEADER}\n{GOOD_ROW}\n{GOOD_ROW}\n"),
        "duplicate id: fixture",
    );

    let malformed_rows = [
        (
            "fixture\tascii\tdecode\tboth\t41\t0041",
            "expected seven tab-separated fields",
        ),
        ("\tascii\tdecode\tboth\t41\t0041\t0", "invalid id"),
        ("Fixture\tascii\tdecode\tboth\t41\t0041\t0", "invalid id"),
        ("fixture\t\tdecode\tboth\t41\t0041\t0", "empty class"),
        (
            "fixture\tascii\tround-trip\tboth\t41\t0041\t0",
            "unknown direction: round-trip",
        ),
        (
            "fixture\tascii\tdecode\tutf16\t41\t0041\t0",
            "unknown variants: utf16",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t0041\t2",
            "lossy must be 0 or 1",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t0041\t",
            "lossy must be 0 or 1",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t0041\t+1",
            "lossy must be 0 or 1",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t0041\t1",
            "a decode row cannot be lossy",
        ),
        (
            "fixture\tascii\tdecode\tboth\t4\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t041\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t4G\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t+1\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41  42\t0041 0042\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t- 41\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t41\t0",
            "invalid unit token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t00041\t0",
            "invalid unit token",
        ),
        (
            "fixture\tascii\tdecode\tboth\t41\t00G1\t0",
            "invalid unit token",
        ),
        (
            "fixture\tbmp\tencode\tboth\t41\t41\t0",
            "invalid unit token",
        ),
        (
            "fixture\tbmp\tencode\tboth\t0041\t0041\t0",
            "invalid byte token",
        ),
        (
            "fixture\tbmp\tencode\tboth\t0041\t4x\t0",
            "invalid byte token",
        ),
    ];
    for (row, reason) in malformed_rows {
        expect_error(&format!("{HEADER}\n{row}\n"), reason);
    }
    let error = parse_corpus(&format!("# comment\n{HEADER}\nbad row\n"), "origin")
        .expect_err("the third line is malformed");
    assert_eq!(error, "origin:3: expected seven tab-separated fields");

    let encode = parse_corpus(
        &format!("{HEADER}\nfixture\tbmp\tencode\tcesu8\t0041 D800\t41 3F\t1\n"),
        "origin",
    )
    .expect("a valid encode row");
    assert_eq!(encode.encode.len(), 1);
    assert_eq!(encode.encode[0].input, [0x0041, 0xD800]);
    assert_eq!(encode.encode[0].expected, [0x41, 0x3F]);
    assert!(encode.encode[0].lossy);
    assert_eq!(encode.encode[0].variants, Variants::Cesu8);
}

#[test]
fn loader_reads_the_utf8_corpus_file_with_either_line_ending() {
    let path = corpus_path();
    assert!(
        path.ends_with("fixtures/utf8-codec-corpus.tsv"),
        "unexpected corpus path {path:?}"
    );
    let text = fs::read_to_string(&path).expect("the shared corpus is readable");
    let crlf_text = text.replace("\r\n", "\n").replace('\n', "\r\n");
    assert_eq!(
        parse_corpus(&crlf_text, &path.display().to_string()).expect("CRLF is supported"),
        load_corpus().expect("the shared corpus is valid")
    );
}
