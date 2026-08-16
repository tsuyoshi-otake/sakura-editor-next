/*! @file
	@brief Built-in Explorer file-icon associations for the bundled Codicon set

	VS Code's default file icons come from the `vs-seti` icon theme. Extension
	icon themes were retired with the extension host, so this product keeps a
	first-party association table that maps common file names and extensions onto
	bundled codicons. It is not a Seti clone: glyphs stay monochrome and themed
	with the Explorer text colour, and unknown types fall back to `file`.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <array>
#include <string_view>

namespace workbench::explorer {

namespace detail {

struct FileNameIcon {
	std::wstring_view name;
	std::wstring_view icon;
};

struct FileExtensionIcon {
	std::wstring_view extension; // without a leading dot
	std::wstring_view icon;
};

//! Exact file-name hits, compared case-insensitively.
constexpr std::array kFileNameIcons{
	FileNameIcon{ L".editorconfig", L"settings-gear" },
	FileNameIcon{ L".gitattributes", L"git-commit" },
	FileNameIcon{ L".gitignore", L"git-commit" },
	FileNameIcon{ L".gitmodules", L"git-commit" },
	FileNameIcon{ L"cmakelists.txt", L"file-code" },
	FileNameIcon{ L"dockerfile", L"file-code" },
	FileNameIcon{ L"gemfile", L"ruby" },
	FileNameIcon{ L"go.mod", L"file-code" },
	FileNameIcon{ L"go.sum", L"file-code" },
	FileNameIcon{ L"license", L"file-text" },
	FileNameIcon{ L"license.md", L"markdown" },
	FileNameIcon{ L"license.txt", L"file-text" },
	FileNameIcon{ L"makefile", L"file-code" },
	FileNameIcon{ L"package-lock.json", L"json" },
	FileNameIcon{ L"package.json", L"json" },
	FileNameIcon{ L"rakefile", L"ruby" },
	FileNameIcon{ L"readme", L"file-text" },
	FileNameIcon{ L"readme.md", L"markdown" },
	FileNameIcon{ L"readme.txt", L"file-text" },
	FileNameIcon{ L"tsconfig.json", L"json" },
};

//! Longer extensions first so `.d.ts` wins over `.ts`.
constexpr std::array kFileExtensionIcons{
	FileExtensionIcon{ L"d.ts", L"file-code" },
	FileExtensionIcon{ L"cmake", L"file-code" },
	FileExtensionIcon{ L"coffee", L"file-code" },
	FileExtensionIcon{ L"ipynb", L"notebook" },
	FileExtensionIcon{ L"csproj", L"file-code" },
	FileExtensionIcon{ L"fsproj", L"file-code" },
	FileExtensionIcon{ L"vbproj", L"file-code" },
	FileExtensionIcon{ L"vcxproj", L"file-code" },
	FileExtensionIcon{ L"filters", L"file-code" },
	FileExtensionIcon{ L"props", L"file-code" },
	FileExtensionIcon{ L"targets", L"file-code" },
	FileExtensionIcon{ L"sln", L"file-code" },
	FileExtensionIcon{ L"jsx", L"file-code" },
	FileExtensionIcon{ L"tsx", L"file-code" },
	FileExtensionIcon{ L"mjs", L"file-code" },
	FileExtensionIcon{ L"cjs", L"file-code" },
	FileExtensionIcon{ L"vue", L"file-code" },
	FileExtensionIcon{ L"svelte", L"file-code" },
	FileExtensionIcon{ L"html", L"file-code" },
	FileExtensionIcon{ L"htm", L"file-code" },
	FileExtensionIcon{ L"xhtml", L"file-code" },
	FileExtensionIcon{ L"css", L"file-code" },
	FileExtensionIcon{ L"scss", L"file-code" },
	FileExtensionIcon{ L"sass", L"file-code" },
	FileExtensionIcon{ L"less", L"file-code" },
	FileExtensionIcon{ L"jsonc", L"json" },
	FileExtensionIcon{ L"json", L"json" },
	FileExtensionIcon{ L"yaml", L"file-code" },
	FileExtensionIcon{ L"yml", L"file-code" },
	FileExtensionIcon{ L"toml", L"file-code" },
	FileExtensionIcon{ L"ini", L"settings-gear" },
	FileExtensionIcon{ L"cfg", L"settings-gear" },
	FileExtensionIcon{ L"conf", L"settings-gear" },
	FileExtensionIcon{ L"config", L"settings-gear" },
	FileExtensionIcon{ L"md", L"markdown" },
	FileExtensionIcon{ L"markdown", L"markdown" },
	FileExtensionIcon{ L"mdx", L"markdown" },
	FileExtensionIcon{ L"rst", L"file-text" },
	FileExtensionIcon{ L"txt", L"file-text" },
	FileExtensionIcon{ L"log", L"file-text" },
	FileExtensionIcon{ L"csv", L"file-text" },
	FileExtensionIcon{ L"tsv", L"file-text" },
	FileExtensionIcon{ L"py", L"python" },
	FileExtensionIcon{ L"pyw", L"python" },
	FileExtensionIcon{ L"pyi", L"python" },
	FileExtensionIcon{ L"rb", L"ruby" },
	FileExtensionIcon{ L"erb", L"ruby" },
	FileExtensionIcon{ L"js", L"file-code" },
	FileExtensionIcon{ L"ts", L"file-code" },
	FileExtensionIcon{ L"c", L"file-code" },
	FileExtensionIcon{ L"cc", L"file-code" },
	FileExtensionIcon{ L"cpp", L"file-code" },
	FileExtensionIcon{ L"cxx", L"file-code" },
	FileExtensionIcon{ L"h", L"file-code" },
	FileExtensionIcon{ L"hh", L"file-code" },
	FileExtensionIcon{ L"hpp", L"file-code" },
	FileExtensionIcon{ L"hxx", L"file-code" },
	FileExtensionIcon{ L"inl", L"file-code" },
	FileExtensionIcon{ L"cs", L"file-code" },
	FileExtensionIcon{ L"fs", L"file-code" },
	FileExtensionIcon{ L"fsx", L"file-code" },
	FileExtensionIcon{ L"java", L"file-code" },
	FileExtensionIcon{ L"kt", L"file-code" },
	FileExtensionIcon{ L"kts", L"file-code" },
	FileExtensionIcon{ L"scala", L"file-code" },
	FileExtensionIcon{ L"go", L"file-code" },
	FileExtensionIcon{ L"rs", L"file-code" },
	FileExtensionIcon{ L"swift", L"file-code" },
	FileExtensionIcon{ L"php", L"file-code" },
	FileExtensionIcon{ L"lua", L"file-code" },
	FileExtensionIcon{ L"r", L"file-code" },
	FileExtensionIcon{ L"pl", L"file-code" },
	FileExtensionIcon{ L"pm", L"file-code" },
	FileExtensionIcon{ L"m", L"file-code" },
	FileExtensionIcon{ L"mm", L"file-code" },
	FileExtensionIcon{ L"sql", L"database" },
	FileExtensionIcon{ L"db", L"database" },
	FileExtensionIcon{ L"sqlite", L"database" },
	FileExtensionIcon{ L"xml", L"file-code" },
	FileExtensionIcon{ L"xaml", L"file-code" },
	FileExtensionIcon{ L"xsd", L"file-code" },
	FileExtensionIcon{ L"xsl", L"file-code" },
	FileExtensionIcon{ L"xslt", L"file-code" },
	FileExtensionIcon{ L"sh", L"terminal-bash" },
	FileExtensionIcon{ L"bash", L"terminal-bash" },
	FileExtensionIcon{ L"zsh", L"terminal-bash" },
	FileExtensionIcon{ L"fish", L"terminal" },
	FileExtensionIcon{ L"ps1", L"terminal-powershell" },
	FileExtensionIcon{ L"psm1", L"terminal-powershell" },
	FileExtensionIcon{ L"psd1", L"terminal-powershell" },
	FileExtensionIcon{ L"bat", L"terminal-cmd" },
	FileExtensionIcon{ L"cmd", L"terminal-cmd" },
	FileExtensionIcon{ L"png", L"file-media" },
	FileExtensionIcon{ L"jpg", L"file-media" },
	FileExtensionIcon{ L"jpeg", L"file-media" },
	FileExtensionIcon{ L"gif", L"file-media" },
	FileExtensionIcon{ L"bmp", L"file-media" },
	FileExtensionIcon{ L"ico", L"file-media" },
	FileExtensionIcon{ L"svg", L"file-media" },
	FileExtensionIcon{ L"webp", L"file-media" },
	FileExtensionIcon{ L"tif", L"file-media" },
	FileExtensionIcon{ L"tiff", L"file-media" },
	FileExtensionIcon{ L"pdf", L"file-pdf" },
	FileExtensionIcon{ L"zip", L"file-zip" },
	FileExtensionIcon{ L"7z", L"file-zip" },
	FileExtensionIcon{ L"rar", L"file-zip" },
	FileExtensionIcon{ L"tar", L"file-zip" },
	FileExtensionIcon{ L"gz", L"file-zip" },
	FileExtensionIcon{ L"tgz", L"file-zip" },
	FileExtensionIcon{ L"bz2", L"file-zip" },
	FileExtensionIcon{ L"xz", L"file-zip" },
	FileExtensionIcon{ L"exe", L"file-binary" },
	FileExtensionIcon{ L"dll", L"file-binary" },
	FileExtensionIcon{ L"so", L"file-binary" },
	FileExtensionIcon{ L"dylib", L"file-binary" },
	FileExtensionIcon{ L"bin", L"file-binary" },
	FileExtensionIcon{ L"o", L"file-binary" },
	FileExtensionIcon{ L"obj", L"file-binary" },
	FileExtensionIcon{ L"lib", L"file-binary" },
	FileExtensionIcon{ L"a", L"file-binary" },
	FileExtensionIcon{ L"wasm", L"file-binary" },
	FileExtensionIcon{ L"pdb", L"file-binary" },
	FileExtensionIcon{ L"lock", L"lock" },
	FileExtensionIcon{ L"pem", L"key" },
	FileExtensionIcon{ L"crt", L"key" },
	FileExtensionIcon{ L"cer", L"key" },
	FileExtensionIcon{ L"key", L"key" },
	FileExtensionIcon{ L"pub", L"key" },
};

[[nodiscard]] constexpr wchar_t AsciiLower(wchar_t value) noexcept
{
	return (value >= L'A' && value <= L'Z') ? static_cast<wchar_t>(value - L'A' + L'a') : value;
}

[[nodiscard]] constexpr bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
	}
	return true;
}

[[nodiscard]] constexpr bool EndsWithIgnoreCase(std::wstring_view value, std::wstring_view suffix) noexcept
{
	if (suffix.size() > value.size()) return false;
	return EqualsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

[[nodiscard]] constexpr std::wstring_view ResolveFileIconByName(std::wstring_view name) noexcept
{
	for (const auto& entry : kFileNameIcons) {
		if (EqualsIgnoreCase(name, entry.name)) return entry.icon;
	}
	std::size_t bestLength = 0;
	std::wstring_view bestIcon{};
	for (const auto& entry : kFileExtensionIcons) {
		if (entry.extension.size() <= bestLength) continue;
		if (name.size() <= entry.extension.size()) continue;
		if (name[name.size() - entry.extension.size() - 1] != L'.') continue;
		if (!EndsWithIgnoreCase(name, entry.extension)) continue;
		bestLength = entry.extension.size();
		bestIcon = entry.icon;
	}
	return bestIcon.empty() ? std::wstring_view{ L"file" } : bestIcon;
}

} // namespace detail

/*!
	@brief Resolves the bundled codicon id for one Explorer row.

	Folders keep the generic folder / root-folder glyphs. Files use the built-in
	name and extension table, falling back to `file`.
*/
[[nodiscard]] constexpr std::wstring_view ResolveExplorerFileIconCodicon(
	std::wstring_view name,
	bool isDirectory,
	bool expanded,
	bool isWorkspaceRoot) noexcept
{
	if (isWorkspaceRoot) return expanded ? L"root-folder-opened" : L"root-folder";
	if (isDirectory) return expanded ? L"folder-opened" : L"folder";
	return detail::ResolveFileIconByName(name);
}

} // namespace workbench::explorer
