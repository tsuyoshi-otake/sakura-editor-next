/*! @file
 * @brief Bounded JSON codec for the durable per-workspace Workspace Trust memento.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/IWorkspaceTrustMementoStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace config {

inline constexpr std::uint32_t kWorkspaceTrustMementoFormatVersion = 1;

//! Encoding and decoding never expose a partially accepted document.
enum class EWorkspaceTrustMementoCodecStatus : std::uint8_t {
	Succeeded,
	CorruptPayload,
	UnsupportedSchema,
	PayloadTooLarge,
};

struct WorkspaceTrustMementoEncodeResult final {
	EWorkspaceTrustMementoCodecStatus status = EWorkspaceTrustMementoCodecStatus::CorruptPayload;
	std::string payload;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkspaceTrustMementoCodecStatus::Succeeded;
	}
};

struct WorkspaceTrustMementoDecodeResult final {
	EWorkspaceTrustMementoCodecStatus status = EWorkspaceTrustMementoCodecStatus::CorruptPayload;
	std::optional<WorkspaceTrustMemento> memento;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkspaceTrustMementoCodecStatus::Succeeded && memento.has_value();
	}
};

/*!
	@brief Machine-owned record format, separate from user-authored Settings JSONC.

	The document is
	@c {"formatVersion":1,"startupPromptShown":bool,"untrustedFilesAccepted":bool}.

	@par A missing flag decodes as false rather than rejecting the document.
	The two flags are independent answers to independent questions, and false is
	the value that asks again. An older writer that only knew one of them therefore
	round-trips into "asked about that one, not the other", which is exactly true.
	Rejecting the document instead would discard the flag that *is* present, which
	is strictly worse: the user would be re-asked something they already answered.

	@par A flag that is present but not a boolean is a corruption, not a default.
	Silently reading @c "true" or @c 1 as false would re-ask; reading it as true
	would claim an answer the payload does not actually contain. Neither is a
	decode. The store then preserves the bytes and behaves as if nothing readable
	exists, which asks again without destroying the record.
 */
class CWorkspaceTrustMementoCodec final {
public:
	[[nodiscard]] static WorkspaceTrustMementoEncodeResult Encode(
		const WorkspaceTrustMemento& memento) noexcept;
	[[nodiscard]] static WorkspaceTrustMementoDecodeResult Decode(
		std::string_view payload) noexcept;
};

} // namespace config
