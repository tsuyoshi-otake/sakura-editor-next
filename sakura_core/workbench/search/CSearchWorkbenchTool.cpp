/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/search/CSearchWorkbenchTool.h"

#include "theme/CThemeService.h"
#include "workbench/controls/CInputBoxGeometry.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/rendering/LatestOnlyMailbox.h"
#include "workbench/WorkerRetirementService.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CSetiFont.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/SetiFileIcon.h"
#include "workbench/icons/SetiIconPainter.h"
#include "workbench/icons/ThemeIconResolver.h"
#include "workbench/search/WorkspaceSearchEngine.h"

#include <CommCtrl.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace workbench::search {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraNativeSearchTool";
//! One completed search snapshot, handed to the window thread.
constexpr UINT kResultMessage = WM_APP + 0x5e1;
//! `search.searchOnType` is on by default upstream, with `searchOnTypeDebouncePeriod`
//! at 300ms. The timer is that debounce.
constexpr UINT_PTR kTypeTimer = 0x5e2;
constexpr UINT kTypeDebounceMilliseconds = 300;

constexpr int kQueryControlId = 1;
constexpr int kReplaceControlId = 2;
constexpr int kListControlId = 3;

//! `.search-widgets-container` from VS Code 1.134.0.
constexpr int kWidgetMarginLeftDip = 2;
constexpr int kWidgetMarginRightDip = 12;
constexpr int kWidgetPaddingDip = 6;
//! `.search-container`/`.replace-container` and `.replace-container`'s margin.
constexpr int kInputContainerMarginLeftDip = 18;
constexpr int kReplaceMarginTopDip = 6;
//! `.monaco-inputbox` height at the default font size.
constexpr int kInputHeightDip = 26;
//! The chevron column left of both boxes, which is upstream's replace toggle.
constexpr int kToggleReplaceWidthDip = 16;
//! One inline toggle button inside a box, and the box's own text padding.
constexpr int kInlineButtonDip = 20;
constexpr int kInputPaddingLeftDip = 6;
//! The box's CSS vertical padding.  It is only the fallback line height here:
//! a single-line Win32 EDIT top-anchors its text inside a taller client, so the
//! EDIT is sized to one measured text line and centered instead of being
//! stretched and inset.  `workbench::controls::CenterSingleLineEditor` owns that
//! rule for every native input box, matching upstream's single `InputBox`.
constexpr int kInputPaddingVerticalDip = 3;
//! The native EDIT stays one pixel inside the painted input border horizontally.
constexpr int kInputBorderDip = 1;
//! The inline controls sit two CSS pixels inside the input's right edge.
constexpr int kInputControlsRightDip = 2;
//! Keep the native edit text out from under the inline controls.
constexpr int kInputControlsGapDip = 2;
//! The Replace All button that sits right of the replace box.
constexpr int kReplaceAllButtonDip = 24;
//! The action bar has a four-pixel margin before its 24-pixel action.
constexpr int kReplaceActionGapDip = 4;
//! The message line under the widget ("N results in M files").
constexpr int kMessageHeightDip = 22;
//! Legacy result-message inset; it is outside the Search widget CSS box.
constexpr int kMessageInsetDip = 8;
//! Result rows retain their existing action spacing.
constexpr int kRowActionGapDip = 4;
//! One result row, upstream's `SearchDelegate.getHeight`.
constexpr int kRowHeightDip = 22;
constexpr int kRowIndentDip = 8;
constexpr int kMatchIndentDip = 22;
constexpr int kIconDip = 16;
constexpr int kIconGapDip = 6;
//! One hover action button on a row.
constexpr int kRowActionDip = 22;
//! The count badge on a file row.
constexpr int kBadgeMinimumWidthDip = 18;
constexpr int kBadgeHeightDip = 16;

[[nodiscard]] int Scale(int dip, unsigned int dpi) noexcept
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? 96u : dpi), 96);
}

//! Returns one of the inline controls measured from the input's right edge.
[[nodiscard]] RECT CalculateInlineToggleRect(const RECT& box, int indexFromRight,
	unsigned int dpi) noexcept
{
	const int size = Scale(kInlineButtonDip, dpi);
	const int rightInset = Scale(kInputControlsRightDip, dpi);
	RECT toggle{};
	toggle.right = box.right - rightInset - indexFromRight * size;
	toggle.left = toggle.right - size;
	toggle.top = box.top + ((box.bottom - box.top) - size) / 2;
	toggle.bottom = toggle.top + size;
	return toggle;
}

SearchWidgetGeometry CalculateSearchWidgetGeometryImpl(const RECT& clientRect,
	unsigned int dpi, bool replaceVisible, int inputLineHeight) noexcept
{
	const auto scaled = [dpi](int dip) noexcept { return Scale(dip, dpi); };
	const int widgetLeft = clientRect.left + scaled(kWidgetMarginLeftDip);
	const int widgetRight = std::max(widgetLeft,
		static_cast<int>(clientRect.right) - scaled(kWidgetMarginRightDip));
	const int inputHeight = scaled(kInputHeightDip);
	const int containerTop = clientRect.top;
	const int queryTop = containerTop + scaled(kWidgetPaddingDip);
	const int queryLeft = widgetLeft + scaled(kInputContainerMarginLeftDip);
	const int queryBottom = queryTop + inputHeight;
	const int replaceTop = queryBottom + scaled(kReplaceMarginTopDip);
	const int replaceBottom = replaceTop + inputHeight;
	const int contentBottom = replaceVisible ? replaceBottom : queryBottom;

	SearchWidgetGeometry geometry{};
	geometry.container = RECT{ widgetLeft, containerTop, widgetRight,
		contentBottom + scaled(kWidgetPaddingDip) };
	geometry.queryBox = RECT{ queryLeft, queryTop,
		std::max(queryLeft, widgetRight), queryBottom };
	geometry.replaceBox = geometry.queryBox;
	geometry.replaceBox.top = replaceTop;
	geometry.replaceBox.bottom = replaceBottom;
	geometry.replaceBox.right = std::max(queryLeft, widgetRight - scaled(kReplaceAllButtonDip)
		- scaled(kReplaceActionGapDip));
	geometry.replaceAll.right = widgetRight;
	geometry.replaceAll.left = std::max(widgetLeft,
		static_cast<int>(geometry.replaceAll.right) - scaled(kReplaceAllButtonDip));
	geometry.replaceAll.top = geometry.replaceBox.top
		+ (inputHeight - scaled(kReplaceAllButtonDip)) / 2;
	geometry.replaceAll.bottom = geometry.replaceAll.top + scaled(kReplaceAllButtonDip);
	geometry.toggleReplace = RECT{ widgetLeft, queryTop,
		std::min(widgetRight, widgetLeft + scaled(kToggleReplaceWidthDip)),
		replaceVisible ? geometry.replaceBox.bottom : geometry.queryBox.bottom };

	const RECT queryCaseToggle = CalculateInlineToggleRect(geometry.queryBox, 3, dpi);
	const RECT replacePreserveToggle = CalculateInlineToggleRect(geometry.replaceBox, 1, dpi);
	const int fallbackLineHeight = inputHeight - 2 * scaled(kInputPaddingVerticalDip);
	geometry.queryEdit = controls::CenterSingleLineEditor(geometry.queryBox,
		inputLineHeight, scaled(kInputBorderDip), fallbackLineHeight);
	geometry.queryEdit.right = std::max(geometry.queryEdit.left,
		static_cast<LONG>(queryCaseToggle.left - scaled(kInputControlsGapDip)));
	geometry.replaceEdit = controls::CenterSingleLineEditor(geometry.replaceBox,
		inputLineHeight, scaled(kInputBorderDip), fallbackLineHeight);
	geometry.replaceEdit.right = std::max(geometry.replaceEdit.left,
		static_cast<LONG>(replacePreserveToggle.left - scaled(kInputControlsGapDip)));
	return geometry;
}

void DrawSearchIcon(HDC dc, std::wstring_view name, const RECT& rect, COLORREF color)
{
	if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) return;
	const auto icon = icons::ResolveThemeIcon(name, icons::CCodiconFont::Instance().FaceName());
	if (icon.font && !icon.fontIcon.glyph.empty()) {
		const HFONT glyphFont = icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName,
			std::max(1, static_cast<int>(rect.bottom - rect.top)));
		if (glyphFont != nullptr) {
			const HGDIOBJ previous = ::SelectObject(dc, glyphFont);
			const int previousMode = ::SetBkMode(dc, TRANSPARENT);
			const COLORREF previousColor = ::SetTextColor(dc, color);
			RECT glyph = rect;
			::DrawTextW(dc, icon.fontIcon.glyph.c_str(), static_cast<int>(icon.fontIcon.glyph.size()),
				&glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
			::SetTextColor(dc, previousColor);
			::SetBkMode(dc, previousMode);
			::SelectObject(dc, previous);
			::DeleteObject(glyphFont);
		}
		return;
	}
	icons::codicons::Draw(dc, icons::IconRect{ rect.left, rect.top, rect.right, rect.bottom },
		icon.builtin, color);
}

//! The same file icon the Explorer draws, for the same reason the SCM view draws it.
void DrawResultFileIcon(HDC dc, std::wstring_view fileName, const RECT& rect, COLORREF fallback)
{
	if (icons::CSetiFont::Instance().IsAvailable()) {
		const auto icon = icons::seti::ResolveSetiFileIcon(fileName, false,
			theme::CThemeService::IsActiveColorThemeLightKind()
				? icons::seti::EIconVariant::Light
				: icons::seti::EIconVariant::Dark);
		if (!icon) return;
		icons::seti::DrawSetiIcon(dc, rect, icon->character,
			icon->color == icons::seti::kInheritColor
				? fallback
				: icons::seti::ColorRefFromThemeRgb(icon->color));
		return;
	}
	DrawSearchIcon(dc, L"file", rect, fallback);
}

void FillRectangle(HDC dc, const RECT& rect, COLORREF color)
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	::FillRect(dc, &rect, brush);
	::DeleteObject(brush);
}

void FrameRectangle(HDC dc, const RECT& rect, COLORREF color)
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	::FrameRect(dc, &rect, brush);
	::DeleteObject(brush);
}

//! Queue a repaint without asking USER to erase the class background first.
//! Every Search surface paints its own background, so an erase pass only exposes
//! a blank intermediate frame while child windows are being repositioned.
void QueueNoEraseInvalidate(HWND window, const RECT* region = nullptr) noexcept
{
	if (window != nullptr) ::InvalidateRect(window, region, FALSE);
}

//! Replaces `{0}` and `{1}` in a localized message.
[[nodiscard]] std::wstring FormatMessage(std::wstring text, std::wstring_view first,
	std::wstring_view second)
{
	const auto replace = [&text](std::wstring_view token, std::wstring_view value) {
		const std::size_t position = text.find(token);
		if (position != std::wstring::npos) text.replace(position, token.size(), value);
	};
	replace(L"{0}", first);
	replace(L"{1}", second);
	return text;
}

//! One rendered row. Upstream's Search tree has exactly these two levels.
struct Row final {
	bool isFile = false;
	std::size_t fileIndex = 0;
	std::size_t matchIndex = 0;
};

//! Which hover action of a row a point belongs to.
enum class ERowAction : std::uint8_t { None, Replace, Dismiss };

//! One inline toggle of the search or replace box.
enum class EToggle : std::uint8_t { None, MatchCase, WholeWord, UseRegex, PreserveCase, ReplaceAll, ToggleReplace };

//! The worker's request. A generation makes a superseded search abandon itself.
struct WorkerRequest final {
	std::wstring root;
	SearchQuery query;
	std::uint64_t generation = 0;
};

//! One completed search held by the depth-one result mailbox.
struct WorkerResult final {
	explicit WorkerResult(WorkerRequest value) : request(std::move(value)) {}
	const WorkerRequest request;
	SearchResults results;
};

//! Everything shared between the window thread and the search worker.
struct SharedState final {
	std::mutex mutex;
	WorkerRequest pending;
	bool hasPending = false;
	std::atomic<std::uint64_t> generation{ 0 };
	std::atomic<bool> stopping{ false };
	HANDLE wake{};
	HANDLE stop{};
	//! The window is cleared before the tool closes, so a late post is dropped
	//! rather than delivered to a destroyed HWND.
	std::mutex windowMutex;
	HWND window{};
	workbench::rendering::LatestOnlyMailbox<std::unique_ptr<WorkerResult>> results;

	~SharedState() noexcept
	{
		if (stop != nullptr) {
			::CloseHandle(stop);
			stop = nullptr;
		}
		if (wake != nullptr) {
			::CloseHandle(wake);
			wake = nullptr;
		}
	}
};

bool SameSearchPattern(const SearchQuery& left, const SearchQuery& right) noexcept
{
	return left.text == right.text && left.matchCase == right.matchCase
		&& left.wholeWord == right.wholeWord && left.useRegex == right.useRegex;
}

bool EnsureClass(HINSTANCE instance);

} // namespace

SearchWidgetGeometry CalculateSearchWidgetGeometry(const RECT& clientRect,
	unsigned int dpi, bool replaceVisible, int inputLineHeight) noexcept
{
	return CalculateSearchWidgetGeometryImpl(clientRect, dpi, replaceVisible, inputLineHeight);
}

struct CSearchWorkbenchTool::Impl {
	HWND window{};
	HWND query{};
	HWND replace{};
	HWND list{};
	controls::COverlayScrollbar scrollbar;
	//! Persistent composition targets. They are UI-thread-owned and only publish
	//! completed pixels to the native target during WM_PAINT/WM_DRAWITEM.
	workbench::rendering::CGdiBackBuffer rootBuffer;
	workbench::rendering::CGdiBackBuffer listBuffer;
	workbench::rendering::FrameNativeSurfacePayloadAdapter nativeSurface;
	unsigned int dpi{ 96 };
	theme::CThemeFont font;
	//! Cached `TEXTMETRICW::tmHeight` for `font`; zero means "not measured yet".
	mutable int inputLineHeight{};
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	SearchViewTexts texts;
	std::wstring root;
	SearchQuery model;
	//! True once the replace box is revealed, which is upstream's `toggleReplace`.
	bool replaceVisible{};
	bool active{};
	bool closed{};
	bool searching{};
	SearchResults results;
private:
	std::optional<WorkerRequest> acceptedRequest;
public:
	std::unordered_set<std::wstring> collapsedFiles;
	std::vector<Row> rows;
	int hoverRow{ -1 };
	ERowAction hoverAction{ ERowAction::None };
	EToggle hoverToggle{ EToggle::None };
	bool trackingMouse{};
	//! Set while the tool itself writes a box, so the resulting `EN_CHANGE` is not
	//! mistaken for the user typing.
	bool updatingText{};
	std::wstring statusText;
	MatchActivationCallback activateMatch;
	FilesChangedCallback filesChanged;
	std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
	std::thread worker;
	std::optional<workbench::WorkerRetirementService::Reservation> workerRetirement;

	// --- geometry -------------------------------------------------------------

	[[nodiscard]] int Dip(int value) const noexcept { return Scale(value, dpi); }

	[[nodiscard]] RECT ClientRect() const noexcept
	{
		RECT client{};
		if (window != nullptr) ::GetClientRect(window, &client);
		return client;
	}

	//! Measured lazily because painting also needs the geometry, and cheap after
	//! the first call: the result only changes when the font is recreated, which
	//! `Layout` signals by clearing the cache.
	[[nodiscard]] int InputLineHeight() const noexcept
	{
		if (inputLineHeight <= 0) {
			inputLineHeight = controls::MeasureTextLineHeight(window, font.Get());
		}
		return inputLineHeight;
	}

	[[nodiscard]] SearchWidgetGeometry Geometry() const noexcept
	{
		return CalculateSearchWidgetGeometry(ClientRect(), dpi, replaceVisible, InputLineHeight());
	}

	[[nodiscard]] RECT QueryBoxRect() const noexcept
	{
		return Geometry().queryBox;
	}

	[[nodiscard]] RECT ReplaceBoxRect() const noexcept
	{
		return Geometry().replaceBox;
	}

	[[nodiscard]] RECT ReplaceAllRect() const noexcept
	{
		return Geometry().replaceAll;
	}

	[[nodiscard]] RECT ToggleReplaceRect() const noexcept
	{
		return Geometry().toggleReplace;
	}

	//! The nth inline toggle inside `box`, counted from its right edge.
	[[nodiscard]] RECT InlineToggleRect(const RECT& box, int indexFromRight) const noexcept
	{
		return CalculateInlineToggleRect(box, indexFromRight, dpi);
	}

	[[nodiscard]] RECT MessageRect() const noexcept
	{
		const RECT client = ClientRect();
		const SearchWidgetGeometry geometry = Geometry();
		RECT message{};
		message.left = client.left + Dip(kMessageInsetDip);
		message.right = client.right - Dip(kMessageInsetDip);
		// The widget's bottom padding is part of its container, so the result
		// message starts after that padding rather than inside the replace row.
		message.top = geometry.container.bottom;
		message.bottom = message.top + Dip(kMessageHeightDip);
		return message;
	}

	[[nodiscard]] RECT ListRect() const noexcept
	{
		const RECT client = ClientRect();
		RECT bounds{};
		bounds.left = client.left;
		bounds.right = client.right;
		bounds.top = MessageRect().bottom;
		bounds.bottom = std::max<LONG>(bounds.top, client.bottom);
		return bounds;
	}

	// --- model ----------------------------------------------------------------

	[[nodiscard]] std::wstring TextOf(HWND edit) const
	{
		if (edit == nullptr) return {};
		const int length = ::GetWindowTextLengthW(edit);
		if (length <= 0) return {};
		std::wstring text(static_cast<std::size_t>(length), L'\0');
		::GetWindowTextW(edit, text.data(), length + 1);
		return text;
	}

	void ReadBoxes()
	{
		model.text = TextOf(query);
		model.replaceText = TextOf(replace);
	}

	void RebuildRows()
	{
		rows.clear();
		for (std::size_t fileIndex = 0; fileIndex < results.files.size(); ++fileIndex) {
			const auto& file = results.files[fileIndex];
			rows.push_back(Row{ true, fileIndex, 0 });
			if (collapsedFiles.contains(file.fullPath)) continue;
			for (std::size_t matchIndex = 0; matchIndex < file.matches.size(); ++matchIndex) {
				rows.push_back(Row{ false, fileIndex, matchIndex });
			}
		}
		if (list == nullptr) return;
		const int previousTop = static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0));
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_RESETCONTENT, 0, 0);
		for (std::size_t index = 0; index < rows.size(); ++index) {
			::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
		}
		if (previousTop > 0 && previousTop < static_cast<int>(rows.size())) {
			::SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(previousTop), 0);
		}
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		QueueNoEraseInvalidate(list);
		scrollbar.Update();
	}

	void UpdateStatusText()
	{
		if (searching) { statusText = texts.searching; return; }
		switch (results.completion) {
		case ESearchCompletion::NoWorkspace: statusText = texts.noWorkspace; return;
		case ESearchCompletion::RegexUnavailable: statusText = texts.regexUnavailable; return;
		case ESearchCompletion::InvalidPattern:
			statusText = results.failureText.empty() ? texts.invalidPattern : results.failureText;
			return;
		default: break;
		}
		if (model.text.empty()) { statusText.clear(); return; }
		if (results.files.empty()) { statusText = texts.noResults; return; }
		statusText = FormatMessage(texts.resultSummary, std::to_wstring(results.matchCount),
			std::to_wstring(results.files.size()));
		if (results.completion == ESearchCompletion::LimitHit) {
			statusText += L" - ";
			statusText += texts.limitHit;
		}
	}

	void Repaint()
	{
		QueueNoEraseInvalidate(window);
	}

	void InvalidateSearch()
	{
		shared->generation.fetch_add(1, std::memory_order_acq_rel);
		{
			std::lock_guard<std::mutex> guard(shared->mutex);
			shared->pending = {};
			shared->hasPending = false;
		}
		DropQueuedResults();
		acceptedRequest.reset();
		results = {};
		searching = false;
		RebuildRows();
		UpdateStatusText();
		Repaint();
	}

	void AcceptResult(std::unique_ptr<WorkerResult> result)
	{
		if (!result || closed || result->request.generation != shared->generation.load(std::memory_order_acquire)
			|| result->request.root != root || !SameSearchPattern(result->request.query, model)) return;
		acceptedRequest = result->request;
		results = std::move(result->results);
		collapsedFiles.clear();
		searching = false;
		RebuildRows();
		UpdateStatusText();
		Repaint();
	}

	void StartSearch()
	{
		if (closed) return;
		ReadBoxes();
		InvalidateSearch();
		if (shared->wake == nullptr) return;
		if (model.text.empty()) return;
		const std::uint64_t generation = shared->generation.load(std::memory_order_acquire);
		{
			std::lock_guard<std::mutex> guard(shared->mutex);
			shared->pending = WorkerRequest{ root, model, generation };
			shared->hasPending = true;
		}
		searching = true;
		UpdateStatusText();
		Repaint();
		::SetEvent(shared->wake);
	}

	void ScheduleSearch()
	{
		if (window == nullptr || closed) return;
		ReadBoxes();
		InvalidateSearch();
		if (model.text.empty()) { ::KillTimer(window, kTypeTimer); return; }
		::SetTimer(window, kTypeTimer, kTypeDebounceMilliseconds, nullptr);
	}

	// --- painting -------------------------------------------------------------

	void PaintBox(HDC dc, const RECT& box, const RECT& editor, HWND edit,
		std::wstring_view placeholder) const
	{
		// `searchWidget.ts` builds these with `InputBox`, whose `defaultInputBoxStyles`
		// are `input.background` / `input.border`; the focused frame is `focusBorder`,
		// which `accent` resolves from.
		FillRectangle(dc, box, palette.inputBackground.ToColorRef());
		const bool focused = edit != nullptr && ::GetFocus() == edit;
		FrameRectangle(dc, box, focused ? palette.accent.ToColorRef() : palette.inputBorder.ToColorRef());
		if (edit == nullptr || ::GetWindowTextLengthW(edit) > 0 || placeholder.empty()) return;
		// Upstream's placeholder is the same DOM node as the value, so it cannot
		// sit anywhere else.  Drawing it in the EDIT's own rectangle keeps the
		// text from jumping when the first character is typed.
		RECT text = editor;
		text.left += Dip(kInputPaddingLeftDip);
		::SetTextColor(dc, palette.disabledText.ToColorRef());
		::DrawTextW(dc, placeholder.data(), static_cast<int>(placeholder.size()), &text,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	}

	void PaintToggle(HDC dc, const RECT& rect, std::wstring_view icon, bool checked,
		bool hovered) const
	{
		if (checked) FillRectangle(dc, rect, palette.raised.ToColorRef());
		if (checked) FrameRectangle(dc, rect, palette.accent.ToColorRef());
		else if (hovered) FillRectangle(dc, rect, palette.panel.ToColorRef());
		RECT glyph = rect;
		::InflateRect(&glyph, -Dip(2), -Dip(2));
		DrawSearchIcon(dc, icon, glyph, palette.primaryText.ToColorRef());
	}

	void PaintWidget(HDC dc)
	{
		const SearchWidgetGeometry geometry = Geometry();
		const RECT queryBox = geometry.queryBox;
		PaintBox(dc, queryBox, geometry.queryEdit, query, texts.searchPlaceholder);
		PaintToggle(dc, InlineToggleRect(queryBox, 1), L"regex", model.useRegex,
			hoverToggle == EToggle::UseRegex);
		PaintToggle(dc, InlineToggleRect(queryBox, 2), L"whole-word", model.wholeWord,
			hoverToggle == EToggle::WholeWord);
		PaintToggle(dc, InlineToggleRect(queryBox, 3), L"case-sensitive", model.matchCase,
			hoverToggle == EToggle::MatchCase);

		const RECT chevron = ToggleReplaceRect();
		if (hoverToggle == EToggle::ToggleReplace) FillRectangle(dc, chevron, palette.panel.ToColorRef());
		RECT chevronGlyph = chevron;
		chevronGlyph.bottom = chevronGlyph.top + Dip(kInputHeightDip);
		::InflateRect(&chevronGlyph, -Dip(3), -Dip(5));
		DrawSearchIcon(dc, replaceVisible ? L"chevron-down" : L"chevron-right", chevronGlyph,
			palette.primaryText.ToColorRef());

		if (replaceVisible) {
			const RECT replaceBox = geometry.replaceBox;
			PaintBox(dc, replaceBox, geometry.replaceEdit, replace, texts.replacePlaceholder);
			PaintToggle(dc, InlineToggleRect(replaceBox, 1), L"preserve-case", model.preserveCase,
				hoverToggle == EToggle::PreserveCase);
			const RECT replaceAll = ReplaceAllRect();
			if (hoverToggle == EToggle::ReplaceAll) FillRectangle(dc, replaceAll, palette.panel.ToColorRef());
			RECT glyph = replaceAll;
			::InflateRect(&glyph, -Dip(3), -Dip(3));
			DrawSearchIcon(dc, L"replace-all", glyph, palette.primaryText.ToColorRef());
		}

		if (statusText.empty()) return;
		RECT message = MessageRect();
		::SetTextColor(dc, results.completion == ESearchCompletion::InvalidPattern
				|| results.completion == ESearchCompletion::RegexUnavailable
			? palette.danger.ToColorRef()
			: palette.secondaryText.ToColorRef());
		::DrawTextW(dc, statusText.c_str(), static_cast<int>(statusText.size()), &message,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	}

	//! Draws one text run and advances `x`.
	void DrawRun(HDC dc, int& x, int top, int bottom, int right, std::wstring_view text,
		COLORREF color, COLORREF background, bool hasBackground) const
	{
		if (text.empty() || x >= right) return;
		SIZE size{};
		::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
		RECT run{ x, top, std::min<LONG>(right, x + size.cx), bottom };
		if (hasBackground) FillRectangle(dc, run, background);
		::SetTextColor(dc, color);
		RECT clip = run;
		::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &clip,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
		x += size.cx;
	}

	[[nodiscard]] RECT RowActionRect(const RECT& row, int indexFromRight) const noexcept
	{
		const int size = Dip(kRowActionDip);
		RECT action{};
		action.right = row.right - Dip(kRowActionGapDip) - indexFromRight * size;
		action.left = action.right - size;
		action.top = row.top;
		action.bottom = row.bottom;
		return action;
	}

	void PaintRow(HDC dc, int index, const RECT& rect, UINT state)
	{
		if (index < 0 || index >= static_cast<int>(rows.size())) return;
		const Row& row = rows[static_cast<std::size_t>(index)];
		const auto& file = results.files[row.fileIndex];
		const bool selected = (state & ODS_SELECTED) != 0;
		const bool hovered = hoverRow == index;
		const COLORREF background = selected
			? palette.accent.ToColorRef()
			: (hovered ? palette.raised.ToColorRef() : palette.sideBar.ToColorRef());
		FillRectangle(dc, rect, background);
		const COLORREF text = selected ? palette.highlightText.ToColorRef()
			: palette.primaryText.ToColorRef();
		const COLORREF secondary = selected ? palette.highlightText.ToColorRef()
			: palette.secondaryText.ToColorRef();
		if (font.Get()) ::SelectObject(dc, font.Get());
		::SetBkMode(dc, TRANSPARENT);

		// Hover actions are laid out first: every other run is clipped against them,
		// so a long path can never draw underneath a button the user can click.
		int right = rect.right - Dip(kRowActionGapDip);
		if (hovered) {
			const RECT dismiss = RowActionRect(rect, 0);
			RECT glyph = dismiss;
			::InflateRect(&glyph, -Dip(4), -Dip(4));
			DrawSearchIcon(dc, L"close", glyph,
				hoverAction == ERowAction::Dismiss ? palette.accent.ToColorRef() : text);
			right = dismiss.left;
			if (replaceVisible) {
				const RECT replaceAction = RowActionRect(rect, 1);
				RECT replaceGlyph = replaceAction;
				::InflateRect(&replaceGlyph, -Dip(4), -Dip(4));
				DrawSearchIcon(dc, row.isFile ? L"replace-all" : L"replace", replaceGlyph,
					hoverAction == ERowAction::Replace ? palette.accent.ToColorRef() : text);
				right = replaceAction.left;
			}
		}

		if (row.isFile) {
			int x = rect.left + Dip(kRowIndentDip);
			RECT chevron{ x, rect.top, x + Dip(kIconDip), rect.bottom };
			DrawSearchIcon(dc, collapsedFiles.contains(file.fullPath) ? L"chevron-right" : L"chevron-down",
				chevron, text);
			x = chevron.right + Dip(2);
			RECT icon{ x, rect.top + (rect.bottom - rect.top - Dip(kIconDip)) / 2,
				x + Dip(kIconDip), 0 };
			icon.bottom = icon.top + Dip(kIconDip);
			DrawResultFileIcon(dc, file.fileName, icon, text);
			x = icon.right + Dip(kIconGapDip);
			// The badge is reserved before the description so the count never
			// scrolls out from under a long folder path.
			SIZE badgeText{};
			const std::wstring count = std::to_wstring(file.matches.size());
			::GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &badgeText);
			const LONG badgeWidth = std::max<LONG>(Dip(kBadgeMinimumWidthDip), badgeText.cx + Dip(8));
			RECT badge{ right - badgeWidth, rect.top + (rect.bottom - rect.top - Dip(kBadgeHeightDip)) / 2,
				right, 0 };
			badge.bottom = badge.top + Dip(kBadgeHeightDip);
			DrawRun(dc, x, rect.top, rect.bottom, badge.left - Dip(kIconGapDip), file.fileName, text, 0, false);
			if (!file.folderLabel.empty()) {
				x += Dip(kIconGapDip);
				DrawRun(dc, x, rect.top, rect.bottom, badge.left - Dip(kIconGapDip), file.folderLabel,
					secondary, 0, false);
			}
			FillRectangle(dc, badge, palette.activityBarBadgeBackground.ToColorRef());
			::SetTextColor(dc, palette.activityBarBadgeForeground.ToColorRef());
			::DrawTextW(dc, count.c_str(), static_cast<int>(count.size()), &badge,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			return;
		}

		const auto& match = file.matches[row.matchIndex];
		int x = rect.left + Dip(kMatchIndentDip);
		const std::wstring_view preview = match.preview;
		const int offset = std::clamp(match.previewOffset, 0, static_cast<int>(preview.size()));
		const int length = std::clamp(match.previewLength, 0, static_cast<int>(preview.size()) - offset);
		DrawRun(dc, x, rect.top, rect.bottom, right, preview.substr(0, static_cast<std::size_t>(offset)),
			text, 0, false);
		if (model.replaceText.empty() || !replaceVisible) {
			DrawRun(dc, x, rect.top, rect.bottom, right,
				preview.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)),
				text, palette.searchMatchHighlightBackground.ToColorRef(), true);
		} else {
			// Upstream previews a replacement by striking the matched text through
			// and showing the replacement immediately after it.
			const HFONT strike = CreateStrikeoutFont();
			const HGDIOBJ previous = strike != nullptr ? ::SelectObject(dc, strike) : nullptr;
			DrawRun(dc, x, rect.top, rect.bottom, right,
				preview.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)),
				secondary, palette.searchMatchHighlightBackground.ToColorRef(), true);
			if (strike != nullptr) {
				::SelectObject(dc, previous);
				::DeleteObject(strike);
			}
			DrawRun(dc, x, rect.top, rect.bottom, right, ReplacementPreview(match), text,
				palette.diffInsertedLineBackground.ToColorRef(), true);
		}
		DrawRun(dc, x, rect.top, rect.bottom, right,
			preview.substr(static_cast<std::size_t>(offset + length)), text, 0, false);
	}

	[[nodiscard]] std::wstring ReplacementPreview(const SearchMatch& match) const
	{
		if (model.useRegex) return model.replaceText;
		const int offset = std::clamp(match.previewOffset, 0, static_cast<int>(match.preview.size()));
		const int length = std::clamp(match.previewLength, 0,
			static_cast<int>(match.preview.size()) - offset);
		const std::wstring_view matched(match.preview.data() + offset,
			static_cast<std::size_t>(length));
		return model.preserveCase ? ApplyPreserveCase(model.replaceText, matched) : model.replaceText;
	}

	[[nodiscard]] HFONT CreateStrikeoutFont() const
	{
		if (font.Get() == nullptr) return nullptr;
		LOGFONTW logFont{};
		if (::GetObjectW(font.Get(), sizeof(logFont), &logFont) == 0) return nullptr;
		logFont.lfStrikeOut = TRUE;
		return ::CreateFontIndirectW(&logFont);
	}

	// --- interaction ----------------------------------------------------------

	[[nodiscard]] EToggle ToggleAt(POINT point) const
	{
		const auto contains = [&point](const RECT& rect) { return ::PtInRect(&rect, point) != FALSE; };
		const RECT queryBox = QueryBoxRect();
		if (contains(InlineToggleRect(queryBox, 1))) return EToggle::UseRegex;
		if (contains(InlineToggleRect(queryBox, 2))) return EToggle::WholeWord;
		if (contains(InlineToggleRect(queryBox, 3))) return EToggle::MatchCase;
		if (replaceVisible) {
			const RECT replaceBox = ReplaceBoxRect();
			if (contains(InlineToggleRect(replaceBox, 1))) return EToggle::PreserveCase;
			if (contains(ReplaceAllRect())) return EToggle::ReplaceAll;
		}
		if (contains(ToggleReplaceRect())) return EToggle::ToggleReplace;
		return EToggle::None;
	}

	void SetHoverToggle(EToggle toggle)
	{
		if (hoverToggle == toggle) return;
		hoverToggle = toggle;
		Repaint();
	}

	void InvokeToggle(EToggle toggle)
	{
		switch (toggle) {
		case EToggle::MatchCase: model.matchCase = !model.matchCase; StartSearch(); break;
		case EToggle::WholeWord: model.wholeWord = !model.wholeWord; StartSearch(); break;
		case EToggle::UseRegex: model.useRegex = !model.useRegex; StartSearch(); break;
		case EToggle::PreserveCase: model.preserveCase = !model.preserveCase; Repaint(); break;
		case EToggle::ReplaceAll: ReplaceAll(); break;
		case EToggle::ToggleReplace: SetReplaceVisible(!replaceVisible); break;
		case EToggle::None: break;
		}
		Repaint();
	}

	void SetReplaceVisible(bool visible)
	{
		if (replaceVisible == visible) return;
		replaceVisible = visible;
		LayoutChildren();
		Repaint();
	}

	[[nodiscard]] ERowAction RowActionAt(int index, POINT point) const
	{
		if (index < 0 || index >= static_cast<int>(rows.size()) || list == nullptr) {
			return ERowAction::None;
		}
		RECT row{};
		if (::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
				reinterpret_cast<LPARAM>(&row)) == LB_ERR) {
			return ERowAction::None;
		}
		const RECT dismiss = RowActionRect(row, 0);
		if (::PtInRect(&dismiss, point)) return ERowAction::Dismiss;
		const RECT replace = RowActionRect(row, 1);
		if (replaceVisible && ::PtInRect(&replace, point)) return ERowAction::Replace;
		return ERowAction::None;
	}

	void ActivateRow(int index)
	{
		if (index < 0 || index >= static_cast<int>(rows.size())) return;
		const Row& row = rows[static_cast<std::size_t>(index)];
		const auto& file = results.files[row.fileIndex];
		if (row.isFile) {
			if (collapsedFiles.contains(file.fullPath)) collapsedFiles.erase(file.fullPath);
			else collapsedFiles.insert(file.fullPath);
			RebuildRows();
			return;
		}
		const auto& match = file.matches[row.matchIndex];
		if (activateMatch) activateMatch(file.fullPath, match.line, match.column, match.length);
	}

	void DismissRow(int index)
	{
		if (index < 0 || index >= static_cast<int>(rows.size())) return;
		const Row row = rows[static_cast<std::size_t>(index)];
		auto& file = results.files[row.fileIndex];
		if (row.isFile) {
			results.matchCount -= file.matches.size();
			results.files.erase(results.files.begin() + static_cast<std::ptrdiff_t>(row.fileIndex));
		} else {
			file.matches.erase(file.matches.begin() + static_cast<std::ptrdiff_t>(row.matchIndex));
			--results.matchCount;
			if (file.matches.empty()) {
				results.files.erase(results.files.begin() + static_cast<std::ptrdiff_t>(row.fileIndex));
			}
		}
		RebuildRows();
		UpdateStatusText();
		Repaint();
	}

	//! The slice of the model one replace gesture acts on.
	void ReplaceRow(int index)
	{
		if (index < 0 || index >= static_cast<int>(rows.size())) return;
		const Row row = rows[static_cast<std::size_t>(index)];
		const auto& file = results.files[row.fileIndex];
		std::vector<SearchFileResult> slice;
		if (row.isFile) {
			slice.push_back(file);
		} else {
			SearchFileResult single = file;
			single.matches = { file.matches[row.matchIndex] };
			slice.push_back(std::move(single));
		}
		RunReplace(slice);
	}

	void ReplaceAll() { RunReplace(results.files); }

	void RunReplace(const std::vector<SearchFileResult>& slice)
	{
		ReadBoxes();
		if (slice.empty() || model.text.empty() || !acceptedRequest || closed
			|| acceptedRequest->generation != shared->generation.load(std::memory_order_acquire)
			|| acceptedRequest->root != root || !SameSearchPattern(acceptedRequest->query, model)) return;
		const auto outcome = ReplaceMatches(slice, model, {});
		if (filesChanged && outcome.replacedFiles != 0) {
			std::vector<std::wstring> changed;
			changed.reserve(slice.size());
			for (const auto& file : slice) {
				if (std::ranges::find(outcome.failedFiles, file.fullPath) == outcome.failedFiles.end()) {
					changed.push_back(file.fullPath);
				}
			}
			filesChanged(changed);
		}
		if (!outcome.failedFiles.empty()) {
			statusText = FormatMessage(texts.replaceFailed,
				std::to_wstring(outcome.failedFiles.size()), {});
			Repaint();
		}
		// Re-running the query is how the view learns what the files now contain;
		// keeping the pre-replace positions would leave stale rows behind.
		StartSearch();
	}

	// --- layout ---------------------------------------------------------------

	void LayoutChildren()
	{
		if (window == nullptr) return;
		const SearchWidgetGeometry geometry = Geometry();
		const RECT& queryInner = geometry.queryEdit;
		const RECT& replaceInner = geometry.replaceEdit;
		const RECT listRect = ListRect();
		const LPARAM editMargins = MAKELPARAM(static_cast<WORD>(Dip(kInputPaddingLeftDip)), 0);
		for (HWND edit : { query, replace }) {
			if (edit != nullptr) {
				// CSS uses `padding-left: 6px`; make the Win32 edit's margin
				// explicit so its text does not inherit the platform default.
				(void)::SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN, editMargins);
			}
		}
		if (list != nullptr) {
			::SendMessageW(list, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(Dip(kRowHeightDip)));
		}

		const UINT commonFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
		const auto moveWindow = [commonFlags](HWND child, const RECT& bounds, UINT visibility) {
			if (child == nullptr) return;
			::SetWindowPos(child, nullptr, bounds.left, bounds.top,
				std::max(0L, bounds.right - bounds.left),
				std::max(0L, bounds.bottom - bounds.top), commonFlags | visibility);
		};
		HDWP transaction = ::BeginDeferWindowPos(3);
		if (transaction != nullptr && query != nullptr) {
			transaction = ::DeferWindowPos(transaction, query, nullptr,
				queryInner.left, queryInner.top,
				std::max(0L, queryInner.right - queryInner.left),
				std::max(0L, queryInner.bottom - queryInner.top), commonFlags | SWP_SHOWWINDOW);
		}
		if (transaction != nullptr && replace != nullptr) {
			transaction = ::DeferWindowPos(transaction, replace, nullptr,
				replaceInner.left, replaceInner.top,
				std::max(0L, replaceInner.right - replaceInner.left),
				std::max(0L, replaceInner.bottom - replaceInner.top),
				commonFlags | (replaceVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
		}
		if (transaction != nullptr && list != nullptr) {
			transaction = ::DeferWindowPos(transaction, list, nullptr,
				listRect.left, listRect.top,
				std::max(0L, listRect.right - listRect.left),
				std::max(0L, listRect.bottom - listRect.top), commonFlags | SWP_SHOWWINDOW);
		}
		if (transaction != nullptr) {
			(void)::EndDeferWindowPos(transaction);
		} else {
			// DeferWindowPos can fail under desktop resource pressure. Keep the
			// no-redraw contract on the fallback path so a partial transaction never
			// reintroduces one-child-at-a-time painting.
			moveWindow(query, queryInner, SWP_SHOWWINDOW);
			moveWindow(replace, replaceInner, replaceVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
			moveWindow(list, listRect, SWP_SHOWWINDOW);
		}
		scrollbar.SetDpi(dpi);
		scrollbar.Update();
		QueueNoEraseInvalidate(window);
		QueueNoEraseInvalidate(query);
		QueueNoEraseInvalidate(replace);
		QueueNoEraseInvalidate(list);
	}

	void ApplyScrollbarColors()
	{
		scrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette, palette.sideBar));
	}

	// --- worker ---------------------------------------------------------------

	void Start()
	{
		shared->wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		shared->stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		auto retirement = workbench::WorkerRetirementService::Instance().TryReserve();
		if (!retirement) return;
		auto state = shared;
		worker = std::thread([state]() {
			const HANDLE handles[2] = { state->stop, state->wake };
			for (;;) {
				const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
				if (wait == WAIT_OBJECT_0 || state->stopping.load(std::memory_order_acquire)) return;
				WorkerRequest request;
				{
					std::lock_guard<std::mutex> guard(state->mutex);
					if (!state->hasPending) continue;
					request = state->pending;
					state->hasPending = false;
				}
				auto result = std::make_unique<WorkerResult>(request);
				result->results = RunWorkspaceSearch(request.root, request.query, [state, &request]() {
					return state->stopping.load(std::memory_order_acquire)
						|| state->generation.load(std::memory_order_acquire) != request.generation;
				});
				std::lock_guard<std::mutex> guard(state->windowMutex);
				if (state->window == nullptr
					|| state->generation.load(std::memory_order_acquire) != request.generation) {
					continue;
				}
				const auto published = state->results.Publish(std::move(result));
				if (published.wakeRequired
					&& ::PostMessageW(state->window, kResultMessage, 0, 0) == FALSE) {
					state->results.CancelWakeAndDiscard();
				}
			}
		});
		workerRetirement.emplace(std::move(*retirement));
	}

	void SetWorkerWindow(HWND value)
	{
		std::lock_guard<std::mutex> guard(shared->windowMutex);
		shared->window = value;
		if (value == nullptr) shared->results.Close();
		else shared->results.Open();
	}

	std::unique_ptr<WorkerResult> TakeLatestResult()
	{
		std::lock_guard<std::mutex> guard(shared->windowMutex);
		auto result = shared->results.Take();
		return result ? std::move(*result) : nullptr;
	}

	void DropQueuedResults() noexcept
	{
		if (window == nullptr) return;
		MSG message{};
		while (::PeekMessageW(&message, window, kResultMessage, kResultMessage, PM_REMOVE) != FALSE) {
		}
		std::lock_guard<std::mutex> guard(shared->windowMutex);
		shared->results.CancelWakeAndDiscard();
	}
};

namespace {

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &CSearchWorkbenchTool::WindowProc;
	wc.hInstance = instance;
	wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

CSearchWorkbenchTool::CSearchWorkbenchTool() : m_impl(std::make_unique<Impl>()) {}
CSearchWorkbenchTool::~CSearchWorkbenchTool() { Close(); }

bool CSearchWorkbenchTool::Create(HWND parent)
{
	if (m_impl->closed || m_impl->window != nullptr || parent == nullptr) return false;
	auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (instance == nullptr) instance = ::GetModuleHandleW(nullptr);
	if (!EnsureClass(instance)) return false;
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_impl->window == nullptr) return false;
	m_impl->query = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kQueryControlId)), instance, nullptr);
	m_impl->replace = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kReplaceControlId)), instance, nullptr);
	m_impl->list = ::CreateWindowExW(0, L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
			| LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kListControlId)), instance, nullptr);
	if (m_impl->query == nullptr || m_impl->replace == nullptr || m_impl->list == nullptr) {
		Close();
		return false;
	}
	(void)::SetWindowSubclass(m_impl->query, &CSearchWorkbenchTool::EditSubclassProc,
		kQueryControlId, reinterpret_cast<DWORD_PTR>(m_impl.get()));
	(void)::SetWindowSubclass(m_impl->replace, &CSearchWorkbenchTool::EditSubclassProc,
		kReplaceControlId, reinterpret_cast<DWORD_PTR>(m_impl.get()));
	(void)::SetWindowSubclass(m_impl->list, &CSearchWorkbenchTool::ListSubclassProc,
		kListControlId, reinterpret_cast<DWORD_PTR>(m_impl.get()));
	Impl* const impl = m_impl.get();
	(void)m_impl->scrollbar.Create(m_impl->window, m_impl->list, [impl](int topRow) {
		if (impl->list != nullptr) {
			(void)::SendMessageW(impl->list, LB_SETTOPINDEX, static_cast<WPARAM>(topRow), 0);
		}
	});
	m_impl->ApplyScrollbarColors();
	m_impl->SetWorkerWindow(m_impl->window);
	m_impl->Start();
	return true;
}

void CSearchWorkbenchTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (m_impl->closed || m_impl->window == nullptr) return;
	m_impl->dpi = dpi == 0 ? 96 : dpi;
	if (m_impl->font.Dpi() != m_impl->dpi) {
		(void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
		// A new font means a new text line height, and the boxes are centered on it.
		m_impl->inputLineHeight = 0;
	}
	for (HWND child : { m_impl->query, m_impl->replace, m_impl->list }) {
		if (child != nullptr) {
			::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), FALSE);
		}
	}
	::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top,
		contentRect.right - contentRect.left, contentRect.bottom - contentRect.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
	m_impl->LayoutChildren();
}

void CSearchWorkbenchTool::Activate()
{
	m_impl->active = true;
	FocusQuery();
}

void CSearchWorkbenchTool::Deactivate() { m_impl->active = false; }

bool CSearchWorkbenchTool::PreTranslateMessage(MSG& message)
{
	if (!m_impl->active) return false;
	if (message.hwnd == m_impl->query || message.hwnd == m_impl->replace) {
		if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
			if (message.hwnd == m_impl->replace && (::GetKeyState(VK_CONTROL) & 0x8000) != 0
				&& (::GetKeyState(VK_MENU) & 0x8000) != 0) {
				// `Ctrl+Alt+Enter` is upstream's Replace All accelerator.
				m_impl->ReplaceAll();
				return true;
			}
			m_impl->StartSearch();
			return true;
		}
		// The frame consults its legacy accelerator table after this hook and would
		// otherwise claim ordinary editing keys before the box ever saw them.
		::TranslateMessage(&message);
		::DispatchMessageW(&message);
		return true;
	}
	if (message.hwnd != m_impl->list || message.message != WM_KEYDOWN) return false;
	if (message.wParam == VK_RETURN) {
		m_impl->ActivateRow(static_cast<int>(::SendMessageW(m_impl->list, LB_GETCURSEL, 0, 0)));
		return true;
	}
	if (message.wParam == VK_DELETE) {
		m_impl->DismissRow(static_cast<int>(::SendMessageW(m_impl->list, LB_GETCURSEL, 0, 0)));
		return true;
	}
	return false;
}

void CSearchWorkbenchTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	m_impl->closed = true;
	m_impl->SetWorkerWindow(nullptr);
	m_impl->shared->stopping.store(true, std::memory_order_release);
	if (m_impl->shared->stop != nullptr) ::SetEvent(m_impl->shared->stop);
	if (m_impl->shared->wake != nullptr) ::SetEvent(m_impl->shared->wake);
	m_impl->DropQueuedResults();
	(void)m_impl->nativeSurface.Close();
	if (m_impl->worker.joinable() && m_impl->workerRetirement) {
		(void)workbench::WorkerRetirementService::Instance().Retire(
			std::move(m_impl->worker), std::move(*m_impl->workerRetirement), m_impl->shared);
		m_impl->workerRetirement.reset();
	}
	m_impl->scrollbar.Destroy();
	m_impl->listBuffer.Reset();
	m_impl->rootBuffer.Reset();
	m_impl->rows.clear();
	m_impl->results = {};
	if (m_impl->window != nullptr && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->query = nullptr;
	m_impl->replace = nullptr;
	m_impl->list = nullptr;
}

void CSearchWorkbenchTool::SetRoot(std::wstring root)
{
	if (m_impl->root == root) return;
	m_impl->root = std::move(root);
	m_impl->StartSearch();
}

void CSearchWorkbenchTool::SetPalette(const theme::ThemePalette& palette)
{
	m_impl->palette = palette;
	m_impl->ApplyScrollbarColors();
	m_impl->scrollbar.Update();
	m_impl->Repaint();
	QueueNoEraseInvalidate(m_impl->list);
}

void CSearchWorkbenchTool::SetTexts(SearchViewTexts texts)
{
	m_impl->texts = std::move(texts);
	m_impl->UpdateStatusText();
	m_impl->Repaint();
}

void CSearchWorkbenchTool::SetMatchActivationCallback(MatchActivationCallback callback)
{
	m_impl->activateMatch = std::move(callback);
}

void CSearchWorkbenchTool::SetFilesChangedCallback(FilesChangedCallback callback)
{
	m_impl->filesChanged = std::move(callback);
}

void CSearchWorkbenchTool::SetVisible(bool visible)
{
	if (m_impl->window != nullptr) ::ShowWindow(m_impl->window, visible ? SW_SHOW : SW_HIDE);
}

void CSearchWorkbenchTool::Refresh()
{
	if (!m_impl->closed) m_impl->StartSearch();
}

void CSearchWorkbenchTool::FocusQuery()
{
	if (m_impl->query == nullptr) return;
	::SetFocus(m_impl->query);
	::SendMessageW(m_impl->query, EM_SETSEL, 0, -1);
}

void CSearchWorkbenchTool::FocusReplace()
{
	m_impl->SetReplaceVisible(true);
	if (m_impl->replace == nullptr) return;
	::SetFocus(m_impl->replace);
	::SendMessageW(m_impl->replace, EM_SETSEL, 0, -1);
}

void CSearchWorkbenchTool::SetQueryText(std::wstring text)
{
	if (m_impl->query == nullptr) return;
	m_impl->updatingText = true;
	::SetWindowTextW(m_impl->query, text.c_str());
	m_impl->updatingText = false;
	m_impl->StartSearch();
}

HWND CSearchWorkbenchTool::GetHwnd() const noexcept { return m_impl->window; }

void CSearchWorkbenchTool::SetNativeSurfaceSink(
	rendering::FrameNativeSurfacePayloadSink sink) noexcept
{
	m_impl->nativeSurface.SetSink(std::move(sink));
}

rendering::FrameNativeSurfacePayloadResult CSearchWorkbenchTool::RegisterNativeSurface(
	const rendering::FrameNativeSurfacePayloadTarget& target) noexcept
{
	return m_impl->nativeSurface.Register(target);
}

rendering::FrameNativeSurfacePayloadResult CSearchWorkbenchTool::UpdateNativeSurface(
	const rendering::FrameNativeSurfacePayloadTarget& target) noexcept
{
	return m_impl->nativeSurface.Update(target);
}

rendering::FrameNativeSurfacePayloadResult CSearchWorkbenchTool::SubmitNativeSurface(
	const HDC sourceDc, const RECT& dirtyRect) noexcept
{
	return m_impl->nativeSurface.Submit(sourceDc, dirtyRect);
}

rendering::FrameNativeSurfacePayloadResult CSearchWorkbenchTool::CloseNativeSurface() noexcept
{
	return m_impl->nativeSurface.Close();
}

LRESULT CALLBACK CSearchWorkbenchTool::EditSubclassProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	if (message == WM_NCDESTROY) {
		::RemoveWindowSubclass(window, &CSearchWorkbenchTool::EditSubclassProc, id);
	}
	auto* const impl = reinterpret_cast<Impl*>(data);
	if (impl != nullptr && (message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
		// The focus ring is painted by the parent around the box, so the parent has
		// to repaint when focus moves between the two boxes.
		impl->Repaint();
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK CSearchWorkbenchTool::ListSubclassProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	if (message == WM_NCDESTROY) {
		::RemoveWindowSubclass(window, &CSearchWorkbenchTool::ListSubclassProc, id);
	}
	auto* const impl = reinterpret_cast<Impl*>(data);
	if (impl != nullptr) {
		switch (message) {
		case WM_MOUSEMOVE: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const int index = static_cast<int>(::SendMessageW(window, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(point.x, point.y)));
			const bool outside = HIWORD(index) != 0;
			const int row = outside ? -1 : LOWORD(index);
			const ERowAction action = impl->RowActionAt(row, point);
			if (row != impl->hoverRow || action != impl->hoverAction) {
				impl->hoverRow = row;
				impl->hoverAction = action;
				::InvalidateRect(window, nullptr, FALSE);
			}
			if (!impl->trackingMouse) {
				TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
				impl->trackingMouse = ::TrackMouseEvent(&track) != FALSE;
			}
			break;
		}
		case WM_MOUSELEAVE:
			impl->trackingMouse = false;
			impl->hoverRow = -1;
			impl->hoverAction = ERowAction::None;
			::InvalidateRect(window, nullptr, FALSE);
			break;
		case WM_LBUTTONUP: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const int index = static_cast<int>(::SendMessageW(window, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(point.x, point.y)));
			if (HIWORD(index) == 0) {
				const int row = LOWORD(index);
				switch (impl->RowActionAt(row, point)) {
				case ERowAction::Dismiss: impl->DismissRow(row); return 0;
				case ERowAction::Replace: impl->ReplaceRow(row); return 0;
				case ERowAction::None: break;
				}
				// A file row toggles on a single click, exactly as a tree twistie does;
				// a match row needs a double click, which arrives as LBN_DBLCLK.
				if (row >= 0 && row < static_cast<int>(impl->rows.size())
					&& impl->rows[static_cast<std::size_t>(row)].isFile) {
					const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
					impl->ActivateRow(row);
					return result;
				}
			}
			break;
		}
		case WM_VSCROLL:
		case WM_MOUSEWHEEL:
		case WM_KEYDOWN:
		case WM_SIZE: {
			const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
			impl->scrollbar.Update();
			return result;
		}
		default:
			break;
		}
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK CSearchWorkbenchTool::WindowProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CSearchWorkbenchTool*>(
			reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	auto* self = reinterpret_cast<CSearchWorkbenchTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (self == nullptr || !self->m_impl) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *self->m_impl;
	switch (message) {
	case WM_SIZE:
		impl.LayoutChildren();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC target = ::BeginPaint(window, &paint);
		if (target == nullptr) return 0;
		RECT client{};
		::GetClientRect(window, &client);
		const int width = std::max(0L, client.right - client.left);
		const int height = std::max(0L, client.bottom - client.top);
		const bool buffered = impl.rootBuffer.Ensure(target, width, height);
		const HDC dc = buffered ? impl.rootBuffer.Dc() : target;
		const int saved = ::SaveDC(dc);
		FillRectangle(dc, buffered ? client : paint.rcPaint, impl.palette.sideBar.ToColorRef());
		if (impl.font.Get() != nullptr) ::SelectObject(dc, impl.font.Get());
		::SetBkMode(dc, TRANSPARENT);
		impl.PaintWidget(dc);
		if (saved != 0) ::RestoreDC(dc, saved);
		if (buffered) (void)impl.rootBuffer.Present(target, paint.rcPaint);
		(void)impl.nativeSurface.Submit(dc, paint.rcPaint);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_SETCURSOR: {
		if (reinterpret_cast<HWND>(wParam) != window) break;
		POINT point{};
		if (::GetCursorPos(&point) == FALSE || ::ScreenToClient(window, &point) == FALSE) break;
		if (impl.ToggleAt(point) != EToggle::None) {
			::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		impl.SetHoverToggle(impl.ToggleAt(point));
		return 0;
	}
	case WM_MOUSELEAVE:
		impl.SetHoverToggle(EToggle::None);
		return 0;
	case WM_LBUTTONUP: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const EToggle toggle = impl.ToggleAt(point);
		if (toggle != EToggle::None) { impl.InvokeToggle(toggle); return 0; }
		break;
	}
	case WM_TIMER:
		if (wParam == kTypeTimer) {
			::KillTimer(window, kTypeTimer);
			impl.StartSearch();
			return 0;
		}
		break;
	case WM_COMMAND:
		if (HIWORD(wParam) == EN_CHANGE && !impl.updatingText) {
			if (LOWORD(wParam) == kQueryControlId) {
				impl.ScheduleSearch();
				impl.Repaint();
				return 0;
			}
			if (LOWORD(wParam) == kReplaceControlId) {
				impl.ReadBoxes();
				impl.Repaint();
				if (impl.list != nullptr) ::InvalidateRect(impl.list, nullptr, FALSE);
				return 0;
			}
		}
		if (LOWORD(wParam) == kListControlId && HIWORD(wParam) == LBN_DBLCLK) {
			impl.ActivateRow(static_cast<int>(::SendMessageW(impl.list, LB_GETCURSEL, 0, 0)));
			return 0;
		}
		break;
	case WM_DRAWITEM: {
		auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
		if (draw != nullptr && draw->CtlID == kListControlId && draw->hwndItem == impl.list
			&& draw->itemID != static_cast<UINT>(-1)) {
			RECT client{};
			::GetClientRect(impl.list, &client);
			const int width = std::max(0L, client.right - client.left);
			const int height = std::max(0L, client.bottom - client.top);
			const bool buffered = impl.listBuffer.Ensure(draw->hDC, width, height);
			const HDC dc = buffered ? impl.listBuffer.Dc() : draw->hDC;
			const int saved = ::SaveDC(dc);
			if (buffered) {
				(void)::IntersectClipRect(dc, draw->rcItem.left, draw->rcItem.top,
					draw->rcItem.right, draw->rcItem.bottom);
			}
			impl.PaintRow(dc, static_cast<int>(draw->itemID), draw->rcItem, draw->itemState);
			if (saved != 0) ::RestoreDC(dc, saved);
			if (buffered) (void)impl.listBuffer.Present(draw->hDC, draw->rcItem);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLOREDIT: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.inputBackground.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.inputBackground.ToColorRef());
		// The native EDIT is one pixel inside PaintBox.  Keep its solid brush so
		// deleting text invalidates and clears glyphs on every theme/DPI.
		return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
	}
	case WM_CTLCOLORLISTBOX: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.sideBar.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.sideBar.ToColorRef());
		return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
	}
	case kResultMessage: {
		impl.AcceptResult(impl.TakeLatestResult());
		return 0;
	}
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::search
