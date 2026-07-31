/*! @file
 * @brief Bounded JSON codec for stable-ID workbench layout mementos.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::layout {

//! Writers always emit v2. Readers retain v1 support so existing profile state
//! is upgraded without discarding the user's layout.
inline constexpr std::uint32_t kWorkbenchLayoutMementoLegacyFormatVersion = 1;
inline constexpr std::uint32_t kWorkbenchLayoutMementoFormatVersion = 2;
inline constexpr std::size_t kMaximumWorkbenchLayoutMementoDepth = 16;
inline constexpr std::size_t kMaximumWorkbenchLayoutMementoParts = 64;
inline constexpr std::size_t kMaximumWorkbenchLayoutMementoContainers = 1024;
inline constexpr std::size_t kMaximumWorkbenchLayoutMementoViews = 2048;
inline constexpr std::int32_t kMinimumWorkbenchLayoutMementoOrder = (-2'147'483'647 - 1);
inline constexpr std::int32_t kMaximumWorkbenchLayoutMementoOrder = 2'147'483'647;
inline constexpr std::uint32_t kMaximumWorkbenchLayoutMementoExtentDip =
	kMaximumWorkbenchLayoutCommittedExtentDip;

//! Encoding and decoding never expose a partially accepted document.
enum class EWorkbenchLayoutMementoCodecStatus : std::uint8_t {
	Succeeded,
	InvalidSnapshot,
	CorruptPayload,
	UnsupportedSchema,
	PayloadTooLarge,
};

struct WorkbenchLayoutMementoEncodeResult final {
	EWorkbenchLayoutMementoCodecStatus status = EWorkbenchLayoutMementoCodecStatus::InvalidSnapshot;
	std::string payload;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkbenchLayoutMementoCodecStatus::Succeeded;
	}
};

struct WorkbenchLayoutMementoDecodeResult final {
	EWorkbenchLayoutMementoCodecStatus status = EWorkbenchLayoutMementoCodecStatus::CorruptPayload;
	std::optional<WorkbenchLayoutStateSnapshot> snapshot;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkbenchLayoutMementoCodecStatus::Succeeded && snapshot.has_value();
	}
};

/*! 
	@brief Machine-owned memento format, separate from user-authored Settings JSONC.

	The payload deliberately omits model generation/revision and every HWND or
	interaction-transient value. Unknown but structurally valid stable IDs remain
	in the decoded snapshot so a later extension registration can consume them.
*/
class CWorkbenchLayoutMementoCodec final {
public:
	[[nodiscard]] static WorkbenchLayoutMementoEncodeResult Encode(
		const WorkbenchLayoutStateSnapshot& snapshot) noexcept;
	[[nodiscard]] static WorkbenchLayoutMementoDecodeResult Decode(
		std::string_view payload) noexcept;
};

} // namespace workbench::layout
