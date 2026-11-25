/*! @file @brief ABI version and typed status shared by the Rust document-codec candidates. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>

// What every document-codec candidate export has in common (#441, #443): the ABI version and the typed status.
// The values mirror rust/native/sakura_native_ffi/src/document_codec_abi.rs, which pins the same wire numbers.
// What a capability adds (its descriptors, bounds and result) stays in the header of its own adapter.

constexpr std::uint32_t SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1 = 1;

//! Typed status of the document-codec exports. The numbers share the candidate status space of the URI candidate.
enum class SakuraDocumentCodecStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	InvalidCapacity = 2,
	InternalError = 12,
	// Adapter-only result when the Rust candidate is not linked. The Rust exports never return this value.
	Unsupported = 13,
};
