/*! @file
	@brief Resolve one file name to its Seti icon, the way VS Code's default theme does

	VS Code applies a file icon theme through CSS: every Explorer row carries the
	classes `<name>-name-file-icon`, one `<suffix>-ext-file-icon` per dotted suffix,
	`<languageId>-lang-file-icon`, and `file-icon`, and the theme contributes one rule
	per association (`fileIconThemeData.ts`, `collectSelectors`). Nothing there is an
	explicit precedence list -- the order falls out of selector specificity:

	| Association     | Classes in the rule                    | Beats            |
	|---|---|---|
	| `fileNames`     | name + every suffix + 2 markers        | everything below |
	| `fileExtensions`| every suffix from the key + 1 marker   | shorter suffixes |
	| `languageIds`   | one language class                     | the default      |
	| `file`          | `file-icon` alone                      | --               |

	So a whole-name match wins, then the longest dotted suffix, then the language, then
	the theme's default icon. The language layer is already folded into the two tables
	in SetiIconThemeTable.h at generation time, because this product has no language
	registry to match it against; see tools/generate-seti-icon-theme.py.

	This header is pure. It never touches GDI, a font, or a palette, so the ordering
	rules above can be tested directly against upstream's own theme document.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/icons/SetiIconThemeTable.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace workbench::icons::seti {

//! The family name seti.ttf registers, and the value CSetiFont reports when available.
inline constexpr std::wstring_view kFontFamily = L"seti";

/*!
	@brief Upstream's `fonts[0].size`, relative to the 16 DIP icon slot it is drawn in

	The theme document asks for `"size": "150%"`, which VS Code resolves against the
	13 px workbench font, giving a 19.5 px em box inside the 16 px
	`.monaco-icon-label::before` slot. Seti's glyphs only ink about 0.56 em, so that
	larger em box is what makes them fill the slot the way Codicons do at 1.0 em.
	Expressed against the slot instead of the font, the ratio survives DPI scaling and
	a different chrome font: 19.5 / 16 = 39 / 32.
*/
inline constexpr int kEmToIconSlotNumerator = 39;
inline constexpr int kEmToIconSlotDenominator = 32;

/*!
	@brief Which section of the theme document applies

	VS Code emits the `light` section under the `.vs` body class alone
	(`collectSelectors(iconThemeDocument.light, '.vs')`). Seti contributes no
	`highContrast` section, so High Contrast and High Contrast Light both fall through
	to the base section. This is therefore not "is the background bright" -- it is
	exactly `ColorThemeKind.Light`.
*/
enum class EIconVariant : std::uint8_t {
	Dark,
	Light,
};

//! One resolved icon: the glyph to draw and the color to draw it in.  A resolution
//! states what the theme document says; it is never a mutable drawing state.
struct SResolvedIcon {
	const wchar_t character{ L'\0' };
	//! 0x00RRGGBB, or kInheritColor when upstream gives the icon no `fontColor`.
	const std::uint32_t color{ kInheritColor };

	[[nodiscard]] constexpr bool operator==(const SResolvedIcon& other) const noexcept
	{
		return character == other.character && color == other.color;
	}
};

/*!
	@brief Upstream contributes no folder association at all

	`vs-seti-icon-theme.json` has no `folder`, `folderExpanded`, `rootFolder`, or
	`rootFolderExpanded` key, so `collectSelectors` never sets `hasFolderIcons` and the
	workbench never gets the `show-folder-icons` class. In real VS Code a folder row
	under the default theme therefore shows its twistie and its name and no glyph.
	Substituting a folder icon from somewhere else would be a look-alike, not the
	theme.
*/
inline constexpr bool kHasFolderIcons = false;

namespace detail {

[[nodiscard]] constexpr wchar_t ToAsciiLower(wchar_t character) noexcept
{
	return character >= L'A' && character <= L'Z'
		? static_cast<wchar_t>(character - L'A' + L'a')
		: character;
}

/*!
	@brief Order `candidate` against an already-lowercase table key

	Only the candidate is folded, because every generated key is lowercase ASCII
	already. Folding ASCII alone is enough to reproduce upstream's `toLowerCase()`
	here: a key is ASCII, so a candidate character outside ASCII can never become one
	of its characters by any case mapping that matters in practice.
*/
[[nodiscard]] constexpr int CompareAsciiLower(std::wstring_view candidate, std::wstring_view key) noexcept
{
	const std::size_t shared = candidate.size() < key.size() ? candidate.size() : key.size();
	for (std::size_t index = 0; index < shared; ++index) {
		const wchar_t left = ToAsciiLower(candidate[index]);
		const wchar_t right = key[index];
		if (left != right) return left < right ? -1 : 1;
	}
	if (candidate.size() == key.size()) return 0;
	return candidate.size() < key.size() ? -1 : 1;
}

//! Binary search over one ascending association table.
template <std::size_t Count>
[[nodiscard]] constexpr const SAssociation* Find(
	const SAssociation (&table)[Count], std::wstring_view candidate) noexcept
{
	std::size_t low = 0;
	std::size_t high = Count;
	while (low < high) {
		const std::size_t middle = low + (high - low) / 2;
		const int order = CompareAsciiLower(candidate, table[middle].key);
		if (order == 0) return &table[middle];
		if (order < 0) high = middle; else low = middle + 1;
	}
	return nullptr;
}

template <std::size_t Count>
[[nodiscard]] constexpr bool IsAscending(const SAssociation (&table)[Count]) noexcept
{
	for (std::size_t index = 1; index < Count; ++index) {
		if (CompareAsciiLower(table[index - 1].key, table[index].key) >= 0) return false;
	}
	return true;
}

[[nodiscard]] constexpr SResolvedIcon StyleAt(std::uint16_t index, EIconVariant variant) noexcept
{
	const SIconStyle& style = kIconStyles[index];
	return { style.character, variant == EIconVariant::Light ? style.lightColor : style.darkColor };
}

} // namespace detail

static_assert(detail::IsAscending(kFileNames), "kFileNames must ascend for the binary search");
static_assert(detail::IsAscending(kFileExtensions), "kFileExtensions must ascend for the binary search");

/*!
	@brief Resolve a file's icon, or nothing when the theme draws no icon for it

	@param fileName The entry's own name, not its path. Matching is case-insensitive,
	       as upstream lowercases the name before building its class list.
	@param isDirectory Directories resolve to nothing; see kHasFolderIcons.
	@param variant The theme section to read colors from.
*/
[[nodiscard]] constexpr std::optional<SResolvedIcon> ResolveSetiFileIcon(
	std::wstring_view fileName, bool isDirectory, EIconVariant variant) noexcept
{
	if (isDirectory) return std::nullopt;
	if (const auto* named = detail::Find(kFileNames, fileName); named != nullptr) {
		return detail::StyleAt(named->style, variant);
	}
	// Upstream splits the name on `.` and builds one `-ext-file-icon` class per
	// suffix from the second segment on, and the rule carrying more of them wins, so
	// the longest suffix decides. `archive.tar.gz` tries `tar.gz` before `gz`;
	// `.gitignore` tries `gitignore`; a name with no dot has no extension at all.
	for (std::size_t dot = fileName.find(L'.'); dot != std::wstring_view::npos;
		dot = fileName.find(L'.', dot + 1)) {
		const std::wstring_view suffix = fileName.substr(dot + 1);
		if (suffix.empty()) break;
		if (const auto* found = detail::Find(kFileExtensions, suffix); found != nullptr) {
			return detail::StyleAt(found->style, variant);
		}
	}
	return detail::StyleAt(kDefaultStyle, variant);
}

} // namespace workbench::icons::seti
