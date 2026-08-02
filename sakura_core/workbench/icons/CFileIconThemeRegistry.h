/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/icons/CExtensionIconFont.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::icons {

//! The visual variants supported by VS Code file icon theme documents.
enum class FileIconThemeVariant : std::uint8_t {
	Default,
	Light,
	HighContrast,
	HighContrastLight,
};

struct FileIconThemeInfo final {
	std::wstring id;
	std::wstring label;
	std::wstring extensionId;
	std::filesystem::path extensionRoot;
	std::filesystem::path themePath;

	[[nodiscard]] bool operator==(const FileIconThemeInfo&) const noexcept = default;
};

//! A registered font resource kept alive for every Explorer item using it.
struct FileIconFont final {
	std::shared_ptr<detail::CRegisteredMemoryFont> resource;
	std::wstring faceName;
};

struct FileIconDefinition final {
	//! Absolute, extension-root-contained image path. Empty means font rendering.
	std::filesystem::path iconPath;
	std::shared_ptr<const FileIconFont> font;
	std::wstring glyph;
	std::optional<std::uint32_t> fontColor;

	[[nodiscard]] bool HasImage() const noexcept { return !iconPath.empty(); }
	[[nodiscard]] bool HasFont() const noexcept { return font != nullptr && !glyph.empty(); }
};

//! One association layer from a VS Code file icon theme document.
struct FileIconAssociationSet final {
	std::optional<std::wstring> file;
	std::optional<std::wstring> folder;
	std::optional<std::wstring> folderExpanded;
	std::optional<std::wstring> rootFolder;
	std::optional<std::wstring> rootFolderExpanded;
	std::map<std::wstring, std::wstring, std::less<>> fileNames;
	std::map<std::wstring, std::wstring, std::less<>> fileExtensions;
	std::map<std::wstring, std::wstring, std::less<>> folderNames;
	std::map<std::wstring, std::wstring, std::less<>> folderNamesExpanded;
	std::map<std::wstring, std::wstring, std::less<>> rootFolderNames;
	std::map<std::wstring, std::wstring, std::less<>> rootFolderNamesExpanded;
};

struct FileIconThemeSnapshot final {
	FileIconThemeInfo info;
	std::map<std::wstring, FileIconDefinition, std::less<>> definitions;
	FileIconAssociationSet base;
	FileIconAssociationSet light;
	FileIconAssociationSet highContrast;
	FileIconAssociationSet highContrastLight;
	bool hidesExplorerArrows = false;

	//! Resolves one Explorer entry according to VS Code's association precedence.
	//! `path` is used only to support the documented one-parent-segment keys.
	[[nodiscard]] const FileIconDefinition* Resolve(
		std::wstring_view name,
		std::wstring_view path,
		bool isDirectory,
		bool expanded,
		bool isWorkspaceRoot,
		FileIconThemeVariant variant = FileIconThemeVariant::Default) const noexcept;
};

struct FileIconThemeLoadResult final {
	std::shared_ptr<const FileIconThemeSnapshot> theme;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return theme != nullptr; }
};

//! Window-local registry of VS Code `contributes.iconThemes` entries.
//!
//! Discovery is deliberately separate from image/font rendering. It validates
//! manifest paths and parses association precedence, while the Explorer owns
//! the short-lived Win32 image list used to display the resolved definitions.
class CFileIconThemeRegistry final {
public:
	CFileIconThemeRegistry() = default;
	CFileIconThemeRegistry(const CFileIconThemeRegistry&) = delete;
	CFileIconThemeRegistry& operator=(const CFileIconThemeRegistry&) = delete;

	//! Replaces entries owned by extensionId. Invalid contributions are skipped.
	[[nodiscard]] bool RegisterExtension(
		std::wstring_view extensionId, const std::filesystem::path& extensionRoot);
	void Clear() noexcept;

	[[nodiscard]] std::vector<FileIconThemeInfo> Themes() const;
	//! Resolves by stable ID first, then by case-insensitive display label.
	[[nodiscard]] FileIconThemeLoadResult Load(std::wstring_view idOrLabel) const;

private:
	std::vector<FileIconThemeInfo> m_themes;
	mutable std::map<std::wstring, std::shared_ptr<const FileIconFont>, std::less<>> m_fonts;
};

} // namespace workbench::icons
