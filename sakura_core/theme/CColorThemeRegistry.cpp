/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "theme/CColorThemeRegistry.h"

#include <sakura/serialization/JsoncDocument.h>
#include "util/string_ex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cwchar>

namespace theme {
namespace {

using JsoncValue = platform::serialization::JsoncValue;
using JsoncObject = JsoncValue::Object;
using JsoncArray = JsoncValue::Array;

constexpr std::size_t kMaximumTokenColorRules = 16'384;
constexpr std::size_t kMaximumSemanticTokenColors = 16'384;

constexpr std::string_view kSakuraDefaultDarkThemeJson = R"json({
	"name": "Sakura Default Dark",
	"type": "dark",
	"colors": {
		"editor.background": "#1E1E1E",
		"editorGutter.background": "#1E1E1E",
		"editorWhitespace.foreground": "#E3E4E229",
		"editorIndentGuide.background1": "#404040",
		"editorLineNumber.foreground": "#858585",
		"editorLineNumber.activeForeground": "#CCCCCC",
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
		"activityBar.border": "#454545",
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
		"editor.background": "#FFFFFF",
		"editorGutter.background": "#FFFFFF",
		"editorWhitespace.foreground": "#33333333",
		"editorIndentGuide.background1": "#D3D3D3",
		"editorLineNumber.foreground": "#6E7681",
		"editorLineNumber.activeForeground": "#171184",
		"sideBar.background": "#F8F8F8",
		"panel.background": "#F8F8F8",
		"sideBarSectionHeader.background": "#F8F8F8",
		"sideBar.border": "#E5E5E5",
		"panel.border": "#E5E5E5",
		"foreground": "#3B3B3B",
		"sideBar.foreground": "#3B3B3B",
		"descriptionForeground": "#3B3B3B",
		"disabledForeground": "#61616180",
		"focusBorder": "#005FB8",
		"button.background": "#005FB8",
		"button.foreground": "#FFFFFF",
		"button.hoverBackground": "#0258A8",
		"titleBar.activeBackground": "#F8F8F8",
		"activityBar.background": "#F8F8F8",
		"activityBar.border": "#E5E5E5",
		"errorForeground": "#F85149",
		"notificationsWarningIcon.foreground": "#BF8800",
		"quickInput.background": "#F8F8F8",
		"input.background": "#FFFFFF",
		"input.border": "#CECECE",
		"list.activeSelectionBackground": "#E8E8E8",
		"list.activeSelectionForeground": "#000000",
		"list.hoverBackground": "#F2F2F2",
		"list.focusAndSelectionOutline": "#005FB8"
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

bool ApplyThemeObjectFields(
	const JsoncObject& object,
	ColorThemeSnapshot& snapshot,
	bool rootFile,
	bool& rootTypeWasExplicit,
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
		} else {
			diagnostic = L"bundled color theme tokenColors must be an array";
			return false;
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
	return ApplyThemeObjectFields(*object, snapshot, true, rootTypeWasExplicit, diagnostic);
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

//! Reproduces VS Code's `lighten(color, factor)` / `darken(color, factor)`, which
//! move the HSL lightness by `l * factor` and leave hue/saturation alone. A
//! positive factor lightens, a negative one darkens. Alpha is dropped because
//! every derived role here is already composited to an opaque GDI color.
ThemeColor AdjustLightness(ThemeColor color, double factor) noexcept
{
	const double red = color.red / 255.0;
	const double green = color.green / 255.0;
	const double blue = color.blue / 255.0;
	const double maximum = std::max({ red, green, blue });
	const double minimum = std::min({ red, green, blue });
	double hue = 0.0;
	double saturation = 0.0;
	const double lightness = (maximum + minimum) / 2.0;
	const double chroma = maximum - minimum;
	if (chroma > 0.0) {
		saturation = lightness > 0.5 ? chroma / (2.0 - maximum - minimum) : chroma / (maximum + minimum);
		if (maximum == red) hue = (green - blue) / chroma + (green < blue ? 6.0 : 0.0);
		else if (maximum == green) hue = (blue - red) / chroma + 2.0;
		else hue = (red - green) / chroma + 4.0;
		hue /= 6.0;
	}
	const double adjusted = std::clamp(lightness + lightness * factor, 0.0, 1.0);
	const auto channel = [](double p, double q, double t) noexcept {
		if (t < 0.0) t += 1.0;
		if (t > 1.0) t -= 1.0;
		if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
		if (t < 1.0 / 2.0) return q;
		if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
		return p;
	};
	const auto quantize = [](double value) noexcept {
		return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
	};
	if (saturation <= 0.0) {
		const std::uint8_t gray = quantize(adjusted);
		return { gray, gray, gray, 0xFF };
	}
	const double q = adjusted < 0.5 ? adjusted * (1.0 + saturation)
		: adjusted + saturation - adjusted * saturation;
	const double p = 2.0 * adjusted - q;
	return { quantize(channel(p, q, hue + 1.0 / 3.0)), quantize(channel(p, q, hue)),
		quantize(channel(p, q, hue - 1.0 / 3.0)), 0xFF };
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
		const auto firstOverWithFallback = [&colors](ThemeColor background, ThemeColor fallback,
			std::initializer_list<std::wstring_view> names) {
			for (const auto name : names) {
				const auto found = colors.find(std::wstring(name));
				if (found != colors.end()) return Composite(found->second, background);
			}
			return fallback;
		};
		const auto firstRaw = [&colors](ThemeColor fallback,
			std::initializer_list<std::wstring_view> names) {
			for (const auto name : names) {
				const auto found = colors.find(std::wstring(name));
				if (found != colors.end()) return found->second;
			}
			return fallback;
		};
		palette.canvas = first(palette.canvas, { L"editor.background", L"editorGroup.emptyBackground" });
		palette.editorGutterBackground = first(palette.canvas, { L"editorGutter.background" });
		palette.editorLineNumberForeground = firstOverWithFallback(palette.editorGutterBackground,
			palette.editorLineNumberForeground, { L"editorLineNumber.foreground" });
		palette.editorLineNumberActiveForeground = firstOverWithFallback(palette.editorGutterBackground,
			palette.editorLineNumberActiveForeground, { L"editorLineNumber.activeForeground" });
		// The diff washes are translucent by definition, and upstream composites them
		// over the editor background rather than over the gutter, so `canvas` is the base.
		palette.diffInsertedLineBackground = firstOverWithFallback(palette.canvas,
			palette.diffInsertedLineBackground, { L"diffEditor.insertedLineBackground" });
		palette.diffRemovedLineBackground = firstOverWithFallback(palette.canvas,
			palette.diffRemovedLineBackground, { L"diffEditor.removedLineBackground" });
		palette.diffDiagonalFill = firstOverWithFallback(palette.canvas,
			palette.diffDiagonalFill, { L"diffEditor.diagonalFill" });
		// Side Bar and Panel are separate VS Code Parts. `sideBar` serves both
		// physical side-bar hosts; `bottomPanel` is the Panel surface and the
		// fallback base for the Panel-only Terminal ViewContainer.
		palette.sideBar = first(palette.sideBar, { L"sideBar.background" });
		palette.panel = first(palette.panel, { L"panel.background", L"editorGroupHeader.tabsBackground" });
		palette.bottomPanel = first(palette.bottomPanel, { L"panel.background", L"editorGroupHeader.tabsBackground" });
		palette.terminalBackground = first(palette.bottomPanel, { L"terminal.background" });
		palette.raised = first(palette.raised, { L"list.hoverBackground", L"sideBarSectionHeader.background",
			L"editorWidget.background", L"quickInput.background" });
		// `activityBar.border` is a fallback when a theme omits `sideBar.border`;
		// the Activity Bar paints this shared Part-edge color on its right edge.
		palette.border = first(palette.border, { L"sideBar.border", L"activityBar.border", L"panel.border",
			L"contrastBorder", L"editorGroup.border", L"editorWidget.border" });
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
		// The button role is its own token, not a synonym for `accent`: upstream's
		// `media/updateTitleBarEntry.css` paints the actionable title-bar Update button
		// with `--vscode-button-background`/`--vscode-button-foreground` and hovers it
		// with `--vscode-button-hoverBackground`. `accent` may already have consumed
		// `button.background` as its third fallback candidate above, which is a
		// deliberately different question (what does focus look like) from this one.
		palette.buttonBackground = first(palette.buttonBackground, { L"button.background" });
		palette.buttonForeground = first(palette.buttonForeground, { L"button.foreground" });
		// A theme that sets only `button.background` gets a hover derived from the
		// resolved background rather than the compiled default, so the two colors can
		// never come from different themes. Upstream registers exactly this derivation:
		// `lighten(button.background, 0.2)` for dark and `darken(..., 0.2)` for light.
		// The Activity Bar badge is a third distinct role alongside `accent` and the
		// button roles; see the note on ThemePalette::activityBarBadgeBackground.
		palette.activityBarBadgeBackground = first(palette.activityBarBadgeBackground,
			{ L"activityBarBadge.background" });
		palette.activityBarBadgeForeground = first(palette.activityBarBadgeForeground,
			{ L"activityBarBadge.foreground" });
		// Quick Input owns a single-line input and a list. Keep these roles explicit
		// instead of deriving them from the broad panel/accent colors so the native
		// overlay can follow VS Code's low-contrast Light Modern treatment.
		palette.quickInputBackground = first(palette.quickInputBackground, { L"quickInput.background" });
		palette.inputBackground = first(palette.inputBackground, { L"input.background" });
		palette.inputBorder = first(palette.inputBorder, { L"input.border" });
		palette.listActiveSelectionBackground = first(palette.listActiveSelectionBackground,
			{ L"list.activeSelectionBackground" });
		palette.listActiveSelectionForeground = first(palette.listActiveSelectionForeground,
			{ L"list.activeSelectionForeground" });
		palette.listHoverBackground = first(palette.listHoverBackground, { L"list.hoverBackground" });
		palette.listFocusAndSelectionOutline = first(palette.listFocusAndSelectionOutline,
			{ L"list.focusAndSelectionOutline" });
		// Unlike ordinary GDI roles, scrollbar colors remain translucent until a
		// concrete Editor/Side Bar/Panel/Quick Input surface is known. VS Code uses
		// the same slider tokens over all of those backgrounds.
		palette.scrollbarBackground = firstRaw(palette.scrollbarBackground,
			{ L"scrollbar.background" });
		palette.scrollbarSliderBackground = firstRaw(palette.scrollbarSliderBackground,
			{ L"scrollbarSlider.background" });
		palette.scrollbarSliderHoverBackground = firstRaw(palette.scrollbarSliderHoverBackground,
			{ L"scrollbarSlider.hoverBackground" });
		palette.scrollbarSliderActiveBackground = firstRaw(palette.scrollbarSliderActiveBackground,
			{ L"scrollbarSlider.activeBackground" });
		palette.editorWhitespaceForeground = firstRaw(palette.editorWhitespaceForeground,
			{ L"editorWhitespace.foreground" });
		palette.editorIndentGuideBackground = firstRaw(palette.editorIndentGuideBackground,
			{ L"editorIndentGuide.background1", L"editorIndentGuide.background" });
		palette.minimapBackground = first(palette.canvas, { L"minimap.background" });
		palette.minimapForegroundOpacity = firstRaw(palette.minimapForegroundOpacity,
			{ L"minimap.foregroundOpacity" });
		const auto halfAlpha = [](ThemeColor color) noexcept {
			color.alpha = static_cast<std::uint8_t>(color.alpha / 2);
			return color;
		};
		palette.minimapSliderBackground = firstRaw(halfAlpha(palette.scrollbarSliderBackground),
			{ L"minimapSlider.background" });
		palette.minimapSliderHoverBackground = firstRaw(
			halfAlpha(palette.scrollbarSliderHoverBackground),
			{ L"minimapSlider.hoverBackground" });
		palette.minimapSliderActiveBackground = firstRaw(
			halfAlpha(palette.scrollbarSliderActiveBackground),
			{ L"minimapSlider.activeBackground" });
		// The Search view's match highlight is the editor's own find-match role, which
		// is what upstream's `searchResult` rendering reuses.
		palette.searchMatchHighlightBackground = firstOverWithFallback(palette.sideBar,
			palette.searchMatchHighlightBackground, { L"editor.findMatchHighlightBackground" });
		// The `gitDecoration.*` keys are contributed by the Git extension, so a theme
		// that names one is overriding that extension's own default rather than a
		// workbench role, and there is no second key to fall back to.
		palette.gitAddedResourceForeground = first(palette.gitAddedResourceForeground,
			{ L"gitDecoration.addedResourceForeground" });
		palette.gitModifiedResourceForeground = first(palette.gitModifiedResourceForeground,
			{ L"gitDecoration.modifiedResourceForeground" });
		palette.gitDeletedResourceForeground = first(palette.gitDeletedResourceForeground,
			{ L"gitDecoration.deletedResourceForeground" });
		palette.gitRenamedResourceForeground = first(palette.gitRenamedResourceForeground,
			{ L"gitDecoration.renamedResourceForeground" });
		palette.gitStageModifiedResourceForeground = first(palette.gitStageModifiedResourceForeground,
			{ L"gitDecoration.stageModifiedResourceForeground" });
		palette.gitStageDeletedResourceForeground = first(palette.gitStageDeletedResourceForeground,
			{ L"gitDecoration.stageDeletedResourceForeground" });
		palette.gitUntrackedResourceForeground = first(palette.gitUntrackedResourceForeground,
			{ L"gitDecoration.untrackedResourceForeground" });
		palette.gitIgnoredResourceForeground = first(palette.gitIgnoredResourceForeground,
			{ L"gitDecoration.ignoredResourceForeground" });
		palette.gitConflictingResourceForeground = first(palette.gitConflictingResourceForeground,
			{ L"gitDecoration.conflictingResourceForeground" });
		palette.gitSubmoduleResourceForeground = first(palette.gitSubmoduleResourceForeground,
			{ L"gitDecoration.submoduleResourceForeground" });
		palette.buttonHoverBackground = firstOverWithFallback(palette.canvas,
			AdjustLightness(palette.buttonBackground,
				ModeForKind(kind) == ThemeMode::Dark ? 0.2 : -0.2),
			{ L"button.hoverBackground" });
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

bool CColorThemeRegistry::RegisterBuiltinThemes()
{
	try {
		m_themes.clear();
		m_builtinThemeDocuments.clear();

		const auto registerTheme = [this](std::wstring_view id, std::wstring_view label,
			ColorThemeKind kind, std::string_view document) {
			ColorThemeInfo info;
			info.id = id;
			info.label = label;
			info.kind = kind;
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
		m_themes.clear();
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
	catch (...) {
		result.theme.reset();
		result.diagnostic = L"color theme loading failed unexpectedly";
		return result;
	}
}

} // namespace theme
