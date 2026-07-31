/*! @file
 * @brief Bounded deterministic codecs for working-copy backups and editor sessions.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/persistence/WorkingCopyPersistenceTypes.h"

#include <optional>
#include <string>
#include <string_view>

namespace workbench::editor::persistence {

enum class EWorkingCopyPersistenceCodecStatus : std::uint8_t {
	Succeeded,
	InvalidRecord,
	CorruptPayload,
	UnsupportedSchema,
	UnknownInputType,
	ChecksumMismatch,
	PayloadTooLarge,
};

struct WorkingCopyBackupEncodeResult final {
	EWorkingCopyPersistenceCodecStatus status = EWorkingCopyPersistenceCodecStatus::InvalidRecord;
	std::string payload;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EWorkingCopyPersistenceCodecStatus::Succeeded; }
};

struct WorkingCopyBackupDecodeResult final {
	EWorkingCopyPersistenceCodecStatus status = EWorkingCopyPersistenceCodecStatus::CorruptPayload;
	std::optional<WorkingCopyBackup> backup;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkingCopyPersistenceCodecStatus::Succeeded && backup.has_value();
	}
};

struct EditorSessionManifestEncodeResult final {
	EWorkingCopyPersistenceCodecStatus status = EWorkingCopyPersistenceCodecStatus::InvalidRecord;
	std::string payload;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EWorkingCopyPersistenceCodecStatus::Succeeded; }
};

struct EditorSessionManifestDecodeResult final {
	EWorkingCopyPersistenceCodecStatus status = EWorkingCopyPersistenceCodecStatus::CorruptPayload;
	std::optional<EditorSessionManifest> manifest;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkingCopyPersistenceCodecStatus::Succeeded && manifest.has_value();
	}
};

/*! 
	@brief JSON persistence codec with no storage, UI, or process dependency.

	V1 only accepts the registered text input type `workbench.editor.text`. Unknown input
	types are terminally rejected so callers cannot accidentally restore opaque extension
	state with the wrong owner. Future registries may add type-specific decoders without
	changing the persisted working-copy key or allowing a partial manifest.
*/
class CWorkingCopyPersistenceCodec final {
public:
	inline static constexpr std::string_view kTextInputTypeId = "workbench.editor.text";

	//! Deterministic non-cryptographic FNV-1a 64 checksum, serialized as 16 lower-case hex digits.
	[[nodiscard]] static std::string ComputeContentChecksum(std::string_view content) noexcept;
	[[nodiscard]] static WorkingCopyBackupEncodeResult EncodeBackup(const WorkingCopyBackup& backup) noexcept;
	[[nodiscard]] static WorkingCopyBackupDecodeResult DecodeBackup(std::string_view payload) noexcept;
	[[nodiscard]] static EditorSessionManifestEncodeResult EncodeSession(const EditorSessionManifest& manifest) noexcept;
	[[nodiscard]] static EditorSessionManifestDecodeResult DecodeSession(std::string_view payload) noexcept;
};

} // namespace workbench::editor::persistence
