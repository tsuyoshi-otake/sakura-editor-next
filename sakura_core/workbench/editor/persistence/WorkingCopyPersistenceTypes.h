/*! @file
 * @brief Presentation-neutral backup and editor-session DTOs.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::editor::persistence {

inline constexpr std::uint32_t kWorkingCopyBackupFormatVersion = 1;
inline constexpr std::uint32_t kEditorSessionManifestFormatVersion = 1;
inline constexpr std::size_t kMaximumWorkingCopyPersistenceIdBytes = 512;
inline constexpr std::size_t kMaximumWorkingCopyPersistenceResourceBytes = 64 * 1024;
inline constexpr std::size_t kMaximumWorkingCopyPersistenceContentBytes = 512 * 1024;
inline constexpr std::size_t kMaximumEditorSessionInputs = 1024;
inline constexpr std::size_t kMaximumEditorSessionUntypedStateBytes = 16 * 1024;
//! Largest integer that remains exact in the JSON persistence representation.
inline constexpr std::uint64_t kMaximumWorkingCopyPersistenceGeneration = UINT64_C(9007199254740991);

//! Validates bounded UTF-8 independently from the smaller per-entry storage transport limit.
[[nodiscard]] bool IsValidWorkingCopyPersistenceUtf8(
	std::string_view value, bool allowEmpty, std::size_t maximumLength) noexcept;

//! Values become durable identity components and must remain bounded UTF-8 without NUL.
[[nodiscard]] constexpr bool IsValidWorkingCopyPersistenceId(
	std::string_view value, std::size_t maximumLength = kMaximumWorkingCopyPersistenceIdBytes) noexcept
{
	return !value.empty() && value.size() <= maximumLength && value.find('\0') == std::string_view::npos;
}

/*! 
	@brief Logical ownership boundary for backup and session records.

	Neither field identifies a process, HWND, display title, nor physical storage path.
	A missing workspaceId denotes the profile-wide logical workspace scope.
*/
struct WorkingCopyPersistenceScope final {
	std::string profileId;
	std::optional<std::string> workspaceId;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const WorkingCopyPersistenceScope&) const noexcept = default;
};

/*! 
	@brief Stable working-copy key used by backups and session backup references.

	Exactly one of canonicalResource and opaqueId is present. canonicalResource is the
	canonical URI text supplied by a resource service; it is never a display label or
	filesystem path chosen by this layer.
*/
struct WorkingCopyPersistenceIdentity final {
	std::string typeId;
	std::optional<std::string> canonicalResource;
	std::optional<std::string> opaqueId;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const WorkingCopyPersistenceIdentity&) const noexcept = default;
};

enum class EWorkingCopyTextEncoding : std::uint8_t {
	Utf8,
	Utf8WithBom,
	Utf16Le,
	Utf16Be,
	Windows1252,
	Unknown,
};

enum class EWorkingCopyEol : std::uint8_t {
	Lf,
	CrLf,
	Cr,
	Unknown,
};

/*! 
	@brief One complete dirty working-copy backup generation.

	checksum is the fixed-width lower-case hexadecimal FNV-1a 64 checksum of content.
	It is deliberately a deterministic corruption detector, not a cryptographic
	integrity or authentication mechanism.
*/
struct WorkingCopyBackup final {
	WorkingCopyPersistenceScope scope;
	WorkingCopyPersistenceIdentity identity;
	std::uint64_t generation = 0;
	std::uint64_t contentVersion = 0;
	std::string checksum;
	EWorkingCopyTextEncoding encoding = EWorkingCopyTextEncoding::Unknown;
	EWorkingCopyEol eol = EWorkingCopyEol::Unknown;
	bool dirty = false;
	std::string content;

	[[nodiscard]] bool IsValid() const noexcept;
};

/*! 
	@brief Versioned, untyped editor input descriptor persisted in a session manifest.

	state is owned by the registered input type and intentionally remains opaque to the
	persistence layer. It must never contain document contents; content recovery is
	through the optional backupGeneration and the corresponding working-copy identity.
*/
struct EditorSessionInputDescriptor final {
	std::string inputId;
	//! Registered editor-input type, independent from the working-copy model type.
	std::string inputTypeId;
	WorkingCopyPersistenceIdentity workingCopyIdentity;
	std::uint32_t stateVersion = 1;
	std::string state;
	std::optional<std::uint64_t> backupGeneration;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const EditorSessionInputDescriptor&) const noexcept = default;
};

/*! 
	@brief One logical editor-group manifest, separate from workbench layout persistence.

	The V1 format contains exactly one logical group ID. A manifest does not persist
	window geometry, focus coordinates, process identity, or any backup content bytes.
*/
struct EditorSessionManifest final {
	WorkingCopyPersistenceScope scope;
	std::uint64_t generation = 0;
	std::string logicalGroupId;
	std::optional<std::string> activeInputId;
	std::vector<EditorSessionInputDescriptor> inputs;

	[[nodiscard]] bool IsValid() const noexcept;
};

} // namespace workbench::editor::persistence
