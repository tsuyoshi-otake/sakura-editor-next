/*! @file
	@brief 書式付きホバーの GDI 実装
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/hover/CHoverWidget.h"

#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CExtensionIconFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/ThemeIconResolver.h"

#include <algorithm>
#include <cwctype>
#include <shellapi.h>
#include <utility>

namespace workbench::hover {
namespace {

using workbench::icons::IconRect;
using workbench::icons::ScaleDip;

constexpr const wchar_t* kWindowClassName = L"SakuraHoverWidget";

//! フォントの役割。BuildLayout が SPositionedRun::fontIndex に入れる値。
enum EFont : int {
	FontRegular = 0,
	FontBold,
	FontItalic,
	FontBoldItalic,
	FontCode,
	FontHeading,
	FontCount,
};

//! VS Code の hover-contents は padding 4px/8px。日本語の行間を考えて縦を少し広く取る。
constexpr int kPaddingXDip = 9;
constexpr int kPaddingYDip = 6;
constexpr int kLineGapDip = 2;
constexpr int kBlockGapDip = 6;
constexpr int kRuleMarginDip = 4;
constexpr int kCellPaddingXDip = 7;
constexpr int kCellPaddingYDip = 2;
constexpr int kListIndentDip = 12;
constexpr int kListMarkerGapDip = 5;
constexpr int kInlineIconGapDip = 3;
constexpr int kCodeBlockPaddingDip = 4;
constexpr int kAnchorGapDip = 4;
//! VS Code の .monaco-hover の max-width。表だけはこれを超えて広がることを許す。
constexpr int kMaxContentWidthDip = 500;
constexpr int kMaxTableWidthDip = 900;
//! 1 つのホバーが描く実行単位の上限。信頼できない入力で描画時間が発散しないようにする。
constexpr std::size_t kMaxPositionedRuns = 4096;

[[nodiscard]] bool HasCaseInsensitivePrefix(std::wstring_view value, std::wstring_view prefix) noexcept
{
	if (value.size() < prefix.size()) return false;
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		if (std::towlower(static_cast<std::wint_t>(value[index])) !=
			std::towlower(static_cast<std::wint_t>(prefix[index]))) return false;
	}
	return true;
}

[[nodiscard]] bool IsAllowedLinkTarget(std::wstring_view target) noexcept
{
	// StatusBarItem.tooltip is extension-provided content. Keep command:, file:,
	// javascript:, and other shell-resolved schemes closed; VS Code's trusted
	// command-link policy is a separate capability that this surface does not expose.
	constexpr wchar_t httpsScheme[] = { L'h', L't', L't', L'p', L's', L':', L'/', L'/', L'\0' };
	constexpr wchar_t httpScheme[] = { L'h', L't', L't', L'p', L':', L'/', L'/', L'\0' };
	constexpr wchar_t mailtoScheme[] = { L'm', L'a', L'i', L'l', L't', L'o', L':', L'\0' };
	return HasCaseInsensitivePrefix(target, httpsScheme) ||
		HasCaseInsensitivePrefix(target, httpScheme) ||
		HasCaseInsensitivePrefix(target, mailtoScheme);
}

[[nodiscard]] bool IsCommandLinkTarget(std::wstring_view target) noexcept
{
	constexpr wchar_t commandScheme[] = {
		L'c', L'o', L'm', L'm', L'a', L'n', L'd', L':', L'\0'
	};
	return HasCaseInsensitivePrefix(target, commandScheme);
}

[[nodiscard]] std::wstring_view CommandIdFromLinkTarget(std::wstring_view target) noexcept
{
	constexpr wchar_t commandScheme[] = {
		L'c', L'o', L'm', L'm', L'a', L'n', L'd', L':', L'\0'
	};
	if (!HasCaseInsensitivePrefix(target, commandScheme)) return {};
	const std::size_t start = std::size(commandScheme) - 1;
	const std::size_t query = target.find(L'?', start);
	return target.substr(start, query == std::wstring_view::npos ? std::wstring_view::npos : query - start);
}

[[nodiscard]] HFONT MakeFont(const wchar_t* family, int pointSize, int weight, bool italic, UINT dpi) noexcept
{
	LOGFONTW logFont{};
	const int effectiveDpi = static_cast<int>(dpi == 0 ? 96U : dpi);
	logFont.lfHeight = -::MulDiv(pointSize, effectiveDpi, 72);
	logFont.lfWeight = weight;
	logFont.lfItalic = italic ? TRUE : FALSE;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	::wcsncpy_s(logFont.lfFaceName, family, _TRUNCATE);
	return ::CreateFontIndirectW(&logFont);
}

//! 装飾の組み合わせを 1 つのフォント役割へ落とす。
[[nodiscard]] int FontIndexFor(const SInlineRun& run, int forcedFont) noexcept
{
	if (run.code) return FontCode;
	if (forcedFont >= 0) return forcedFont;
	if (run.bold && run.italic) return FontBoldItalic;
	if (run.bold) return FontBold;
	if (run.italic) return FontItalic;
	return FontRegular;
}

[[nodiscard]] bool IsBreakableSpace(wchar_t ch) noexcept
{
	// U+00A0 は「折り返さない空白」なので、意図どおり分割候補から外す。
	return ch == L' ' || ch == L'\t';
}

//! 折り返しの単位。空白そのものもトークンにして、行頭に来たときだけ捨てられるようにする。
struct SToken {
	std::wstring text;
	std::wstring iconId;
	std::wstring linkTarget;
	int fontIndex = FontRegular;
	bool link = false;
	bool code = false;
	bool space = false;
};

[[nodiscard]] std::vector<SToken> Tokenize(const SInlineText& runs, int forcedFont)
{
	std::vector<SToken> tokens;
	for (const auto& run : runs) {
		if (!run.iconId.empty()) {
			SToken token;
			token.iconId = run.iconId;
			token.link = run.link;
			token.linkTarget = run.linkTarget;
			tokens.push_back(std::move(token));
			continue;
		}
		const int fontIndex = FontIndexFor(run, forcedFont);
		std::size_t index = 0;
		while (index < run.text.size()) {
			const bool space = IsBreakableSpace(run.text[index]);
			std::size_t end = index;
			while (end < run.text.size() && IsBreakableSpace(run.text[end]) == space) ++end;
			SToken token;
			token.text = run.text.substr(index, end - index);
			token.fontIndex = fontIndex;
			token.link = run.link;
			token.linkTarget = run.linkTarget;
			token.code = run.code;
			token.space = space;
			tokens.push_back(std::move(token));
			index = end;
		}
	}
	return tokens;
}

} // namespace

CHoverWidget::~CHoverWidget()
{
	Destroy();
}

bool CHoverWidget::EnsureWindowClass() noexcept
{
	static bool registered = false;
	if (registered) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	// CS_DROPSHADOW は VS Code のホバーの box-shadow に相当する。
	windowClass.style = CS_DROPSHADOW;
	windowClass.lpfnWndProc = &CHoverWidget::HoverWndProc;
	windowClass.hInstance = ::GetModuleHandleW(nullptr);
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = nullptr;
	windowClass.lpszClassName = kWindowClassName;
	if (::RegisterClassExW(&windowClass) == 0) {
		// 既に登録済みなら成功として扱う。二重登録は正常な再入。
		if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
	}
	registered = true;
	return true;
}

bool CHoverWidget::Create(HWND owner) noexcept
{
	if (m_hwnd != nullptr) return true;
	if (owner == nullptr) return false;
	if (!EnsureWindowClass()) return false;
	m_owner = owner;
	// WS_EX_NOACTIVATE: ホバーへポインターを移しても、エディター本体のアクティブ状態を
	// 変えない。ホスト側がアンカーとホバーの間の移動を管理するため、入力は受け取る。
	m_hwnd = ::CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		kWindowClassName,
		L"",
		WS_POPUP,
		0, 0, 0, 0,
		owner,
		nullptr,
		::GetModuleHandleW(nullptr),
		this);
	return m_hwnd != nullptr;
}

void CHoverWidget::Destroy() noexcept
{
	if (m_hwnd != nullptr) {
		const HWND hwnd = m_hwnd;
		m_hwnd = nullptr;
		::DestroyWindow(hwnd);
	}
	m_owner = nullptr;
	m_trackingMouse = false;
	m_leftButtonDown = false;
	m_trustedLinks = false;
	m_trustedCommands.clear();
	m_layout = SLayout{};
	ReleaseFonts();
}

bool CHoverWidget::IsVisible() const noexcept
{
	return m_hwnd != nullptr && ::IsWindowVisible(m_hwnd) != FALSE;
}

void CHoverWidget::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (m_hwnd != nullptr && IsVisible()) ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

void CHoverWidget::SetIconRegistry(const workbench::icons::CExtensionIconFontRegistry* registry) noexcept
{
	m_iconRegistry = registry;
}

void CHoverWidget::SetPointerCallback(PointerCallback callback)
{
	m_pointerCallback = std::move(callback);
}

void CHoverWidget::SetLinkCallback(LinkCallback callback)
{
	m_linkCallback = std::move(callback);
}

bool CHoverWidget::IsPointerInside() const noexcept
{
	if (!IsVisible()) return false;
	POINT cursor{};
	RECT bounds{};
	if (::GetCursorPos(&cursor) == FALSE || ::GetWindowRect(m_hwnd, &bounds) == FALSE) return false;
	return ::PtInRect(&bounds, cursor) != FALSE;
}

bool CHoverWidget::IsTrustedCommand(std::wstring_view target) const noexcept
{
	if (!IsCommandLinkTarget(target)) return false;
	if (m_trustedLinks) return true;
	const std::wstring_view command = CommandIdFromLinkTarget(target);
	return !command.empty() && std::any_of(
		m_trustedCommands.begin(), m_trustedCommands.end(),
		[command](const std::wstring& allowed) { return allowed == command; });
}

bool CHoverWidget::IsLinkAt(POINT point) const noexcept
{
	for (auto it = m_layout.runs.rbegin(); it != m_layout.runs.rend(); ++it) {
		if (it->link && !it->linkTarget.empty() &&
			(IsAllowedLinkTarget(it->linkTarget) || IsTrustedCommand(it->linkTarget)) &&
			::PtInRect(&it->bounds, point)) return true;
	}
	return false;
}

bool CHoverWidget::OpenLinkAt(POINT point) noexcept
{
	for (auto it = m_layout.runs.rbegin(); it != m_layout.runs.rend(); ++it) {
		if (!it->link || it->linkTarget.empty() || !::PtInRect(&it->bounds, point)) continue;
		if (!IsAllowedLinkTarget(it->linkTarget) &&
			!IsTrustedCommand(it->linkTarget)) return false;
		if (IsCommandLinkTarget(it->linkTarget)) {
			if (!m_linkCallback) return false;
			return m_linkCallback(it->linkTarget);
		}
		const HINSTANCE result = ::ShellExecuteW(
			m_owner != nullptr ? m_owner : m_hwnd,
			nullptr,
			it->linkTarget.c_str(),
			nullptr,
			nullptr,
			SW_SHOWNORMAL);
		return reinterpret_cast<INT_PTR>(result) > 32;
	}
	return false;
}

void CHoverWidget::ReleaseFonts() noexcept
{
	for (HFONT font : m_fonts) {
		if (font != nullptr) ::DeleteObject(font);
	}
	m_fonts.clear();
	for (auto& entry : m_iconFonts) {
		if (entry.font != nullptr) ::DeleteObject(entry.font);
	}
	m_iconFonts.clear();
	m_fontDpi = 0;
}

bool CHoverWidget::EnsureFonts(UINT dpi) noexcept
{
	if (m_fontDpi == dpi && m_fonts.size() == static_cast<std::size_t>(FontCount)) return true;
	ReleaseFonts();

	const wchar_t* chrome = theme::CThemeService::ResolveFontFamily(theme::ThemeFontKind::Chrome);
	const wchar_t* mono = theme::CThemeService::ResolveFontFamily(theme::ThemeFontKind::Editor);
	const int chromePoints = theme::CThemeService::FontSpec(theme::ThemeFontKind::Chrome).pointSize;
	// インラインコードは本文より 1pt 落とす。VS Code の hover もコードのほうが小さく見える。
	const int codePoints = std::max(6, chromePoints - 1);

	m_fonts.assign(static_cast<std::size_t>(FontCount), nullptr);
	m_fonts[FontRegular] = MakeFont(chrome, chromePoints, FW_NORMAL, false, dpi);
	m_fonts[FontBold] = MakeFont(chrome, chromePoints, FW_SEMIBOLD, false, dpi);
	m_fonts[FontItalic] = MakeFont(chrome, chromePoints, FW_NORMAL, true, dpi);
	m_fonts[FontBoldItalic] = MakeFont(chrome, chromePoints, FW_SEMIBOLD, true, dpi);
	m_fonts[FontCode] = MakeFont(mono, codePoints, FW_NORMAL, false, dpi);
	m_fonts[FontHeading] = MakeFont(chrome, chromePoints + 2, FW_SEMIBOLD, false, dpi);

	if (std::any_of(m_fonts.begin(), m_fonts.end(), [](HFONT font) { return font == nullptr; })) {
		ReleaseFonts();
		return false;
	}
	m_fontDpi = dpi;
	return true;
}

HFONT CHoverWidget::AcquireIconFont(const std::wstring& faceName, int height) const noexcept
{
	for (const auto& entry : m_iconFonts) {
		if (entry.height == height && entry.faceName == faceName) return entry.font;
	}
	// `workbench/icons/LabelRunPainter.h` が唯一の LOGFONTW 組み立て規則。ここで
	// 別の複製を持つと、ステータスバーと違う書体でグリフが描かれかねない。
	// なお `LF_FACESIZE` を超える書体名は、以前のように黙って切り詰めるのではなく
	// 描かないほうを選ぶ（別書体で代替されるより誤解が少ない）。CMainStatusBar
	// 側は既にこの契約なので、両者を揃える。
	const HFONT font = workbench::icons::CreateLabelRunGlyphFont(faceName, height);
	if (font == nullptr) return nullptr;
	m_iconFonts.push_back(SIconFont{ faceName, height, font });
	return font;
}

CHoverWidget::SLayout CHoverWidget::BuildLayout(HDC dc, const SDocument& document, UINT dpi) const
{
	SLayout layout;
	if (dc == nullptr || document.empty()) return layout;
	if (m_fonts.size() != static_cast<std::size_t>(FontCount)) return layout;

	const int paddingX = ScaleDip(kPaddingXDip, dpi);
	const int paddingY = ScaleDip(kPaddingYDip, dpi);
	const int lineGap = ScaleDip(kLineGapDip, dpi);
	const int blockGap = ScaleDip(kBlockGapDip, dpi);
	const int ruleMargin = ScaleDip(kRuleMarginDip, dpi);
	const int cellPaddingX = ScaleDip(kCellPaddingXDip, dpi);
	const int cellPaddingY = ScaleDip(kCellPaddingYDip, dpi);
	const int listIndent = ScaleDip(kListIndentDip, dpi);
	const int listMarkerGap = ScaleDip(kListMarkerGapDip, dpi);
	const int inlineIconGap = ScaleDip(kInlineIconGapDip, dpi);
	const int codeBlockPadding = ScaleDip(kCodeBlockPaddingDip, dpi);
	const int iconSize = ScaleDip(workbench::icons::kStatusIconDip, dpi);
	const int stroke = workbench::icons::LineStrokePixels(dpi);
	const int maxContentWidth = ScaleDip(kMaxContentWidthDip, dpi);
	const int maxTableWidth = ScaleDip(kMaxTableWidthDip, dpi);

	// 各フォントの行高。混在行はその行で使ったフォントの最大値を採る。
	std::vector<int> fontHeights(static_cast<std::size_t>(FontCount), 0);
	for (int index = 0; index < FontCount; ++index) {
		const HGDIOBJ previous = ::SelectObject(dc, m_fonts[static_cast<std::size_t>(index)]);
		TEXTMETRICW metrics{};
		if (::GetTextMetricsW(dc, &metrics) != FALSE) {
			fontHeights[static_cast<std::size_t>(index)] = metrics.tmHeight;
		}
		::SelectObject(dc, previous);
	}
	const int baseLineHeight = std::max(1, fontHeights[FontRegular]);

	const auto measure = [&](int fontIndex, const std::wstring& text) -> int {
		if (text.empty()) return 0;
		const HGDIOBJ previous = ::SelectObject(dc, m_fonts[static_cast<std::size_t>(fontIndex)]);
		SIZE size{};
		::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
		::SelectObject(dc, previous);
		return size.cx;
	};

	int y = paddingY;
	int usedRight = paddingX;
	// 直前のブロックが末尾に足した行間。下パディングを対称にするため最後に引き戻す。
	int trailingGap = 0;
	// 幅がレイアウト後にしか決まらない塗り（区切り線・コードブロック背景）の添字。
	std::vector<std::size_t> fullWidthFills;

	//! 1 論理行を折り返しながら配置する。戻り値は消費した高さ。
	const auto emitWrapped = [&](const SInlineText& runs, int left, int right, int forcedFont) {
		const std::vector<SToken> tokens = Tokenize(runs, forcedFont);
		int x = left;
		int lineHeight = baseLineHeight;
		std::vector<std::pair<SPositionedRun, bool>> pending;
		const auto flushLine = [&]() {
			for (auto& [run, codeBackground] : pending) {
				run.bounds.top = y;
				run.bounds.bottom = y + lineHeight;
				if (codeBackground) {
					layout.fills.push_back(SFilledRect{ run.bounds, m_palette.raised.ToColorRef() });
				}
				if (layout.runs.size() < kMaxPositionedRuns) layout.runs.push_back(std::move(run));
			}
			pending.clear();
			usedRight = std::max(usedRight, x);
			y += lineHeight + lineGap;
			trailingGap = lineGap;
			x = left;
			lineHeight = baseLineHeight;
		};
		for (const auto& token : tokens) {
			const bool isIcon = !token.iconId.empty();
			const int width = isIcon ? iconSize + inlineIconGap : measure(token.fontIndex, token.text);
			if (!token.space && x > left && x + width > right) flushLine();
			if (token.space && x == left) continue; // 行頭に残った空白は落とす
			SPositionedRun run;
			run.bounds.left = x;
			run.bounds.right = x + width;
			run.text = token.text;
			run.iconId = token.iconId;
			run.fontIndex = token.fontIndex;
			run.link = token.link;
			run.linkTarget = token.linkTarget;
			if (isIcon) {
				lineHeight = std::max(lineHeight, iconSize);
			}
			else {
				lineHeight = std::max(lineHeight, fontHeights[static_cast<std::size_t>(token.fontIndex)]);
			}
			pending.emplace_back(std::move(run), token.code && !isIcon);
			x += width;
		}
		flushLine();
	};

	//! 折り返さずに 1 行だけ配置する（テーブルのセル）。戻り値は自然幅。
	const auto emitSingleLine = [&](const SInlineText& runs, int left, int top, int height, int clipRight, int forcedFont) {
		int x = left;
		for (const auto& run : runs) {
			if (x >= clipRight) break;
			const bool isIcon = !run.iconId.empty();
			const int fontIndex = FontIndexFor(run, forcedFont);
			const int width = isIcon ? iconSize + inlineIconGap : measure(fontIndex, run.text);
			SPositionedRun positioned;
			positioned.bounds.left = x;
			positioned.bounds.right = std::min(clipRight, x + width);
			positioned.bounds.top = top;
			positioned.bounds.bottom = top + height;
			positioned.text = run.text;
			positioned.iconId = run.iconId;
			positioned.fontIndex = fontIndex;
			positioned.link = run.link;
			positioned.linkTarget = run.linkTarget;
			if (run.code && !isIcon) {
				layout.fills.push_back(SFilledRect{ positioned.bounds, m_palette.raised.ToColorRef() });
			}
			if (layout.runs.size() < kMaxPositionedRuns) layout.runs.push_back(std::move(positioned));
			x += width;
		}
	};

	const auto naturalWidth = [&](const SInlineText& runs, int forcedFont) {
		int width = 0;
		for (const auto& run : runs) {
			width += run.iconId.empty() ? measure(FontIndexFor(run, forcedFont), run.text) : iconSize + inlineIconGap;
		}
		return width;
	};

	for (std::size_t blockIndex = 0; blockIndex < document.blocks.size(); ++blockIndex) {
		const SBlock& block = document.blocks[blockIndex];
		if (blockIndex != 0) y += blockGap;
		const int contentRight = paddingX + maxContentWidth;

		switch (block.kind) {
		case EBlockKind::HorizontalRule: {
			y += ruleMargin;
			SFilledRect rule{ RECT{ paddingX, y, paddingX, y + stroke }, m_palette.border.ToColorRef() };
			fullWidthFills.push_back(layout.fills.size());
			layout.fills.push_back(rule);
			y += stroke + ruleMargin;
			trailingGap = 0;
			break;
		}
		case EBlockKind::Heading: {
			const int font = block.level <= 2 ? FontHeading : FontBold;
			for (const auto& line : block.lines) emitWrapped(line, paddingX, contentRight, font);
			break;
		}
		case EBlockKind::CodeBlock: {
			const int blockTop = y;
			y += codeBlockPadding;
			const std::size_t backgroundIndex = layout.fills.size();
			layout.fills.push_back(SFilledRect{ RECT{ paddingX, blockTop, paddingX, blockTop }, m_palette.raised.ToColorRef() });
			fullWidthFills.push_back(backgroundIndex);
			for (const auto& line : block.lines) {
				emitWrapped(line, paddingX + codeBlockPadding, contentRight, FontCode);
			}
			y += codeBlockPadding;
			layout.fills[backgroundIndex].bounds.bottom = y;
			trailingGap = 0;
			break;
		}
		case EBlockKind::ListItem: {
			const int indent = paddingX + listIndent * std::max(0, block.level);
			const int markerWidth = measure(FontRegular, block.marker);
			const int textLeft = indent + markerWidth + listMarkerGap;
			if (!block.marker.empty()) {
				SPositionedRun marker;
				marker.bounds = RECT{ indent, y, indent + markerWidth, y + baseLineHeight };
				marker.text = block.marker;
				marker.fontIndex = FontRegular;
				if (layout.runs.size() < kMaxPositionedRuns) layout.runs.push_back(std::move(marker));
			}
			for (const auto& line : block.lines) emitWrapped(line, textLeft, contentRight, -1);
			break;
		}
		case EBlockKind::Table: {
			const std::size_t columnCount = block.alignments.size();
			if (columnCount == 0 || block.rows.empty()) break;
			std::vector<int> columnWidths(columnCount, 0);
			for (const auto& row : block.rows) {
				const int forcedFont = row.header ? FontBold : -1;
				for (std::size_t column = 0; column < row.cells.size() && column < columnCount; ++column) {
					const int width = naturalWidth(row.cells[column], forcedFont) + cellPaddingX * 2;
					columnWidths[column] = std::max(columnWidths[column], width);
				}
			}
			int tableWidth = 0;
			for (int width : columnWidths) tableWidth += width;
			if (tableWidth > maxTableWidth && tableWidth > 0) {
				// はみ出す分は列幅に比例して削る。セルはクリップされ、切れたことは見て分かる。
				for (int& width : columnWidths) {
					width = std::max(cellPaddingX * 2, ::MulDiv(width, maxTableWidth, tableWidth));
				}
			}
			const int rowHeight = baseLineHeight + cellPaddingY * 2;
			for (const auto& row : block.rows) {
				const int forcedFont = row.header ? FontBold : -1;
				int x = paddingX;
				for (std::size_t column = 0; column < columnCount; ++column) {
					const int cellWidth = columnWidths[column];
					if (column < row.cells.size()) {
						const int inner = std::max(0, cellWidth - cellPaddingX * 2);
						const int content = std::min(inner, naturalWidth(row.cells[column], forcedFont));
						int offset = 0;
						switch (block.alignments[column]) {
						case EColumnAlign::Center: offset = (inner - content) / 2; break;
						case EColumnAlign::Right: offset = inner - content; break;
						case EColumnAlign::Left:
						default: break;
						}
						emitSingleLine(
							row.cells[column],
							x + cellPaddingX + offset,
							y + cellPaddingY,
							baseLineHeight,
							x + cellWidth - cellPaddingX,
							forcedFont);
					}
					x += cellWidth;
				}
				usedRight = std::max(usedRight, x);
				y += rowHeight;
			}
			trailingGap = 0;
			break;
		}
		case EBlockKind::Paragraph:
		default:
			for (const auto& line : block.lines) emitWrapped(line, paddingX, contentRight, -1);
			break;
		}
	}

	// 最後の論理行のあとに付いた行間ぶんを戻してから下パディングを足す。
	layout.width = usedRight + paddingX;
	layout.height = y + paddingY - trailingGap;
	layout.height = std::max(layout.height, paddingY * 2 + baseLineHeight);
	for (std::size_t index : fullWidthFills) {
		layout.fills[index].bounds.right = layout.width - paddingX;
	}
	return layout;
}

void CHoverWidget::Show(
	const SDocument& document, const RECT& anchorScreen, bool trustedLinks,
	std::vector<std::wstring> trustedCommands)
{
	if (m_hwnd == nullptr || document.empty()) {
		Hide();
		return;
	}
	const UINT dpi = m_owner != nullptr ? ::GetDpiForWindow(m_owner) : 96U;
	if (!EnsureFonts(dpi == 0 ? 96U : dpi)) {
		Hide();
		return;
	}
	const HDC screen = ::GetDC(m_hwnd);
	if (screen == nullptr) {
		Hide();
		return;
	}
	m_layout = BuildLayout(screen, document, m_fontDpi);
	m_trustedLinks = trustedLinks;
	m_trustedCommands = std::move(trustedCommands);
	::ReleaseDC(m_hwnd, screen);
	if (m_layout.width <= 0 || m_layout.height <= 0 || m_layout.runs.empty()) {
		Hide();
		return;
	}
	PositionWindow(anchorScreen);
	::InvalidateRect(m_hwnd, nullptr, TRUE);
	::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
	::UpdateWindow(m_hwnd);
}

void CHoverWidget::PositionWindow(const RECT& anchorScreen) noexcept
{
	RECT work{ 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
	if (const HMONITOR monitor = ::MonitorFromRect(&anchorScreen, MONITOR_DEFAULTTONEAREST); monitor != nullptr) {
		MONITORINFO info{};
		info.cbSize = sizeof(info);
		if (::GetMonitorInfoW(monitor, &info) != FALSE) work = info.rcWork;
	}
	const int gap = ScaleDip(kAnchorGapDip, m_fontDpi);
	const int width = std::min<int>(m_layout.width, std::max<int>(1, work.right - work.left));
	const int height = m_layout.height;

	int left = anchorScreen.left;
	if (left + width > work.right) left = work.right - width;
	if (left < work.left) left = work.left;

	// VS Code のステータスバーホバーと同じく、既定はアンカーの上。入らなければ下へ。
	int top = anchorScreen.top - gap - height;
	if (top < work.top) top = std::min<int>(anchorScreen.bottom + gap, std::max<int>(work.top, work.bottom - height));

	::SetWindowPos(m_hwnd, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
}

void CHoverWidget::Hide() noexcept
{
	if (m_hwnd == nullptr) return;
	if (::GetCapture() == m_hwnd) ::ReleaseCapture();
	m_trackingMouse = false;
	m_leftButtonDown = false;
	m_trustedLinks = false;
	m_trustedCommands.clear();
	::ShowWindow(m_hwnd, SW_HIDE);
}

void CHoverWidget::OnPaint(HDC dc) const noexcept
{
	if (dc == nullptr) return;
	RECT client{};
	if (::GetClientRect(m_hwnd, &client) == FALSE) return;

	if (const HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef()); background != nullptr) {
		::FillRect(dc, &client, background);
		::DeleteObject(background);
	}
	if (const HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef()); border != nullptr) {
		::FrameRect(dc, &client, border);
		::DeleteObject(border);
	}

	for (const auto& fill : m_layout.fills) {
		if (fill.bounds.right <= fill.bounds.left || fill.bounds.bottom <= fill.bounds.top) continue;
		if (const HBRUSH brush = ::CreateSolidBrush(fill.color); brush != nullptr) {
			RECT bounds = fill.bounds;
			::FillRect(dc, &bounds, brush);
			::DeleteObject(brush);
		}
	}

	const int previousMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF textColor = m_palette.primaryText.ToColorRef();
	const COLORREF linkColor = m_palette.accent.ToColorRef();

	for (const auto& run : m_layout.runs) {
		if (!run.iconId.empty()) {
			const IconRect box = workbench::icons::CenteredIconBounds(
				IconRect{ run.bounds.left, run.bounds.top, run.bounds.right, run.bounds.bottom },
				workbench::icons::kStatusIconDip,
				m_fontDpi);
			// 組み込み `$(name)` は同梱 codicon.ttf の 1 グリフとして描く。ステータスバー
			// 本文と同じ解決順・同じ書体でなければ、同じ id が場所によって別物になる。
			const auto resolved = workbench::icons::ResolveThemeIcon(
				run.iconId, m_iconRegistry, workbench::icons::CCodiconFont::Instance().FaceName());
			if (resolved.font) {
				const HFONT iconFont = AcquireIconFont(resolved.fontIcon.faceName, box.Height());
				if (iconFont != nullptr) {
					const HGDIOBJ previous = ::SelectObject(dc, iconFont);
					::SetTextColor(dc, textColor);
					RECT bounds{ box.left, box.top, box.right, box.bottom };
					::DrawTextW(
						dc,
						resolved.fontIcon.glyph.c_str(),
						static_cast<int>(resolved.fontIcon.glyph.size()),
						&bounds,
						DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
					::SelectObject(dc, previous);
				}
			}
			else {
				workbench::icons::codicons::Draw(dc, box, resolved.builtin, textColor);
			}
			continue;
		}
		if (run.text.empty()) continue;
		const HGDIOBJ previous = ::SelectObject(dc, m_fonts[static_cast<std::size_t>(run.fontIndex)]);
		::SetTextColor(dc, run.link ? linkColor : textColor);
		RECT bounds = run.bounds;
		::DrawTextW(
			dc,
			run.text.c_str(),
			static_cast<int>(run.text.size()),
			&bounds,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		::SelectObject(dc, previous);
	}
	::SetBkMode(dc, previousMode);
}

LRESULT CALLBACK CHoverWidget::HoverWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}
	auto* self = reinterpret_cast<CHoverWidget*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEMOVE:
		if (self != nullptr) {
			if (!self->m_trackingMouse) {
				TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 };
				self->m_trackingMouse = ::TrackMouseEvent(&tracking) != FALSE;
				if (self->m_pointerCallback) self->m_pointerCallback(true);
			}
			::SetCursor(::LoadCursorW(nullptr, self->IsLinkAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })
				? IDC_HAND : IDC_ARROW));
		}
		return 0;
	case WM_MOUSELEAVE:
		if (self != nullptr) {
			self->m_trackingMouse = false;
			if (self->m_pointerCallback) self->m_pointerCallback(false);
		}
		return 0;
	case WM_LBUTTONDOWN:
		if (self != nullptr && self->IsLinkAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
			self->m_leftButtonDown = true;
			::SetCapture(hwnd);
		}
		return 0;
	case WM_SETCURSOR:
		if (self != nullptr && LOWORD(lParam) == HTCLIENT) {
			POINT point{};
			if (::GetCursorPos(&point) != FALSE && ::ScreenToClient(hwnd, &point) != FALSE) {
				::SetCursor(::LoadCursorW(nullptr, self->IsLinkAt(point) ? IDC_HAND : IDC_ARROW));
				return TRUE;
			}
		}
		break;
	case WM_LBUTTONUP:
		if (self != nullptr) {
			const bool pressed = self->m_leftButtonDown;
			self->m_leftButtonDown = false;
			if (::GetCapture() == hwnd) ::ReleaseCapture();
			if (pressed && self->OpenLinkAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
				self->Hide();
			}
		}
		return 0;
	case WM_CAPTURECHANGED:
	case WM_CANCELMODE:
		if (self != nullptr) self->m_leftButtonDown = false;
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_NCHITTEST:
		return HTCLIENT;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(hwnd, &paint);
		if (dc != nullptr) {
			if (self != nullptr) self->OnPaint(dc);
			::EndPaint(hwnd, &paint);
		}
		return 0;
	}
	case WM_NCDESTROY:
		if (self != nullptr) {
			self->m_trackingMouse = false;
			self->m_leftButtonDown = false;
		}
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		if (self != nullptr && self->m_hwnd == hwnd) self->m_hwnd = nullptr;
		break;
	default:
		break;
	}
	return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace workbench::hover
