/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/IUpdateService.h"

namespace update {

//! The on-disk half of an update, under
//! `%LOCALAPPDATA%\sakura-editor-next\update\`.
//!
//! Per-user, deliberately: the package installs with `PrivilegesRequired=lowest`,
//! so a staging area that needed administrator rights would be unwritable in
//! exactly the configuration this feature has to work in.
class UpdateStagingStore final : public IUpdateStagingStore {
public:
	explicit UpdateStagingStore(std::filesystem::path root);

	//! `%LOCALAPPDATA%\sakura-editor-next\update`, or an empty path when the
	//! known folder cannot be resolved — which the caller must treat as "no
	//! staging is possible", not as the current directory.
	[[nodiscard]] static std::filesystem::path DefaultRoot();

	[[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_root; }

	[[nodiscard]] std::optional<UpdateManifest> ReadManifest() override;
	[[nodiscard]] bool WriteManifest(const UpdateManifest& manifest) override;
	void ClearManifest() noexcept override;

	[[nodiscard]] std::optional<std::wstring> StoreInstaller(
		const UpdateVersion& version,
		std::wstring_view assetName,
		const std::vector<std::uint8_t>& bytes) override;

	[[nodiscard]] bool InstallerMatches(std::wstring_view installerPath, std::uint64_t sizeBytes) override;
	[[nodiscard]] std::wstring InstallLogPath(const UpdateVersion& version) override;
	void RemoveOtherStagedBuilds(const UpdateVersion& keep) noexcept override;

private:
	[[nodiscard]] std::filesystem::path ManifestPath() const;
	[[nodiscard]] std::filesystem::path BuildDirectory(const UpdateVersion& version) const;

	std::filesystem::path m_root;
};

//! The manifest's wire form, exposed so it can be round-tripped in tests without
//! a filesystem. It is machine-owned JSON, not user-authored settings: unknown
//! members are ignored and a malformed document yields nothing at all.
[[nodiscard]] std::string EncodeUpdateManifest(const UpdateManifest& manifest);
[[nodiscard]] std::optional<UpdateManifest> DecodeUpdateManifest(std::string_view utf8);

} // namespace update
