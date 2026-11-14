/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalTool.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

#include "terminal/DefaultTerminalLaunchProfileService.h"
#include "terminal/input/TerminalShortcutPreset.h"
#include "terminal/window/TerminalClipboardPaste.h"
#include "terminal/window/TerminalGroupLayout.h"
#include "terminal/window/TerminalHeaderLayout.h"
#include "terminal/window/TerminalPaneLayout.h"
#include "terminal/window/TerminalTabPresentation.h"
#include "terminal/window/TerminalAgentPresence.h"
#include "terminal/window/CTerminalWnd.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/ThemeIconResolver.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <imm.h>
#include <windowsx.h>

namespace terminal {
namespace {

constexpr wchar_t kTerminalToolWindowClass[] = L"SakuraNativeTerminalTool";
constexpr unsigned int kDefaultDpi = 96;
constexpr int kTabHeightDip = 30;
constexpr int kTerminalTabsRowHeightDip = 24;
constexpr UINT kOutputAvailableMessage = WM_APP + 0x3a1;
constexpr UINT kStateChangedMessage = WM_APP + 0x3a2;
constexpr UINT kJobProcessMessage = WM_APP + 0x3a3;
constexpr UINT kTopologyChangedMessage = WM_APP + 0x3a4;
constexpr UINT_PTR kSynchronizedOutputTimer = 0x5344;
constexpr UINT_PTR kProtocolInputRetryTimer = 0x5346;
constexpr UINT kProtocolInputRetryMilliseconds = 30;
constexpr ULONGLONG kSynchronizedOutputMaximumMilliseconds = 100;
constexpr UINT kCommandNewTerminal = 1;
constexpr UINT kCommandRestartTerminal = 2;
constexpr UINT kCommandCloseTerminal = 3;
constexpr UINT kCommandRedetectPowerShell = 4;
constexpr UINT kCommandSplitTerminal = 5;
constexpr UINT kCommandCloseSplit = 6;
constexpr UINT kCommandSplitTerminalDown = 7;
constexpr UINT kCommandRunTask = 8;
constexpr UINT kCommandRunBuildTask = 9;
constexpr UINT kCommandUnsplitTerminal = 10;
constexpr UINT kCommandProfileFirst = 1000;
constexpr UINT kCommandTabFirst = 2000;
constexpr UINT kCommandShortcutPresetFirst = 3000;

//! Menu order of the multiplexer keybinding presets.
constexpr std::array<TerminalShortcutPreset, 3> kShortcutPresets{
	TerminalShortcutPreset::None,
	TerminalShortcutPreset::Tmux,
	TerminalShortcutPreset::Screen,
};

int ShortcutPresetLabelId( TerminalShortcutPreset preset ) noexcept
{
	switch( preset ) {
	case TerminalShortcutPreset::Tmux: return STR_TERMINAL_SHORTCUT_PRESET_TMUX;
	case TerminalShortcutPreset::Screen: return STR_TERMINAL_SHORTCUT_PRESET_SCREEN;
	case TerminalShortcutPreset::None: break;
	}
	return STR_TERMINAL_SHORTCUT_PRESET_NONE;
}

int ScaleDip( int value, unsigned int dpi ) noexcept
{
	return std::max(0, ::MulDiv(value, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 96));
}

bool EnsureToolClass( HINSTANCE instance )
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = CTerminalTool::WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kTerminalToolWindowClass;
	if( ::RegisterClassExW(&windowClass) != 0 ) return true;
	return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

struct NotificationGate {
	std::mutex mutex;
	HWND window{};
	bool alive{ true };
	bool stateWakePending{};
	//! One coalesced wake for any number of runtime topology changes (#403).
	bool topologyWakePending{};
	std::unordered_set<std::uint64_t> outputWakePending;
	//! Job-membership hints (#402) that could change a tab's agent presence.
	std::unordered_set<std::uint64_t> jobWakePending;
	//! Agent processes per tab from the last UI-thread observation. Only their
	//! exit, or the start of an agent image, wakes the UI thread, so a build that
	//! spawns hundreds of compilers costs one image-name check per process here.
	std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> agentProcessIds;
};

//! Runs on the backend notification worker with the gate locked.
bool IsAgentPresenceHint( const NotificationGate& gate, const TerminalTabEvent& event )
{
	if( !event.jobProcess ) return false;
	if( event.jobProcess->kind == TerminalJobProcessEventKind::Started ) {
		return TerminalAgentKindFromImageFileName(event.jobProcess->imageFileName).has_value();
	}
	const auto tracked = gate.agentProcessIds.find(event.tabId);
	return tracked != gate.agentProcessIds.end()
		&& std::find(tracked->second.begin(), tracked->second.end(), event.jobProcess->processId)
			!= tracked->second.end();
}

std::wstring FormatProfileLabel( const TerminalProfile& profile )
{
	std::wstring label = L"PowerShell " + std::to_wstring(profile.version.major) + L"."
		+ std::to_wstring(profile.version.minor) + L"." + std::to_wstring(profile.version.patch);
	if( profile.channel == TerminalChannel::Preview ) {
		label += L" (";
		label += LS(STR_TERMINAL_PROFILE_PREVIEW);
		label += L")";
	} else if( profile.channel == TerminalChannel::Legacy ) {
		label += L" (";
		label += LS(STR_TERMINAL_PROFILE_LEGACY);
		label += L")";
	}
	label += L"  -  ";
	label += profile.path;
	return label;
}

bool IsEmptyRect( const RECT& rect ) noexcept
{
	return rect.right <= rect.left || rect.bottom <= rect.top;
}

POINT CenterOf( const RECT& rect ) noexcept
{
	return { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
}

void FillSolidRect( HDC dc, const RECT& rect, COLORREF color )
{
	if( IsEmptyRect(rect) ) return;
	const HBRUSH brush = ::CreateSolidBrush(color);
	if( brush ) {
		::FillRect(dc, &rect, brush);
		::DeleteObject(brush);
	}
}

//! The default terminal icon is a real VS Code codicon projection. The
//! resolver supplies the embedded codicon glyph when available and its
//! existing vector fallback otherwise; the terminal row never fabricates a
//! text marker in the icon slot.
void DrawTerminalTabIcon( HDC dc, const RECT& bounds, COLORREF color )
{
	if( IsEmptyRect(bounds) ) return;
	const auto icon = workbench::icons::ResolveThemeIcon(
		L"terminal", workbench::icons::CCodiconFont::Instance().FaceName());
	if( !icon.font ) {
		workbench::icons::codicons::Draw(dc,
			workbench::icons::IconRect{ bounds.left, bounds.top, bounds.right, bounds.bottom },
			icon.builtin, color);
		return;
	}
	const int side = std::max(1, std::min(static_cast<int>(bounds.right - bounds.left),
		static_cast<int>(bounds.bottom - bounds.top)));
	const HFONT glyphFont = workbench::icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName, side);
	if( glyphFont == nullptr || icon.fontIcon.glyph.empty() ) {
		if( glyphFont != nullptr ) ::DeleteObject(glyphFont);
		return;
	}
	const HGDIOBJ oldFont = ::SelectObject(dc, glyphFont);
	const int oldBkMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF oldColor = ::SetTextColor(dc, color);
	RECT glyph = bounds;
	::DrawTextW(dc, icon.fontIcon.glyph.c_str(), static_cast<int>(icon.fontIcon.glyph.size()), &glyph,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::SetTextColor(dc, oldColor);
	::SetBkMode(dc, oldBkMode);
	::SelectObject(dc, oldFont);
	::DeleteObject(glyphFont);
}

void DrawHeaderIcon( HDC dc, const RECT& bounds, TerminalHeaderTarget target, bool maximized,
	COLORREF color, unsigned int dpi )
{
	if( IsEmptyRect(bounds) ) return;
	const POINT center = CenterOf(bounds);
	const int stroke = workbench::icons::LineStrokePixels(dpi);
	const int half = std::max(3, ScaleDip(6, dpi));
	const HPEN pen = ::CreatePen(PS_SOLID, stroke, color);
	if( !pen ) return;
	const auto oldPen = ::SelectObject(dc, pen);
	const auto oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

	switch( target ) {
	case TerminalHeaderTarget::Profile: {
		const int left = center.x - half;
		const int top = center.y - std::max(3, ScaleDip(5, dpi));
		const int right = center.x + half;
		const int bottom = center.y + std::max(3, ScaleDip(5, dpi));
		::Rectangle(dc, left, top, right, bottom);
		::MoveToEx(dc, left + ScaleDip(3, dpi), top + ScaleDip(3, dpi), nullptr);
		::LineTo(dc, left + ScaleDip(6, dpi), center.y);
		::LineTo(dc, left + ScaleDip(3, dpi), bottom - ScaleDip(3, dpi));
		::MoveToEx(dc, left + ScaleDip(7, dpi), bottom - ScaleDip(3, dpi), nullptr);
		::LineTo(dc, right - ScaleDip(2, dpi), bottom - ScaleDip(3, dpi));
		break;
	}
	case TerminalHeaderTarget::New:
		::MoveToEx(dc, center.x - half / 2, center.y, nullptr);
		::LineTo(dc, center.x + half / 2 + 1, center.y);
		::MoveToEx(dc, center.x, center.y - half / 2, nullptr);
		::LineTo(dc, center.x, center.y + half / 2 + 1);
		break;
	case TerminalHeaderTarget::Dropdown:
		::MoveToEx(dc, center.x - ScaleDip(3, dpi), center.y - ScaleDip(1, dpi), nullptr);
		::LineTo(dc, center.x, center.y + ScaleDip(2, dpi));
		::LineTo(dc, center.x + ScaleDip(3, dpi), center.y - ScaleDip(1, dpi));
		break;
	case TerminalHeaderTarget::Split: {
		const int left = center.x - half;
		const int top = center.y - std::max(3, ScaleDip(5, dpi));
		const int right = center.x + half;
		const int bottom = center.y + std::max(3, ScaleDip(5, dpi));
		::Rectangle(dc, left, top, right, bottom);
		::MoveToEx(dc, center.x, top, nullptr);
		::LineTo(dc, center.x, bottom);
		break;
	}
	case TerminalHeaderTarget::SplitDown: {
		const int left = center.x - half;
		const int top = center.y - std::max(3, ScaleDip(5, dpi));
		const int right = center.x + half;
		const int bottom = center.y + std::max(3, ScaleDip(5, dpi));
		::Rectangle(dc, left, top, right, bottom);
		::MoveToEx(dc, left, center.y, nullptr);
		::LineTo(dc, right, center.y);
		break;
	}
	case TerminalHeaderTarget::Kill: {
		const int bodyHalf = std::max(3, ScaleDip(4, dpi));
		const int top = center.y - ScaleDip(4, dpi);
		const int bottom = center.y + ScaleDip(6, dpi);
		::Rectangle(dc, center.x - bodyHalf, top, center.x + bodyHalf, bottom);
		::MoveToEx(dc, center.x - bodyHalf - ScaleDip(2, dpi), top - ScaleDip(2, dpi), nullptr);
		::LineTo(dc, center.x + bodyHalf + ScaleDip(2, dpi), top - ScaleDip(2, dpi));
		::MoveToEx(dc, center.x - ScaleDip(2, dpi), top - ScaleDip(4, dpi), nullptr);
		::LineTo(dc, center.x + ScaleDip(2, dpi), top - ScaleDip(4, dpi));
		break;
	}
	case TerminalHeaderTarget::More: {
		const auto oldDotBrush = ::SelectObject(dc, ::CreateSolidBrush(color));
		const int radius = std::max(1, ScaleDip(1, dpi));
		for( int offset : { -ScaleDip(4, dpi), 0, ScaleDip(4, dpi) } ) {
			::Ellipse(dc, center.x + offset - radius, center.y - radius,
				center.x + offset + radius + 1, center.y + radius + 1);
		}
		const HGDIOBJ dotBrush = ::SelectObject(dc, oldDotBrush);
		if( dotBrush ) ::DeleteObject(dotBrush);
		break;
	}
	case TerminalHeaderTarget::Maximize: {
		const int left = center.x - half;
		const int top = center.y - std::max(3, ScaleDip(5, dpi));
		const int right = center.x + half;
		const int bottom = center.y + std::max(3, ScaleDip(5, dpi));
		if( maximized ) {
			::Rectangle(dc, left + ScaleDip(2, dpi), top, right, bottom - ScaleDip(2, dpi));
			::Rectangle(dc, left, top + ScaleDip(2, dpi), right - ScaleDip(2, dpi), bottom);
		} else {
			::Rectangle(dc, left, top, right, bottom);
		}
		break;
	}
	case TerminalHeaderTarget::Close:
		::MoveToEx(dc, center.x - ScaleDip(4, dpi), center.y - ScaleDip(4, dpi), nullptr);
		::LineTo(dc, center.x + ScaleDip(4, dpi) + 1, center.y + ScaleDip(4, dpi) + 1);
		::MoveToEx(dc, center.x + ScaleDip(4, dpi), center.y - ScaleDip(4, dpi), nullptr);
		::LineTo(dc, center.x - ScaleDip(4, dpi) - 1, center.y + ScaleDip(4, dpi) + 1);
		break;
	default:
		break;
	}

	::SelectObject(dc, oldBrush);
	::SelectObject(dc, oldPen);
	::DeleteObject(pen);
}

} // namespace

struct CTerminalTool::Impl {
	explicit Impl( TerminalTabManagerDependencies dependencies )
		: gate(std::make_shared<NotificationGate>())
	{
		profileService = dependencies.launchProfiles
			? dependencies.launchProfiles
			: std::make_shared<CDefaultTerminalLaunchProfileService>();
		if( !dependencies.createSession ) {
			dependencies.createSession = [](TerminalSessionCallbacks callbacks) {
				return std::make_unique<CTerminalSession>(CreateConPtyTerminalBackend(), std::move(callbacks));
			};
		}
		if( !dependencies.resolveLaunch ) {
			const auto profiles = profileService;
			dependencies.resolveLaunch = [profiles](TerminalSize size, std::wstring_view workingDirectory) {
				return profiles->Resolve(size, workingDirectory);
			};
		}
		const std::weak_ptr<NotificationGate> weakGate = gate;
		manager = std::make_unique<TerminalTabManager>(std::move(dependencies), [weakGate](const TerminalTabEvent& event) {
			const auto gate = weakGate.lock();
			if( !gate ) return;
			HWND target = nullptr;
			bool post = false;
			{
				const std::lock_guard lock(gate->mutex);
				if( gate->alive ) {
					target = gate->window;
					if( event.kind == TerminalTabEventKind::OutputAvailable ) {
						post = gate->outputWakePending.insert(event.tabId).second;
					} else if( event.kind == TerminalTabEventKind::JobProcess ) {
						post = IsAgentPresenceHint(*gate, event)
							&& gate->jobWakePending.insert(event.tabId).second;
					} else if( event.kind == TerminalTabEventKind::TopologyChanged ) {
						post = !gate->topologyWakePending;
						gate->topologyWakePending = true;
					} else if( !gate->stateWakePending ) {
						gate->stateWakePending = true;
						post = true;
					}
				}
			}
			if( !target || !post ) return;
			const UINT message = event.kind == TerminalTabEventKind::OutputAvailable
				? kOutputAvailableMessage
				: event.kind == TerminalTabEventKind::JobProcess ? kJobProcessMessage
				: event.kind == TerminalTabEventKind::TopologyChanged ? kTopologyChangedMessage
				: kStateChangedMessage;
			if( ::PostMessageW(target, message, static_cast<WPARAM>(event.tabId), 0) ) return;
			const std::lock_guard lock(gate->mutex);
			if( event.kind == TerminalTabEventKind::OutputAvailable ) {
				gate->outputWakePending.erase(event.tabId);
			} else if( event.kind == TerminalTabEventKind::JobProcess ) {
				gate->jobWakePending.erase(event.tabId);
			} else if( event.kind == TerminalTabEventKind::TopologyChanged ) {
				gate->topologyWakePending = false;
			} else {
				gate->stateWakePending = false;
			}
		});
	}

	std::shared_ptr<NotificationGate> gate;
	std::shared_ptr<CDefaultTerminalLaunchProfileService> profileService;
	std::unique_ptr<TerminalTabManager> manager;
	TerminalNativeFrameBridgePtr nativeFrameBridge;
	std::uint64_t nativeDeviceEpoch = 1;
	//! Which tabs host an AI agent CLI (#402). Updated only by events: a title
	//! change re-resolves from cached state, and only an agent start/exit hint or
	//! a session/tab-set change re-observes a job. Nothing here polls.
	TerminalAgentPresenceTracker agentPresence;
	std::function<void()> agentPresenceSink;

	// A terminal session belongs to exactly one group; only the selected group
	// owns native viewport HWNDs at a time. The ordered groups and each group's
	// recursive split tree live in groupLayout, the HWND-free model.
	struct TerminalPane final {
		std::uint64_t tabId{};
		std::unique_ptr<CTerminalWnd> window;
		bool needsFullRepaint{};
		ULONGLONG synchronizedOutputSince{};
	};

	struct WorkspaceProjectionState final {
		TerminalDetachedTabProjection tabs;
		TerminalGroupLayout groupLayout;
		std::wstring workingDirectory;
		bool terminalTabsFocused{};
		std::size_t terminalTabsFirstVisible{};
		bool renderersMaterialized{};
	};

	struct WorkspaceProjectionApplyResult final {
		bool succeeded{};
		bool startedSession{};
		std::size_t attachedTabCount{};
		std::uint32_t errorCode{};
		TerminalWorkspaceSwitchOutcome failureOutcome{ TerminalWorkspaceSwitchOutcome::AttachFailed };
	};

	TerminalGroupLayout groupLayout;
	std::vector<TerminalPane> panes;
	TerminalPaneLayoutResult paneLayout;
	bool destroyingPaneRenderers{};
	HWND window{};
	HINSTANCE instance{};
	unsigned int dpi{ kDefaultDpi };
	RECT bounds{};
	HWND headerHost{};
	RECT hostedHeaderBounds{};
	unsigned int hostedHeaderDpi{ kDefaultDpi };
	std::wstring workingDirectory;
	bool active{};
	bool closed{};
	bool protocolInputRetryScheduled{};
	std::optional<std::size_t> draggingPaneDivider;
	bool terminalTabsFocused{};
	std::size_t terminalTabsFirstVisible{};
	//! terminal.integrated.tabs.* presentation contract. Defaults match VS Code;
	//! PR 1B pushes the configured snapshot in from CBottomPanelTool.
	TerminalTabPresentationSettings tabPresentationSettings;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont chromeFont;
	TerminalPanelActions panelActions;
	TerminalHeaderTarget hotTarget{ TerminalHeaderTarget::None };
	TerminalHeaderTarget pressedTarget{ TerminalHeaderTarget::None };
	bool trackingMouseLeave{};
	std::vector<std::wstring> profileCommandPaths;
	std::vector<std::uint64_t> tabCommandIds;
	//! Multiplexer keybinding preset. GNU Screen is the owner-chosen default and
	//! matches the registered default of `sakura.terminal.shortcutPreset`, so a
	//! window with no configuration service behaves like one that read it.
	TerminalShortcutPreset shortcutPreset{ TerminalShortcutPreset::Screen };
	bool shortcutPrefixArmed{};
	std::function<void(TerminalShortcutPreset)> shortcutPresetSink;
	std::unordered_map<std::wstring, WorkspaceProjectionState> workspaceRegistry;
	std::wstring currentWorkspaceIdentityKey;

	TerminalPane* FindPane( std::uint64_t tabId ) noexcept
	{
		const auto found = std::find_if(panes.begin(), panes.end(), [tabId](const TerminalPane& pane) {
			return pane.tabId == tabId;
		});
		return found == panes.end() ? nullptr : &*found;
	}

	const TerminalPane* FindPane( std::uint64_t tabId ) const noexcept
	{
		const auto found = std::find_if(panes.begin(), panes.end(), [tabId](const TerminalPane& pane) {
			return pane.tabId == tabId;
		});
		return found == panes.end() ? nullptr : &*found;
	}

	void DrainNativeFrames( const std::uint64_t tabId ) noexcept
	{
		if( closed || !nativeFrameBridge ) return;
		auto* pane = FindPane(tabId);
		if( pane == nullptr || !pane->window ) return;
		// The publisher is a latest-only mailbox. Drain exactly one immutable
		// frame per UI notification; if the worker publishes again while this
		// handler is running it posts another notification. An unbounded loop here
		// would let a continuously publishing worker monopolize the UI message
		// pump and would make terminal teardown depend on producer quiescence.
		if( auto frame = pane->window->TakeNativeFrame() ) {
			nativeFrameBridge->Submit(std::move(frame));
		}
	}

	void AttachNativeFrameBridge( TerminalPane& pane ) noexcept
	{
		if( !pane.window ) return;
		const auto tabId = pane.tabId;
		if( pane.window->FrameSurfaceSnapshot().deviceEpoch != nativeDeviceEpoch ) {
			(void)pane.window->NotifyFrameDeviceEpoch(nativeDeviceEpoch);
		}
		pane.window->SetNativeFrameBridge(nativeFrameBridge);
		if( !nativeFrameBridge ) {
			pane.window->SetNativeFrameReadySink({});
			return;
		}
		pane.window->SetNativeFrameReadySink([this, tabId] {
			DrainNativeFrames(tabId);
		});
		// A frame may have completed before the bridge was attached (for example
		// while the runtime was being initialized). Drain that mailbox now rather
		// than waiting for a message that was posted to the old sink state.
		DrainNativeFrames(tabId);
	}

	void SetNativeFrameRuntimeBridge( TerminalNativeFrameBridgePtr bridge ) noexcept
	{
		if( nativeFrameBridge && nativeFrameBridge != bridge ) DetachNativeFrameRuntime();
		nativeFrameBridge = std::move(bridge);
		for( auto& pane : panes ) AttachNativeFrameBridge(pane);
	}

	void DetachNativeFrameRuntime() noexcept
	{
		const auto bridge = std::exchange(nativeFrameBridge, {});
		for( auto& pane : panes ) {
			if( pane.window ) pane.window->SetNativeFrameReadySink({});
		}
		if( !bridge ) {
			for( auto& pane : panes ) {
				if( pane.window ) pane.window->SetNativeFrameBridge({});
			}
			return;
		}
		// CloseSurface is sent while the runtime is still accepting commands;
		// only after every stable identity is withdrawn do we atomically fence the
		// callbacks. No worker or presentation-owner wait occurs here.
		for( const auto& pane : panes ) {
			if( !pane.window ) continue;
			const auto snapshot = pane.window->FrameSurfaceSnapshot();
			bridge->CloseSurface(snapshot.surfaceId, snapshot.surfaceLifetimeEpoch);
		}
		bridge->Close();
		for( auto& pane : panes ) {
			if( pane.window ) pane.window->SetNativeFrameBridge({});
		}
	}

	void DestroyPaneRenderers() noexcept
	{
		// Destroying the focused child transfers focus to this host synchronously.
		// Do not let the host's WM_SETFOCUS re-enter LayoutChildren against a child
		// that is midway through DestroyWindow.
		const bool wasDestroying = destroyingPaneRenderers;
		destroyingPaneRenderers = true;
		for( auto& pane : panes ) {
			if( !pane.window ) continue;
			pane.window->SetNativeFrameReadySink({});
			pane.window->SetFocusSink(nullptr);
			pane.window->SetInputAdapter(nullptr);
			pane.window->SetModel(nullptr);
			pane.window->Close();
		}
		panes.clear();
		paneLayout = {};
		destroyingPaneRenderers = wasDestroying;
	}

	bool HasSynchronizedOutput() const noexcept
	{
		return std::any_of(panes.begin(), panes.end(), [](const TerminalPane& pane) {
			return pane.synchronizedOutputSince != 0;
		});
	}

	void InvalidateTerminalTabs()
	{
		if( !window ) return;
		const RECT content = ContentRect();
		if( !IsEmptyRect(content) ) ::InvalidateRect(window, &content, FALSE);
	}

	void SetTabPresentationSettings( TerminalTabPresentationSettings settings )
	{
		tabPresentationSettings = std::move(settings);
		// This is a presentation-only projection. The manager, sessions, models,
		// and PTY lifetimes are intentionally untouched.
		LayoutChildren();
		InvalidateTabs();
		InvalidateTerminalTabs();
	}

	void SetScrollbackLimit( std::size_t lines )
	{
		for( const auto& mutation : manager->SetScrollbackLimit(lines) ) {
			if( auto* pane = FindPane(mutation.tabId); pane && pane->window ) {
				pane->window->ApplyScrollbackChange(mutation.change);
			}
		}
	}

	void OnPaneFocused( std::uint64_t tabId )
	{
		terminalTabsFocused = false;
		if( manager->SelectTab(tabId) ) {
			EnsureTerminalTabVisible(tabId);
			InvalidateTabs();
			InvalidateTerminalTabs();
		}
	}

	bool CreatePaneRenderer( std::uint64_t tabId )
	{
		// A visible, not-yet-activated terminal panel still owns one native
		// viewport.  That lets the first session inherit the real grid size while
		// preserving the no-session-until-activation contract.  tabId == 0 is that
		// intentionally unbound viewport and is never a TerminalTabManager id.
		const bool unboundViewport = tabId == 0;
		if( !window || !instance || closed || (!unboundViewport && !manager->Model(tabId)) ) return false;
		auto candidate = std::make_unique<CTerminalWnd>(TerminalSurfaceIdForTab(tabId));
		if( !candidate->Create(window, instance) ) return false;
		candidate->SetInputSink([this, tabId](std::span<const std::uint8_t> bytes) {
			return tabId == 0 ? TerminalQueueInputResult::NotRunning : manager->QueueInput(tabId, bytes);
		});
		// Focus reports reach ConPTY, never the prompt: a shell that was only
		// focused is still at its first prompt (#415).
		candidate->SetReportSink([this, tabId](std::span<const std::uint8_t> bytes) {
			return tabId == 0 ? TerminalQueueInputResult::NotRunning
				: manager->QueueInput(tabId, bytes, TerminalInputSource::Protocol);
		});
		auto* const renderer = candidate.get();
		candidate->SetResizeSink([this, tabId, renderer](TerminalSize size) {
			if( tabId == 0 || !manager->ResizeTab(tabId, size) ) return;
			if( auto* model = manager->Model(tabId) ) {
				renderer->ApplyScrollbackChange(model->ConsumeScrollbackChange());
			}
		});
		candidate->SetFocusSink([this, tabId] {
			if( tabId != 0 ) OnPaneFocused(tabId);
		});
		candidate->SetPalette(palette);
		candidate->SetInputAdapter(unboundViewport ? nullptr : manager->InputAdapter(tabId));
		candidate->SetModel(unboundViewport ? nullptr : manager->Model(tabId));
		panes.push_back({ tabId, std::move(candidate), true, 0 });
		AttachNativeFrameBridge(panes.back());
		return true;
	}

	bool RebuildPaneRenderers()
	{
		DestroyPaneRenderers();
		const auto* group = groupLayout.ActiveGroup();
		if( !group ) return true;
		for( const auto tabId : group->tabIds ) {
			if( !CreatePaneRenderer(tabId) ) {
				DestroyPaneRenderers();
				return false;
			}
		}
		for( const auto& tab : manager->Snapshot() ) HandleOutput(tab.id);
		return true;
	}

	bool EnsureTerminalWindow()
	{
		if( !window || !instance || closed ) return false;
		if( !groupLayout.ActiveGroup() ) {
			if( const auto activeId = manager->ActiveTabId() ) groupLayout.EnsureGroupForTab(*activeId);
		}
		const auto* group = groupLayout.ActiveGroup();
		if( !group ) {
			if( !HasVisibleContentBounds() ) return true;
			if( panes.size() == 1 && panes.front().tabId == 0 && panes.front().window ) return true;
			DestroyPaneRenderers();
			return CreatePaneRenderer(0);
		}
		if( panes.size() == group->tabIds.size()
			&& std::equal(panes.begin(), panes.end(), group->tabIds.begin(), [](const TerminalPane& pane, std::uint64_t tabId) {
				return pane.tabId == tabId && pane.window != nullptr;
			}) ) {
			return true;
		}
		return RebuildPaneRenderers();
	}

	bool HasVisibleContentBounds() const noexcept
	{
		return bounds.right > bounds.left && bounds.bottom > bounds.top;
	}

	void PrepareProjectionDetach() noexcept
	{
		if( window ) {
			::KillTimer(window, kSynchronizedOutputTimer);
			::KillTimer(window, kProtocolInputRetryTimer);
		}
		for( auto& pane : panes ) {
			if( pane.window ) pane.window->ResetSessionInputState();
		}
		DestroyPaneRenderers();
		terminalTabsFocused = false;
		protocolInputRetryScheduled = false;
		draggingPaneDivider.reset();
	}

	WorkspaceProjectionState DetachCurrentProjection() noexcept
	{
		WorkspaceProjectionState state;
		state.groupLayout = std::move(groupLayout);
		state.workingDirectory = workingDirectory;
		state.terminalTabsFocused = terminalTabsFocused;
		state.terminalTabsFirstVisible = terminalTabsFirstVisible;
		state.renderersMaterialized = !panes.empty();
		PrepareProjectionDetach();
		state.tabs = manager->DetachTabs();
		groupLayout.Clear();
		terminalTabsFirstVisible = 0;
		return state;
	}

	[[nodiscard]] WorkspaceProjectionApplyResult ApplyWorkspaceProjection(
		WorkspaceProjectionState state,
		std::wstring nextWorkingDirectory,
		bool renderProjection,
		bool startWhenEmpty )
	{
		WorkspaceProjectionApplyResult result;
		PrepareProjectionDetach();
		groupLayout.Clear();
		terminalTabsFirstVisible = 0;
		workingDirectory = nextWorkingDirectory.empty() ? std::move(state.workingDirectory)
			: std::move(nextWorkingDirectory);
		const auto attach = manager->AttachTabs(std::move(state.tabs));
		if( !attach.Succeeded() ) {
			result.errorCode = attach.errorCode;
			result.failureOutcome = TerminalWorkspaceSwitchOutcome::AttachFailed;
			return result;
		}
		result.attachedTabCount = attach.attachedTabCount;
		groupLayout = std::move(state.groupLayout);
		terminalTabsFocused = state.terminalTabsFocused;
		terminalTabsFirstVisible = state.terminalTabsFirstVisible;

		if( startWhenEmpty && manager->TabCount() == 0 ) {
			const auto id = EnsureSessionStarted();
			if( !id ) {
				result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
				result.failureOutcome = TerminalWorkspaceSwitchOutcome::StartFailed;
				return result;
			}
			const auto tabs = manager->Snapshot();
			const auto found = std::ranges::find(tabs, *id, &TerminalTabSnapshot::id);
			if( found == tabs.end() || found->state == TerminalSessionState::Failed ) {
				result.errorCode = found == tabs.end() ? ERROR_GEN_FAILURE : found->errorCode;
				result.failureOutcome = TerminalWorkspaceSwitchOutcome::StartFailed;
				return result;
			}
			result.startedSession = true;
		} else if( renderProjection && window ) {
			if( !EnsureTerminalWindow() ) {
				result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
				result.failureOutcome = TerminalWorkspaceSwitchOutcome::AttachFailed;
				return result;
			}
			LayoutChildren();
		}

		InvalidateTabs();
		InvalidateTerminalTabs();
		result.succeeded = true;
		return result;
	}

	void StoreWorkspaceProjection( const std::wstring& key, WorkspaceProjectionState state )
	{
		if( key.empty() ) return;
		workspaceRegistry[key] = std::move(state);
	}

	TerminalSize CurrentSize() const noexcept
	{
		if( const auto focused = FocusedTabId() ) {
			if( const auto* pane = FindPane(*focused); pane && pane->window ) return pane->window->GetTerminalSize();
		}
		for( const auto& pane : panes ) {
			if( pane.window ) return pane.window->GetTerminalSize();
		}
		return { 120, 30 };
	}

	TerminalSize SizeForTab( std::uint64_t tabId ) const noexcept
	{
		if( const auto* pane = FindPane(tabId); pane && pane->window ) return pane->window->GetTerminalSize();
		return CurrentSize();
	}

	void BindPaneModels()
	{
		for( auto& pane : panes ) {
			if( !pane.window ) continue;
			pane.window->SetInputAdapter(manager->InputAdapter(pane.tabId));
			pane.window->SetModel(manager->Model(pane.tabId));
			static_cast<void>(pane.window->NotifyFrameContent());
			pane.needsFullRepaint = true;
		}
	}

	//! VS Code drops the active-terminal entry and the Split/Kill title actions
	//! once the tab list takes over (for example a second terminal group), so
	//! every header layout, paint and hit test uses this one projection.
	TerminalHeaderActionVisibility HeaderActionVisibility() const noexcept
	{
		const auto groupCount = groupLayout.GroupCount();
		const bool tabsNarrow = IsTerminalTabsNarrow();
		return TerminalHeaderActionVisibility{
			ShouldShowTerminalHeaderActiveTerminal(tabPresentationSettings, groupCount, tabsNarrow, HasTerminalSplit()),
			ShouldShowTerminalHeaderInlineActions(tabPresentationSettings, groupCount, tabsNarrow),
		};
	}

	TerminalHeaderLayout HeaderLayout() const noexcept
	{
		RECT client{};
		if( window ) ::GetClientRect(window, &client);
		return CalculateTerminalHeaderLayout(client, dpi, panelActions.renderPanelActions,
			panelActions.renderHeader ? 30 : 0, HeaderActionVisibility());
	}

	void InvalidateTabs()
	{
		if( headerHost && hostedHeaderBounds.right > hostedHeaderBounds.left
			&& hostedHeaderBounds.bottom > hostedHeaderBounds.top ) {
			// The host also owns the active Panel tab label. Title or selection
			// changes therefore invalidate the complete shared header row, not only
			// the terminal action sub-range.
			RECT header{};
			if( ::GetClientRect(headerHost, &header) ) {
				header.bottom = std::min(header.bottom, hostedHeaderBounds.bottom);
				::InvalidateRect(headerHost, &header, FALSE);
			} else {
				::InvalidateRect(headerHost, &hostedHeaderBounds, FALSE);
			}
		} else if( window && panelActions.renderHeader ) {
			const RECT header = HeaderLayout().header;
			::InvalidateRect(window, &header, FALSE);
		}
	}

	std::optional<std::uint64_t> FocusedTabId() const noexcept
	{
		const HWND focused = ::GetFocus();
		for( const auto& pane : panes ) {
			if( !pane.window ) continue;
			const HWND paneWindow = pane.window->GetHwnd();
			if( pane.tabId != 0 && (focused == paneWindow || (paneWindow && focused && ::IsChild(paneWindow, focused)) ) ) return pane.tabId;
		}
		if( const auto activeId = manager->ActiveTabId(); activeId
			&& groupLayout.FindGroup(*activeId) == groupLayout.ActiveGroupIndex() ) return activeId;
		if( const auto* group = groupLayout.ActiveGroup(); group && !group->tabIds.empty() ) return group->tabIds.front();
		return manager->ActiveTabId();
	}

	bool IsTerminalTabsNarrow() const noexcept
	{
		if( IsEmptyRect(paneLayout.tabsBounds) ) return false;
		return paneLayout.tabsBounds.right - paneLayout.tabsBounds.left <= ScaleDip(80, dpi);
	}

	TerminalTabPresentationSnapshot PresentationSnapshot( const TerminalTabSnapshot& tab ) const
	{
		return TerminalTabPresentationSnapshot(
			tab.processName,
			tab.profileLabel,
			tab.sequenceTitle,
			tab.initialWorkingDirectory);
	}

	//! Both native consumers project the same raw snapshot through the typed
	//! presentation seam; only the final surface operation differs.
	ResolvedTerminalTabPresentation TabPresentation( const TerminalTabSnapshot& tab ) const
	{
		return ResolveTerminalTabListPresentation(tabPresentationSettings, PresentationSnapshot(tab));
	}

	std::wstring TabTitle( const TerminalTabSnapshot& tab ) const
	{
		auto title = ResolveTerminalTabDropdownPresentation(
			tabPresentationSettings, PresentationSnapshot(tab)).title;
		if( title.empty() ) title = tab.profileLabel;
		return title;
	}

	//! The terminal list's row order (#415 Phase 4): groups in order with each
	//! group's leaf order (upstream terminalTabsList.ts' `instances`). A tab
	//! not yet installed in a group (transient between AddTab and its group
	//! install) keeps its creation-order place after the grouped ones.
	[[nodiscard]] std::vector<TerminalTabSnapshot> OrderedListTabs() const
	{
		const auto tabs = manager->Snapshot();
		std::unordered_map<std::uint64_t, std::size_t> indexById;
		indexById.reserve(tabs.size());
		for( std::size_t index = 0; index < tabs.size(); ++index ) {
			indexById.emplace(tabs[index].id, index);
		}
		std::vector<TerminalTabSnapshot> ordered;
		ordered.reserve(tabs.size());
		std::vector<bool> placed(tabs.size(), false);
		for( const auto tabId : groupLayout.InstancesInOrder() ) {
			const auto found = indexById.find(tabId);
			if( found == indexById.end() || placed[found->second] ) continue;
			placed[found->second] = true;
			ordered.push_back(tabs[found->second]);
		}
		for( std::size_t index = 0; index < tabs.size(); ++index ) {
			if( !placed[index] ) ordered.push_back(tabs[index]);
		}
		return ordered;
	}

	//! OrderedListTabs decorated for painting and observation: each row's
	//! `label` carries the tree-glyph prefix VS Code composes inside the
	//! label, so measurement and ellipsizing account for it automatically.
	[[nodiscard]] std::vector<TerminalListRow> TerminalListRows() const
	{
		const auto tabs = OrderedListTabs();
		std::vector<TerminalListRow> rows;
		rows.reserve(tabs.size());
		for( const auto& tab : tabs ) {
			TerminalListRow row;
			row.splitPosition = groupLayout.SplitPositionOf(tab.id);
			const auto presentation = TabPresentation(tab);
			auto title = presentation.title;
			if( title.empty() ) title = tab.profileLabel;
			if( title.empty() ) title = LS(STR_TERMINAL_FALLBACK_LABEL);
			row.label = std::wstring(TerminalSplitPrefix(row.splitPosition)) + title;
			row.description = presentation.description;
			row.tab = tab;
			rows.push_back(std::move(row));
		}
		return rows;
	}

	std::wstring ActiveTerminalTitle() const
	{
		const auto focused = FocusedTabId();
		if( !focused ) return {};
		const auto tabs = manager->Snapshot();
		const auto found = std::find_if(tabs.begin(), tabs.end(), [focused](const auto& tab) {
			return tab.id == *focused;
		});
		return found == tabs.end() ? std::wstring{} : TabTitle(*found);
	}

	std::wstring HeaderProfileLabel()
	{
		const auto tabs = manager->Snapshot();
		const auto focused = FocusedTabId();
		const auto found = std::find_if(tabs.begin(), tabs.end(), [focused](const auto& tab) {
			return focused && tab.id == *focused;
		});
		if( found != tabs.end() ) {
			// Keep the compact header in sync with the tab list. In particular, a
			// recognized Agent CLI title must remain visible when the tab list is
			// hidden by the single-terminal policy.
			if( auto title = TabTitle(*found); !title.empty() ) return title;
			if( !found->profileLabel.empty() ) return found->profileLabel;
		}
		if( profileService ) {
			if( const auto selected = profileService->SelectedProfile() ) {
				const auto stem = std::filesystem::path(selected->path).stem().wstring();
				if( !stem.empty() ) return stem;
			}
		}
		return L"pwsh";
	}

	bool IsHeaderTargetEnabled( TerminalHeaderTarget target ) const noexcept
	{
		// A hidden action is absent, never a disabled placeholder. More stays: VS
		// Code always keeps the view-title overflow menu.
		const auto visibility = HeaderActionVisibility();
		switch( target ) {
		case TerminalHeaderTarget::Profile:
			return visibility.activeTerminal;
		case TerminalHeaderTarget::Split:
		case TerminalHeaderTarget::SplitDown:
			return visibility.inlineActions;
		case TerminalHeaderTarget::Kill:
			return visibility.inlineActions && FocusedTabId().has_value();
		case TerminalHeaderTarget::Maximize:
			return panelActions.renderPanelActions && static_cast<bool>(panelActions.toggleMaximize);
		case TerminalHeaderTarget::Close:
			return panelActions.renderPanelActions && static_cast<bool>(panelActions.closePanel);
		default:
			return target != TerminalHeaderTarget::None && target != TerminalHeaderTarget::Count;
		}
	}

	void UpdateHotTarget( TerminalHeaderTarget target )
	{
		if( target == hotTarget ) return;
		hotTarget = target;
		InvalidateTabs();
	}

	RECT ContentRect() const noexcept
	{
		RECT content{};
		if( window ) ::GetClientRect(window, &content);
		content.top = panelActions.renderHeader ? HeaderLayout().header.bottom : content.top;
		return content;
	}

	std::optional<std::size_t> HitTestPaneDivider( int x, int y ) const noexcept
	{
		const POINT point{ x, y };
		for( std::size_t index = 0; index < paneLayout.paneDividers.size(); ++index ) {
			if( ::PtInRect(&paneLayout.paneDividers[index], point) != FALSE ) return index;
		}
		return std::nullopt;
	}

	void SetPaneDividerFromMouse( std::size_t dividerIndex, int x, int y )
	{
		const auto groupIndex = groupLayout.ActiveGroupIndex();
		if( !groupIndex || dividerIndex >= paneLayout.paneDividerNodes.size()
			|| dividerIndex >= paneLayout.paneDividerChildren.size()
			|| dividerIndex >= paneLayout.paneDividerFirstPanes.size()
			|| dividerIndex >= paneLayout.paneDividerSecondPanes.size() ) return;
		const auto nodeIndex = paneLayout.paneDividerNodes[dividerIndex];
		const auto childPosition = paneLayout.paneDividerChildren[dividerIndex];
		const auto orientation = groupLayout.NodeOrientation(*groupIndex, nodeIndex);
		if( !orientation ) return;
		const RECT& firstPane = paneLayout.paneDividerFirstPanes[dividerIndex];
		const RECT& secondPane = paneLayout.paneDividerSecondPanes[dividerIndex];
		const bool vertical = *orientation == TerminalPaneOrientation::Vertical;
		const LONG combined = vertical
			? std::max<LONG>(1, secondPane.bottom - firstPane.top - (secondPane.top - firstPane.bottom))
			: std::max<LONG>(1, secondPane.right - firstPane.left - (secondPane.left - firstPane.right));
		const LONG minimum = std::min<LONG>(ScaleDip(80, dpi), combined / 2);
		const LONG firstExtent = vertical
			? std::clamp<LONG>(y - firstPane.top, minimum, combined - minimum)
			: std::clamp<LONG>(x - firstPane.left, minimum, combined - minimum);
		if( !groupLayout.ApplyDividerResize(*groupIndex, nodeIndex, childPosition,
			static_cast<int>(firstExtent), static_cast<int>(combined)) ) return;
		LayoutChildren();
	}

	void LayoutChildren()
	{
		if( !window ) return;
		paneLayout = {};
		if( !EnsureTerminalWindow() ) return;
		const auto groupIndex = groupLayout.ActiveGroupIndex();
		const auto* group = groupLayout.ActiveGroup();
		const RECT content = ContentRect();
		if( !group ) {
			if( panes.size() == 1 && panes.front().window ) {
				paneLayout = CalculateTerminalPaneLayout({ content, dpi, 1, {}, false });
				panes.front().window->Layout(paneLayout.panes.front(), dpi);
			}
			return;
		}
		if( panes.empty() ) return;
		groupLayout.NormalizeGroup(*groupIndex);
		const bool showTabs = ShouldShowTerminalTabs(tabPresentationSettings,
			manager->TabCount(), groupLayout.GroupCount());
		paneLayout = CalculateTerminalPaneTreeLayout({ content, dpi,
			std::span<const TerminalPaneLayoutTreeNode>(group->nodes), group->root, showTabs,
			tabPresentationSettings.location });
		ClampTerminalTabsFirstVisible();
		const auto count = std::min(panes.size(), paneLayout.panes.size());
		for( std::size_t index = 0; index < count; ++index ) {
			if( panes[index].window ) panes[index].window->Layout(paneLayout.panes[index], dpi);
		}
	}

	void PaintHeaderTo( HDC dc, const RECT& paintBounds, unsigned int paintDpi,
		bool includePanelActions, bool drawTitle, int headerHeightDip )
	{
		if( !dc ) return;
		const unsigned int effectiveDpi = paintDpi == 0 ? kDefaultDpi : paintDpi;
		const auto layout = CalculateTerminalHeaderLayout(paintBounds, effectiveDpi,
			includePanelActions, headerHeightDip, HeaderActionVisibility());
		const int width = layout.header.right - layout.header.left;
		const int height = layout.header.bottom - layout.header.top;
		if( width <= 0 || height <= 0 ) return;
		const HDC memory = ::CreateCompatibleDC(dc);
		const HBITMAP bitmap = memory ? ::CreateCompatibleBitmap(dc, width, height) : nullptr;
		if( !memory || !bitmap ) {
			if( bitmap ) ::DeleteObject(bitmap);
			if( memory ) ::DeleteDC(memory);
			return;
		}
		const auto previousBitmap = ::SelectObject(memory, bitmap);
		RECT localHeader{ 0, 0, width, height };
		FillSolidRect(memory, localHeader, (drawTitle ? palette.canvas : palette.bottomPanel).ToColorRef());
		::SetBkMode(memory, TRANSPARENT);
		::SetTextColor(memory, palette.primaryText.ToColorRef());

		if( chromeFont.Get() == nullptr || chromeFont.Dpi() != effectiveDpi ) {
			static_cast<void>(chromeFont.Recreate(theme::ThemeFontKind::Chrome, effectiveDpi));
		}
		const auto previousFont = chromeFont.Get() ? ::SelectObject(memory, chromeFont.Get()) : nullptr;

		if( drawTitle ) {
			RECT title = layout.title;
			::OffsetRect(&title, -layout.header.left, -layout.header.top);
			::DrawTextW(memory, LS(STR_TERMINAL_TITLE), -1, &title,
				DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			RECT underline = layout.underline;
			::OffsetRect(&underline, -layout.header.left, -layout.header.top);
			FillSolidRect(memory, underline, palette.accent.ToColorRef());
		}

		constexpr std::array actionTargets{
			TerminalHeaderTarget::Profile,
			TerminalHeaderTarget::New,
			TerminalHeaderTarget::Dropdown,
			TerminalHeaderTarget::Split,
			TerminalHeaderTarget::SplitDown,
			TerminalHeaderTarget::Kill,
			TerminalHeaderTarget::More,
			TerminalHeaderTarget::Maximize,
			TerminalHeaderTarget::Close,
		};
		const bool maximized = panelActions.isMaximized && panelActions.isMaximized();
		for( const auto target : actionTargets ) {
			RECT targetRect = layout.RectFor(target);
			::OffsetRect(&targetRect, -layout.header.left, -layout.header.top);
			if( IsEmptyRect(targetRect) ) continue;
			const bool enabled = IsHeaderTargetEnabled(target);
			const bool highlighted = enabled && (target == hotTarget || target == pressedTarget);
			if( highlighted ) FillSolidRect(memory, targetRect, palette.raised.ToColorRef());

			COLORREF iconColor = enabled ? palette.secondaryText.ToColorRef() : palette.border.ToColorRef();
			if( highlighted ) {
				iconColor = target == TerminalHeaderTarget::Kill
					? palette.danger.ToColorRef()
					: palette.primaryText.ToColorRef();
			}
			if( target == TerminalHeaderTarget::Profile ) {
				RECT iconRect = targetRect;
				iconRect.right = std::min(iconRect.right, iconRect.left + ScaleDip(23, effectiveDpi));
				DrawHeaderIcon(memory, iconRect, target, maximized, iconColor, effectiveDpi);
				RECT labelRect = targetRect;
				labelRect.left = iconRect.right + ScaleDip(2, effectiveDpi);
				labelRect.right -= ScaleDip(2, effectiveDpi);
				::SetTextColor(memory, enabled ? palette.primaryText.ToColorRef() : palette.secondaryText.ToColorRef());
				const auto label = HeaderProfileLabel();
				::DrawTextW(memory, label.c_str(), -1, &labelRect,
					DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			} else {
				DrawHeaderIcon(memory, targetRect, target, maximized, iconColor, effectiveDpi);
			}
		}

		if( previousFont ) ::SelectObject(memory, previousFont);
		::BitBlt(dc, layout.header.left, layout.header.top, width, height, memory, 0, 0, SRCCOPY);
		::SelectObject(memory, previousBitmap);
		::DeleteObject(bitmap);
		::DeleteDC(memory);
	}

	void Paint()
	{
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if( !dc ) return;
		RECT client{};
		::GetClientRect(window, &client);
		FillSolidRect(dc, paint.rcPaint, palette.canvas.ToColorRef());
		if( panelActions.renderHeader ) {
			PaintHeaderTo(dc, client, dpi, panelActions.renderPanelActions, true, 30);
		}
		for( const auto& divider : paneLayout.paneDividers ) {
			FillSolidRect(dc, divider, palette.border.ToColorRef());
		}
		if( !IsEmptyRect(paneLayout.tabsDivider) ) FillSolidRect(dc, paneLayout.tabsDivider, palette.border.ToColorRef());
		PaintTerminalTabs(dc);
		::EndPaint(window, &paint);
	}

	std::size_t TerminalTabCapacity() const noexcept
	{
		if( IsEmptyRect(paneLayout.tabsBounds) ) return 0;
		const LONG rowHeight = std::max(1, ScaleDip(kTerminalTabsRowHeightDip, dpi));
		return static_cast<std::size_t>(std::max<LONG>(0,
			(paneLayout.tabsBounds.bottom - paneLayout.tabsBounds.top) / rowHeight));
	}

	void ClampTerminalTabsFirstVisible() noexcept
	{
		const auto count = manager->TabCount();
		const auto capacity = TerminalTabCapacity();
		if( count == 0 || capacity == 0 ) {
			terminalTabsFirstVisible = 0;
			return;
		}
		terminalTabsFirstVisible = std::min(terminalTabsFirstVisible, count - std::min(count, capacity));
	}

	void EnsureTerminalTabVisible( std::uint64_t tabId ) noexcept
	{
		const auto tabs = OrderedListTabs();
		const auto found = std::find_if(tabs.begin(), tabs.end(), [tabId](const TerminalTabSnapshot& tab) {
			return tab.id == tabId;
		});
		const auto capacity = TerminalTabCapacity();
		if( found == tabs.end() || capacity == 0 ) {
			ClampTerminalTabsFirstVisible();
			return;
		}
		const auto index = static_cast<std::size_t>(found - tabs.begin());
		if( index < terminalTabsFirstVisible ) {
			terminalTabsFirstVisible = index;
		} else if( index >= terminalTabsFirstVisible + capacity ) {
			terminalTabsFirstVisible = index + 1 - capacity;
		}
		ClampTerminalTabsFirstVisible();
	}

	void ScrollTerminalTabs( int wheelDelta )
	{
		const auto capacity = TerminalTabCapacity();
		const auto count = manager->TabCount();
		if( capacity == 0 || count <= capacity || wheelDelta == 0 ) return;
		const int rows = wheelDelta / WHEEL_DELTA;
		if( rows == 0 ) return;
		const auto maximum = count - capacity;
		const auto next = static_cast<long long>(terminalTabsFirstVisible) - rows;
		terminalTabsFirstVisible = static_cast<std::size_t>(std::clamp<long long>(next, 0,
			static_cast<long long>(maximum)));
		InvalidateTerminalTabs();
	}

	RECT TerminalTabRow( std::size_t index ) const noexcept
	{
		RECT row = paneLayout.tabsBounds;
		if( IsEmptyRect(row) || index < terminalTabsFirstVisible ) return {};
		const LONG rowHeight = std::max(1, ScaleDip(kTerminalTabsRowHeightDip, dpi));
		const auto top = static_cast<long long>(row.top)
			+ static_cast<long long>(index - terminalTabsFirstVisible) * rowHeight;
		if( top >= row.bottom ) return {};
		row.top = static_cast<LONG>(top);
		row.bottom = std::min<LONG>(row.bottom, row.top + rowHeight);
		return row;
	}

	std::optional<std::uint64_t> TerminalTabAtPoint( const POINT& point ) const
	{
		if( ::PtInRect(&paneLayout.tabsBounds, point) == FALSE ) return std::nullopt;
		const auto tabs = OrderedListTabs();
		for( std::size_t index = 0; index < tabs.size(); ++index ) {
			const RECT row = TerminalTabRow(index);
			if( ::PtInRect(&row, point) != FALSE ) return tabs[index].id;
		}
		return std::nullopt;
	}

	void PaintTerminalTabs( HDC dc )
	{
		if( !dc || IsEmptyRect(paneLayout.tabsBounds) ) return;
		FillSolidRect(dc, paneLayout.tabsBounds, palette.bottomPanel.ToColorRef());
		if( chromeFont.Get() == nullptr || chromeFont.Dpi() != dpi ) {
			static_cast<void>(chromeFont.Recreate(theme::ThemeFontKind::Chrome, dpi));
		}
		const auto previousFont = chromeFont.Get() ? ::SelectObject(dc, chromeFont.Get()) : nullptr;
		::SetBkMode(dc, TRANSPARENT);
		const auto rows = TerminalListRows();
		const auto first = std::min(terminalTabsFirstVisible, rows.size());
		const auto focused = FocusedTabId();
		const int markerWidth = std::max(2, ScaleDip(3, dpi));
		const int horizontalPadding = ScaleDip(8, dpi);
		for( std::size_t index = first; index < rows.size(); ++index ) {
			const auto& row = rows[index];
			const RECT rowRect = TerminalTabRow(index);
			if( IsEmptyRect(rowRect) ) break;
			const bool selected = focused && row.tab.id == *focused;
			if( selected ) FillSolidRect(dc, rowRect, palette.raised.ToColorRef());

			RECT marker = rowRect;
			marker.right = std::min<LONG>(rowRect.right, marker.left + markerWidth);
			const bool split = row.splitPosition != TerminalSplitPosition::None;
			FillSolidRect(dc, marker, (selected ? palette.accent : (split ? palette.secondaryText : palette.border)).ToColorRef());

			RECT label = rowRect;
			label.left = std::min<LONG>(label.right, label.left + horizontalPadding);
			label.right = std::max<LONG>(label.left, label.right - horizontalPadding);
			const TerminalTabRowLayoutInput geometryInput {
				{ static_cast<int>(label.left), static_cast<int>(label.top),
					static_cast<int>(label.right), static_cast<int>(label.bottom) }, dpi, split, true,
				!row.description.empty(), row.tab.state == TerminalSessionState::Failed };
			const auto geometry = CalculateTerminalTabRowLayout(geometryInput);
			const auto toNativeRect = [](const TerminalTabPresentationRect& rect) {
				return RECT { rect.left, rect.top, rect.right, rect.bottom };
			};
			if( geometry.Icon().Width() > 0 ) {
				DrawTerminalTabIcon(dc, toNativeRect(geometry.Icon()),
					(selected ? palette.primaryText : palette.secondaryText).ToColorRef());
			}
			if( geometry.Title().Width() > 0 ) {
				RECT titleRect = toNativeRect(geometry.Title());
				::SetTextColor(dc, row.tab.state == TerminalSessionState::Failed
					? palette.danger.ToColorRef()
					: (selected ? palette.primaryText : palette.secondaryText).ToColorRef());
				::DrawTextW(dc, row.label.c_str(), -1, &titleRect,
					DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			}
			if( geometry.Description().Width() > 0 && !row.description.empty() ) {
				RECT descriptionRect = toNativeRect(geometry.Description());
				::SetTextColor(dc, palette.descriptionText.ToColorRef());
				::DrawTextW(dc, row.description.c_str(), -1, &descriptionRect,
					DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			}
			if( geometry.Status().Width() > 0 && row.tab.state == TerminalSessionState::Failed ) {
				RECT statusRect = toNativeRect(geometry.Status());
				::SetTextColor(dc, palette.danger.ToColorRef());
				::DrawTextW(dc, L"!", 1, &statusRect,
					DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
			}
		}
		if( previousFont ) ::SelectObject(dc, previousFont);
	}

	void PaintTerminalOutput( CTerminalWnd& renderer, const TerminalModel* model, const TerminalDrainResult& result,
		bool& needsFullRepaint, ULONGLONG& synchronizedSince )
	{
		const bool synchronized = model && model->Modes().synchronizedOutput;
		renderer.ApplyScrollbackChange(result.scrollbackChange,
			!synchronized || result.synchronizedOutputCommitted);
		// DrainOutput has already consumed one bounded model publication. Keep
		// terminal bytes and dirty-row damage in their existing paths; only the
		// frame projection is latest-only until the enclosing post-paint boundary.
		static_cast<void>(renderer.NotifyFrameContent());
		if( result.synchronizedOutputCommitted ) {
			// A single drain may close one synchronized frame and immediately open
			// another. Paint the completed boundary even though the final mode is on.
			renderer.InvalidateAll();
			needsFullRepaint = false;
			if( synchronized ) synchronizedSince = ::GetTickCount64();
		}
		if( synchronized ) {
			if( synchronizedSince == 0 ) {
				synchronizedSince = ::GetTickCount64();
				// SetTimer restarts an existing timer, so arm it only on entry. Re-arming
				// every 16 ms output drain can postpone the fallback forever.
				if( window ) ::SetTimer(window, kSynchronizedOutputTimer,
					static_cast<UINT>(kSynchronizedOutputMaximumMilliseconds), nullptr);
			} else if( result.synchronizedOutputCommitted && window ) {
				// A committed frame starts a distinct bounded synchronization window.
				::SetTimer(window, kSynchronizedOutputTimer,
					static_cast<UINT>(kSynchronizedOutputMaximumMilliseconds), nullptr);
			}
			return;
		}
		if( synchronizedSince != 0 ) {
			synchronizedSince = 0;
			renderer.InvalidateAll();
			needsFullRepaint = false;
			if( !HasSynchronizedOutput() && window ) {
				::KillTimer(window, kSynchronizedOutputTimer);
			}
			return;
		}
		if( result.synchronizedOutputCommitted ) return;
		if( needsFullRepaint ) {
			renderer.InvalidateAll();
			needsFullRepaint = false;
		} else {
			renderer.InvalidateDirtyRows(result.dirtyRows);
		}
	}

	void HandleOutput( std::uint64_t tabId )
	{
		const auto result = manager->DrainOutput(tabId);
		if( !result.found ) return;
		if( result.protocolInputPending ) ScheduleProtocolInputRetry();
		if( result.sequenceChanged ) {
			InvalidateTabs();
			InvalidateTerminalTabs();
			OnAgentTitleChanged(tabId);
		}
		// OSC 52 write from the application (for example a TUI's own selection
		// copy). Best effort: a busy clipboard drops it, as in xterm.js.
		if( result.clipboardWrite ) WriteTerminalClipboardText(window, *result.clipboardWrite);
		if( auto* pane = FindPane(tabId); pane && pane->window ) {
			PaintTerminalOutput(*pane->window, manager->Model(tabId), result,
				pane->needsFullRepaint, pane->synchronizedOutputSince);
			manager->RecordViewportDiagnostic(tabId, pane->window->GetViewportDiagnostic());
		}
	}

	void ScheduleProtocolInputRetry()
	{
		if( protocolInputRetryScheduled || !window ) return;
		protocolInputRetryScheduled = ::SetTimer(window, kProtocolInputRetryTimer, kProtocolInputRetryMilliseconds, nullptr) != 0;
	}

	void RetryPendingProtocolInput()
	{
		bool pending = false;
		for( const auto& tab : manager->Snapshot() ) {
			if( !manager->HasPendingProtocolInput(tab.id) ) continue;
			const auto result = manager->FlushPendingProtocolInput(tab.id);
			pending = pending || result == TerminalQueueInputResult::QueueFull || manager->HasPendingProtocolInput(tab.id);
		}
		if( !pending && window ) {
			::KillTimer(window, kProtocolInputRetryTimer);
			protocolInputRetryScheduled = false;
		}
	}

	void ScheduleOutputFrame( std::uint64_t tabId )
	{
		if( window == nullptr ) return;
		// NotificationGate keeps at most one queued wake per tab. Consume that wake
		// before entering here, drain the latest available state once, and let output
		// arriving during the drain post the next wake. This is event driven and does
		// not impose a 16 ms application timer or hold a short echo for a later tick.
		HandleOutput(tabId);
	}

	void ConsumeJobWake( std::uint64_t tabId )
	{
		const std::lock_guard lock(gate->mutex);
		gate->jobWakePending.erase(tabId);
	}

	void NotifyAgentPresence() noexcept
	{
		if( !agentPresenceSink ) return;
		try {
			const auto sink = agentPresenceSink;
			sink();
		} catch( ... ) {
			// The Projects projection is advisory; it must not unwind the panel.
		}
	}

	//! Hands the observed agent PIDs to the worker-side hint filter.
	void PublishAgentProcessIds( std::uint64_t tabId )
	{
		auto ids = agentPresence.AgentProcessIds(tabId);
		const std::lock_guard lock(gate->mutex);
		if( ids.empty() ) gate->agentProcessIds.erase(tabId);
		else gate->agentProcessIds[tabId] = std::move(ids);
	}

	bool ObserveAgentProcesses( std::uint64_t tabId )
	{
		const bool changed = agentPresence.ApplyJobObservation(tabId, manager->ObserveJobProcesses(tabId));
		PublishAgentProcessIds(tabId);
		return changed;
	}

	//! Brings presence in line with the current tab set. `observe` re-reads every
	//! job; it is used for session and tab-set changes and explicit refreshes,
	//! each bounded by the tab count, never on a timer.
	bool ReconcileAgentPresence( bool observe )
	{
		const auto tabs = manager->Snapshot();
		std::vector<std::uint64_t> live;
		live.reserve(tabs.size());
		for( const auto& tab : tabs ) live.push_back(tab.id);
		bool changed = agentPresence.Retain(live);
		{
			const std::lock_guard lock(gate->mutex);
			std::erase_if(gate->agentProcessIds, [&live](const auto& entry) {
				return std::find(live.begin(), live.end(), entry.first) == live.end();
			});
		}
		for( const auto& tab : tabs ) {
			if( agentPresence.ApplyTitle(tab.id, tab.sequenceTitle) ) changed = true;
			if( observe && ObserveAgentProcesses(tab.id) ) changed = true;
		}
		return changed;
	}

	void OnAgentTitleChanged( std::uint64_t tabId )
	{
		const auto tabs = manager->Snapshot();
		const auto found = std::find_if(tabs.begin(), tabs.end(), [tabId](const TerminalTabSnapshot& tab) { return tab.id == tabId; });
		if( found != tabs.end() && agentPresence.ApplyTitle(tabId, found->sequenceTitle) ) NotifyAgentPresence();
	}

	void ConsumeOutputWake( std::uint64_t tabId )
	{
		const std::lock_guard lock(gate->mutex);
		gate->outputWakePending.erase(tabId);
	}

	void ConsumeStateWake()
	{
		const std::lock_guard lock(gate->mutex);
		gate->stateWakePending = false;
	}

	void ConsumeTopologyWake()
	{
		const std::lock_guard lock(gate->mutex);
		gate->topologyWakePending = false;
	}

	bool PaneHasKeyboardFocus() const noexcept
	{
		const HWND focused = ::GetFocus();
		if( !focused ) return false;
		return std::any_of(panes.begin(), panes.end(), [focused](const TerminalPane& pane) {
			const HWND paneWindow = pane.window ? pane.window->GetHwnd() : nullptr;
			return paneWindow && (focused == paneWindow || ::IsChild(paneWindow, focused));
		});
	}

	//! Applies runtime topology changes made outside this panel (#403). A tmux
	//! window is a terminal group here (the preset digit chords already say so),
	//! so `new-window` opens a group and `split-window` splits the pane it came
	//! from, on the runtime split's axis. `kill-pane`/`kill-window` drop their
	//! tabs; `select-window`/`select-pane` and a non-detached create move the
	//! selection. Keyboard focus follows only when it is already in a pane,
	//! so an agent working in the background never takes it from the editor.
	void ReconcileTopology()
	{
		if( closed ) return;
		const auto activeBefore = manager->ActiveTabId();
		std::vector<std::uint64_t> remainingInGroup;
		std::size_t removedIndex = 0;
		if( activeBefore ) {
			if( const auto owner = groupLayout.FindGroup(*activeBefore) ) {
				remainingInGroup = groupLayout.GroupAt(*owner)->tabIds;
				const auto found = std::find(remainingInGroup.begin(), remainingInGroup.end(), *activeBefore);
				removedIndex = static_cast<std::size_t>(found - remainingInGroup.begin());
			}
		}
		const auto change = manager->ReconcileTopology();
		if( !change.Changed() ) return;
		const bool hadFocus = PaneHasKeyboardFocus();
		DestroyPaneRenderers();
		for( const auto tabId : change.removed ) static_cast<void>(groupLayout.RemoveTab(tabId));
		for( const auto& adoption : change.adopted ) {
			if( adoption.splitOf ) {
				if( const auto group = groupLayout.FindGroup(*adoption.splitOf);
					group && groupLayout.InsertPaneSplit(*group, *adoption.splitOf, adoption.tabId, adoption.orientation) ) {
					continue;
				}
			}
			groupLayout.AppendGroup(adoption.tabId);
		}

		// Same successor rule as DeleteTerminal (VS Code TerminalGroup): the
		// instance now at the removed one's index in its group, else the group
		// TerminalGroupLayout::RemoveTab activated.
		std::optional<std::uint64_t> target = change.selected;
		const bool activeRemoved = activeBefore
			&& std::find(change.removed.begin(), change.removed.end(), *activeBefore) != change.removed.end();
		if( !target && activeRemoved ) {
			std::erase_if(remainingInGroup, [&change](const std::uint64_t id) {
				return std::find(change.removed.begin(), change.removed.end(), id) != change.removed.end();
			});
			if( !remainingInGroup.empty() ) {
				target = remainingInGroup[(std::min)(removedIndex, remainingInGroup.size() - 1)];
			} else if( const auto* activeGroup = groupLayout.ActiveGroup();
				activeGroup && !activeGroup->tabIds.empty() ) {
				target = activeGroup->tabIds.front();
			}
		}
		if( target && groupLayout.FindGroup(*target) ) static_cast<void>(manager->SelectTab(*target));
		if( const auto activeId = manager->ActiveTabId() ) {
			if( const auto group = groupLayout.FindGroup(*activeId) ) groupLayout.SetActiveGroup(group);
		}
		groupLayout.NormalizeActiveGroup();
		if( window ) {
			static_cast<void>(RebuildPaneRenderers());
			LayoutChildren();
		}
		if( const auto activeId = manager->ActiveTabId() ) {
			EnsureTerminalTabVisible(*activeId);
			if( hadFocus ) {
				terminalTabsFocused = false;
				if( auto* pane = FindPane(*activeId); pane && pane->window ) pane->window->Focus();
			}
		}
		for( const auto& adoption : change.adopted ) HandleOutput(adoption.tabId);
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( ReconcileAgentPresence(false) ) NotifyAgentPresence();
	}

	void ShowContextMenu( int x, int y, HWND owner = nullptr, bool terminalContext = true )
	{
		const HMENU menu = ::CreatePopupMenu();
		if( !menu ) return;
		const HWND menuOwner = owner ? owner : window;
		const auto focusedId = FocusedTabId();
		POINT clientPoint{ x, y };
		if( window ) ::ScreenToClient(window, &clientPoint);
		const auto rowId = window ? TerminalTabAtPoint(clientPoint) : std::nullopt;
		const auto contextId = rowId ? rowId : focusedId;
		const auto tabs = OrderedListTabs();
		const HMENU sessionsMenu = ::CreatePopupMenu();
		tabCommandIds.clear();
		if( sessionsMenu ) {
			for( const auto& tab : tabs ) {
				const UINT command = kCommandTabFirst + static_cast<UINT>(tabCommandIds.size());
				UINT flags = MF_STRING;
				if( focusedId && tab.id == *focusedId ) flags |= MF_CHECKED;
				std::wstring label = TabTitle(tab);
				if( label.empty() ) label = LS(STR_TERMINAL_FALLBACK_LABEL);
				::AppendMenuW(sessionsMenu, flags, command, label.c_str());
				tabCommandIds.push_back(tab.id);
			}
			if( tabCommandIds.empty() ) {
				::AppendMenuW(sessionsMenu, MF_STRING | MF_GRAYED, kCommandTabFirst, LS(STR_TERMINAL_NO_SESSIONS));
			}
			::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sessionsMenu), LS(STR_TERMINAL_SESSIONS));
			::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		}
		::AppendMenuW(menu, MF_STRING, kCommandNewTerminal, LS(STR_TERMINAL_NEW));
		if( panelActions.runCommand ) {
			::AppendMenuW(menu, MF_STRING, kCommandRunTask, LS(STR_TERMINAL_RUN_TASK));
			::AppendMenuW(menu, MF_STRING, kCommandRunBuildTask, LS(STR_TERMINAL_RUN_BUILD_TASK));
		}
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandRestartTerminal, LS(STR_TERMINAL_RESTART));
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandCloseTerminal, LS(STR_TERMINAL_CLOSE));
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandSplitTerminal, LS(STR_TERMINAL_SPLIT));
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandSplitTerminalDown, LS(STR_TERMINAL_SPLIT_DOWN));
		::AppendMenuW(menu, MF_STRING | (HasTerminalSplit() ? 0 : MF_GRAYED), kCommandCloseSplit, LS(STR_TERMINAL_CLOSE_SPLIT));
		if( terminalContext && contextId && groupLayout.SplitPositionOf(*contextId) != TerminalSplitPosition::None ) {
			::AppendMenuW(menu, MF_STRING, kCommandUnsplitTerminal, LS(STR_TERMINAL_UNSPLIT));
		}
		if( const HMENU shortcutsMenu = ::CreatePopupMenu() ) {
			for( std::size_t index = 0; index < kShortcutPresets.size(); ++index ) {
				const auto preset = kShortcutPresets[index];
				const UINT flags = MF_STRING | (preset == shortcutPreset ? MF_CHECKED : 0);
				::AppendMenuW(shortcutsMenu, flags,
					kCommandShortcutPresetFirst + static_cast<UINT>(index),
					LS(ShortcutPresetLabelId(preset)));
			}
			::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcutsMenu), LS(STR_TERMINAL_SHORTCUTS));
		}
		if( profileService ) {
			const HMENU profilesMenu = ::CreatePopupMenu();
			if( profilesMenu ) {
				profileCommandPaths.clear();
				const auto selected = profileService->SelectedProfile();
				for( const auto& profile : profileService->Profiles() ) {
					const UINT command = kCommandProfileFirst + static_cast<UINT>(profileCommandPaths.size());
					const bool checked = selected && _wcsicmp(selected->path.c_str(), profile.path.c_str()) == 0;
					::AppendMenuW(profilesMenu, MF_STRING | (checked ? MF_CHECKED : 0), command,
						FormatProfileLabel(profile).c_str());
					profileCommandPaths.push_back(profile.path);
				}
				if( profileCommandPaths.empty() ) {
					::AppendMenuW(profilesMenu, MF_STRING | MF_GRAYED, kCommandProfileFirst, LS(STR_TERMINAL_NO_POWERSHELL_PROFILE));
				}
				::AppendMenuW(profilesMenu, MF_SEPARATOR, 0, nullptr);
				::AppendMenuW(profilesMenu, MF_STRING, kCommandRedetectPowerShell, LS(STR_TERMINAL_REDETECT_POWERSHELL));
				::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profilesMenu), LS(STR_TERMINAL_POWERSHELL_PROFILE));
			}
		}
		// The owner may be the Panel's header host rather than this tool window, and
		// that window's WM_COMMAND handler does not know these IDs. Take the chosen
		// ID back synchronously and dispatch it here so every owner reaches the same
		// terminal commands.
		const UINT command = static_cast<UINT>(::TrackPopupMenu(menu,
			TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, menuOwner, nullptr));
		::DestroyMenu(menu);
		if( command != 0 ) ExecuteContextMenuCommand(command, contextId);
	}

	void ExecuteContextMenuCommand( UINT command, std::optional<std::uint64_t> contextId )
	{
		if( closed ) return;
		if( command >= kCommandProfileFirst && command < kCommandProfileFirst + profileCommandPaths.size() ) {
			if( profileService ) {
				const auto index = static_cast<std::size_t>(command - kCommandProfileFirst);
				(void)profileService->SelectProfile(profileCommandPaths[index]);
			}
			return;
		}
		if( command >= kCommandShortcutPresetFirst
			&& command < kCommandShortcutPresetFirst + kShortcutPresets.size() ) {
			const auto index = static_cast<std::size_t>(command - kCommandShortcutPresetFirst);
			ApplyShortcutPreset(kShortcutPresets[index], true);
			return;
		}
		if( command >= kCommandTabFirst && command < kCommandTabFirst + tabCommandIds.size() ) {
			const auto index = static_cast<std::size_t>(command - kCommandTabFirst);
			static_cast<void>(SelectTerminal(tabCommandIds[index]));
			return;
		}
		switch( command ) {
		case kCommandNewTerminal: static_cast<void>(AddTerminal()); break;
		case kCommandRunTask: if( panelActions.runCommand ) panelActions.runCommand("workbench.action.tasks.runTask"); break;
		case kCommandRunBuildTask: if( panelActions.runCommand ) panelActions.runCommand("workbench.action.tasks.build"); break;
		case kCommandRestartTerminal: if( const auto id = FocusedTabId() ) RestartTerminal(*id); break;
		case kCommandCloseTerminal: if( const auto id = FocusedTabId() ) DeleteTerminal(*id); break;
		case kCommandRedetectPowerShell: if( profileService ) profileService->Redetect(); break;
		case kCommandSplitTerminal: static_cast<void>(SplitTerminalRight()); break;
		case kCommandSplitTerminalDown: static_cast<void>(SplitTerminalDown()); break;
		case kCommandCloseSplit: CloseTerminalSplit(); break;
		case kCommandUnsplitTerminal: if( contextId ) static_cast<void>(UnsplitTerminal(contextId)); break;
		default: break;
		}
	}

	void ShowHeaderMenu( TerminalHeaderTarget target, HWND owner = nullptr,
		const RECT* externalBounds = nullptr, unsigned int externalDpi = 0 )
	{
		const HWND source = owner ? owner : window;
		if( !source ) return;
		const TerminalHeaderLayout layout = externalBounds
			? CalculateTerminalHeaderLayout(*externalBounds, externalDpi, false, 34, HeaderActionVisibility())
			: HeaderLayout();
		const RECT action = layout.RectFor(target);
		POINT point{ action.left, action.bottom };
		::ClientToScreen(source, &point);
		ShowContextMenu(point.x, point.y, source, false);
	}

	void ExecuteHeaderTarget( TerminalHeaderTarget target, HWND owner = nullptr,
		const RECT* externalBounds = nullptr, unsigned int externalDpi = 0 )
	{
		if( !IsHeaderTargetEnabled(target) ) return;
		switch( target ) {
		case TerminalHeaderTarget::Profile:
		case TerminalHeaderTarget::Dropdown:
		case TerminalHeaderTarget::More:
			ShowHeaderMenu(target, owner, externalBounds, externalDpi);
			break;
		case TerminalHeaderTarget::New:
			static_cast<void>(AddTerminal());
			break;
		case TerminalHeaderTarget::Split:
			// Match the editor Split action: Alt chooses the orthogonal orientation.
			if( (::GetKeyState(VK_MENU) & 0x8000) != 0 ) static_cast<void>(SplitTerminalDown());
			else static_cast<void>(SplitTerminalRight());
			break;
		case TerminalHeaderTarget::SplitDown:
			// Fork extension: a visible stacked split. Alt mirrors Split's
			// orthogonal choice.
			if( (::GetKeyState(VK_MENU) & 0x8000) != 0 ) static_cast<void>(SplitTerminalRight());
			else static_cast<void>(SplitTerminalDown());
			break;
		case TerminalHeaderTarget::Kill:
			if( const auto id = FocusedTabId() ) static_cast<void>(DeleteTerminal(*id));
			break;
		case TerminalHeaderTarget::Maximize:
			panelActions.toggleMaximize();
			InvalidateTabs();
			break;
		case TerminalHeaderTarget::Close:
			panelActions.closePanel();
			break;
		default:
			break;
		}
	}

	bool HandleHostedHeaderMessage( UINT message, WPARAM wParam, LPARAM lParam,
		const RECT& headerBounds, unsigned int headerDpi )
	{
		static_cast<void>(wParam);
		if( !headerHost || headerBounds.right <= headerBounds.left || headerBounds.bottom <= headerBounds.top ) return false;
		hostedHeaderBounds = headerBounds;
		hostedHeaderDpi = headerDpi == 0 ? kDefaultDpi : headerDpi;
		const auto layout = CalculateTerminalHeaderLayout(headerBounds, hostedHeaderDpi, false, 34,
			HeaderActionVisibility());
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const bool inHeader = ::PtInRect(&layout.header, point) != FALSE;

		switch( message ) {
		case WM_LBUTTONDOWN: {
			if( !inHeader ) return false;
			const auto target = layout.HitTest(point);
			if( IsHeaderTargetEnabled(target) ) {
				pressedTarget = target;
				UpdateHotTarget(target);
				::SetCapture(headerHost);
				InvalidateTabs();
			} else if( const auto id = FocusedTabId() ) {
				static_cast<void>(SelectTerminal(*id, true));
			}
			return true;
		}
		case WM_MOUSEMOVE: {
			if( !inHeader && pressedTarget == TerminalHeaderTarget::None ) return false;
			auto target = layout.HitTest(point);
			if( !IsHeaderTargetEnabled(target) ) target = TerminalHeaderTarget::None;
			UpdateHotTarget(target);
			if( inHeader && !trackingMouseLeave ) {
				TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, headerHost, 0 };
				trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
			}
			return true;
		}
		case WM_LBUTTONUP:
			if( pressedTarget != TerminalHeaderTarget::None ) {
				const auto pressed = pressedTarget;
				pressedTarget = TerminalHeaderTarget::None;
				const auto released = layout.HitTest(point);
				if( ::GetCapture() == headerHost ) ::ReleaseCapture();
				if( released == pressed ) ExecuteHeaderTarget(pressed, headerHost, &headerBounds, hostedHeaderDpi);
				InvalidateTabs();
				return true;
			}
			return inHeader;
		case WM_MOUSELEAVE:
			trackingMouseLeave = false;
			UpdateHotTarget(TerminalHeaderTarget::None);
			return true;
		case WM_CAPTURECHANGED:
			if( pressedTarget != TerminalHeaderTarget::None ) {
				pressedTarget = TerminalHeaderTarget::None;
				InvalidateTabs();
				return true;
			}
			return false;
		case WM_SETCURSOR: {
			POINT cursor{};
			::GetCursorPos(&cursor);
			::ScreenToClient(headerHost, &cursor);
			if( ::PtInRect(&layout.header, cursor)
				&& IsHeaderTargetEnabled(layout.HitTest(cursor)) ) {
				::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
				return true;
			}
			return false;
		}
		case WM_RBUTTONUP:
			if( !inHeader ) return false;
			{
				POINT screenPoint = point;
				::ClientToScreen(headerHost, &screenPoint);
				ShowContextMenu(screenPoint.x, screenPoint.y, headerHost, false);
			}
			return true;
		default:
			return false;
		}
	}

	LRESULT HandleMessage( UINT message, WPARAM wParam, LPARAM lParam )
	{
		switch( message ) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
			Paint();
			return 0;
		case WM_SIZE:
			LayoutChildren();
			return 0;
		case WM_SETFOCUS:
			if( !terminalTabsFocused && !destroyingPaneRenderers ) {
				if( const auto id = FocusedTabId() ) static_cast<void>(SelectTerminal(*id, true));
			}
			return 0;
		case WM_LBUTTONDOWN: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if( const auto tabId = TerminalTabAtPoint(point) ) {
				terminalTabsFocused = true;
				static_cast<void>(SelectTerminal(*tabId, false));
				::SetFocus(window);
				return 0;
			}
			if( const auto divider = HitTestPaneDivider(point.x, point.y) ) {
				draggingPaneDivider = divider;
				::SetCapture(window);
				return 0;
			}
			const auto layout = HeaderLayout();
			if( point.y >= layout.header.top && point.y < layout.header.bottom ) {
				const auto target = layout.HitTest(point);
				if( IsHeaderTargetEnabled(target) ) {
					pressedTarget = target;
					UpdateHotTarget(target);
					::SetCapture(window);
					InvalidateTabs();
				} else if( const auto id = FocusedTabId() ) {
					static_cast<void>(SelectTerminal(*id, true));
				}
			}
			return 0;
		}
		case WM_LBUTTONDBLCLK: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if( const auto tabId = TerminalTabAtPoint(point) ) {
				terminalTabsFocused = false;
				static_cast<void>(SelectTerminal(*tabId, true));
				return 0;
			}
			break;
		}
		case WM_MOUSEMOVE: {
			if( draggingPaneDivider ) {
				SetPaneDividerFromMouse(*draggingPaneDivider, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				return 0;
			}
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const auto layout = HeaderLayout();
			auto target = layout.HitTest(point);
			if( !IsHeaderTargetEnabled(target) ) target = TerminalHeaderTarget::None;
			UpdateHotTarget(target);
			if( !trackingMouseLeave ) {
				TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
				trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
			}
			return 0;
		}
		case WM_LBUTTONUP: {
			if( draggingPaneDivider ) {
				const auto divider = *draggingPaneDivider;
				draggingPaneDivider.reset();
				if( ::GetCapture() == window ) ::ReleaseCapture();
				SetPaneDividerFromMouse(divider, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				return 0;
			}
			if( pressedTarget != TerminalHeaderTarget::None ) {
				const auto pressed = pressedTarget;
				pressedTarget = TerminalHeaderTarget::None;
				const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				const auto released = HeaderLayout().HitTest(point);
				if( ::GetCapture() == window ) ::ReleaseCapture();
				if( released == pressed ) ExecuteHeaderTarget(pressed);
				if( window && ::IsWindow(window) ) InvalidateTabs();
				return 0;
			}
			break;
		}
		case WM_MOUSELEAVE:
			trackingMouseLeave = false;
			UpdateHotTarget(TerminalHeaderTarget::None);
			return 0;
		case WM_CAPTURECHANGED:
			draggingPaneDivider.reset();
			if( pressedTarget != TerminalHeaderTarget::None ) {
				pressedTarget = TerminalHeaderTarget::None;
				InvalidateTabs();
			}
			return 0;
		case WM_SETCURSOR: {
			POINT point{};
			::GetCursorPos(&point);
			::ScreenToClient(window, &point);
			if( HitTestPaneDivider(point.x, point.y) ) {
				const auto divider = HitTestPaneDivider(point.x, point.y);
				bool vertical = false;
				if( divider && *divider < paneLayout.paneDividerNodes.size() ) {
					const auto activeIndex = groupLayout.ActiveGroupIndex();
					const auto orientation = activeIndex
						? groupLayout.NodeOrientation(*activeIndex, paneLayout.paneDividerNodes[*divider])
						: std::nullopt;
					if( orientation ) vertical = *orientation == TerminalPaneOrientation::Vertical;
				}
				::SetCursor(::LoadCursor(nullptr, vertical ? IDC_SIZENS : IDC_SIZEWE));
				return TRUE;
			}
			break;
		}
		case WM_KEYDOWN:
			if( terminalTabsFocused && (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_RETURN) ) {
				const auto tabs = OrderedListTabs();
				if( tabs.empty() ) return 0;
				const auto focused = FocusedTabId();
				auto index = std::find_if(tabs.begin(), tabs.end(), [focused](const TerminalTabSnapshot& tab) {
					return focused && tab.id == *focused;
				});
				const auto current = index == tabs.end() ? std::size_t{} : static_cast<std::size_t>(index - tabs.begin());
				if( wParam == VK_RETURN ) {
					terminalTabsFocused = false;
					static_cast<void>(SelectTerminal(tabs[current].id, true));
				} else {
					const auto direction = wParam == VK_UP ? tabs.size() - 1 : 1u;
					const auto next = (current + direction) % tabs.size();
					static_cast<void>(SelectTerminal(tabs[next].id, false));
				}
				return 0;
			}
			break;
		case WM_MOUSEWHEEL: {
			POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			::ScreenToClient(window, &point);
			if( ::PtInRect(&paneLayout.tabsBounds, point) != FALSE ) {
				ScrollTerminalTabs(GET_WHEEL_DELTA_WPARAM(wParam));
				return 0;
			}
			break;
		}
		case WM_RBUTTONUP: {
			POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			::ClientToScreen(window, &point);
			ShowContextMenu(point.x, point.y);
			return 0;
		}
		case kOutputAvailableMessage:
			ConsumeOutputWake(static_cast<std::uint64_t>(wParam));
			ScheduleOutputFrame(static_cast<std::uint64_t>(wParam));
			return 0;
		case WM_TIMER:
			if( wParam == kSynchronizedOutputTimer ) {
				// Broken or very long synchronized updates still present a bounded-rate
				// preview instead of freezing indefinitely.
				for( auto& pane : panes ) {
					if( pane.synchronizedOutputSince == 0 || !pane.window ) continue;
					pane.window->InvalidateAll();
					pane.needsFullRepaint = false;
					pane.synchronizedOutputSince = ::GetTickCount64();
				}
				return 0;
			}
			if( wParam == kProtocolInputRetryTimer ) {
				RetryPendingProtocolInput();
				return 0;
			}
			return ::DefWindowProcW(window, message, wParam, lParam);
		case kStateChangedMessage:
			ConsumeStateWake();
			InvalidateTabs();
			InvalidateTerminalTabs();
			if( ReconcileAgentPresence(true) ) NotifyAgentPresence();
			return 0;
		case kJobProcessMessage:
			ConsumeJobWake(static_cast<std::uint64_t>(wParam));
			if( ObserveAgentProcesses(static_cast<std::uint64_t>(wParam)) ) NotifyAgentPresence();
			return 0;
		case kTopologyChangedMessage:
			ConsumeTopologyWake();
			ReconcileTopology();
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return ::DefWindowProcW(window, message, wParam, lParam);
	}

	std::optional<std::uint64_t> AddTerminal( std::wstring_view directory = {} )
	{
		// New Terminal creates a separate terminal group.  A group is the unit
		// shown in the workbench panel; only Split Terminal adds another pane to
		// the currently active group.  Keeping this distinction also means that
		// the right-hand terminal list can flatten all groups without making a
		// newly-created terminal unexpectedly resize the active pane.
		const auto id = manager->AddTab(CurrentSize(),
			directory.empty() ? std::wstring_view(workingDirectory) : directory);
		if( !id ) return std::nullopt;
		const auto group = groupLayout.AppendGroup(*id);
		groupLayout.SetActiveGroup(group);
		if( window ) {
			static_cast<void>(RebuildPaneRenderers());
			LayoutChildren();
		}
		HandleOutput(*id);
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( active ) static_cast<void>(SelectTerminal(*id, true));
		return id;
	}

	std::optional<std::uint64_t> EnsureSessionStarted()
	{
		const auto id = manager->Activate(CurrentSize(), workingDirectory);
		if( !id ) return std::nullopt;
		const auto group = groupLayout.EnsureGroupForTab(*id);
		groupLayout.SetActiveGroup(group);
		if( window ) {
			static_cast<void>(EnsureTerminalWindow());
			LayoutChildren();
		}
		HandleOutput(*id);
		InvalidateTabs();
		InvalidateTerminalTabs();
		return id;
	}

	bool SelectTerminal( std::uint64_t tabId, bool focus = true )
	{
		const auto group = groupLayout.FindGroup(tabId);
		if( !group ) return false;
		if( !manager->SelectTab(tabId) ) return false;
		const bool groupChanged = groupLayout.ActiveGroupIndex() != group;
		groupLayout.SetActiveGroup(group);
		if( window ) {
			const bool rendered = groupChanged ? RebuildPaneRenderers() : EnsureTerminalWindow();
			if( !rendered ) return false;
			LayoutChildren();
		}
		EnsureTerminalTabVisible(tabId);
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( focus ) {
			terminalTabsFocused = false;
			if( auto* pane = FindPane(tabId); pane && pane->window ) pane->window->Focus();
		}
		return true;
	}

	bool RestartTerminal( std::uint64_t tabId )
	{
		const bool restarted = manager->RestartTab(tabId, SizeForTab(tabId), workingDirectory);
		if( auto* pane = FindPane(tabId); pane && pane->window ) {
			pane->window->SetInputAdapter(manager->InputAdapter(tabId));
			pane->window->SetModel(manager->Model(tabId));
			// RestartTab may retain the model object while replacing its semantic
			// generation. Fence that replacement even when the pointer is unchanged.
			static_cast<void>(pane->window->NotifyFrameContent());
			pane->needsFullRepaint = true;
		}
		HandleOutput(tabId);
		InvalidateTabs();
		InvalidateTerminalTabs();
		return restarted;
	}

	bool DeleteTerminal( std::uint64_t tabId )
	{
		const auto tabs = manager->Snapshot();
		if( std::none_of(tabs.begin(), tabs.end(), [tabId](const TerminalTabSnapshot& tab) { return tab.id == tabId; }) ) return false;
		const bool wasActive = manager->ActiveTabId() == tabId;
		std::vector<std::uint64_t> remainingInGroup;
		std::size_t removedIndex = 0;
		if( const auto owner = groupLayout.FindGroup(tabId) ) {
			remainingInGroup = groupLayout.GroupAt(*owner)->tabIds;
			const auto found = std::find(remainingInGroup.begin(), remainingInGroup.end(), tabId);
			removedIndex = static_cast<std::size_t>(found - remainingInGroup.begin());
			if( found != remainingInGroup.end() ) remainingInGroup.erase(found);
		}
		DestroyPaneRenderers();
		if( !manager->DeleteTab(tabId) ) {
			if( window ) static_cast<void>(RebuildPaneRenderers());
			return false;
		}
		static_cast<void>(groupLayout.RemoveTab(tabId));
		// VS Code TerminalGroup: removing the active instance activates the one now
		// at the same index in that group, or the last one when it was last. Only
		// an emptied group hands selection to the group RemoveTab chose.
		// The manager's successor follows global creation order and can belong to
		// another group, so it is used only when no group-local choice exists.
		std::optional<std::uint64_t> successor;
		if( wasActive && !remainingInGroup.empty() ) {
			successor = remainingInGroup[(std::min)(removedIndex, remainingInGroup.size() - 1)];
		} else if( wasActive ) {
			if( const auto* activeGroup = groupLayout.ActiveGroup();
				activeGroup && !activeGroup->tabIds.empty() ) {
				successor = activeGroup->tabIds.front();
			}
		}
		if( successor ) static_cast<void>(manager->SelectTab(*successor));
		if( const auto activeId = manager->ActiveTabId() ) groupLayout.SetActiveGroup(groupLayout.FindGroup(*activeId));
		if( window ) {
			static_cast<void>(RebuildPaneRenderers());
			LayoutChildren();
		}
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( ReconcileAgentPresence(false) ) NotifyAgentPresence();
		return true;
	}

	//! The current projection's groups as plain data for the recorder
	//! (#415 Phase 3). The focused pane names the active leaf, falling back to
	//! the manager's active tab; a tab that belongs to no group is never
	//! reported.
	[[nodiscard]] TerminalGroupLayoutSnapshot GroupLayoutSnapshot() const
	{
		TerminalGroupLayoutSnapshot snapshot;
		snapshot.groups = groupLayout.Groups();
		if( const auto focused = FocusedTabId(); focused && groupLayout.FindGroup(*focused) ) {
			snapshot.activeTabId = *focused;
		}
		return snapshot;
	}

	//! Installs the planned restore groups as one transaction (#415 Phase 3):
	//! every leaf gets a terminal first (the leaf marked `reuseFirstPrompt`
	//! may take a shell still at its first prompt, which leaves its old group
	//! for its place in the restored tree), each surviving tree is appended
	//! through TerminalGroupLayout::InstallGroup, and the renderers are
	//! rebuilt once. A leaf whose terminal fails collapses out of its tree;
	//! the rest still installs.
	[[nodiscard]] TerminalRestoreResult RestoreTerminalGroupLayout(
		std::vector<TerminalRestoreGroup> groups,
		std::optional<std::pair<std::size_t, std::size_t>> activeLeaf )
	{
		TerminalRestoreResult result;
		result.leafTabIds.resize(groups.size());
		for( std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex ) {
			result.leafTabIds[groupIndex].resize(groups[groupIndex].leaves.size());
		}
		if( closed || groups.empty() ) return result;
		// The first-prompt eligibility check needs fresh job data; like
		// AgentPresence(true) the caller is not notified of what it finds.
		static_cast<void>(ReconcileAgentPresence(true));

		// Pass 1: one terminal per leaf. Only the first leaf marked
		// `reuseFirstPrompt` may consume an existing first-prompt shell.
		std::optional<std::uint64_t> reusedTabId;
		for( std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex ) {
			const auto& spec = groups[groupIndex];
			for( std::size_t leafIndex = 0; leafIndex < spec.leaves.size(); ++leafIndex ) {
				const auto& leaf = spec.leaves[leafIndex];
				if( leaf.drop ) continue;
				const auto bytes = std::span(
					reinterpret_cast<const std::uint8_t*>(leaf.input.data()), leaf.input.size());
				std::optional<std::uint64_t> placed;
				if( leaf.reuseFirstPrompt && !leaf.input.empty() && !reusedTabId ) {
					auto tabs = OrderedListTabs();
					std::stable_partition(tabs.begin(), tabs.end(),
						[](const TerminalTabSnapshot& tab) { return tab.active; });
					for( const auto& tab : tabs ) {
						if( !IsTerminalAtFirstPrompt(tab, leaf.workingDirectory)
							|| agentPresence.Presence(tab.id) ) continue;
						if( manager->QueueInput(tab.id, bytes) != TerminalQueueInputResult::Accepted ) {
							continue;
						}
						// The reused shell leaves its own group for the
						// restored tree; an emptied group is erased, so no
						// stray single-pane group is left behind.
						static_cast<void>(groupLayout.RemoveTab(tab.id));
						reusedTabId = tab.id;
						placed = tab.id;
						break;
					}
				}
				if( !placed ) {
					const auto id = manager->AddTab(CurrentSize(), leaf.workingDirectory);
					if( !id ) continue;
					if( !leaf.input.empty()
						&& manager->QueueInput(*id, bytes) != TerminalQueueInputResult::Accepted ) {
						// The typed-ahead shell could not take the command: the
						// terminal is deleted rather than left ungrouped.
						static_cast<void>(manager->DeleteTab(*id));
						continue;
					}
					placed = id;
				}
				result.leafTabIds[groupIndex][leafIndex] = *placed;
			}
		}

		// Pass 2: emit each tree minus its unplaced leaves and install it.
		// A split left with one child collapses and the surviving neighbours
		// keep their weights.
		for( std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex ) {
			const auto& spec = groups[groupIndex];
			TerminalPaneGroup group;
			std::unordered_set<std::size_t> visited;
			const std::function<std::optional<std::size_t>(std::size_t)> emit =
				[&](const std::size_t nodeIndex) -> std::optional<std::size_t> {
				if( nodeIndex >= spec.nodes.size() || !visited.insert(nodeIndex).second ) {
					return std::nullopt;
				}
				const auto& node = spec.nodes[nodeIndex];
				if( node.leaf ) {
					if( !node.children.empty() || *node.leaf >= result.leafTabIds[groupIndex].size()
						|| !result.leafTabIds[groupIndex][*node.leaf] ) return std::nullopt;
					const auto tabId = *result.leafTabIds[groupIndex][*node.leaf];
					group.tabIds.push_back(tabId);
					group.nodes.push_back(TerminalPaneLayoutTreeNode{ .leafId = tabId });
					return group.nodes.size() - 1;
				}
				std::vector<std::size_t> children;
				std::vector<int> weights;
				for( std::size_t childIndex = 0; childIndex < node.children.size(); ++childIndex ) {
					const auto emitted = emit(node.children[childIndex]);
					if( !emitted ) continue;
					children.push_back(*emitted);
					weights.push_back(childIndex < node.weights.size() && node.weights[childIndex] > 0
						? node.weights[childIndex] : 1000);
				}
				if( children.empty() ) return std::nullopt;
				if( children.size() == 1 ) return children.front();
				group.nodes.push_back(TerminalPaneLayoutTreeNode{
					.leafId = 0,
					.children = std::move(children),
					.weights = std::move(weights),
					.orientation = node.orientation,
				});
				return group.nodes.size() - 1;
			};
			const auto root = spec.root < spec.nodes.size() ? emit(spec.root) : std::nullopt;
			if( !root ) continue;	// every leaf failed or was dropped: nothing to install
			group.root = *root;
			if( groupLayout.InstallGroup(std::move(group)) ) continue;
			// The model rejected the tree: roll its terminals back so no
			// half-built state remains. The reused shell returns to a group
			// of its own; created terminals are deleted.
			for( std::size_t leafIndex = 0; leafIndex < result.leafTabIds[groupIndex].size();
				++leafIndex ) {
				auto& placed = result.leafTabIds[groupIndex][leafIndex];
				if( !placed ) continue;
				if( *placed == reusedTabId ) {
					groupLayout.AppendGroup(*placed);
				} else {
					static_cast<void>(manager->DeleteTab(*placed));
					placed.reset();
				}
			}
		}

		// Pass 3: select the planned active leaf (or the first placed one)
		// and rebuild the renderers once for the new active group.
		std::optional<std::uint64_t> target;
		if( activeLeaf && activeLeaf->first < result.leafTabIds.size()
			&& activeLeaf->second < result.leafTabIds[activeLeaf->first].size() ) {
			target = result.leafTabIds[activeLeaf->first][activeLeaf->second];
		}
		if( !target ) {
			for( const auto& leafIds : result.leafTabIds ) {
				for( const auto& tabId : leafIds ) {
					if( tabId ) { target = *tabId; break; }
				}
				if( target ) break;
			}
		}
		if( target ) {
			static_cast<void>(SelectTerminal(*target, active));
		} else if( reusedTabId && window ) {
			// Nothing installed, but a shell moved out of its group: rebuild
			// the stale renderers of whatever group stays active.
			static_cast<void>(RebuildPaneRenderers());
			LayoutChildren();
		}
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( ReconcileAgentPresence(false) ) NotifyAgentPresence();
		return result;
	}

	TerminalWorkspaceSwitchResult SwitchWorkspace( TerminalWorkspaceSwitchRequest request )
	{
		TerminalWorkspaceSwitchResult result;
		if( closed ) return result;
		const auto oldKey = request.oldWorkspaceIdentityKey.empty()
			? currentWorkspaceIdentityKey : request.oldWorkspaceIdentityKey;
		if( currentWorkspaceIdentityKey.empty() ) currentWorkspaceIdentityKey = oldKey;

		if( oldKey == request.newWorkspaceIdentityKey ) {
			currentWorkspaceIdentityKey = request.newWorkspaceIdentityKey;
			workingDirectory = std::move(request.newWorkingDirectory);
			if( request.terminalVisible && manager->TabCount() == 0 ) {
				const auto id = EnsureSessionStarted();
				if( !id ) {
					result.outcome = TerminalWorkspaceSwitchOutcome::StartFailed;
					result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
					return result;
				}
				const auto tabs = manager->Snapshot();
				const auto found = std::ranges::find(tabs, *id, &TerminalTabSnapshot::id);
				if( found == tabs.end() || found->state == TerminalSessionState::Failed ) {
					result.outcome = TerminalWorkspaceSwitchOutcome::StartFailed;
					result.errorCode = found == tabs.end() ? ERROR_GEN_FAILURE : found->errorCode;
					return result;
				}
				result.outcome = TerminalWorkspaceSwitchOutcome::Created;
				result.startedTabCount = 1;
				return result;
			}
			InvalidateTabs();
			InvalidateTerminalTabs();
			result.outcome = TerminalWorkspaceSwitchOutcome::Unchanged;
			result.restoredTabCount = manager->TabCount();
			return result;
		}

		auto oldState = DetachCurrentProjection();
		result.savedTabCount = oldState.tabs.tabs.size();
		WorkspaceProjectionState targetState;
		std::wstring targetWorkingDirectory = request.newWorkingDirectory;
		if( const auto found = workspaceRegistry.find(request.newWorkspaceIdentityKey);
			found != workspaceRegistry.end() ) {
			targetState = found->second;
			targetWorkingDirectory = targetState.workingDirectory;
		}
		if( targetWorkingDirectory.empty() ) targetWorkingDirectory = request.newWorkingDirectory;

		const auto applied = ApplyWorkspaceProjection(
			std::move(targetState), std::move(targetWorkingDirectory),
			request.terminalVisible, request.terminalVisible);
		if( applied.succeeded ) {
			StoreWorkspaceProjection(oldKey, std::move(oldState));
			workspaceRegistry.erase(request.newWorkspaceIdentityKey);
			currentWorkspaceIdentityKey = std::move(request.newWorkspaceIdentityKey);
			result.restoredTabCount = applied.attachedTabCount;
			result.startedTabCount = applied.startedSession ? 1 : 0;
			if( applied.startedSession ) result.outcome = TerminalWorkspaceSwitchOutcome::Created;
			else if( applied.attachedTabCount != 0 ) result.outcome = TerminalWorkspaceSwitchOutcome::Attached;
			else result.outcome = TerminalWorkspaceSwitchOutcome::Detached;
			return result;
		}

		auto failedTarget = DetachCurrentProjection();
		if( !failedTarget.tabs.tabs.empty() || failedTarget.groupLayout.GroupCount() != 0 ) {
			StoreWorkspaceProjection(request.newWorkspaceIdentityKey, std::move(failedTarget));
		}
		const bool restoreRenderers = oldState.renderersMaterialized;
		const auto restored = ApplyWorkspaceProjection(
			std::move(oldState), std::wstring{}, restoreRenderers, false);
		result.outcome = restored.succeeded
			? applied.failureOutcome : TerminalWorkspaceSwitchOutcome::RestoreFailed;
		result.oldProjectionRestored = restored.succeeded;
		result.restoredTabCount = restored.attachedTabCount;
		result.errorCode = restored.succeeded ? applied.errorCode : restored.errorCode;
		return result;
	}

	TerminalWorkspaceResetResult ResetForWorkspace( std::wstring nextWorkingDirectory, bool recreateSession )
	{
		TerminalWorkspaceResetResult result;
		if( closed ) return result;
		workingDirectory = std::move(nextWorkingDirectory);

		PrepareProjectionDetach();
		groupLayout.Clear();
		terminalTabsFocused = false;

		const auto deadline = std::chrono::steady_clock::now()
			+ CTerminalSession::kGracefulCloseTimeout + CTerminalSession::kForcedCloseTimeout;
		const auto cleared = manager->ClearTabs(deadline);
		result.clearedTabCount = cleared.clearedTabCount;
		result.closeDeadlineExceeded = cleared.status == TerminalTabClearStatus::DeadlineExceeded;
		if( cleared.status == TerminalTabClearStatus::Unavailable ) return result;
		if( cleared.status == TerminalTabClearStatus::InProgress ) {
			result.outcome = TerminalWorkspaceResetOutcome::Busy;
			return result;
		}

		LayoutChildren();
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( !recreateSession ) {
			result.outcome = TerminalWorkspaceResetOutcome::Cleared;
			return result;
		}

		const auto replacementId = EnsureSessionStarted();
		if( !replacementId ) {
			result.outcome = TerminalWorkspaceResetOutcome::RestartFailed;
			result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
			return result;
		}
		const auto tabs = manager->Snapshot();
		const auto replacement = std::ranges::find(tabs, *replacementId, &TerminalTabSnapshot::id);
		if( replacement == tabs.end() || replacement->state == TerminalSessionState::Failed ) {
			result.outcome = TerminalWorkspaceResetOutcome::RestartFailed;
			result.errorCode = replacement == tabs.end() ? ERROR_GEN_FAILURE : replacement->errorCode;
			return result;
		}
		result.outcome = TerminalWorkspaceResetOutcome::Restarted;
		return result;
	}

	void CloseAllWorkspaceProjections() noexcept
	{
		const auto deadline = std::chrono::steady_clock::now()
			+ CTerminalSession::kGracefulCloseTimeout + CTerminalSession::kForcedCloseTimeout;
		auto activeProjection = manager->DetachTabs();
		for( auto& entry : workspaceRegistry ) {
			const auto attached = manager->AttachTabs(std::move(entry.second.tabs));
			if( attached.Succeeded() ) static_cast<void>(manager->ClearTabs(deadline));
		}
		workspaceRegistry.clear();
		const auto activeAttached = manager->AttachTabs(std::move(activeProjection));
		if( activeAttached.Succeeded() ) static_cast<void>(manager->ClearTabs(deadline));
	}

	bool SplitTerminal( TerminalPaneOrientation orientation )
	{
		if( closed ) return false;
		auto primaryId = FocusedTabId();
		if( !primaryId ) {
			primaryId = manager->Activate(CurrentSize(), workingDirectory);
			if( !primaryId ) return false;
		}
		const auto groupIndex = groupLayout.EnsureGroupForTab(*primaryId);
		groupLayout.SetActiveGroup(groupIndex);
		const auto originalActiveGroup = groupLayout.ActiveGroupIndex();
		const auto originalActiveTab = manager->ActiveTabId();
		const auto* group = groupLayout.ActiveGroup();
		if( !group ) return false;
		groupLayout.NormalizeGroup(groupIndex);
		const auto originalGroup = *group;
		const auto newId = manager->AddTab(SizeForTab(*primaryId), workingDirectory);
		if( !newId ) return false;
		if( !groupLayout.ActiveGroup() ) {
			static_cast<void>(manager->DeleteTab(*newId));
			return false;
		}
		if( !groupLayout.InsertPaneSplit(groupIndex, *primaryId, *newId, orientation) ) {
			static_cast<void>(manager->DeleteTab(*newId));
			return false;
		}
		if( window ) {
			if( !RebuildPaneRenderers() ) {
				// The renderer rebuild can fail after the manager has created a live
				// session.  Restore the group before returning so a failed UI operation
				// never leaves an unowned terminal or a phantom split in the model.
				if( manager->DeleteTab(*newId) ) {
					groupLayout.ReplaceGroup(groupIndex, originalGroup);
					groupLayout.SetActiveGroup(originalActiveGroup);
					if( originalActiveTab ) static_cast<void>(manager->SelectTab(*originalActiveTab));
					static_cast<void>(RebuildPaneRenderers());
					LayoutChildren();
				}
				return false;
			}
			LayoutChildren();
		}
		InvalidateTabs();
		InvalidateTerminalTabs();
		if( active ) static_cast<void>(SelectTerminal(*newId, true));
		else static_cast<void>(manager->SelectTab(*newId));
		return true;
	}

	bool SplitTerminalRight()
	{
		return SplitTerminal(TerminalPaneOrientation::Horizontal);
	}

	bool SplitTerminalDown()
	{
		return SplitTerminal(TerminalPaneOrientation::Vertical);
	}

	bool HasTerminalSplit() const noexcept
	{
		const auto* group = groupLayout.ActiveGroup();
		return group && group->tabIds.size() > 1;
	}

	bool CloseTerminalSplit()
	{
		const auto focused = FocusedTabId();
		return HasTerminalSplit() && focused && DeleteTerminal(*focused);
	}

	//! Commits one layout transaction (#415 Phase 5): selects `selectId` when
	//! given, installs `updated` and rebuilds the renderers once. A renderer
	//! rebuild that fails restores the previous layout and selection, as
	//! SplitTerminal does, so a failed UI step never leaves a layout without its
	//! panes. Keyboard focus follows only when it was already in a pane.
	bool CommitLayoutEdit( TerminalGroupLayout updated, std::optional<std::uint64_t> selectId )
	{
		const bool hadFocus = PaneHasKeyboardFocus();
		const auto previousActive = manager->ActiveTabId();
		if( selectId && selectId != previousActive && !manager->SelectTab(*selectId) ) return false;
		auto previous = std::exchange(groupLayout, std::move(updated));
		if( window ) {
			if( !RebuildPaneRenderers() ) {
				groupLayout = std::move(previous);
				if( previousActive && manager->ActiveTabId() != previousActive ) {
					static_cast<void>(manager->SelectTab(*previousActive));
				}
				static_cast<void>(RebuildPaneRenderers());
				LayoutChildren();
				return false;
			}
			LayoutChildren();
		}
		if( const auto id = manager->ActiveTabId() ) {
			EnsureTerminalTabVisible(*id);
			if( hadFocus ) {
				terminalTabsFocused = false;
				if( auto* pane = FindPane(*id); pane && pane->window ) pane->window->Focus();
			}
		}
		InvalidateTabs();
		InvalidateTerminalTabs();
		NotifyAgentPresence();
		return true;
	}

	//! VS Code unsplit keeps the old group; tmux break selects the moved pane.
	bool UnsplitTerminal( std::optional<std::uint64_t> tabId, bool selectMoved = false )
	{
		const auto id = tabId ? tabId : manager->ActiveTabId();
		if( !id ) return false;
		const auto group = groupLayout.FindGroup(*id);
		if( !group || groupLayout.GroupAt(*group)->tabIds.size() < 2 ) return false;
		auto remaining = groupLayout.GroupAt(*group)->tabIds;
		const auto found = std::find(remaining.begin(), remaining.end(), *id);
		const auto index = static_cast<std::size_t>(found - remaining.begin());
		remaining.erase(found);
		const bool wasActive = manager->ActiveTabId() == id;
		auto updated = groupLayout;
		if( updated.Unsplit(*id) != TerminalLayoutEdit::Changed ) return false;
		std::optional<std::uint64_t> select;
		if( selectMoved ) {
			select = *id;
		} else if( wasActive ) {
			select = remaining[(std::min)(index, remaining.size() - 1)];
		}
		if( select ) updated.SetActiveGroup(updated.FindGroup(*select));
		return CommitLayoutEdit(std::move(updated), select);
	}

	[[nodiscard]] std::vector<std::uint64_t> JoinCandidates() const
	{
		std::vector<std::uint64_t> result;
		const auto activeId = manager->ActiveTabId();
		for( const auto& tab : OrderedListTabs() ) {
			const auto group = groupLayout.FindGroup(tab.id);
			if( tab.id != activeId && group && groupLayout.GroupAt(*group)->tabIds.size() == 1 ) {
				result.push_back(tab.id);
			}
		}
		return result;
	}

	bool JoinTerminals( std::uint64_t pickedTabId )
	{
		const auto activeId = manager->ActiveTabId();
		const auto candidates = JoinCandidates();
		if( !activeId || std::find(candidates.begin(), candidates.end(), pickedTabId) == candidates.end() ) return false;
		auto updated = groupLayout;
		if( updated.Join({ pickedTabId, *activeId }) != TerminalLayoutEdit::Changed ) return false;
		return CommitLayoutEdit(std::move(updated), pickedTabId);
	}

	//! tmux swap-pane moves identities, not screen-position weights.
	bool SwapPane( int direction )
	{
		const auto id = manager->ActiveTabId();
		if( !id ) return false;
		const auto other = groupLayout.RelativeLeaf(*id, direction);
		if( !other ) return false;
		auto updated = groupLayout;
		if( updated.Swap(*id, *other) != TerminalLayoutEdit::Changed ) return false;
		return CommitLayoutEdit(std::move(updated), std::nullopt);
	}

	bool FocusRelativePane( int direction )
	{
		const auto* group = groupLayout.ActiveGroup();
		if( !group || group->tabIds.empty() ) return false;
		const auto focused = FocusedTabId();
		auto found = std::find(group->tabIds.begin(), group->tabIds.end(), focused.value_or(group->tabIds.front()));
		const auto current = found == group->tabIds.end() ? std::size_t{} : static_cast<std::size_t>(found - group->tabIds.begin());
		const auto count = group->tabIds.size();
		const auto next = direction < 0 ? (current + count - 1) % count : (current + 1) % count;
		return SelectTerminal(group->tabIds[next], true);
	}

	bool FocusRelativeGroup( int direction )
	{
		const auto next = groupLayout.RelativeGroupIndex(direction);
		if( !next ) return false;
		const auto* group = groupLayout.GroupAt(*next);
		return group && !group->tabIds.empty() && SelectTerminal(group->tabIds.front(), true);
	}

	//! tmux windows and Screen windows are terminal groups here, not panes, so a
	//! digit chord selects the group's first pane exactly as its list order shows.
	bool SelectTerminalGroup( std::size_t index )
	{
		const auto* group = groupLayout.GroupAt(index);
		return group && !group->tabIds.empty() && SelectTerminal(group->tabIds.front(), true);
	}

	void ApplyShortcutPreset( TerminalShortcutPreset preset, bool persist )
	{
		if( preset == shortcutPreset ) return;
		shortcutPreset = preset;
		shortcutPrefixArmed = false;
		if( persist && shortcutPresetSink ) shortcutPresetSink(preset);
	}

	bool RunShortcutPresetAction( const TerminalPresetResolution& resolution )
	{
		switch( resolution.action ) {
		case TerminalPresetAction::NewTerminal: return AddTerminal().has_value();
		case TerminalPresetAction::SplitRight: return SplitTerminalRight();
		case TerminalPresetAction::SplitDown: return SplitTerminalDown();
		case TerminalPresetAction::ClosePane: {
			// tmux kill-pane / Screen remove: the last pane of a group closes
			// its terminal, so a lone pane is not a no-op (#403).
			const auto id = FocusedTabId();
			return id && DeleteTerminal(*id);
		}
		case TerminalPresetAction::CloseTerminal: {
			const auto id = FocusedTabId();
			return id && DeleteTerminal(*id);
		}
		case TerminalPresetAction::NextPane: return FocusRelativePane(1);
		case TerminalPresetAction::PreviousPane: return FocusRelativePane(-1);
		case TerminalPresetAction::SwapPanePrevious: return SwapPane(-1);
		case TerminalPresetAction::SwapPaneNext: return SwapPane(1);
		case TerminalPresetAction::BreakPane: return UnsplitTerminal(manager->ActiveTabId(), true);
		case TerminalPresetAction::NextTerminal: return FocusRelativeGroup(1);
		case TerminalPresetAction::PreviousTerminal: return FocusRelativeGroup(-1);
		case TerminalPresetAction::SelectTerminal: return SelectTerminalGroup(resolution.terminalIndex);
		case TerminalPresetAction::FocusTerminalList: return FocusTerminalTabs();
		case TerminalPresetAction::None: break;
		}
		return false;
	}

	//! Returns true when the preset owns the key. The key is then never forwarded
	//! to the shell, even if the resolved action could not run: swallowing a
	//! chord is the multiplexer behaviour, while leaking `%` into the shell is not.
	bool DispatchShortcutPresetKey( const TerminalPresetKey& key )
	{
		if( shortcutPreset == TerminalShortcutPreset::None ) return false;
		const auto resolution = ResolveTerminalPresetKey(shortcutPreset, shortcutPrefixArmed, key);
		shortcutPrefixArmed = resolution.prefixArmed;
		if( !resolution.consumed ) return false;
		static_cast<void>(RunShortcutPresetAction(resolution));
		return true;
	}

	bool HandleShortcutPresetKey( const MSG& message )
	{
		if( shortcutPreset == TerminalShortcutPreset::None ) return false;
		if( message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN ) return false;
		if( !IsTerminalUiMessage(message) ) return false;
		TerminalPresetKey key;
		key.virtualKey = static_cast<std::uint32_t>(message.wParam);
		// An active IME can replace a printable key message with VK_PROCESSKEY.
		// The multiplexer prefix is a physical-key chord, so recover the original
		// virtual key before passing the message to the pure shortcut resolver.
		if( key.virtualKey == VK_PROCESSKEY ) {
			const UINT imeVirtualKey = ::ImmGetVirtualKey(message.hwnd);
			if( imeVirtualKey != VK_PROCESSKEY ) key.virtualKey = imeVirtualKey;
		}
		key.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		key.control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		key.alt = (static_cast<ULONG_PTR>(message.lParam) & (1ULL << 29)) != 0;
		return DispatchShortcutPresetKey(key);
	}

	bool FocusTerminalTabs()
	{
		if( IsEmptyRect(paneLayout.tabsBounds) || !window ) return false;
		terminalTabsFocused = true;
		::SetFocus(window);
		InvalidateTerminalTabs();
		return true;
	}

	bool IsPaneMessage( const MSG& message ) const noexcept
	{
		return std::any_of(panes.begin(), panes.end(), [&message](const TerminalPane& pane) {
			const HWND paneWindow = pane.window ? pane.window->GetHwnd() : nullptr;
			return paneWindow && (message.hwnd == paneWindow || ::IsChild(paneWindow, message.hwnd));
		});
	}

	bool IsTerminalUiMessage( const MSG& message ) const noexcept
	{
		return message.hwnd == window || IsPaneMessage(message);
	}

	bool HandleWorkbenchTerminalKeybinding( MSG& message )
	{
		if( !IsTerminalUiMessage(message)
			|| (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) ) return false;
		const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool alt = (static_cast<ULONG_PTR>(message.lParam) & (1ULL << 29)) != 0;
		// Stable VS Code terminal command IDs: workbench.action.terminal.split,
		// focusPreviousPane, focusNextPane, focusTabs, focusPrevious, and focusNext.
		// Ctrl+Alt+5 is Sakura's orthogonal (vertical) split; upstream Panel stays
		// single-axis, so this keybinding is an intentional extension.
		if( control && shift && !alt && message.wParam == '5' ) return SplitTerminalRight();
		if( control && alt && !shift && message.wParam == '5' ) return SplitTerminalDown();
		if( alt && message.wParam == VK_LEFT ) return FocusRelativePane(-1);
		if( alt && message.wParam == VK_RIGHT ) return FocusRelativePane(1);
		if( alt && message.wParam == VK_UP ) return FocusRelativePane(-1);
		if( alt && message.wParam == VK_DOWN ) return FocusRelativePane(1);
		if( control && shift && message.wParam == VK_OEM_5 ) return FocusTerminalTabs();
		if( control && message.wParam == VK_PRIOR ) return FocusRelativeGroup(-1);
		if( control && message.wParam == VK_NEXT ) return FocusRelativeGroup(1);
		return false;
	}
};

CTerminalTool::CTerminalTool( TerminalTabManagerDependencies dependencies )
	: m_impl(std::make_unique<Impl>(std::move(dependencies)))
{
}

CTerminalTool::~CTerminalTool()
{
	Close();
}

bool CTerminalTool::Create( HWND parent )
{
	if( m_impl->closed || m_impl->window || parent == nullptr ) return false;
	m_impl->instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if( !m_impl->instance ) m_impl->instance = ::GetModuleHandleW(nullptr);
	if( !EnsureToolClass(m_impl->instance) ) return false;
	m_impl->window = ::CreateWindowExW(0, kTerminalToolWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, m_impl->instance, m_impl.get());
	if( !m_impl->window ) return false;
	{
		const std::lock_guard lock(m_impl->gate->mutex);
		m_impl->gate->window = m_impl->window;
	}
	// The host is intentionally lightweight.  A hidden bottom panel must not
	// create the renderer (and its font/scrollbar resources) just by starting
	// Sakura.  The first visible Layout, Activate or explicit AddTerminal owns
	// that materialization.
	return true;
}

void CTerminalTool::Layout( const RECT& contentRect, unsigned int dpi )
{
	if( m_impl->closed || !m_impl->window ) return;
	m_impl->bounds = contentRect;
	m_impl->dpi = dpi == 0 ? kDefaultDpi : dpi;
	if( m_impl->chromeFont.Get() == nullptr || m_impl->chromeFont.Dpi() != m_impl->dpi ) {
		static_cast<void>(m_impl->chromeFont.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi));
	}
	::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top, std::max(0L, contentRect.right - contentRect.left),
		std::max(0L, contentRect.bottom - contentRect.top), SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
	if( m_impl->HasVisibleContentBounds() ) static_cast<void>(m_impl->EnsureTerminalWindow());
	m_impl->LayoutChildren();
	m_impl->InvalidateTabs();
	m_impl->InvalidateTerminalTabs();
}

void CTerminalTool::Activate()
{
	if( m_impl->closed ) return;
	m_impl->active = true;
	static_cast<void>(m_impl->EnsureSessionStarted());
	if( const auto id = m_impl->FocusedTabId() ) static_cast<void>(m_impl->SelectTerminal(*id, true));
}

void CTerminalTool::Deactivate()
{
	// Deliberately preserve every tab, session, pipe and parser while hidden.
	m_impl->active = false;
}

void CTerminalTool::CommitGdiFrames() noexcept
{
	if( !m_impl || m_impl->closed ) return;
	for( auto& pane : m_impl->panes ) {
		if( pane.window ) static_cast<void>(pane.window->CommitGdiFrame());
	}
}

void CTerminalTool::NotifyFrameDeviceEpoch(const std::uint64_t epoch) noexcept
{
	if( !m_impl || m_impl->closed || epoch == 0 ) return;
	m_impl->nativeDeviceEpoch = epoch;
	for( auto& pane : m_impl->panes ) {
		if( !pane.window ) continue;
		const auto before = pane.window->FrameSurfaceSnapshot();
		if( before.deviceEpoch == epoch ) continue;
		const auto result = pane.window->NotifyFrameDeviceEpoch(epoch);
		if( result.Accepted() && pane.window->GetHwnd() != nullptr ) {
			::InvalidateRect(pane.window->GetHwnd(), nullptr, FALSE);
		}
	}
}

void CTerminalTool::SetNativeFrameRuntimeBridge(
	TerminalNativeFrameBridgePtr bridge ) noexcept
{
	if( !m_impl || m_impl->closed ) return;
	m_impl->SetNativeFrameRuntimeBridge(std::move(bridge));
}

void CTerminalTool::DetachNativeFrameRuntime() noexcept
{
	if( !m_impl ) return;
	m_impl->DetachNativeFrameRuntime();
}

bool CTerminalTool::PreTranslateMessage( MSG& message )
{
	if( m_impl->closed ) return false;
	// The preset prefix runs first. Once it is armed the second key belongs to the
	// preset, not to a workbench keybinding that happens to use the same key.
	if( m_impl->HandleShortcutPresetKey(message) ) return true;
	if( m_impl->HandleWorkbenchTerminalKeybinding(message) ) return true;
	for( auto& pane : m_impl->panes ) {
		if( pane.window && pane.window->PreTranslateMessage(message) ) return true;
	}
	return false;
}

void CTerminalTool::Close()
{
	if( !m_impl || m_impl->closed ) return;
	{
		const std::lock_guard lock(m_impl->gate->mutex);
		m_impl->gate->alive = false;
		m_impl->gate->window = nullptr;
		m_impl->gate->stateWakePending = false;
		m_impl->gate->topologyWakePending = false;
		m_impl->gate->outputWakePending.clear();
		m_impl->gate->jobWakePending.clear();
		m_impl->gate->agentProcessIds.clear();
	}
	m_impl->agentPresenceSink = {};
	if( m_impl->window ) ::KillTimer(m_impl->window, kSynchronizedOutputTimer);
	if( m_impl->window ) ::KillTimer(m_impl->window, kProtocolInputRetryTimer);
	m_impl->DetachNativeFrameRuntime();
	m_impl->DestroyPaneRenderers();
	m_impl->groupLayout.Clear();
	m_impl->CloseAllWorkspaceProjections();
	m_impl->manager->Close();
	m_impl->closed = true;
	if( m_impl->headerHost && ::GetCapture() == m_impl->headerHost ) ::ReleaseCapture();
	m_impl->headerHost = nullptr;
	m_impl->hostedHeaderBounds = {};
	if( m_impl->window ) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
}

void CTerminalTool::SetWorkingDirectory( std::wstring workingDirectory )
{
	// Existing sessions keep their original CWD. This value is used only by a
	// subsequently created or explicitly restarted tab.
	m_impl->workingDirectory = std::move(workingDirectory);
}

TerminalWorkspaceSwitchResult CTerminalTool::SwitchWorkspace(
	TerminalWorkspaceSwitchRequest request )
{
	auto result = m_impl->SwitchWorkspace(std::move(request));
	// The visible tab set is now another workspace's projection; agents of the
	// detached one are no longer this window's current project's agents.
	if( !m_impl->closed && m_impl->ReconcileAgentPresence(true) ) m_impl->NotifyAgentPresence();
	return result;
}

TerminalWorkspaceResetResult CTerminalTool::ResetForWorkspace(
	std::wstring workingDirectory, bool recreateSession )
{
	auto result = m_impl->ResetForWorkspace(std::move(workingDirectory), recreateSession);
	if( !m_impl->closed && m_impl->ReconcileAgentPresence(true) ) m_impl->NotifyAgentPresence();
	return result;
}

void CTerminalTool::SetPalette( const theme::ThemePalette& palette )
{
	m_impl->palette = palette;
	for( auto& pane : m_impl->panes ) {
		if( pane.window ) pane.window->SetPalette(palette);
	}
	if( m_impl->window ) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CTerminalTool::SetPanelActions( TerminalPanelActions actions )
{
	m_impl->panelActions = std::move(actions);
	m_impl->LayoutChildren();
	m_impl->InvalidateTabs();
}

void CTerminalTool::SetPanelHeaderHost( HWND host )
{
	if( m_impl->headerHost && ::GetCapture() == m_impl->headerHost && m_impl->headerHost != host ) {
		::ReleaseCapture();
	}
	m_impl->headerHost = host;
	if( !host ) m_impl->hostedHeaderBounds = {};
	m_impl->InvalidateTabs();
}

void CTerminalTool::PaintPanelHeader( HDC dc, const RECT& bounds, unsigned int dpi )
{
	if( m_impl->closed || !dc ) return;
	m_impl->hostedHeaderBounds = bounds;
	m_impl->hostedHeaderDpi = dpi == 0 ? kDefaultDpi : dpi;
	m_impl->PaintHeaderTo(dc, bounds, m_impl->hostedHeaderDpi, false, false, 34);
}

bool CTerminalTool::HandlePanelHeaderMessage( UINT message, WPARAM wParam, LPARAM lParam,
	const RECT& bounds, unsigned int dpi )
{
	if( m_impl->closed ) return false;
	return m_impl->HandleHostedHeaderMessage(message, wParam, lParam, bounds, dpi);
}

bool CTerminalTool::EnsureSessionStarted()
{
	if( m_impl->closed ) return false;
	const auto id = m_impl->EnsureSessionStarted();
	if( !id ) return false;
	// AddTab deliberately retains a Failed tab so the user can see/restart it.
	// Do not mistake that visible tab identifier for a successful first launch.
	const auto tabs = m_impl->manager->Snapshot();
	const auto found = std::find_if(tabs.begin(), tabs.end(), [id](const auto& tab) { return tab.id == *id; });
	return found != tabs.end() && found->state == TerminalSessionState::Running;
}

std::optional<std::uint64_t> CTerminalTool::AddTerminal()
{
	return m_impl->closed ? std::nullopt : m_impl->AddTerminal();
}

std::optional<std::uint64_t> CTerminalTool::AddTerminalWithInput(
	std::wstring_view workingDirectory, std::string_view input )
{
	if( m_impl->closed ) return std::nullopt;
	const auto id = m_impl->AddTerminal(workingDirectory);
	if( !id ) return std::nullopt;
	// The session is Running once AddTab returns a started tab, and ConPTY
	// keeps typed-ahead input until the shell reads it.
	const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
	if( m_impl->manager->QueueInput(*id, bytes) != TerminalQueueInputResult::Accepted ) return std::nullopt;
	return id;
}

std::optional<TerminalInputTarget> CTerminalTool::RunInFirstPromptTerminalOrAdd(
	std::wstring_view workingDirectory, std::string_view input )
{
	if( m_impl->closed ) return std::nullopt;
	const std::wstring directory = workingDirectory.empty()
		? m_impl->workingDirectory : std::wstring(workingDirectory);
	// A job observation is bounded by the tab count. Like AgentPresence(true),
	// the caller is not notified of what the refresh finds.
	static_cast<void>(m_impl->ReconcileAgentPresence(true));
	auto tabs = m_impl->OrderedListTabs();
	std::stable_partition(tabs.begin(), tabs.end(), [](const TerminalTabSnapshot& tab) { return tab.active; });
	const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
	for( const auto& tab : tabs ) {
		if( !IsTerminalAtFirstPrompt(tab, directory) || m_impl->agentPresence.Presence(tab.id) ) continue;
		if( m_impl->manager->QueueInput(tab.id, bytes) == TerminalQueueInputResult::Accepted ) {
			return TerminalInputTarget{ tab.id, true };
		}
	}
	const auto id = AddTerminalWithInput(directory, input);
	if( !id ) return std::nullopt;
	return TerminalInputTarget{ *id, false };
}

TerminalGroupLayoutSnapshot CTerminalTool::GroupLayoutSnapshot() const
{
	return m_impl->GroupLayoutSnapshot();
}

TerminalRestoreResult CTerminalTool::RestoreTerminalGroupLayout(
	std::vector<TerminalRestoreGroup> groups,
	std::optional<std::pair<std::size_t, std::size_t>> activeLeaf )
{
	return m_impl->RestoreTerminalGroupLayout(std::move(groups), activeLeaf);
}

void CTerminalTool::SetShortcutPreset( TerminalShortcutPreset preset )
{
	m_impl->ApplyShortcutPreset(preset, false);
}

void CTerminalTool::SetTabPresentationSettings( TerminalTabPresentationSettings settings )
{
	if( m_impl->closed ) return;
	m_impl->SetTabPresentationSettings(std::move(settings));
}

void CTerminalTool::SetScrollbackLimit( std::size_t lines )
{
	if( m_impl->closed ) return;
	m_impl->SetScrollbackLimit(lines);
}

std::size_t CTerminalTool::ScrollbackLimit() const noexcept
{
	return m_impl && !m_impl->closed ? m_impl->manager->ScrollbackLimit() : 0;
}

TerminalShortcutPreset CTerminalTool::ShortcutPreset() const noexcept
{
	return m_impl->shortcutPreset;
}

void CTerminalTool::SetShortcutPresetSink( std::function<void(TerminalShortcutPreset)> sink )
{
	m_impl->shortcutPresetSink = std::move(sink);
}

bool CTerminalTool::DispatchShortcutPresetKey( const TerminalPresetKey& key )
{
	if( m_impl->closed ) return false;
	return m_impl->DispatchShortcutPresetKey(key);
}

void CTerminalTool::RedetectPowerShell()
{
	if( !m_impl->closed && m_impl->profileService ) {
		m_impl->profileService->Redetect();
	}
}

bool CTerminalTool::SelectTerminal( std::uint64_t tabId )
{
	return !m_impl->closed && m_impl->SelectTerminal(tabId);
}

bool CTerminalTool::RestartTerminal( std::uint64_t tabId )
{
	return !m_impl->closed && m_impl->RestartTerminal(tabId);
}

bool CTerminalTool::DeleteTerminal( std::uint64_t tabId )
{
	return !m_impl->closed && m_impl->DeleteTerminal(tabId);
}

bool CTerminalTool::SplitTerminalRight()
{
	return !m_impl->closed && m_impl->SplitTerminalRight();
}

bool CTerminalTool::SplitTerminalDown()
{
	return !m_impl->closed && m_impl->SplitTerminalDown();
}

bool CTerminalTool::CloseTerminalSplit()
{
	return !m_impl->closed && m_impl->CloseTerminalSplit();
}

bool CTerminalTool::UnsplitTerminal( std::optional<std::uint64_t> tabId )
{
	return !m_impl->closed && m_impl->UnsplitTerminal(tabId);
}

std::vector<std::uint64_t> CTerminalTool::JoinCandidates() const
{
	return m_impl->closed ? std::vector<std::uint64_t>{} : m_impl->JoinCandidates();
}

bool CTerminalTool::JoinTerminals( std::uint64_t pickedTabId )
{
	return !m_impl->closed && m_impl->JoinTerminals(pickedTabId);
}

bool CTerminalTool::HasTerminalSplit() const noexcept
{
	return !m_impl->closed && m_impl->HasTerminalSplit();
}

TerminalPaneOrientation CTerminalTool::ActivePaneOrientation() const noexcept
{
	if( m_impl->closed ) return TerminalPaneOrientation::Horizontal;
	const auto* group = m_impl->groupLayout.ActiveGroup();
	if( !group ) return TerminalPaneOrientation::Horizontal;
	return m_impl->groupLayout.PaneOrientationForTab(*m_impl->groupLayout.ActiveGroupIndex(),
		m_impl->manager->ActiveTabId().value_or(group->tabIds.empty() ? 0 : group->tabIds.front()));
}

std::vector<TerminalTabSnapshot> CTerminalTool::Tabs() const
{
	return m_impl->manager->Snapshot();
}

std::vector<TerminalListRow> CTerminalTool::TerminalListRows() const
{
	return m_impl->closed ? std::vector<TerminalListRow>{} : m_impl->TerminalListRows();
}

std::vector<TerminalAgentPresenceRecord> CTerminalTool::AgentPresence( bool refresh )
{
	std::vector<TerminalAgentPresenceRecord> result;
	if( m_impl->closed ) return result;
	// The caller asked for current data and receives it, so a refresh-driven
	// change is not echoed back through the sink.
	if( refresh ) static_cast<void>(m_impl->ReconcileAgentPresence(true));
	const auto tabs = m_impl->OrderedListTabs();
	const auto projection = m_impl->manager->ProjectionSnapshot();
	for( const auto& tab : tabs ) {
		const auto kind = m_impl->agentPresence.Presence(tab.id);
		if( !kind ) continue;
		const auto record = std::find_if(projection.begin(), projection.end(),
			[&tab](const TerminalTabProjectionRecord& value) { return value.tabId == tab.id; });
		if( record == projection.end() ) continue;
		result.push_back({ tab.id, record->instanceId, *kind,
			m_impl->agentPresence.SessionTitle(tab.id),
			m_impl->agentPresence.StartedUnixSeconds(tab.id).value_or(0) });
	}
	return result;
}

void CTerminalTool::SetAgentPresenceSink( std::function<void()> sink )
{
	if( m_impl->closed ) return;
	m_impl->agentPresenceSink = std::move(sink);
}

std::optional<std::uint64_t> CTerminalTool::ActiveTerminalId() const noexcept
{
	return m_impl->manager->ActiveTabId();
}

std::wstring CTerminalTool::ActiveTerminalTitle() const
{
	return m_impl->closed ? std::wstring{} : m_impl->ActiveTerminalTitle();
}

std::size_t CTerminalTool::TabCount() const noexcept
{
	return m_impl->manager->TabCount();
}

std::size_t CTerminalTool::VisiblePaneCount() const noexcept
{
	return m_impl->closed ? 0 : m_impl->panes.size();
}

bool CTerminalTool::HasTerminalTabsList() const noexcept
{
	return !m_impl->closed && !IsEmptyRect(m_impl->paneLayout.tabsBounds);
}

RECT CTerminalTool::TerminalTabsBounds() const noexcept
{
	return m_impl->closed ? RECT{} : m_impl->paneLayout.tabsBounds;
}

bool CTerminalTool::HasStartedAnySession() const noexcept
{
	return m_impl->manager->HasStartedAnySession();
}

bool CTerminalTool::HasCreatedRenderer() const noexcept
{
	return !m_impl->panes.empty();
}

std::vector<TerminalTabProjectionRecord> CTerminalTool::TerminalProjectionSnapshot() const
{
	return m_impl->manager->ProjectionSnapshot();
}

std::size_t CTerminalTool::WorkspaceRegistryEntryCount() const noexcept
{
	return m_impl && !m_impl->closed ? m_impl->workspaceRegistry.size() : 0;
}

bool CTerminalTool::HasWorkspaceProjection( std::wstring_view workspaceIdentityKey ) const noexcept
{
	if( !m_impl || m_impl->closed ) return false;
	return std::any_of(m_impl->workspaceRegistry.begin(), m_impl->workspaceRegistry.end(),
		[workspaceIdentityKey](const auto& entry) {
			return std::wstring_view(entry.first) == workspaceIdentityKey;
		});
}

void CTerminalTool::DiscardWorkspaceProjection( std::wstring_view workspaceIdentityKey ) noexcept
{
	if( !m_impl || m_impl->closed || workspaceIdentityKey.empty()
		|| workspaceIdentityKey == m_impl->currentWorkspaceIdentityKey ) return;
	const auto found = m_impl->workspaceRegistry.find(std::wstring(workspaceIdentityKey));
	if( found == m_impl->workspaceRegistry.end()
		|| !found->second.tabs.tabs.empty() || found->second.groupLayout.GroupCount() != 0 ) return;
	m_impl->workspaceRegistry.erase(found);
}

HWND CTerminalTool::GetHwnd() const noexcept
{
	return m_impl->window;
}

bool CTerminalTool::IsTerminalInstanceMessage( const MSG& message ) const noexcept
{
	return m_impl && m_impl->IsPaneMessage(message);
}

LRESULT CALLBACK CTerminalTool::WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	if( message == WM_NCCREATE ) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* impl = static_cast<Impl*>(create->lpCreateParams);
		if( impl ) {
			impl->window = window;
			::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
		}
	}
	auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if( impl ) {
		if( message == WM_NCDESTROY ) {
			{
				const std::lock_guard lock(impl->gate->mutex);
				if( impl->gate->window == window ) impl->gate->window = nullptr;
			}
			impl->window = nullptr;
			::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return impl->HandleMessage(message, wParam, lParam);
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace terminal
