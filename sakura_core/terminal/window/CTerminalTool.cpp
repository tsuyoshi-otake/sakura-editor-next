/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalTool.h"

#include "terminal/PowerShellLocator.h"
#include "terminal/window/CTerminalWnd.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <utility>
#include <windowsx.h>

namespace terminal {
namespace {

constexpr wchar_t kTerminalToolWindowClass[] = L"SakuraNativeTerminalTool";
constexpr unsigned int kDefaultDpi = 96;
constexpr int kTabHeightDip = 30;
constexpr int kMinimumTabWidthDip = 84;
constexpr int kMaximumTabWidthDip = 180;
constexpr int kAddButtonWidthDip = 32;
constexpr UINT kOutputAvailableMessage = WM_APP + 0x3a1;
constexpr UINT kStateChangedMessage = WM_APP + 0x3a2;
constexpr UINT kCommandNewTerminal = 1;
constexpr UINT kCommandRestartTerminal = 2;
constexpr UINT kCommandCloseTerminal = 3;
constexpr UINT kCommandRedetectPowerShell = 4;
constexpr UINT kCommandProfileFirst = 1000;

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
	if( profile.channel == TerminalChannel::Preview ) label += L" Preview";
	else if( profile.channel == TerminalChannel::Legacy ) label += L" (Windows PowerShell)";
	label += L"  —  ";
	label += profile.path;
	return label;
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
	std::unique_ptr<CTerminalWnd> terminalWindow;
	HWND window{};
	HINSTANCE instance{};
	unsigned int dpi{ kDefaultDpi };
	RECT bounds{};
	std::wstring workingDirectory;
	bool active{};
	bool closed{};
	bool usesDefaultResolver{};
	bool needsFullTerminalRepaint{};
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	std::vector<std::wstring> profileCommandPaths;

	void EnsureDefaultResolver()
	{
		if( !defaultResolver ) defaultResolver = std::make_unique<DefaultLaunchResolver>();
	}

	bool EnsureTerminalWindow()
	{
		if( terminalWindow ) return true;
		if( !window || !instance || closed ) return false;

		auto candidate = std::make_unique<CTerminalWnd>();
		if( !candidate->Create(window, instance) ) return false;
		candidate->SetInputSink([this](std::span<const std::uint8_t> bytes) {
			static_cast<void>(manager->QueueActiveInput(bytes));
		});
		candidate->SetResizeSink([this](TerminalSize size) { manager->Resize(size); });
		candidate->SetPalette(palette);
		terminalWindow = std::move(candidate);
		BindActiveModel();
		for( const auto& tab : manager->Snapshot() ) HandleOutput(tab.id);
		return true;
	}

	bool HasVisibleContentBounds() const noexcept
	{
		return bounds.right > bounds.left && bounds.bottom > bounds.top;
	}

	TerminalSize CurrentSize() const noexcept
	{
		return terminalWindow ? terminalWindow->GetTerminalSize() : TerminalSize{ 120, 30 };
	}

	void BindActiveModel()
	{
		if( terminalWindow ) {
			terminalWindow->SetInputAdapter(manager->ActiveInputAdapter());
			terminalWindow->SetModel(manager->ActiveModel());
			needsFullTerminalRepaint = true;
		}
	}

	void InvalidateTabs()
	{
		if( window ) {
			RECT client{};
			::GetClientRect(window, &client);
			client.bottom = std::min<LONG>(client.bottom, ScaleDip(kTabHeightDip, dpi));
			::InvalidateRect(window, &client, FALSE);
		}
	}

	int TabWidth( std::size_t count, int clientWidth ) const noexcept
	{
		if( count == 0 ) return ScaleDip(kMinimumTabWidthDip, dpi);
		const auto available = std::max(0, clientWidth - ScaleDip(kAddButtonWidthDip, dpi));
		return std::clamp(available / static_cast<int>(count), ScaleDip(kMinimumTabWidthDip, dpi), ScaleDip(kMaximumTabWidthDip, dpi));
	}

	std::optional<std::uint64_t> HitTestTab( int x, bool* closeButton = nullptr ) const
	{
		const auto tabs = manager->Snapshot();
		if( !window || tabs.empty() ) return std::nullopt;
		RECT client{};
		::GetClientRect(window, &client);
		const auto width = TabWidth(tabs.size(), client.right - client.left);
		if( x < 0 || x >= width * static_cast<int>(tabs.size()) ) return std::nullopt;
		const auto index = static_cast<std::size_t>(x / width);
		if( closeButton ) *closeButton = x % width >= width - ScaleDip(24, dpi);
		return tabs[index].id;
	}

	bool HitTestAdd( int x ) const
	{
		if( !window ) return false;
		const auto tabs = manager->Snapshot();
		RECT client{};
		::GetClientRect(window, &client);
		const auto left = TabWidth(tabs.size(), client.right - client.left) * static_cast<int>(tabs.size());
		return x >= left && x < left + ScaleDip(kAddButtonWidthDip, dpi);
	}

	void LayoutChildren()
	{
		if( !window || !terminalWindow ) return;
		RECT client{};
		::GetClientRect(window, &client);
		client.top = std::min(client.bottom, client.top + ScaleDip(kTabHeightDip, dpi));
		terminalWindow->Layout(client, dpi);
	}

	void Paint()
	{
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if( !dc ) return;
		RECT client{};
		::GetClientRect(window, &client);
		RECT tabStrip = client;
		tabStrip.bottom = std::min<LONG>(tabStrip.bottom, ScaleDip(kTabHeightDip, dpi));
		const int width = tabStrip.right - tabStrip.left;
		const int height = tabStrip.bottom - tabStrip.top;
		const HDC memory = ::CreateCompatibleDC(dc);
		const HBITMAP bitmap = memory && width > 0 && height > 0 ? ::CreateCompatibleBitmap(dc, width, height) : nullptr;
		if( !memory || !bitmap ) {
			if( bitmap ) ::DeleteObject(bitmap);
			if( memory ) ::DeleteDC(memory);
			::EndPaint(window, &paint);
			return;
		}
		const auto previousBitmap = ::SelectObject(memory, bitmap);
		const HBRUSH stripBrush = ::CreateSolidBrush(palette.panel.ToColorRef());
		::FillRect(memory, &tabStrip, stripBrush);
		::DeleteObject(stripBrush);
		::SetBkMode(memory, TRANSPARENT);
		::SetTextColor(memory, palette.primaryText.ToColorRef());
		const auto tabs = manager->Snapshot();
		const auto tabWidth = TabWidth(tabs.size(), width);
		for( std::size_t index = 0; index < tabs.size(); ++index ) {
			RECT tab{ static_cast<LONG>(index * tabWidth), 0, static_cast<LONG>((index + 1) * tabWidth), tabStrip.bottom };
			const HBRUSH tabBrush = ::CreateSolidBrush(tabs[index].active ? palette.raised.ToColorRef() : palette.panel.ToColorRef());
			::FillRect(memory, &tab, tabBrush);
			::DeleteObject(tabBrush);
			if( tabs[index].active ) {
				RECT accent = tab;
				accent.bottom = accent.top + ScaleDip(2, dpi);
				const HBRUSH accentBrush = ::CreateSolidBrush(palette.accent.ToColorRef());
				::FillRect(memory, &accent, accentBrush);
				::DeleteObject(accentBrush);
			}
			RECT label = tab;
			label.left += ScaleDip(10, dpi);
			label.right -= ScaleDip(25, dpi);
			if( tabs[index].state == TerminalSessionState::Failed ) ::SetTextColor(memory, RGB(241, 76, 76));
			else ::SetTextColor(memory, palette.primaryText.ToColorRef());
			::DrawTextW(memory, tabs[index].label.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			RECT close = tab;
			close.left = close.right - ScaleDip(24, dpi);
			::SetTextColor(memory, palette.secondaryText.ToColorRef());
			::DrawTextW(memory, L"\u00d7", 1, &close, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
		}
		RECT add{ static_cast<LONG>(tabs.size() * tabWidth), 0, static_cast<LONG>(tabs.size() * tabWidth + ScaleDip(kAddButtonWidthDip, dpi)), tabStrip.bottom };
		::SetTextColor(memory, palette.primaryText.ToColorRef());
		::DrawTextW(memory, L"+", 1, &add, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
		::BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
		::SelectObject(memory, previousBitmap);
		::DeleteObject(bitmap);
		::DeleteDC(memory);
		::EndPaint(window, &paint);
	}

	void HandleOutput( std::uint64_t tabId )
	{
		const auto result = manager->DrainOutput(tabId);
		if( !result.found ) return;
		if( result.titleChanged ) InvalidateTabs();
		if( result.active && terminalWindow ) {
			// The first output may arrive while the renderer is still transitioning
			// from its deferred 0x0 layout. Paint the complete viewport once, then
			// return to dirty-row invalidation for steady-state throughput.
			if( needsFullTerminalRepaint ) {
				terminalWindow->InvalidateAll();
				needsFullTerminalRepaint = false;
			} else {
				terminalWindow->InvalidateDirtyRows(result.dirtyRows);
			}
		}
	}

	void ShowContextMenu( int x, int y )
	{
		const HMENU menu = ::CreatePopupMenu();
		if( !menu ) return;
		::AppendMenuW(menu, MF_STRING, kCommandNewTerminal, L"New Terminal");
		const auto activeId = manager->ActiveTabId();
		::AppendMenuW(menu, MF_STRING | (activeId ? 0 : MF_GRAYED), kCommandRestartTerminal, L"Restart Terminal");
		::AppendMenuW(menu, MF_STRING | (activeId ? 0 : MF_GRAYED), kCommandCloseTerminal, L"Close Terminal");
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
					::AppendMenuW(profilesMenu, MF_STRING | MF_GRAYED, kCommandProfileFirst, L"No PowerShell profile found");
				}
				::AppendMenuW(profilesMenu, MF_SEPARATOR, 0, nullptr);
				::AppendMenuW(profilesMenu, MF_STRING, kCommandRedetectPowerShell, L"Redetect PowerShell");
				::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profilesMenu), L"PowerShell Profile");
			}
		}
		::TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, window, nullptr);
		::DestroyMenu(menu);
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
			if( terminalWindow ) terminalWindow->Focus();
			return 0;
		case WM_LBUTTONDOWN: {
			bool close = false;
			const auto tabId = HitTestTab(GET_X_LPARAM(lParam), &close);
			if( tabId ) {
				if( close ) DeleteTerminal(*tabId);
				else SelectTerminal(*tabId);
			} else if( HitTestAdd(GET_X_LPARAM(lParam)) ) AddTerminal();
			return 0;
		}
		case WM_MBUTTONDOWN:
			if( const auto tabId = HitTestTab(GET_X_LPARAM(lParam)) ) DeleteTerminal(*tabId);
			return 0;
		case WM_LBUTTONDBLCLK:
			if( const auto tabId = HitTestTab(GET_X_LPARAM(lParam)) ) RestartTerminal(*tabId);
			return 0;
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
			switch( LOWORD(wParam) ) {
			case kCommandNewTerminal: AddTerminal(); break;
			case kCommandRestartTerminal: if( const auto id = manager->ActiveTabId() ) RestartTerminal(*id); break;
			case kCommandCloseTerminal: if( const auto id = manager->ActiveTabId() ) DeleteTerminal(*id); break;
			case kCommandRedetectPowerShell: if( defaultResolver ) defaultResolver->Redetect(); break;
			default: break;
			}
			return 0;
		case kOutputAvailableMessage:
			HandleOutput(static_cast<std::uint64_t>(wParam));
			return 0;
		case kStateChangedMessage:
			InvalidateTabs();
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
	}

	std::optional<std::uint64_t> AddTerminal()
	{
		// Explicit creation is allowed to materialize the renderer even before a
		// WM_SIZE; a hidden panel never calls this path on its own.
		static_cast<void>(EnsureTerminalWindow());
		const auto id = manager->AddTab(CurrentSize(), workingDirectory);
		BindActiveModel();
		InvalidateTabs();
		if( active && terminalWindow ) terminalWindow->Focus();
		return id;
	}

	bool SelectTerminal( std::uint64_t tabId )
	{
		if( !manager->SelectTab(tabId) ) return false;
		BindActiveModel();
		InvalidateTabs();
		if( active && terminalWindow ) terminalWindow->Focus();
		return true;
	}

	bool RestartTerminal( std::uint64_t tabId )
	{
		static_cast<void>(EnsureTerminalWindow());
		const bool restarted = manager->RestartTab(tabId, CurrentSize(), workingDirectory);
		if( manager->ActiveTabId() == tabId ) BindActiveModel();
		InvalidateTabs();
		return restarted;
	}

	bool DeleteTerminal( std::uint64_t tabId )
	{
		if( !manager->DeleteTab(tabId) ) return false;
		BindActiveModel();
		InvalidateTabs();
		return true;
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
	::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top, std::max(0L, contentRect.right - contentRect.left),
		std::max(0L, contentRect.bottom - contentRect.top), SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
	if( m_impl->HasVisibleContentBounds() ) static_cast<void>(m_impl->EnsureTerminalWindow());
	m_impl->LayoutChildren();
	m_impl->InvalidateTabs();
}

void CTerminalTool::Activate()
{
	if( m_impl->closed ) return;
	m_impl->active = true;
	static_cast<void>(m_impl->EnsureTerminalWindow());
	static_cast<void>(m_impl->manager->Activate(m_impl->CurrentSize(), m_impl->workingDirectory));
	m_impl->BindActiveModel();
	m_impl->InvalidateTabs();
	if( m_impl->terminalWindow ) m_impl->terminalWindow->Focus();
}

void CTerminalTool::Deactivate()
{
	// Deliberately preserve every tab, session, pipe and parser while hidden.
	m_impl->active = false;
}

bool CTerminalTool::PreTranslateMessage( MSG& message )
{
	return !m_impl->closed && m_impl->terminalWindow && m_impl->terminalWindow->PreTranslateMessage(message);
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
	if( m_impl->terminalWindow ) {
		m_impl->terminalWindow->SetInputAdapter(nullptr);
		m_impl->terminalWindow->SetModel(nullptr);
	}
	m_impl->manager->Close();
	if( m_impl->terminalWindow ) m_impl->terminalWindow->Close();
	m_impl->terminalWindow.reset();
	if( m_impl->window ) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
}

void CTerminalTool::SetWorkingDirectory( std::wstring workingDirectory )
{
	// Existing sessions keep their original CWD. This value is used only by a
	// subsequently created or explicitly restarted tab.
	m_impl->workingDirectory = std::move(workingDirectory);
}

void CTerminalTool::SetPalette( const theme::ThemePalette& palette )
{
	m_impl->palette = palette;
	if( m_impl->terminalWindow ) m_impl->terminalWindow->SetPalette(palette);
	if( m_impl->window ) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

std::optional<std::uint64_t> CTerminalTool::AddTerminal()
{
	return m_impl->closed ? std::nullopt : m_impl->AddTerminal();
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

bool CTerminalTool::HasStartedAnySession() const noexcept
{
	return m_impl->manager->HasStartedAnySession();
}

bool CTerminalTool::HasCreatedRenderer() const noexcept
{
	return m_impl->terminalWindow != nullptr;
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
