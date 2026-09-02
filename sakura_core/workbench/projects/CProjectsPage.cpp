/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/CProjectsPage.h"

#include "workbench/layout/WorkbenchIds.h"
#include "workbench/projects/ProjectsModel.h"
#include "workbench/worktree/GitWorktreeDiscoverySource.h"

#include <CommCtrl.h>

#include <algorithm>
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
constexpr int kRowHeightDip = 24;
constexpr int kRowInsetDip = 10;
constexpr int kChildIndentDip = 18;
constexpr int kColumnGapDip = 8;
constexpr int kListControlId = 1;
constexpr UINT_PTR kListSubclassId = 0x7072;
constexpr UINT kRemoveProjectMenuId = 1;
constexpr std::size_t kMaximumBranchRepositoriesPerProject = 8;
constexpr std::size_t kMaximumBranchRequestsPerRefresh = 64;

int ScaleDip(const int value, const unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

enum class EPageState : std::uint8_t {
	Empty,
	Loaded,
	Unavailable,
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
		m_options(std::move(options))
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
		m_list = ::CreateWindowExW(0, L"LISTBOX", L"",
			WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED
				| LBS_HASSTRINGS | LBS_NOTIFY,
			0, 0, 0, 0, m_window,
			reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kListControlId)), instance, nullptr);
		if (m_list == nullptr) {
			Cleanup();
			return false;
		}
		if (::SetWindowSubclass(m_list, &CProjectsPage::ListSubclassProc,
			kListSubclassId, reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
			Cleanup();
			return false;
		}
		m_created = true;
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
			if (m_list != nullptr && m_state == EPageState::Loaded) (void)::SetFocus(m_list);
			return;
		}
		m_active = true;
		Refresh();
		if (m_list != nullptr && m_state == EPageState::Loaded) (void)::SetFocus(m_list);
	}

	void DeactivateProjection() noexcept override
	{
		m_active = false;
	}

	[[nodiscard]] bool PreTranslateProjection(MSG& message) noexcept override
	{
		if (!m_active || m_list == nullptr || message.hwnd != m_list
			|| message.message != WM_KEYDOWN) return false;
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
		if (m_list != nullptr) {
			(void)::SetWindowPos(m_list, nullptr, left, top, right - left,
				bottom - top,
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
		}
		m_contentBounds = { left, top, right, bottom };
		m_layoutValid = true;
		ApplyVisibility();
		// Contributed pages participate in the enclosing Workbench frame transaction.
		// Queue the final page and control pixels, but leave synchronous presentation
		// to CEditWnd's one RDW_ALLCHILDREN commit after every Part has moved. Painting
		// this LISTBOX early can publish its resized parent background as a separate
		// compositor frame before the parent's committed redraw reaches the child.
		if (m_list != nullptr) {
			::RedrawWindow(m_list, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE);
		}
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
		if (m_window != nullptr) {
			::RedrawWindow(m_window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
		}
	}

	void RefreshProjectionStrings() noexcept override
	{
		if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
	}

	void RefreshProjectionContent() noexcept override
	{
		if (m_active) Refresh();
	}

	static LRESULT CALLBACK ListSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data)
	{
		auto* self = reinterpret_cast<CProjectsPage*>(data);
		if (message == WM_NCDESTROY) {
			(void)::RemoveWindowSubclass(window, &CProjectsPage::ListSubclassProc, id);
			if (self != nullptr && self->m_list == window) self->m_list = nullptr;
			return ::DefSubclassProc(window, message, wParam, lParam);
		}
		if (self != nullptr) {
			if (message == WM_CONTEXTMENU) {
				self->ShowProjectContextMenu(lParam);
				return 0;
			}
			if (message == WM_KEYDOWN && wParam == VK_DELETE && self->CanRemoveSelection()) {
				self->RemoveSelectedProject();
				return 0;
			}
		}
		return ::DefSubclassProc(window, message, wParam, lParam);
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
		case WM_DRAWITEM:
			if (self->DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
			break;
		case WM_CTLCOLORLISTBOX: {
			const HDC dc = reinterpret_cast<HDC>(wParam);
			::SetTextColor(dc, self->m_palette.primaryText.ToColorRef());
			::SetBkColor(dc, self->m_palette.sideBar.ToColorRef());
			::SetDCBrushColor(dc, self->m_palette.sideBar.ToColorRef());
			return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
		}
		case WM_TIMER:
			if (wParam == kRefreshTimer) { self->PollRefresh(); return 0; }
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == kListControlId && HIWORD(wParam) == LBN_SELCHANGE) {
				self->CaptureSelection();
				return 0;
			}
			if (LOWORD(wParam) == kListControlId && HIWORD(wParam) == LBN_DBLCLK) {
				self->ActivateSelection();
				return 0;
			}
			break;
		case WM_SETFOCUS:
			if (self->m_list != nullptr && self->m_state == EPageState::Loaded) {
				(void)::SetFocus(self->m_list);
				return 0;
			}
			break;
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
		if (m_list != nullptr) {
			::SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.Get()), FALSE);
			::SendMessageW(m_list, LB_SETITEMHEIGHT, 0, ScaleDip(kRowHeightDip, m_dpi));
		}
	}

	void Show(const bool visible) noexcept
	{
		if (m_list != nullptr) ::ShowWindow(m_list,
			visible && m_state == EPageState::Loaded ? SW_SHOW : SW_HIDE);
		if (m_window != nullptr) ::ShowWindow(m_window, visible ? SW_SHOW : SW_HIDE);
	}

	void ApplyVisibility() noexcept
	{
		Show(m_visible && m_layoutValid && m_attachedHost.has_value());
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
			m_restartPending = true;
			(void)m_discovery->CancelCurrent();
			return;
		}
		BeginBranchRefresh();
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
						.label = L"Git unavailable",
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
					entry.observations.empty(), entry.truncated);
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
				entry.completed == entry.observations.size(), entry.truncated);
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
								projected.rows[*projected.currentIndex]);
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
						entry.completed == entry.observations.size(), entry.truncated);
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

	void RebuildProjection() noexcept
	{
		try {
			std::vector<ProjectBranchSummary> summaries;
			summaries.reserve(m_projectBranchKeys.size());
			for (const auto& identity : m_projectBranchKeys) {
				const auto found = m_branchCache.find(identity);
				summaries.push_back(found == m_branchCache.end()
					? ProjectBranchSummary{} : found->second.summary);
			}
			const auto projection = ProjectProjects(m_projects, m_workspace,
				m_worktrees ? &*m_worktrees : nullptr, m_worktreesExpanded,
				summaries, m_selectedKind, m_selectedWorktreeIdentity);
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
		if (m_list == nullptr) return;
		::SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
		(void)::SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
		for (const auto& row : m_rows) {
			const auto accessible = ProjectsAccessibleLabel(row);
			(void)::SendMessageW(m_list, LB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(accessible.c_str()));
		}
		if (m_selectedIndex && *m_selectedIndex < m_rows.size()) {
			(void)::SendMessageW(m_list, LB_SETCURSEL, *m_selectedIndex, 0);
		} else {
			m_selectedIndex.reset();
		}
		::SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
		::RedrawWindow(m_list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
	}

	void CaptureSelection() noexcept
	{
		if (m_list == nullptr) return;
		const LRESULT selected = ::SendMessageW(m_list, LB_GETCURSEL, 0, 0);
		if (selected == LB_ERR || static_cast<std::size_t>(selected) >= m_rows.size()) return;
		m_selectedIndex = static_cast<std::size_t>(selected);
		const auto& row = m_rows[*m_selectedIndex];
		m_selectedKind = row.kind;
		if (row.worktreeIndex && m_worktrees
			&& *row.worktreeIndex < m_worktrees->rows.size()) {
			m_selectedWorktreeIdentity = m_worktrees->rows[*row.worktreeIndex].identity;
		} else {
			m_selectedWorktreeIdentity.clear();
		}
	}

	void SetSelectedToggle(const bool expanded) noexcept
	{
		CaptureSelection();
		if (!m_selectedIndex || *m_selectedIndex >= m_rows.size()
			|| m_rows[*m_selectedIndex].kind != EProjectsRowKind::WorktreesToggle
			|| m_worktreesExpanded == expanded) return;
		m_worktreesExpanded = expanded;
		m_selectedKind = EProjectsRowKind::WorktreesToggle;
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
		if (m_list == nullptr) return false;
		const LRESULT selected = ::SendMessageW(m_list, LB_GETCURSEL, 0, 0);
		if (selected == LB_ERR || static_cast<std::size_t>(selected) >= m_rows.size()) return false;
		const auto& row = m_rows[static_cast<std::size_t>(selected)];
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
		Refresh();
	}

	void ShowProjectContextMenu(const LPARAM position) noexcept
	{
		if (m_list == nullptr || m_state != EPageState::Loaded) return;
		POINT popup{};
		if (position == static_cast<LPARAM>(-1)) {
			CaptureSelection();
			if (!CanRemoveSelection() || !m_selectedIndex) return;
			RECT rowBounds{};
			if (::SendMessageW(m_list, LB_GETITEMRECT, *m_selectedIndex,
				reinterpret_cast<LPARAM>(&rowBounds)) == LB_ERR) return;
			popup = { rowBounds.left + ScaleDip(kRowInsetDip, m_dpi), rowBounds.bottom };
			if (::ClientToScreen(m_list, &popup) == FALSE) return;
		} else {
			popup = {
				static_cast<int>(static_cast<short>(LOWORD(position))),
				static_cast<int>(static_cast<short>(HIWORD(position))),
			};
			POINT client = popup;
			if (::ScreenToClient(m_list, &client) == FALSE) return;
			const DWORD hit = static_cast<DWORD>(::SendMessageW(m_list,
				LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y)));
			if (HIWORD(hit) != 0 || LOWORD(hit) >= m_rows.size()) return;
			(void)::SendMessageW(m_list, LB_SETCURSEL, LOWORD(hit), 0);
			CaptureSelection();
			if (!CanRemoveSelection()) return;
		}

		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		constexpr UINT kOpenProjectInNewWindowMenuId = 0x5102;
		if (::AppendMenuW(menu, MF_STRING, kOpenProjectInNewWindowMenuId,
			L"Open in New Window") == FALSE) {
			::DestroyMenu(menu);
			return;
		}
		if (::AppendMenuW(menu, MF_STRING, kRemoveProjectMenuId,
			L"Remove from Projects") == FALSE) {
			::DestroyMenu(menu);
			return;
		}
		const UINT command = ::TrackPopupMenuEx(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON, popup.x, popup.y, m_list, nullptr);
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
		if (m_list != nullptr) ::InvalidateRect(m_list, nullptr, FALSE);
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
		if (m_state != EPageState::Loaded) {
			const wchar_t* message = m_state == EPageState::Empty
				? L"Open a folder or workspace to add a Project."
				: L"Projects are unavailable for this profile.";
			RECT body = m_contentBounds;
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

	[[nodiscard]] bool DrawItem(const DRAWITEMSTRUCT& item) noexcept
	{
		if (item.CtlID != kListControlId || item.itemID == static_cast<UINT>(-1)
			|| item.itemID >= m_rows.size() || item.hDC == nullptr) return false;
		const auto& row = m_rows[item.itemID];
		const bool selected = (item.itemState & ODS_SELECTED) != 0;
		::SetDCBrushColor(item.hDC, selected
			? m_palette.listActiveSelectionBackground.ToColorRef()
			: m_palette.sideBar.ToColorRef());
		::FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		const HGDIOBJ previousFont = m_font.Get() != nullptr ? ::SelectObject(item.hDC, m_font.Get()) : nullptr;
		const int previousMode = ::SetBkMode(item.hDC, TRANSPARENT);
		const COLORREF primary = selected
			? m_palette.listActiveSelectionForeground.ToColorRef()
			: (row.enabled ? m_palette.primaryText.ToColorRef() : m_palette.descriptionText.ToColorRef());
		const COLORREF secondary = selected ? primary : m_palette.descriptionText.ToColorRef();

		const int inset = ScaleDip(kRowInsetDip, m_dpi);
		const int childIndent = ScaleDip(kChildIndentDip, m_dpi);
		RECT labelRect = item.rcItem;
		labelRect.left += inset;
		labelRect.right -= inset;
		std::wstring trailing;
		if (row.kind == EProjectsRowKind::Project) {
			trailing = row.trailing;
		} else if (row.kind == EProjectsRowKind::WorktreesToggle) {
			trailing = std::to_wstring(row.hiddenWorktreeCount);
			(void)::SetTextColor(item.hDC, secondary);
			RECT chevron = labelRect;
			chevron.right = chevron.left + childIndent;
			::DrawTextW(item.hDC, row.expanded ? L"v" : L">", -1, &chevron,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			labelRect.left += childIndent;
		} else {
			labelRect.left += childIndent;
			if (row.primaryWorktree) trailing = L"Primary";
			if (row.kind == EProjectsRowKind::CurrentWorktree) {
				if (!trailing.empty()) trailing += L"  ";
				trailing += L"This Window";
			}
			const int dot = ScaleDip(6, m_dpi);
			RECT dotRect = item.rcItem;
			dotRect.left = item.rcItem.left + inset + ScaleDip(2, m_dpi);
			dotRect.right = dotRect.left + dot;
			dotRect.top += (item.rcItem.bottom - item.rcItem.top - dot) / 2;
			dotRect.bottom = dotRect.top + dot;
			::SetDCBrushColor(item.hDC, row.kind == EProjectsRowKind::CurrentWorktree
				? m_palette.activityBarBadgeBackground.ToColorRef() : secondary);
			::Ellipse(item.hDC, dotRect.left, dotRect.top, dotRect.right, dotRect.bottom);
		}
		if (!trailing.empty()) {
			SIZE trailingSize{};
			(void)::GetTextExtentPoint32W(item.hDC, trailing.c_str(),
				static_cast<int>(trailing.size()), &trailingSize);
			RECT trailingRect = labelRect;
			trailingRect.left = std::max(trailingRect.left,
				trailingRect.right - trailingSize.cx);
			(void)::SetTextColor(item.hDC, secondary);
			::DrawTextW(item.hDC, trailing.c_str(), static_cast<int>(trailing.size()),
				&trailingRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			labelRect.right = std::max(labelRect.left,
				trailingRect.left - ScaleDip(kColumnGapDip, m_dpi));
		}
		std::wstring label = row.label;
		if (row.kind != EProjectsRowKind::Project
			&& row.kind != EProjectsRowKind::WorktreesToggle && !row.description.empty()) {
			label += L"  " + row.description;
		}
		(void)::SetTextColor(item.hDC, primary);
		::DrawTextW(item.hDC, label.c_str(), static_cast<int>(label.size()), &labelRect,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		if ((item.itemState & ODS_FOCUS) != 0) {
			::SetDCBrushColor(item.hDC, m_palette.listFocusAndSelectionOutline.ToColorRef());
			RECT border = item.rcItem;
			::FrameRect(item.hDC, &border, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		}
		::SetBkMode(item.hDC, previousMode);
		if (previousFont != nullptr) ::SelectObject(item.hDC, previousFont);
		return true;
	}

	void Cleanup() noexcept
	{
		if (m_window != nullptr) (void)::KillTimer(m_window, kRefreshTimer);
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
		if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
		m_list = nullptr;
		m_window = nullptr;
		m_created = false;
	}

	ProjectsPageOptions m_options;
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
	std::optional<std::size_t> m_selectedIndex;
	std::optional<std::size_t> m_currentProjectIndex;
	std::optional<EProjectsRowKind> m_selectedKind;
	std::wstring m_selectedWorktreeIdentity;
	std::wstring m_currentWorkspaceRoot;
	std::optional<viewcontainer::ViewContainerPageHost> m_attachedHost;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	RECT m_contentBounds{};
	HWND m_window = nullptr;
	HWND m_list = nullptr;
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
