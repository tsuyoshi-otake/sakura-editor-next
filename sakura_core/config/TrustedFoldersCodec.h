/*! @file
 * @brief Bounded JSON codec for the durable Trusted Folders and Workspaces list.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/ITrustedFoldersStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace config {

inline constexpr std::uint32_t kTrustedFoldersFormatVersion = 1;
//! An upper bound on how many separate folders one profile may have trusted.
inline constexpr std::size_t kMaximumTrustedFolderEntries = 256;
//! Per-entry URI bound, in UTF-8 bytes of the canonical serialized form.
inline constexpr std::size_t kMaximumTrustedFolderUriBytes = 2048;

//! Encoding and decoding never expose a partially accepted document.
enum class ETrustedFoldersCodecStatus : std::uint8_t {
	Succeeded,
	InvalidSnapshot,
	CorruptPayload,
	UnsupportedSchema,
	PayloadTooLarge,
};

struct TrustedFoldersEncodeResult final {
	ETrustedFoldersCodecStatus status = ETrustedFoldersCodecStatus::InvalidSnapshot;
	std::string payload;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ETrustedFoldersCodecStatus::Succeeded;
	}
};

struct TrustedFoldersDecodeResult final {
	ETrustedFoldersCodecStatus status = ETrustedFoldersCodecStatus::CorruptPayload;
	std::optional<TrustedFoldersSnapshot> snapshot;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ETrustedFoldersCodecStatus::Succeeded && snapshot.has_value();
	}
};

/*!
	@brief Machine-owned list format, separate from user-authored Settings JSONC.

	The document is @c {"formatVersion":1,"entries":[{"uri":"...","includesDescendants":bool}]}.
	A URI is stored in its canonical @c platform::uri::Uri serialized form and is
	re-parsed on decode, so a payload that has been hand-edited into something the
	URI model cannot represent is rejected rather than silently reinterpreted.

	@par Duplicate entries are accepted, not rejected.
	Unlike the layout memento, where two parts sharing a stable ID is structurally
	incoherent, two identical trust entries are merely redundant: the policy
	resolves exactly the same answer either way. Rejecting them would let a benign
	redundancy invalidate the entire list and silently untrust every folder the
	user had granted. Deduplication belongs to whoever adds an entry, not to the
	codec, and the codec stays a faithful round trip.

	@par What "same" means is not decided here either.
	@c WorkspaceTrustEntryCovers owns URI equivalence, including case folding on
	Windows paths. The codec must not grow a second, weaker notion of identity.
 */
class CTrustedFoldersCodec final {
public:
	[[nodiscard]] static TrustedFoldersEncodeResult Encode(
		const TrustedFoldersSnapshot& snapshot) noexcept;
	[[nodiscard]] static TrustedFoldersDecodeResult Decode(
		std::string_view payload) noexcept;
};

} // namespace config
