//! What a row of `utf8-codec-corpus.tsv` means. Where the file is and how its lines are read stays in
//! `fixture_tsv`; `model` is the independent reading of the policy that the rows freeze.

pub mod model;

use crate::fixture_tsv::{fixture_path, for_each_row, parse_hex, read_fixture, UniqueIds};
use sakura_document_codec::Utf8Variant;
use std::path::PathBuf;

const FILE_NAME: &str = "utf8-codec-corpus.tsv";
const HEADER: &str = "id\tclass\tdirection\tvariants\tinput\toutput\tlossy";

/// The variants a row applies to.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum Variants {
    /// UTF-8 and CESU-8 give the same expectation.
    Both,
    /// UTF-8 only.
    Utf8,
    /// CESU-8 only.
    Cesu8,
}

impl Variants {
    /// The variants the row applies to, UTF-8 first.
    pub fn each(self) -> impl Iterator<Item = Utf8Variant> {
        [Utf8Variant::Utf8, Utf8Variant::Cesu8]
            .into_iter()
            .filter(move |variant| {
                matches!(
                    (self, variant),
                    (Variants::Both, _)
                        | (Variants::Utf8, Utf8Variant::Utf8)
                        | (Variants::Cesu8, Utf8Variant::Cesu8)
                )
            })
    }
}

/// One decode row: bytes and the UTF-16 units they decode to. Decoding never loses information.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DecodeCase {
    pub id: String,
    pub class: String,
    pub variants: Variants,
    pub input: Vec<u8>,
    pub expected: Vec<u16>,
}

/// One encode row: UTF-16 units and the bytes they encode to, with the loss the encoding reports.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EncodeCase {
    pub id: String,
    pub class: String,
    pub variants: Variants,
    pub input: Vec<u16>,
    pub expected: Vec<u8>,
    pub lossy: bool,
}

/// Every row of the corpus, in file order, split by direction.
#[derive(Debug, Default, Eq, PartialEq)]
pub struct Corpus {
    pub decode: Vec<DecodeCase>,
    pub encode: Vec<EncodeCase>,
}

pub fn corpus_path() -> PathBuf {
    fixture_path(FILE_NAME)
}

pub fn load_corpus() -> Result<Corpus, String> {
    let (origin, text) = read_fixture(FILE_NAME)?;
    parse_corpus(&text, &origin)
}

pub fn parse_corpus(text: &str, origin: &str) -> Result<Corpus, String> {
    let mut corpus = Corpus::default();
    let mut ids = UniqueIds::default();
    for_each_row(text, origin, HEADER, |line| {
        parse_row(line, &mut ids, &mut corpus)
    })?;
    Ok(corpus)
}

fn parse_row<'a>(
    line: &'a str,
    ids: &mut UniqueIds<'a>,
    corpus: &mut Corpus,
) -> Result<(), String> {
    let fields: Vec<_> = line.split('\t').collect();
    let &[id, class, direction, variants, input, output, lossy] = fields.as_slice() else {
        return Err("expected seven tab-separated fields".into());
    };
    ids.insert(id)?;
    if class.is_empty() {
        return Err("empty class".into());
    }
    let variants = parse_variants(variants)?;
    let lossy = match lossy {
        "0" => false,
        "1" => true,
        _ => return Err("lossy must be 0 or 1".into()),
    };
    match direction {
        "decode" => {
            if lossy {
                return Err("a decode row cannot be lossy".into());
            }
            corpus.decode.push(DecodeCase {
                id: id.to_owned(),
                class: class.to_owned(),
                variants,
                input: tokens(input, 2, "invalid byte token")?,
                expected: tokens(output, 4, "invalid unit token")?,
            });
        }
        "encode" => corpus.encode.push(EncodeCase {
            id: id.to_owned(),
            class: class.to_owned(),
            variants,
            input: tokens(input, 4, "invalid unit token")?,
            expected: tokens(output, 2, "invalid byte token")?,
            lossy,
        }),
        _ => return Err(format!("unknown direction: {direction}")),
    }
    Ok(())
}

fn parse_variants(text: &str) -> Result<Variants, String> {
    match text {
        "both" => Ok(Variants::Both),
        "utf8" => Ok(Variants::Utf8),
        "cesu8" => Ok(Variants::Cesu8),
        _ => Err(format!("unknown variants: {text}")),
    }
}

/// Reads space-separated hex tokens of exactly `digits` digits; `-` is the empty sequence.
fn tokens<T: TryFrom<u32>>(text: &str, digits: usize, reason: &str) -> Result<Vec<T>, String> {
    if text == "-" {
        return Ok(Vec::new());
    }
    text.split(' ')
        .map(|token| {
            if token.len() != digits {
                return Err(reason.to_owned());
            }
            T::try_from(parse_hex(token, reason)?).map_err(|_| reason.to_owned())
        })
        .collect()
}
