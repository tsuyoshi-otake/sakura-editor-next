/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/CProjectsPage.h"

#include "CSelectLang.h"
#include "sakura_rc.h"
#include "workbench/icons/CodiconGlyphPainter.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/projects/AgentSessionManagement.h"
#include "workbench/projects/ProjectsListPane.h"
#include "workbench/projects/ProjectsModel.h"
#include "workbench/viewcontainer/ViewPaneChrome.h"
#include "workbench/worktree/GitWorktreeDiscoverySource.h"

#include <CommCtrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::projects {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraProjectsPage";
constexpr UINT_PTR kRefreshTimer = 0x7072;
constexpr UINT kRefreshPollMilliseconds = 50;
constexpr UINT kAgentsChangedMessage = WM_APP + 0x72;
//! Polls the one in-flight session-history discovery (#406); runs only while
//! a discovery is in flight, like kRefreshTimer for Git.
constexpr UINT_PTR kSessionsTimer = 0x7073;
constexpr UINT kSessionsPollMilliseconds = 50;
//! #410: an agent CLI writes its session file only after the first message,
//! so its row appears first. While a present agent still has no session of its
//! own, discovery reruns after 5 s, doubling to 60 s, at most 12 times per
//! activation or agent-set change (about 9 minutes). F5 still refreshes.
constexpr UINT_PTR kSessionsCatchUpTimer = 0x7074;
constexpr UINT kSessionsCatchUpFirstDelayMilliseconds = 5000;
constexpr UINT kSessionsCatchUpMaximumDelayMilliseconds = 60000;
constexpr std::size_t kSessionsCatchUpMaximumAttempts = 12;
//! #408: polls the one rename or delete running in an agent's store.
constexpr UINT_PTR kSessionOperationTimer = 0x7075;
constexpr UINT kSessionOperationPollMilliseconds = 50;
//! Sessions context menu: command id = this + the index in the built menu.
constexpr UINT kSessionMenuFirstId = 0x5200;
constexpr int kSessionPinIconDip = 16;
constexpr int kRowHeightDip = 24;
constexpr int kRowInsetDip = 10;
//! One nesting level: the Explorer TreeView's own indent (#409).
constexpr int kChildIndentDip = 12;
constexpr int kColumnGapDip = 8;
constexpr int kTooltipMaxWidthDip = 480;
//! #411: the Projects View's list and the Sessions View's list.
constexpr int kProjectsListControlId = 1;
constexpr int kSessionsListControlId = 2;
//! The Sessions View's share of the two expanded bodies until a sash drag.
constexpr double kDefaultSessionsShare = 0.5;
constexpr UINT kRemoveProjectMenuId = 1;
constexpr std::size_t kMaximumBranchRepositoriesPerProject = 8;
constexpr std::size_t kMaximumBranchRequestsPerRefresh = 64;

ProjectsTexts LoadProjectsTexts()
{
	ProjectsTexts texts;
	const auto load = [](std::wstring& target, const UINT id) {
		const auto value = CSelectLang::LoadStringW(id);
		if (!value.empty()) target.assign(value);
	};
	load(texts.currentFolder, STR_WORKBENCH_PROJECTS_CURRENT_FOLDER);
	load(texts.currentWorkspace, STR_WORKBENCH_PROJECTS_CURRENT_WORKSPACE);
	load(texts.folder, STR_WORKBENCH_PROJECTS_FOLDER);
	load(texts.workspace, STR_WORKBENCH_PROJECTS_WORKSPACE);
	load(texts.detached, STR_WORKBENCH_PROJECTS_DETACHED);
	load(texts.bare, STR_WORKBENCH_PROJECTS_BARE);
	load(texts.locked, STR_WORKBENCH_PROJECTS_LOCKED);
	load(texts.prunable, STR_WORKBENCH_PROJECTS_PRUNABLE);
	load(texts.linkedWorktreesFormat, STR_WORKBENCH_PROJECTS_LINKED_COUNT);
	load(texts.primary, STR_WORKBENCH_PROJECTS_PRIMARY);
	load(texts.thisWindow, STR_WORKBENCH_PROJECTS_THIS_WINDOW);
	load(texts.unavailable, STR_WORKBENCH_PROJECTS_UNAVAILABLE);
	load(texts.loadingGit, STR_WORKBENCH_PROJECTS_LOADING_GIT);
	load(texts.noGit, STR_WORKBENCH_PROJECTS_NO_GIT);
	load(texts.gitUnavailable, STR_WORKBENCH_PROJECTS_GIT_UNAVAILABLE);
	load(texts.repositoriesFormat, STR_WORKBENCH_PROJECTS_REPOSITORIES_COUNT);
	load(texts.branchesFormat, STR_WORKBENCH_PROJECTS_BRANCHES_COUNT);
	load(texts.hideLinkedWorktrees, STR_WORKBENCH_PROJECTS_HIDE_LINKED);
	load(texts.showLinkedWorktrees, STR_WORKBENCH_PROJECTS_SHOW_LINKED);
	load(texts.projects, STR_WORKBENCH_PROJECTS_TITLE);
	load(texts.sessions, STR_WORKBENCH_PROJECTS_SESSIONS);
	load(texts.loadingSessions, STR_WORKBENCH_PROJECTS_SESSIONS_LOADING);
	load(texts.noSessions, STR_WORKBENCH_PROJECTS_SESSIONS_EMPTY);
	load(texts.untitledSession, STR_WORKBENCH_PROJECTS_SESSION_UNTITLED);
	load(texts.resumeSessionHint, STR_WORKBENCH_PROJECTS_SESSION_RESUME_HINT);
	load(texts.pinnedSession, STR_WORKBENCH_PROJECTS_SESSION_PINNED);
	load(texts.archivedSessionsFormat, STR_WORKBENCH_PROJECTS_SESSIONS_ARCHIVED);
	load(texts.sessionAge.now, STR_WORKBENCH_PROJECTS_SESSION_AGE_NOW);
	load(texts.sessionAge.minutesFormat, STR_WORKBENCH_PROJECTS_SESSION_AGE_MINUTES);
	load(texts.sessionAge.hoursFormat, STR_WORKBENCH_PROJECTS_SESSION_AGE_HOURS);
	load(texts.sessionAge.daysFormat, STR_WORKBENCH_PROJECTS_SESSION_AGE_DAYS);
	return texts;
}

int ScaleDip(const int value, const unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

enum class EPageState : std::uint8_t {
	Empty,
	Loaded,
	Unavailable,
};

//! #411: the two Views of the Projects ViewContainer, as pane headers name them.
enum class EPane : std::uint8_t {
	None,
	Projects,
	Sessions,
};

struct ProjectBranchCacheEntry final {
	std::vector<ProjectRepositoryBranchObservation> observations;
	std::vector<std::optional<agent::AgentWorkspacesProjectionResult>> projections;
	ProjectBranchSummary summary;
	std::size_t completed{};
	bool truncated = false;
};

class CProjectsPage final : public viewcontainer::IViewContainerPage,
	public viewcontainer::IViewContainerPageProjection {
public:
	explicit CProjectsPage(ProjectsPageOptions options) :
		m_options(std::move(options)), m_texts(LoadProjectsTexts())
	{
		if (m_options.gitDiscoveryFactory) {
			m_discovery = m_options.gitDiscoveryFactory();
		} else {
			worktree::GitWorktreeDiscoveryLimits limits;
			limits.commandTimeoutMilliseconds = 3000;
			limits.retry.maximumAttempts = 1;
			m_discovery = std::make_unique<worktree::GitWorktreeDiscoverySource>(
				std::make_shared<worktree::GitWorktreeListRunner>(),
				std::make_shared<worktree::SystemGitWorktreeRetryJitterSource>(), limits);
		}
	}

	~CProjectsPage() override
	{
		Cleanup();
	}

	[[nodiscard]] bool Create(HWND parkingParent) noexcept
	{
		if (m_created || m_closeInvoked || parkingParent == nullptr
			|| !::IsWindow(parkingParent) || !m_options.projects || !m_options.workspace
			|| !m_options.workspaceRoot || !m_options.repositoryRoots || !m_options.activateProject
			|| !m_options.activateWorktree || !m_options.removeProject
			|| !m_discovery) return false;
		m_parkingParent = parkingParent;
		HINSTANCE instance = reinterpret_cast<HINSTANCE>(
			::GetWindowLongPtrW(parkingParent, GWLP_HINSTANCE));
		if (instance == nullptr) instance = ::GetModuleHandleW(nullptr);
		if (!EnsureClass(instance)) return false;
		m_window = ::CreateWindowExW(0, kWindowClass, L"",
			WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, 0, 0, parkingParent, nullptr, instance, this);
		if (m_window == nullptr) return false;
		// Hover text is optional: without the tooltip the rows still work.
		CreateRowTooltip(instance);
		if (!m_projectsList.Create(m_window, kProjectsListControlId, instance, m_tooltip,
				ProjectsListCallbacks())
			// #411: the Sessions View exists only when session history does.
			// Without it the container has one View, whose header VS Code
			// merges into the container title.
			|| (m_options.agentSessions && !m_sessionsList.Create(m_window,
				kSessionsListControlId, instance, m_tooltip, SessionsListCallbacks()))) {
			Cleanup();
			return false;
		}
		m_created = true;
		// The registered listener may arrive from a foreign thread; it only
		// coalesces one posted message back to this page's window and never
		// touches page state directly.
		if (m_options.agentsChanged) {
			m_options.agentsChanged([this] {
				const HWND window = m_window;
				if (window == nullptr) return;
				bool expected = false;
				if (!m_agentsChangedPending.compare_exchange_strong(expected, true)) return;
				if (::PostMessageW(window, kAgentsChangedMessage, 0, 0) == FALSE) {
					m_agentsChangedPending = false;
				}
			});
		}
		ApplyDpi(96);
		ApplyVisibility();
		return true;
	}

	[[nodiscard]] std::string_view ContainerId() const noexcept override
	{
		return layout::ids::viewContainer::Projects;
	}

	[[nodiscard]] viewcontainer::ViewContainerFocusCaptureResult CaptureFocusToken() noexcept override
	{
		if (!m_attachedHost) {
			return { viewcontainer::EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
		}
		const HWND focused = ::GetFocus();
		if (!OwnsWindow(focused)) {
			return { viewcontainer::EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
		}
		return { viewcontainer::EViewContainerFocusCaptureStatus::Captured,
			viewcontainer::ViewContainerFocusToken{
				reinterpret_cast<viewcontainer::ViewContainerNativeHandle>(focused) } };
	}

	[[nodiscard]] viewcontainer::EViewContainerPageDetachStatus Detach(
		const viewcontainer::ViewContainerPageHost& host) noexcept override
	{
		if (!m_created || !m_attachedHost || *m_attachedHost != host) {
			return viewcontainer::EViewContainerPageDetachStatus::Failed;
		}
		Show(false);
		m_attachedHost.reset();
		return viewcontainer::EViewContainerPageDetachStatus::Detached;
	}

	[[nodiscard]] viewcontainer::EViewContainerPageReparentStatus Reparent(
		const viewcontainer::ViewContainerNativeHandle nativeParent) noexcept override
	{
		if (!m_created || m_attachedHost) return viewcontainer::EViewContainerPageReparentStatus::Failed;
		if (nativeParent == m_nativeParent) return viewcontainer::EViewContainerPageReparentStatus::Reparented;
		const HWND target = nativeParent == 0 ? m_parkingParent : reinterpret_cast<HWND>(nativeParent);
		if (target == nullptr || !::IsWindow(target)) return viewcontainer::EViewContainerPageReparentStatus::Failed;
		::SetLastError(ERROR_SUCCESS);
		const HWND previous = ::SetParent(m_window, target);
		if (previous == nullptr && ::GetLastError() != ERROR_SUCCESS) {
			return viewcontainer::EViewContainerPageReparentStatus::Failed;
		}
		m_nativeParent = nativeParent;
		return viewcontainer::EViewContainerPageReparentStatus::Reparented;
	}

	[[nodiscard]] viewcontainer::EViewContainerPageAttachStatus Attach(
		const viewcontainer::ViewContainerPageHost& host) noexcept override
	{
		if (!m_created || m_attachedHost || host.nativeParent == 0
			|| host.nativeParent != m_nativeParent) return viewcontainer::EViewContainerPageAttachStatus::Failed;
		try { m_attachedHost = host; }
		catch (...) { return viewcontainer::EViewContainerPageAttachStatus::Failed; }
		ApplyVisibility();
		return viewcontainer::EViewContainerPageAttachStatus::Attached;
	}

	[[nodiscard]] viewcontainer::EViewContainerFocusRestoreStatus RestoreFocusToken(
		const viewcontainer::ViewContainerFocusToken token) noexcept override
	{
		const HWND target = reinterpret_cast<HWND>(token.value);
		if (!m_attachedHost || !OwnsWindow(target)) {
			return viewcontainer::EViewContainerFocusRestoreStatus::Failed;
		}
		(void)::SetFocus(target);
		return ::GetFocus() == target
			? viewcontainer::EViewContainerFocusRestoreStatus::Restored
			: viewcontainer::EViewContainerFocusRestoreStatus::Failed;
	}

	[[nodiscard]] viewcontainer::EViewContainerPageCloseStatus Close() noexcept override
	{
		if (m_closeInvoked) return viewcontainer::EViewContainerPageCloseStatus::Closed;
		m_closeInvoked = true;
		Cleanup();
		return viewcontainer::EViewContainerPageCloseStatus::Closed;
	}

	void ActivateProjection() noexcept override
	{
		if (m_active) {
			(void)FocusFirstList();
			return;
		}
		m_active = true;
		m_sessionCatchUpAttempts = 0;
		Refresh();
		(void)FocusFirstList();
	}

	void DeactivateProjection() noexcept override
	{
		m_active = false;
		StopSessionCatchUp();
	}

	[[nodiscard]] bool PreTranslateProjection(MSG& message) noexcept override
	{
		if (!m_active || message.hwnd == nullptr || message.message != WM_KEYDOWN) return false;
		if (HasSessionsView() && message.hwnd == m_sessionsList.Handle()) {
			switch (message.wParam) {
			case VK_RETURN:
				ActivateSelectedSessionRow();
				return true;
			case VK_F5:
				Refresh();
				return true;
			default:
				return false;
			}
		}
		if (message.hwnd != m_projectsList.Handle()) return false;
		switch (message.wParam) {
		case VK_RETURN:
			ActivateSelection();
			return true;
		case VK_F5:
			Refresh();
			return true;
		case VK_DELETE:
			if (!CanRemoveSelection()) return false;
			RemoveSelectedProject();
			return true;
		case VK_LEFT:
			SetSelectedToggle(false);
			return true;
		case VK_RIGHT:
			SetSelectedToggle(true);
			return true;
		default:
			return false;
		}
	}

	void LayoutProjection(const RECT& hostBounds, const RECT& contentBounds,
		const unsigned int dpi) noexcept override
	{
		if (!m_created || m_window == nullptr) return;
		ApplyDpi(dpi == 0 ? 96 : dpi);
		const LONG width = std::max(0L, hostBounds.right - hostBounds.left);
		const LONG height = std::max(0L, hostBounds.bottom - hostBounds.top);
		if (!::SetWindowPos(m_window, nullptr, hostBounds.left, hostBounds.top, width, height,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW)) {
			m_layoutValid = false;
			ApplyVisibility();
			return;
		}
		const LONG left = std::clamp(contentBounds.left - hostBounds.left, 0L, width);
		const LONG top = std::clamp(contentBounds.top - hostBounds.top, 0L, height);
		const LONG right = std::clamp(contentBounds.right - hostBounds.left, left, width);
		const LONG bottom = std::clamp(contentBounds.bottom - hostBounds.top, top, height);
		m_contentBounds = { left, top, right, bottom };
		LayoutPanes();
		m_layoutValid = true;
		ApplyVisibility();
		// Contributed pages participate in the enclosing Workbench frame transaction.
		// Queue the final page and control pixels, but leave synchronous presentation
		// to CEditWnd's one RDW_ALLCHILDREN commit after every Part has moved. Painting
		// a LISTBOX early can publish its resized parent background as a separate
		// compositor frame before the parent's committed redraw reaches the child.
		m_projectsList.Invalidate();
		m_sessionsList.Invalidate();
		::RedrawWindow(m_window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE);
	}

	void SetProjectionVisible(const bool visible) noexcept override
	{
		m_visible = visible;
		ApplyVisibility();
	}

	void SetProjectionPalette(const theme::ThemePalette& palette) noexcept override
	{
		m_palette = palette;
		m_projectsList.SetPalette(palette);
		m_sessionsList.SetPalette(palette);
		if (m_window != nullptr) {
			::RedrawWindow(m_window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
		}
	}

	void RefreshProjectionStrings() noexcept override
	{
		try {
			m_texts = LoadProjectsTexts();
			// Reuse completed Git observations; a language change is not a refresh request.
			for (auto& [identity, entry] : m_branchCache) {
				for (std::size_t index = 0; index < entry.projections.size(); ++index) {
					const auto& projection = entry.projections[index];
					if (projection && projection->currentIndex
						&& *projection->currentIndex < projection->rows.size()) {
						entry.observations[index].label = ProjectWorktreeBranchLabel(
							projection->rows[*projection->currentIndex], m_texts);
					}
				}
				if (entry.summary.status == EProjectBranchSummaryStatus::Unavailable
					&& entry.observations.empty()) {
					entry.summary.label = m_texts.gitUnavailable;
				} else {
					entry.summary = SummarizeProjectBranches(entry.observations,
						entry.completed == entry.observations.size(), entry.truncated, m_texts);
				}
			}
			const auto selected = m_projectsList.Selection();
			const auto top = TopIndex(m_projectsList);
			const auto sessionsTop = TopIndex(m_sessionsList);
			CaptureSessionSelection();
			RebuildProjection();
			if (selected && *selected < m_rows.size()) {
				m_selectedIndex = *selected;
				m_projectsList.Select(*selected);
			}
			if (top != LB_ERR) (void)::SendMessageW(m_projectsList.Handle(), LB_SETTOPINDEX, top, 0);
			RebuildSessions();
			if (sessionsTop != LB_ERR) {
				(void)::SendMessageW(m_sessionsList.Handle(), LB_SETTOPINDEX, sessionsTop, 0);
			}
			m_projectsList.UpdateScrollbar();
			m_sessionsList.UpdateScrollbar();
			InvalidateContent();
		} catch (...) {
			// Keep the accepted rows if allocating the new presentation failed.
		}
	}

	void RefreshProjectionContent() noexcept override
	{
		if (m_active) Refresh();
	}

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NCCREATE) {
			auto* self = static_cast<CProjectsPage*>(
				reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
			::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}
		auto* self = reinterpret_cast<CProjectsPage*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
		if (self == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
		switch (message) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
			self->Paint();
			return 0;
		case WM_DRAWITEM: {
			const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
			if (item.CtlID == kProjectsListControlId ? self->DrawItem(item)
				: item.CtlID == kSessionsListControlId && self->DrawSessionItem(item)) return TRUE;
			break;
		}
		case WM_CTLCOLORLISTBOX: {
			const HDC dc = reinterpret_cast<HDC>(wParam);
			::SetTextColor(dc, self->m_palette.primaryText.ToColorRef());
			::SetBkColor(dc, self->m_palette.sideBar.ToColorRef());
			::SetDCBrushColor(dc, self->m_palette.sideBar.ToColorRef());
			return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
		}
		case WM_TIMER:
			if (wParam == kRefreshTimer) { self->PollRefresh(); return 0; }
			if (wParam == kSessionsTimer) { self->PollSessionDiscovery(); return 0; }
			if (wParam == kSessionsCatchUpTimer) { self->OnSessionCatchUpTimer(); return 0; }
			if (wParam == kSessionOperationTimer) { self->PollSessionOperation(); return 0; }
			break;
		case kAgentsChangedMessage:
			self->OnAgentsChangedMessage();
			return 0;
		case WM_COMMAND:
			if (LOWORD(wParam) == kProjectsListControlId && HIWORD(wParam) == LBN_SELCHANGE) {
				self->CaptureSelection();
				return 0;
			}
			if (LOWORD(wParam) == kProjectsListControlId && HIWORD(wParam) == LBN_DBLCLK) {
				// The first click already toggled a group row; VS Code's
				// singleClick expand mode does not toggle it again (#412).
				if (!self->SelectedRowIsGroup()) self->ActivateSelection();
				return 0;
			}
			if (LOWORD(wParam) == kSessionsListControlId && HIWORD(wParam) == LBN_SELCHANGE) {
				self->CaptureSessionSelection();
				return 0;
			}
			if (LOWORD(wParam) == kSessionsListControlId && HIWORD(wParam) == LBN_DBLCLK) {
				// The first click already toggled the Archived group (#412).
				if (!self->SelectedSessionRowIsGroup()) self->ResumeSelectedSession();
				return 0;
			}
			break;
		case WM_SETFOCUS:
			if (self->FocusFirstList()) return 0;
			break;
		case WM_LBUTTONDOWN: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if (self->BeginSashDrag(point)) return 0;
			self->m_pressedHeader = self->HeaderAt(point);
			return 0;
		}
		case WM_LBUTTONUP: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if (self->m_draggingSash) {
				self->EndSashDrag();
				return 0;
			}
			// A pane header toggles when the press and the release both land on it.
			const auto pressed = std::exchange(self->m_pressedHeader, EPane::None);
			if (pressed != EPane::None && pressed == self->HeaderAt(point)) self->TogglePane(pressed);
			return 0;
		}
		case WM_MOUSEMOVE:
			if (self->m_draggingSash) {
				self->UpdateSashDrag({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
				return 0;
			}
			break;
		case WM_CAPTURECHANGED:
			// Capture can be taken away; a drag that believed it was still
			// running would move the boundary on the next unrelated move.
			self->m_draggingSash = false;
			return 0;
		case WM_SETCURSOR: {
			if (reinterpret_cast<HWND>(wParam) != window) break;
			POINT point{};
			if (::GetCursorPos(&point) == FALSE || ::ScreenToClient(window, &point) == FALSE) break;
			if (self->m_draggingSash || self->PointInSash(point)) {
				::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
				return TRUE;
			}
			if (self->HeaderAt(point) != EPane::None) {
				::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
				return TRUE;
			}
			break;
		}
		}
		return ::DefWindowProcW(window, message, wParam, lParam);
	}

private:
	[[nodiscard]] static bool EnsureClass(HINSTANCE instance) noexcept
	{
		static ATOM atom = 0;
		if (atom != 0) return true;
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.hInstance = instance;
		windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
		windowClass.lpfnWndProc = &CProjectsPage::WindowProc;
		windowClass.lpszClassName = kWindowClass;
		atom = ::RegisterClassExW(&windowClass);
		if (atom != 0) return true;
		if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) { atom = 1; return true; }
		return false;
	}

	[[nodiscard]] bool OwnsWindow(HWND target) const noexcept
	{
		return target != nullptr && ::IsWindow(target) && m_window != nullptr
			&& (target == m_window || ::IsChild(m_window, target));
	}

	void ApplyDpi(const unsigned int dpi) noexcept
	{
		if (m_dpi == dpi && m_font.Get() != nullptr) return;
		m_dpi = dpi;
		(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
		const int rowHeight = ScaleDip(kRowHeightDip, m_dpi);
		m_projectsList.ApplyDpi(m_dpi, m_font.Get(), rowHeight);
		m_sessionsList.ApplyDpi(m_dpi, m_font.Get(), rowHeight);
		if (m_tooltip != nullptr) {
			(void)::SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, ScaleDip(kTooltipMaxWidthDip, m_dpi));
		}
	}

	//! One tooltip for the page; each list adds one tool whose text follows
	//! its hovered row.
	void CreateRowTooltip(HINSTANCE instance) noexcept
	{
		m_tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
			WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			m_window, nullptr, instance, nullptr);
	}

	[[nodiscard]] ProjectsListPaneCallbacks ProjectsListCallbacks()
	{
		ProjectsListPaneCallbacks callbacks;
		callbacks.tooltip = [this](const std::size_t row) {
			return row < m_rows.size() ? m_rows[row].tooltip : std::wstring{};
		};
		callbacks.opensOnSingleClick = [this](const std::size_t row) {
			return row < m_rows.size() && OpensOnSingleClick(m_rows[row].kind);
		};
		callbacks.clicked = [this](std::size_t) { ActivateSelection(); };
		callbacks.contextMenu = [this](const LPARAM position) { ShowProjectContextMenu(position); };
		callbacks.keyDown = [this](const WPARAM key) {
			if (key != VK_DELETE || !CanRemoveSelection()) return false;
			RemoveSelectedProject();
			return true;
		};
		return callbacks;
	}

	[[nodiscard]] ProjectsListPaneCallbacks SessionsListCallbacks()
	{
		ProjectsListPaneCallbacks callbacks;
		callbacks.tooltip = [this](const std::size_t row) {
			return row < m_sessionRows.size() ? m_sessionRows[row].tooltip : std::wstring{};
		};
		// A session row starts a process, so it keeps Enter/double-click. The
		// Archived group row toggles on a single click, like the worktrees one.
		callbacks.opensOnSingleClick = [this](const std::size_t row) {
			return row < m_sessionRows.size()
				&& m_sessionRows[row].kind == EAgentSessionsRowKind::ArchivedToggle;
		};
		callbacks.clicked = [this](std::size_t) { ActivateSelectedSessionRow(); };
		callbacks.contextMenu = [this](const LPARAM position) { ShowSessionContextMenu(position); };
		return callbacks;
	}

	[[nodiscard]] bool SelectedSessionRowIsGroup() const noexcept
	{
		const auto selected = m_sessionsList.Selection();
		return selected && *selected < m_sessionRows.size()
			&& m_sessionRows[*selected].kind == EAgentSessionsRowKind::ArchivedToggle;
	}

	//! Rows a single click opens: an agent focuses its terminal and a group
	//! row toggles. A Project or worktree click would switch the workspace, so
	//! those keep Enter/double-click.
	[[nodiscard]] static constexpr bool OpensOnSingleClick(const EProjectsRowKind kind) noexcept
	{
		return kind == EProjectsRowKind::Agent || IsGroupRow(kind);
	}

	[[nodiscard]] static constexpr bool IsGroupRow(const EProjectsRowKind kind) noexcept
	{
		return kind == EProjectsRowKind::WorktreesToggle;
	}

	[[nodiscard]] bool SelectedRowIsGroup() const noexcept
	{
		const auto selected = m_projectsList.Selection();
		return selected && *selected < m_rows.size() && IsGroupRow(m_rows[*selected].kind);
	}

	[[nodiscard]] static LRESULT TopIndex(const CProjectsListPane& pane) noexcept
	{
		return pane.Handle() != nullptr ? ::SendMessageW(pane.Handle(), LB_GETTOPINDEX, 0, 0) : LB_ERR;
	}

	[[nodiscard]] bool HasSessionsView() const noexcept
	{
		return m_sessionsList.Handle() != nullptr;
	}

	//! The Projects list shows rows only once the catalog loaded; otherwise its
	//! body paints the empty or unavailable message.
	[[nodiscard]] bool ProjectsListUsable() const noexcept
	{
		return m_state == EPageState::Loaded && !(HasSessionsView() && m_projectsCollapsed);
	}

	[[nodiscard]] bool SessionsListUsable() const noexcept
	{
		return HasSessionsView() && !m_sessionsCollapsed;
	}

	//! Focuses the first list that can take input, Projects before Sessions.
	bool FocusFirstList() noexcept
	{
		const HWND target = ProjectsListUsable() ? m_projectsList.Handle()
			: SessionsListUsable() ? m_sessionsList.Handle() : nullptr;
		if (target == nullptr) return false;
		(void)::SetFocus(target);
		return true;
	}

	void Show(const bool visible) noexcept
	{
		m_projectsList.SetVisible(visible && ProjectsListUsable());
		if (HasSessionsView()) m_sessionsList.SetVisible(visible && SessionsListUsable());
		if (m_window != nullptr) ::ShowWindow(m_window, visible ? SW_SHOW : SW_HIDE);
	}

	void ApplyVisibility() noexcept
	{
		Show(m_visible && m_layoutValid && m_attachedHost.has_value());
	}

	//! The two expanded bodies' combined height, and the smallest body either
	//! may be squeezed to: VS Code's 120-DIP pane minimum, or half the space
	//! when two minimums do not fit.
	[[nodiscard]] std::pair<int, int> PaneBodySpace() const noexcept
	{
		const int header = ScaleDip(viewcontainer::kViewPaneHeaderDip, m_dpi);
		const int height = std::max(0, static_cast<int>(m_contentBounds.bottom - m_contentBounds.top));
		const int body = std::max(0, height - 2 * header);
		return { body, std::min(ScaleDip(viewcontainer::kViewPaneMinimumBodyDip, m_dpi), body / 2) };
	}

	//! #411: the Projects ViewContainer's View stack. With the Sessions View it
	//! is VS Code's paneview: two 22-DIP headers, collapsible bodies, and one
	//! sash between two expanded bodies. Without it the single View's header
	//! merges into the container title, as mergeViewWithContainerWhenSingleView
	//! does, and the Projects list fills the content area.
	void LayoutPanes() noexcept
	{
		const RECT content = m_contentBounds;
		m_projectsHeader = m_sessionsHeader = m_sash = m_sessionsBody = RECT{};
		if (!HasSessionsView()) {
			m_projectsBody = content;
			m_projectsList.SetBounds(m_projectsBody);
			return;
		}
		const int header = ScaleDip(viewcontainer::kViewPaneHeaderDip, m_dpi);
		const int height = std::max(0, static_cast<int>(content.bottom - content.top));
		const auto [body, minimum] = PaneBodySpace();
		const int sessions = std::clamp(static_cast<int>(std::lround(body * m_sessionsShare)),
			minimum, body - minimum);
		const std::array<viewcontainer::ViewPaneSize, 2> sizes{ {
			{ .collapsed = m_projectsCollapsed, .preferredBody = body - sessions, .minimumBody = minimum },
			{ .collapsed = m_sessionsCollapsed, .preferredBody = sessions, .minimumBody = minimum },
		} };
		const auto stack = viewcontainer::BuildViewPaneStackLayout(sizes, height, header);
		if (!stack.valid) {
			m_projectsBody = RECT{};
		} else {
			// A viewport shorter than both headers clips the stack at its bottom.
			const auto band = [&content](const int top, const int bottom) {
				return RECT{ content.left, std::min(content.top + top, content.bottom),
					content.right, std::min(content.top + bottom, content.bottom) };
			};
			m_projectsHeader = band(stack.panes[0].top, stack.panes[0].headerBottom);
			m_projectsBody = band(stack.panes[0].headerBottom, stack.panes[0].bottom);
			m_sessionsHeader = band(stack.panes[1].top, stack.panes[1].headerBottom);
			m_sessionsBody = band(stack.panes[1].headerBottom, stack.panes[1].bottom);
		}
		RECT projectsList = m_projectsBody;
		if (stack.valid && !m_projectsCollapsed && !m_sessionsCollapsed) {
			// VS Code's 4-DIP sash straddles the boundary. The Projects list
			// stops at its top half so the page, not the list, receives the drag.
			const int sash = ScaleDip(viewcontainer::kViewPaneSashDip, m_dpi);
			m_sash = { content.left, m_sessionsHeader.top - sash / 2,
				content.right, m_sessionsHeader.top - sash / 2 + sash };
			projectsList.bottom = std::max(projectsList.top, m_sash.top);
		}
		m_projectsList.SetBounds(projectsList);
		m_sessionsList.SetBounds(m_sessionsBody);
	}

	//! A gesture inside the container owns its frame and publishes it.
	void RelayoutPanes() noexcept
	{
		LayoutPanes();
		ApplyVisibility();
		if (m_window != nullptr) {
			::RedrawWindow(m_window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		}
	}

	[[nodiscard]] EPane HeaderAt(const POINT point) const noexcept
	{
		if (!HasSessionsView() || PointInSash(point)) return EPane::None;
		if (::PtInRect(&m_projectsHeader, point) != FALSE) return EPane::Projects;
		if (::PtInRect(&m_sessionsHeader, point) != FALSE) return EPane::Sessions;
		return EPane::None;
	}

	void TogglePane(const EPane pane) noexcept
	{
		const bool projects = pane == EPane::Projects;
		bool& collapsed = projects ? m_projectsCollapsed : m_sessionsCollapsed;
		const HWND list = projects ? m_projectsList.Handle() : m_sessionsList.Handle();
		const bool hadFocus = list != nullptr && ::GetFocus() == list;
		collapsed = !collapsed;
		RelayoutPanes();
		// A collapsed body cannot keep the keyboard focus it hides.
		if (collapsed && hadFocus && !FocusFirstList() && m_window != nullptr) (void)::SetFocus(m_window);
	}

	[[nodiscard]] bool PointInSash(const POINT point) const noexcept
	{
		return m_sash.bottom > m_sash.top && ::PtInRect(&m_sash, point) != FALSE;
	}

	bool BeginSashDrag(const POINT point) noexcept
	{
		if (m_window == nullptr || !PointInSash(point)) return false;
		m_draggingSash = true;
		m_sashDragOriginY = point.y;
		m_sashDragOriginBody = static_cast<int>(m_sessionsBody.bottom - m_sessionsBody.top);
		::SetCapture(m_window);
		return true;
	}

	void UpdateSashDrag(const POINT point) noexcept
	{
		if (!m_draggingSash) return;
		// Dragging upwards grows the Sessions body, because the boundary is
		// its top edge. The delta is measured against the drag's own origin,
		// so repeated rounding cannot accumulate.
		const auto [body, minimum] = PaneBodySpace();
		if (body <= 0) return;
		const int next = std::clamp(
			m_sashDragOriginBody - (static_cast<int>(point.y) - m_sashDragOriginY),
			minimum, body - minimum);
		const double share = static_cast<double>(next) / body;
		if (share == m_sessionsShare) return;
		m_sessionsShare = share;
		RelayoutPanes();
	}

	void EndSashDrag() noexcept
	{
		if (!m_draggingSash) return;
		m_draggingSash = false;
		::ReleaseCapture();
	}

	void SetState(const EPageState state) noexcept
	{
		m_state = state;
		ApplyVisibility();
		if (m_window != nullptr) {
			::RedrawWindow(m_window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_UPDATENOW);
			(void)::GdiFlush();
		}
	}

	void Refresh() noexcept
	{
		if (!m_created || !m_discovery) return;
		if (m_currentBranchRequest) {
			// The current rows stay on screen until the cancelled request
			// restarts, so they are reprojected against the new agent list
			// instead of indexing it with stale agentIndex values.
			ReloadAgents(true);
			RebuildProjection();
			m_restartPending = true;
			(void)m_discovery->CancelCurrent();
			ReloadSessionView(true);
			BeginSessionDiscovery();
			return;
		}
		ReloadAgents(true);
		BeginBranchRefresh();
		ReloadSessionView(true);
		BeginSessionDiscovery();
	}

	//! #408: the profile's Sessions view state, or null without one.
	[[nodiscard]] IAgentSessionViewStateService* SessionViewService() const noexcept
	{
		if (!m_options.sessionManagement.viewState) return nullptr;
		try { return m_options.sessionManagement.viewState(); }
		catch (...) { return nullptr; }
	}

	//! Copies the service's state into the page. `load` re-reads storage
	//! first, because another window of the profile may have changed it.
	void ReloadSessionView(const bool load) noexcept
	{
		try {
			if (const auto service = SessionViewService(); service != nullptr) {
				if (load) service->Load();
				m_sessionView = service->Snapshot();
			} else {
				m_sessionView = {};
			}
		} catch (...) {
			m_sessionView = {};
		}
	}

	[[nodiscard]] std::int64_t NowUnixSeconds() const noexcept
	{
		if (m_options.nowUnixSeconds) {
			try { return m_options.nowUnixSeconds(); }
			catch (...) {}
		}
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	//! Single-flight: while one discovery runs, a request only marks one rerun
	//! (and cancels the running one when the folder changed). The sessions
	//! last accepted for the same folder stay on screen while it reloads.
	void BeginSessionDiscovery() noexcept
	{
		if (!m_created || !m_options.agentSessions) return;
		std::wstring folder;
		try { folder = m_options.workspaceRoot(); }
		catch (...) { folder.clear(); }
		if (m_sessionDiscovery.valid()) {
			m_sessionRerunPending = true;
			if (!IsSameAgentSessionFolder(folder, m_sessionDiscoveryFolder) && m_sessionStop != nullptr) {
				(void)::SetEvent(m_sessionStop);
			}
			return;
		}
		if (folder.empty()) {
			m_sessions.clear();
			m_sessionsFolder.clear();
			m_sessionsState = EProjectSessionsState::Unavailable;
			RebuildSessions();
			return;
		}
		try {
			if (!IsSameAgentSessionFolder(folder, m_sessionsFolder)) {
				m_sessions.clear();
				m_sessionsFolder = folder;
			}
			m_sessionStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (m_sessionStop == nullptr) throw std::bad_alloc();
			m_sessionDiscoveryFolder = folder;
			ProjectAgentSessionsQuery query{ .folder = folder, .displayCount = m_sessionView.displayCount };
			query.markedIdentities = m_sessionView.pinned;
			query.markedIdentities.insert(query.markedIdentities.end(),
				m_sessionView.archived.begin(), m_sessionView.archived.end());
			m_sessionDiscovery = std::async(std::launch::async,
				[provider = m_options.agentSessions, query = std::move(query), stop = m_sessionStop]() mutable {
					return provider(std::move(query), stop);
				});
		} catch (...) {
			ReleaseSessionDiscovery();
			m_sessionsState = EProjectSessionsState::Unavailable;
			RebuildSessions();
			return;
		}
		if (m_window == nullptr
			|| ::SetTimer(m_window, kSessionsTimer, kSessionsPollMilliseconds, nullptr) == 0) {
			// Nothing could observe the completion: cancel and fail closed.
			CancelSessionDiscovery();
			m_sessionsState = EProjectSessionsState::Unavailable;
			RebuildSessions();
			return;
		}
		m_sessionsState = EProjectSessionsState::Loading;
		RebuildSessions();
	}

	void PollSessionDiscovery() noexcept
	{
		if (!m_sessionDiscovery.valid()) {
			if (m_window != nullptr) (void)::KillTimer(m_window, kSessionsTimer);
			return;
		}
		if (m_sessionDiscovery.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
		std::optional<std::vector<AgentSessionRecord>> result;
		try { result = m_sessionDiscovery.get(); }
		catch (...) { result.reset(); }
		const auto folder = m_sessionDiscoveryFolder;
		ReleaseSessionDiscovery();
		CaptureSessionSelection();
		if (IsSameAgentSessionFolder(folder, m_sessionsFolder)) {
			if (result) {
				m_sessions = std::move(*result);
				m_sessionsState = EProjectSessionsState::Ready;
			} else {
				// Cancelled or failed: keep the last accepted list, if any.
				m_sessionsState = m_sessions.empty()
					? EProjectSessionsState::Unavailable : EProjectSessionsState::Ready;
			}
		}
		RebuildSessions();
		if (m_sessionRerunPending) {
			m_sessionRerunPending = false;
			BeginSessionDiscovery();
		} else {
			UpdateSessionCatchUp();
		}
	}

	//! #410: arms one catch-up rerun while a present agent still has no
	//! session of its own. Single-flight: it waits for a running discovery,
	//! whose completion calls this again.
	void UpdateSessionCatchUp() noexcept
	{
		if (!m_active || m_window == nullptr || m_sessionDiscovery.valid()
			|| m_sessionCatchUpArmed) return;
		std::size_t waiting = 0;
		try {
			std::vector<ProjectAgentSighting> sightings;
			sightings.reserve(m_agents.size());
			for (const auto& agent : m_agents) {
				const auto found = m_agentFirstSeen.find(agent.identity);
				if (found != m_agentFirstSeen.end()) sightings.push_back({ agent.name, found->second });
			}
			waiting = CountAgentsAwaitingSessionHistory(sightings, m_sessions);
		} catch (...) {
			waiting = 0;
		}
		if (waiting == 0 || m_sessionCatchUpAttempts >= kSessionsCatchUpMaximumAttempts) return;
		const auto doublings = static_cast<unsigned int>(std::min<std::size_t>(m_sessionCatchUpAttempts, 4));
		const UINT delay = std::min(kSessionsCatchUpMaximumDelayMilliseconds,
			kSessionsCatchUpFirstDelayMilliseconds << doublings);
		m_sessionCatchUpArmed = ::SetTimer(m_window, kSessionsCatchUpTimer, delay, nullptr) != 0;
	}

	void OnSessionCatchUpTimer() noexcept
	{
		// A stray tick after StopSessionCatchUp is not an attempt.
		if (!m_sessionCatchUpArmed) return;
		StopSessionCatchUp();
		if (!m_active) return;
		++m_sessionCatchUpAttempts;
		BeginSessionDiscovery();
	}

	void StopSessionCatchUp() noexcept
	{
		if (m_window != nullptr) (void)::KillTimer(m_window, kSessionsCatchUpTimer);
		m_sessionCatchUpArmed = false;
	}

	//! Closes the finished job's handles; the future must be ready or empty.
	void ReleaseSessionDiscovery() noexcept
	{
		if (m_window != nullptr) (void)::KillTimer(m_window, kSessionsTimer);
		m_sessionDiscovery = {};
		if (m_sessionStop != nullptr) ::CloseHandle(m_sessionStop);
		m_sessionStop = nullptr;
		m_sessionDiscoveryFolder.clear();
	}

	//! Signals the in-flight job and waits for it. The provider contract bounds
	//! that wait: it returns once `stop` is set.
	void CancelSessionDiscovery() noexcept
	{
		m_sessionRerunPending = false;
		if (m_sessionDiscovery.valid()) {
			if (m_sessionStop != nullptr) (void)::SetEvent(m_sessionStop);
			try { m_sessionDiscovery.wait(); }
			catch (...) {}
		}
		ReleaseSessionDiscovery();
	}

	//! Replaces m_agents. Callers that keep the current selection capture it
	//! before this call, while the rows still index the old list; callers then
	//! reproject so no row keeps a stale agentIndex.
	void ReloadAgents(const bool refresh) noexcept
	{
		m_agents.clear();
		if (m_options.agents) {
			try { m_agents = m_options.agents(refresh); }
			catch (...) { m_agents.clear(); }
		}
		// #410: remember when each agent row first appeared; a session counts
		// as that agent's once its activity is at least that recent.
		try {
			std::erase_if(m_agentFirstSeen, [this](const auto& entry) {
				return std::ranges::none_of(m_agents, [&entry](const auto& agent) {
					return agent.identity == entry.first;
				});
			});
			const auto now = NowUnixSeconds();
			for (const auto& agent : m_agents) m_agentFirstSeen.try_emplace(agent.identity, now);
		} catch (...) {
			m_agentFirstSeen.clear();
		}
	}

	void BeginBranchRefresh() noexcept
	{
		if (m_window != nullptr) (void)::KillTimer(m_window, kRefreshTimer);
		m_branchQueue.clear();
		m_currentBranchRequest.reset();
		m_refresh = {};
		m_projectBranchKeys.clear();
		m_branchCache.clear();
		m_worktrees.reset();
		m_currentWorkspaceRoot.clear();
		try {
			auto projects = m_options.projects();
			if (!projects) {
				m_projects.clear();
				m_rows.clear();
				RebuildList();
				SetState(EPageState::Unavailable);
				return;
			}
			m_projects = std::move(*projects);
			m_workspace = m_options.workspace();
			m_currentWorkspaceRoot = m_options.workspaceRoot();
		} catch (...) {
			m_projects.clear();
			m_rows.clear();
			RebuildList();
			SetState(EPageState::Unavailable);
			return;
		}
		if (m_projects.empty()) {
			RebuildProjection();
			SetState(EPageState::Empty);
			return;
		}
		SetState(EPageState::Loaded);

		try {
			const auto base = ProjectProjects(m_projects, m_workspace, nullptr, false);
			m_currentProjectIndex = base.currentProjectIndex;
			std::vector<ProjectBranchDiscoveryTarget> targets;
			targets.reserve(m_projects.size());
			m_projectBranchKeys.reserve(m_projects.size());
			for (std::size_t index = 0; index < m_projects.size(); ++index) {
				const auto identity = platform::uri::UriIdentityService::MakeComparisonKey(
					m_projects[index].uri);
				m_projectBranchKeys.push_back(identity);
				ProjectBranchCacheEntry entry;
				std::optional<std::vector<std::wstring>> roots;
				try { roots = m_options.repositoryRoots(m_projects[index]); }
				catch (...) { roots.reset(); }
				if (!roots) {
					entry.summary = {
						.status = EProjectBranchSummaryStatus::Unavailable,
						.label = m_texts.gitUnavailable,
					};
				}
				m_branchCache.emplace(identity, std::move(entry));
				targets.push_back({
					.identity = identity,
					.repositoryRoots = roots ? std::move(*roots) : std::vector<std::wstring>{},
					.currentProject = m_currentProjectIndex && *m_currentProjectIndex == index,
				});
			}
			const auto plan = PlanProjectBranchDiscovery(targets,
				kMaximumBranchRepositoriesPerProject, kMaximumBranchRequestsPerRefresh);
			for (const auto& request : plan.requests) {
				auto found = m_branchCache.find(request.identity);
				if (found == m_branchCache.end()) continue;
				auto& entry = found->second;
				if (entry.observations.size() <= request.repositoryIndex) {
					entry.observations.resize(request.repositoryIndex + 1U);
					entry.projections.resize(request.repositoryIndex + 1U);
				}
				m_branchQueue.push_back(request);
			}
			for (std::size_t index = 0; index < m_projectBranchKeys.size(); ++index) {
				auto found = m_branchCache.find(m_projectBranchKeys[index]);
				if (found == m_branchCache.end()) continue;
				auto& entry = found->second;
				entry.truncated = index < plan.truncatedProjects.size()
					&& plan.truncatedProjects[index];
				if (entry.summary.status == EProjectBranchSummaryStatus::Unavailable) continue;
				entry.summary = SummarizeProjectBranches(entry.observations,
					entry.observations.empty(), entry.truncated, m_texts);
			}
			if (!m_branchQueue.empty()
				&& (m_window == nullptr || ::SetTimer(m_window, kRefreshTimer,
					kRefreshPollMilliseconds, nullptr) == 0)) {
				while (!m_branchQueue.empty()) {
					CompleteUnavailable(m_branchQueue.front());
					m_branchQueue.pop_front();
				}
				RebuildProjection();
				InvalidateContent();
				return;
			}
			RebuildProjection();
			StartNextBranchRequest();
		} catch (...) {
			if (m_window != nullptr) (void)::KillTimer(m_window, kRefreshTimer);
			m_branchQueue.clear();
			m_currentBranchRequest.reset();
			m_branchCache.clear();
			m_projectBranchKeys.clear();
			m_rows.clear();
			RebuildList();
			SetState(EPageState::Unavailable);
		}
	}

	void CompleteUnavailable(const ProjectBranchDiscoveryRequest& request) noexcept
	{
		try {
			auto found = m_branchCache.find(request.identity);
			if (found == m_branchCache.end()
				|| request.repositoryIndex >= found->second.observations.size()) return;
			auto& entry = found->second;
			entry.observations[request.repositoryIndex].unavailable = true;
			++entry.completed;
			entry.summary = SummarizeProjectBranches(entry.observations,
				entry.completed == entry.observations.size(), entry.truncated, m_texts);
		} catch (...) {
			auto found = m_branchCache.find(request.identity);
			if (found == m_branchCache.end()) return;
			found->second.summary.status = EProjectBranchSummaryStatus::Unavailable;
			found->second.summary.label.clear();
		}
	}

	void StartNextBranchRequest() noexcept
	{
		while (!m_branchQueue.empty()) {
			auto request = std::move(m_branchQueue.front());
			m_branchQueue.pop_front();
			worktree::GitWorktreeRefresh refresh;
			try { refresh = m_discovery->Refresh(request.repositoryRoot); }
			catch (...) { CompleteUnavailable(request); continue; }
			if ((refresh.admission != worktree::EGitWorktreeRefreshAdmission::Started
				&& refresh.admission != worktree::EGitWorktreeRefreshAdmission::JoinedInFlight)
				|| !refresh.completion.valid()) {
				CompleteUnavailable(request);
				continue;
			}
			m_currentBranchRequest = std::move(request);
			m_refresh = std::move(refresh);
			RebuildProjection();
			InvalidateContent();
			return;
		}
		m_currentBranchRequest.reset();
		m_refresh = {};
		if (m_window != nullptr) (void)::KillTimer(m_window, kRefreshTimer);
		RebuildProjection();
		InvalidateContent();
	}

	void PollRefresh() noexcept
	{
		if (!m_refresh.completion.valid()
			|| m_refresh.completion.wait_for(std::chrono::milliseconds(0))
				!= std::future_status::ready) return;
		if (!m_currentBranchRequest) {
			m_refresh = {};
			return;
		}
		auto request = std::move(*m_currentBranchRequest);
		m_currentBranchRequest.reset();
		const bool superseded = m_restartPending;
		try {
			const auto& result = m_refresh.completion.get();
			if (!superseded) {
				auto found = m_branchCache.find(request.identity);
				if (found != m_branchCache.end()
					&& request.repositoryIndex < found->second.observations.size()) {
					auto& entry = found->second;
					if (result.Succeeded()) {
						auto projected = agent::ProjectAgentWorkspaces(result.records,
							request.repositoryRoot, m_selectedWorktreeIdentity);
						if (projected.Succeeded() && projected.currentIndex
							&& *projected.currentIndex < projected.rows.size()) {
							auto& observation = entry.observations[request.repositoryIndex];
							observation.label = ProjectWorktreeBranchLabel(
								projected.rows[*projected.currentIndex], m_texts);
							observation.succeeded = !observation.label.empty();
							entry.projections[request.repositoryIndex] = projected;
							if (m_currentProjectIndex
								&& *m_currentProjectIndex == request.projectIndex
								&& !m_currentWorkspaceRoot.empty()
								&& _wcsicmp(m_currentWorkspaceRoot.c_str(),
									request.repositoryRoot.c_str()) == 0) {
								m_worktrees = std::move(projected);
							}
						} else {
							entry.observations[request.repositoryIndex].unavailable = true;
						}
					} else {
						entry.observations[request.repositoryIndex].unavailable = true;
					}
					++entry.completed;
					entry.summary = SummarizeProjectBranches(entry.observations,
						entry.completed == entry.observations.size(), entry.truncated, m_texts);
				}
			}
		} catch (...) {
			if (!superseded) CompleteUnavailable(request);
		}
		m_refresh = {};
		if (superseded) {
			m_restartPending = false;
			BeginBranchRefresh();
			return;
		}
		RebuildProjection();
		StartNextBranchRequest();
	}

	void OnAgentsChangedMessage() noexcept
	{
		m_agentsChangedPending = false;
		if (!m_created) return;
		CaptureSelection();
		const auto before = AgentIdentities();
		ReloadAgents(false);
		// Agent presence changed: reproject rows only; Git discovery state,
		// the catalog, and the workspace snapshot all stay untouched.
		RebuildProjection();
		// An agent that started or exited is the moment its session history
		// changes; a session-title change alone is not (#406). A new agent set
		// also gets a fresh catch-up schedule (#410).
		if (m_active && AgentIdentities() != before) {
			m_sessionCatchUpAttempts = 0;
			BeginSessionDiscovery();
		}
	}

	[[nodiscard]] std::vector<std::wstring> AgentIdentities() const
	{
		std::vector<std::wstring> identities;
		try {
			identities.reserve(m_agents.size());
			for (const auto& agent : m_agents) identities.push_back(agent.identity);
			std::ranges::sort(identities);
		} catch (...) {
			identities.clear();
		}
		return identities;
	}

	//! Rebuilds the Projects View's rows. The Sessions View rebuilds on its own
	//! (RebuildSessions), so session discovery never resets this list.
	void RebuildProjection() noexcept
	{
		try {
			std::vector<ProjectBranchSummary> summaries;
			summaries.reserve(m_projectBranchKeys.size());
			for (const auto& identity : m_projectBranchKeys) {
				const auto found = m_branchCache.find(identity);
				summaries.push_back(found == m_branchCache.end()
					? SummarizeProjectBranches({}, false, false, m_texts) : found->second.summary);
			}
			const auto projection = ProjectProjects(m_projects, m_workspace,
				m_worktrees ? &*m_worktrees : nullptr, m_worktreesExpanded,
				summaries, m_selectedKind, m_selectedWorktreeIdentity, m_texts,
				m_agents, m_selectedAgentIdentity);
			m_rows = projection.rows;
			m_selectedIndex = projection.selectedRowIndex;
		} catch (...) {
			m_rows.clear();
			m_selectedIndex.reset();
		}
		RebuildList();
	}

	void RebuildList() noexcept
	{
		if (m_selectedIndex && *m_selectedIndex >= m_rows.size()) m_selectedIndex.reset();
		std::vector<std::wstring> labels;
		try {
			labels.reserve(m_rows.size());
			for (const auto& row : m_rows) labels.push_back(ProjectsAccessibleLabel(row, m_texts));
		} catch (...) {
			// Without the accessible labels the list cannot name its rows; show none.
			m_rows.clear();
			m_selectedIndex.reset();
			labels.clear();
		}
		m_projectsList.SetRows(labels, m_selectedIndex);
	}

	//! #411: rebuilds the Sessions View's rows from the accepted sessions.
	void RebuildSessions() noexcept
	{
		if (!HasSessionsView()) return;
		std::vector<std::wstring> labels;
		std::optional<std::size_t> selected;
		std::wstring emptyMessage;
		try {
			const ProjectSessionsInput input{
				.state = m_sessionsState,
				.sessions = m_sessions,
				.nowUnixSeconds = NowUnixSeconds(),
				.preferredIdentity = m_selectedSessionIdentity,
				.view = &m_sessionView,
				.archivedExpanded = m_archivedExpanded,
			};
			auto projection = ProjectAgentSessions(input, m_texts);
			labels.reserve(projection.rows.size());
			for (const auto& row : projection.rows) {
				labels.push_back(AgentSessionsAccessibleLabel(row, m_texts));
			}
			m_sessionRows = std::move(projection.rows);
			selected = projection.selectedRowIndex;
			emptyMessage = std::move(projection.emptyMessage);
		} catch (...) {
			m_sessionRows.clear();
			labels.clear();
			selected.reset();
			emptyMessage.clear();
		}
		m_sessionsList.SetEmptyMessage(std::move(emptyMessage));
		m_sessionsList.SetRows(labels, selected);
	}

	void CaptureSessionSelection() noexcept
	{
		const auto selected = m_sessionsList.Selection();
		if (!selected || *selected >= m_sessionRows.size()) return;
		try {
			if (m_sessionRows[*selected].kind == EAgentSessionsRowKind::ArchivedToggle) {
				m_selectedSessionIdentity = kArchivedAgentSessionsRowIdentity;
				return;
			}
			const auto index = m_sessionRows[*selected].sessionIndex;
			if (index >= m_sessions.size()) return;
			m_selectedSessionIdentity = AgentSessionIdentity(m_sessions[index]);
		} catch (...) {
			m_selectedSessionIdentity.clear();
		}
	}

	//! The selected Sessions row as a session; nullopt for the group row.
	[[nodiscard]] std::optional<std::size_t> SelectedSessionIndex() const noexcept
	{
		const auto selected = m_sessionsList.Selection();
		if (!selected || *selected >= m_sessionRows.size()
			|| m_sessionRows[*selected].kind != EAgentSessionsRowKind::Session) return std::nullopt;
		const auto index = m_sessionRows[*selected].sessionIndex;
		return index < m_sessions.size() ? std::optional<std::size_t>(index) : std::nullopt;
	}

	//! Enter or a single click: the Archived group toggles, a session resumes.
	void ActivateSelectedSessionRow() noexcept
	{
		if (!SelectedSessionRowIsGroup()) {
			ResumeSelectedSession();
			return;
		}
		CaptureSessionSelection();
		m_archivedExpanded = !m_archivedExpanded;
		const auto top = TopIndex(m_sessionsList);
		RebuildSessions();
		if (top != LB_ERR) (void)::SendMessageW(m_sessionsList.Handle(), LB_SETTOPINDEX, top, 0);
		m_sessionsList.UpdateScrollbar();
	}

	//! Whether the page can run `command`: its seam exists.
	[[nodiscard]] bool SessionCommandAvailable(const EAgentSessionCommand command) const noexcept
	{
		const auto& seams = m_options.sessionManagement;
		switch (command) {
		case EAgentSessionCommand::Resume: return static_cast<bool>(m_options.resumeAgentSession);
		case EAgentSessionCommand::Rename: return seams.promptTitle && seams.rename;
		case EAgentSessionCommand::Delete: return seams.confirmDelete && seams.remove;
		case EAgentSessionCommand::CopyId:
		case EAgentSessionCommand::CopyResumeCommand: return static_cast<bool>(seams.copyText);
		case EAgentSessionCommand::Pin:
		case EAgentSessionCommand::Unpin:
		case EAgentSessionCommand::Archive:
		case EAgentSessionCommand::Unarchive:
		case EAgentSessionCommand::ShowCount: return SessionViewService() != nullptr;
		}
		return false;
	}

	[[nodiscard]] static std::wstring SessionMenuLabel(const AgentSessionMenuItem& item)
	{
		const auto text = [](const UINT id) { return std::wstring(LS(id)); };
		switch (item.command) {
		case EAgentSessionCommand::Resume: return text(STR_WORKBENCH_PROJECTS_SESSION_RESUME);
		case EAgentSessionCommand::Rename: return text(STR_WORKBENCH_PROJECTS_SESSION_RENAME);
		case EAgentSessionCommand::Pin: return text(STR_WORKBENCH_PROJECTS_SESSION_PIN);
		case EAgentSessionCommand::Unpin: return text(STR_WORKBENCH_PROJECTS_SESSION_UNPIN);
		case EAgentSessionCommand::Archive: return text(STR_WORKBENCH_PROJECTS_SESSION_ARCHIVE);
		case EAgentSessionCommand::Unarchive: return text(STR_WORKBENCH_PROJECTS_SESSION_UNARCHIVE);
		case EAgentSessionCommand::CopyId: return text(STR_WORKBENCH_PROJECTS_SESSION_COPY_ID);
		case EAgentSessionCommand::CopyResumeCommand:
			return text(STR_WORKBENCH_PROJECTS_SESSION_COPY_RESUME_COMMAND);
		case EAgentSessionCommand::Delete: return text(STR_WORKBENCH_PROJECTS_SESSION_DELETE);
		case EAgentSessionCommand::ShowCount: {
			auto label = text(STR_WORKBENCH_PROJECTS_SESSIONS_SHOW_COUNT);
			const auto marker = label.find(L"{0}");
			if (marker != std::wstring::npos) label.replace(marker, 3, std::to_wstring(item.displayCount));
			return label;
		}
		}
		return {};
	}

	//! #408: a right click selects the row under it and opens that row's
	//! menu; Shift+F10 (position -1) opens the focused row's. Off a session
	//! row, the menu holds the display counts only.
	void ShowSessionContextMenu(const LPARAM position) noexcept
	{
		const HWND list = m_sessionsList.Handle();
		if (list == nullptr) return;
		POINT popup{};
		if (position == static_cast<LPARAM>(-1)) {
			RECT bounds{};
			const auto selected = m_sessionsList.Selection();
			if (selected && ::SendMessageW(list, LB_GETITEMRECT, *selected,
				reinterpret_cast<LPARAM>(&bounds)) != LB_ERR) {
				popup = { bounds.left + ScaleDip(kRowInsetDip, m_dpi), bounds.bottom };
			}
			if (::ClientToScreen(list, &popup) == FALSE) return;
		} else {
			popup = {
				static_cast<int>(static_cast<short>(LOWORD(position))),
				static_cast<int>(static_cast<short>(HIWORD(position))),
			};
			POINT client = popup;
			if (::ScreenToClient(list, &client) == FALSE) return;
			const DWORD hit = static_cast<DWORD>(::SendMessageW(list,
				LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y)));
			if (HIWORD(hit) == 0 && LOWORD(hit) < m_sessionRows.size()) {
				(void)::SendMessageW(list, LB_SETCURSEL, LOWORD(hit), 0);
			} else {
				(void)::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
			}
		}
		CaptureSessionSelection();

		// The menu acts on the session it was opened for, even when the list
		// is rebuilt while the menu is up.
		std::optional<AgentSessionRecord> target;
		std::vector<AgentSessionMenuItem> items;
		std::vector<AgentSessionMenuItem> offered;	//! the entries the menu shows, in order
		HMENU menu = nullptr;
		try {
			if (const auto index = SelectedSessionIndex()) {
				target = m_sessions[*index];
				const auto identity = AgentSessionIdentity(*target);
				items = BuildAgentSessionMenu(target->provider, IsAgentSessionPinned(m_sessionView, identity),
					IsAgentSessionArchived(m_sessionView, identity), m_sessionView.displayCount);
			} else {
				items = BuildAgentSessionsGroupMenu(m_sessionView.displayCount);
			}
			menu = ::CreatePopupMenu();
			if (menu == nullptr) return;
			bool separator = false;
			bool any = false;
			offered.reserve(items.size());
			for (std::size_t index = 0; index < items.size(); ++index) {
				separator = separator || items[index].separatorBefore;
				if (!SessionCommandAvailable(items[index].command)) continue;
				if (separator && any) (void)::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				separator = false;
				const auto label = SessionMenuLabel(items[index]);
				UINT flags = MF_STRING;
				if (items[index].checked) flags |= MF_CHECKED;
				if (::AppendMenuW(menu, flags, kSessionMenuFirstId + static_cast<UINT>(index),
					label.c_str()) == FALSE) {
					::DestroyMenu(menu);
					return;
				}
				offered.push_back(items[index]);
				any = true;
			}
			if (!any) {
				::DestroyMenu(menu);
				return;
			}
		} catch (...) {
			if (menu != nullptr) ::DestroyMenu(menu);
			return;
		}
		if (const auto& choose = m_options.sessionManagement.chooseMenuItem) {
			::DestroyMenu(menu);
			std::optional<std::size_t> chosen;
			try { chosen = choose(offered); }
			catch (...) { chosen.reset(); }
			if (chosen && *chosen < offered.size()) RunSessionCommand(offered[*chosen], target);
			return;
		}
		const UINT command = ::TrackPopupMenuEx(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON, popup.x, popup.y, list, nullptr);
		::DestroyMenu(menu);
		if (command < kSessionMenuFirstId || command - kSessionMenuFirstId >= items.size()) return;
		RunSessionCommand(items[command - kSessionMenuFirstId], target);
	}

	//! Applies one view-state change and reprojects. A changed display count
	//! or mark set also changes what discovery must find.
	void UpdateSessionView(const std::function<bool(AgentSessionViewState&)>& change) noexcept
	{
		bool changed = false;
		try {
			const auto service = SessionViewService();
			changed = service != nullptr && service->Update(change);
		} catch (...) {
			changed = false;
		}
		if (!changed) {
			(void)::MessageBeep(MB_ICONWARNING);
			return;
		}
		ReloadSessionView(false);
		const auto top = TopIndex(m_sessionsList);
		RebuildSessions();
		if (top != LB_ERR) (void)::SendMessageW(m_sessionsList.Handle(), LB_SETTOPINDEX, top, 0);
		m_sessionsList.UpdateScrollbar();
		BeginSessionDiscovery();
	}

	void RunSessionCommand(const AgentSessionMenuItem& item,
		const std::optional<AgentSessionRecord>& target) noexcept
	{
		try {
			if (item.command == EAgentSessionCommand::ShowCount) {
				UpdateSessionView([count = item.displayCount](AgentSessionViewState& state) {
					return SetAgentSessionDisplayCount(state, count);
				});
				return;
			}
			if (!target) return;
			const auto identity = AgentSessionIdentity(*target);
			const auto& seams = m_options.sessionManagement;
			switch (item.command) {
			case EAgentSessionCommand::Resume:
				if (!m_options.resumeAgentSession || m_options.resumeAgentSession(*target, m_sessionsFolder)
					== EProjectsActivationStatus::Failed) (void)::MessageBeep(MB_ICONWARNING);
				return;
			case EAgentSessionCommand::Pin:
				UpdateSessionView([&](AgentSessionViewState& state) { return PinAgentSession(state, identity); });
				return;
			case EAgentSessionCommand::Unpin:
				UpdateSessionView([&](AgentSessionViewState& state) { return UnpinAgentSession(state, identity); });
				return;
			case EAgentSessionCommand::Archive:
				UpdateSessionView([&](AgentSessionViewState& state) { return ArchiveAgentSession(state, identity); });
				return;
			case EAgentSessionCommand::Unarchive:
				UpdateSessionView([&](AgentSessionViewState& state) { return UnarchiveAgentSession(state, identity); });
				return;
			case EAgentSessionCommand::CopyId:
				if (!seams.copyText(target->id)) (void)::MessageBeep(MB_ICONWARNING);
				return;
			case EAgentSessionCommand::CopyResumeCommand: {
				const auto command = BuildAgentSessionResumeCommand(target->provider, target->id);
				if (!command || !seams.copyText(*command)) (void)::MessageBeep(MB_ICONWARNING);
				return;
			}
			case EAgentSessionCommand::Rename: {
				if (m_sessionOperation.valid()) {
					(void)::MessageBeep(MB_ICONWARNING);
					return;
				}
				const auto entered = seams.promptTitle(*target);
				if (!entered) return;
				const auto title = PrepareAgentSessionRenameTitle(*entered);
				if (!title) {
					ReportSessionFailure(EAgentSessionCommand::Rename, EAgentSessionOperationStatus::Failed);
					return;
				}
				BeginSessionOperation(EAgentSessionCommand::Rename, identity,
					[rename = seams.rename, session = *target, folder = m_sessionsFolder, title = *title](
						const HANDLE stop) { return rename(session, folder, title, stop); });
				return;
			}
			case EAgentSessionCommand::Delete:
				if (m_sessionOperation.valid()) {
					(void)::MessageBeep(MB_ICONWARNING);
					return;
				}
				if (!seams.confirmDelete(*target, DeletesAgentSessionPermanently(target->provider))) return;
				BeginSessionOperation(EAgentSessionCommand::Delete, identity,
					[remove = seams.remove, session = *target, folder = m_sessionsFolder](
						const HANDLE stop) { return remove(session, folder, stop); });
				return;
			case EAgentSessionCommand::ShowCount:
				return;
			}
		} catch (...) {
			(void)::MessageBeep(MB_ICONWARNING);
		}
	}

	void ReportSessionFailure(const EAgentSessionCommand command,
		const EAgentSessionOperationStatus status) noexcept
	{
		try {
			if (m_options.sessionManagement.reportFailure) {
				m_options.sessionManagement.reportFailure(command, status);
				return;
			}
		} catch (...) {}
		(void)::MessageBeep(MB_ICONWARNING);
	}

	//! Single-flight: one rename or delete at a time, on a worker thread. The
	//! timer observes its end; Cleanup signals `stop` and waits for it.
	void BeginSessionOperation(const EAgentSessionCommand command, std::wstring identity,
		std::function<EAgentSessionOperationStatus(HANDLE)> work) noexcept
	{
		if (m_sessionOperation.valid() || m_window == nullptr) return;
		try {
			m_sessionOperationStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (m_sessionOperationStop == nullptr) throw std::bad_alloc();
			m_sessionOperationCommand = command;
			m_sessionOperationIdentity = std::move(identity);
			m_sessionOperation = std::async(std::launch::async,
				[work = std::move(work), stop = m_sessionOperationStop]() {
					try { return work(stop); }
					catch (...) { return EAgentSessionOperationStatus::Failed; }
				});
		} catch (...) {
			ReleaseSessionOperation();
			ReportSessionFailure(command, EAgentSessionOperationStatus::Failed);
			return;
		}
		if (::SetTimer(m_window, kSessionOperationTimer, kSessionOperationPollMilliseconds, nullptr) == 0) {
			// Nothing could observe the end: wait for it here instead.
			FinishSessionOperation();
		}
	}

	void PollSessionOperation() noexcept
	{
		if (!m_sessionOperation.valid()) {
			if (m_window != nullptr) (void)::KillTimer(m_window, kSessionOperationTimer);
			return;
		}
		if (m_sessionOperation.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
		FinishSessionOperation();
	}

	void FinishSessionOperation() noexcept
	{
		auto status = EAgentSessionOperationStatus::Failed;
		try { status = m_sessionOperation.get(); }
		catch (...) { status = EAgentSessionOperationStatus::Failed; }
		const auto command = m_sessionOperationCommand;
		const auto identity = std::move(m_sessionOperationIdentity);
		ReleaseSessionOperation();
		// NotFound: the session is already gone, so its row is stale either way.
		if (status == EAgentSessionOperationStatus::Succeeded
			|| status == EAgentSessionOperationStatus::NotFound) {
			if (command == EAgentSessionCommand::Delete) {
				// A deleted session keeps no pin or archive mark.
				try {
					if (const auto service = SessionViewService(); service != nullptr) {
						(void)service->Update([&identity](AgentSessionViewState& state) {
							return ForgetAgentSession(state, identity);
						});
					}
				} catch (...) {}
				ReloadSessionView(false);
			}
			// The agent's store changed: read the list again.
			BeginSessionDiscovery();
		}
		if (status != EAgentSessionOperationStatus::Succeeded
			&& status != EAgentSessionOperationStatus::Cancelled) {
			ReportSessionFailure(command, status);
		}
	}

	//! Closes the finished operation's handles; the future is ready or empty.
	void ReleaseSessionOperation() noexcept
	{
		if (m_window != nullptr) (void)::KillTimer(m_window, kSessionOperationTimer);
		m_sessionOperation = {};
		if (m_sessionOperationStop != nullptr) ::CloseHandle(m_sessionOperationStop);
		m_sessionOperationStop = nullptr;
		m_sessionOperationIdentity.clear();
	}

	void CancelSessionOperation() noexcept
	{
		if (m_sessionOperation.valid()) {
			if (m_sessionOperationStop != nullptr) (void)::SetEvent(m_sessionOperationStop);
			try { m_sessionOperation.wait(); }
			catch (...) {}
		}
		ReleaseSessionOperation();
	}

	void ResumeSelectedSession() noexcept
	{
		CaptureSessionSelection();
		const auto selected = m_sessionsList.Selection();
		if (!selected || *selected >= m_sessionRows.size()
			|| m_sessionRows[*selected].kind != EAgentSessionsRowKind::Session) return;
		const auto index = m_sessionRows[*selected].sessionIndex;
		EProjectsActivationStatus status = EProjectsActivationStatus::Failed;
		try {
			if (index < m_sessions.size() && m_options.resumeAgentSession) {
				status = m_options.resumeAgentSession(m_sessions[index], m_sessionsFolder);
			}
		} catch (...) {
			status = EProjectsActivationStatus::Failed;
		}
		if (status == EProjectsActivationStatus::Failed) (void)::MessageBeep(MB_ICONWARNING);
	}

	void CaptureSelection() noexcept
	{
		const auto selected = m_projectsList.Selection();
		if (!selected || *selected >= m_rows.size()) return;
		m_selectedIndex = *selected;
		const auto& row = m_rows[*m_selectedIndex];
		m_selectedKind = row.kind;
		if (row.worktreeIndex && m_worktrees
			&& *row.worktreeIndex < m_worktrees->rows.size()) {
			m_selectedWorktreeIdentity = m_worktrees->rows[*row.worktreeIndex].identity;
		} else {
			m_selectedWorktreeIdentity.clear();
		}
		if (row.kind == EProjectsRowKind::Agent && row.agentIndex
			&& *row.agentIndex < m_agents.size()) {
			m_selectedAgentIdentity = m_agents[*row.agentIndex].identity;
		} else {
			m_selectedAgentIdentity.clear();
		}
	}

	void SetSelectedToggle(const bool expanded) noexcept
	{
		CaptureSelection();
		if (!m_selectedIndex || *m_selectedIndex >= m_rows.size()) return;
		const auto kind = m_rows[*m_selectedIndex].kind;
		if (kind != EProjectsRowKind::WorktreesToggle || m_worktreesExpanded == expanded) return;
		m_worktreesExpanded = expanded;
		m_selectedKind = kind;
		RebuildProjection();
	}

	void ActivateSelection() noexcept
	{
		CaptureSelection();
		if (!m_selectedIndex || *m_selectedIndex >= m_rows.size()) return;
		const auto row = m_rows[*m_selectedIndex];
		if (row.kind == EProjectsRowKind::WorktreesToggle) {
			m_worktreesExpanded = !m_worktreesExpanded;
			m_selectedKind = EProjectsRowKind::WorktreesToggle;
			RebuildProjection();
			return;
		}
		if (!row.enabled) { (void)::MessageBeep(MB_ICONWARNING); return; }
		EProjectsActivationStatus status = EProjectsActivationStatus::Failed;
		try {
			if (row.kind == EProjectsRowKind::Project && row.projectIndex < m_projects.size()) {
				status = m_options.activateProject(m_projects[row.projectIndex], row.currentProject);
			} else if (row.kind == EProjectsRowKind::Agent && row.agentIndex
				&& *row.agentIndex < m_agents.size() && m_options.activateAgent) {
				status = m_options.activateAgent(m_agents[*row.agentIndex].terminalId);
			} else if (row.worktreeIndex && m_worktrees
				&& *row.worktreeIndex < m_worktrees->rows.size()) {
				const auto& worktree = m_worktrees->rows[*row.worktreeIndex];
				status = m_options.activateWorktree(worktree.path,
					worktree.windowState == agent::EAgentWorktreeWindowState::ThisWindow);
			}
		} catch (...) {
			status = EProjectsActivationStatus::Failed;
		}
		if (status == EProjectsActivationStatus::Failed) (void)::MessageBeep(MB_ICONWARNING);
	}

	[[nodiscard]] bool CanRemoveSelection() const noexcept
	{
		const auto selected = m_projectsList.Selection();
		if (!selected || *selected >= m_rows.size()) return false;
		const auto& row = m_rows[*selected];
		return row.kind == EProjectsRowKind::Project && row.projectIndex < m_projects.size();
	}

	void RemoveSelectedProject() noexcept
	{
		CaptureSelection();
		if (!CanRemoveSelection() || !m_selectedIndex) return;
		const auto row = m_rows[*m_selectedIndex];
		const auto project = m_projects[row.projectIndex];
		EProjectsRemovalStatus status = EProjectsRemovalStatus::Failed;
		try {
			status = m_options.removeProject(project);
		} catch (...) {
			status = EProjectsRemovalStatus::Failed;
		}
		if (status != EProjectsRemovalStatus::Removed) {
			(void)::MessageBeep(MB_ICONWARNING);
			return;
		}
		m_selectedIndex.reset();
		m_selectedKind.reset();
		m_selectedWorktreeIdentity.clear();
		m_selectedAgentIdentity.clear();
		Refresh();
	}

	void ShowProjectContextMenu(const LPARAM position) noexcept
	{
		const HWND list = m_projectsList.Handle();
		if (list == nullptr || m_state != EPageState::Loaded) return;
		POINT popup{};
		if (position == static_cast<LPARAM>(-1)) {
			CaptureSelection();
			if (!CanRemoveSelection() || !m_selectedIndex) return;
			RECT rowBounds{};
			if (::SendMessageW(list, LB_GETITEMRECT, *m_selectedIndex,
				reinterpret_cast<LPARAM>(&rowBounds)) == LB_ERR) return;
			popup = { rowBounds.left + ScaleDip(kRowInsetDip, m_dpi), rowBounds.bottom };
			if (::ClientToScreen(list, &popup) == FALSE) return;
		} else {
			popup = {
				static_cast<int>(static_cast<short>(LOWORD(position))),
				static_cast<int>(static_cast<short>(HIWORD(position))),
			};
			POINT client = popup;
			if (::ScreenToClient(list, &client) == FALSE) return;
			const DWORD hit = static_cast<DWORD>(::SendMessageW(list,
				LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y)));
			if (HIWORD(hit) != 0 || LOWORD(hit) >= m_rows.size()) return;
			(void)::SendMessageW(list, LB_SETCURSEL, LOWORD(hit), 0);
			CaptureSelection();
			if (!CanRemoveSelection()) return;
		}

		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		constexpr UINT kOpenProjectInNewWindowMenuId = 0x5102;
		if (::AppendMenuW(menu, MF_STRING, kOpenProjectInNewWindowMenuId,
			LS(STR_WORKBENCH_PROJECTS_OPEN_NEW_WINDOW)) == FALSE) {
			::DestroyMenu(menu);
			return;
		}
		if (::AppendMenuW(menu, MF_STRING, kRemoveProjectMenuId,
			LS(STR_WORKBENCH_PROJECTS_REMOVE)) == FALSE) {
			::DestroyMenu(menu);
			return;
		}
		const UINT command = ::TrackPopupMenuEx(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON, popup.x, popup.y, list, nullptr);
		::DestroyMenu(menu);
		if (command == kOpenProjectInNewWindowMenuId) {
			if (!m_selectedIndex || *m_selectedIndex >= m_rows.size()) return;
			const auto& row = m_rows[*m_selectedIndex];
			if (row.kind != EProjectsRowKind::Project || row.projectIndex >= m_projects.size()
				|| !m_options.activateProjectInNewWindow) return;
			try {
				if (m_options.activateProjectInNewWindow(m_projects[row.projectIndex])
					== EProjectsActivationStatus::Failed) (void)::MessageBeep(MB_ICONWARNING);
			} catch (...) {
				(void)::MessageBeep(MB_ICONWARNING);
			}
		} else if (command == kRemoveProjectMenuId) RemoveSelectedProject();
	}

	void InvalidateContent() noexcept
	{
		m_projectsList.Invalidate();
		m_sessionsList.Invalidate();
		if (m_window != nullptr) ::InvalidateRect(m_window, &m_contentBounds, FALSE);
	}

	void Paint() noexcept
	{
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(m_window, &paint);
		if (dc == nullptr) return;
		RECT client{};
		::GetClientRect(m_window, &client);
		::SetDCBrushColor(dc, m_palette.sideBar.ToColorRef());
		::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		const HGDIOBJ previousFont = m_font.Get() != nullptr ? ::SelectObject(dc, m_font.Get()) : nullptr;
		const int previousMode = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF previousColor = ::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		if (HasSessionsView()) {
			// The first header sits under the container title, which already
			// separates it; every later header draws the section line.
			viewcontainer::PaintViewPaneHeader(dc, m_projectsHeader, m_texts.projects,
				m_projectsCollapsed, m_projectsHeader.top > m_contentBounds.top, m_dpi, m_palette);
			viewcontainer::PaintViewPaneHeader(dc, m_sessionsHeader, m_texts.sessions,
				m_sessionsCollapsed, m_sessionsHeader.top > m_contentBounds.top, m_dpi, m_palette);
		}
		if (m_state != EPageState::Loaded && !(HasSessionsView() && m_projectsCollapsed)) {
			const wchar_t* message = m_state == EPageState::Empty
				? LS(STR_WORKBENCH_PROJECTS_EMPTY)
				: LS(STR_WORKBENCH_PROJECTS_PROFILE_UNAVAILABLE);
			RECT body = m_projectsBody;
			const int inset = ScaleDip(20, m_dpi);
			body.left += inset;
			body.right -= inset;
			(void)::SetTextColor(dc, m_palette.descriptionText.ToColorRef());
			::DrawTextW(dc, message, -1, &body,
				DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
		}
		::SetTextColor(dc, previousColor);
		::SetBkMode(dc, previousMode);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		::EndPaint(m_window, &paint);
	}

	struct RowColors final {
		COLORREF primary{};
		COLORREF secondary{};
	};

	//! Fills one owner-drawn row of either list and selects the row font.
	[[nodiscard]] RowColors BeginRow(const DRAWITEMSTRUCT& item, const bool enabled) noexcept
	{
		const bool selected = (item.itemState & ODS_SELECTED) != 0;
		::SetDCBrushColor(item.hDC, selected
			? m_palette.listActiveSelectionBackground.ToColorRef()
			: m_palette.sideBar.ToColorRef());
		::FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		if (m_font.Get() != nullptr) ::SelectObject(item.hDC, m_font.Get());
		(void)::SetBkMode(item.hDC, TRANSPARENT);
		const COLORREF primary = selected
			? m_palette.listActiveSelectionForeground.ToColorRef()
			: (enabled ? m_palette.primaryText.ToColorRef() : m_palette.descriptionText.ToColorRef());
		return { primary, selected ? primary : m_palette.descriptionText.ToColorRef() };
	}

	//! Draws the right-aligned trailing text, then the label in what it leaves,
	//! then the focus outline.
	void EndRow(const DRAWITEMSTRUCT& item, RECT labelRect, const std::wstring& label,
		const std::wstring& trailing, const RowColors colors) noexcept
	{
		if (!trailing.empty()) {
			SIZE trailingSize{};
			(void)::GetTextExtentPoint32W(item.hDC, trailing.c_str(),
				static_cast<int>(trailing.size()), &trailingSize);
			RECT trailingRect = labelRect;
			trailingRect.left = std::max(trailingRect.left,
				trailingRect.right - trailingSize.cx);
			(void)::SetTextColor(item.hDC, colors.secondary);
			::DrawTextW(item.hDC, trailing.c_str(), static_cast<int>(trailing.size()),
				&trailingRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			labelRect.right = std::max(labelRect.left,
				trailingRect.left - ScaleDip(kColumnGapDip, m_dpi));
		}
		(void)::SetTextColor(item.hDC, colors.primary);
		::DrawTextW(item.hDC, label.c_str(), static_cast<int>(label.size()), &labelRect,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		if ((item.itemState & ODS_FOCUS) != 0) {
			::SetDCBrushColor(item.hDC, m_palette.listFocusAndSelectionOutline.ToColorRef());
			RECT border = item.rcItem;
			::FrameRect(item.hDC, &border, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		}
	}

	[[nodiscard]] bool DrawItem(const DRAWITEMSTRUCT& item) noexcept
	{
		if (item.itemID == static_cast<UINT>(-1)
			|| item.itemID >= m_rows.size() || item.hDC == nullptr) return false;
		const auto& row = m_rows[item.itemID];
		const int savedDc = ::SaveDC(item.hDC);
		const auto colors = BeginRow(item, row.enabled);
		const COLORREF secondary = colors.secondary;

		const int inset = ScaleDip(kRowInsetDip, m_dpi);
		const int childIndent = ScaleDip(kChildIndentDip, m_dpi);
		RECT labelRect = item.rcItem;
		labelRect.left += inset;
		labelRect.right -= inset;
		std::wstring trailing;
		// Agent rows nest under the CurrentWorktree row when that row exists
		// for the same Project, otherwise under the Project row.
		const bool underWorktree = std::ranges::any_of(m_rows, [&row](const auto& candidate) {
			return candidate.kind == EProjectsRowKind::CurrentWorktree
				&& candidate.projectIndex == row.projectIndex;
		});
		const int folderChildIndent = childIndent * (underWorktree ? 2 : 1);
		if (row.kind == EProjectsRowKind::Project) {
			trailing = row.trailing;
		} else if (row.kind == EProjectsRowKind::WorktreesToggle) {
			trailing = std::to_wstring(row.hiddenWorktreeCount);
			// The chevron takes the column just before the group label.
			(void)::SetTextColor(item.hDC, secondary);
			RECT chevron = labelRect;
			chevron.right = chevron.left + childIndent;
			::DrawTextW(item.hDC, row.expanded ? L"v" : L">", -1, &chevron,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			labelRect.left += childIndent;
		} else {
			const int rowIndent = row.kind == EProjectsRowKind::Agent ? folderChildIndent : childIndent;
			labelRect.left += rowIndent;
			if (row.primaryWorktree) trailing = m_texts.primary;
			if (row.kind == EProjectsRowKind::CurrentWorktree) {
				if (!trailing.empty()) trailing += L"  ";
				trailing += m_texts.thisWindow;
			}
			const int dot = ScaleDip(6, m_dpi);
			RECT dotRect = item.rcItem;
			dotRect.left = item.rcItem.left + inset + ScaleDip(2, m_dpi)
				+ (rowIndent - childIndent);
			dotRect.right = dotRect.left + dot;
			dotRect.top += (item.rcItem.bottom - item.rcItem.top - dot) / 2;
			dotRect.bottom = dotRect.top + dot;
			if (row.kind == EProjectsRowKind::Agent) {
				// Phase A presence marker: a filled rounded square in the
				// secondary color, distinct from the worktree dot.
				::SetDCBrushColor(item.hDC, secondary);
				const int corner = ScaleDip(2, m_dpi);
				::RoundRect(item.hDC, dotRect.left, dotRect.top,
					dotRect.right, dotRect.bottom, corner, corner);
			} else {
				::SetDCBrushColor(item.hDC, row.kind == EProjectsRowKind::CurrentWorktree
					? m_palette.activityBarBadgeBackground.ToColorRef() : secondary);
				::Ellipse(item.hDC, dotRect.left, dotRect.top, dotRect.right, dotRect.bottom);
			}
		}
		std::wstring label = row.label;
		if (row.kind != EProjectsRowKind::Project
			&& row.kind != EProjectsRowKind::WorktreesToggle && !row.description.empty()) {
			label += L"  " + row.description;
		}
		EndRow(item, labelRect, label, trailing, colors);
		if (savedDc != 0) ::RestoreDC(item.hDC, savedDc);
		return true;
	}

	//! #411: a Sessions View row is top level. The provider name and the age
	//! stay visible in the trailing column; a long title takes the ellipsis.
	[[nodiscard]] bool DrawSessionItem(const DRAWITEMSTRUCT& item) noexcept
	{
		if (item.itemID == static_cast<UINT>(-1)
			|| item.itemID >= m_sessionRows.size() || item.hDC == nullptr) return false;
		const auto& row = m_sessionRows[item.itemID];
		const int savedDc = ::SaveDC(item.hDC);
		const auto colors = BeginRow(item, true);
		RECT labelRect = item.rcItem;
		const int inset = ScaleDip(kRowInsetDip, m_dpi);
		const int childIndent = ScaleDip(kChildIndentDip, m_dpi);
		labelRect.left += inset;
		labelRect.right -= inset;
		std::wstring trailing;
		if (row.kind == EAgentSessionsRowKind::ArchivedToggle) {
			// The same chevron column as the Projects View's group row.
			(void)::SetTextColor(item.hDC, colors.secondary);
			RECT chevron = labelRect;
			chevron.right = chevron.left + childIndent;
			::DrawTextW(item.hDC, row.expanded ? L"v" : L">", -1, &chevron,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			labelRect.left += childIndent;
			EndRow(item, labelRect, row.label, trailing, colors);
			if (savedDc != 0) ::RestoreDC(item.hDC, savedDc);
			return true;
		}
		if (row.archived) labelRect.left += childIndent;
		if (row.pinned) {
			// VS Code's `pinned` codicon at the row's trailing edge.
			const int size = ScaleDip(kSessionPinIconDip, m_dpi);
			const int top = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - size) / 2;
			const icons::IconRect box{ labelRect.right - size, top, labelRect.right, top + size };
			if (icons::PaintCodiconByName(item.hDC, box, L"pinned", colors.secondary, m_codiconFont)) {
				labelRect.right = box.left - ScaleDip(kColumnGapDip, m_dpi);
				if (m_font.Get() != nullptr) ::SelectObject(item.hDC, m_font.Get());
			}
		}
		try {
			trailing = row.description;
			if (!row.trailing.empty()) trailing += L"  " + row.trailing;
		} catch (...) {
			trailing.clear();
		}
		EndRow(item, labelRect, row.label, trailing, colors);
		if (savedDc != 0) ::RestoreDC(item.hDC, savedDc);
		return true;
	}

	void Cleanup() noexcept
	{
		// Unregister before the window goes away so no stale listener can post
		// into a destroyed HWND.
		if (m_options.agentsChanged) {
			try { m_options.agentsChanged({}); } catch (...) {}
		}
		if (m_window != nullptr) (void)::KillTimer(m_window, kRefreshTimer);
		StopSessionCatchUp();
		CancelSessionDiscovery();
		CancelSessionOperation();
		m_codiconFont.Release();
		m_sessions.clear();
		m_agentFirstSeen.clear();
		m_sessionsFolder.clear();
		m_sessionsState = EProjectSessionsState::Unavailable;
		m_attachedHost.reset();
		m_nativeParent = 0;
		if (m_discovery) (void)m_discovery->Stop();
		m_discovery.reset();
		m_refresh = {};
		m_branchQueue.clear();
		m_currentBranchRequest.reset();
		m_branchCache.clear();
		m_projectBranchKeys.clear();
		m_rows.clear();
		m_projects.clear();
		m_worktrees.reset();
		m_agents.clear();
		m_agentsChangedPending = false;
		if (m_draggingSash) EndSashDrag();
		// Each list takes its tooltip tool back while the tooltip still exists.
		m_projectsList.Destroy();
		m_sessionsList.Destroy();
		m_sessionRows.clear();
		// The tooltip is a top-level popup, owned by the frame rather than by
		// m_window, so destroying m_window would not take it along.
		if (m_tooltip != nullptr && ::IsWindow(m_tooltip)) ::DestroyWindow(m_tooltip);
		m_tooltip = nullptr;
		if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
		m_window = nullptr;
		m_created = false;
	}

	ProjectsPageOptions m_options;
	ProjectsTexts m_texts;
	std::unique_ptr<worktree::GitWorktreeDiscoverySource> m_discovery;
	worktree::GitWorktreeRefresh m_refresh;
	std::vector<ProjectEntry> m_projects;
	config::WorkspaceContextSnapshot m_workspace;
	std::optional<agent::AgentWorkspacesProjectionResult> m_worktrees;
	std::map<std::wstring, ProjectBranchCacheEntry> m_branchCache;
	std::vector<std::wstring> m_projectBranchKeys;
	std::deque<ProjectBranchDiscoveryRequest> m_branchQueue;
	std::optional<ProjectBranchDiscoveryRequest> m_currentBranchRequest;
	std::vector<ProjectsRow> m_rows;
	std::vector<ProjectAgentEntry> m_agents;
	std::optional<std::size_t> m_selectedIndex;
	std::optional<std::size_t> m_currentProjectIndex;
	std::optional<EProjectsRowKind> m_selectedKind;
	std::wstring m_selectedWorktreeIdentity;
	std::wstring m_selectedAgentIdentity;
	std::atomic_bool m_agentsChangedPending{ false };
	std::vector<AgentSessionRecord> m_sessions;
	std::wstring m_sessionsFolder;	//! the folder m_sessions belongs to
	EProjectSessionsState m_sessionsState{ EProjectSessionsState::Unavailable };
	std::wstring m_selectedSessionIdentity;
	//! The Sessions View's rows; each names an index into m_sessions.
	std::vector<AgentSessionsRow> m_sessionRows;
	std::future<std::optional<std::vector<AgentSessionRecord>>> m_sessionDiscovery;
	std::wstring m_sessionDiscoveryFolder;
	HANDLE m_sessionStop = nullptr;	//! manual-reset event owned with m_sessionDiscovery
	bool m_sessionRerunPending = false;
	//! #408: the view state the rows were projected with, and the one rename
	//! or delete running in an agent's store.
	AgentSessionViewState m_sessionView;
	bool m_archivedExpanded = false;
	std::future<EAgentSessionOperationStatus> m_sessionOperation;
	HANDLE m_sessionOperationStop = nullptr;
	EAgentSessionCommand m_sessionOperationCommand{ EAgentSessionCommand::Rename };
	std::wstring m_sessionOperationIdentity;
	icons::CCodiconGlyphFont m_codiconFont;
	//! #410 catch-up: when each present agent row first appeared, and the
	//! backoff state of the rerun that waits for its session.
	std::map<std::wstring, std::int64_t> m_agentFirstSeen;
	std::size_t m_sessionCatchUpAttempts{};
	bool m_sessionCatchUpArmed = false;
	std::wstring m_currentWorkspaceRoot;
	std::optional<viewcontainer::ViewContainerPageHost> m_attachedHost;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	//! #411: the Projects View's list and, with session history, the Sessions
	//! View's list. Both are children of m_window and share m_tooltip.
	CProjectsListPane m_projectsList;
	CProjectsListPane m_sessionsList;
	RECT m_contentBounds{};
	//! The View stack inside m_contentBounds, rebuilt by LayoutPanes. The
	//! headers and the sash stay empty while the container has one View.
	RECT m_projectsHeader{};
	RECT m_projectsBody{};
	RECT m_sessionsHeader{};
	RECT m_sessionsBody{};
	RECT m_sash{};
	//! Collapse state and the sash position live with the page only; VS Code
	//! keeps them in its view-state memento, which this container lacks.
	double m_sessionsShare = kDefaultSessionsShare;
	int m_sashDragOriginY{};
	int m_sashDragOriginBody{};
	EPane m_pressedHeader{ EPane::None };
	bool m_projectsCollapsed = false;
	bool m_sessionsCollapsed = false;
	bool m_draggingSash = false;
	HWND m_window = nullptr;
	HWND m_tooltip = nullptr;
	HWND m_parkingParent = nullptr;
	viewcontainer::ViewContainerNativeHandle m_nativeParent{};
	unsigned int m_dpi = 96;
	EPageState m_state{ EPageState::Empty };
	bool m_worktreesExpanded = false;
	bool m_restartPending = false;
	bool m_active = false;
	bool m_visible = false;
	bool m_layoutValid = true;
	bool m_created = false;
	bool m_closeInvoked = false;
};

} // namespace

std::unique_ptr<viewcontainer::IViewContainerPage> CreateProjectsPage(
	HWND parkingParent, ProjectsPageOptions options) noexcept
{
	try {
		auto page = std::make_unique<CProjectsPage>(std::move(options));
		if (!page->Create(parkingParent)) return {};
		return page;
	} catch (...) {
		return {};
	}
}

} // namespace workbench::projects
