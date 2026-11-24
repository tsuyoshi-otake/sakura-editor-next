#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::all)]
#![deny(missing_docs)]

//! Safe, allocation-free document file codecs for the native product.
//!
//! The crate owns byte-level policy that the C++ document loader applies today,
//! as comparison-only candidates. It exports no C ABI and installs no panic
//! handler: `sakura-native-ffi` is the only crate that exposes native symbols.
//! C++ stays the production authority until an adoption Issue says otherwise.

mod encoded_eol;

pub use encoded_eol::{
    scan_encoded_eol, EncodedEolFormat, EncodedEolScan, EncodedEolScanError, EolKind, EolRecord,
};
