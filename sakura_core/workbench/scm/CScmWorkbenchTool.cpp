/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitHistoryModel.h"
#include "workbench/scm/GitInitCloneCommands.h"
#include "workbench/scm/GitScmMenus.h"
#include "workbench/scm/GitScmPublisher.h"
#include "workbench/scm/ScmViewStackLayout.h"

#include "theme/CThemeService.h"
#include "workbench/IconMetrics.h"
#include "workbench/ViewsWelcomeMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CSetiFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/SetiFileIcon.h"
#include "workbench/icons/SetiIconPainter.h"
#include "workbench/icons/ThemeIconResolver.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/rendering/LatestOnlyMailbox.h"
#include "workbench/WorkerRetirementService.h"

#include <CommCtrl.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <ranges>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace workbench::scm {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraNativeScmTool";
constexpr UINT kResultMessage = WM_APP + 0x5a1;
constexpr UINT_PTR kRefreshTimer = 0x5a2;
	constexpr UINT kRefreshMilliseconds = 5000;
constexpr std::size_t kMaximumStatusBytes = 4u * 1024u * 1024u;

//! Current VS Code stacks independent Repositories, Changes, and Graph Views
//! inside the Source Control ViewContainer.  This native host draws matching
//! section headers inside its single SCM HWND.
constexpr int kScmViewHeaderHeightDip = 30;
//! One repository row, matching upstream's `ListDelegate.getHeight` for the
//! `repository` template.
constexpr int kRepositoryRowHeightDip = 22;
//! The row's own left/right inset, shared with all View headers so their labels
//! and their content line up.
constexpr int kRowInsetDip = 10;
//! `.scm-provider > .icon`, which is `$(repo)` for a provider with no `iconPath`.
constexpr int kRepositoryIconDip = 16;
constexpr int kRepositoryIconGapDip = 6;
//! Padding inside one toolbar button, on each side of its rendered label.
constexpr int kActionInsetDip = 6;
//! `scm.inputMinLineCount` and `scm.inputMaxLineCount` at upstream's registered
//! defaults and upstream's registered bounds. The effective values arrive from
//! the configuration service through `SetInputLineCountRange`; these are what a
//! window with no runtime to read settings from falls back to.
constexpr int kScmInputMinLineCountDefault = 1;
constexpr int kScmInputMaxLineCountDefault = 10;
constexpr int kScmInputLineCountLowerBound = 1;
constexpr int kScmInputLineCountUpperBound = 50;
//! Upstream's `InputRenderer.getHeight` is the widget's content height plus ten
//! pixels, which is this margin above the box and the same margin below it.
constexpr int kInputOuterMarginDip = 5;

//! Scrolls an owner-drawn list box by one wheel notch worth of rows. The list
//! boxes here keep WS_VSCROLL for their SCROLLINFO but hide the platform bar
//! behind the themed overlay, and a list box whose scroll bar is hidden ignores
//! WM_MOUSEWHEEL entirely, so the rows have to be moved by hand.
void ScrollListBoxByWheel(HWND list, WPARAM wParam)
{
	if (list == nullptr) return;
	const int count = static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
	if (count <= 0) return;
	UINT linesPerNotch = 3;
	if (::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0) == FALSE) {
		linesPerNotch = 3;
	}
	// WHEEL_PAGESCROLL asks for a page; the visible row count is the page here.
	int rows = 0;
	const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
	if (linesPerNotch == WHEEL_PAGESCROLL) {
		RECT client{};
		::GetClientRect(list, &client);
		const int rowHeight = static_cast<int>(::SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
		const int page = rowHeight > 0
			? std::max(1, static_cast<int>(client.bottom - client.top) / rowHeight) : 1;
		rows = delta > 0 ? -page : page;
	} else {
		rows = -(delta * static_cast<int>(linesPerNotch)) / WHEEL_DELTA;
	}
	if (rows == 0) return;
	const int top = static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0));
	const int next = std::clamp(top + rows, 0, std::max(0, count - 1));
	if (next == top) return;
	(void)::SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(next), 0);
}

//! Builds the overlay model directly from the list's stable row contract.
//! Native LISTBOX SCROLLINFO can publish its new page one paint later after a
//! resize; using it would make the overlay thumb the only late SCM surface.
[[nodiscard]] controls::OverlayScrollbarModel ListOverlayModel(HWND list)
{
	if (list == nullptr) return {};
	const int count = std::max(0,
		static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0)));
	const int itemHeight = std::max(1,
		static_cast<int>(::SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
	RECT client{};
	::GetClientRect(list, &client);
	const int viewport = std::max(1,
		static_cast<int>(client.bottom - client.top) / itemHeight);
	const int top = std::max(0,
		static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
	return { .contentExtent = count, .viewportExtent = viewport, .offset = top };
}
//! The box's own text padding. One line plus both paddings is upstream's
//! `InputRenderer.DEFAULT_HEIGHT` of 26 at the default font size.
constexpr int kInputPaddingDip = 4;
//! The gap between the Commit button's label and the dropdown half, and the
//! dropdown half's own width. Upstream's `.monaco-button-dropdown` is a 1px
//! separator plus a `$(chevron-down)` box; this is that box at 100%.
constexpr int kActionButtonDropdownDip = 22;
//! The commit box's child-control id. The list predates it and keeps 1.
constexpr int kInputControlId = 2;
//! The empty-state welcome content is upstream's `viewsWelcome`, the same
//! contribution the Explorer renders, so its inset, its 300-DIP action column
//! and its button box all come from workbench/ViewsWelcomeMetrics.h. Measuring
//! the button height from the running font here is what let these two Views
//! disagree about how tall the same upstream button is.
//! The deliberately non-interactive Graph body reserved under its View header.
//! It is large enough to communicate the unsupported boundary without stealing
//! the change-list's normal scrolling area.
constexpr int kGraphPlaceholderHeightDip = 48;
//! The Graph body's own starting height, and the smallest either section may be
//! squeezed to by a sash drag.  Upstream's pane sashes have the same two facts.
constexpr int kGraphDefaultBodyHeightDip = 180;
constexpr int kGraphMinimumBodyHeightDip = 44;
constexpr int kChangesMinimumBodyHeightDip = 66;
//! `.monaco-sash` is 4px, centred on the boundary it drags.
constexpr int kSashHeightDip = 4;
//! One history row, upstream's `HistoryItemRenderer` height.
constexpr int kGraphRowHeightDip = 22;
//! Horizontal distance between two swimlanes, and the commit circle's radius.
constexpr int kGraphLaneWidthDip = 11;
constexpr int kGraphCircleRadiusDip = 4;
//! Upstream's `CIRCLE_STROKE_WIDTH`, which is also the radius of the hole it
//! punches in the HEAD node.
constexpr int kGraphCircleStrokeWidthDip = 2;
//! How many commits the Graph reads.  Upstream pages its history; this reads one
//! bounded page, because an unbounded `git log` on the refresh timer is not.
constexpr std::size_t kGraphHistoryCount = 50;
constexpr int kGraphControlId = 2;
//!
//! @brief The five swimlane colours, in `BuildScmHistoryGraph`'s own index order.
//!
//! These are the registered defaults of upstream's `scmGraph.foreground1`..`5`.
//! They are literals here because the theme palette publishes no graph tokens
//! yet; when it does, they must come from it rather than being re-tuned here.
//!
constexpr COLORREF kGraphLaneColors[kScmGraphColorCount] = {
	RGB(0xFF, 0xB0, 0x00), RGB(0xDC, 0x26, 0x7F), RGB(0x99, 0x4F, 0x00),
	RGB(0x40, 0xB0, 0xA6), RGB(0xB6, 0x6D, 0xFF),
};

//! The history graph is painted into a GDI list box, but its node circles are
//! vector shapes in VS Code's SVG. A DC render target gives those small shapes
//! per-primitive antialiasing without changing the list's text or lane paths.
//! The target is kept per SCM tool and rebound to the current row on every paint.
class GraphNodePainter final {
public:
	[[nodiscard]] bool Paint(HDC dc, const RECT& clip, LONG circleX, LONG middle,
		int outerRadius, int strokeWidth, int innerRadius, int innerMergeRadius,
		int headInnerStrokeWidth, bool isHead, bool isMerge, COLORREF circleColor,
		COLORREF background) noexcept
	{
		if (dc == nullptr || clip.right <= clip.left || clip.bottom <= clip.top) return false;
		if (!EnsureTarget()) return false;
		const int saved = ::SaveDC(dc);
		if (saved == 0) return false;
		bool drawn = false;
		// The lane paths were issued through GDI immediately before this call.
		// Flush them before Direct2D composites the antialiased node over them.
		::GdiFlush();
		if (SUCCEEDED(m_target->BindDC(dc, &clip))) {
			m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke;
			if (SUCCEEDED(m_target->CreateSolidColorBrush(ToColor(circleColor), &fill))
				&& SUCCEEDED(m_target->CreateSolidColorBrush(ToColor(background), &stroke))) {
				m_target->BeginDraw();
				// BindDC makes the sub-rectangle's top-left the render target's local
				// origin. Keep the target row-sized so EndDraw cannot copy an internal
				// bitmap over neighbouring rows.
				const auto center = D2D1::Point2F(
					static_cast<float>(circleX - clip.left),
					static_cast<float>(middle - clip.top));
				const auto drawEllipse = [&](int radius, ID2D1Brush* brush) {
					const auto ellipse = D2D1::Ellipse(center, static_cast<float>(radius), static_cast<float>(radius));
					m_target->FillEllipse(ellipse, brush);
					m_target->DrawEllipse(ellipse, stroke.Get(), static_cast<float>(strokeWidth));
				};
				if (isHead) {
					drawEllipse(outerRadius, fill.Get());
					// The HEAD node is an outlined ring. Its inner SVG circle has a
					// background fill and a stroke as wide as the normal node radius.
					const auto inner = D2D1::Ellipse(center, static_cast<float>(innerRadius),
						static_cast<float>(innerRadius));
					m_target->FillEllipse(inner, stroke.Get());
					m_target->DrawEllipse(inner, stroke.Get(), static_cast<float>(headInnerStrokeWidth));
				} else if (isMerge) {
					drawEllipse(outerRadius, fill.Get());
					// VS Code's multi-parent node has a second coloured circle inside
					// the background outline of the outer circle.
					const auto inner = D2D1::Ellipse(center, static_cast<float>(innerMergeRadius),
						static_cast<float>(innerMergeRadius));
					m_target->FillEllipse(inner, fill.Get());
					m_target->DrawEllipse(inner, stroke.Get(), static_cast<float>(strokeWidth));
				} else {
					drawEllipse(outerRadius, fill.Get());
				}
				const HRESULT result = m_target->EndDraw();
				if (result == D2DERR_RECREATE_TARGET) m_target.Reset();
				drawn = SUCCEEDED(result);
			}
		}
		::RestoreDC(dc, saved);
		return drawn;
	}

private:
	static D2D1_COLOR_F ToColor(COLORREF color) noexcept
	{
		return D2D1::ColorF(
			static_cast<float>(GetRValue(color)) / 255.0F,
			static_cast<float>(GetGValue(color)) / 255.0F,
			static_cast<float>(GetBValue(color)) / 255.0F, 1.0F);
	}

	[[nodiscard]] bool EnsureTarget() noexcept
	{
		if (!m_factory) {
			if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				m_factory.GetAddressOf()))) return false;
		}
		if (m_target) return true;
		const auto properties = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
			96.0F, 96.0F);
		return SUCCEEDED(m_factory->CreateDCRenderTarget(&properties, m_target.GetAddressOf()));
	}

	Microsoft::WRL::ComPtr<ID2D1Factory> m_factory;
	Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_target;
};

//!
//! @brief `scm.providerCountBadge`'s documented default, `hidden`.
//!
//! Upstream toggles `hide-provider-counts` / `auto-provider-counts` on the tree
//! container from that setting, and `scm.css` reads them:
//! `.scm-view.hide-provider-counts .scm-provider > .count` and
//! `.scm-view.auto-provider-counts .scm-provider > .count[data-count="0"]` are
//! both `display: none`. With the default, a real VS Code shows **no** count on
//! this row at any value, so neither does this band. The count itself is still
//! computed exactly as upstream computes it, so reading the setting is all that
//! `auto` and `visible` would need.
//!
enum class EProviderCountBadgePolicy { Hidden, Auto, Visible };
constexpr EProviderCountBadgePolicy kProviderCountBadgePolicy = EProviderCountBadgePolicy::Hidden;

//! Upstream's two CSS rules, evaluated against a resolved count.
[[nodiscard]] constexpr bool ShowsProviderCountBadge(std::int64_t count) noexcept
{
	switch (kProviderCountBadgePolicy) {
	case EProviderCountBadgePolicy::Hidden: return false;
	case EProviderCountBadgePolicy::Auto: return count != 0;
	case EProviderCountBadgePolicy::Visible: return true;
	}
	return false;
}

//! この band は自前のフォントキャッシュを持たない。ステータスバーと違って
//! `renderLabelWithIcons` を描くのは行の再構築時だけなので、都度生成で足りる。
const icons::SLabelRunFontProvider& GlyphFonts()
{
	static const icons::SLabelRunFontProvider provider = icons::OwnedGlyphFontProvider();
	return provider;
}

//! The Codicon the built-in Git extension gives a history-item ref: a branch
//! and the HEAD it points at are `$(git-branch)`, a remote-tracking branch is
//! `$(cloud)`, and a tag is `$(tag)`.
[[nodiscard]] std::wstring GraphRefIcon(EGitHistoryRefKind kind)
{
	switch (kind) {
	case EGitHistoryRefKind::RemoteBranch: return L"$(cloud)";
	case EGitHistoryRefKind::Tag: return L"$(tag)";
	case EGitHistoryRefKind::Head:
	case EGitHistoryRefKind::LocalBranch:
	default: return L"$(git-branch)";
	}
}

std::vector<icons::SLabelRun> ParseRuns(std::wstring_view label)
{
	// No contributed-icon registry here: the built-in Git provider's own titles
	// only ever name codicons, and passing a registry this tool does not own
	// would let another extension's `$(name)` change what Git renders.
	return icons::ParseLabelWithIcons(label, icons::CCodiconFont::Instance().FaceName());
}

//! `$(discard)` -> `discard`. A contribution declares its icon in the `$(name)`
//! form, while `DrawScmIcon` names the codicon directly.
std::wstring StripCodiconWrapper(std::wstring_view icon)
{
	if (icon.starts_with(L"$(") && icon.ends_with(L")")) {
		return std::wstring(icon.substr(2, icon.size() - 3));
	}
	return std::wstring(icon);
}

void DrawScmIcon(HDC dc, std::wstring_view name, const RECT& rect, COLORREF color)
{
	if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) return;
	const auto icon = icons::ResolveThemeIcon(name, icons::CCodiconFont::Instance().FaceName());
	if (icon.font && !icon.fontIcon.glyph.empty()) {
		const HFONT glyphFont = icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName,
			std::max(1, static_cast<int>(rect.bottom - rect.top)));
		if (glyphFont != nullptr) {
			const HGDIOBJ previous = ::SelectObject(dc, glyphFont);
			const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
			const COLORREF previousTextColor = ::SetTextColor(dc, color);
			RECT glyph = rect;
			::DrawTextW(dc, icon.fontIcon.glyph.c_str(), static_cast<int>(icon.fontIcon.glyph.size()),
				&glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
			::SetTextColor(dc, previousTextColor);
			::SetBkMode(dc, previousBackgroundMode);
			::SelectObject(dc, previous);
			::DeleteObject(glyphFont);
		}
	} else {
		icons::codicons::Draw(dc, icons::IconRect{ rect.left, rect.top, rect.right, rect.bottom }, icon.builtin, color);
	}
}

/*!
	@brief Draws a resource row's file icon the way the Explorer draws one

	A Source Control resource row carries the same file icon as the same file in
	the Explorer, because upstream resolves both through the active file icon
	theme rather than per view. The bundled `vs-seti` theme is what VS Code
	selects by default; when seti.ttf failed to register, its code points mean
	nothing in another face, so the row falls back to the plain `file` codicon.
*/
void DrawScmFileIcon(HDC dc, std::wstring_view path, const RECT& rect, COLORREF fallbackColor)
{
	const std::size_t separator = path.find_last_of(L"\\/");
	const std::wstring_view fileName =
		separator == std::wstring_view::npos ? path : path.substr(separator + 1);
	if (icons::CSetiFont::Instance().IsAvailable()) {
		const auto icon = icons::seti::ResolveSetiFileIcon(fileName, false,
			theme::CThemeService::IsActiveColorThemeLightKind()
				? icons::seti::EIconVariant::Light
				: icons::seti::EIconVariant::Dark);
		if (!icon) return;
		icons::seti::DrawSetiIcon(dc, rect, icon->character,
			icon->color == icons::seti::kInheritColor
				? fallbackColor
				: icons::seti::ColorRefFromThemeRgb(icon->color));
		return;
	}
	DrawScmIcon(dc, L"file", rect, fallbackColor);
}

//! Which element of the repository row a region belongs to.
enum class EBandSegment : std::uint8_t {
	//! `.icon` plus `IconLabel`, which upstream renders with `supportIcons: false`.
	Name,
	//! One `StatusBarAction` of the row's toolbar.
	Action,
	//! The toolbar's `...`, which opens `scm/title`'s secondary actions.
	Overflow,
};

//! One hoverable region of the repository row.
struct BandSegment final {
	EBandSegment kind{ EBandSegment::Name };
	RECT rect{};
	//! Rendered label runs. Only an action has them: the name is not parsed for
	//! `$(name)`, exactly as upstream's `supportIcons: false` label is not.
	std::vector<icons::SLabelRun> runs;
	//! Empty for the name segment, which carries a title but runs nothing.
	std::string command;
	std::string argumentsJson;
	std::wstring tooltip;
};

//! One clickable action button inside the Source Control empty-state welcome
//! content (`GitScmWelcomeModel::actions`). Mirrors `BandSegment`'s
//! rect/command/argumentsJson hit-test-and-dispatch shape rather than
//! inventing a second interaction model — see this directory's CLAUDE.md.
struct WelcomeSegment final {
	RECT rect{};
	std::wstring label;
	std::string command;
	std::string argumentsJson;
};

//! What the repository row renders, copied out of the published provider so the
//! row, like the list, can only ever show state the service already holds.
struct BandModel final {
	bool visible{};
	//! `RepositoryRenderer`'s label, which is `provider.name`.
	std::wstring name;
	//! `${provider.label}: ${getUriLabel(provider.rootUri)}`, the row's title.
	std::wstring title;
	//! The count badge's text. Empty when the badge is hidden.
	std::wstring count;
	//! `provider.statusBarCommands`, which upstream turns into the row's toolbar.
	std::vector<ScmCommand> commands;

	[[nodiscard]] bool operator==(const BandModel&) const = default;
};

//! What the commit box renders, copied out of the published provider exactly as
//! the repository row is, so the box can never show state the service does not
//! hold. `accept` is upstream's `SourceControl.acceptInputCommand`, which the
//! Git extension sets to `git.commit`.
struct InputModel final {
	bool visible{};
	bool enabled{ true };
	std::wstring placeholder;
	std::wstring value;
	std::optional<ScmCommand> accept;

	[[nodiscard]] bool operator==(const InputModel&) const = default;
};

struct WorkerResult {
	std::uint64_t generation{};
	//! Valid only when `execution` is `Succeeded`; failed status reads leave this
	//! default-constructed so the last published repository snapshot can remain.
	GitScmState state;
	//! Why the refresh ended. An empty change list means "clean" only when this
	//! says the command actually succeeded; otherwise it means "unknown".
	EGitExecutionStatus execution{ EGitExecutionStatus::InvalidRequest };
	//! git's own failure text, kept so a later surface can show a reason instead
	//! of an empty list. UTF-8, already bounded by the runner.
	std::string failureReason;
	//! The Graph's own page of history.  `historyRead` separates "this repository
	//! has no commits" from "the history could not be read", exactly as `execution`
	//! separates a clean worktree from an unknown one.
	std::vector<GitHistoryItem> history;
	bool historyRead{};
};
struct Gate {
	std::mutex mutex;
	HWND window{};
	bool alive{};
	//! Depth-one result mailbox. A slow UI never makes periodic refresh results
	//! accumulate: the worker replaces an unpublished result with the newest one.
	workbench::rendering::LatestOnlyMailbox<std::unique_ptr<WorkerResult>> results;
	//! Exactly one payload-free wakeup may be queued for `results`. The HWND owns
	//! the take; no pointer crosses the Win32 message boundary.
};
struct SharedState {
	std::mutex mutex;
	std::wstring root;
	//! Guarded by `mutex` together with `root`, so a worker cannot observe a new
	//! root paired with the previous generation (or the inverse).
	std::uint64_t generation{ 1 };
	std::shared_ptr<Gate> gate = std::make_shared<Gate>();
	HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

	~SharedState()
	{
		if (stop != nullptr) ::CloseHandle(stop);
		if (wake != nullptr) ::CloseHandle(wake);
	}
};

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.hInstance = instance;
	wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	wc.lpfnWndProc = CScmWorkbenchTool::WindowProc;
	wc.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

//! The refresh query. `--branch` is what carries head/upstream/ahead/behind, and
//! `-z` is what makes a rename's two paths unambiguous even when a path contains
//! a quote or a newline.
GitExecutionRequest MakeStatusRequest(const std::wstring& root)
{
	GitExecutionRequest request;
	request.workingDirectory = root;
	request.arguments = { L"status", L"--porcelain=v2", L"--branch", L"-z", L"--untracked-files=normal" };
	request.policy = EGitRequestPolicy::PassiveRepositoryRead;
	request.timeoutMilliseconds = 3000;
	request.maximumOutputBytes = kMaximumStatusBytes;
	return request;
}

//! The Graph's own query.  It is a second invocation rather than part of the
//! status refresh because `git status` cannot report commits at all.
GitExecutionRequest MakeHistoryRequest(const std::wstring& root)
{
	GitExecutionRequest request;
	request.workingDirectory = root;
	request.arguments = MakeGitHistoryArguments(kGraphHistoryCount);
	request.policy = EGitRequestPolicy::PassiveRepositoryRead;
	request.timeoutMilliseconds = 3000;
	request.maximumOutputBytes = kMaximumStatusBytes;
	return request;
}

//! The built-in Git provider's owner. VS Code's own extension id, because a
//! consumer looking for the Git provider must find this one and not a parallel
//! identity we invented. Generation 1: the tool publishes for the process's
//! lifetime and never re-registers under a new generation.
const ScmOwner& BuiltinGitOwner()
{
	static const ScmOwner owner{ std::string(kGitProviderId), 1 };
	return owner;
}

std::wstring ToWide(std::string_view utf8)
{
	if (utf8.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring wide(static_cast<std::size_t>(length), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
	return wide;
}

//! The inverse, for the one value that travels back out of the view: the typed
//! commit message the publisher owns.
std::string ToUtf8(std::wstring_view text)
{
	if (text.empty()) return {};
	const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		nullptr, 0, nullptr, nullptr);
	if (length <= 0) return {};
	std::string utf8(static_cast<std::size_t>(length), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		utf8.data(), length, nullptr, nullptr);
	return utf8;
}

//! Show a resource the way upstream does: relative to the repository root, since
//! the root is the provider's own label and repeating it on every row would only
//! push the part that differs off the right edge.
std::wstring RelativeDisplayPath(std::wstring_view root, const std::wstring& absolute)
{
	if (root.empty() || absolute.size() <= root.size()) return absolute;
	auto prefix = std::wstring(root);
	if (prefix.back() != L'\\') prefix += L'\\';
	if (absolute.size() <= prefix.size()) return absolute;
	if (::CompareStringOrdinal(absolute.c_str(), static_cast<int>(prefix.size()),
		prefix.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
		return absolute;
	}
	return absolute.substr(prefix.size());
}

void PostResult(const std::shared_ptr<SharedState>& shared, std::unique_ptr<WorkerResult> result)
{
	const auto gate = shared->gate;
	std::lock_guard lock(gate->mutex);
	if (!gate->alive || gate->window == nullptr) return;
	const auto publication = gate->results.Publish(std::move(result));
	if (!publication.wakeRequired) return;
	if (!::PostMessageW(gate->window, kResultMessage, 0, 0)) {
		gate->results.CancelWakeAndDiscard();
	}
}

void WorkerMain(std::shared_ptr<SharedState> shared)
{
	HANDLE waits[] = { shared->stop, shared->wake };
	for (;;) {
		const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kRefreshMilliseconds);
		if (wait == WAIT_OBJECT_0) return;
		std::wstring root;
		std::uint64_t generation = 0;
		{
			std::lock_guard lock(shared->mutex);
			root = shared->root;
			generation = shared->generation;
		}
		if (root.empty()) continue;
		const auto execution = RunGit(MakeStatusRequest(root), shared->stop);
		if (execution.status == EGitExecutionStatus::Cancelled) return;
		auto result = std::make_unique<WorkerResult>();
		result->generation = generation;
		result->execution = execution.status;
		result->failureReason = execution.standardError;
		if (execution.Succeeded()) {
			result->state = ParsePorcelainV2({ reinterpret_cast<const char*>(execution.standardOutput.data()),
				execution.standardOutput.size() });
		}
		// Only a repository has a history to read, and a repository with no commit
		// at all makes `git log` fail; that failure is reported as "unavailable"
		// rather than as an empty history.
		if (execution.Succeeded() && result->state.repository) {
			const auto history = RunGit(MakeHistoryRequest(root), shared->stop);
			if (history.status == EGitExecutionStatus::Cancelled) return;
			if (history.Succeeded()) {
				result->historyRead = true;
				result->history = ParseGitHistory({ reinterpret_cast<const char*>(history.standardOutput.data()),
					history.standardOutput.size() });
			}
		}
		PostResult(shared, std::move(result));
	}
}

//! One rendered line. A group header and a resource are the same row type so the
//! list keeps the published order; only a resource carries a command or a path.
struct ScmRow final {
	bool header{};
	std::wstring label;
	std::wstring text;
	std::wstring windowsPath;
	//! Stable resource identity from SourceControlResourceState. A path is not
	//! sufficient here because another SCM provider can expose a non-file URI.
	std::wstring resourceUri;
	std::wstring tooltip;
	std::string groupKey;
	wchar_t statusLetter{};
	std::size_t resourceCount{};
	bool faded{};
	bool strikeThrough{};
	std::optional<ScmCommand> command;
	//! Which built-in Git group this row belongs to, header rows included. Empty
	//! for a provider we did not publish, whose rows Git's menus must not claim.
	std::optional<EGitResourceGroup> group;
	//! What a resource-scoped command operates on. Empty on a header row, and on
	//! a resource row published by another provider.
	std::optional<GitStageResource> operand;
	[[nodiscard]] bool operator==(const ScmRow&) const = default;
};

//! One inline action drawn on a group header row: upstream's
//! `scm/resourceGroup/context` `inline` group, which it renders as an
//! always-visible action bar rather than as context-menu-only entries.
struct GroupRowAction final {
	RECT rect{};
	//! The bare codicon name, with `$(` and `)` already stripped.
	std::wstring icon;
	std::string command;
	std::wstring tooltip;
};

//! The one list selection that can survive a provider refresh. It names a row
//! by provider group and URI, rather than its transient list index.
struct ScmRowSelection final {
	bool header{};
	std::string groupKey;
	std::wstring resourceUri;
};

//! Upstream's own SCMTreeFilter condition, verbatim:
//! `resources.length > 0 || !hideWhenEmpty`. Reproducing the condition rather
//! than guessing is what makes an empty repository look the way stock VS Code
//! looks instead of showing headers upstream would have hidden.
bool IsVisibleGroup(const ScmResourceGroupState& group) noexcept
{
	return !group.resources.empty() || !group.hideWhenEmpty;
}

} // namespace

struct CScmWorkbenchTool::Impl {
	std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
	std::thread worker;
	std::optional<workbench::WorkerRetirementService::Reservation> workerRetirement;
	HWND window{};
	HWND list{};
	//! Persistent composition targets. Each owner-drawn surface publishes a
	//! completed image with one blit instead of exposing background/text phases.
	workbench::rendering::CGdiBackBuffer windowBuffer;
	workbench::rendering::CGdiBackBuffer listBuffer;
	workbench::rendering::CGdiBackBuffer graphBuffer;
	workbench::rendering::CGdiBackBuffer inputBuffer;
	//! Optional physical-Part bridge for the retained Changes list. The bridge
	//! owns only CPU staging and an asynchronous sink; it never owns the HWND.
	ScmNativeSurfacePayloadAdapter nativeSurface;
	//! The VS Code-style overlay scrollbar shared with the Explorer view. The
	//! list keeps its WS_VSCROLL scroll state; the overlay hides the platform bar
	//! and draws the themed one over the same rows.
	controls::COverlayScrollbar listScrollbar;
	//! The commit message box. Upstream renders the SCM input directly under the
	//! repository row and above the resource groups, and so does this.
	HWND input{};
	InputModel inputModel;
	//! Rendered lines the box is currently sized for, already clamped to
	//! `scm.inputMinLineCount` / `scm.inputMaxLineCount`.
	int inputLineCount{ kScmInputMinLineCountDefault };
	//! Effective `scm.inputMinLineCount` / `scm.inputMaxLineCount`.
	int inputMinLineCount{ kScmInputMinLineCountDefault };
	int inputMaxLineCount{ kScmInputMaxLineCountDefault };
	//! One rendered line's height at the current font and DPI.
	int inputLineHeight{};
	//! The authoritative EDIT formatting rectangle established by LayoutInput.
	//! Placeholder composition must not query EM_GETRECT during WM_PAINT: the
	//! native EDIT can publish its internal rectangle a frame after WM_SIZE.
	RECT inputFormattingRect{};
	//! Set while the tool itself writes the box, so the resulting `EN_CHANGE` is
	//! not mistaken for the user typing and published back as a new value.
	bool updatingInput{};
	unsigned int dpi{ 96 };
	std::wstring root;
	GitScmState state;
	//! The last refresh's terminal state, kept separate from `state` so an empty
	//! change list is never mistaken for a clean worktree after a failed refresh.
	EGitExecutionStatus execution{ EGitExecutionStatus::InvalidRequest };
	std::string failureReason;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont font;
	FileActivationCallback activateFile;
	StatusBarCommandsCallback statusBarCommands;
	FileDecorationsCallback fileDecorations;
	//! The decorations of the last render, kept so a late consumer sees them too.
	std::vector<GitResourceDecoration> publishedDecorations;
	CommandCallback runCommand;
	TextResolver text;
	PublicationTextResolver publicationText;
	//! Borrowed, never owned. Null until the runtime hands its service over, and
	//! the tool must keep working without one.
	SourceControlService* service{};
	//! Owns the built-in Git provider handle for as long as this tool lives.
	std::unique_ptr<GitScmPublisher> publisher;
	//! What the list actually renders. Derived from published provider state, so
	//! the view never reads `state` directly and cannot disagree with the service.
	std::vector<ScmRow> rows;
	std::unordered_set<std::string> collapsedGroups;
	int listHoverIndex{ -1 };
	//! Where the pointer last was inside the change list, in its client
	//! coordinates. A group row's inline action highlights from this rather than
	//! from a second hit-test pass during paint.
	POINT listPointer{};
	int listPointerDownIndex{ -1 };
	bool trackingListMouse{};
	//! Resources published by every provider, group headers included.
	std::size_t resourceCount{};
	//! Providers currently published. This is upstream's `gitOpenRepositoryCount`
	//! and is read from the published snapshot, never from `state.repository`,
	//! so the command gate and the rendered view can never disagree.
	std::size_t openRepositoryCount{};
	//! The repository row. Upstream renders it only when more than one repository
	//! is visible or `scm.alwaysShowRepositories` is set; see this directory's
	//! CLAUDE.md for why that setting is hard-coded on here.
	BandModel band;
	//! `workbench.scm.history` has no provider snapshot yet.  Its typed status
	//! makes the visible Graph frame an explicit unsupported boundary, not a
	//! partial Git-history implementation.
	ScmGraphPresentation graphPresentation;
	//! The Graph's own owner-drawn list and overlay scrollbar. It is a separate
	//! control from the change list because the two are separate upstream Views
	//! with separate scroll positions.
	HWND graphList{};
	controls::COverlayScrollbar graphScrollbar;
	GraphNodePainter graphNodePainter;
	std::vector<GitHistoryItem> history;
	//! `toISCMHistoryItemViewModelArray`'s output, one entry per `history` entry.
	std::vector<ScmGraphRow> graphRows;
	//! The Graph body's height in DIP, so a sash drag survives a DPI change. The
	//! layout model clamps it against the Changes body's own minimum.
	int graphBodyDip{ kGraphDefaultBodyHeightDip };
	//! Upstream's collapsible panes. A collapsed section keeps its header.
	bool repositoriesCollapsed{};
	bool changesCollapsed{};
	bool graphCollapsed{};
	bool draggingSash{};
	//! Layout is performed as one transaction while the sash moves. Child
	//! windows must not synchronously repaint after each MoveWindow call, or the
	//! old and new stack geometry becomes visible as alternating blank frames.
	bool deferChildRepaint{};
	//! Set only when a visible list actually received WM_SETREDRAW(FALSE). A
	//! hidden window must never receive the matching TRUE: DefWindowProc would
	//! add WS_VISIBLE and resurrect a page its ViewContainer deliberately hid.
	bool listRedrawDeferred{};
	bool graphRedrawDeferred{};
	//! Where the drag started, and the height it started from, so the drag is
	//! absolute against its own origin rather than accumulating rounding error.
	int sashDragOriginY{};
	int sashDragOriginDip{};
	std::vector<BandSegment> bandSegments;
	RECT bandCountRect{};
	HWND tooltip{};
	//! Held for as long as the tooltip may read the pointer we hand it back.
	std::wstring tooltipText;
	//! Highest tool id added to `tooltip`, so a rebuild removes exactly its own.
	std::size_t tooltipToolCount{};
	//! `bandSegments` index plus one, or zero when the pointer is over none.
	std::size_t hoveredSegment{};
	//! Left edge of the Graph header's toolbar, or zero when it has none.
	LONG graphHeaderActionsLeft{};
	bool trackingMouse{};
	bool active{};
	bool closed{};
	//! Explicit workspace kind projected by the composition root. It is kept
	//! separately from the provider snapshot because a workspace can have zero
	//! folders or folders without repositories.
	EGitScmWelcomeWorkspaceState welcomeWorkspaceState{EGitScmWelcomeWorkspaceState::Empty};
	//! What the Source Control empty state currently shows. Recomputed from the
	//! explicit workspace kind and `openRepositoryCount` every
	//! `PublishAndRender()`, never read directly from `GitScmState` — the same
	//! "derive, don't duplicate" rule `band` and `inputModel` already follow.
	GitScmWelcomeModel welcomeModel;
	//! `ISCMProvider.actionButton`: the Commit split button upstream renders
	//! directly under the commit box. Nothing when the provider contributes none,
	//! which is upstream's own "no button" state rather than a disabled one.
	std::optional<GitActionButton> actionButton;
	RECT actionButtonPrimaryRect{};
	RECT actionButtonDropdownRect{};
	//! 0 none, 1 the primary half, 2 the dropdown half.
	int hoveredActionButtonPart{};
	std::vector<WelcomeSegment> welcomeSegments;
	RECT welcomeMessageRect{};
	//! `welcomeSegments` index plus one, or zero when the pointer is over none.
	std::size_t hoveredWelcomeSegment{};

	void Start() {
		if (worker.joinable()) return;
		auto retirement = workbench::WorkerRetirementService::Instance().TryReserve();
		if (!retirement) return;
		worker = std::thread(WorkerMain, shared);
		workerRetirement.emplace(std::move(*retirement));
	}
	void NotifyWindow(HWND target, bool alive) {
		std::lock_guard lock(shared->gate->mutex);
		shared->gate->window = target;
		shared->gate->alive = alive;
		if (alive) shared->gate->results.Open();
		else shared->gate->results.Close();
	}
	std::unique_ptr<WorkerResult> TakeLatest() {
		std::lock_guard lock(shared->gate->mutex);
		auto value = shared->gate->results.Take();
		return value ? std::move(*value) : nullptr;
	}
	//! Apply the current Git state to the service, then re-render from what the
	//! service holds. Publishing and rendering are one step so the list can never
	//! show a generation the service has already replaced.
	/*!
		@brief Hands the last published badges to the decorations consumer.

		The projection runs here rather than in the consumer so the URI-to-path
		conversion stays beside the URIs that were published, and so a resource whose
		URI has no native path is dropped once instead of by every consumer.
	*/
	void PublishFileDecorations() const {
		if (!fileDecorations) return;
		fileDecorations(BuildGitFileDecorationEntries(publishedDecorations));
	}
	void PublishAndRender() {
		const auto selected = CaptureListSelection();
		std::vector<ScmProviderState> providers;
		std::vector<GitResourceDecoration> decorations;
		std::vector<GitResourceOperand> operands;
		if (publisher) {
			// A non-repository root must retract rather than publish an empty
			// provider: "no repository here" and "a repository with no changes"
			// are different facts and must not render the same.
			if (state.repository) {
				(void)publisher->Publish(root, state, EUntrackedChangesPolicy::Mixed, publicationText);
				decorations = publisher->Decorations();
				operands = publisher->Operands();
			} else {
				(void)publisher->Retract();
			}
		}
		if (service) {
			auto snapshot = service->Snapshot();
			providers = std::move(snapshot.providers);
		} else if (state.repository) {
			// Without a service there is still exactly one way to shape a row.
			// Building the same publication locally keeps that single renderer
			// instead of growing a second, drifting one for this configuration.
			auto publication = BuildGitPublication(
				BuiltinGitOwner(), root, state, EUntrackedChangesPolicy::Mixed, publicationText);
			// With no service there is no publisher to own the typed message across
			// refreshes, so the view carries it exactly as the publisher would.
			// Otherwise every five-second refresh would erase what the user typed.
			publication.provider.inputBox.value = ToUtf8(inputModel.value);
			providers.push_back(std::move(publication.provider));
			decorations = std::move(publication.decorations);
			operands = std::move(publication.operands);
		}
		openRepositoryCount = providers.size();
		RebuildWelcome();
		const bool rowsChanged = RebuildRows(providers, decorations, operands);
		RebuildBand(providers);
		RebuildInput(providers);
		RebuildActionButton(providers);
		if (rowsChanged) Populate(selected);
		PublishStatusBarCommands(providers);
		publishedDecorations = std::move(decorations);
		PublishFileDecorations();
	}
	//! Keep sibling HWNDs off the compositor while one worker result replaces the
	//! Changes and Graph projections. This is a UI-thread presentation boundary,
	//! not a worker lock: no thread waits for another thread and no Git work runs
	//! inside it.
	void BeginWorkerPresentation()
	{
		deferChildRepaint = true;
		listRedrawDeferred = false;
		graphRedrawDeferred = false;
	}
	void EndWorkerPresentation()
	{
		if (listRedrawDeferred && list != nullptr) ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		if (graphRedrawDeferred && graphList != nullptr) ::SendMessageW(graphList, WM_SETREDRAW, TRUE, 0);
		listRedrawDeferred = false;
		graphRedrawDeferred = false;
		deferChildRepaint = false;
		if (window != nullptr) {
			::RedrawWindow(window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		}
	}
	class WorkerPresentationScope final {
	public:
		explicit WorkerPresentationScope(Impl& owner) noexcept
			: m_owner(owner), m_ownsPresentation(!owner.deferChildRepaint)
		{
			if (m_ownsPresentation) m_owner.BeginWorkerPresentation();
		}
		~WorkerPresentationScope() noexcept
		{
			if (m_ownsPresentation) m_owner.EndWorkerPresentation();
		}
		WorkerPresentationScope(const WorkerPresentationScope&) = delete;
		WorkerPresentationScope& operator=(const WorkerPresentationScope&) = delete;
	private:
		Impl& m_owner;
		bool m_ownsPresentation{};
	};
	//! Apply one immutable worker result. Valid status snapshots become one visible
	//! SCM revision; a failed status only records diagnostics. Identical periodic
	//! results are discarded before touching the service, controls, decorations,
	//! status bar, or invalidation state.
	void ApplyWorkerResult(std::unique_ptr<WorkerResult> result)
	{
		if (!result) return;
		// `state` and the provider are the last successfully read repository
		// snapshot. A failed status command does not produce a replacement
		// snapshot: its default-constructed state is only the worker's empty
		// payload. In particular, treating that payload as a real state would
		// retract the provider during a timeout, launch failure, or an
		// unavailable/over-limit git invocation. This is also why an unattempted
		// history query must not clear the last graph page on this path.
		const bool statusSucceeded = ClassifyGitStatusRefresh(result->execution)
			== EGitStatusRefreshDisposition::ApplySnapshot;
		const bool stateChanged = statusSucceeded && state != result->state;
		const bool diagnosticsChanged = execution != result->execution
			|| failureReason != result->failureReason;
		const bool historyChanged = statusSucceeded && (result->historyRead
			? graphPresentation.status != EScmGraphPresentationStatus::Available
				|| history != result->history
			: graphPresentation.status != EScmGraphPresentationStatus::Unavailable
				|| !history.empty());
		if (!stateChanged && !historyChanged) {
			if (diagnosticsChanged) {
				execution = result->execution;
				failureReason = std::move(result->failureReason);
			}
			return;
		}

		WorkerPresentationScope presentation(*this);
		if (diagnosticsChanged) {
			execution = result->execution;
			failureReason = std::move(result->failureReason);
		}
		if (stateChanged) {
			state = std::move(result->state);
			PublishAndRender();
		}
		if (historyChanged) ApplyHistory(std::move(result->history), result->historyRead);
	}
	//! Drop the old repository's authoritative presentation before a new root's
	//! first status result arrives. The next result is generation-fenced, but an
	//! unknown refresh must not leave the previous root's provider or Graph visible.
	void ResetForRootChange()
	{
		WorkerPresentationScope presentation(*this);
		state = {};
		execution = EGitExecutionStatus::InvalidRequest;
		failureReason.clear();
		PublishAndRender();
		// A Git history page belongs to the previous root just as its provider does;
		// provider disposal also clears the typed commit input through RebuildInput.
		ApplyHistory({}, false);
	}
	[[nodiscard]] int BandHeight() const noexcept
	{
		return band.visible ? icons::ScaleDip(kRepositoryRowHeightDip, dpi) : 0;
	}
	//! VS Code merges a sole Changes view into the Source Control container. The
	//! Git welcome variants appear only with no provider, so the container title
	//! is the only header in that state.
	[[nodiscard]] bool ChangesHeaderVisible() const noexcept
	{
		return welcomeModel.content == EGitScmWelcomeContent::None;
	}
	[[nodiscard]] bool GraphFrameVisible() const noexcept
	{
		return graphPresentation.ShouldRenderFrameForProvider(band.visible);
	}
	//! The body the user last dragged the sash to, scaled for the current DPI and
	//! never smaller than one usable row.
	[[nodiscard]] int GraphBodyHeight() const noexcept
	{
		return icons::ScaleDip(std::max(kGraphMinimumBodyHeightDip, graphBodyDip), dpi);
	}
	//! The box's own height, auto-grown between upstream's two line-count bounds.
	[[nodiscard]] int InputHeight() const noexcept
	{
		if (!inputModel.visible) return 0;
		const int lines = std::clamp(inputLineCount, inputMinLineCount, inputMaxLineCount);
		return lines * std::max(1, inputLineHeight) + 2 * icons::ScaleDip(kInputPaddingDip, dpi);
	}
	[[nodiscard]] ScmViewStackLayout ViewStack() const
	{
		RECT client{};
		if (window) ::GetClientRect(window, &client);
		return BuildScmViewStackLayout({
			.clientTop = client.top,
			.clientBottom = client.bottom,
			.viewHeaderHeight = icons::ScaleDip(kScmViewHeaderHeightDip, dpi),
			.repositoryRowHeight = BandHeight(),
			.inputOuterMargin = icons::ScaleDip(kInputOuterMarginDip, dpi),
			.inputHeight = InputHeight(),
			.actionButtonHeight = ActionButtonHeight(),
			.graphBodyHeight = GraphBodyHeight(),
			.sashHeight = icons::ScaleDip(kSashHeightDip, dpi),
			.minimumBodyHeight = icons::ScaleDip(kChangesMinimumBodyHeightDip, dpi),
			.repositoriesVisible = band.visible,
			.repositoriesCollapsed = repositoriesCollapsed,
			.changesCollapsed = changesCollapsed,
			.graphCollapsed = graphCollapsed,
			.changesHeaderVisible = ChangesHeaderVisible(),
			.inputVisible = inputModel.visible,
			.actionButtonVisible = actionButton.has_value(),
			.graphVisible = GraphFrameVisible(),
		});
	}
	[[nodiscard]] RECT ViewBounds(const ScmVerticalBounds& vertical) const
	{
		RECT client{};
		if (window) ::GetClientRect(window, &client);
		return RECT{ client.left, vertical.top, client.right, vertical.bottom };
	}
	[[nodiscard]] RECT RepositoriesHeaderBounds() const
	{
		return ViewBounds(ViewStack().repositoriesHeader);
	}
	[[nodiscard]] RECT ChangesHeaderBounds() const
	{
		return ViewBounds(ViewStack().changesHeader);
	}
	[[nodiscard]] RECT GraphHeaderBounds() const
	{
		return ViewBounds(ViewStack().graphHeader);
	}
	[[nodiscard]] RECT GraphBodyBounds() const
	{
		return ViewBounds(ViewStack().graphBody);
	}
	[[nodiscard]] RECT InputBounds() const
	{
		RECT client{};
		if (window) ::GetClientRect(window, &client);
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const auto layout = ViewStack();
		const LONG left = client.left + inset;
		return RECT{ left, layout.input.top, std::max(left, client.right - inset), layout.input.bottom };
	}
	//! The Changes list starts after its own header and input.  The Graph frame is
	//! reserved from its lower edge by `ViewStack`, so neither the list nor the
	//! empty-state welcome content can paint through it.
	[[nodiscard]] int ListTop() const noexcept
	{
		return ViewStack().changesBody.top;
	}
	void MeasureInputLineHeight()
	{
		if (!window) return;
		const HDC dc = ::GetDC(window);
		if (dc == nullptr) return;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
		TEXTMETRICW metrics{};
		if (::GetTextMetricsW(dc, &metrics) != FALSE) inputLineHeight = static_cast<int>(metrics.tmHeight);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::ReleaseDC(window, dc);
	}
	void LayoutInput()
	{
		if (!window || !input) return;
		MeasureInputLineHeight();
		::ShowWindow(input, inputModel.visible ? SW_SHOW : SW_HIDE);
		if (!inputModel.visible) return;
		const RECT bounds = InputBounds();
		const LONG width = std::max(0L, bounds.right - bounds.left);
		const LONG height = std::max(0L, bounds.bottom - bounds.top);
		::SetWindowPos(input, nullptr, bounds.left, bounds.top, width, height,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
		// A multiline edit has no vertical margin message, so the padding is the
		// formatting rectangle; without it the first line would hug the border.
		const LONG pad = icons::ScaleDip(kInputPaddingDip, dpi);
		RECT formatting{ pad, pad, std::max(pad, width - pad), std::max(pad, height - pad) };
		::SendMessageW(input, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
		inputFormattingRect = formatting;
	}
	//! Re-measure how many rendered lines the box holds and resize if it changed.
	void UpdateInputHeight()
	{
		if (!input) return;
		// A wrapping multiline edit counts wrapped lines here, which is the same
		// content height upstream's word-wrapped editor reports.
		const auto lines = static_cast<int>(::SendMessageW(input, EM_GETLINECOUNT, 0, 0));
		const int clamped = std::clamp(std::max(1, lines), inputMinLineCount, inputMaxLineCount);
		if (clamped == inputLineCount) return;
		inputLineCount = clamped;
		LayoutInput();
		LayoutActionButton();
		LayoutList();
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	[[nodiscard]] std::wstring ReadInputText() const
	{
		if (!input) return {};
		const int length = ::GetWindowTextLengthW(input);
		if (length <= 0) return {};
		std::wstring value(static_cast<std::size_t>(length), L'\0');
		const int copied = ::GetWindowTextW(input, value.data(), length + 1);
		value.resize(static_cast<std::size_t>(std::max(0, copied)));
		return value;
	}
	//! Set the box's text without letting the resulting `EN_CHANGE` echo back.
	void WriteInputText(const std::wstring& value)
	{
		if (!input) return;
		updatingInput = true;
		::SetWindowTextW(input, value.c_str());
		updatingInput = false;
		::InvalidateRect(input, nullptr, FALSE);
	}
	void OnInputChanged()
	{
		if (!input || updatingInput) return;
		const bool wasEmpty = inputModel.value.empty();
		inputModel.value = ReadInputText();
		// The publisher owns the typed message across provider replacements. Going
		// through the service snapshot instead would deep-copy every provider on
		// every keystroke, which is why nothing here re-renders.
		if (publisher) (void)publisher->SetInputBoxValue(ToUtf8(inputModel.value));
		// The placeholder is painted only while the box is empty, so only crossing
		// that boundary needs a repaint.
		if (wasEmpty != inputModel.value.empty()) ::InvalidateRect(input, nullptr, FALSE);
		UpdateInputHeight();
	}
	//! Upstream's `scm.acceptInput`: run the provider's own `acceptInputCommand`.
	//! A disabled box accepts nothing, exactly as upstream's does.
	void AcceptInput()
	{
		if (!inputModel.enabled || !inputModel.accept || !runCommand) return;
		(void)runCommand(inputModel.accept->command, inputModel.accept->argumentsJson);
	}
	//! The commit box's half of `RebuildBand`, reading the same published state.
	void RebuildInput(const std::vector<ScmProviderState>& providers)
	{
		const auto previous = inputModel;
		InputModel next;
		if (!providers.empty()) {
			const auto& provider = providers.front();
			next.visible = provider.inputBox.visible;
			next.enabled = provider.inputBox.enabled;
			next.placeholder = ToWide(provider.inputBox.placeholder);
			next.value = ToWide(provider.inputBox.value);
			next.accept = provider.acceptInputCommand;
		}
		if (next == previous) return;
		inputModel = std::move(next);
		if (input) {
			// Only when the authority's value really changed: `SetWindowText` drops
			// the selection and moves the caret to the start, which would fight the
			// user on every refresh while they are still typing.
			if (inputModel.value != previous.value) WriteInputText(inputModel.value);
			::EnableWindow(input, inputModel.enabled ? TRUE : FALSE);
			UpdateInputHeight();
		}
		LayoutInput();
		LayoutActionButton();
		LayoutList();
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	//! The button's own box, which is `.monaco-text-button` exactly as the
	//! ViewWelcome buttons are, so the two cannot disagree about its height.
	[[nodiscard]] int ActionButtonHeight() const noexcept
	{
		return actionButton ? views::WelcomeButtonHeight(dpi) : 0;
	}
	//! Full width between the same insets the commit box uses, because upstream's
	//! button is a block element inside the same `.scm-editor` container.
	[[nodiscard]] RECT ActionButtonBounds() const
	{
		RECT client{};
		if (window) ::GetClientRect(window, &client);
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const auto layout = ViewStack();
		const LONG left = client.left + inset;
		return RECT{ left, layout.actionButton.top, std::max(left, client.right - inset),
			layout.actionButton.bottom };
	}
	//! Split the box into upstream's two halves once, so a hit test never depends
	//! on a paint having happened.
	void LayoutActionButton()
	{
		actionButtonPrimaryRect = RECT{};
		actionButtonDropdownRect = RECT{};
		if (!window || !actionButton) return;
		const RECT bounds = ActionButtonBounds();
		if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
		// With no secondary command there is no dropdown half at all, exactly as
		// upstream renders a plain `Button` instead of a `ButtonWithDropdown`.
		const LONG dropdown = actionButton->secondaryCommands.empty()
			? 0 : icons::ScaleDip(kActionButtonDropdownDip, dpi);
		const LONG split = std::max(bounds.left, bounds.right - dropdown);
		actionButtonPrimaryRect = RECT{ bounds.left, bounds.top, split, bounds.bottom };
		if (dropdown > 0) {
			actionButtonDropdownRect = RECT{ split, bounds.top, bounds.right, bounds.bottom };
		}
	}
	//! Recompute the button from the same published providers the commit box and
	//! the repository row read, so the three can never describe different state.
	void RebuildActionButton(const std::vector<ScmProviderState>& providers)
	{
		const auto previous = actionButton;
		std::optional<GitActionButton> next;
		if (!providers.empty()) {
			std::size_t resources = 0;
			for (const auto& group : providers.front().groups) resources += group.resources.size();
			next = BuildGitCommitActionButton(resources != 0, inputModel.enabled);
			LocalizeActionButton(next);
		}
		if (next == previous) return;
		actionButton = std::move(next);
		hoveredActionButtonPart = 0;
		LayoutActionButton();
		// The button occupies its own band in the View stack, so appearing or
		// disappearing moves the list and the welcome content with it.
		LayoutList();
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	//! Replace the model's upstream English with the running language's own
	//! strings. The model stays the authority on which rows exist and in what
	//! order; only the rendered titles are resolved here, exactly as the resource
	//! context menu does it.
	void LocalizeActionButton(std::optional<GitActionButton>& button) const
	{
		if (!button || !text) return;
		const auto resolve = [this](EScmTextKey key, std::wstring& title) {
			if (std::wstring localized = text(key, {}); !localized.empty()) title = std::move(localized);
		};
		// The icon prefix is `renderLabelWithIcons` syntax, not text: it survives
		// translation and only the label after it is replaced.
		std::wstring label = L"Commit";
		resolve(EScmTextKey::GitCommitAction, label);
		button->title = L"$(check) " + label;
		for (auto& item : button->secondaryCommands) {
			if (item.commandId == "git.commit" && item.argumentsJson == "[]") {
				resolve(EScmTextKey::GitCommitAction, item.title);
			}
			else if (item.commandId == "git.commitAmend") {
				resolve(EScmTextKey::GitCommitAmendAction, item.title);
			}
			else if (item.commandId == "git.commit" && item.argumentsJson == R"(["git.push"])") {
				resolve(EScmTextKey::GitCommitAndPushAction, item.title);
			}
			else if (item.commandId == "git.commit" && item.argumentsJson == R"(["git.sync"])") {
				resolve(EScmTextKey::GitCommitAndSyncAction, item.title);
			}
		}
	}
	void PaintActionButton(HDC dc)
	{
		if (!actionButton) return;
		if (actionButtonPrimaryRect.right <= actionButtonPrimaryRect.left) return;
		const int radius = views::WelcomeButtonCornerRadius(dpi);
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);
		const RECT bounds{ actionButtonPrimaryRect.left, actionButtonPrimaryRect.top,
			actionButtonDropdownRect.right > actionButtonDropdownRect.left
				? actionButtonDropdownRect.right : actionButtonPrimaryRect.right,
			actionButtonPrimaryRect.bottom };
		// A disabled button keeps upstream's own dimmed look by staying on the base
		// background and dropping to the description colour; it is never hidden,
		// because upstream still shows what the command would be.
		const bool enabled = actionButton->enabled;
		const COLORREF background =
			(enabled && hoveredActionButtonPart != 0 ? palette.buttonHoverBackground
				: palette.buttonBackground).ToColorRef();
		const HBRUSH brush = ::CreateSolidBrush(background);
		const HPEN border = ::CreatePen(PS_SOLID, 1, background);
		if (brush != nullptr && border != nullptr) {
			const HGDIOBJ previousBrush = ::SelectObject(dc, brush);
			const HGDIOBJ previousPen = ::SelectObject(dc, border);
			::RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
			::SelectObject(dc, previousPen);
			::SelectObject(dc, previousBrush);
		}
		if (border != nullptr) ::DeleteObject(border);
		if (brush != nullptr) ::DeleteObject(brush);

		const COLORREF foreground =
			(enabled ? palette.buttonForeground : palette.descriptionText).ToColorRef();
		auto runs = ParseRuns(actionButton->title);
		const int width = icons::MeasureLabelRuns(dc, runs, iconSide);
		const LONG centre = (actionButtonPrimaryRect.left + actionButtonPrimaryRect.right) / 2;
		RECT label{ std::max(actionButtonPrimaryRect.left, centre - width / 2), actionButtonPrimaryRect.top,
			actionButtonPrimaryRect.right, actionButtonPrimaryRect.bottom };
		::SetTextColor(dc, foreground);
		icons::DrawLabelRuns(dc, runs, label, iconSide, foreground, GlyphFonts());
		if (actionButtonDropdownRect.right <= actionButtonDropdownRect.left) return;
		// `.monaco-button-dropdown-separator`: a hairline in the button's own
		// foreground, inset from both ends.
		const LONG inset = icons::ScaleDip(4, dpi);
		const HPEN separator = ::CreatePen(PS_SOLID, 1, foreground);
		if (separator != nullptr) {
			const HGDIOBJ previousPen = ::SelectObject(dc, separator);
			::MoveToEx(dc, actionButtonDropdownRect.left, actionButtonDropdownRect.top + inset, nullptr);
			::LineTo(dc, actionButtonDropdownRect.left, actionButtonDropdownRect.bottom - inset);
			::SelectObject(dc, previousPen);
			::DeleteObject(separator);
		}
		auto chevron = ParseRuns(L"$(chevron-down)");
		const int chevronWidth = icons::MeasureLabelRuns(dc, chevron, iconSide);
		const LONG chevronCentre = (actionButtonDropdownRect.left + actionButtonDropdownRect.right) / 2;
		RECT chevronRect{ std::max(actionButtonDropdownRect.left, chevronCentre - chevronWidth / 2),
			actionButtonDropdownRect.top, actionButtonDropdownRect.right, actionButtonDropdownRect.bottom };
		icons::DrawLabelRuns(dc, chevron, chevronRect, iconSide, foreground, GlyphFonts());
	}
	[[nodiscard]] int ActionButtonPartAt(POINT point) const
	{
		if (!actionButton) return 0;
		if (::PtInRect(&actionButtonPrimaryRect, point)) return 1;
		if (::PtInRect(&actionButtonDropdownRect, point)) return 2;
		return 0;
	}
	void SetHoveredActionButtonPart(int part)
	{
		if (hoveredActionButtonPart == part) return;
		hoveredActionButtonPart = part;
		if (!window) return;
		RECT bounds = ActionButtonBounds();
		::InvalidateRect(window, &bounds, FALSE);
	}
	//! The dropdown half's menu: upstream's `secondaryCommands`, run through the
	//! same command route the primary half uses.
	void ShowActionButtonMenu()
	{
		if (!window || !actionButton || actionButton->secondaryCommands.empty()) return;
		// A copy, because `TrackPopupMenu` pumps messages and a refresh landing
		// while the menu is open can replace the button under it.
		const auto items = actionButton->secondaryCommands;
		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		for (std::size_t position = 0; position < items.size(); ++position) {
			if (items[position].separator) {
				::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				continue;
			}
			::AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(position + 1),
				items[position].title.c_str());
		}
		// Upstream drops the menu from the button's lower edge, right-aligned with
		// the dropdown half it belongs to.
		POINT anchor{ actionButtonDropdownRect.right, actionButtonDropdownRect.bottom };
		::ClientToScreen(window, &anchor);
		::SetForegroundWindow(window);
		const auto chosen = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
			anchor.x, anchor.y, 0, window, nullptr);
		::DestroyMenu(menu);
		::PostMessageW(window, WM_NULL, 0, 0);
		if (chosen <= 0 || static_cast<std::size_t>(chosen) > items.size()) return;
		const auto& item = items[static_cast<std::size_t>(chosen) - 1];
		if (item.commandId.empty() || !runCommand) return;
		(void)runCommand(item.commandId, item.argumentsJson);
	}
	bool InvokeActionButtonAt(POINT point)
	{
		const int part = ActionButtonPartAt(point);
		if (part == 0) return false;
		// A disabled button consumes the click without running anything, which is
		// what upstream's disabled button does too.
		if (!actionButton->enabled) return true;
		if (part == 2) { ShowActionButtonMenu(); return true; }
		if (actionButton->commandId.empty() || !runCommand) return true;
		(void)runCommand(actionButton->commandId, {});
		return true;
	}
	//! The box's themed border. The control itself has no `WS_BORDER`, because a
	//! non-client frame is drawn in system colors and would ignore the theme.
	void PaintInputFrame(HDC dc)
	{
		if (!input || !inputModel.visible) return;
		RECT frame = InputBounds();
		::InflateRect(&frame, 1, 1);
		const HBRUSH brush = ::CreateSolidBrush(palette.inputBorder.ToColorRef());
		if (brush == nullptr) return;
		::FrameRect(dc, &frame, brush);
		::DeleteObject(brush);
	}
	[[nodiscard]] RECT BandBounds() const
	{
		return ViewBounds(ViewStack().repositoryRow);
	}
	//! A wheel that lands on the SCM frame itself has no native child target:
	//! headers, collapsed bodies, and the unavailable Graph message are all
	//! painted by the frame.  Route scrollable regions to their owning list and
	//! consume every other wheel here so DefWindowProc cannot bounce the message
	//! back to the editor frame.
	void HandleMouseWheel(WPARAM wParam, LPARAM lParam)
	{
		if (!window) return;
		POINT point{
			GET_X_LPARAM(lParam),
			GET_Y_LPARAM(lParam),
		};
		if (!::ScreenToClient(window, &point)) return;

		const auto routeToList = [&](HWND target) {
			if (target == nullptr || ::IsWindowVisible(target) == FALSE) return;
			::SendMessageW(target, WM_MOUSEWHEEL, wParam, lParam);
		};
		const auto layout = ViewStack();
		const RECT graphHeader = ViewBounds(layout.graphHeader);
		const RECT graphBody = ViewBounds(layout.graphBody);
		const bool inGraph = GraphFrameVisible()
			&& (::PtInRect(&graphHeader, point) != FALSE
				|| ::PtInRect(&graphBody, point) != FALSE);
		if (inGraph) {
			routeToList(graphList);
			return;
		}

		const RECT changesHeader = ViewBounds(layout.changesHeader);
		const RECT changesBody = ViewBounds(layout.changesBody);
		const bool inChanges = ::PtInRect(&changesHeader, point) != FALSE
			|| ::PtInRect(&changesBody, point) != FALSE;
		if (inChanges) routeToList(list);
	}
	void LayoutList()
	{
		if (!window || !list) return;
		const RECT bounds = ViewBounds(ViewStack().changesBody);
		// A collapsed section owns no body at all, so the control is hidden rather
		// than moved to a zero-height rectangle it would still paint a border into.
		const bool visible = !changesCollapsed && bounds.bottom > bounds.top
			&& welcomeModel.content == EGitScmWelcomeContent::None;
		::ShowWindow(list, visible ? SW_SHOW : SW_HIDE);
		if (visible) {
			::SetWindowPos(list, nullptr, bounds.left, bounds.top,
				std::max(0L, bounds.right - bounds.left),
				std::max(0L, bounds.bottom - bounds.top),
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
		}
		UpdateListScrollbar();
		LayoutGraphList();
	}
	void LayoutGraphList()
	{
		if (!window || !graphList) return;
		const RECT bounds = GraphBodyBounds();
		const bool visible = GraphFrameVisible() && !graphCollapsed
			&& graphPresentation.status == EScmGraphPresentationStatus::Available
			&& bounds.bottom > bounds.top;
		::ShowWindow(graphList, visible ? SW_SHOW : SW_HIDE);
		if (visible) {
			::SetWindowPos(graphList, nullptr, bounds.left, bounds.top,
				std::max(0L, bounds.right - bounds.left),
				std::max(0L, bounds.bottom - bounds.top),
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
		}
		UpdateGraphScrollbar();
	}
	void UpdateGraphScrollbar()
	{
		graphScrollbar.SetDpi(dpi);
		graphScrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette, palette.sideBar));
		graphScrollbar.SetScrollModel(ListOverlayModel(graphList));
		graphScrollbar.Update();
	}
	//! Feeds the overlay the current DPI and the Side Bar tokens the Explorer maps
	//! its own overlay from, then lets it reposition itself over the list.
	void UpdateListScrollbar()
	{
		listScrollbar.SetDpi(dpi);
		listScrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette, palette.sideBar));
		listScrollbar.SetScrollModel(ListOverlayModel(list));
		listScrollbar.Update();
	}
	void InvalidateListRow(int index) const
	{
		if (list == nullptr || index < 0) return;
		RECT bounds{};
		if (::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
			reinterpret_cast<LPARAM>(&bounds)) == LB_ERR) return;
		::InvalidateRect(list, &bounds, FALSE);
	}
	//! Upstream's `RepositoryRenderer.renderElement`, reading the same fields.
	void RebuildBand(const std::vector<ScmProviderState>& providers)
	{
		const auto previous = band;
		band = {};
		if (!providers.empty()) {
			// Exactly one repository is visible here, so upstream's `repoSelected`
			// icon (multiple repositories only) and its disambiguating description
			// (same name or same root as another repository) both stay absent.
			const auto& provider = providers.front();
			band.visible = true;
			band.name = ToWide(provider.Name());
			band.title = ToWide(provider.label);
			if (provider.rootUri) {
				const auto path = provider.rootUri->ToWindowsPath();
				band.title += L": ";
				band.title += path.value.value_or(provider.rootUri->ToString());
			}
			std::size_t resources = 0;
			for (const auto& group : provider.groups) resources += group.resources.size();
			// Upstream: `provider.count ?? getRepositoryResourceCount(provider)`.
			const auto count = provider.count.has_value()
				? static_cast<std::int64_t>(*provider.count)
				: static_cast<std::int64_t>(resources);
			if (ShowsProviderCountBadge(count)) band.count = std::to_wstring(count);
			band.commands = provider.statusBarCommands;
		}
		if (band == previous) return;
		hoveredSegment = 0;
		LayoutBand();
		// The provider changes both sibling View headers and may reserve/release the
		// Graph frame.  Reposition every dependent child as one atomic layout pass.
		LayoutInput();
		LayoutActionButton();
		LayoutList();
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	//! Measure the row and store its regions. Kept separate from painting so a hit
	//! test never depends on whether a paint has happened yet.
	void LayoutBand()
	{
		bandSegments.clear();
		bandCountRect = RECT{};
		graphHeaderActionsLeft = 0;
		if (!window) { SyncTooltips(); return; }
		const HDC dc = ::GetDC(window);
		if (dc == nullptr) return;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
		if (band.visible) LayoutBandRow(dc);
		LayoutGraphHeaderActions(dc);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::ReleaseDC(window, dc);
		SyncTooltips();
	}
	//! The repository row proper. Split out of `LayoutBand` so the Graph header's
	//! own toolbar can share `bandSegments` -- and with it one hover model, one
	//! tooltip model, and one dispatch path -- without depending on whether a
	//! repository row is rendered at all.
	void LayoutBandRow(HDC dc)
	{
		const RECT bounds = BandBounds();
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const int gap = icons::ScaleDip(kRepositoryIconGapDip, dpi);
		const int actionInset = icons::ScaleDip(kActionInsetDip, dpi);
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);

		LONG right = std::max(bounds.left, bounds.right - inset);
		if (!band.count.empty()) {
			SIZE extent{};
			(void)::GetTextExtentPoint32W(dc, band.count.c_str(), static_cast<int>(band.count.size()), &extent);
			bandCountRect = RECT{ std::max(bounds.left, right - extent.cx), bounds.top, right, bounds.bottom };
			right = std::max(bounds.left, bandCountRect.left - gap);
		}

		std::vector<BandSegment> actions;
		int actionsWidth = 0;
		for (const auto& command : band.commands) {
			if (command.title.empty()) continue;
			BandSegment segment;
			segment.kind = EBandSegment::Action;
			segment.runs = ParseRuns(ToWide(command.title));
			segment.command = command.command;
			segment.argumentsJson = command.argumentsJson;
			segment.tooltip = ToWide(command.tooltip);
			const int width = icons::MeasureLabelRuns(dc, segment.runs, iconSide) + 2 * actionInset;
			segment.rect = RECT{ 0, bounds.top, width, bounds.bottom };
			actionsWidth += width;
			actions.push_back(std::move(segment));
		}
		// Upstream's `RepositoryRenderer` appends `scm/title`'s `navigation` group
		// after the provider's `statusBarCommands`, then the `...` that opens the
		// rest of that menu. The order here is that order.
		for (const auto& action : BuildGitScmTitleToolbarActions()) {
			BandSegment segment;
			segment.kind = EBandSegment::Action;
			segment.runs = ParseRuns(action.icon);
			segment.command = action.commandId;
			segment.tooltip = action.tooltip;
			const int width = icons::MeasureLabelRuns(dc, segment.runs, iconSide) + 2 * actionInset;
			segment.rect = RECT{ 0, bounds.top, width, bounds.bottom };
			actionsWidth += width;
			actions.push_back(std::move(segment));
		}
		{
			BandSegment overflow;
			overflow.kind = EBandSegment::Overflow;
			overflow.runs = ParseRuns(L"$(ellipsis)");
			overflow.tooltip = L"More Actions...";
			const int width = icons::MeasureLabelRuns(dc, overflow.runs, iconSide) + 2 * actionInset;
			overflow.rect = RECT{ 0, bounds.top, width, bounds.bottom };
			actionsWidth += width;
			actions.push_back(std::move(overflow));
		}
		LONG cursor = std::max(bounds.left, right - actionsWidth);
		const LONG actionsLeft = cursor;
		for (auto& segment : actions) {
			const LONG width = segment.rect.right;
			segment.rect = RECT{ cursor, bounds.top, std::min(right, cursor + width), bounds.bottom };
			cursor = std::min(right, cursor + width);
		}

		BandSegment name;
		name.kind = EBandSegment::Name;
		name.rect = RECT{ bounds.left + inset, bounds.top,
			std::max(bounds.left + inset, actionsLeft - gap), bounds.bottom };
		// Upstream puts the provider label and the repository path in the row's
		// `title`, which is the hover text; the row itself shows only the name.
		name.tooltip = band.title;
		bandSegments.push_back(std::move(name));
		for (auto& segment : actions) bandSegments.push_back(std::move(segment));
	}
	//! `MenuId.SCMHistoryTitle`'s `navigation` group, right-aligned in the Graph
	//! pane header exactly as upstream right-aligns a view title's action bar. A
	//! collapsed pane has no toolbar, because upstream hides the whole title menu
	//! along with the view's body.
	void LayoutGraphHeaderActions(HDC dc)
	{
		if (!GraphFrameVisible() || graphCollapsed) return;
		const RECT bounds = GraphHeaderBounds();
		if (bounds.right <= bounds.left) return;
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const int actionInset = icons::ScaleDip(kActionInsetDip, dpi);
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);
		std::vector<BandSegment> actions;
		int width = 0;
		for (const auto& action : BuildGitScmHistoryTitleToolbarActions()) {
			BandSegment segment;
			segment.kind = EBandSegment::Action;
			segment.runs = ParseRuns(action.icon);
			segment.command = action.commandId;
			segment.tooltip = action.tooltip;
			const int extent = icons::MeasureLabelRuns(dc, segment.runs, iconSide) + 2 * actionInset;
			segment.rect = RECT{ 0, bounds.top, extent, bounds.bottom };
			width += extent;
			actions.push_back(std::move(segment));
		}
		if (actions.empty()) return;
		const LONG right = std::max(bounds.left, bounds.right - inset);
		LONG cursor = std::max(bounds.left, right - width);
		graphHeaderActionsLeft = cursor;
		for (auto& segment : actions) {
			const LONG extent = segment.rect.right;
			segment.rect = RECT{ cursor, bounds.top, std::min(right, cursor + extent), bounds.bottom };
			cursor = std::min(right, cursor + extent);
			bandSegments.push_back(std::move(segment));
		}
	}
	void PaintBand(HDC dc)
	{
		if (bandSegments.empty()) return;
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);
		const int gap = icons::ScaleDip(kRepositoryIconGapDip, dpi);
		const int actionInset = icons::ScaleDip(kActionInsetDip, dpi);
		for (std::size_t index = 0; index < bandSegments.size(); ++index) {
			const auto& segment = bandSegments[index];
			if (segment.rect.right <= segment.rect.left) continue;
			if (segment.kind == EBandSegment::Name) {
				if (!band.visible) continue;
				const int height = static_cast<int>(segment.rect.bottom - segment.rect.top);
				const int side = std::min<int>(iconSide, static_cast<int>(segment.rect.right - segment.rect.left));
				const int left = static_cast<int>(segment.rect.left);
				const int top = static_cast<int>(segment.rect.top) + (height - side) / 2;
				const icons::IconRect box{ left, top, left + side, top + side };
				const auto icon = icons::ResolveThemeIcon(L"repo", icons::CCodiconFont::Instance().FaceName());
				const COLORREF color = palette.primaryText.ToColorRef();
				if (icon.font) {
					const HFONT glyphFont = icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName, std::max(1, box.Height()));
					if (glyphFont != nullptr) {
						if (!icon.fontIcon.glyph.empty()) {
							const HGDIOBJ previous = ::SelectObject(dc, glyphFont);
							::SetTextColor(dc, color);
							RECT glyph{ box.left, box.top, box.right, box.bottom };
							::DrawTextW(dc, icon.fontIcon.glyph.c_str(), static_cast<int>(icon.fontIcon.glyph.size()),
								&glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
							::SelectObject(dc, previous);
						}
						::DeleteObject(glyphFont);
					}
				} else {
					icons::codicons::Draw(dc, box, icon.builtin, color);
				}
				RECT labelRect{ std::min<LONG>(segment.rect.right, static_cast<LONG>(box.right) + gap), segment.rect.top,
					segment.rect.right, segment.rect.bottom };
				::SetTextColor(dc, color);
				::DrawTextW(dc, band.name.c_str(), static_cast<int>(band.name.size()), &labelRect,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
				continue;
			}
			if (hoveredSegment == index + 1) {
				const HBRUSH brush = ::CreateSolidBrush(palette.raised.ToColorRef());
				if (brush != nullptr) { ::FillRect(dc, &segment.rect, brush); ::DeleteObject(brush); }
			}
			const RECT content{ segment.rect.left + actionInset, segment.rect.top,
				std::max(segment.rect.left + actionInset, segment.rect.right - actionInset), segment.rect.bottom };
			icons::DrawLabelRuns(dc, segment.runs, content, iconSide, palette.primaryText.ToColorRef(), GlyphFonts());
		}
		if (!band.count.empty() && bandCountRect.right > bandCountRect.left) {
			::SetTextColor(dc, palette.descriptionText.ToColorRef());
			RECT count = bandCountRect;
			::DrawTextW(dc, band.count.c_str(), static_cast<int>(band.count.size()), &count,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}
	[[nodiscard]] std::wstring ResolveViewTitle(EScmTextKey key, std::wstring_view fallback) const
	{
		if (text) {
			if (std::wstring resolved = text(key, {}); !resolved.empty()) return resolved;
		}
		return std::wstring(fallback);
	}
	//! Upstream's pane header: a twistie, then the title. The chevron is the same
	//! pair the resource-group headers already use, so one gesture reads the same
	//! way everywhere in this view.
	void PaintViewHeader(HDC dc, RECT bounds, EScmTextKey key, std::wstring_view fallback,
		bool collapsed)
	{
		if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
		// Every pane header but the first draws the side bar's section separator,
		// the same one the Outline header draws in the Explorer container.  The
		// topmost pane has the view container's own title above it, so a line
		// there would double that boundary.
		RECT client{};
		if (window) ::GetClientRect(window, &client);
		if (bounds.top > client.top) {
			const HPEN separator = ::CreatePen(PS_SOLID, 1, palette.border.ToColorRef());
			const HGDIOBJ previousPen = ::SelectObject(dc, separator);
			::MoveToEx(dc, bounds.left, bounds.top, nullptr);
			::LineTo(dc, bounds.right, bounds.top);
			::SelectObject(dc, previousPen);
			::DeleteObject(separator);
		}
		const int headerInset = icons::ScaleDip(kRowInsetDip, dpi);
		const int twistieSide = icons::ScaleDip(16, dpi);
		const LONG twistieTop = bounds.top + (bounds.bottom - bounds.top - twistieSide) / 2;
		const RECT twistie{ bounds.left + headerInset, twistieTop,
			bounds.left + headerInset + twistieSide, twistieTop + twistieSide };
		DrawScmIcon(dc, collapsed ? L"chevron-right" : L"chevron-down", twistie,
			palette.secondaryText.ToColorRef());
		bounds.left = twistie.right + icons::ScaleDip(4, dpi);
		if (bounds.right <= bounds.left) return;
		const std::wstring title = ResolveViewTitle(key, fallback);
		::SetTextColor(dc, palette.primaryText.ToColorRef());
		::DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &bounds,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	void PaintGraph(HDC dc)
	{
		if (!GraphFrameVisible()) return;
		RECT graphHeader = GraphHeaderBounds();
		// Upstream's view title shrinks for its action bar rather than drawing under
		// it, so a long title ellipsizes instead of colliding with the toolbar.
		if (graphHeaderActionsLeft > graphHeader.left) {
			graphHeader.right = std::max(graphHeader.left,
				graphHeaderActionsLeft - icons::ScaleDip(4, dpi));
		}
		PaintViewHeader(dc, graphHeader, EScmTextKey::GraphTitle, L"Graph", graphCollapsed);
		if (graphCollapsed) return;
		RECT body = GraphBodyBounds();
		if (body.right <= body.left || body.bottom <= body.top) return;
		// With a history to show the body belongs to the graph list, which paints
		// its own rows; a frame here would give this section a border the change
		// list above it does not have.
		if (graphPresentation.status == EScmGraphPresentationStatus::Available) return;
		const std::wstring message =
			ResolveViewTitle(EScmTextKey::GraphUnavailable, L"Graph view is not available yet.");
		if (message.empty()) return;
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		::InflateRect(&body, -inset, -1);
		if (body.right <= body.left || body.bottom <= body.top) return;
		::SetTextColor(dc, palette.descriptionText.ToColorRef());
		::DrawTextW(dc, message.c_str(), static_cast<int>(message.size()), &body,
			DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
	}
	//! Take the worker's history page and rebuild the swimlane model from it. The
	//! rows are derived here and nowhere else, so what the list draws and what the
	//! model computed cannot drift.
	void ApplyHistory(std::vector<GitHistoryItem> items, bool read)
	{
		if (!read) {
			if (history.empty() && graphPresentation.status == EScmGraphPresentationStatus::Unavailable) return;
			history.clear();
			graphRows.clear();
			graphPresentation.status = EScmGraphPresentationStatus::Unavailable;
		} else {
			if (items == history && graphPresentation.status == EScmGraphPresentationStatus::Available) return;
			history = std::move(items);
			graphRows = BuildScmHistoryGraph(history);
			graphPresentation.status = EScmGraphPresentationStatus::Available;
		}
		PopulateGraph();
		LayoutGraphList();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	void PopulateGraph()
	{
		if (!graphList) return;
		const bool suspendRedraw = ::IsWindowVisible(graphList) != FALSE;
		if (suspendRedraw) ::SendMessageW(graphList, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(graphList, LB_RESETCONTENT, 0, 0);
		for (const auto& item : history) {
			::SendMessageW(graphList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.subject.c_str()));
		}
		::SendMessageW(graphList, LB_SETITEMHEIGHT, 0, icons::ScaleDip(kGraphRowHeightDip, dpi));
		if (!deferChildRepaint) {
			if (suspendRedraw) ::SendMessageW(graphList, WM_SETREDRAW, TRUE, 0);
			// Outside a multi-surface result transaction this is the complete state
			// transition, so the Graph can be presented immediately.
			::RedrawWindow(graphList, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		} else if (suspendRedraw) graphRedrawDeferred = true;
		UpdateGraphScrollbar();
	}
	//! `%at` is seconds since the epoch, rendered here in local time.
	[[nodiscard]] static std::wstring FormatHistoryDate(std::int64_t timestamp)
	{
		if (timestamp <= 0) return {};
		const auto value = static_cast<__time64_t>(timestamp);
		tm parts{};
		if (::_localtime64_s(&parts, &value) != 0) return {};
		wchar_t buffer[32]{};
		const int written = ::swprintf(buffer, _countof(buffer), L"%04d/%02d/%02d",
			parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
		if (written <= 0) return {};
		return std::wstring(buffer, static_cast<std::size_t>(written));
	}
	[[nodiscard]] static COLORREF LaneColor(std::size_t index) noexcept
	{
		return kGraphLaneColors[index % kScmGraphColorCount];
	}
	//! One history row: the swimlanes, this commit's circle, its ref badges, its
	//! subject, and a right-aligned author and date.
	void PaintGraphRow(HDC dc, int index, const RECT& bounds, UINT itemState)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= history.size()) return;
		if (static_cast<std::size_t>(index) >= graphRows.size()) return;
		const auto& item = history[static_cast<std::size_t>(index)];
		const auto& row = graphRows[static_cast<std::size_t>(index)];
		const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
		const bool focused = (::GetFocus() == graphList);
		const bool selected = (itemState & ODS_SELECTED) != 0;
		COLORREF background = palette.sideBar.ToColorRef();
		if (selected) background = (focused ? palette.accent : palette.raised).ToColorRef();
		const HBRUSH brush = ::CreateSolidBrush(background);
		if (brush != nullptr) { ::FillRect(dc, &bounds, brush); ::DeleteObject(brush); }

		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const int lane = icons::ScaleDip(kGraphLaneWidthDip, dpi);
		const LONG middle = (bounds.top + bounds.bottom) / 2;
		// Upstream's `drawCircle` puts lane `n` at `SWIMLANE_WIDTH * (n + 1)`, so the
		// first lane sits one whole swimlane in from the graph's left edge rather than
		// half of one.
		const auto laneX = [&](std::size_t position) {
			return static_cast<LONG>(bounds.left + inset + static_cast<LONG>(position + 1) * lane);
		};
		// Upstream's `renderSCMHistoryItemGraph` draws SVG paths, not bare vertical
		// lines: a lane that changes position curves into its new one, and every parent
		// after the first gets an explicit connector out of the commit's circle. Without
		// those two a merge is invisible -- the rows read as unrelated columns.
		// `gx(k)` is upstream's `SWIMLANE_WIDTH * k`, so lane `n` is centred on `gx(n + 1)`.
		const LONG curve = icons::ScaleDip(5, dpi); // upstream `SWIMLANE_CURVE_RADIUS`
		const auto gx = [&](std::size_t k) {
			return static_cast<LONG>(bounds.left + inset + static_cast<LONG>(k) * lane);
		};
		LONG penX = 0;
		LONG penY = 0;
		const auto moveTo = [&](LONG x, LONG y) { penX = x; penY = y; ::MoveToEx(dc, x, y, nullptr); };
		const auto lineTo = [&](LONG x, LONG y) { penX = x; penY = y; (void)::LineTo(dc, x, y); };
		// One `A r r 0 0 s` quarter turn. Both of its tangents are axis-aligned, so the
		// arc is the cubic Bezier whose control points sit 0.5523 of the way from each
		// end towards the corner those tangents meet at -- the standard quarter-circle
		// approximation, which GDI can draw directly and `Arc` cannot express as
		// conveniently from a current position.
		const auto arcTo = [&](LONG x, LONG y, LONG cornerX, LONG cornerY) {
			const auto toward = [](LONG from, LONG to) {
				return static_cast<LONG>(from + (to - from) * 5523 / 10000);
			};
			const POINT points[3]{
				{ toward(penX, cornerX), toward(penY, cornerY) },
				{ toward(x, cornerX), toward(y, cornerY) },
				{ x, y },
			};
			(void)::PolyBezierTo(dc, points, 3);
			penX = x;
			penY = y;
		};
		const auto stroke = [&](std::size_t color, const auto& path) {
			const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, icons::ScaleDip(1, dpi)), LaneColor(color));
			if (pen == nullptr) return;
			const HGDIOBJ previousPen = ::SelectObject(dc, pen);
			path();
			::SelectObject(dc, previousPen);
			::DeleteObject(pen);
		};

		const auto& inputLanes = row.inputSwimlanes;
		const auto& output = row.outputSwimlanes;
		constexpr std::size_t kNoLane = static_cast<std::size_t>(-1);
		std::size_t inputIndex = kNoLane;
		for (std::size_t laneIndex = 0; laneIndex < inputLanes.size(); ++laneIndex) {
			if (inputLanes[laneIndex].id == item.id) {
				inputIndex = laneIndex;
				break;
			}
		}
		// Upstream: the commit sits on whichever lane was waiting for it, or on a new
		// lane just past the right-hand end when nothing was.
		const std::size_t circleIndex = inputIndex != kNoLane ? inputIndex : inputLanes.size();
		const std::size_t circleColorIndex =
			circleIndex < output.size()  ? output[circleIndex].colorIndex
			: circleIndex < inputLanes.size() ? inputLanes[circleIndex].colorIndex
			                             : row.circleColorIndex;

		std::size_t outputIndex = 0;
		for (std::size_t laneIndex = 0; laneIndex < inputLanes.size(); ++laneIndex) {
			const std::size_t color = inputLanes[laneIndex].colorIndex;
			if (inputLanes[laneIndex].id == item.id) {
				// A second lane arriving at this same commit: it turns into the circle
				// instead of continuing down. This is a merge's incoming side, `/` then `-`.
				if (laneIndex != circleIndex) {
					stroke(color, [&] {
						moveTo(gx(laneIndex + 1), bounds.top);
						arcTo(gx(laneIndex), middle, gx(laneIndex + 1), middle);
						lineTo(gx(circleIndex + 1), middle);
					});
				} else {
					++outputIndex;
				}
				continue;
			}
			if (outputIndex >= output.size() || inputLanes[laneIndex].id != output[outputIndex].id) continue;
			if (laneIndex == outputIndex) {
				stroke(color, [&] {
					moveTo(gx(laneIndex + 1), bounds.top);
					lineTo(gx(laneIndex + 1), bounds.bottom);
				});
			} else {
				// The lane shifted left because a lane to its left ended here. Upstream
				// draws `|` down to y=6, a curve, the horizontal run, a second curve, `|`.
				stroke(color, [&] {
					moveTo(gx(laneIndex + 1), bounds.top);
					lineTo(gx(laneIndex + 1), bounds.top + icons::ScaleDip(6, dpi));
					arcTo(gx(laneIndex + 1) - curve, middle, gx(laneIndex + 1), middle);
					lineTo(gx(outputIndex + 1) + curve, middle);
					arcTo(gx(outputIndex + 1), middle + curve, gx(outputIndex + 1), middle);
					lineTo(gx(outputIndex + 1), bounds.bottom);
				});
			}
			++outputIndex;
		}

		// Every parent after the first: the `-` out of the circle and the `\` that turns
		// down into that parent's own lane, drawn in the parent's colour. This is the
		// stroke that makes a merge readable, and the one this row had no equivalent of.
		for (std::size_t parent = 1; parent < item.parentIds.size(); ++parent) {
			std::size_t parentLane = kNoLane;
			for (std::size_t laneIndex = output.size(); laneIndex-- > 0;) {
				if (output[laneIndex].id == item.parentIds[parent]) {
					parentLane = laneIndex;
					break;
				}
			}
			if (parentLane == kNoLane) continue;
			stroke(output[parentLane].colorIndex, [&] {
				moveTo(gx(parentLane), middle);
				arcTo(gx(parentLane + 1), bounds.bottom, gx(parentLane + 1), middle);
				moveTo(gx(parentLane), middle);
				lineTo(gx(circleIndex + 1), middle);
			});
		}

		// `|` into the circle and `|` out of it, each in its own end's colour.
		if (inputIndex != kNoLane) {
			stroke(inputLanes[inputIndex].colorIndex, [&] {
				moveTo(gx(circleIndex + 1), bounds.top);
				lineTo(gx(circleIndex + 1), middle);
			});
		}
		if (!item.parentIds.empty()) {
			stroke(circleColorIndex, [&] {
				moveTo(gx(circleIndex + 1), middle);
				lineTo(gx(circleIndex + 1), bounds.bottom);
			});
		}

		// The three node shapes are upstream's own: HEAD is `CIRCLE_RADIUS + 3` with a
		// `CIRCLE_STROKE_WIDTH` hole, a multi-parent commit is `CIRCLE_RADIUS + 2`, and
		// every other commit is `CIRCLE_RADIUS + 1`. Direct2D owns the circles so the
		// small diagonal edge pixels are antialiased like the SVG renderer.
		const COLORREF circleColor = LaneColor(circleColorIndex);
		const LONG circleX = gx(circleIndex + 1);
		const bool isHead = std::ranges::any_of(item.refs, [](const GitHistoryRef& ref) {
			return ref.kind == EGitHistoryRefKind::Head;
		});
		const bool isMerge = item.parentIds.size() > 1;
		const int outerRadius = icons::ScaleDip(
			isHead ? kGraphCircleRadiusDip + 3
			: isMerge ? kGraphCircleRadiusDip + 2
			          : kGraphCircleRadiusDip + 1, dpi);
		const int strokeWidth = icons::ScaleDip(kGraphCircleStrokeWidthDip, dpi);
		const int innerRadius = icons::ScaleDip(kGraphCircleStrokeWidthDip, dpi);
		const int innerMergeRadius = icons::ScaleDip(kGraphCircleRadiusDip - 1, dpi);
		const int headInnerStrokeWidth = icons::ScaleDip(kGraphCircleRadiusDip, dpi);
		const auto disc = [&](int radius, COLORREF fillColor, int outlineWidth) {
			const HBRUSH fill = ::CreateSolidBrush(fillColor);
			const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, outlineWidth), background);
			if (fill != nullptr && pen != nullptr) {
				const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
				const HGDIOBJ previousPen = ::SelectObject(dc, pen);
				::Ellipse(dc, circleX - radius, middle - radius,
					circleX + radius + 1, middle + radius + 1);
				::SelectObject(dc, previousPen);
				::SelectObject(dc, previousBrush);
			}
			if (pen != nullptr) ::DeleteObject(pen);
			if (fill != nullptr) ::DeleteObject(fill);
		};
		if (!graphNodePainter.Paint(dc, bounds, circleX, middle,
			outerRadius, strokeWidth, innerRadius, innerMergeRadius, headInnerStrokeWidth,
			isHead, isMerge, circleColor, background)) {
			if (isHead) {
				disc(outerRadius, circleColor, strokeWidth);
				disc(innerRadius, background, headInnerStrokeWidth);
			} else if (isMerge) {
				disc(outerRadius, circleColor, strokeWidth);
				disc(innerMergeRadius, circleColor, strokeWidth);
			} else {
				disc(outerRadius, circleColor, strokeWidth);
			}
		}

		const std::size_t lanes = std::max<std::size_t>(1,
			std::max(row.inputSwimlanes.size(), row.outputSwimlanes.size()));
		LONG cursor = laneX(lanes) + icons::ScaleDip(4, dpi);
		const LONG contentRight = bounds.right - inset;
		const COLORREF textColor = selected && focused ? palette.highlightText.ToColorRef()
			: palette.primaryText.ToColorRef();
		const COLORREF mutedColor = selected && focused ? palette.highlightText.ToColorRef()
			: palette.descriptionText.ToColorRef();
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);

		// Upstream's row is the graph, then one `IconLabel` whose label is the subject
		// and whose description is the author, then the badge container -- so a ref
		// badge trails the text rather than preceding it. The badges are measured
		// first so the subject ellipsizes into whatever they leave, exactly as that
		// flex row does.
		struct GraphBadge final {
			std::vector<icons::SLabelRun> runs;
			int width{};
		};
		std::vector<GraphBadge> badges;
		const LONG badgePadding = icons::ScaleDip(4, dpi);
		const LONG badgeGap = icons::ScaleDip(4, dpi);
		LONG badgesWidth = 0;
		// Upstream's badge container sits in a flex row beside the label, so a long
		// ref name shrinks rather than pushing the subject out of the row entirely.
		// Half of the remaining width is the share a badge may take here.
		const LONG badgeWidthLimit = std::max<LONG>(0, (contentRight - cursor) / 2);
		bool named = true;
		for (const auto& ref : item.refs) {
			if (ref.name.empty()) continue;
			std::wstring label = GraphRefIcon(ref.kind);
			if (named) label += L" " + ref.name;
			GraphBadge badge;
			badge.runs = ParseRuns(label);
			badge.width = icons::MeasureLabelRuns(dc, badge.runs, iconSide) + 2 * static_cast<int>(badgePadding);
			badge.width = static_cast<int>(std::min<LONG>(badge.width, badgeWidthLimit));
			badgesWidth += badge.width + badgeGap;
			badges.push_back(std::move(badge));
			named = false;
		}
		const LONG textRight = std::max<LONG>(cursor, contentRight - badgesWidth);

		const auto drawRun = [&](std::wstring_view text, COLORREF color) {
			if (text.empty() || cursor >= textRight) return;
			SIZE extent{};
			(void)::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
			RECT area{ cursor, bounds.top, textRight, bounds.bottom };
			::SetTextColor(dc, color);
			::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &area,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			cursor = std::min<LONG>(textRight, cursor + extent.cx);
		};
		drawRun(item.subject, textColor);
		if (!item.authorName.empty()) {
			cursor += icons::ScaleDip(6, dpi);
			drawRun(item.authorName, mutedColor);
		}

		cursor += badgeGap;
		for (const auto& badge : badges) {
			const LONG right = std::min<LONG>(contentRight, cursor + badge.width);
			if (right <= cursor) break;
			RECT box{ cursor, bounds.top + icons::ScaleDip(3, dpi), right,
				bounds.bottom - icons::ScaleDip(3, dpi) };
			const HBRUSH fill = ::CreateSolidBrush(circleColor);
			const HPEN pen = ::CreatePen(PS_SOLID, 1, circleColor);
			if (fill != nullptr && pen != nullptr) {
				const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
				const HGDIOBJ previousPen = ::SelectObject(dc, pen);
				const int corner = views::WelcomeButtonCornerRadius(dpi);
				::RoundRect(dc, box.left, box.top, box.right, box.bottom, corner, corner);
				::SelectObject(dc, previousPen);
				::SelectObject(dc, previousBrush);
			}
			if (pen != nullptr) ::DeleteObject(pen);
			if (fill != nullptr) ::DeleteObject(fill);
			RECT label{ box.left + badgePadding, bounds.top, right - badgePadding, bounds.bottom };
			icons::DrawLabelRuns(dc, badge.runs, label, iconSide,
				palette.buttonForeground.ToColorRef(), GlyphFonts());
			cursor = right + badgeGap;
		}
		::SetBkMode(dc, previousBackgroundMode);
	}
	//! Which section header the pointer is over: 1 Repositories, 2 Changes,
	//! 3 Graph, 0 none. A header that is not rendered is never hit.
	[[nodiscard]] int SectionHeaderAt(POINT point) const
	{
		if (band.visible) {
			RECT bounds = RepositoriesHeaderBounds();
			if (::PtInRect(&bounds, point)) return 1;
		}
		if (ChangesHeaderVisible()) {
			RECT bounds = ChangesHeaderBounds();
			if (::PtInRect(&bounds, point)) return 2;
		}
		if (GraphFrameVisible()) {
			RECT bounds = GraphHeaderBounds();
			if (::PtInRect(&bounds, point)) return 3;
		}
		return 0;
	}
	bool ToggleSectionAt(POINT point)
	{
		// A header action lies inside the header rectangle, so it has to be tried
		// first: otherwise pressing Refresh would collapse the pane instead.
		if (SegmentIndexAt(point) != 0) return false;
		switch (SectionHeaderAt(point)) {
		case 1: repositoriesCollapsed = !repositoriesCollapsed; break;
		case 2: changesCollapsed = !changesCollapsed; break;
		case 3: graphCollapsed = !graphCollapsed; break;
		default: return false;
		}
		RelayoutStack();
		return true;
	}
	//! Every band moved, so every band is laid out again. Collapsing one section
	//! moves the input, the button, both lists, and the welcome content.
	void RelayoutStack()
	{
		const bool previousDefer = deferChildRepaint;
		deferChildRepaint = true;
		LayoutBand();
		LayoutInput();
		LayoutActionButton();
		LayoutList();
		LayoutWelcome();
		deferChildRepaint = previousDefer;
		if (window) {
			// Invalidate the complete stack once, after all sibling HWNDs have their
			// final rectangles. Do not erase first: every SCM surface paints its own
			// background and an erase pass exposes a blank intermediate frame.
			::RedrawWindow(window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		}
	}
	//! The Changes/Graph boundary's drag handle. It is an overlay band, exactly as
	//! upstream's `.monaco-sash` is, so it consumes no layout space.
	[[nodiscard]] RECT SashBounds() const
	{
		return ViewBounds(ViewStack().sash);
	}
	[[nodiscard]] bool PointInSash(POINT point) const
	{
		RECT bounds = SashBounds();
		if (bounds.bottom <= bounds.top) return false;
		return ::PtInRect(&bounds, point) != FALSE;
	}
	bool BeginSashDrag(POINT point)
	{
		if (!window || !PointInSash(point)) return false;
		draggingSash = true;
		sashDragOriginY = point.y;
		sashDragOriginDip = std::max(kGraphMinimumBodyHeightDip, graphBodyDip);
		::SetCapture(window);
		return true;
	}
	void UpdateSashDrag(POINT point)
	{
		if (!draggingSash) return;
		// Dragging upwards grows the Graph, because the boundary is its top edge.
		// The delta is measured against the drag's own origin rather than the
		// previous message, so repeated rounding cannot accumulate.
		const int deltaPixels = sashDragOriginY - point.y;
		const int scale = static_cast<int>(dpi == 0 ? 96 : dpi);
		const int next = std::max(kGraphMinimumBodyHeightDip,
			sashDragOriginDip + ::MulDiv(deltaPixels, 96, scale));
		if (next == graphBodyDip) return;
		graphBodyDip = next;
		RelayoutStack();
	}
	void EndSashDrag()
	{
		if (!draggingSash) return;
		draggingSash = false;
		::ReleaseCapture();
	}
	[[nodiscard]] RECT WelcomeBounds() const
	{
		return ViewBounds(ViewStack().changesBody);
	}
	//! Recompute upstream's `viewsWelcome` choice from the same providers count
	//! already read for `openRepositoryCount` and the explicit workspace state
	//! projected by the composition root.
	void RebuildWelcome()
	{
		const auto previous = welcomeModel;
		welcomeModel = BuildGitScmWelcomeModel(welcomeWorkspaceState, openRepositoryCount != 0, text);
		if (welcomeModel == previous) return;
		hoveredWelcomeSegment = 0;
		// The empty resource list and the welcome content occupy the same region
		// and are mutually exclusive, exactly as upstream's tree body and its
		// `viewsWelcome` overlay are: showing both would draw an "open a folder"
		// prompt over rows that do not exist.
		if (list) {
			::ShowWindow(list, welcomeModel.content == EGitScmWelcomeContent::None ? SW_SHOW : SW_HIDE);
		}
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}
	//! ViewWelcome is a top-flow flex column: its content starts one `em` below
	//! the view body, each direct child gets a one-`em` block-start margin, and
	//! the action container is a centered column capped at 300px.
	void LayoutWelcome()
	{
		welcomeSegments.clear();
		welcomeMessageRect = RECT{};
		if (!window || welcomeModel.content == EGitScmWelcomeContent::None) return;
		const RECT body = WelcomeBounds();
		const int inset = views::WelcomeHorizontalInset(dpi);
		const LONG availableWidth = std::max<LONG>(0, body.right - body.left - 2 * inset);
		const LONG messageLeft = body.left + inset;
		const LONG messageRight = body.right - inset;
		const LONG buttonWidth = views::WelcomeButtonColumnWidth(static_cast<int>(availableWidth), dpi);
		const LONG buttonLeft = messageLeft + (availableWidth - buttonWidth) / 2;
		const LONG buttonRight = buttonLeft + buttonWidth;
		const LONG top = body.top;
		const LONG bottom = std::max(top, body.bottom);
		if (messageRight <= messageLeft || buttonRight <= buttonLeft || bottom <= top) return;

		const HDC dc = ::GetDC(window);
		if (dc == nullptr) return;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());

		TEXTMETRICW metrics{};
		(void)::GetTextMetricsW(dc, &metrics);
		const LONG em = std::max<LONG>(1, metrics.tmHeight);
		RECT messageRect{ messageLeft, 0, messageRight, 0 };
		if (!welcomeModel.message.empty()) {
			::DrawTextW(dc, welcomeModel.message.c_str(), static_cast<int>(welcomeModel.message.size()),
				&messageRect, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
		}
		const LONG messageHeight = messageRect.bottom - messageRect.top;

		// `.monaco-text-button` fixes its own font-size and line-height, so the
		// box does not grow with the label's font.
		const LONG buttonHeight = views::WelcomeButtonHeight(dpi);

		struct WelcomeButton {
			std::wstring label;
			std::string command;
			std::string argumentsJson;
		};
		std::vector<WelcomeButton> buttons;
		for (const auto& action : welcomeModel.actions) {
			if (action.label.empty() || action.command.empty()) continue;
			buttons.push_back(WelcomeButton{ action.label, action.command, action.argumentsJson });
		}

		LONG cursorTop = top + em;
		if (messageHeight > 0) {
			messageRect.top = cursorTop;
			messageRect.bottom = cursorTop + messageHeight;
			welcomeMessageRect = messageRect;
			cursorTop = messageRect.bottom;
		}
		if (!buttons.empty()) cursorTop += em;
		for (auto& button : buttons) {
			WelcomeSegment segment;
			segment.rect = RECT{ buttonLeft, cursorTop, buttonRight, cursorTop + buttonHeight };
			segment.label = std::move(button.label);
			segment.command = std::move(button.command);
			segment.argumentsJson = std::move(button.argumentsJson);
			welcomeSegments.push_back(std::move(segment));
			cursorTop += buttonHeight + em;
		}

		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::ReleaseDC(window, dc);
	}
	void PaintWelcome(HDC dc)
	{
		if (welcomeModel.content == EGitScmWelcomeContent::None) return;
		if (welcomeMessageRect.right > welcomeMessageRect.left && !welcomeModel.message.empty()) {
			::SetTextColor(dc, palette.primaryText.ToColorRef());
			RECT message = welcomeMessageRect;
			::DrawTextW(dc, welcomeModel.message.c_str(), static_cast<int>(welcomeModel.message.size()),
			&message, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
		}
		const int radius = views::WelcomeButtonCornerRadius(dpi);
		for (std::size_t index = 0; index < welcomeSegments.size(); ++index) {
			const auto& segment = welcomeSegments[index];
			if (segment.rect.right <= segment.rect.left) continue;
			const bool hovered = hoveredWelcomeSegment == index + 1;
			const COLORREF background =
				(hovered ? palette.buttonHoverBackground : palette.buttonBackground).ToColorRef();
			const HBRUSH brush = ::CreateSolidBrush(background);
			// `background-color` fills the whole border box. A NULL_PEN RoundRect
			// would stop one pixel short of the laid-out rectangle, which is enough
			// to make this button a different size from the Explorer's.
			const HPEN border = ::CreatePen(PS_SOLID, 1, background);
			if (brush != nullptr && border != nullptr) {
				const HGDIOBJ previousBrush = ::SelectObject(dc, brush);
				const HGDIOBJ previousPen = ::SelectObject(dc, border);
				::RoundRect(dc, segment.rect.left, segment.rect.top, segment.rect.right, segment.rect.bottom,
					radius, radius);
				::SelectObject(dc, previousPen);
				::SelectObject(dc, previousBrush);
			}
			if (border != nullptr) ::DeleteObject(border);
			if (brush != nullptr) ::DeleteObject(brush);
			::SetTextColor(dc, palette.buttonForeground.ToColorRef());
			RECT labelRect = segment.rect;
			::DrawTextW(dc, segment.label.c_str(), static_cast<int>(segment.label.size()), &labelRect,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}
	//! `scm/resourceGroup/context`'s `inline` group, laid out right to left from
	//! the count the row already draws. One function serves both the paint and the
	//! hit test so a button can never be drawn where a press does not land.
	[[nodiscard]] std::vector<GroupRowAction> GroupRowActions(HDC dc, const ScmRow& row,
		const RECT& bounds) const
	{
		std::vector<GroupRowAction> actions;
		if (!row.header || !row.group) return actions;
		// `mixed` is the `git.untrackedChanges` value this product publishes, and
		// it is the same value the row's context menu is built with.
		const auto built = BuildGitResourceGroupInlineActions(*row.group, EUntrackedChangesPolicy::Mixed);
		if (built.empty()) return actions;
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const int iconSide = icons::ScaleDip(16, dpi);
		const int actionInset = icons::ScaleDip(kActionInsetDip, dpi);
		const int width = iconSide + 2 * actionInset;
		const std::wstring count = std::to_wstring(row.resourceCount);
		SIZE extent{};
		(void)::GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &extent);
		LONG right = bounds.right - inset - extent.cx - icons::ScaleDip(5, dpi);
		for (auto entry = built.rbegin(); entry != built.rend(); ++entry) {
			const LONG left = right - width;
			if (left <= bounds.left) break;
			GroupRowAction action;
			action.rect = RECT{ left, bounds.top, right, bounds.bottom };
			action.icon = StripCodiconWrapper(entry->icon);
			action.command = entry->commandId;
			action.tooltip = entry->tooltip;
			actions.push_back(std::move(action));
			right = left;
		}
		std::ranges::reverse(actions);
		return actions;
	}
	//! Run a group row's inline action. Returns false when the press was not on
	//! one, which leaves the row's own collapse gesture in charge.
	bool InvokeGroupRowActionAt(int index, POINT point)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return false;
		const auto& row = rows[static_cast<std::size_t>(index)];
		if (!row.header) return false;
		RECT bounds{};
		if (::SendMessageW(list, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&bounds)) == LB_ERR) {
			return false;
		}
		const HDC dc = ::GetDC(list);
		if (dc == nullptr) return false;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
		const auto actions = GroupRowActions(dc, row, bounds);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::ReleaseDC(list, dc);
		for (const auto& action : actions) {
			if (!::PtInRect(&action.rect, point)) continue;
			if (action.command.empty() || !runCommand) return false;
			// Upstream's group-scoped commands take the group itself, which the
			// executor resolves; the row's own context menu dispatches the same way.
			(void)runCommand(action.command, {});
			return true;
		}
		return false;
	}
	void ToggleGroupAt(int index)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size() || !rows[index].header) return;
		const std::string key = rows[index].groupKey;
		if (key.empty()) return;
		if (!collapsedGroups.insert(key).second) collapsedGroups.erase(key);
		PublishAndRender();
		if (list == nullptr) return;
		for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
			if (rows[rowIndex].header && rows[rowIndex].groupKey == key) {
				(void)::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(rowIndex), 0);
				break;
			}
		}
	}
	bool ToggleSelectedGroup()
	{
		if (list == nullptr) return false;
		const LRESULT selection = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
		if (selection == LB_ERR) return false;
		const int index = static_cast<int>(selection);
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size() || !rows[index].header) return false;
		ToggleGroupAt(index);
		return true;
	}
	void PaintRow(HDC dc, int index, const RECT& bounds, UINT itemState)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return;
		const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF previousTextColor = ::GetTextColor(dc);
		const auto& row = rows[static_cast<std::size_t>(index)];
		const bool focused = (::GetFocus() == list);
		const bool selected = (itemState & ODS_SELECTED) != 0;
		const bool hovered = index == listHoverIndex;
		COLORREF background = palette.sideBar.ToColorRef();
		if (selected) background = (focused ? palette.accent : palette.raised).ToColorRef();
		else if (hovered) background = palette.raised.ToColorRef();
		const HBRUSH brush = ::CreateSolidBrush(background);
		if (brush != nullptr) { ::FillRect(dc, &bounds, brush); ::DeleteObject(brush); }
		const int inset = icons::ScaleDip(kRowInsetDip, dpi);
		const int iconSide = icons::ScaleDip(16, dpi);
		RECT iconRect{ bounds.left + inset, bounds.top + (bounds.bottom - bounds.top - iconSide) / 2,
			bounds.left + inset + iconSide, bounds.top + (bounds.bottom - bounds.top - iconSide) / 2 + iconSide };
		const COLORREF textColor = selected && focused ? palette.highlightText.ToColorRef()
			: (row.faded ? palette.descriptionText.ToColorRef() : palette.primaryText.ToColorRef());
		if (row.header) {
			const bool collapsed = collapsedGroups.contains(row.groupKey);
			DrawScmIcon(dc, collapsed ? L"chevron-right" : L"chevron-down", iconRect, palette.secondaryText.ToColorRef());
			const std::wstring count = std::to_wstring(row.resourceCount);
			SIZE countExtent{};
			(void)::GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &countExtent);
			RECT countRect{ bounds.right - inset - countExtent.cx, bounds.top, bounds.right - inset, bounds.bottom };
			const auto actions = GroupRowActions(dc, row, bounds);
			LONG labelRight = countRect.left - icons::ScaleDip(5, dpi);
			for (const auto& action : actions) {
				const bool actionHovered = hovered && ::PtInRect(&action.rect, listPointer);
				if (actionHovered) {
					const HBRUSH actionBrush = ::CreateSolidBrush(palette.raised.ToColorRef());
					if (actionBrush != nullptr) {
						RECT fill = action.rect;
						::FillRect(dc, &fill, actionBrush);
						::DeleteObject(actionBrush);
					}
				}
				const int side = icons::ScaleDip(16, dpi);
				const LONG left = action.rect.left + (action.rect.right - action.rect.left - side) / 2;
				const LONG top = action.rect.top + (action.rect.bottom - action.rect.top - side) / 2;
				const RECT glyph{ left, top, left + side, top + side };
				DrawScmIcon(dc, action.icon, glyph, textColor);
				labelRight = std::min(labelRight, action.rect.left - icons::ScaleDip(2, dpi));
			}
			RECT label{ iconRect.right + icons::ScaleDip(4, dpi), bounds.top,
				std::max<LONG>(iconRect.right + icons::ScaleDip(4, dpi), labelRight), bounds.bottom };
			::SetTextColor(dc, textColor);
			::DrawTextW(dc, row.label.c_str(), static_cast<int>(row.label.size()), &label,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			::SetTextColor(dc, palette.descriptionText.ToColorRef());
			::DrawTextW(dc, count.c_str(), static_cast<int>(count.size()), &countRect,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		} else {
			iconRect.left += inset;
			iconRect.right += inset;
			DrawScmFileIcon(dc, row.label, iconRect, palette.secondaryText.ToColorRef());
			RECT label{ iconRect.right + icons::ScaleDip(4, dpi), bounds.top,
				bounds.right - inset - icons::ScaleDip(22, dpi), bounds.bottom };
			::SetTextColor(dc, textColor);
			::DrawTextW(dc, row.label.c_str(), static_cast<int>(row.label.size()), &label,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			if (row.strikeThrough) {
				SIZE extent{};
				::GetTextExtentPoint32W(dc, row.label.c_str(), static_cast<int>(row.label.size()), &extent);
				const int y = (label.top + label.bottom) / 2;
				const HPEN pen = ::CreatePen(PS_SOLID, 1, textColor);
				if (pen != nullptr) {
					const HGDIOBJ previousPen = ::SelectObject(dc, pen);
					::MoveToEx(dc, label.left, y, nullptr);
					::LineTo(dc, std::min<LONG>(label.right, label.left + extent.cx), y);
					::SelectObject(dc, previousPen);
					::DeleteObject(pen);
				}
			}
			if (row.statusLetter != L' ') {
				COLORREF status = palette.secondaryText.ToColorRef();
				switch (row.statusLetter) {
				case L'A': case L'U': status = palette.accent.ToColorRef(); break;
				case L'M': case L'C': status = palette.warning.ToColorRef(); break;
				case L'D': status = palette.danger.ToColorRef(); break;
				default: break;
				}
				RECT statusRect{ bounds.right - inset - icons::ScaleDip(16, dpi), bounds.top, bounds.right - inset, bounds.bottom };
				::SetTextColor(dc, status);
				::DrawTextW(dc, &row.statusLetter, 1, &statusRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			}
		}
		if ((itemState & ODS_FOCUS) != 0) {
			const HBRUSH focusBrush = ::CreateSolidBrush(palette.accent.ToColorRef());
			if (focusBrush != nullptr) { ::FrameRect(dc, &bounds, focusBrush); ::DeleteObject(focusBrush); }
		}
		::SetTextColor(dc, previousTextColor);
		::SetBkMode(dc, previousBackgroundMode);
	}
	//! Paint the Changes LISTBOX as one retained frame. Native fixed-owner-draw
	//! dispatches one WM_DRAWITEM per row, so a resize can expose a valid empty
	//! background between row callbacks even though every callback is buffered.
	void PaintChangesListFrame(HDC target, const RECT& dirtyRect)
	{
		if (target == nullptr || list == nullptr) return;
		RECT client{};
		::GetClientRect(list, &client);
		const int width = std::max(0L, client.right - client.left);
		const int height = std::max(0L, client.bottom - client.top);
		if (width == 0 || height == 0) return;
		const bool buffered = listBuffer.Ensure(target, width, height);
		const HDC dc = buffered ? listBuffer.Dc() : target;
		(void)::SelectClipRgn(dc, nullptr);
		const HBRUSH background = ::CreateSolidBrush(palette.sideBar.ToColorRef());
		if (background != nullptr) {
			::FillRect(dc, &client, background);
			::DeleteObject(background);
		}
		const HGDIOBJ previousFont = font.Get() != nullptr ? ::SelectObject(dc, font.Get()) : nullptr;
		const int selected = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
		const int top = std::max(0, static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
		const int itemHeight = std::max(1,
			static_cast<int>(::SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
		for (int index = top; static_cast<std::size_t>(index) < rows.size(); ++index) {
			const LONG itemTop = static_cast<LONG>((index - top) * itemHeight);
			if (itemTop >= client.bottom) break;
			const RECT item{ client.left, itemTop, client.right, itemTop + itemHeight };
			UINT itemState = index == selected ? ODS_SELECTED : 0;
			if (index == selected && ::GetFocus() == list) itemState |= ODS_FOCUS;
			PaintRow(dc, index, item, itemState);
		}
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		if (buffered) (void)listBuffer.Present(target, client);
		// Publish only after the retained list image is complete. The source DC
		// remains valid until the caller ends WM_PAINT, and the adapter copies only
		// dirtyRect into its immutable payload.
		nativeSurface.CaptureAndSubmit(list, buffered ? listBuffer.Dc() : target, dirtyRect);
	}
	//! Paint the Graph LISTBOX as one retained frame. The native fixed-owner-draw
	//! path can split visible rows across multiple WM_DRAWITEM callbacks after a
	//! resize, which lets DWM present a valid background between row batches.
	void PaintGraphListFrame(HDC target)
	{
		if (target == nullptr || graphList == nullptr) return;
		RECT client{};
		::GetClientRect(graphList, &client);
		const int width = std::max(0L, client.right - client.left);
		const int height = std::max(0L, client.bottom - client.top);
		if (width == 0 || height == 0) return;
		const bool buffered = graphBuffer.Ensure(target, width, height);
		const HDC dc = buffered ? graphBuffer.Dc() : target;
		const HBRUSH background = ::CreateSolidBrush(palette.sideBar.ToColorRef());
		if (background != nullptr) {
			::FillRect(dc, &client, background);
			::DeleteObject(background);
		}
		const HGDIOBJ previousFont = font.Get() != nullptr ? ::SelectObject(dc, font.Get()) : nullptr;
		const int selected = static_cast<int>(::SendMessageW(graphList, LB_GETCURSEL, 0, 0));
		const int top = std::max(0, static_cast<int>(::SendMessageW(graphList, LB_GETTOPINDEX, 0, 0)));
		const int itemHeight = std::max(1,
			static_cast<int>(::SendMessageW(graphList, LB_GETITEMHEIGHT, 0, 0)));
		for (int index = top; static_cast<std::size_t>(index) < history.size(); ++index) {
			const LONG itemTop = static_cast<LONG>((index - top) * itemHeight);
			if (itemTop >= client.bottom) break;
			const RECT item{ client.left, itemTop, client.right, itemTop + itemHeight };
			UINT itemState = index == selected ? ODS_SELECTED : 0;
			if (index == selected && ::GetFocus() == graphList) itemState |= ODS_FOCUS;
			PaintGraphRow(dc, index, item, itemState);
		}
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		if (buffered) (void)graphBuffer.Present(target, client);
	}
	[[nodiscard]] std::size_t WelcomeSegmentIndexAt(POINT point) const
	{
		for (std::size_t index = 0; index < welcomeSegments.size(); ++index) {
			if (::PtInRect(&welcomeSegments[index].rect, point)) return index + 1;
		}
		return 0;
	}
	void SetHoveredWelcomeSegment(std::size_t segment)
	{
		if (hoveredWelcomeSegment == segment) return;
		hoveredWelcomeSegment = segment;
		if (window) {
			const RECT bounds = WelcomeBounds();
			::InvalidateRect(window, &bounds, FALSE);
		}
	}
	//! Run the pressed welcome action button, which dispatches `git.init` or
	//! `git.clone` exactly as the Command Palette entry for the same command
	//! would.
	bool InvokeWelcomeSegmentAt(POINT point)
	{
		const auto index = WelcomeSegmentIndexAt(point);
		if (index == 0) return false;
		const auto& segment = welcomeSegments[index - 1];
		if (segment.command.empty() || !runCommand) return false;
		(void)runCommand(segment.command, segment.argumentsJson);
		return true;
	}
	//! Replace the row's tools. Upstream's title and its command tooltips are the
	//! only place the repository path, the remote, and the commit counts appear.
	void SyncTooltips()
	{
		if (tooltip == nullptr || window == nullptr) return;
		for (std::size_t index = 0; index < tooltipToolCount; ++index) {
			TOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			tool.hwnd = window;
			tool.uId = index + 1;
			::SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
		}
		tooltipToolCount = bandSegments.size();
		for (std::size_t index = 0; index < bandSegments.size(); ++index) {
			const auto& segment = bandSegments[index];
			if (segment.tooltip.empty()) continue;
			TOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			tool.uFlags = TTF_SUBCLASS;
			tool.hwnd = window;
			tool.uId = index + 1;
			tool.rect = segment.rect;
			tool.lpszText = LPSTR_TEXTCALLBACKW;
			::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
		}
	}
	[[nodiscard]] std::size_t SegmentIndexAt(POINT point) const
	{
		for (std::size_t index = 0; index < bandSegments.size(); ++index) {
			if (::PtInRect(&bandSegments[index].rect, point)) return index + 1;
		}
		return 0;
	}
	void SetHoveredSegment(std::size_t segment)
	{
		if (hoveredSegment == segment) return;
		hoveredSegment = segment;
		if (window) {
			RECT bounds = BandBounds();
			::InvalidateRect(window, &bounds, FALSE);
			if (GraphFrameVisible()) {
				bounds = GraphHeaderBounds();
				::InvalidateRect(window, &bounds, FALSE);
			}
		}
	}
	//! Run the row's toolbar action, which is upstream's `StatusBarAction.run`.
	bool InvokeSegmentAt(POINT point)
	{
		const auto index = SegmentIndexAt(point);
		if (index == 0) return false;
		const auto& segment = bandSegments[index - 1];
		if (segment.kind == EBandSegment::Overflow) {
			POINT anchor{ segment.rect.left, segment.rect.bottom };
			if (window) ::ClientToScreen(window, &anchor);
			ShowTitleOverflowMenu(anchor);
			return true;
		}
		if (segment.command.empty() || !runCommand) return false;
		(void)runCommand(segment.command, segment.argumentsJson);
		// The press belonged to this row either way: an unrecognized command has
		// no second meaning here, unlike a resource row's file fallback.
		return true;
	}
	//! Upstream's `SCMStatusBarController` renders exactly one repository's
	//! `statusBarCommands`: the one owning the active editor, falling back to the
	//! first repository. There is only ever one built-in provider here, so the
	//! fallback is the whole rule; concatenating every provider's commands would
	//! be a behavior upstream does not have.
	void PublishStatusBarCommands(const std::vector<ScmProviderState>& providers) {
		if (!statusBarCommands) return;
		static const std::vector<ScmCommand> none;
		statusBarCommands(providers.empty() ? none : providers.front().statusBarCommands);
	}
	bool RebuildRows(const std::vector<ScmProviderState>& providers,
		const std::vector<GitResourceDecoration>& decorations,
		const std::vector<GitResourceOperand>& operands) {
		// Index the two side tables once. The previous per-resource `find_if`
		// made a refresh O(R * (O + D)), which becomes visible as the repository
		// grows and keeps the UI thread busy before it can service paint messages.
		std::unordered_map<std::wstring, const GitResourceDecoration*> decorationByUri;
		decorationByUri.reserve(decorations.size());
		for (const auto& decoration : decorations) {
			decorationByUri.try_emplace(decoration.resourceUri, &decoration);
		}
		const auto operandKey = [](EGitResourceGroup group, std::wstring_view uri) {
			std::wstring key = std::to_wstring(static_cast<int>(group));
			key.push_back(L'\x1f');
			key.append(uri);
			return key;
		};
		std::unordered_map<std::wstring, const GitResourceOperand*> operandByGroupAndUri;
		operandByGroupAndUri.reserve(operands.size());
		for (const auto& operand : operands) {
			operandByGroupAndUri.try_emplace(
				operandKey(operand.resource.group, operand.resourceUri), &operand);
		}
		std::vector<ScmRow> nextRows;
		std::size_t nextResourceCount = 0;
		for (const auto& provider : providers) {
			// Git's menus belong to Git's rows. A provider an extension published
			// is a different SCM system, and offering it `git.stage` would name a
			// command that does not own its resources.
			const bool builtinGit = provider.id == kGitProviderId;
			for (const auto& group : provider.groups) {
				if (!IsVisibleGroup(group)) continue;
				const auto groupKind = builtinGit ? ParseGitResourceGroupId(group.id) : std::nullopt;
				const std::string groupKey = provider.id + "\x1f" + group.id;
				ScmRow header;
				header.header = true;
				header.group = groupKind;
				header.label = ToWide(group.label);
				header.text = header.label;
				header.groupKey = groupKey;
				header.resourceCount = group.resources.size();
				nextRows.push_back(std::move(header));
				nextResourceCount += group.resources.size();
				if (collapsedGroups.contains(groupKey)) continue;
				for (const auto& resource : group.resources) {
					const auto uri = resource.resourceUri.ToString();
					const auto windowsPath = resource.resourceUri.ToWindowsPath();
					ScmRow row;
					row.windowsPath = windowsPath.value.value_or(std::wstring{});
					row.resourceUri = uri;
					row.command = resource.command;
					row.group = groupKind;
					row.groupKey = groupKey;
					row.tooltip = resource.tooltip ? ToWide(*resource.tooltip) : std::wstring{};
					row.faded = resource.faded;
					row.strikeThrough = resource.strikeThrough;
					if (groupKind) {
						// Keyed by URI *and* group: the same path legitimately has a
						// row in Staged Changes and one in Changes, and those two
						// rows stage and unstage different things.
						const auto operand = operandByGroupAndUri.find(operandKey(*groupKind, uri));
						if (operand != operandByGroupAndUri.end()) row.operand = operand->second->resource;
					}
					const auto found = decorationByUri.find(uri);
					// A provider we did not publish has no badge of ours, and
					// inventing one would claim a status nobody reported.
					row.statusLetter = found == decorationByUri.end() ? L' ' : found->second->letter;
					row.label = row.windowsPath.empty() ? uri : RelativeDisplayPath(root, row.windowsPath);
					row.text = row.label;
					nextRows.push_back(std::move(row));
				}
			}
		}
		const bool changed = rows != nextRows || resourceCount != nextResourceCount;
		rows = std::move(nextRows);
		resourceCount = nextResourceCount;
		return changed;
	}
	[[nodiscard]] std::optional<ScmRowSelection> CaptureListSelection() const
	{
		if (list == nullptr) return std::nullopt;
		const LRESULT selection = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
		if (selection == LB_ERR || selection < 0 || static_cast<std::size_t>(selection) >= rows.size()) {
			return std::nullopt;
		}
		const auto& row = rows[static_cast<std::size_t>(selection)];
		return ScmRowSelection{ row.header, row.groupKey, row.resourceUri };
	}

	void RestoreListSelection(const std::optional<ScmRowSelection>& selected)
	{
		if (list == nullptr || !selected) return;
		for (std::size_t index = 0; index < rows.size(); ++index) {
			const auto& row = rows[index];
			if (row.header == selected->header && row.groupKey == selected->groupKey
				&& (row.header || row.resourceUri == selected->resourceUri)) {
				(void)::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
				return;
			}
		}
	}

	void Populate(const std::optional<ScmRowSelection>& selected) {
		if (!list) return;
		const bool suspendRedraw = ::IsWindowVisible(list) != FALSE;
		if (suspendRedraw) ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_RESETCONTENT, 0, 0);
		for (const auto& row : rows) {
			::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.label.c_str()));
		}
		::SendMessageW(list, LB_SETITEMHEIGHT, 0, icons::ScaleDip(kRepositoryRowHeightDip, dpi));
		RestoreListSelection(selected);
		if (!deferChildRepaint) {
			if (suspendRedraw) ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
			// A direct refresh owns the complete transition. A worker-result
			// transaction leaves redraw disabled until its sibling Graph is ready.
			::RedrawWindow(list, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		} else if (suspendRedraw) listRedrawDeferred = true;
		UpdateListScrollbar();
		::InvalidateRect(window, nullptr, FALSE);
	}
	void ActivateRow(const ScmRow& row) {
		if (row.header) return;
		// The published command is the resource's real activation contract; the
		// file callback is the fallback for a host that does not recognize it.
		if (row.command && runCommand && runCommand(row.command->command, row.command->argumentsJson)) return;
		if (activateFile && !row.windowsPath.empty()) activateFile(row.windowsPath);
	}
	void ActivateSelection() {
		if (!list) return;
		const auto index = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return;
		ActivateRow(rows[static_cast<std::size_t>(index)]);
	}
	//! Run one chosen menu entry against the row it was built for.
	void InvokeMenuItem(const GitMenuItem& item, const ScmRow& row) {
		if (item.commandId.empty()) return;
		// The row's own published command already carries its payload, so the open
		// entry runs exactly what a double-click runs, fallback included.
		if (row.command && row.command->command == item.commandId) { ActivateRow(row); return; }
		// A resource-scoped command names the one row the menu was opened on. A
		// group-scoped one takes no arguments at all: upstream's `stageAll` and
		// friends read the repository's own groups, and so do ours.
		std::string arguments;
		if (!row.header && row.operand) arguments = BuildGitStageArguments({ *row.operand });
		if (runCommand) (void)runCommand(item.commandId, arguments);
	}
	//!
	//! @brief Track one of upstream's menu models and return the chosen entry.
	//!
	//! Every popup this view shows goes through here, so a resource menu, a
	//! group menu, and the row toolbar's overflow all dismiss, anchor, and
	//! report the same way. Nothing for a dismissal or an empty model: an empty
	//! popup would claim the surface has actions that merely happen to be
	//! unavailable.
	//!
	[[nodiscard]] std::optional<GitMenuItem> TrackMenu(const std::vector<GitMenuItem>& items, POINT screen)
	{
		if (items.empty() || !window) return std::nullopt;
		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return std::nullopt;
		for (std::size_t position = 0; position < items.size(); ++position) {
			if (items[position].separator) {
				::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				continue;
			}
			::AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(position + 1),
				items[position].title.c_str());
		}
		// The owning window must be foreground or the menu never sees its own
		// dismissal; the trailing `WM_NULL` is that requirement's documented half.
		::SetForegroundWindow(window);
		const auto chosen = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
			screen.x, screen.y, 0, window, nullptr);
		::DestroyMenu(menu);
		::PostMessageW(window, WM_NULL, 0, 0);
		if (chosen <= 0 || static_cast<std::size_t>(chosen) > items.size()) return std::nullopt;
		return items[static_cast<std::size_t>(chosen) - 1];
	}
	//!
	//! @brief `scm/historyItem/context` for the clicked commit in the Graph.
	//!
	//! Anchors and selects exactly as the resource list's menu does, so the row
	//! the user sees and the row the command receives are the same row. A gesture
	//! that names no commit shows nothing rather than acting on the selection it
	//! happens to find.
	//!
	void ShowHistoryItemContextMenu(POINT screen, bool fromKeyboard)
	{
		if (!graphList || !window) return;
		int index = -1;
		if (fromKeyboard) {
			index = static_cast<int>(::SendMessageW(graphList, LB_GETCURSEL, 0, 0));
			if (index < 0) return;
			RECT item{};
			if (::SendMessageW(graphList, LB_GETITEMRECT, static_cast<WPARAM>(index),
					reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
				return;
			}
			POINT anchor{ item.left, item.bottom };
			::ClientToScreen(graphList, &anchor);
			screen = anchor;
		} else {
			POINT client = screen;
			::ScreenToClient(graphList, &client);
			const auto hit = ::SendMessageW(graphList, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<WORD>(client.x), static_cast<WORD>(client.y)));
			if (HIWORD(hit) != 0) return;
			index = static_cast<int>(LOWORD(hit));
			::SendMessageW(graphList, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
		}
		if (index < 0 || static_cast<std::size_t>(index) >= history.size()) return;
		// A copy of the id, not a reference into `history`: `TrackPopupMenu` pumps
		// messages, so a refresh can replace the page while the menu is open.
		const std::wstring historyItemId = history[static_cast<std::size_t>(index)].id;
		const auto chosen = TrackMenu(BuildGitHistoryItemContextMenu(), screen);
		if (!chosen || chosen->commandId.empty() || !runCommand) return;
		(void)runCommand(chosen->commandId, BuildGitHistoryItemArguments(historyItemId));
	}
	//! The repository row toolbar's `...`: `scm/title`'s secondary actions.
	void ShowTitleOverflowMenu(POINT screen)
	{
		const auto chosen = TrackMenu(BuildGitScmTitleOverflowMenu(), screen);
		if (!chosen || chosen->commandId.empty() || !runCommand) return;
		// Every `scm/title` command is repository-scoped upstream and takes no
		// operand, so none of them carries arguments here either.
		(void)runCommand(chosen->commandId, {});
	}
	//!
	//! @brief `scm/resourceState/context` and `scm/resourceGroup/context`, tracked.
	//!
	//! `screen` is where the popup goes. `fromKeyboard` marks the menu-key path,
	//! which carries no cursor position and must anchor on the focused row instead.
	//!
	void ShowContextMenu(POINT screen, bool fromKeyboard) {
		if (!list || !window) return;
		int index = -1;
		if (fromKeyboard) {
			index = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
			if (index < 0) return;
			RECT item{};
			if (::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
					reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
				return;
			}
			POINT anchor{ item.left, item.bottom };
			::ClientToScreen(list, &anchor);
			screen = anchor;
		} else {
			POINT client = screen;
			::ScreenToClient(list, &client);
			const auto hit = ::SendMessageW(list, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<WORD>(client.x), static_cast<WORD>(client.y)));
			// The nearest row is reported even for a click past the last one, so the
			// outside flag is the only thing separating "this row" from "no row".
			if (HIWORD(hit) != 0) return;
			index = static_cast<int>(LOWORD(hit));
			// Selecting first is what makes the row the user sees and the row the
			// command receives the same row, exactly as VS Code's list does.
			::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
		}
		if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return;
		// A copy, not a reference: `TrackPopupMenu` pumps messages, so a refresh
		// result can rebuild `rows` while the menu is open and leave a reference
		// naming a row that no longer exists.
		const ScmRow row = rows[static_cast<std::size_t>(index)];
		// A provider we did not publish has no group of ours, and a resource row
		// with no operand cannot say what a command would act on. Either way Git's
		// menu would promise something about a row it cannot describe.
		if (!row.group) return;
		if (!row.header && !row.operand) return;
		// `Mixed` is the policy both publish paths use, so the menu's untracked
		// branch matches the groups the rows were actually built from.
		const auto items = row.header
			? BuildGitResourceGroupContextMenu(*row.group, EUntrackedChangesPolicy::Mixed)
			: BuildGitResourceContextMenu(*row.group);
		// An empty model means every upstream entry for this row is unroutable
		// here. Showing an empty popup would claim the row has actions that merely
		// happen to be unavailable.
		if (items.empty()) return;
		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		for (std::size_t position = 0; position < items.size(); ++position) {
			if (items[position].separator) {
				::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				continue;
			}
			std::wstring title = items[position].title;
			if (text) {
				const auto localize = [this, &title](EScmTextKey key) {
					if (std::wstring localized = text(key, {}); !localized.empty()) title = std::move(localized);
				};
				if (items[position].commandId == "git.openChange") localize(EScmTextKey::GitOpenChanges);
				else if (items[position].commandId == "git.openFile") localize(EScmTextKey::GitOpenFile);
				else if (items[position].commandId == "git.stage") localize(EScmTextKey::GitStageChanges);
				else if (items[position].commandId == "git.unstage") localize(EScmTextKey::GitUnstageChanges);
				else if (items[position].commandId == "git.clean") localize(EScmTextKey::GitDiscardChanges);
				else if (items[position].commandId == "git.stageAll") localize(EScmTextKey::GitStageAllChanges);
				else if (items[position].commandId == "git.unstageAll") localize(EScmTextKey::GitUnstageAllChanges);
				else if (items[position].commandId == "git.cleanAll") localize(EScmTextKey::GitDiscardAllChanges);
			}
			::AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(position + 1), title.c_str());
		}
		// The owning window must be foreground or the menu never sees its own
		// dismissal; the trailing `WM_NULL` is that requirement's documented half.
		::SetForegroundWindow(window);
		const auto chosen = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
			screen.x, screen.y, 0, window, nullptr);
		::DestroyMenu(menu);
		::PostMessageW(window, WM_NULL, 0, 0);
		if (chosen <= 0 || static_cast<std::size_t>(chosen) > items.size()) return;
		InvokeMenuItem(items[static_cast<std::size_t>(chosen) - 1], row);
	}
};

LRESULT CALLBACK CScmWorkbenchTool::InputSubclassProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* const impl = reinterpret_cast<Impl*>(data);
	if (message == WM_NCDESTROY) {
		::RemoveWindowSubclass(window, &CScmWorkbenchTool::InputSubclassProc, id);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (message == WM_ERASEBKGND && impl != nullptr) return 1;
	if (message != WM_PAINT || impl == nullptr) {
		return ::DefSubclassProc(window, message, wParam, lParam);
	}

	PAINTSTRUCT paint{};
	const HDC target = ::BeginPaint(window, &paint);
	if (target == nullptr) return 0;
	RECT client{};
	::GetClientRect(window, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	const bool buffered = impl->inputBuffer.Ensure(target, width, height);
	const HDC dc = buffered ? impl->inputBuffer.Dc() : target;
	const int frameState = ::SaveDC(dc);
	::SetMapMode(dc, MM_TEXT);
	::SetWindowOrgEx(dc, 0, 0, nullptr);
	::SetViewportOrgEx(dc, 0, 0, nullptr);
	(void)::SelectClipRgn(dc, nullptr);
	const int nativeState = ::SaveDC(dc);
	(void)::DefSubclassProc(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
		PRF_CLIENT | PRF_ERASEBKGND);
	if (nativeState != 0) (void)::RestoreDC(dc, nativeState);

	// Upstream shows the placeholder whenever the box is empty, focused or not.
	if (impl->inputModel.value.empty() && !impl->inputModel.placeholder.empty()) {
	const HGDIOBJ previousFont = impl->font.Get() == nullptr ? nullptr : ::SelectObject(dc, impl->font.Get());
	// Use the formatting rectangle so the placeholder shares the caret's left
	// inset, then center the single line vertically like Monaco's SCM editor.
	// `DT_END_ELLIPSIS` is important here: a long branch name must clip inside
	// the input instead of wrapping and making the short input look top-heavy.
	RECT bounds = impl->inputFormattingRect;
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, impl->palette.descriptionText.ToColorRef());
	::DrawTextW(dc, impl->inputModel.placeholder.c_str(), -1, &bounds,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	}
	if (frameState != 0) (void)::RestoreDC(dc, frameState);
	if (buffered) (void)impl->inputBuffer.Present(target, client);
	::EndPaint(window, &paint);
	return 0;
}

LRESULT CALLBACK CScmWorkbenchTool::ListSubclassProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* const impl = reinterpret_cast<Impl*>(data);
	if (message == WM_NCDESTROY) {
		::RemoveWindowSubclass(window, &CScmWorkbenchTool::ListSubclassProc, id);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (impl != nullptr && (window == impl->list || window == impl->graphList)) {
		if (message == WM_ERASEBKGND) return 1;
		if (message == WM_PAINT) {
			PAINTSTRUCT paint{};
			const HDC target = ::BeginPaint(window, &paint);
			if (window == impl->list) impl->PaintChangesListFrame(target, paint.rcPaint);
			else impl->PaintGraphListFrame(target);
			::EndPaint(window, &paint);
			return 0;
		}
	}
	// Both lists share this subclass. Row hover, group toggling, and the change
	// list's own hit tests belong to the change list alone; the graph list only
	// needs the focus and scroll handling below.
	const bool isChangeList = impl != nullptr && window == impl->list;
	if (isChangeList) {
		if (message == WM_MOUSEMOVE) {
			const auto hit = ::SendMessageW(window, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<WORD>(GET_X_LPARAM(lParam)), static_cast<WORD>(GET_Y_LPARAM(lParam))));
			const int index = HIWORD(hit) == 0 ? static_cast<int>(LOWORD(hit)) : -1;
			const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const bool moved = impl->listPointer.x != pointer.x || impl->listPointer.y != pointer.y;
			impl->listPointer = pointer;
			const int previousIndex = impl->listHoverIndex;
			if (previousIndex != index) {
				impl->listHoverIndex = index;
				impl->InvalidateListRow(previousIndex);
				impl->InvalidateListRow(index);
			} else if (moved && index >= 0 && static_cast<std::size_t>(index) < impl->rows.size()
				&& impl->rows[static_cast<std::size_t>(index)].header) {
				// Only group-row action hover depends on the pointer's position inside
				// an already-hovered row. Redraw that row, not every visible resource.
				impl->InvalidateListRow(index);
			}
			if (!impl->trackingListMouse) {
				TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
				impl->trackingListMouse = ::TrackMouseEvent(&track) != FALSE;
			}
		} else if (message == WM_MOUSELEAVE) {
			impl->trackingListMouse = false;
			const int previousIndex = impl->listHoverIndex;
			impl->listHoverIndex = -1;
			impl->InvalidateListRow(previousIndex);
		} else if (message == WM_LBUTTONDOWN) {
			const auto hit = ::SendMessageW(window, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<WORD>(GET_X_LPARAM(lParam)), static_cast<WORD>(GET_Y_LPARAM(lParam))));
			impl->listPointerDownIndex = HIWORD(hit) == 0 ? static_cast<int>(LOWORD(hit)) : -1;
		} else if (message == WM_LBUTTONUP) {
			const auto hit = ::SendMessageW(window, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<WORD>(GET_X_LPARAM(lParam)), static_cast<WORD>(GET_Y_LPARAM(lParam))));
			const int index = HIWORD(hit) == 0 ? static_cast<int>(LOWORD(hit)) : -1;
			if (index >= 0 && index == impl->listPointerDownIndex && index < static_cast<int>(impl->rows.size())
				&& impl->rows[static_cast<std::size_t>(index)].header) {
				const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				if (impl->InvokeGroupRowActionAt(index, pointer)) {
					impl->listPointerDownIndex = -1;
					return 0;
				}
				impl->ToggleGroupAt(index);
				impl->listPointerDownIndex = -1;
				return 0;
			}
			impl->listPointerDownIndex = -1;
		}
	}
	if (impl != nullptr && message == WM_MOUSEWHEEL) {
		// The overlay scrollbar hides the platform bar, and a list box with no
		// visible scroll bar drops WM_MOUSEWHEEL on the floor -- the wheel scrolls
		// nothing even though WS_VSCROLL is set and the SCROLLINFO is authoritative.
		// Scroll explicitly instead, exactly as the Explorer tree does.
		ScrollListBoxByWheel(window, wParam);
		if (window == impl->list) impl->UpdateListScrollbar();
		else if (window == impl->graphList) impl->UpdateGraphScrollbar();
		return 0;
	}
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if (impl != nullptr && (message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
		::InvalidateRect(window, nullptr, FALSE);
	}
	// Every path that can move or resize the list's own scroll state: the control
	// updates its SCROLLINFO inside DefSubclassProc, so the overlay is refreshed
	// after it rather than before.
	if (impl != nullptr && (message == WM_VSCROLL || message == WM_MOUSEWHEEL
		|| message == WM_KEYDOWN || message == WM_SIZE)) {
		if (window == impl->list) impl->UpdateListScrollbar();
		else if (window == impl->graphList) impl->UpdateGraphScrollbar();
	}
	return result;
}

CScmWorkbenchTool::CScmWorkbenchTool() : m_impl(std::make_unique<Impl>()) {}
CScmWorkbenchTool::~CScmWorkbenchTool() { Close(); }

bool CScmWorkbenchTool::Create(HWND parent)
{
	if (m_impl->closed || m_impl->window || !parent) return false;
	auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (!instance) instance = ::GetModuleHandleW(nullptr);
	if (!EnsureClass(instance)) return false;
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (!m_impl->window) return false;
	m_impl->list = ::CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
		| LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(1), instance, nullptr);
	if (!m_impl->list) { Close(); return false; }
	(void)::SetWindowSubclass(m_impl->list, &CScmWorkbenchTool::ListSubclassProc, 1,
		reinterpret_cast<DWORD_PTR>(m_impl.get()));
	// The list keeps WS_VSCROLL for keyboard semantics, while the overlay derives
	// its geometry from the stable row count/height/top-index contract. LISTBOX
	// SCROLLINFO can lag a resize by one paint and must not be presentation truth.
	Impl* const impl = m_impl.get();
	(void)m_impl->listScrollbar.Create(m_impl->window, m_impl->list, [impl](int topRow) {
		if (impl->list != nullptr) (void)::SendMessageW(impl->list, LB_SETTOPINDEX, static_cast<WPARAM>(topRow), 0);
	}, controls::OverlayScrollbarSource::ExplicitModel);
	m_impl->listScrollbar.SetHideNativeBar(true);
	// The Graph is a second owner-drawn list rather than a hand-scrolled canvas,
	// so it inherits the same keyboard, wheel, and overlay-scrollbar behaviour the
	// change list already has.
	m_impl->graphList = ::CreateWindowExW(0, L"LISTBOX", L"",
		WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
		0, 0, 0, 0, m_impl->window,
		reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kGraphControlId)), instance, nullptr);
	if (!m_impl->graphList) { Close(); return false; }
	(void)::SetWindowSubclass(m_impl->graphList, &CScmWorkbenchTool::ListSubclassProc,
		static_cast<UINT_PTR>(kGraphControlId), reinterpret_cast<DWORD_PTR>(m_impl.get()));
	(void)m_impl->graphScrollbar.Create(m_impl->window, m_impl->graphList, [impl](int topRow) {
		if (impl->graphList != nullptr) {
			(void)::SendMessageW(impl->graphList, LB_SETTOPINDEX, static_cast<WPARAM>(topRow), 0);
		}
	}, controls::OverlayScrollbarSource::ExplicitModel);
	m_impl->graphScrollbar.SetHideNativeBar(true);
	// Multiline and wrapping, because upstream's SCM input is a real multi-line
	// editor: a commit body is not one line. `ES_WANTRETURN` keeps Enter
	// inserting a newline, which is what leaves Ctrl+Enter free to mean commit.
	m_impl->input = ::CreateWindowExW(0, L"EDIT", L"",
		WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
		0, 0, 0, 0, m_impl->window,
		reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kInputControlId)), instance, nullptr);
	if (!m_impl->input) { Close(); return false; }
	(void)::SetWindowSubclass(m_impl->input, &CScmWorkbenchTool::InputSubclassProc,
		static_cast<UINT_PTR>(kInputControlId), reinterpret_cast<DWORD_PTR>(m_impl.get()));
	// The repository row's own hover text. `TTF_SUBCLASS` relays the pointer
	// messages, so the row needs no manual `TTM_RELAYEVENT` pump.
	m_impl->tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
		WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		m_impl->window, nullptr, instance, nullptr);
	if (m_impl->tooltip) ::SendMessageW(m_impl->tooltip, TTM_SETMAXTIPWIDTH, 0, 640);
	m_impl->NotifyWindow(m_impl->window, true);
	m_impl->Start();
	Refresh();
	return true;
}

void CScmWorkbenchTool::Layout(const RECT& rect, unsigned int dpi)
{
	if (m_impl->closed || !m_impl->window) return;
	const bool previousDefer = m_impl->deferChildRepaint;
	m_impl->deferChildRepaint = true;
	m_impl->dpi = dpi == 0 ? 96 : dpi;
	const bool fontChanged = m_impl->font.Dpi() != m_impl->dpi;
	if (fontChanged) {
		(void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
		::SendMessageW(m_impl->list, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), FALSE);
		if (m_impl->graphList) {
			::SendMessageW(m_impl->graphList, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), FALSE);
		}
		if (m_impl->input) {
			::SendMessageW(m_impl->input, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), FALSE);
		}
	}
	::SetWindowPos(m_impl->window, nullptr, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
	m_impl->LayoutBand();
	// The box is laid out before the Changes list; the list's body begins below
	// the current input height and ends above the reserved Graph frame.
	m_impl->LayoutInput();
	m_impl->LayoutActionButton();
	m_impl->LayoutList();
	m_impl->LayoutWelcome();
	m_impl->deferChildRepaint = previousDefer;
	if (!previousDefer) {
		// Commit parent chrome and both native controls before returning from the
		// geometry transaction. The Graph subclass turns its WM_PAINT into one
		// retained visible-row frame rather than exposing WM_DRAWITEM row batches.
		::RedrawWindow(m_impl->window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		::RedrawWindow(m_impl->list, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
		::RedrawWindow(m_impl->graphList, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
		// RedrawWindow completes WM_PAINT dispatch, but GDI may still hold the row
		// blits in this UI thread's batch. Flush only this thread's commands; unlike
		// DwmFlush this never waits for the compositor or another application thread.
		(void)::GdiFlush();
	}
}

void CScmWorkbenchTool::Activate()
{
	m_impl->active = true;
	// Upstream's `SCMViewPane.focus` focuses the rendered input widget when the
	// tree has no focused element, and the tree otherwise.
	const bool listHasFocusRow = m_impl->list != nullptr
		&& ::SendMessageW(m_impl->list, LB_GETCURSEL, 0, 0) != LB_ERR;
	if (m_impl->input != nullptr && m_impl->inputModel.visible && !listHasFocusRow) {
		::SetFocus(m_impl->input);
	} else if (m_impl->list != nullptr) {
		::SetFocus(m_impl->list);
	}
	Refresh();
}
void CScmWorkbenchTool::Deactivate() { m_impl->active = false; }
bool CScmWorkbenchTool::PreTranslateMessage(MSG& message) {
	if (!m_impl->active) return false;
	if (m_impl->input != nullptr && message.hwnd == m_impl->input) {
		if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN
			&& (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
			m_impl->AcceptInput();
			return true;
		}
		// Deliver the box's own keys here. The frame consults its legacy
		// accelerator table after this hook, and would otherwise claim ordinary
		// editing keys before the control ever saw them.
		::TranslateMessage(&message);
		::DispatchMessageW(&message);
		return true;
	}
	if (message.hwnd != m_impl->list) return false;
	if (message.message == WM_KEYDOWN) {
		if ((message.wParam == VK_RETURN || message.wParam == VK_SPACE) && m_impl->ToggleSelectedGroup()) return true;
		if (message.wParam == VK_RETURN) { m_impl->ActivateSelection(); return true; }
	}
	return false;
}

void CScmWorkbenchTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	m_impl->nativeSurface.ClearTarget();
	m_impl->nativeSurface.SetSink({});
	m_impl->closed = true;
	// Drop the provider first: after this point the tool stops refreshing, and a
	// surviving handle would keep advertising a repository nobody is watching.
	if (m_impl->publisher) { (void)m_impl->publisher->Retract(); m_impl->publisher.reset(); }
	m_impl->service = nullptr;
	m_impl->rows.clear();
	m_impl->resourceCount = 0;
	m_impl->inputModel = {};
	m_impl->NotifyWindow(nullptr, false);
	::SetEvent(m_impl->shared->stop);
	::SetEvent(m_impl->shared->wake);
	if (m_impl->worker.joinable() && m_impl->workerRetirement) {
		(void)workbench::WorkerRetirementService::Instance().Retire(
			std::move(m_impl->worker), std::move(*m_impl->workerRetirement), m_impl->shared);
		m_impl->workerRetirement.reset();
	}
	m_impl->shared.reset();
	m_impl->bandSegments.clear();
	m_impl->band = {};
	m_impl->welcomeSegments.clear();
	m_impl->welcomeModel = {};
	m_impl->hoveredWelcomeSegment = 0;
	m_impl->tooltipToolCount = 0;
	if (m_impl->tooltip && ::IsWindow(m_impl->tooltip)) ::DestroyWindow(m_impl->tooltip);
	m_impl->tooltip = nullptr;
	m_impl->listScrollbar.Destroy();
	m_impl->graphScrollbar.Destroy();
	m_impl->history.clear();
	m_impl->graphRows.clear();
	if (m_impl->window && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->list = nullptr;
	m_impl->graphList = nullptr;
	m_impl->input = nullptr;
}

void CScmWorkbenchTool::SetRoot(std::wstring root)
{
	if (m_impl->closed || !m_impl->shared || m_impl->root == root) return;
	m_impl->root = std::move(root);
	{
		std::lock_guard lock(m_impl->shared->mutex);
		m_impl->shared->root = m_impl->root;
		++m_impl->shared->generation;
	}
	m_impl->ResetForRootChange();
	if (!m_impl->closed && m_impl->shared->wake) ::SetEvent(m_impl->shared->wake);
}
void CScmWorkbenchTool::SetWelcomeWorkspaceState(EGitScmWelcomeWorkspaceState workspaceState)
{
	if (m_impl->welcomeWorkspaceState == workspaceState) return;
	m_impl->welcomeWorkspaceState = workspaceState;
	if (!m_impl->closed) m_impl->RebuildWelcome();
}
void CScmWorkbenchTool::SetPalette(const theme::ThemePalette& palette)
{
	m_impl->palette = palette;
	m_impl->UpdateListScrollbar();
	m_impl->UpdateGraphScrollbar();
	if (m_impl->window) {
		// Commit the palette across the complete native subtree. Parent-only
		// invalidation does not repaint EDIT/LISTBOX client areas and used to leave
		// the previous theme visible until the next unrelated interaction.
		(void)::RedrawWindow(m_impl->window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
	}
}
void CScmWorkbenchTool::SetInputLineCountRange(int minLineCount, int maxLineCount)
{
	const int lower = std::clamp(minLineCount, kScmInputLineCountLowerBound, kScmInputLineCountUpperBound);
	// Upstream bounds each key independently and does not reject the pair, so a
	// maximum below the minimum resolves to the minimum instead of failing.
	const int upper = std::max(lower,
		std::clamp(maxLineCount, kScmInputLineCountLowerBound, kScmInputLineCountUpperBound));
	if (lower == m_impl->inputMinLineCount && upper == m_impl->inputMaxLineCount) return;
	m_impl->inputMinLineCount = lower;
	m_impl->inputMaxLineCount = upper;
	m_impl->inputLineCount = std::clamp(m_impl->inputLineCount, lower, upper);
	// The box may already be laid out at the previous minimum, so re-measure from
	// the control's own line count rather than trusting the cached value.
	m_impl->UpdateInputHeight();
	m_impl->LayoutInput();
	m_impl->LayoutActionButton();
	m_impl->LayoutList();
	m_impl->LayoutWelcome();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}
void CScmWorkbenchTool::SetFileActivationCallback(FileActivationCallback callback) { m_impl->activateFile = std::move(callback); }
void CScmWorkbenchTool::SetStatusBarCommandsCallback(StatusBarCommandsCallback callback) { m_impl->statusBarCommands = std::move(callback); }
void CScmWorkbenchTool::SetCommandCallback(CommandCallback callback) { m_impl->runCommand = std::move(callback); }
void CScmWorkbenchTool::SetFileDecorationsCallback(FileDecorationsCallback callback)
{
	m_impl->fileDecorations = std::move(callback);
	// A consumer that arrives after the first refresh must not wait for the next
	// one to see a decorated tree, so it is handed what has already been published.
	if (m_impl->fileDecorations) m_impl->PublishFileDecorations();
}
void CScmWorkbenchTool::RepublishFileDecorations() { m_impl->PublishFileDecorations(); }
void CScmWorkbenchTool::SetTextResolver(TextResolver resolver)
{
	m_impl->text = std::move(resolver);
	if (!m_impl->closed) {
		m_impl->RebuildWelcome();
		// View headers and the Graph unsupported message are direct presentation
		// text, rather than provider data, so invalidate even if the welcome model
		// itself remained structurally identical.
		if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
	}
}
void CScmWorkbenchTool::SetPublicationTextResolver(PublicationTextResolver resolver)
{
	m_impl->publicationText = std::move(resolver);
	if (!m_impl->closed) m_impl->PublishAndRender();
}
void CScmWorkbenchTool::SetNativeSurfaceSink(NativeSurfaceSink sink)
{
	m_impl->nativeSurface.SetSink(std::move(sink));
}
bool CScmWorkbenchTool::SetNativeSurfaceTarget(
	const ScmNativeSurfaceTarget& target) noexcept
{
	return m_impl->nativeSurface.SetTarget(target);
}
void CScmWorkbenchTool::ClearNativeSurfaceTarget() noexcept
{
	m_impl->nativeSurface.ClearTarget();
}
void CScmWorkbenchTool::RefreshStrings()
{
	if (m_impl->closed) return;
	// The provider stores group labels and serialized diff titles, so redrawing
	// alone would retain the previous language. Re-publish before painting.
	m_impl->PublishAndRender();
}

void CScmWorkbenchTool::SetSourceControlService(SourceControlService* service)
{
	if (m_impl->service == service) return;
	// Retract through the old service before adopting the new one, so a handle
	// can never outlive the authority that issued it.
	if (m_impl->publisher) { (void)m_impl->publisher->Retract(); m_impl->publisher.reset(); }
	m_impl->service = service;
	if (service) m_impl->publisher = std::make_unique<GitScmPublisher>(service, BuiltinGitOwner());
	if (!m_impl->closed) m_impl->PublishAndRender();
}

void CScmWorkbenchTool::SetVisible(bool visible) { if (m_impl->window) ::ShowWindow(m_impl->window, visible ? SW_SHOW : SW_HIDE); }
void CScmWorkbenchTool::Refresh()
{
	if (m_impl->closed || !m_impl->shared->wake) return;
	{
		// A manual refresh supersedes work already in flight. The timer does not
		// advance this value, so an ordinary periodic refresh remains publishable.
		std::lock_guard lock(m_impl->shared->mutex);
		++m_impl->shared->generation;
	}
	::SetEvent(m_impl->shared->wake);
}
const GitScmState& CScmWorkbenchTool::State() const noexcept { return m_impl->state; }
std::size_t CScmWorkbenchTool::OpenRepositoryCount() const noexcept { return m_impl->openRepositoryCount; }
std::optional<GitHistoryItem> CScmWorkbenchTool::HistoryItem(std::wstring_view id) const
{
	const auto found = std::ranges::find(m_impl->history, id, &GitHistoryItem::id);
	if (found == m_impl->history.end()) return std::nullopt;
	return *found;
}

std::wstring CScmWorkbenchTool::CommitMessage() const { return m_impl->inputModel.value; }

void CScmWorkbenchTool::SetCommitMessage(std::wstring value)
{
	if (m_impl->closed || m_impl->inputModel.value == value) return;
	m_impl->inputModel.value = std::move(value);
	// The publisher is the authority for this value across provider
	// replacements, so it is updated even when no control exists yet.
	if (m_impl->publisher) (void)m_impl->publisher->SetInputBoxValue(ToUtf8(m_impl->inputModel.value));
	m_impl->WriteInputText(m_impl->inputModel.value);
	m_impl->UpdateInputHeight();
}
HWND CScmWorkbenchTool::GetHwnd() const noexcept { return m_impl->window; }

LRESULT CALLBACK CScmWorkbenchTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CScmWorkbenchTool*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	auto* self = reinterpret_cast<CScmWorkbenchTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self || !self->m_impl) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *self->m_impl;
	switch (message) {
	case WM_MOUSEWHEEL:
		impl.HandleMouseWheel(wParam, lParam);
		return 0;
	case WM_SIZE: {
		const bool previousDefer = impl.deferChildRepaint;
		impl.deferChildRepaint = true;
		impl.LayoutBand();
		impl.LayoutInput();
		impl.LayoutActionButton();
		impl.LayoutList();
		impl.LayoutWelcome();
		impl.deferChildRepaint = previousDefer;
		if (!previousDefer) {
			::RedrawWindow(window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		}
		return 0;
	}
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC target = ::BeginPaint(window, &paint);
		if (target == nullptr) return 0;
		RECT client{};
		::GetClientRect(window, &client);
		const int width = std::max(0L, client.right - client.left);
		const int height = std::max(0L, client.bottom - client.top);
		const bool buffered = impl.windowBuffer.Ensure(target, width, height);
		const HDC dc = buffered ? impl.windowBuffer.Dc() : target;
		const HBRUSH brush = ::CreateSolidBrush(impl.palette.sideBar.ToColorRef());
		::FillRect(dc, &client, brush);
		::DeleteObject(brush);
		const HGDIOBJ previousFont = impl.font.Get() ? ::SelectObject(dc, impl.font.Get()) : nullptr;
		::SetBkMode(dc, TRANSPARENT);
		if (impl.band.visible) {
			impl.PaintViewHeader(dc, impl.RepositoriesHeaderBounds(),
				EScmTextKey::RepositoriesTitle, L"Repositories", impl.repositoriesCollapsed);
		}
		if (impl.ChangesHeaderVisible()) {
			impl.PaintViewHeader(dc, impl.ChangesHeaderBounds(), EScmTextKey::ChangesTitle, L"Changes",
				impl.changesCollapsed);
		}
		impl.PaintInputFrame(dc);
		impl.PaintActionButton(dc);
		impl.PaintWelcome(dc);
		impl.PaintGraph(dc);
		impl.PaintBand(dc);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		if (buffered) (void)impl.windowBuffer.Present(target, paint.rcPaint);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (impl.BeginSashDrag(point)) return 0;
		break;
	}
	case WM_LBUTTONUP: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (impl.draggingSash) { impl.EndSashDrag(); return 0; }
		if (impl.ToggleSectionAt(point)) return 0;
		if (impl.InvokeSegmentAt(point)) return 0;
		if (impl.InvokeActionButtonAt(point)) return 0;
		if (impl.InvokeWelcomeSegmentAt(point)) return 0;
		break;
	}
	case WM_CAPTURECHANGED:
		// Capture can be taken away, so the drag ends here as well as on the button
		// up: a drag that believed it was still running would move the boundary on
		// the next unrelated mouse move.
		if (impl.draggingSash) { impl.draggingSash = false; }
		return 0;
	case WM_SETCURSOR: {
		if (reinterpret_cast<HWND>(wParam) != window) break;
		POINT point{};
		if (!::GetCursorPos(&point) || !::ScreenToClient(window, &point)) break;
		if (impl.draggingSash || impl.PointInSash(point)) {
			::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
			return TRUE;
		}
		if (impl.SectionHeaderAt(point) != 0) {
			::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		const auto index = impl.SegmentIndexAt(point);
		if (index != 0 && !impl.bandSegments[index - 1].command.empty()) {
			::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		if (impl.ActionButtonPartAt(point) != 0) {
			::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		if (impl.WelcomeSegmentIndexAt(point) != 0) {
			::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (impl.draggingSash) { impl.UpdateSashDrag(point); return 0; }
		const auto index = impl.SegmentIndexAt(point);
		// Only an action highlights: the name segment carries a title but runs
		// nothing, and highlighting it would advertise a click that does nothing.
		impl.SetHoveredSegment(index != 0 && impl.bandSegments[index - 1].kind == EBandSegment::Action ? index : 0);
		impl.SetHoveredActionButtonPart(impl.ActionButtonPartAt(point));
		impl.SetHoveredWelcomeSegment(impl.WelcomeSegmentIndexAt(point));
		if (!impl.trackingMouse) {
			TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
			impl.trackingMouse = ::TrackMouseEvent(&track) != FALSE;
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		impl.trackingMouse = false;
		impl.SetHoveredSegment(0);
		impl.SetHoveredActionButtonPart(0);
		impl.SetHoveredWelcomeSegment(0);
		return 0;
	case WM_NOTIFY: {
		auto* const header = reinterpret_cast<NMHDR*>(lParam);
		if (header == nullptr || header->hwndFrom != impl.tooltip) break;
		if (header->code != TTN_GETDISPINFOW) break;
		// The tooltip keeps the returned pointer, so the text has to outlive this
		// call; `tooltipText` is that storage and is overwritten per request.
		const auto index = static_cast<std::size_t>(header->idFrom);
		if (index == 0 || index > impl.bandSegments.size()) break;
		impl.tooltipText = impl.bandSegments[index - 1].tooltip;
		reinterpret_cast<NMTTDISPINFOW*>(lParam)->lpszText = impl.tooltipText.data();
		return 0;
	}
	case WM_CONTEXTMENU: {
		// The list is a child window, so its own `DefWindowProc` forwards the
		// message here while `wParam` still names where the gesture landed.
		const HWND source = reinterpret_cast<HWND>(wParam);
		if (source != impl.list && source != impl.graphList) break;
		const bool fromKeyboard = lParam == static_cast<LPARAM>(-1);
		const POINT screen{
			static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
			static_cast<LONG>(static_cast<short>(HIWORD(lParam))),
		};
		if (source == impl.graphList) impl.ShowHistoryItemContextMenu(screen, fromKeyboard);
		else impl.ShowContextMenu(screen, fromKeyboard);
		return 0;
	}
	case WM_DRAWITEM: {
		auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
		if (draw != nullptr && draw->CtlID == 1 && draw->hwndItem == impl.list
			&& draw->itemID != static_cast<UINT>(-1)) {
			RECT client{};
			::GetClientRect(impl.list, &client);
			const bool buffered = impl.listBuffer.Ensure(draw->hDC,
				std::max(0L, client.right - client.left), std::max(0L, client.bottom - client.top));
			const HDC dc = buffered ? impl.listBuffer.Dc() : draw->hDC;
			const HGDIOBJ previousFont = impl.font.Get() ? ::SelectObject(dc, impl.font.Get()) : nullptr;
			impl.PaintRow(dc, static_cast<int>(draw->itemID), draw->rcItem, draw->itemState);
			if (previousFont != nullptr) ::SelectObject(dc, previousFont);
			if (buffered) (void)impl.listBuffer.Present(draw->hDC, draw->rcItem);
			return TRUE;
		}
		if (draw != nullptr && draw->CtlID == kGraphControlId && draw->hwndItem == impl.graphList
			&& draw->itemID != static_cast<UINT>(-1)) {
			RECT client{};
			::GetClientRect(impl.graphList, &client);
			const bool buffered = impl.graphBuffer.Ensure(draw->hDC,
				std::max(0L, client.right - client.left), std::max(0L, client.bottom - client.top));
			const HDC dc = buffered ? impl.graphBuffer.Dc() : draw->hDC;
			const HGDIOBJ previousFont = impl.font.Get() ? ::SelectObject(dc, impl.font.Get()) : nullptr;
			impl.PaintGraphRow(dc, static_cast<int>(draw->itemID), draw->rcItem, draw->itemState);
			if (previousFont != nullptr) ::SelectObject(dc, previousFont);
			if (buffered) (void)impl.graphBuffer.Present(draw->hDC, draw->rcItem);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLOREDIT: {
		if (reinterpret_cast<HWND>(lParam) != impl.input) break;
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.inputBackground.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.inputBackground.ToColorRef());
		return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
	}
	case WM_CTLCOLORLISTBOX: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.sideBar.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.sideBar.ToColorRef());
		return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == 1 && HIWORD(wParam) == LBN_DBLCLK) { impl.ActivateSelection(); return 0; }
		if (LOWORD(wParam) == kInputControlId && HIWORD(wParam) == EN_CHANGE) { impl.OnInputChanged(); return 0; }
		break;
	case kResultMessage: {
		auto result = impl.TakeLatest();
		std::uint64_t generation = 0;
		{
			std::lock_guard lock(impl.shared->mutex);
			generation = impl.shared->generation;
		}
		if (result && result->generation == generation) {
			impl.ApplyWorkerResult(std::move(result));
		}
		return 0;
	}
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::scm
