/*! @file
	@brief Profile-scoped extension enablement state.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

/*! 
	@brief VS Code-compatible profile selection for globally stored extensions.

	The extension payload is shared under the user extension home, while this
	file records which extension IDs are enabled by one user-data profile. A
	missing file is intentionally interpreted by the caller: the default profile
	keeps legacy installed extensions enabled, and named/transient profiles start
	empty.
*/
class CExtensionProfileState final {
public:
	static constexpr std::uint32_t kCurrentVersion = 1;
	static constexpr std::size_t kMaximumFileBytes = 256 * 1024;
	static constexpr std::size_t kMaximumExtensionCount = 4096;

	enum class EStatus : std::uint8_t {
		Unavailable,
		Missing,
		Valid,
		Invalid,
		IoError,
	};

	struct Snapshot {
		EStatus status = EStatus::Unavailable;
		std::unordered_map<std::wstring, bool> enabled;
	};

	CExtensionProfileState() = default;
	explicit CExtensionProfileState(std::filesystem::path path)
		: m_path(std::move(path))
	{
	}

	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
	[[nodiscard]] Snapshot Load() const;

	//! Resolve the sparse profile map without treating malformed state as enabled.
	[[nodiscard]] static bool IsEnabled(
		const Snapshot& snapshot,
		std::wstring_view extensionId,
		bool defaultWhenAbsent) noexcept;

	//! Add or replace one profile selection atomically.
	[[nodiscard]] bool SetEnabled(std::wstring_view extensionId, bool enabled) const;

	//! Remove one profile selection. Missing state is already the terminal result.
	[[nodiscard]] bool Remove(std::wstring_view extensionId) const;

	//! Extension IDs are persisted as ASCII lowercase identifiers only.
	[[nodiscard]] static bool IsSafeExtensionId(std::wstring_view extensionId) noexcept;

private:
	[[nodiscard]] static std::wstring CanonicalizeExtensionId(std::wstring_view extensionId) noexcept;
	[[nodiscard]] static bool IsSafeExtensionId(std::string_view extensionId) noexcept;
	[[nodiscard]] bool Write(const Snapshot& snapshot) const;

	std::filesystem::path m_path;
};
