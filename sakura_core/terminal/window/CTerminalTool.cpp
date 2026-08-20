/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalTool.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

#include "terminal/PowerShellLocator.h"
#include "terminal/input/TerminalShortcutPreset.h"
#include "terminal/window/TerminalHeaderLayout.h"
#include "terminal/window/TerminalPaneLayout.h"
#include "terminal/window/TerminalTabPresentation.h"
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
#include <utility>
#include <windowsx.h>

namespace terminal {
namespace {

constexpr wchar_t kTerminalToolWindowClass[] = L"SakuraNativeTerminalTool";
constexpr unsigned int kDefaultDpi = 96;
constexpr int kTabHeightDip = 30;
constexpr int kTerminalTabsRowHeightDip = 24;
constexpr UINT kOutputAvailableMessage = WM_APP + 0x3a1;
constexpr UINT kStateChangedMessage = WM_APP + 0x3a2;
constexpr UINT_PTR kOutputFrameTimer = 0x5343;
constexpr UINT_PTR kSynchronizedOutputTimer = 0x5344;
constexpr UINT_PTR kProtocolInputRetryTimer = 0x5346;
constexpr UINT kOutputFrameMilliseconds = 16;
constexpr UINT kProtocolInputRetryMilliseconds = 30;
constexpr ULONGLONG kSynchronizedOutputMaximumMilliseconds = 100;
constexpr UINT kCommandNewTerminal = 1;
constexpr UINT kCommandRestartTerminal = 2;
constexpr UINT kCommandCloseTerminal = 3;
constexpr UINT kCommandRedetectPowerShell = 4;
constexpr UINT kCommandSplitTerminal = 5;
constexpr UINT kCommandCloseSplit = 6;
constexpr UINT kCommandSplitTerminalDown = 7;
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
};

class DefaultLaunchResolver final {
public:
	DefaultLaunchResolver()
		: locator(provider)
		, catalog(locator)
	{
	}

	std::optional<TerminalLaunchOptions> Resolve( TerminalSize size, std::wstring_view workingDirectory )
	{
		const auto profile = catalog.ResolveProfile();
		if( !profile ) return std::nullopt;
		TerminalLaunchOptions options;
		options.executablePath = profile->path;
		options.arguments.emplace_back(L"-NoLogo");
		options.workingDirectory.assign(workingDirectory);
		options.initialSize = size;
		return options;
	}

	void Redetect() noexcept
	{
		catalog.Redetect();
	}

	std::vector<TerminalProfile> Profiles() { return catalog.Profiles(); }
	std::optional<TerminalProfile> SelectedProfile() { return catalog.ResolveProfile(); }
	bool SelectProfile( std::wstring_view path ) { return catalog.SelectProfile(path); }

private:
	NativePowerShellLocatorProvider provider;
	PowerShellLocator locator;
	TerminalProfileCatalog catalog;
};

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
		if( !dependencies.createSession ) {
			dependencies.createSession = [](TerminalSessionCallbacks callbacks) {
				return std::make_unique<CTerminalSession>(CreateConPtyTerminalBackend(), std::move(callbacks));
			};
		}
		if( !dependencies.resolveLaunch ) {
			usesDefaultResolver = true;
			dependencies.resolveLaunch = [this](TerminalSize size, std::wstring_view workingDirectory) {
				EnsureDefaultResolver();
				return defaultResolver->Resolve(size, workingDirectory);
			};
		}
		const std::weak_ptr<NotificationGate> weakGate = gate;
		manager = std::make_unique<TerminalTabManager>(std::move(dependencies), [weakGate](const TerminalTabEvent& event) {
			const auto gate = weakGate.lock();
			if( !gate ) return;
			HWND target = nullptr;
			{
				const std::lock_guard lock(gate->mutex);
				if( gate->alive ) target = gate->window;
			}
			if( target ) ::PostMessageW(target, event.kind == TerminalTabEventKind::OutputAvailable ? kOutputAvailableMessage : kStateChangedMessage,
				static_cast<WPARAM>(event.tabId), 0);
		});
	}

	std::shared_ptr<NotificationGate> gate;
	std::unique_ptr<DefaultLaunchResolver> defaultResolver;
	std::unique_ptr<TerminalTabManager> manager;

	// This is deliberately the same flat model VS Code uses for a terminal
	// group: an ordered list of panes and their relative widths.  A terminal
	// session belongs to exactly one group; only the selected group owns native
	// viewport HWNDs at a time.
	struct TerminalPane final {
		std::uint64_t tabId{};
		std::unique_ptr<CTerminalWnd> window;
		bool needsFullRepaint{};
		ULONGLONG synchronizedOutputSince{};
	};

	struct TerminalPaneGroup final {
		std::vector<std::uint64_t> tabIds;
		std::vector<int> weights;
		TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	};

	std::vector<TerminalPaneGroup> paneGroups;
	std::optional<std::size_t> activePaneGroup;
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
	bool usesDefaultResolver{};
	bool outputFrameScheduled{};
	bool protocolInputRetryScheduled{};
	std::vector<std::uint64_t> pendingOutputTabs;
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

	void EnsureDefaultResolver()
	{
		if( !defaultResolver ) defaultResolver = std::make_unique<DefaultLaunchResolver>();
	}

	TerminalPaneGroup* ActiveGroup() noexcept
	{
		if( !activePaneGroup || *activePaneGroup >= paneGroups.size() ) return nullptr;
		return &paneGroups[*activePaneGroup];
	}

	const TerminalPaneGroup* ActiveGroup() const noexcept
	{
		if( !activePaneGroup || *activePaneGroup >= paneGroups.size() ) return nullptr;
		return &paneGroups[*activePaneGroup];
	}

	std::optional<std::size_t> FindGroup( std::uint64_t tabId ) const noexcept
	{
		for( std::size_t index = 0; index < paneGroups.size(); ++index ) {
			const auto& group = paneGroups[index];
			if( std::find(group.tabIds.begin(), group.tabIds.end(), tabId) != group.tabIds.end() ) return index;
		}
		return std::nullopt;
	}

	void NormalizeWeights( TerminalPaneGroup& group ) noexcept
	{
		if( group.tabIds.empty() ) {
			group.weights.clear();
			return;
		}
		const bool valid = group.weights.size() == group.tabIds.size()
			&& std::all_of(group.weights.begin(), group.weights.end(), [](int weight) { return weight > 0; });
		if( !valid ) group.weights.assign(group.tabIds.size(), 1000);
	}

	std::size_t EnsureGroupForTab( std::uint64_t tabId )
	{
		if( const auto existing = FindGroup(tabId) ) return *existing;
		paneGroups.push_back({ { tabId }, { 1000 }, TerminalPaneOrientation::Horizontal });
		activePaneGroup = paneGroups.size() - 1;
		return *activePaneGroup;
	}

	void RemoveTabFromGroups( std::uint64_t tabId ) noexcept
	{
		for( std::size_t index = 0; index < paneGroups.size(); ++index ) {
			auto& group = paneGroups[index];
			const auto found = std::find(group.tabIds.begin(), group.tabIds.end(), tabId);
			if( found == group.tabIds.end() ) continue;
			const auto paneIndex = static_cast<std::size_t>(found - group.tabIds.begin());
			group.tabIds.erase(found);
			if( paneIndex < group.weights.size() ) group.weights.erase(group.weights.begin() + paneIndex);
			if( !group.tabIds.empty() ) {
				NormalizeWeights(group);
				return;
			}

			if( activePaneGroup ) {
				if( *activePaneGroup == index ) {
					if( index + 1 < paneGroups.size() ) *activePaneGroup = index;
					else if( index > 0 ) *activePaneGroup = index - 1;
					else activePaneGroup.reset();
				} else if( *activePaneGroup > index ) {
					--*activePaneGroup;
				}
			}
			paneGroups.erase(paneGroups.begin() + index);
			return;
		}
	}

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

	void DestroyPaneRenderers() noexcept
	{
		// Destroying the focused child transfers focus to this host synchronously.
		// Do not let the host's WM_SETFOCUS re-enter LayoutChildren against a child
		// that is midway through DestroyWindow.
		const bool wasDestroying = destroyingPaneRenderers;
		destroyingPaneRenderers = true;
		for( auto& pane : panes ) {
			if( !pane.window ) continue;
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
		auto candidate = std::make_unique<CTerminalWnd>();
		if( !candidate->Create(window, instance) ) return false;
		candidate->SetInputSink([this, tabId](std::span<const std::uint8_t> bytes) {
			return tabId == 0 ? TerminalQueueInputResult::NotRunning : manager->QueueInput(tabId, bytes);
		});
		candidate->SetResizeSink([this, tabId](TerminalSize size) {
			if( tabId != 0 ) static_cast<void>(manager->ResizeTab(tabId, size));
		});
		candidate->SetFocusSink([this, tabId] {
			if( tabId != 0 ) OnPaneFocused(tabId);
		});
		candidate->SetPalette(palette);
		candidate->SetInputAdapter(unboundViewport ? nullptr : manager->InputAdapter(tabId));
		candidate->SetModel(unboundViewport ? nullptr : manager->Model(tabId));
		panes.push_back({ tabId, std::move(candidate), true, 0 });
		return true;
	}

	bool RebuildPaneRenderers()
	{
		DestroyPaneRenderers();
		const auto* group = ActiveGroup();
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
		if( !ActiveGroup() ) {
			if( const auto activeId = manager->ActiveTabId() ) EnsureGroupForTab(*activeId);
		}
		const auto* group = ActiveGroup();
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
			pane.needsFullRepaint = true;
		}
	}

	TerminalHeaderLayout HeaderLayout() const noexcept
	{
		RECT client{};
		if( window ) ::GetClientRect(window, &client);
		return CalculateTerminalHeaderLayout(client, dpi, panelActions.renderPanelActions,
			panelActions.renderHeader ? 30 : 0);
	}

	void InvalidateTabs()
	{
		if( headerHost && hostedHeaderBounds.right > hostedHeaderBounds.left
			&& hostedHeaderBounds.bottom > hostedHeaderBounds.top ) {
			::InvalidateRect(headerHost, &hostedHeaderBounds, FALSE);
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
		if( const auto activeId = manager->ActiveTabId(); activeId && FindGroup(*activeId) == activePaneGroup ) return activeId;
		if( const auto* group = ActiveGroup(); group && !group->tabIds.empty() ) return group->tabIds.front();
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

	std::wstring HeaderProfileLabel()
	{
		if( !ShouldShowTerminalTabPolicy(tabPresentationSettings.showActiveTerminal,
			manager->TabCount(), IsTerminalTabsNarrow()) ) return {};
		const auto tabs = manager->Snapshot();
		const auto focused = FocusedTabId();
		const auto found = std::find_if(tabs.begin(), tabs.end(), [focused](const auto& tab) {
			return focused && tab.id == *focused;
		});
		if( found != tabs.end() && !found->profileLabel.empty() ) return found->profileLabel;
		if( defaultResolver ) {
			if( const auto selected = defaultResolver->SelectedProfile() ) {
				const auto stem = std::filesystem::path(selected->path).stem().wstring();
				if( !stem.empty() ) return stem;
			}
		}
		return L"pwsh";
	}

	bool IsHeaderTargetEnabled( TerminalHeaderTarget target ) const noexcept
	{
		if( target == TerminalHeaderTarget::Split
			|| target == TerminalHeaderTarget::Kill
			|| target == TerminalHeaderTarget::More ) {
			if( !ShouldShowTerminalTabPolicy(tabPresentationSettings.showActions,
				manager->TabCount(), IsTerminalTabsNarrow()) ) return false;
		}
		switch( target ) {
		case TerminalHeaderTarget::Kill:
			return FocusedTabId().has_value();
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
		auto* group = ActiveGroup();
		if( !group || dividerIndex + 1 >= group->tabIds.size() || dividerIndex + 1 >= paneLayout.panes.size() ) return;
		NormalizeWeights(*group);
		const RECT& firstPane = paneLayout.panes[dividerIndex];
		const RECT& secondPane = paneLayout.panes[dividerIndex + 1];
		const bool vertical = group->orientation == TerminalPaneOrientation::Vertical;
		const LONG combined = vertical
			? std::max<LONG>(1, secondPane.bottom - firstPane.top - (secondPane.top - firstPane.bottom))
			: std::max<LONG>(1, secondPane.right - firstPane.left - (secondPane.left - firstPane.right));
		const LONG minimum = std::min<LONG>(ScaleDip(80, dpi), combined / 2);
		const LONG firstExtent = vertical
			? std::clamp<LONG>(y - firstPane.top, minimum, combined - minimum)
			: std::clamp<LONG>(x - firstPane.left, minimum, combined - minimum);
		const int combinedWeight = std::max(2, group->weights[dividerIndex] + group->weights[dividerIndex + 1]);
		const int firstWeight = std::clamp(static_cast<int>((static_cast<long long>(combinedWeight) * firstExtent) / combined), 1, combinedWeight - 1);
		group->weights[dividerIndex] = firstWeight;
		group->weights[dividerIndex + 1] = combinedWeight - firstWeight;
		LayoutChildren();
	}

	void LayoutChildren()
	{
		if( !window ) return;
		paneLayout = {};
		if( !EnsureTerminalWindow() ) return;
		auto* group = ActiveGroup();
		const RECT content = ContentRect();
		if( !group ) {
			if( panes.size() == 1 && panes.front().window ) {
				paneLayout = CalculateTerminalPaneLayout({ content, dpi, 1, {}, false });
				panes.front().window->Layout(paneLayout.panes.front(), dpi);
			}
			return;
		}
		if( panes.empty() ) return;
		NormalizeWeights(*group);
		const bool showTabs = ShouldShowTerminalTabs(tabPresentationSettings,
			manager->TabCount(), paneGroups.size());
		paneLayout = CalculateTerminalPaneLayout({ content, dpi, group->tabIds.size(),
			std::span<const int>(group->weights), showTabs, group->orientation,
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
			includePanelActions, headerHeightDip);
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
		const auto tabs = manager->Snapshot();
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
		const auto tabs = manager->Snapshot();
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
		const auto tabs = manager->Snapshot();
		const auto first = std::min(terminalTabsFirstVisible, tabs.size());
		const auto focused = FocusedTabId();
		const int markerWidth = std::max(2, ScaleDip(3, dpi));
		const int horizontalPadding = ScaleDip(8, dpi);
		for( std::size_t index = first; index < tabs.size(); ++index ) {
			const auto& tab = tabs[index];
			const RECT row = TerminalTabRow(index);
			if( IsEmptyRect(row) ) break;
			const bool selected = focused && tab.id == *focused;
			if( selected ) FillSolidRect(dc, row, palette.raised.ToColorRef());

			RECT marker = row;
			marker.right = std::min<LONG>(row.right, marker.left + markerWidth);
			const auto group = FindGroup(tab.id);
			const bool split = group && paneGroups[*group].tabIds.size() > 1;
			FillSolidRect(dc, marker, (selected ? palette.accent : (split ? palette.secondaryText : palette.border)).ToColorRef());

			RECT label = row;
			label.left = std::min<LONG>(label.right, label.left + horizontalPadding);
			label.right = std::max<LONG>(label.left, label.right - horizontalPadding);
			const auto presentation = TabPresentation(tab);
			std::wstring title = presentation.title;
			if( title.empty() ) title = tab.profileLabel;
			if( title.empty() ) title = LS(STR_TERMINAL_FALLBACK_LABEL);
			const TerminalTabRowLayoutInput geometryInput {
				{ static_cast<int>(label.left), static_cast<int>(label.top),
					static_cast<int>(label.right), static_cast<int>(label.bottom) }, dpi, split, true,
				!presentation.description.empty(), tab.state == TerminalSessionState::Failed };
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
				::SetTextColor(dc, tab.state == TerminalSessionState::Failed
					? palette.danger.ToColorRef()
					: (selected ? palette.primaryText : palette.secondaryText).ToColorRef());
				::DrawTextW(dc, title.c_str(), -1, &titleRect,
					DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			}
			if( geometry.Description().Width() > 0 && !presentation.description.empty() ) {
				RECT descriptionRect = toNativeRect(geometry.Description());
				::SetTextColor(dc, palette.descriptionText.ToColorRef());
				::DrawTextW(dc, presentation.description.c_str(), -1, &descriptionRect,
					DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			}
			if( geometry.Status().Width() > 0 && tab.state == TerminalSessionState::Failed ) {
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
		}
		if( auto* pane = FindPane(tabId); pane && pane->window ) {
			PaintTerminalOutput(*pane->window, manager->Model(tabId), result,
				pane->needsFullRepaint, pane->synchronizedOutputSince);
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
		if( std::find(pendingOutputTabs.begin(), pendingOutputTabs.end(), tabId) == pendingOutputTabs.end() ) {
			pendingOutputTabs.push_back(tabId);
		}
		if( outputFrameScheduled || window == nullptr ) return;

		// Windows Terminal's Renderer::NotifyPaintFrame wakes its render thread on
		// the leading edge instead of waiting for a periodic timer. Mirror that
		// interactive contract here so a key echo or short command response is not
		// held behind low-priority WM_TIMER dispatch. Sakura still needs a trailing
		// frame gate because parsing runs on the UI thread: notifications arriving
		// during the next 16 ms are coalesced by DrainOutputFrame().
		outputFrameScheduled = ::SetTimer(window, kOutputFrameTimer, kOutputFrameMilliseconds, nullptr) != 0;
		auto pending = std::move(pendingOutputTabs);
		pendingOutputTabs.clear();
		for( const auto pendingId : pending ) HandleOutput(pendingId);
	}

	void DrainOutputFrame()
	{
		if( window != nullptr ) ::KillTimer(window, kOutputFrameTimer);
		outputFrameScheduled = false;
		auto pending = std::move(pendingOutputTabs);
		pendingOutputTabs.clear();
		for( const auto tabId : pending ) HandleOutput(tabId);
	}

	void ShowContextMenu( int x, int y, HWND owner = nullptr )
	{
		const HMENU menu = ::CreatePopupMenu();
		if( !menu ) return;
		const HWND menuOwner = owner ? owner : window;
		const auto focusedId = FocusedTabId();
		const auto tabs = manager->Snapshot();
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
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandRestartTerminal, LS(STR_TERMINAL_RESTART));
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandCloseTerminal, LS(STR_TERMINAL_CLOSE));
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandSplitTerminal, LS(STR_TERMINAL_SPLIT));
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandSplitTerminalDown, LS(STR_TERMINAL_SPLIT_DOWN));
		::AppendMenuW(menu, MF_STRING | (HasTerminalSplit() ? 0 : MF_GRAYED), kCommandCloseSplit, LS(STR_TERMINAL_CLOSE_SPLIT));
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
		if( defaultResolver ) {
			const HMENU profilesMenu = ::CreatePopupMenu();
			if( profilesMenu ) {
				profileCommandPaths.clear();
				const auto selected = defaultResolver->SelectedProfile();
				for( const auto& profile : defaultResolver->Profiles() ) {
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
		::TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, menuOwner, nullptr);
		::DestroyMenu(menu);
	}

	void ShowHeaderMenu( TerminalHeaderTarget target, HWND owner = nullptr,
		const RECT* externalBounds = nullptr, unsigned int externalDpi = 0 )
	{
		const HWND source = owner ? owner : window;
		if( !source ) return;
		const TerminalHeaderLayout layout = externalBounds
			? CalculateTerminalHeaderLayout(*externalBounds, externalDpi, false, 34)
			: HeaderLayout();
		const RECT action = layout.RectFor(target);
		POINT point{ action.left, action.bottom };
		::ClientToScreen(source, &point);
		ShowContextMenu(point.x, point.y, source);
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
		const auto layout = CalculateTerminalHeaderLayout(headerBounds, hostedHeaderDpi, false, 34);
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
				ShowContextMenu(screenPoint.x, screenPoint.y, headerHost);
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
				const bool vertical = ActiveGroup()
					&& ActiveGroup()->orientation == TerminalPaneOrientation::Vertical;
				::SetCursor(::LoadCursor(nullptr, vertical ? IDC_SIZENS : IDC_SIZEWE));
				return TRUE;
			}
			break;
		}
		case WM_KEYDOWN:
			if( terminalTabsFocused && (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_RETURN) ) {
				const auto tabs = manager->Snapshot();
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
		case WM_COMMAND:
			if( const UINT command = LOWORD(wParam);
				command >= kCommandProfileFirst && command < kCommandProfileFirst + profileCommandPaths.size() ) {
				if( defaultResolver ) {
					const auto index = static_cast<std::size_t>(command - kCommandProfileFirst);
					(void)defaultResolver->SelectProfile(profileCommandPaths[index]);
				}
				return 0;
			}
			if( const UINT command = LOWORD(wParam);
				command >= kCommandShortcutPresetFirst
				&& command < kCommandShortcutPresetFirst + kShortcutPresets.size() ) {
				const auto index = static_cast<std::size_t>(command - kCommandShortcutPresetFirst);
				ApplyShortcutPreset(kShortcutPresets[index], true);
				return 0;
			}
			if( const UINT command = LOWORD(wParam);
				command >= kCommandTabFirst && command < kCommandTabFirst + tabCommandIds.size() ) {
				const auto index = static_cast<std::size_t>(command - kCommandTabFirst);
				static_cast<void>(SelectTerminal(tabCommandIds[index]));
				return 0;
			}
			switch( LOWORD(wParam) ) {
			case kCommandNewTerminal: AddTerminal(); break;
			case kCommandRestartTerminal: if( const auto id = FocusedTabId() ) RestartTerminal(*id); break;
			case kCommandCloseTerminal: if( const auto id = FocusedTabId() ) DeleteTerminal(*id); break;
			case kCommandRedetectPowerShell: if( defaultResolver ) defaultResolver->Redetect(); break;
			case kCommandSplitTerminal: SplitTerminalRight(); break;
			case kCommandSplitTerminalDown: SplitTerminalDown(); break;
			case kCommandCloseSplit: CloseTerminalSplit(); break;
			default: break;
			}
			return 0;
		case kOutputAvailableMessage:
			ScheduleOutputFrame(static_cast<std::uint64_t>(wParam));
			return 0;
		case WM_TIMER:
			if( wParam == kOutputFrameTimer ) {
				DrainOutputFrame();
				return 0;
			}
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
			InvalidateTabs();
			InvalidateTerminalTabs();
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return ::DefWindowProcW(window, message, wParam, lParam);
	}

	std::optional<std::uint64_t> AddTerminal()
	{
		// New Terminal creates a separate terminal group.  A group is the unit
		// shown in the workbench panel; only Split Terminal adds another pane to
		// the currently active group.  Keeping this distinction also means that
		// the right-hand terminal list can flatten all groups without making a
		// newly-created terminal unexpectedly resize the active pane.
		const auto id = manager->AddTab(CurrentSize(), workingDirectory);
		if( !id ) return std::nullopt;
		paneGroups.push_back({ { *id }, { 1000 }, TerminalPaneOrientation::Horizontal });
		const auto group = paneGroups.size() - 1;
		activePaneGroup = group;
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
		const auto group = EnsureGroupForTab(*id);
		activePaneGroup = group;
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
		const auto group = FindGroup(tabId);
		if( !group ) return false;
		if( !manager->SelectTab(tabId) ) return false;
		const bool groupChanged = activePaneGroup != group;
		activePaneGroup = group;
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
		DestroyPaneRenderers();
		if( !manager->DeleteTab(tabId) ) {
			if( window ) static_cast<void>(RebuildPaneRenderers());
			return false;
		}
		RemoveTabFromGroups(tabId);
		// Preserve the tab manager's deterministic successor: it selects the next
		// tab in tab order, or the preceding tab when the deleted terminal was last.
		// Re-selecting a wrapped pane within this split group makes repeated Close
		// Terminal Split jump back to the first pane instead of closing the focused
		// neighbor.
		if( const auto activeId = manager->ActiveTabId() ) activePaneGroup = FindGroup(*activeId);
		if( window ) {
			static_cast<void>(RebuildPaneRenderers());
			LayoutChildren();
		}
		InvalidateTabs();
		InvalidateTerminalTabs();
		return true;
	}

	TerminalWorkspaceResetResult ResetForWorkspace( std::wstring nextWorkingDirectory, bool recreateSession )
	{
		TerminalWorkspaceResetResult result;
		if( closed ) return result;
		workingDirectory = std::move(nextWorkingDirectory);

		if( window ) {
			::KillTimer(window, kOutputFrameTimer);
			::KillTimer(window, kSynchronizedOutputTimer);
			::KillTimer(window, kProtocolInputRetryTimer);
		}
		outputFrameScheduled = false;
		pendingOutputTabs.clear();
		for( auto& pane : panes ) {
			if( pane.window ) pane.window->ResetSessionInputState();
		}
		DestroyPaneRenderers();
		paneGroups.clear();
		activePaneGroup.reset();
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

	bool SplitTerminal( TerminalPaneOrientation orientation )
	{
		if( closed ) return false;
		auto primaryId = FocusedTabId();
		if( !primaryId ) {
			primaryId = manager->Activate(CurrentSize(), workingDirectory);
			if( !primaryId ) return false;
		}
		const auto groupIndex = EnsureGroupForTab(*primaryId);
		activePaneGroup = groupIndex;
		const auto originalActiveGroup = activePaneGroup;
		const auto originalActiveTab = manager->ActiveTabId();
		auto* group = ActiveGroup();
		if( !group ) return false;
		NormalizeWeights(*group);
		const auto originalGroup = *group;
		const auto newId = manager->AddTab(SizeForTab(*primaryId), workingDirectory);
		if( !newId ) return false;
		group = ActiveGroup();
		if( !group ) {
			static_cast<void>(manager->DeleteTab(*newId));
			return false;
		}
		const auto found = std::find(group->tabIds.begin(), group->tabIds.end(), *primaryId);
		if( found == group->tabIds.end() ) {
			static_cast<void>(manager->DeleteTab(*newId));
			return false;
		}
		const auto index = static_cast<std::size_t>(found - group->tabIds.begin());
		const int originalWeight = std::max(2, group->weights[index]);
		group->weights[index] = originalWeight / 2;
		group->tabIds.insert(found + 1, *newId);
		group->weights.insert(group->weights.begin() + index + 1, originalWeight - group->weights[index]);
		// A group is single-axis. Requesting the orthogonal split reorients every
		// pane rather than inventing a nested 2D tree VS Code's Panel also lacks.
		group->orientation = orientation;
		if( window ) {
			if( !RebuildPaneRenderers() ) {
				// The renderer rebuild can fail after the manager has created a live
				// session.  Restore the group before returning so a failed UI operation
				// never leaves an unowned terminal or a phantom split in the model.
				if( manager->DeleteTab(*newId) ) {
					paneGroups[groupIndex] = originalGroup;
					activePaneGroup = originalActiveGroup;
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
		const auto* group = ActiveGroup();
		return group && group->tabIds.size() > 1;
	}

	bool CloseTerminalSplit()
	{
		const auto focused = FocusedTabId();
		return HasTerminalSplit() && focused && DeleteTerminal(*focused);
	}

	bool FocusRelativePane( int direction )
	{
		const auto* group = ActiveGroup();
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
		if( paneGroups.empty() ) return false;
		const auto current = activePaneGroup.value_or(0);
		const auto count = paneGroups.size();
		const auto next = direction < 0 ? (current + count - 1) % count : (current + 1) % count;
		const auto& group = paneGroups[next];
		return !group.tabIds.empty() && SelectTerminal(group.tabIds.front(), true);
	}

	//! tmux windows and Screen windows are terminal groups here, not panes, so a
	//! digit chord selects the group's first pane exactly as its list order shows.
	bool SelectTerminalGroup( std::size_t index )
	{
		if( index >= paneGroups.size() ) return false;
		const auto& group = paneGroups[index];
		return !group.tabIds.empty() && SelectTerminal(group.tabIds.front(), true);
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
		case TerminalPresetAction::ClosePane: return CloseTerminalSplit();
		case TerminalPresetAction::CloseTerminal: {
			const auto id = FocusedTabId();
			return id && DeleteTerminal(*id);
		}
		case TerminalPresetAction::NextPane: return FocusRelativePane(1);
		case TerminalPresetAction::PreviousPane: return FocusRelativePane(-1);
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
	m_impl->closed = true;
	{
		const std::lock_guard lock(m_impl->gate->mutex);
		m_impl->gate->alive = false;
		m_impl->gate->window = nullptr;
	}
	if( m_impl->window ) ::KillTimer(m_impl->window, kOutputFrameTimer);
	if( m_impl->window ) ::KillTimer(m_impl->window, kSynchronizedOutputTimer);
	if( m_impl->window ) ::KillTimer(m_impl->window, kProtocolInputRetryTimer);
	m_impl->outputFrameScheduled = false;
	m_impl->pendingOutputTabs.clear();
	m_impl->DestroyPaneRenderers();
	m_impl->paneGroups.clear();
	m_impl->activePaneGroup.reset();
	m_impl->manager->Close();
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

TerminalWorkspaceResetResult CTerminalTool::ResetForWorkspace(
	std::wstring workingDirectory, bool recreateSession )
{
	return m_impl->ResetForWorkspace(std::move(workingDirectory), recreateSession);
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

void CTerminalTool::SetShortcutPreset( TerminalShortcutPreset preset )
{
	m_impl->ApplyShortcutPreset(preset, false);
}

void CTerminalTool::SetTabPresentationSettings( TerminalTabPresentationSettings settings )
{
	if( m_impl->closed ) return;
	m_impl->SetTabPresentationSettings(std::move(settings));
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
	if( !m_impl->closed && m_impl->usesDefaultResolver ) {
		m_impl->EnsureDefaultResolver();
		m_impl->defaultResolver->Redetect();
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

bool CTerminalTool::HasTerminalSplit() const noexcept
{
	return !m_impl->closed && m_impl->HasTerminalSplit();
}

TerminalPaneOrientation CTerminalTool::ActivePaneOrientation() const noexcept
{
	if( m_impl->closed ) return TerminalPaneOrientation::Horizontal;
	const auto* group = m_impl->ActiveGroup();
	return group ? group->orientation : TerminalPaneOrientation::Horizontal;
}

std::vector<TerminalTabSnapshot> CTerminalTool::Tabs() const
{
	return m_impl->manager->Snapshot();
}

std::optional<std::uint64_t> CTerminalTool::ActiveTerminalId() const noexcept
{
	return m_impl->manager->ActiveTabId();
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

HWND CTerminalTool::GetHwnd() const noexcept
{
	return m_impl->window;
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
