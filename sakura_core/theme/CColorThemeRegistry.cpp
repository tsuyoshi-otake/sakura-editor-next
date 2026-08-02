/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "theme/CColorThemeRegistry.h"

#include "platform/serialization/JsoncDocument.h"
#include "util/string_ex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <set>

namespace theme {
namespace {

using JsoncValue = platform::serialization::JsoncValue;
using JsoncObject = JsoncValue::Object;
using JsoncArray = JsoncValue::Array;

constexpr std::size_t kMaximumThemeFileBytes = 1024U * 1024U;
constexpr std::size_t kMaximumIncludeDepth = 16;
constexpr std::size_t kMaximumThemeContributions = 256;
constexpr std::size_t kMaximumTokenColorRules = 16'384;
constexpr std::size_t kMaximumSemanticTokenColors = 16'384;
constexpr std::wstring_view kBuiltinThemeExtensionId = L"sakura.builtin";

constexpr std::string_view kSakuraDefaultDarkThemeJson = R"json({
	"name": "Sakura Default Dark",
	"type": "dark",
	"colors": {
		"editor.background": "#1E1E1E",
		"sideBar.background": "#293134",
		"panel.background": "#252526",
		"sideBarSectionHeader.background": "#2A2D2E",
		"sideBar.border": "#454545",
		"panel.border": "#454545",
		"foreground": "#CCCCCC",
		"sideBar.foreground": "#969696",
		"descriptionForeground": "#989898",
		"disabledForeground": "#757575",
		"focusBorder": "#1F8AD2",
		"button.foreground": "#FFFFFF",
		"titleBar.activeBackground": "#3C3C3C",
		"activityBar.background": "#333333",
		"errorForeground": "#C42B1C",
		"notificationsWarningIcon.foreground": "#CCA700"
	},
	"tokenColors": [
		{ "scope": "comment", "settings": { "foreground": "#6A9955", "fontStyle": "italic" } },
		{ "scope": "string", "settings": { "foreground": "#CE9178" } },
		{ "scope": "constant.numeric", "settings": { "foreground": "#B5CEA8" } },
		{ "scope": ["keyword", "storage"], "settings": { "foreground": "#569CD6", "fontStyle": "bold" } },
		{ "scope": ["entity.name.type", "support.type", "storage.type"], "settings": { "foreground": "#4EC9B0" } },
		{ "scope": ["entity.name.function", "support.function", "meta.function-call"], "settings": { "foreground": "#DCDCAA" } },
		{ "scope": "variable", "settings": { "foreground": "#9CDCFE" } },
		{ "scope": "constant", "settings": { "foreground": "#4FC1FF" } },
		{ "scope": "string.regexp", "settings": { "foreground": "#D16969" } },
		{ "scope": "entity.name.tag", "settings": { "foreground": "#569CD6" } },
		{ "scope": "entity.other.attribute-name", "settings": { "foreground": "#9CDCFE" } },
		{ "scope": "invalid", "settings": { "foreground": "#F44747", "background": "#3F0001" } }
	]
})json";

constexpr std::string_view kSakuraDefaultLightThemeJson = R"json({
	"name": "Sakura Default Light",
	"type": "light",
	"colors": {
		"editor.background": "#F4F5F7",
		"sideBar.background": "#FFFFFF",
		"panel.background": "#FFFFFF",
		"sideBarSectionHeader.background": "#E9ECF1",
		"sideBar.border": "#CDD2DB",
		"panel.border": "#CDD2DB",
		"foreground": "#1F2329",
		"sideBar.foreground": "#5C6573",
		"descriptionForeground": "#717171",
		"disabledForeground": "#AAABAC",
		"focusBorder": "#B83268",
		"button.foreground": "#FFFFFF",
		"titleBar.activeBackground": "#F3F3F3",
		"activityBar.background": "#F3F3F3",
		"errorForeground": "#C42B1C",
		"notificationsWarningIcon.foreground": "#BF8800"
	},
	"tokenColors": [
		{ "scope": "comment", "settings": { "foreground": "#008000", "fontStyle": "italic" } },
		{ "scope": "string", "settings": { "foreground": "#A31515" } },
		{ "scope": "constant.numeric", "settings": { "foreground": "#098658" } },
		{ "scope": ["keyword", "storage"], "settings": { "foreground": "#0000FF", "fontStyle": "bold" } },
		{ "scope": ["entity.name.type", "support.type", "storage.type"], "settings": { "foreground": "#267F99" } },
		{ "scope": ["entity.name.function", "support.function", "meta.function-call"], "settings": { "foreground": "#795E26" } },
		{ "scope": "variable", "settings": { "foreground": "#001080" } },
		{ "scope": "constant", "settings": { "foreground": "#0070C1" } },
		{ "scope": "string.regexp", "settings": { "foreground": "#811F3F" } },
		{ "scope": "entity.name.tag", "settings": { "foreground": "#800000" } },
		{ "scope": "entity.other.attribute-name", "settings": { "foreground": "#FF0000" } },
		{ "scope": "invalid", "settings": { "foreground": "#CD3131", "background": "#F8F8F8" } }
	]
})json";

const JsoncValue* Member(const JsoncObject& object, std::wstring_view key) noexcept
{
	const auto found = object.find(std::wstring(key));
	return found == object.end() ? nullptr : &found->second;
}

const JsoncObject* AsObject(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<JsoncObject>(&value->Value());
}

const JsoncArray* AsArray(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<JsoncArray>(&value->Value());
}

const std::wstring* AsString(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<std::wstring>(&value->Value());
}

std::optional<bool> AsBool(const JsoncValue* value) noexcept
{
	if (value == nullptr) return std::nullopt;
	if (const auto* boolean = std::get_if<bool>(&value->Value())) return *boolean;
	return std::nullopt;
}

std::wstring Lower(std::wstring_view value)
{
	std::wstring result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towlower(character));
	});
	return result;
}

std::wstring Trim(std::wstring_view value)
{
	std::size_t first = 0;
	while (first < value.size() && std::iswspace(value[first])) ++first;
	std::size_t last = value.size();
	while (last > first && std::iswspace(value[last - 1])) --last;
	return std::wstring(value.substr(first, last - first));
}

std::filesystem::path ComparisonPath(const std::filesystem::path& path)
{
	std::error_code error;
	const auto canonical = std::filesystem::weakly_canonical(path, error);
	if (!error) return canonical.lexically_normal();
	const auto absolute = std::filesystem::absolute(path, error);
	return error ? path.lexically_normal() : absolute.lexically_normal();
}

std::wstring NormalizedPathText(const std::filesystem::path& path)
{
	std::wstring text = Lower(ComparisonPath(path).wstring());
	std::replace(text.begin(), text.end(), L'/', L'\\');
	while (text.size() > 3 && !text.empty() && text.back() == L'\\') text.pop_back();
	return text;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	const std::wstring rootText = NormalizedPathText(root);
	const std::wstring candidateText = NormalizedPathText(candidate);
	if (candidateText == rootText) return true;
	return candidateText.size() > rootText.size()
		&& candidateText.compare(0, rootText.size(), rootText) == 0
		&& candidateText[rootText.size()] == L'\\';
}

std::optional<std::filesystem::path> SafePath(
	const std::filesystem::path& root,
	const std::filesystem::path& base,
	std::wstring_view relative)
{
	if (relative.empty()) return std::nullopt;
	const std::filesystem::path relativePath{ std::wstring(relative) };
	if (relativePath.has_root_name() || relativePath.has_root_directory()) return std::nullopt;
	const auto candidate = (base / relativePath).lexically_normal();
	return IsWithin(root, candidate) ? std::optional(candidate) : std::nullopt;
}

std::optional<std::string> ReadUtf8File(const std::filesystem::path& path, std::wstring& diagnostic)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > kMaximumThemeFileBytes) {
		diagnostic = L"theme file is missing or exceeds the 1 MiB limit: " + path.wstring();
		return std::nullopt;
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		diagnostic = L"theme file could not be opened: " + path.wstring();
		return std::nullopt;
	}
	std::string content(static_cast<std::size_t>(size), '\0');
	if (size != 0) input.read(content.data(), static_cast<std::streamsize>(size));
	if (!input && !input.eof()) {
		diagnostic = L"theme file could not be read: " + path.wstring();
		return std::nullopt;
	}
	return content;
}

std::optional<JsoncValue> ReadJson(const std::filesystem::path& path, std::wstring& diagnostic)
{
	const auto content = ReadUtf8File(path, diagnostic);
	if (!content) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::Parse(*content);
	if (!parsed.Succeeded()) {
		diagnostic = L"theme JSONC parse failed in " + path.wstring();
		if (parsed.diagnostic) diagnostic += L": " + u8stowcs(parsed.diagnostic->message);
		return std::nullopt;
	}
	return parsed.value;
}

ColorThemeKind KindForUiTheme(std::wstring_view value) noexcept
{
	const auto lower = Lower(value);
	if (lower == L"vs") return ColorThemeKind::Light;
	if (lower == L"hc-black" || lower == L"hc") return ColorThemeKind::HighContrast;
	if (lower == L"hc-light") return ColorThemeKind::HighContrastLight;
	return ColorThemeKind::Dark;
}

std::optional<ColorThemeKind> KindForType(std::wstring_view value) noexcept
{
	const auto lower = Lower(value);
	if (lower == L"light") return ColorThemeKind::Light;
	if (lower == L"dark") return ColorThemeKind::Dark;
	if (lower == L"hc" || lower == L"high-contrast") return ColorThemeKind::HighContrast;
	if (lower == L"hc-light" || lower == L"high-contrast-light") return ColorThemeKind::HighContrastLight;
	return std::nullopt;
}

std::optional<std::vector<std::wstring>> ReadStringList(const JsoncValue* value)
{
	if (const auto* string = AsString(value)) return std::vector<std::wstring>{ *string };
	const auto* array = AsArray(value);
	if (array == nullptr) return std::nullopt;
	std::vector<std::wstring> result;
	result.reserve(array->size());
	for (const auto& item : *array) {
		const auto* string = AsString(&item);
		if (string == nullptr) return std::nullopt;
		result.push_back(*string);
	}
	return result;
}

void ParseTokenColorArray(const JsoncArray& array, std::vector<ThemeTokenColorRule>& output)
{
	for (const auto& item : array) {
		if (output.size() >= kMaximumTokenColorRules) break;
		const auto* object = AsObject(&item);
		if (object == nullptr) continue;
		ThemeTokenColorRule rule;
		if (const auto scopes = ReadStringList(Member(*object, L"scope"))) {
			rule.scopes = *scopes;
		}
		if (const auto* settings = AsObject(Member(*object, L"settings"))) {
			if (const auto* foreground = AsString(Member(*settings, L"foreground"))) {
				rule.foreground = CColorThemeRegistry::ParseColor(*foreground);
			}
			if (const auto* background = AsString(Member(*settings, L"background"))) {
				rule.background = CColorThemeRegistry::ParseColor(*background);
			}
			if (const auto* fontStyle = AsString(Member(*settings, L"fontStyle"))) {
				rule.fontStyle = *fontStyle;
			}
		}
		output.push_back(std::move(rule));
	}
}

void ParseSemanticTokenColors(const JsoncObject& object, ColorThemeSnapshot& snapshot)
{
	for (const auto& [key, value] : object) {
		if (snapshot.semanticTokenColors.size() >= kMaximumSemanticTokenColors) break;
		if (const auto* string = AsString(&value)) {
			if (const auto color = CColorThemeRegistry::ParseColor(*string)) {
				snapshot.semanticTokenColors[key] = *color;
			}
			continue;
		}
		if (const auto* objectValue = AsObject(&value)) {
			if (const auto* foreground = AsString(Member(*objectValue, L"foreground"))) {
				if (const auto color = CColorThemeRegistry::ParseColor(*foreground)) {
					snapshot.semanticTokenColors[key] = *color;
				}
			}
		}
	}
}

bool LoadThemeFile(
	const std::filesystem::path& file,
	const std::filesystem::path& root,
	ColorThemeSnapshot& snapshot,
	bool rootFile,
	bool& rootTypeWasExplicit,
	std::set<std::wstring>& activeFiles,
	std::size_t depth,
	std::wstring& diagnostic);

bool ParseTokenColorsFile(
	const std::filesystem::path& file,
	const std::filesystem::path& root,
	std::vector<ThemeTokenColorRule>& output,
	std::wstring& diagnostic)
{
	const auto document = ReadJson(file, diagnostic);
	if (!document) return false;
	const auto* array = AsArray(&*document);
	if (array == nullptr) {
		diagnostic = L"tokenColors must be an array: " + file.wstring();
		return false;
	}
	ParseTokenColorArray(*array, output);
	(void)root;
	return true;
}

bool ApplyThemeObjectFields(
	const JsoncObject& object,
	ColorThemeSnapshot& snapshot,
	bool rootFile,
	bool& rootTypeWasExplicit,
	const std::filesystem::path& file,
	const std::filesystem::path& root,
	std::wstring& diagnostic)
{
	if (rootFile) {
		if (const auto* type = AsString(Member(object, L"type"))) {
			if (const auto kind = KindForType(*type)) {
				snapshot.info.kind = *kind;
				rootTypeWasExplicit = true;
			}
		}
	}

	if (const auto* colors = AsObject(Member(object, L"colors"))) {
		for (const auto& [key, value] : *colors) {
			if (const auto* string = AsString(&value)) {
				if (const auto color = CColorThemeRegistry::ParseColor(*string)) snapshot.colors[key] = *color;
			}
		}
	}
	if (const auto* tokenColors = Member(object, L"tokenColors")) {
		if (const auto* array = AsArray(tokenColors)) {
			ParseTokenColorArray(*array, snapshot.tokenColors);
		} else if (const auto* path = AsString(tokenColors)) {
			if (file.empty() || root.empty()) {
				diagnostic = L"embedded color theme tokenColors paths are unsupported";
				return false;
			}
			const auto tokenPath = SafePath(root, file.parent_path(), *path);
			if (!tokenPath || !std::filesystem::is_regular_file(*tokenPath)
				|| !ParseTokenColorsFile(*tokenPath, root, snapshot.tokenColors, diagnostic)) {
				return false;
			}
		}
	}
	if (const auto* semantic = AsObject(Member(object, L"semanticTokenColors"))) {
		ParseSemanticTokenColors(*semantic, snapshot);
	}
	if (const auto semanticHighlighting = AsBool(Member(object, L"semanticHighlighting"))) {
		snapshot.semanticHighlighting = *semanticHighlighting;
	}
	return true;
}

bool LoadEmbeddedThemeDocument(
	std::string_view content,
	ColorThemeSnapshot& snapshot,
	std::wstring& diagnostic)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(content);
	if (!parsed.Succeeded()) {
		diagnostic = L"embedded color theme JSONC parse failed";
		if (parsed.diagnostic) diagnostic += L": " + u8stowcs(parsed.diagnostic->message);
		return false;
	}
	const auto* object = AsObject(&*parsed.value);
	if (object == nullptr) {
		diagnostic = L"embedded color theme document must be a JSON object";
		return false;
	}
	if (Member(*object, L"include") != nullptr || Member(*object, L"extends") != nullptr) {
		diagnostic = L"embedded color theme inheritance is unsupported";
		return false;
	}
	bool rootTypeWasExplicit = false;
	return ApplyThemeObjectFields(*object, snapshot, true, rootTypeWasExplicit, {}, {}, diagnostic);
}

bool LoadThemeFile(
	const std::filesystem::path& file,
	const std::filesystem::path& root,
	ColorThemeSnapshot& snapshot,
	bool rootFile,
	bool& rootTypeWasExplicit,
	std::set<std::wstring>& activeFiles,
	std::size_t depth,
	std::wstring& diagnostic)
{
	if (depth > kMaximumIncludeDepth) {
		diagnostic = L"theme include depth exceeded the safety limit";
		return false;
	}
	const std::wstring identity = NormalizedPathText(file);
	if (!activeFiles.insert(identity).second) {
		diagnostic = L"theme include cycle detected: " + file.wstring();
		return false;
	}
	const auto leave = [&activeFiles, &identity](bool success) {
		activeFiles.erase(identity);
		return success;
	};

	const auto document = ReadJson(file, diagnostic);
	if (!document) return leave(false);
	const auto* object = AsObject(&*document);
	if (object == nullptr) {
		diagnostic = L"theme document must be a JSON object: " + file.wstring();
		return leave(false);
	}

	const auto loadIncludes = [&](const JsoncValue* includeValue) {
		const auto includes = ReadStringList(includeValue);
		if (!includes) return true;
		for (const auto& include : *includes) {
			const auto includePath = SafePath(root, file.parent_path(), include);
			if (!includePath || !std::filesystem::is_regular_file(*includePath)) {
				diagnostic = L"theme include is outside the extension or missing: " + include;
				return false;
			}
			if (!LoadThemeFile(*includePath, root, snapshot, false, rootTypeWasExplicit,
				activeFiles, depth + 1, diagnostic)) return false;
		}
		return true;
	};
	if (!loadIncludes(Member(*object, L"include"))
		|| !loadIncludes(Member(*object, L"extends"))) {
		return leave(false);
	}

	if (!ApplyThemeObjectFields(*object, snapshot, rootFile, rootTypeWasExplicit, file, root, diagnostic)) {
		return leave(false);
	}
	return leave(true);
}

ThemeColor Composite(ThemeColor foreground, ThemeColor background) noexcept
{
	if (foreground.alpha == 0xFF) return { foreground.red, foreground.green, foreground.blue, 0xFF };
	if (foreground.alpha == 0) return { background.red, background.green, background.blue, 0xFF };
	const unsigned int alpha = foreground.alpha;
	const unsigned int inverse = 0xFFU - alpha;
	return {
		static_cast<std::uint8_t>((foreground.red * alpha + background.red * inverse + 127U) / 255U),
		static_cast<std::uint8_t>((foreground.green * alpha + background.green * inverse + 127U) / 255U),
		static_cast<std::uint8_t>((foreground.blue * alpha + background.blue * inverse + 127U) / 255U),
		0xFF,
	};
}

} // namespace

std::optional<ThemeColor> CColorThemeRegistry::ParseColor(std::wstring_view value) noexcept
{
	try {
		const auto text = Trim(value);
		const auto lower = Lower(text);
		if (lower == L"transparent") return ThemeColor{ 0, 0, 0, 0 };
		if (text.size() >= 2 && text.front() == L'#') {
			const auto nibble = [](wchar_t character) -> int {
				if (character >= L'0' && character <= L'9') return character - L'0';
				if (character >= L'a' && character <= L'f') return character - L'a' + 10;
				if (character >= L'A' && character <= L'F') return character - L'A' + 10;
				return -1;
			};
			const auto byte = [&nibble](wchar_t high, wchar_t low) -> int {
				const int left = nibble(high);
				const int right = nibble(low);
				return left < 0 || right < 0 ? -1 : left * 16 + right;
			};
			if (text.size() == 4 || text.size() == 5) {
				const int red = nibble(text[1]);
				const int green = nibble(text[2]);
				const int blue = nibble(text[3]);
				const int alpha = text.size() == 5 ? nibble(text[4]) : 15;
				if (red < 0 || green < 0 || blue < 0 || alpha < 0) return std::nullopt;
				return ThemeColor{ static_cast<std::uint8_t>(red * 17),
					static_cast<std::uint8_t>(green * 17), static_cast<std::uint8_t>(blue * 17),
					static_cast<std::uint8_t>(alpha * 17) };
			}
			if (text.size() == 7 || text.size() == 9) {
				const int red = byte(text[1], text[2]);
				const int green = byte(text[3], text[4]);
				const int blue = byte(text[5], text[6]);
				const int alpha = text.size() == 9 ? byte(text[7], text[8]) : 255;
				if (red < 0 || green < 0 || blue < 0 || alpha < 0) return std::nullopt;
				return ThemeColor{ static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green),
					static_cast<std::uint8_t>(blue), static_cast<std::uint8_t>(alpha) };
			}
		}

		unsigned int red = 0;
		unsigned int green = 0;
		unsigned int blue = 0;
		float alpha = 1.0F;
		if (::swscanf_s(text.c_str(), L"rgb(%u,%u,%u)", &red, &green, &blue) == 3
			|| ::swscanf_s(text.c_str(), L"rgb(%u, %u, %u)", &red, &green, &blue) == 3) {
			if (red > 255 || green > 255 || blue > 255) return std::nullopt;
			return ThemeColor{ static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green),
				static_cast<std::uint8_t>(blue), 0xFF };
		}
		if (::swscanf_s(text.c_str(), L"rgba(%u,%u,%u,%f)", &red, &green, &blue, &alpha) == 4
			|| ::swscanf_s(text.c_str(), L"rgba(%u, %u, %u, %f)", &red, &green, &blue, &alpha) == 4) {
			if (red > 255 || green > 255 || blue > 255 || !std::isfinite(alpha)
				|| alpha < 0.0F || alpha > 1.0F) return std::nullopt;
			return ThemeColor{ static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green),
				static_cast<std::uint8_t>(blue), static_cast<std::uint8_t>(std::lround(alpha * 255.0F)) };
		}
	}
	catch (...) {
		return std::nullopt;
	}
	return std::nullopt;
}

ThemeMode CColorThemeRegistry::ModeForKind(ColorThemeKind kind) noexcept
{
	return kind == ColorThemeKind::Light || kind == ColorThemeKind::HighContrastLight
		? ThemeMode::Light : ThemeMode::Dark;
}

ThemePalette CColorThemeRegistry::ProjectPalette(
	ColorThemeKind kind, const std::map<std::wstring, ThemeColor, std::less<>>& colors) noexcept
{
	ThemePalette palette = CThemeService::PaletteFor(ModeForKind(kind));
	try {
		const auto first = [&colors](ThemeColor fallback, std::initializer_list<std::wstring_view> names) {
			for (const auto name : names) {
				const auto found = colors.find(std::wstring(name));
				if (found != colors.end()) return Composite(found->second, fallback);
			}
			return fallback;
		};
		const auto firstOver = [&colors](ThemeColor background, std::initializer_list<std::wstring_view> names) {
			for (const auto name : names) {
				const auto found = colors.find(std::wstring(name));
				if (found != colors.end()) return Composite(found->second, background);
			}
			return background;
		};
		palette.canvas = first(palette.canvas, { L"editor.background", L"editorGroup.emptyBackground" });
		// Side Bar and Panel are separate VS Code Parts. `sideBar` is the Primary
		// Side Bar/Explorer surface; `panel` remains the Secondary Side Bar role
		// used by the right host and by legacy workbench controls.
		palette.sideBar = first(palette.sideBar, { L"sideBar.background" });
		palette.panel = first(palette.panel, { L"panel.background", L"editorGroupHeader.tabsBackground" });
		palette.bottomPanel = first(palette.bottomPanel, { L"panel.background", L"editorGroupHeader.tabsBackground" });
		palette.raised = first(palette.raised, { L"sideBarSectionHeader.background", L"list.hoverBackground",
			L"editorWidget.background", L"quickInput.background" });
		palette.border = first(palette.border, { L"sideBar.border", L"panel.border", L"contrastBorder",
			L"editorGroup.border", L"editorWidget.border" });
		palette.primaryText = first(palette.primaryText, { L"foreground", L"editor.foreground", L"sideBar.foreground",
			L"panel.foreground" });
		palette.secondaryText = first(palette.secondaryText, { L"sideBar.foreground", L"panelTitle.inactiveForeground",
			L"activityBar.inactiveForeground" });
		palette.descriptionText = firstOver(palette.canvas, { L"descriptionForeground" });
		palette.disabledText = firstOver(palette.canvas, { L"disabledForeground" });
		palette.accent = first(palette.accent, { L"focusBorder", L"textLink.foreground", L"button.background",
			L"activityBarBadge.background" });
		palette.highlightText = first(palette.highlightText, { L"button.foreground", L"list.activeSelectionForeground" });
		palette.titleBar = first(palette.titleBar, { L"titleBar.activeBackground", L"titleBar.inactiveBackground" });
		palette.activityBar = first(palette.activityBar, { L"activityBar.background" });
		palette.danger = first(palette.danger, { L"errorForeground", L"editorError.foreground", L"errorLens.foreground" });
		palette.warning = first(palette.warning, { L"notificationsWarningIcon.foreground", L"editorWarning.foreground",
			L"problemsWarningIcon.foreground" });
	}
	catch (...) {
		// A malformed/oversized map cannot make the native workbench lose its
		// deterministic fallback palette.
	}
	return palette;
}

namespace {

bool ScopeStartsWith(std::wstring_view scope, std::wstring_view prefix)
{
	return scope.size() >= prefix.size() && scope.compare(0, prefix.size(), prefix) == 0
		&& (scope.size() == prefix.size() || scope[prefix.size()] == L'.');
}

bool ScopeMatches(std::wstring_view scope, ThemeSyntaxTokenKind kind)
{
	const auto lower = Lower(scope);
	switch (kind) {
	case ThemeSyntaxTokenKind::Comment:
		return ScopeStartsWith(lower, L"comment");
	case ThemeSyntaxTokenKind::String:
		return ScopeStartsWith(lower, L"string") && !ScopeStartsWith(lower, L"string.regexp");
	case ThemeSyntaxTokenKind::Number:
		return ScopeStartsWith(lower, L"constant.numeric") || ScopeStartsWith(lower, L"number");
	case ThemeSyntaxTokenKind::Keyword:
		return ScopeStartsWith(lower, L"keyword") || ScopeStartsWith(lower, L"storage");
	case ThemeSyntaxTokenKind::Type:
		return ScopeStartsWith(lower, L"entity.name.type") || ScopeStartsWith(lower, L"support.type")
			|| ScopeStartsWith(lower, L"storage.type") || ScopeStartsWith(lower, L"entity.name.class")
			|| ScopeStartsWith(lower, L"entity.name.enum") || ScopeStartsWith(lower, L"type");
	case ThemeSyntaxTokenKind::Function:
		return ScopeStartsWith(lower, L"entity.name.function") || ScopeStartsWith(lower, L"support.function")
			|| ScopeStartsWith(lower, L"meta.function-call") || ScopeStartsWith(lower, L"function");
	case ThemeSyntaxTokenKind::Variable:
		return ScopeStartsWith(lower, L"variable") || ScopeStartsWith(lower, L"entity.name.variable")
			|| ScopeStartsWith(lower, L"parameter") || ScopeStartsWith(lower, L"property");
	case ThemeSyntaxTokenKind::Constant:
		return ScopeStartsWith(lower, L"constant") || ScopeStartsWith(lower, L"support.constant")
			|| ScopeStartsWith(lower, L"language.constant");
	case ThemeSyntaxTokenKind::Regexp:
		return ScopeStartsWith(lower, L"string.regexp") || ScopeStartsWith(lower, L"constant.regexp")
			|| ScopeStartsWith(lower, L"regexp");
	case ThemeSyntaxTokenKind::Tag:
		return ScopeStartsWith(lower, L"entity.name.tag") || ScopeStartsWith(lower, L"meta.tag")
			|| ScopeStartsWith(lower, L"tag");
	case ThemeSyntaxTokenKind::Attribute:
		return ScopeStartsWith(lower, L"entity.other.attribute-name")
			|| ScopeStartsWith(lower, L"entity.name.tag.attribute") || ScopeStartsWith(lower, L"attribute");
	case ThemeSyntaxTokenKind::Invalid:
		return ScopeStartsWith(lower, L"invalid");
	}
	return false;
}

bool RuleMatches(const ThemeTokenColorRule& rule, ThemeSyntaxTokenKind kind)
{
	for (const auto& scope : rule.scopes) {
		std::wstring token;
		for (const wchar_t character : scope) {
			if (std::iswspace(character) || character == L',') {
				if (!token.empty() && ScopeMatches(token, kind)) return true;
				token.clear();
			} else {
				token.push_back(character);
			}
		}
		if (!token.empty() && ScopeMatches(token, kind)) return true;
	}
	return false;
}

void ApplyRule(const ThemeTokenColorRule& rule, ThemeSyntaxStyle& style)
{
	if (rule.foreground) style.foreground = rule.foreground;
	if (rule.background) style.background = rule.background;
	if (!rule.fontStyle.empty()) {
		const auto fontStyle = Lower(rule.fontStyle);
		style.bold = fontStyle.find(L"bold") != std::wstring::npos;
		style.underline = fontStyle.find(L"underline") != std::wstring::npos;
	}
}

std::optional<ThemeSyntaxTokenKind> SemanticKind(std::wstring_view key)
{
	const auto lower = Lower(key);
	const auto separator = lower.find(L'.');
	const auto base = lower.substr(0, separator);
	if (base == L"comment") return ThemeSyntaxTokenKind::Comment;
	if (base == L"string") return ThemeSyntaxTokenKind::String;
	if (base == L"number") return ThemeSyntaxTokenKind::Number;
	if (base == L"keyword") return ThemeSyntaxTokenKind::Keyword;
	if (base == L"class" || base == L"interface" || base == L"type" || base == L"enum"
		|| base == L"namespace" || base == L"typeparameter") return ThemeSyntaxTokenKind::Type;
	if (base == L"function" || base == L"method") return ThemeSyntaxTokenKind::Function;
	if (base == L"variable" || base == L"parameter" || base == L"property" || base == L"field") {
		return ThemeSyntaxTokenKind::Variable;
	}
	if (base == L"constant" || base == L"enummember") return ThemeSyntaxTokenKind::Constant;
	if (base == L"regexp") return ThemeSyntaxTokenKind::Regexp;
	return std::nullopt;
}

} // namespace

ThemeSyntaxPalette CColorThemeRegistry::ProjectSyntaxPalette(
	const std::vector<ThemeTokenColorRule>& tokenColors,
	const std::map<std::wstring, ThemeColor, std::less<>>& semanticTokenColors,
	bool semanticHighlighting) noexcept
{
	ThemeSyntaxPalette palette;
	try {
		constexpr std::array kinds{
			ThemeSyntaxTokenKind::Comment, ThemeSyntaxTokenKind::String, ThemeSyntaxTokenKind::Number,
			ThemeSyntaxTokenKind::Keyword, ThemeSyntaxTokenKind::Type, ThemeSyntaxTokenKind::Function,
			ThemeSyntaxTokenKind::Variable, ThemeSyntaxTokenKind::Constant, ThemeSyntaxTokenKind::Regexp,
			ThemeSyntaxTokenKind::Tag, ThemeSyntaxTokenKind::Attribute, ThemeSyntaxTokenKind::Invalid,
		};
		for (const auto& rule : tokenColors) {
			// TextMate's full selector precedence needs a grammar scope tree. In the
			// native renderer the safe approximation is the published rule order:
			// later matching rules replace earlier projected properties.
			for (const auto kind : kinds) {
				if (RuleMatches(rule, kind)) ApplyRule(rule, palette.For(kind));
			}
		}
		if (semanticHighlighting) {
			for (const auto& [key, color] : semanticTokenColors) {
				if (const auto kind = SemanticKind(key)) palette.For(*kind).foreground = color;
			}
		}
	}
	catch (...) {
		// A malformed theme cannot make the editor lose its native syntax colors.
		return {};
	}
	return palette;
}

bool CColorThemeRegistry::RegisterExtension(
	std::wstring_view extensionId, const std::filesystem::path& extensionRoot)
{
	try {
		const std::wstring ownedExtensionId(extensionId);
		m_themes.erase(std::remove_if(m_themes.begin(), m_themes.end(),
			[&ownedExtensionId](const ColorThemeInfo& theme) { return theme.extensionId == ownedExtensionId; }), m_themes.end());
		if (ownedExtensionId.empty()) return false;
		std::wstring manifestDiagnostic;
		const auto document = ReadJson(extensionRoot / L"package.json", manifestDiagnostic);
		// The diagnostic is intentionally local to this manifest read. Theme
		// discovery is best-effort per extension and never blocks other extensions.
		if (!document) return false;
		const auto* root = AsObject(&*document);
		if (root == nullptr) return false;
		const auto* contributes = AsObject(Member(*root, L"contributes"));
		const auto* themes = contributes == nullptr ? nullptr : AsArray(Member(*contributes, L"themes"));
		if (themes == nullptr) return false;
		std::vector<ColorThemeInfo> discovered;
		for (const auto& item : *themes) {
			if (discovered.size() >= kMaximumThemeContributions) break;
			const auto* object = AsObject(&item);
			if (object == nullptr) continue;
			const auto* label = AsString(Member(*object, L"label"));
			const auto* path = AsString(Member(*object, L"path"));
			if (label == nullptr || label->empty() || path == nullptr) continue;
			const auto themePath = SafePath(extensionRoot, extensionRoot, *path);
			if (!themePath || !std::filesystem::is_regular_file(*themePath)) continue;
			ColorThemeInfo info;
			info.extensionId = ownedExtensionId;
			info.extensionRoot = extensionRoot;
			info.label = *label;
			if (const auto* id = AsString(Member(*object, L"id")); id != nullptr && !id->empty()) {
				info.id = *id;
			} else {
				info.id = ownedExtensionId + L":" + *label;
			}
			info.themePath = *themePath;
			if (const auto* uiTheme = AsString(Member(*object, L"uiTheme"))) {
				info.kind = KindForUiTheme(*uiTheme);
			}
			if (std::any_of(discovered.begin(), discovered.end(), [&info](const ColorThemeInfo& existing) {
				return existing.id == info.id;
			})) continue;
			discovered.push_back(std::move(info));
		}
		m_themes.insert(m_themes.end(), discovered.begin(), discovered.end());
		return !discovered.empty();
	}
	catch (...) {
		return false;
	}
}

bool CColorThemeRegistry::RegisterBuiltinThemes()
{
	try {
		m_themes.erase(std::remove_if(m_themes.begin(), m_themes.end(), [](const ColorThemeInfo& theme) {
			return theme.isBuiltin;
		}), m_themes.end());
		m_builtinThemeDocuments.clear();

		const auto registerTheme = [this](std::wstring_view id, std::wstring_view label,
			ColorThemeKind kind, std::string_view document) {
			ColorThemeInfo info;
			info.id = id;
			info.label = label;
			info.extensionId = kBuiltinThemeExtensionId;
			info.kind = kind;
			info.isBuiltin = true;
			m_builtinThemeDocuments.emplace(info.id, document);
			m_themes.push_back(std::move(info));
		};
		registerTheme(BuiltinThemeId(ThemeMode::Dark), L"Sakura Default Dark", ColorThemeKind::Dark,
			kSakuraDefaultDarkThemeJson);
		registerTheme(BuiltinThemeId(ThemeMode::Light), L"Sakura Default Light", ColorThemeKind::Light,
			kSakuraDefaultLightThemeJson);
		return m_builtinThemeDocuments.size() == 2U;
	}
	catch (...) {
		m_themes.erase(std::remove_if(m_themes.begin(), m_themes.end(), [](const ColorThemeInfo& theme) {
			return theme.isBuiltin;
		}), m_themes.end());
		m_builtinThemeDocuments.clear();
		return false;
	}
}

void CColorThemeRegistry::Clear() noexcept
{
	m_themes.clear();
	m_builtinThemeDocuments.clear();
}

std::vector<ColorThemeInfo> CColorThemeRegistry::Themes() const
{
	std::vector<ColorThemeInfo> result = m_themes;
	std::sort(result.begin(), result.end(), [](const ColorThemeInfo& left, const ColorThemeInfo& right) {
		if (left.label != right.label) return left.label < right.label;
		return left.id < right.id;
	});
	return result;
}

ColorThemeLoadResult CColorThemeRegistry::Load(std::wstring_view idOrLabel) const
{
	ColorThemeLoadResult result;
	try {
		const ColorThemeInfo* selected = nullptr;
		for (const auto& theme : m_themes) {
			if (theme.id == idOrLabel) {
				selected = &theme;
				break;
			}
		}
		if (selected == nullptr) {
			const auto query = Lower(idOrLabel);
			for (const auto& theme : m_themes) {
				if (Lower(theme.label) == query) {
					selected = &theme;
					break;
				}
			}
		}
		if (selected == nullptr) {
			result.diagnostic = L"color theme was not found: " + std::wstring(idOrLabel);
			return result;
		}
		ColorThemeSnapshot snapshot;
		snapshot.info = *selected;
		if (selected->isBuiltin) {
			const auto document = m_builtinThemeDocuments.find(selected->id);
			if (document == m_builtinThemeDocuments.end()
				|| !LoadEmbeddedThemeDocument(document->second, snapshot, result.diagnostic)) {
				return result;
			}
			snapshot.palette = ProjectPalette(snapshot.info.kind, snapshot.colors);
			snapshot.syntaxPalette = ProjectSyntaxPalette(snapshot.tokenColors, snapshot.semanticTokenColors,
				snapshot.semanticHighlighting);
			result.theme = std::move(snapshot);
			return result;
		}
		bool rootTypeWasExplicit = false;
		std::set<std::wstring> activeFiles;
		if (!LoadThemeFile(selected->themePath, selected->extensionRoot, snapshot, true,
			rootTypeWasExplicit, activeFiles, 0, result.diagnostic)) {
			return result;
		}
		snapshot.palette = ProjectPalette(snapshot.info.kind, snapshot.colors);
		snapshot.syntaxPalette = ProjectSyntaxPalette(snapshot.tokenColors, snapshot.semanticTokenColors,
			snapshot.semanticHighlighting);
		result.theme = std::move(snapshot);
		return result;
	}
	catch (...) {
		result.theme.reset();
		result.diagnostic = L"color theme loading failed unexpectedly";
		return result;
	}
}

} // namespace theme
