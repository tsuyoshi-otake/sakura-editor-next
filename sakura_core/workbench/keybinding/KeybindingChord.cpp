/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/keybinding/KeybindingChord.h"

#include <algorithm>
#include <array>
#include <string>

namespace workbench::keybinding {

namespace {

/*
	仮想キーコードは Windows.h の VK_* と同値だが、数値で書いてある。
	このモデルはメッセージループから切り離して検証できることが要件なので、
	Windows.h を引き込まない。値がずれると全て壊れるため、名前をコメントに残す。
*/
constexpr std::uint32_t kVkBack = 0x08;
constexpr std::uint32_t kVkTab = 0x09;
constexpr std::uint32_t kVkReturn = 0x0D;
constexpr std::uint32_t kVkPause = 0x13;
constexpr std::uint32_t kVkCapital = 0x14;
constexpr std::uint32_t kVkEscape = 0x1B;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkPrior = 0x21;	// PageUp
constexpr std::uint32_t kVkNext = 0x22;		// PageDown
constexpr std::uint32_t kVkEnd = 0x23;
constexpr std::uint32_t kVkHome = 0x24;
constexpr std::uint32_t kVkLeft = 0x25;
constexpr std::uint32_t kVkUp = 0x26;
constexpr std::uint32_t kVkRight = 0x27;
constexpr std::uint32_t kVkDown = 0x28;
constexpr std::uint32_t kVkInsert = 0x2D;
constexpr std::uint32_t kVkDelete = 0x2E;
constexpr std::uint32_t kVkNumpad0 = 0x60;
constexpr std::uint32_t kVkMultiply = 0x6A;
constexpr std::uint32_t kVkAdd = 0x6B;
constexpr std::uint32_t kVkSeparator = 0x6C;
constexpr std::uint32_t kVkSubtract = 0x6D;
constexpr std::uint32_t kVkDecimal = 0x6E;
constexpr std::uint32_t kVkDivide = 0x6F;
constexpr std::uint32_t kVkF1 = 0x70;
constexpr std::uint32_t kVkNumLock = 0x90;
constexpr std::uint32_t kVkScroll = 0x91;
constexpr std::uint32_t kVkOem1 = 0xBA;			// ;:
constexpr std::uint32_t kVkOemPlus = 0xBB;		// +=
constexpr std::uint32_t kVkOemComma = 0xBC;		// ,<
constexpr std::uint32_t kVkOemMinus = 0xBD;		// -_
constexpr std::uint32_t kVkOemPeriod = 0xBE;	// .>
constexpr std::uint32_t kVkOem2 = 0xBF;			// /?
constexpr std::uint32_t kVkOem3 = 0xC0;			// `~
constexpr std::uint32_t kVkOem4 = 0xDB;			// [{
constexpr std::uint32_t kVkOem5 = 0xDC;			// \|
constexpr std::uint32_t kVkOem6 = 0xDD;			// ]}
constexpr std::uint32_t kVkOem7 = 0xDE;			// '"

//! 名前つきキー。VS Code の `keybindingLabels` に載っている綴りと別名を両方持つ。
struct NamedKey {
	std::wstring_view	name;
	std::uint32_t		virtualKey;
};

constexpr std::array kNamedKeys = std::to_array<NamedKey>({
	{ L"backspace", kVkBack },
	{ L"tab", kVkTab },
	{ L"enter", kVkReturn },
	{ L"return", kVkReturn },
	{ L"pausebreak", kVkPause },
	{ L"capslock", kVkCapital },
	{ L"escape", kVkEscape },
	{ L"esc", kVkEscape },
	{ L"space", kVkSpace },
	{ L"pageup", kVkPrior },
	{ L"pgup", kVkPrior },
	{ L"pagedown", kVkNext },
	{ L"pgdn", kVkNext },
	{ L"end", kVkEnd },
	{ L"home", kVkHome },
	{ L"left", kVkLeft },
	{ L"up", kVkUp },
	{ L"right", kVkRight },
	{ L"down", kVkDown },
	{ L"insert", kVkInsert },
	{ L"delete", kVkDelete },
	{ L"del", kVkDelete },
	{ L"numlock", kVkNumLock },
	{ L"scrolllock", kVkScroll },
	{ L"numpad_multiply", kVkMultiply },
	{ L"numpad_add", kVkAdd },
	{ L"numpad_separator", kVkSeparator },
	{ L"numpad_subtract", kVkSubtract },
	{ L"numpad_decimal", kVkDecimal },
	{ L"numpad_divide", kVkDivide },
	// 記号は US 配列の OEM 割り当て。VS Code も既定キーマップは US で書く。
	{ L";", kVkOem1 },
	{ L"=", kVkOemPlus },
	{ L",", kVkOemComma },
	{ L"-", kVkOemMinus },
	{ L".", kVkOemPeriod },
	{ L"/", kVkOem2 },
	{ L"`", kVkOem3 },
	{ L"[", kVkOem4 },
	{ L"\\", kVkOem5 },
	{ L"]", kVkOem6 },
	{ L"'", kVkOem7 },
});

[[nodiscard]] std::wstring ToLowerAscii(std::wstring_view value)
{
	std::wstring lowered(value);
	std::ranges::transform(lowered, lowered.begin(), [](wchar_t ch) {
		return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
	});
	return lowered;
}

/*!
	@brief `[KeyA]` 形式（スキャンコード指定）を素のキー名へ均す

	VS Code は配列非依存の指定としてこの形を許す。US 配列を前提にした
	仮想キーコードへ写す以上、`[KeyA]` と `a` は同じところへ着く。
	知らない綴りは空を返し、呼び出し側で「解釈できない式」として落とす。
*/
[[nodiscard]] std::wstring NormalizeScanCodeName(std::wstring_view token)
{
	if (token.size() < 3 || token.front() != L'[' || token.back() != L']') return std::wstring(token);
	const auto inner = token.substr(1, token.size() - 2);
	if (inner.size() == 4 && inner.starts_with(L"Key")) return ToLowerAscii(inner.substr(3));
	if (inner.size() == 6 && inner.starts_with(L"Digit")) return std::wstring(inner.substr(5));
	static constexpr std::array kScanCodeAliases = std::to_array<NamedKey>({
		{ L"Semicolon", kVkOem1 },
		{ L"Equal", kVkOemPlus },
		{ L"Comma", kVkOemComma },
		{ L"Minus", kVkOemMinus },
		{ L"Period", kVkOemPeriod },
		{ L"Slash", kVkOem2 },
		{ L"Backquote", kVkOem3 },
		{ L"BracketLeft", kVkOem4 },
		{ L"Backslash", kVkOem5 },
		{ L"BracketRight", kVkOem6 },
		{ L"Quote", kVkOem7 },
	});
	const auto alias = std::ranges::find(kScanCodeAliases, inner, &NamedKey::name);
	if (alias == kScanCodeAliases.end()) return {};
	// 記号の別名は、素の記号 1 文字と同じ経路で引けるよう記号そのものへ戻す。
	const auto named = std::ranges::find(kNamedKeys, alias->virtualKey, &NamedKey::virtualKey);
	return named == kNamedKeys.end() ? std::wstring{} : std::wstring(named->name);
}

//! キー名 1 個を仮想キーコードへ。解釈できなければ 0。
[[nodiscard]] std::uint32_t VirtualKeyOf(std::wstring_view name)
{
	if (name.empty()) return 0;
	if (name.size() == 1) {
		const auto ch = name.front();
		if (ch >= L'a' && ch <= L'z') return static_cast<std::uint32_t>(ch - L'a' + L'A');
		if (ch >= L'0' && ch <= L'9') return static_cast<std::uint32_t>(ch);
	}
	if (name.size() >= 2 && name.front() == L'f') {
		// f1..f24。先頭 0 埋め（`f01`）は VS Code も受け付けないので通さない。
		const auto digits = name.substr(1);
		if (digits.front() != L'0'
			&& std::ranges::all_of(digits, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
			unsigned value = 0;
			for (const auto ch : digits) value = value * 10 + static_cast<unsigned>(ch - L'0');
			if (value >= 1 && value <= 24) return kVkF1 + value - 1;
		}
	}
	if (name.size() == 7 && name.starts_with(L"numpad")) {
		const auto digit = name.back();
		if (digit >= L'0' && digit <= L'9') return kVkNumpad0 + static_cast<std::uint32_t>(digit - L'0');
	}
	const auto named = std::ranges::find(kNamedKeys, name, &NamedKey::name);
	return named == kNamedKeys.end() ? 0 : named->virtualKey;
}

//! 1 打鍵ぶんの式（`ctrl+shift+p`）を解釈する。失敗は virtualKey == 0 で返す。
[[nodiscard]] KeyStroke ParseStroke(std::wstring_view part)
{
	KeyStroke stroke;
	std::wstring_view rest = part;
	while (true) {
		const auto separator = rest.find(L'+');
		// 末尾の `+` 自体をキーにする書き方は VS Code に無い。`a+` は式の壊れ。
		if (separator == std::wstring_view::npos) break;
		const auto token = ToLowerAscii(rest.substr(0, separator));
		if (token == L"ctrl" || token == L"control") stroke.control = true;
		else if (token == L"shift") stroke.shift = true;
		else if (token == L"alt" || token == L"option") stroke.alt = true;
		else if (token == L"win" || token == L"meta" || token == L"cmd" || token == L"super") stroke.win = true;
		else return {};
		rest = rest.substr(separator + 1);
	}
	const auto normalized = NormalizeScanCodeName(rest);
	stroke.virtualKey = VirtualKeyOf(ToLowerAscii(normalized));
	return stroke.virtualKey == 0 ? KeyStroke{} : stroke;
}

} // namespace

std::vector<KeyStroke> ParseKeybinding(const std::wstring_view expression)
{
	std::vector<KeyStroke> strokes;
	std::wstring_view rest = expression;
	const auto isSpace = [](wchar_t ch) { return ch == L' ' || ch == L'\t'; };
	while (!rest.empty()) {
		while (!rest.empty() && isSpace(rest.front())) rest.remove_prefix(1);
		if (rest.empty()) break;
		std::size_t end = 0;
		while (end < rest.size() && !isSpace(rest[end])) ++end;
		const auto stroke = ParseStroke(rest.substr(0, end));
		// 打鍵 1 個でも読めなければ式ごと捨てる。半端な列は誤爆の元。
		if (stroke.virtualKey == 0) return {};
		if (strokes.size() >= kMaxChordStrokes) return {};
		strokes.push_back(stroke);
		rest = rest.substr(end);
	}
	return strokes;
}

} // namespace workbench::keybinding
