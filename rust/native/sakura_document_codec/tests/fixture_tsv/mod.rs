//! Shared reading of the TSV corpora under `rust/native/fixtures`: where a file is, how its lines
//! are read (comments, blank lines, a trailing CR, the header, line numbers in errors), what an id
//! is and how a hex field is parsed. What a row means stays with the corpus that owns the file.

use std::collections::BTreeSet;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

/// Returns the path of a corpus file in `rust/native/fixtures`.
pub fn fixture_path(file_name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../fixtures")
        .join(file_name)
}

/// Reads a corpus file and returns the origin used in errors (its path) with its text.
pub fn read_fixture(file_name: &str) -> Result<(String, String), String> {
    let path = fixture_path(file_name);
    let origin = path.display().to_string();
    let mut input = File::open(&path).map_err(|_| format!("{origin}:1: cannot open corpus"))?;
    let mut text = String::new();
    if input.read_to_string(&mut text).is_err() {
        let line_number = text.split_terminator('\n').count() + 1;
        return Err(format!("{origin}:{line_number}: cannot read corpus"));
    }
    Ok((origin, text))
}

/// Passes every row line of `text` to `on_row` after the header, in order.
///
/// Blank lines and lines starting with `#` are ignored, a trailing CR is removed and the first
/// remaining line must equal `header`. A reason `on_row` returns is reported as
/// `origin:line: reason`.
pub fn for_each_row<'a>(
    text: &'a str,
    origin: &str,
    header: &str,
    mut on_row: impl FnMut(&'a str) -> Result<(), String>,
) -> Result<(), String> {
    let mut line_number = 0;
    let mut saw_header = false;
    for (index, raw_line) in text.split_terminator('\n').enumerate() {
        line_number = index + 1;
        let line = raw_line.strip_suffix('\r').unwrap_or(raw_line);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if !saw_header {
            if line != header {
                return Err(format!("{origin}:{line_number}: invalid header"));
            }
            saw_header = true;
            continue;
        }

        on_row(line).map_err(|reason| format!("{origin}:{line_number}: {reason}"))?;
    }
    if !saw_header {
        return Err(format!("{origin}:{}: missing header", line_number + 1));
    }
    Ok(())
}

/// The ids of one corpus: lower-case letters, digits and `-`, each used once.
#[derive(Default)]
pub struct UniqueIds<'a>(BTreeSet<&'a str>);

impl<'a> UniqueIds<'a> {
    /// Accepts `id`, or returns why it is not a valid new id.
    pub fn insert(&mut self, id: &'a str) -> Result<(), String> {
        if id.is_empty()
            || !id
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
        {
            return Err("invalid id".into());
        }
        if !self.0.insert(id) {
            return Err(format!("duplicate id: {id}"));
        }
        Ok(())
    }
}

/// Parses hex digits only, with no sign, prefix or separator.
pub fn parse_hex(text: &str, reason: &str) -> Result<u32, String> {
    if !text.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(reason.to_owned());
    }
    u32::from_str_radix(text, 16).map_err(|_| reason.to_owned())
}
