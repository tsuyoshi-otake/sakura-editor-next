/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/icons/CFileIconThemeRegistry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

class TemporaryFileIconThemeExtension final {
public:
	TemporaryFileIconThemeExtension()
	{
		const auto suffix = std::to_wstring(::GetCurrentProcessId()) + L"." +
			std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
		m_root = std::filesystem::temp_directory_path() / (L"SakuraEditor.FileIconThemeRegistry." + suffix);
		std::error_code error;
		std::filesystem::create_directories(m_root, error);
		if (error) m_root.clear();
	}

	~TemporaryFileIconThemeExtension()
	{
		if (!m_root.empty()) {
			std::error_code error;
			std::filesystem::remove_all(m_root, error);
		}
	}

	TemporaryFileIconThemeExtension(const TemporaryFileIconThemeExtension&) = delete;
	TemporaryFileIconThemeExtension& operator=(const TemporaryFileIconThemeExtension&) = delete;

	[[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_root; }

	[[nodiscard]] bool Write(std::wstring_view relative, std::string_view content) const
	{
		if (m_root.empty()) return false;
		const auto path = m_root / std::filesystem::path(std::wstring(relative));
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return false;
		std::ofstream output(path, std::ios::binary);
		if (!output) return false;
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

private:
	std::filesystem::path m_root;
};

void WriteIconThemeExtension(const TemporaryFileIconThemeExtension& extension)
{
	ASSERT_TRUE(extension.Write(L"package.json", R"json({
		// VS Code extension manifests are JSONC.
		"contributes": {
			"iconThemes": [{
				"id": "publisher.test-icons",
				"label": "Test File Icons",
				"path": "theme.json"
			}]
		}
	})json"));
	ASSERT_TRUE(extension.Write(L"file.png", "not a decoded image in this registry test"));
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"iconDefinitions": {
			"file": { "iconPath": "file.png" },
			"name": { "iconPath": "file.png" },
			"parentName": { "iconPath": "file.png" },
			"extension": { "iconPath": "file.png" },
			"parentExtension": { "iconPath": "file.png" },
			"lightFile": { "iconPath": "file.png" },
			"folder": { "iconPath": "file.png" },
			"folderExpanded": { "iconPath": "file.png" },
			"root": { "iconPath": "file.png" }
		},
		"file": "file",
		"fileNames": {
			"readme.md": "name",
			"src/main.cpp": "parentName"
		},
		"fileExtensions": {
			"cpp": "extension",
			"src/cpp": "parentExtension"
		},
		"folder": "folder",
		"folderExpanded": "folderExpanded",
		"rootFolder": "root",
		"light": { "file": "lightFile" },
		"hidesExplorerArrows": true
	})json"));
}

TEST(CFileIconThemeRegistry, DiscoversLoadsAndResolvesVsCodeAssociations)
{
	using workbench::icons::CFileIconThemeRegistry;
	using workbench::icons::FileIconThemeVariant;

	TemporaryFileIconThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	WriteIconThemeExtension(extension);

	CFileIconThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-icons", extension.Root()));
	const auto themes = registry.Themes();
	ASSERT_EQ(1U, themes.size());
	EXPECT_EQ(L"publisher.test-icons", themes[0].id);
	EXPECT_EQ(L"Test File Icons", themes[0].label);
	EXPECT_EQ(L"publisher.test-icons", themes[0].extensionId);

	const auto loaded = registry.Load(L"test file icons");
	ASSERT_TRUE(loaded.Succeeded()) << loaded.diagnostic.c_str();
	ASSERT_TRUE(loaded.theme);
	const auto& theme = *loaded.theme;
	EXPECT_TRUE(theme.hidesExplorerArrows);

	const auto path = extension.Root() / L"workspace" / L"src" / L"main.cpp";
	ASSERT_NE(nullptr, theme.Resolve(L"README.MD", path.wstring(), false, false, false));
	EXPECT_EQ(&theme.definitions.at(L"name"), theme.Resolve(L"README.MD", path.wstring(), false, false, false));
	EXPECT_EQ(&theme.definitions.at(L"parentName"), theme.Resolve(L"main.cpp", path.wstring(), false, false, false));

	const auto sourcePath = extension.Root() / L"workspace" / L"src" / L"util.cpp";
	EXPECT_EQ(&theme.definitions.at(L"parentExtension"),
		theme.Resolve(L"util.cpp", sourcePath.wstring(), false, false, false));
	const auto otherPath = extension.Root() / L"workspace" / L"other" / L"util.cpp";
	EXPECT_EQ(&theme.definitions.at(L"extension"),
		theme.Resolve(L"util.cpp", otherPath.wstring(), false, false, false));
	const auto textPath = extension.Root() / L"workspace" / L"notes.txt";
	EXPECT_EQ(&theme.definitions.at(L"file"),
		theme.Resolve(L"notes.txt", textPath.wstring(), false, false, false));

	const auto folderPath = extension.Root() / L"workspace" / L"src";
	EXPECT_EQ(&theme.definitions.at(L"folder"),
		theme.Resolve(L"src", folderPath.wstring(), true, false, false));
	EXPECT_EQ(&theme.definitions.at(L"folderExpanded"),
		theme.Resolve(L"src", folderPath.wstring(), true, true, false));
	const auto rootPath = extension.Root() / L"workspace";
	EXPECT_EQ(&theme.definitions.at(L"root"),
		theme.Resolve(L"workspace", rootPath.wstring(), true, false, true));

	EXPECT_EQ(&theme.definitions.at(L"lightFile"),
		theme.Resolve(L"notes.txt", textPath.wstring(), false, false, false, FileIconThemeVariant::Light));
	EXPECT_EQ(&theme.definitions.at(L"lightFile"),
		theme.Resolve(L"notes.txt", textPath.wstring(), false, false, false, FileIconThemeVariant::HighContrastLight));
}

TEST(CFileIconThemeRegistry, RejectsThemePathsOutsideExtensionRoot)
{
	TemporaryFileIconThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"outside.json", R"json({
		"iconDefinitions": { "file": { "iconPath": "file.png" } },
		"file": "file"
	})json"));
	ASSERT_TRUE(extension.Write(L"package.json", R"json({
		"contributes": {
			"iconThemes": [{
				"id": "publisher.unsafe-icons",
				"label": "Unsafe File Icons",
				"path": "../outside.json"
			}]
		}
	})json"));

	workbench::icons::CFileIconThemeRegistry registry;
	EXPECT_FALSE(registry.RegisterExtension(L"publisher.unsafe-icons", extension.Root()));
	EXPECT_TRUE(registry.Themes().empty());
}

} // namespace
