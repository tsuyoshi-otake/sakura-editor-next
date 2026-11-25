//! An independent reading of the policy the corpus freezes, written from the rules and the standard
//! library rather than from the candidate. Well-formed UTF-8 is whatever `str::from_utf8` accepts,
//! scalar values are converted with `char::encode_utf16`/`encode_utf8` and pairs are found with
//! `char::decode_utf16`; only the policy of the C++ converters is spelled out here: a byte that
//! starts no valid sequence is the binary unit `0xDC00 + byte`, CESU-8 has no four-byte form and
//! reads a surrogate pair only as two three-byte forms, a lone unit in `U+DC00..=U+DCFF` is its low
//! byte and any other lone surrogate is `?` with loss.

use sakura_document_codec::Utf8Variant;

/// The UTF-16 units `bytes` decode to.
pub fn decode(bytes: &[u8], variant: Utf8Variant) -> Vec<u16> {
    let mut units = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let rest = &bytes[at..];
        if variant == Utf8Variant::Cesu8 {
            if let Some((high, low)) = surrogate_pair(rest) {
                units.extend([high, low]);
                at += 6;
                continue;
            }
        }
        match well_formed_length(rest) {
            Some(length) if length < 4 || variant == Utf8Variant::Utf8 => {
                let text = std::str::from_utf8(&rest[..length]).expect("a well-formed prefix");
                let scalar = text
                    .chars()
                    .next()
                    .expect("a well-formed prefix is not empty");
                units.extend_from_slice(scalar.encode_utf16(&mut [0; 2]));
                at += length;
            }
            _ => {
                units.push(0xDC00 + u16::from(rest[0]));
                at += 1;
            }
        }
    }
    units
}

/// The bytes `units` encode to and whether any unit was lost.
pub fn encode(units: &[u16], variant: Utf8Variant) -> (Vec<u8>, bool) {
    let mut bytes = Vec::new();
    let mut lossy = false;
    for decoded in char::decode_utf16(units.iter().copied()) {
        match decoded {
            Ok(scalar) if variant == Utf8Variant::Cesu8 && u32::from(scalar) > 0xFFFF => {
                for unit in scalar.encode_utf16(&mut [0; 2]).iter() {
                    bytes.extend(three_byte_form(*unit));
                }
            }
            Ok(scalar) => bytes.extend_from_slice(scalar.encode_utf8(&mut [0; 4]).as_bytes()),
            Err(unpaired) => {
                let unit = unpaired.unpaired_surrogate();
                if (0xDC00..=0xDCFF).contains(&unit) {
                    bytes.push(unit.to_le_bytes()[0]);
                } else {
                    bytes.push(b'?');
                    lossy = true;
                }
            }
        }
    }
    (bytes, lossy)
}

/// The length of the well-formed UTF-8 sequence that starts `rest`, if one does.
fn well_formed_length(rest: &[u8]) -> Option<usize> {
    (1..=rest.len().min(4)).find(|&length| std::str::from_utf8(&rest[..length]).is_ok())
}

/// Reads `rest` as two three-byte forms that decode to a high and then a low surrogate.
fn surrogate_pair(rest: &[u8]) -> Option<(u16, u16)> {
    let high = three_byte_unit(rest.get(..3)?)?;
    let low = three_byte_unit(rest.get(3..6)?)?;
    ((0xD800..=0xDBFF).contains(&high) && (0xDC00..=0xDFFF).contains(&low)).then_some((high, low))
}

/// The unit a three-byte form `1110xxxx 10xxxxxx 10xxxxxx` stands for, whatever its value.
fn three_byte_unit(form: &[u8]) -> Option<u16> {
    match *form {
        [lead, middle, tail]
            if lead & 0xF0 == 0xE0 && middle & 0xC0 == 0x80 && tail & 0xC0 == 0x80 =>
        {
            Some(
                (u16::from(lead & 0x0F) << 12)
                    | (u16::from(middle & 0x3F) << 6)
                    | u16::from(tail & 0x3F),
            )
        }
        _ => None,
    }
}

/// The three-byte form of a UTF-16 unit, whatever its value.
fn three_byte_form(unit: u16) -> [u8; 3] {
    [
        0b1110_0000 | (unit >> 12) as u8,
        0b1000_0000 | ((unit >> 6) & 0b11_1111) as u8,
        0b1000_0000 | (unit & 0b11_1111) as u8,
    ]
}
