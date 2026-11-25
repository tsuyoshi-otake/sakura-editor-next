#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::all)]
#![deny(missing_docs)]

//! Safe, allocation-free document file codecs for the native product.
//!
//! The crate owns byte-level policy that the C++ document loader applies today,
//! as comparison-only candidates: the encoded end-of-line scan and the one-shot
//! UTF-8 and CESU-8 conversion with loss reporting. It exports no C ABI and
//! installs no panic handler: `sakura-native-ffi` is the only crate that
//! exposes native symbols. C++ stays the production authority until an adoption
//! Issue says otherwise.

mod encoded_eol;
mod utf8_codec;

pub use encoded_eol::{
    scan_encoded_eol, EncodedEolFormat, EncodedEolScan, EncodedEolScanError, EolKind, EolRecord,
};
pub use utf8_codec::{
    utf8_decode_measure, utf8_decode_write, utf8_encode_measure, utf8_encode_write, OutputTooSmall,
    Utf8Encoded, Utf8Variant,
};
