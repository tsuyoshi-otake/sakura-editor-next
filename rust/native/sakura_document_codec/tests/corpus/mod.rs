use sakura_document_codec::{EncodedEolFormat, EolKind, EolRecord};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

const HEADER: &str = "id\tclass\tformats\tstart\tunits\trecords_off\trecords_on";
const FORMATS: [(&str, EncodedEolFormat); 4] = [
    ("utf16le", EncodedEolFormat::Utf16Le),
    ("utf16be", EncodedEolFormat::Utf16Be),
    ("utf32le", EncodedEolFormat::Utf32Le),
    ("utf32be", EncodedEolFormat::Utf32Be),
];

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Case {
    pub id: String,
    pub class: String,
    pub format: EncodedEolFormat,
    pub extended_eol: bool,
    pub start: usize,
    pub bytes: Vec<u8>,
    pub records: Vec<EolRecord>,
    pub has_tail: bool,
}

impl Case {
    pub fn describe(&self) -> String {
        format!(
            "{}/{:?}/extended={}",
            self.id,
            self.format,
            u8::from(self.extended_eol)
        )
    }
}

pub fn corpus_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../fixtures/encoded-eol-corpus.tsv")
}

pub fn load_cases() -> Result<Vec<Case>, String> {
    let path = corpus_path();
    let origin = path.display().to_string();
    let mut input = File::open(&path).map_err(|_| format!("{origin}:1: cannot open corpus"))?;
    let mut text = String::new();
    if input.read_to_string(&mut text).is_err() {
        let line_number = text.split_terminator('\n').count() + 1;
        return Err(format!("{origin}:{line_number}: cannot read corpus"));
    }
    parse_corpus(&text, &origin)
}

pub fn parse_corpus(text: &str, origin: &str) -> Result<Vec<Case>, String> {
    let mut cases = Vec::new();
    let mut ids = BTreeSet::new();
    let mut line_number = 0;
    let mut saw_header = false;
    for (index, raw_line) in text.split_terminator('\n').enumerate() {
        line_number = index + 1;
        let line = raw_line.strip_suffix('\r').unwrap_or(raw_line);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if !saw_header {
            if line != HEADER {
                return Err(format!("{origin}:{line_number}: invalid header"));
            }
            saw_header = true;
            continue;
        }

        parse_row(line, &mut ids, &mut cases)
            .map_err(|reason| format!("{origin}:{line_number}: {reason}"))?;
    }
    if !saw_header {
        return Err(format!("{origin}:{}: missing header", line_number + 1));
    }
    Ok(cases)
}

fn parse_row<'a>(
    line: &'a str,
    ids: &mut BTreeSet<&'a str>,
    cases: &mut Vec<Case>,
) -> Result<(), String> {
    let fields: Vec<_> = line.split('\t').collect();
    let &[id, class, format_names, start, units, records_off, records_on] = fields.as_slice()
    else {
        return Err("expected seven tab-separated fields".into());
    };
    if id.is_empty()
        || !id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
    {
        return Err("invalid id".into());
    }
    if !ids.insert(id) {
        return Err(format!("duplicate id: {id}"));
    }
    if class.is_empty() {
        return Err("empty class".into());
    }
    let selected_formats = parse_formats(format_names)?;
    let start = parse_decimal(start, "invalid start")?;
    for ((_, format), selected) in FORMATS.into_iter().zip(selected_formats) {
        if !selected {
            continue;
        }
        let width = format.unit_bytes();
        let bytes = encode_units(units, format)?;
        let complete_units = bytes.len() / width;
        if start > complete_units {
            return Err("start is past the last complete unit".into());
        }
        for extended_eol in [false, true] {
            let expected = if extended_eol && records_on != "=" {
                records_on
            } else {
                records_off
            };
            cases.push(Case {
                id: id.to_owned(),
                class: class.to_owned(),
                format,
                extended_eol,
                start: start * width,
                records: parse_records(expected, start, complete_units, width)?,
                has_tail: !bytes.len().is_multiple_of(width),
                bytes: bytes.clone(),
            });
        }
    }
    Ok(())
}

fn parse_formats(text: &str) -> Result<[bool; FORMATS.len()], String> {
    match text {
        "all" => return Ok([true, true, true, true]),
        "utf16" => return Ok([true, true, false, false]),
        "utf32" => return Ok([false, false, true, true]),
        _ => {}
    }
    let mut selected = [false; FORMATS.len()];
    for name in text.split(',') {
        let index = FORMATS
            .iter()
            .position(|&(label, _)| label == name)
            .ok_or_else(|| format!("unknown format: {name}"))?;
        selected[index] = true;
    }
    Ok(selected)
}

fn parse_decimal(text: &str, reason: &str) -> Result<usize, String> {
    if text.is_empty() || !text.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err(reason.to_owned());
    }
    text.parse().map_err(|_| reason.to_owned())
}

fn parse_hex(text: &str, reason: &str) -> Result<u32, String> {
    if !text.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(reason.to_owned());
    }
    u32::from_str_radix(text, 16).map_err(|_| reason.to_owned())
}

fn parse_unit(token: &str, width: usize) -> Result<u32, String> {
    match token {
        "CR" => Ok(0x0D),
        "LF" => Ok(0x0A),
        "NEL" => Ok(0x85),
        "LS" => Ok(0x2028),
        "PS" => Ok(0x2029),
        "NUL" => Ok(0),
        "A" | "B" | "C" | "D" => Ok(u32::from(token.as_bytes()[0])),
        _ => {
            if let Some(hex) = token.strip_prefix('u').filter(|hex| hex.len() == 4) {
                return parse_hex(hex, "invalid 16-bit hex unit");
            }
            if let Some(hex) = token.strip_prefix('U').filter(|hex| hex.len() == 8) {
                if width == 2 {
                    return Err("32-bit unit in a UTF-16 format".into());
                }
                return parse_hex(hex, "invalid 32-bit hex unit");
            }
            Err(format!("invalid unit token: {token}"))
        }
    }
}

fn encode_units(text: &str, format: EncodedEolFormat) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    if text == "-" {
        return Ok(bytes);
    }
    let width = format.unit_bytes();
    let tokens: Vec<_> = text.split(' ').collect();
    for (index, &token) in tokens.iter().enumerate() {
        let (unit, byte_count) = if let Some((head, suffix)) = token.split_once('~') {
            let count = match suffix.as_bytes() {
                [count @ b'1'..=b'3'] if index + 1 == tokens.len() => usize::from(*count - b'0'),
                _ => {
                    return Err(
                        "partial unit must be the last token and specify 1 to 3 bytes".into(),
                    );
                }
            };
            if count >= width {
                return Err("partial unit is too wide for the format".into());
            }
            (head, count)
        } else {
            (token, width)
        };
        let value = parse_unit(unit, width)?;
        match format {
            EncodedEolFormat::Utf16Le => {
                bytes.extend_from_slice(&(value as u16).to_le_bytes()[..byte_count]);
            }
            EncodedEolFormat::Utf16Be => {
                bytes.extend_from_slice(&(value as u16).to_be_bytes()[..byte_count]);
            }
            EncodedEolFormat::Utf32Le => {
                bytes.extend_from_slice(&value.to_le_bytes()[..byte_count]);
            }
            EncodedEolFormat::Utf32Be => {
                bytes.extend_from_slice(&value.to_be_bytes()[..byte_count]);
            }
        }
    }
    Ok(bytes)
}

fn parse_records(
    text: &str,
    start: usize,
    complete_units: usize,
    unit_bytes: usize,
) -> Result<Vec<EolRecord>, String> {
    let mut records = Vec::new();
    if text == "-" {
        return Ok(records);
    }

    let mut next_allowed = start;
    for part in text.split(';') {
        let fields: Vec<_> = part.split(':').collect();
        let &[offset, width, name] = fields.as_slice() else {
            return Err("expected offset:width:KIND".into());
        };
        let offset = parse_decimal(offset, "invalid record offset")?;
        let width = parse_decimal(width, "invalid record width")?;
        let kind = match name {
            "CRLF" => EolKind::CrLf,
            "LF" => EolKind::Lf,
            "CR" => EolKind::Cr,
            "NEL" => EolKind::Nel,
            "LS" => EolKind::Ls,
            "PS" => EolKind::Ps,
            _ => return Err(format!("unknown record kind: {name}")),
        };
        let expected_width = if kind == EolKind::CrLf { 2 } else { 1 };
        if width != expected_width {
            return Err("record width does not match its kind".into());
        }
        if !(next_allowed..=complete_units).contains(&offset) || width > complete_units - offset {
            return Err("record is out of order, overlapping or outside the scan range".into());
        }
        records.push(EolRecord {
            offset: offset * unit_bytes,
            width: width * unit_bytes,
            kind,
        });
        next_allowed = offset + width;
    }
    Ok(records)
}
