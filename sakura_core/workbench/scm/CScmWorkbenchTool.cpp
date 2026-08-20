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

#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
	RGB(0x40, 0xC8, 0xAE), RGB(0x69, 0x29, 0xC4),
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

std::vector<icons::SLabelRun> ParseRuns(std::wstring_view label)
{
	// No contributed-icon registry here: the built-in Git provider's own titles
	// only ever name codicons, and passing a registry this tool does not own
	// would let another extension's `$(name)` change what Git renders.
	return icons::ParseLabelWithIcons(label, icons::CCodiconFont::Instance().FaceName());
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
	std::vector<std::unique_ptr<WorkerResult>> pending;
};
struct SharedState {
	std::mutex mutex;
	std::wstring root;
	std::atomic<std::uint64_t> generation{ 1 };
	std::shared_ptr<Gate> gate = std::make_shared<Gate>();
	HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
	const auto raw = result.get();
	const auto gate = shared->gate;
	std::lock_guard lock(gate->mutex);
	if (!gate->alive || gate->window == nullptr) return;
	gate->pending.push_back(std::move(result));
	if (!::PostMessageW(gate->window, kResultMessage, 0, reinterpret_cast<LPARAM>(raw))) gate->pending.pop_back();
}

void WorkerMain(std::shared_ptr<SharedState> shared)
{
	HANDLE waits[] = { shared->stop, shared->wake };
	for (;;) {
		const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kRefreshMilliseconds);
		if (wait == WAIT_OBJECT_0) return;
		std::wstring root;
		{
			std::lock_guard lock(shared->mutex);
			root = shared->root;
		}
		const auto generation = shared->generation.load(std::memory_order_acquire);
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
	HWND window{};
	HWND list{};
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

	void Start() { if (!worker.joinable()) worker = std::thread(WorkerMain, shared); }
	void NotifyWindow(HWND target, bool alive) {
		std::lock_guard lock(shared->gate->mutex);
		shared->gate->window = target;
		shared->gate->alive = alive;
		if (!alive) shared->gate->pending.clear();
	}
	std::unique_ptr<WorkerResult> Take(WorkerResult* raw) {
		std::lock_guard lock(shared->gate->mutex);
		auto& values = shared->gate->pending;
		const auto found = std::find_if(values.begin(), values.end(), [raw](const auto& item) { return item.get() == raw; });
		if (found == values.end()) return {};
		auto value = std::move(*found);
		values.erase(found);
		return value;
	}
	//! Apply the current Git state to the service, then re-render from what the
	//! service holds. Publishing and rendering are one step so the list can never
	//! show a generation the service has already replaced.
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
		RebuildRows(providers, decorations, operands);
		RebuildBand(providers);
		RebuildInput(providers);
		RebuildActionButton(providers);
		Populate(selected);
		PublishStatusBarCommands(providers);
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
		::MoveWindow(input, bounds.left, bounds.top, width, height, TRUE);
		// A multiline edit has no vertical margin message, so the padding is the
		// formatting rectangle; without it the first line would hug the border.
		const LONG pad = icons::ScaleDip(kInputPaddingDip, dpi);
		RECT formatting{ pad, pad, std::max(pad, width - pad), std::max(pad, height - pad) };
		::SendMessageW(input, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
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
		::InvalidateRect(input, nullptr, TRUE);
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
		if (wasEmpty != inputModel.value.empty()) ::InvalidateRect(input, nullptr, TRUE);
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
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
			if (item.commandId == "git.commit") resolve(EScmTextKey::GitCommitAction, item.title);
			else if (item.commandId == "git.commitAmend") resolve(EScmTextKey::GitCommitAmendAction, item.title);
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
		::InvalidateRect(window, &bounds, TRUE);
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
		(void)runCommand(item.commandId, {});
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
		const HBRUSH brush = ::CreateSolidBrush(palette.border.ToColorRef());
		if (brush == nullptr) return;
		::FrameRect(dc, &frame, brush);
		::DeleteObject(brush);
	}
	[[nodiscard]] RECT BandBounds() const
	{
		return ViewBounds(ViewStack().repositoryRow);
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
			::MoveWindow(list, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
				std::max(0L, bounds.bottom - bounds.top), TRUE);
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
			::MoveWindow(graphList, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
				std::max(0L, bounds.bottom - bounds.top), TRUE);
		}
		UpdateGraphScrollbar();
	}
	void UpdateGraphScrollbar()
	{
		graphScrollbar.SetDpi(dpi);
		graphScrollbar.SetColors(controls::OverlayScrollbarColors{
			palette.sideBar.ToColorRef(),
			palette.raised.ToColorRef(),
			palette.border.ToColorRef(),
			palette.secondaryText.ToColorRef() });
		graphScrollbar.Update();
	}
	//! Feeds the overlay the current DPI and the Side Bar tokens the Explorer maps
	//! its own overlay from, then lets it reposition itself over the list.
	void UpdateListScrollbar()
	{
		listScrollbar.SetDpi(dpi);
		listScrollbar.SetColors(controls::OverlayScrollbarColors{
			palette.sideBar.ToColorRef(),
			palette.raised.ToColorRef(),
			palette.border.ToColorRef(),
			palette.secondaryText.ToColorRef() });
		listScrollbar.Update();
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
	}
	//! Measure the row and store its regions. Kept separate from painting so a hit
	//! test never depends on whether a paint has happened yet.
	void LayoutBand()
	{
		bandSegments.clear();
		bandCountRect = RECT{};
		if (!window || !band.visible) { SyncTooltips(); return; }
		const HDC dc = ::GetDC(window);
		if (dc == nullptr) return;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
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

		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::ReleaseDC(window, dc);
		SyncTooltips();
	}
	void PaintBand(HDC dc)
	{
		if (!band.visible || bandSegments.empty()) return;
		const int iconSide = icons::ScaleDip(kRepositoryIconDip, dpi);
		const int gap = icons::ScaleDip(kRepositoryIconGapDip, dpi);
		const int actionInset = icons::ScaleDip(kActionInsetDip, dpi);
		for (std::size_t index = 0; index < bandSegments.size(); ++index) {
			const auto& segment = bandSegments[index];
			if (segment.rect.right <= segment.rect.left) continue;
			if (segment.kind == EBandSegment::Name) {
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
		PaintViewHeader(dc, GraphHeaderBounds(), EScmTextKey::GraphTitle, L"Graph", graphCollapsed);
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
	}
	void PopulateGraph()
	{
		if (!graphList) return;
		::SendMessageW(graphList, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(graphList, LB_RESETCONTENT, 0, 0);
		for (const auto& item : history) {
			::SendMessageW(graphList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.subject.c_str()));
		}
		::SendMessageW(graphList, LB_SETITEMHEIGHT, 0, icons::ScaleDip(kGraphRowHeightDip, dpi));
		::SendMessageW(graphList, WM_SETREDRAW, TRUE, 0);
		// The same stale-row rule the change list follows: complete the child
		// redraw at the transition that replaced its items.
		::RedrawWindow(graphList, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
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
		const int radius = icons::ScaleDip(kGraphCircleRadiusDip, dpi);
		const LONG middle = (bounds.top + bounds.bottom) / 2;
		const auto laneX = [&](std::size_t position) {
			return static_cast<LONG>(bounds.left + inset + lane / 2 + static_cast<LONG>(position) * lane);
		};
		// A lane entering this row is drawn to its middle and a lane leaving it
		// from the middle down, so a lane that appears or disappears here draws
		// only half a segment: that is what makes a branch look like it starts,
		// and a merge look like it ends.
		const auto stroke = [&](std::size_t position, std::size_t color, LONG top, LONG bottom) {
			const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, icons::ScaleDip(1, dpi)), LaneColor(color));
			if (pen == nullptr) return;
			const HGDIOBJ previousPen = ::SelectObject(dc, pen);
			::MoveToEx(dc, laneX(position), top, nullptr);
			::LineTo(dc, laneX(position), bottom);
			::SelectObject(dc, previousPen);
			::DeleteObject(pen);
		};
		for (std::size_t position = 0; position < row.inputSwimlanes.size(); ++position) {
			stroke(position, row.inputSwimlanes[position].colorIndex, bounds.top, middle);
		}
		for (std::size_t position = 0; position < row.outputSwimlanes.size(); ++position) {
			stroke(position, row.outputSwimlanes[position].colorIndex, middle, bounds.bottom);
		}
		const COLORREF circleColor = LaneColor(row.circleColorIndex);
		const HBRUSH circle = ::CreateSolidBrush(circleColor);
		const HPEN circlePen = ::CreatePen(PS_SOLID, 1, circleColor);
		if (circle != nullptr && circlePen != nullptr) {
			const HGDIOBJ previousBrush = ::SelectObject(dc, circle);
			const HGDIOBJ previousPen = ::SelectObject(dc, circlePen);
			const LONG x = laneX(row.circleLane);
			::Ellipse(dc, x - radius, middle - radius, x + radius, middle + radius);
			::SelectObject(dc, previousPen);
			::SelectObject(dc, previousBrush);
		}
		if (circlePen != nullptr) ::DeleteObject(circlePen);
		if (circle != nullptr) ::DeleteObject(circle);

		const std::size_t lanes = std::max<std::size_t>(1,
			std::max(row.inputSwimlanes.size(), row.outputSwimlanes.size()));
		LONG cursor = laneX(lanes) + icons::ScaleDip(4, dpi);
		const COLORREF textColor = selected && focused ? palette.highlightText.ToColorRef()
			: palette.primaryText.ToColorRef();

		// The author/date column is reserved before the subject is laid out, so a
		// long subject is ellipsized rather than drawn through it.
		std::wstring trailing = item.authorName;
		if (const std::wstring date = FormatHistoryDate(item.authorTimestamp); !date.empty()) {
			trailing = trailing.empty() ? date : trailing + L", " + date;
		}
		LONG subjectRight = bounds.right - inset;
		if (!trailing.empty()) {
			SIZE extent{};
			(void)::GetTextExtentPoint32W(dc, trailing.c_str(), static_cast<int>(trailing.size()), &extent);
			RECT trailingRect{ std::max<LONG>(cursor, bounds.right - inset - extent.cx), bounds.top,
				bounds.right - inset, bounds.bottom };
			::SetTextColor(dc, selected && focused ? palette.highlightText.ToColorRef()
				: palette.descriptionText.ToColorRef());
			::DrawTextW(dc, trailing.c_str(), static_cast<int>(trailing.size()), &trailingRect,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			subjectRight = trailingRect.left - icons::ScaleDip(6, dpi);
		}

		// Upstream badges each decoration beside the subject. The badge takes this
		// commit's own lane colour, so a branch tip stays attached to its line.
		for (const auto& ref : item.refs) {
			if (ref.name.empty() || cursor >= subjectRight) break;
			SIZE extent{};
			(void)::GetTextExtentPoint32W(dc, ref.name.c_str(), static_cast<int>(ref.name.size()), &extent);
			const LONG padding = icons::ScaleDip(4, dpi);
			const LONG right = std::min<LONG>(subjectRight, cursor + extent.cx + 2 * padding);
			RECT badge{ cursor, bounds.top + icons::ScaleDip(3, dpi), right,
				bounds.bottom - icons::ScaleDip(3, dpi) };
			if (badge.right <= badge.left) break;
			const HBRUSH fill = ::CreateSolidBrush(circleColor);
			const HPEN pen = ::CreatePen(PS_SOLID, 1, circleColor);
			if (fill != nullptr && pen != nullptr) {
				const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
				const HGDIOBJ previousPen = ::SelectObject(dc, pen);
				const int corner = views::WelcomeButtonCornerRadius(dpi);
				::RoundRect(dc, badge.left, badge.top, badge.right, badge.bottom, corner, corner);
				::SelectObject(dc, previousPen);
				::SelectObject(dc, previousBrush);
			}
			if (pen != nullptr) ::DeleteObject(pen);
			if (fill != nullptr) ::DeleteObject(fill);
			RECT label{ badge.left + padding, bounds.top, badge.right, bounds.bottom };
			::SetTextColor(dc, palette.buttonForeground.ToColorRef());
			::DrawTextW(dc, ref.name.c_str(), static_cast<int>(ref.name.size()), &label,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			cursor = right + icons::ScaleDip(4, dpi);
		}

		if (cursor < subjectRight) {
			RECT subject{ cursor, bounds.top, subjectRight, bounds.bottom };
			::SetTextColor(dc, textColor);
			::DrawTextW(dc, item.subject.c_str(), static_cast<int>(item.subject.size()), &subject,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
		LayoutBand();
		LayoutInput();
		LayoutActionButton();
		LayoutList();
		LayoutWelcome();
		if (window) ::InvalidateRect(window, nullptr, TRUE);
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
		if (window) ::InvalidateRect(window, nullptr, TRUE);
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
			RECT label{ iconRect.right + icons::ScaleDip(4, dpi), bounds.top,
				std::max<LONG>(iconRect.right + icons::ScaleDip(4, dpi), countRect.left - icons::ScaleDip(5, dpi)), bounds.bottom };
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
			::InvalidateRect(window, &bounds, TRUE);
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
			const RECT bounds = BandBounds();
			::InvalidateRect(window, &bounds, TRUE);
		}
	}
	//! Run the row's toolbar action, which is upstream's `StatusBarAction.run`.
	bool InvokeSegmentAt(POINT point)
	{
		const auto index = SegmentIndexAt(point);
		if (index == 0) return false;
		const auto& segment = bandSegments[index - 1];
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
	void RebuildRows(const std::vector<ScmProviderState>& providers,
		const std::vector<GitResourceDecoration>& decorations,
		const std::vector<GitResourceOperand>& operands) {
		rows.clear();
		resourceCount = 0;
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
				rows.push_back(std::move(header));
				resourceCount += group.resources.size();
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
						const auto operand = std::find_if(operands.begin(), operands.end(),
							[&uri, &groupKind](const GitResourceOperand& candidate) {
								return candidate.resource.group == *groupKind && candidate.resourceUri == uri;
							});
						if (operand != operands.end()) row.operand = operand->resource;
					}
					const auto found = std::find_if(decorations.begin(), decorations.end(),
						[&uri](const GitResourceDecoration& decoration) { return decoration.resourceUri == uri; });
					// A provider we did not publish has no badge of ours, and
					// inventing one would claim a status nobody reported.
					row.statusLetter = found == decorations.end() ? L' ' : found->letter;
					row.label = row.windowsPath.empty() ? uri : RelativeDisplayPath(root, row.windowsPath);
					row.text = row.label;
					rows.push_back(std::move(row));
				}
			}
		}
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
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_RESETCONTENT, 0, 0);
		for (const auto& row : rows) {
			::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.label.c_str()));
		}
		::SendMessageW(list, LB_SETITEMHEIGHT, 0, icons::ScaleDip(kRepositoryRowHeightDip, dpi));
		RestoreListSelection(selected);
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		// A Git refresh can shrink the owner-drawn list.  Invalidation alone leaves
		// that newly empty tail pending until a later message-loop turn, which lets
		// pixels from rows that no longer exist remain on the composited screen.
		// Complete this child redraw at the same state transition that replaces its
		// items so the screen and the model cannot temporarily disagree.
		::RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
		UpdateListScrollbar();
		::InvalidateRect(window, nullptr, TRUE);
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
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if (message != WM_PAINT || impl == nullptr) return result;
	// Upstream shows the placeholder whenever the box is empty, focused or not.
	if (!impl->inputModel.value.empty() || impl->inputModel.placeholder.empty()) return result;
	const HDC dc = ::GetDC(window);
	if (dc == nullptr) return result;
	const HGDIOBJ previousFont = impl->font.Get() == nullptr ? nullptr : ::SelectObject(dc, impl->font.Get());
	// The formatting rectangle, so the placeholder starts exactly where the
	// caret does rather than at the control's own corner.
	RECT bounds{};
	::SendMessageW(window, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&bounds));
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, impl->palette.descriptionText.ToColorRef());
	::DrawTextW(dc, impl->inputModel.placeholder.c_str(), -1, &bounds,
		DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	::ReleaseDC(window, dc);
	return result;
}

LRESULT CALLBACK CScmWorkbenchTool::ListSubclassProc(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* const impl = reinterpret_cast<Impl*>(data);
	if (message == WM_NCDESTROY) {
		::RemoveWindowSubclass(window, &CScmWorkbenchTool::ListSubclassProc, id);
		return ::DefSubclassProc(window, message, wParam, lParam);
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
			if (impl->listHoverIndex != index) { impl->listHoverIndex = index; ::InvalidateRect(window, nullptr, FALSE); }
			if (!impl->trackingListMouse) {
				TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
				impl->trackingListMouse = ::TrackMouseEvent(&track) != FALSE;
			}
		} else if (message == WM_MOUSELEAVE) {
			impl->trackingListMouse = false;
			impl->listHoverIndex = -1;
			::InvalidateRect(window, nullptr, FALSE);
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
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (!m_impl->window) return false;
	m_impl->list = ::CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
		| LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(1), instance, nullptr);
	if (!m_impl->list) { Close(); return false; }
	(void)::SetWindowSubclass(m_impl->list, &CScmWorkbenchTool::ListSubclassProc, 1,
		reinterpret_cast<DWORD_PTR>(m_impl.get()));
	// The list keeps WS_VSCROLL so its SCROLLINFO stays authoritative; the overlay
	// hides the platform bar and draws the same themed one the Explorer draws.
	Impl* const impl = m_impl.get();
	(void)m_impl->listScrollbar.Create(m_impl->window, m_impl->list, [impl](int topRow) {
		if (impl->list != nullptr) (void)::SendMessageW(impl->list, LB_SETTOPINDEX, static_cast<WPARAM>(topRow), 0);
	});
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
	});
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
	m_impl->dpi = dpi == 0 ? 96 : dpi;
	if (m_impl->font.Dpi() != m_impl->dpi) (void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
	::SendMessageW(m_impl->list, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), TRUE);
	if (m_impl->graphList) {
		::SendMessageW(m_impl->graphList, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), TRUE);
	}
	if (m_impl->input) {
		::SendMessageW(m_impl->input, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), TRUE);
	}
	::SetWindowPos(m_impl->window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
	m_impl->LayoutBand();
	// The box is laid out before the Changes list; the list's body begins below
	// the current input height and ends above the reserved Graph frame.
	m_impl->LayoutInput();
	m_impl->LayoutActionButton();
	m_impl->LayoutList();
	m_impl->LayoutWelcome();
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
	if (m_impl->worker.joinable()) m_impl->worker.join();
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
	if (m_impl->shared->stop) { ::CloseHandle(m_impl->shared->stop); m_impl->shared->stop = nullptr; }
	if (m_impl->shared->wake) { ::CloseHandle(m_impl->shared->wake); m_impl->shared->wake = nullptr; }
}

void CScmWorkbenchTool::SetRoot(std::wstring root)
{
	m_impl->root = std::move(root);
	{
		std::lock_guard lock(m_impl->shared->mutex);
		m_impl->shared->root = m_impl->root;
	}
	m_impl->shared->generation.fetch_add(1, std::memory_order_acq_rel);
	Refresh();
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
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, TRUE);
	if (m_impl->list) ::InvalidateRect(m_impl->list, nullptr, TRUE);
	if (m_impl->graphList) ::InvalidateRect(m_impl->graphList, nullptr, TRUE);
	m_impl->UpdateListScrollbar();
	m_impl->UpdateGraphScrollbar();
	// The box paints its own background through `WM_CTLCOLOREDIT`, so it needs
	// its own invalidation: the parent's does not reach a child's client area.
	if (m_impl->input) ::InvalidateRect(m_impl->input, nullptr, TRUE);
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
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, TRUE);
}
void CScmWorkbenchTool::SetFileActivationCallback(FileActivationCallback callback) { m_impl->activateFile = std::move(callback); }
void CScmWorkbenchTool::SetStatusBarCommandsCallback(StatusBarCommandsCallback callback) { m_impl->statusBarCommands = std::move(callback); }
void CScmWorkbenchTool::SetCommandCallback(CommandCallback callback) { m_impl->runCommand = std::move(callback); }
void CScmWorkbenchTool::SetTextResolver(TextResolver resolver)
{
	m_impl->text = std::move(resolver);
	if (!m_impl->closed) {
		m_impl->RebuildWelcome();
		// View headers and the Graph unsupported message are direct presentation
		// text, rather than provider data, so invalidate even if the welcome model
		// itself remained structurally identical.
		if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, TRUE);
	}
}
void CScmWorkbenchTool::SetPublicationTextResolver(PublicationTextResolver resolver)
{
	m_impl->publicationText = std::move(resolver);
	if (!m_impl->closed) m_impl->PublishAndRender();
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
void CScmWorkbenchTool::Refresh() { if (!m_impl->closed && m_impl->shared->wake) ::SetEvent(m_impl->shared->wake); }
const GitScmState& CScmWorkbenchTool::State() const noexcept { return m_impl->state; }
std::size_t CScmWorkbenchTool::OpenRepositoryCount() const noexcept { return m_impl->openRepositoryCount; }
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
	case WM_SIZE: {
		impl.LayoutBand();
		impl.LayoutInput();
		impl.LayoutActionButton();
		impl.LayoutList();
		impl.LayoutWelcome();
		return 0;
	}
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		const HBRUSH brush = ::CreateSolidBrush(impl.palette.sideBar.ToColorRef());
		::FillRect(dc, &paint.rcPaint, brush);
		::DeleteObject(brush);
		if (impl.font.Get()) ::SelectObject(dc, impl.font.Get());
		::SetBkMode(dc, TRANSPARENT);
		if (impl.band.visible) {
			impl.PaintViewHeader(dc, impl.RepositoriesHeaderBounds(),
				EScmTextKey::RepositoriesTitle, L"Repositories", impl.repositoriesCollapsed);
		}
		impl.PaintBand(dc);
		if (impl.ChangesHeaderVisible()) {
			impl.PaintViewHeader(dc, impl.ChangesHeaderBounds(), EScmTextKey::ChangesTitle, L"Changes",
				impl.changesCollapsed);
		}
		impl.PaintInputFrame(dc);
		impl.PaintActionButton(dc);
		impl.PaintWelcome(dc);
		impl.PaintGraph(dc);
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
		if (reinterpret_cast<HWND>(wParam) != impl.list) break;
		const bool fromKeyboard = lParam == static_cast<LPARAM>(-1);
		const POINT screen{
			static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
			static_cast<LONG>(static_cast<short>(HIWORD(lParam))),
		};
		impl.ShowContextMenu(screen, fromKeyboard);
		return 0;
	}
	case WM_DRAWITEM: {
		auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
		if (draw != nullptr && draw->CtlID == 1 && draw->hwndItem == impl.list
			&& draw->itemID != static_cast<UINT>(-1)) {
			impl.PaintRow(draw->hDC, static_cast<int>(draw->itemID), draw->rcItem, draw->itemState);
			return TRUE;
		}
		if (draw != nullptr && draw->CtlID == kGraphControlId && draw->hwndItem == impl.graphList
			&& draw->itemID != static_cast<UINT>(-1)) {
			impl.PaintGraphRow(draw->hDC, static_cast<int>(draw->itemID), draw->rcItem, draw->itemState);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLOREDIT: {
		if (reinterpret_cast<HWND>(lParam) != impl.input) break;
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.raised.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.raised.ToColorRef());
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
		auto result = impl.Take(reinterpret_cast<WorkerResult*>(lParam));
		if (result && result->generation == impl.shared->generation.load(std::memory_order_acquire)) {
			impl.state = std::move(result->state);
			impl.execution = result->execution;
			impl.failureReason = std::move(result->failureReason);
			impl.PublishAndRender();
			// After the render, because the graph's own layout depends on the
			// bands the change list and welcome content just settled into.
			impl.ApplyHistory(std::move(result->history), result->historyRead);
		}
		return 0;
	}
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::scm
