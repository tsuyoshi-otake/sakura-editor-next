/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalTool.h"

#include "terminal/PowerShellLocator.h"
#include "terminal/window/TerminalHeaderLayout.h"
#include "terminal/window/CTerminalWnd.h"
#include "workbench/IconMetrics.h"

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
constexpr int kPaneDividerDip = 4;
constexpr int kMinimumPaneWidthDip = 80;
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
constexpr UINT kCommandProfileFirst = 1000;
constexpr UINT kCommandTabFirst = 2000;

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
	std::unique_ptr<CTerminalWnd> terminalWindow;
	std::unique_ptr<CTerminalWnd> secondaryTerminalWindow;
	std::optional<std::uint64_t> secondaryTabId;
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
	bool needsFullTerminalRepaint{};
	bool needsFullSecondaryRepaint{};
	bool outputFrameScheduled{};
	bool protocolInputRetryScheduled{};
	std::vector<std::uint64_t> pendingOutputTabs;
	ULONGLONG synchronizedOutputSince{};
	ULONGLONG secondarySynchronizedOutputSince{};
	int splitRatioPermille{ 500 };
	bool draggingPaneDivider{};
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont chromeFont;
	TerminalPanelActions panelActions;
	TerminalHeaderTarget hotTarget{ TerminalHeaderTarget::None };
	TerminalHeaderTarget pressedTarget{ TerminalHeaderTarget::None };
	bool trackingMouseLeave{};
	std::vector<std::wstring> profileCommandPaths;
	std::vector<std::uint64_t> tabCommandIds;

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
			if( const auto tabId = manager->ActiveTabId() ) return manager->QueueInput(*tabId, bytes);
			return TerminalQueueInputResult::NotRunning;
		});
		candidate->SetResizeSink([this](TerminalSize size) {
			if( const auto tabId = manager->ActiveTabId() ) static_cast<void>(manager->ResizeTab(*tabId, size));
		});
		candidate->SetPalette(palette);
		terminalWindow = std::move(candidate);
		BindActiveModel();
		if( secondaryTabId ) static_cast<void>(EnsureSecondaryTerminalWindow());
		for( const auto& tab : manager->Snapshot() ) HandleOutput(tab.id);
		return true;
	}

	bool EnsureSecondaryTerminalWindow()
	{
		if( secondaryTerminalWindow ) return true;
		if( !secondaryTabId || !window || !instance || closed ) return false;
		auto candidate = std::make_unique<CTerminalWnd>();
		if( !candidate->Create(window, instance) ) return false;
		candidate->SetInputSink([this](std::span<const std::uint8_t> bytes) {
			if( secondaryTabId ) return manager->QueueInput(*secondaryTabId, bytes);
			return TerminalQueueInputResult::NotRunning;
		});
		candidate->SetResizeSink([this](TerminalSize size) {
			if( secondaryTabId ) static_cast<void>(manager->ResizeTab(*secondaryTabId, size));
		});
		candidate->SetPalette(palette);
		secondaryTerminalWindow = std::move(candidate);
		BindSecondaryModel();
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

	TerminalSize SecondarySize() const noexcept
	{
		return secondaryTerminalWindow ? secondaryTerminalWindow->GetTerminalSize() : CurrentSize();
	}

	void BindActiveModel()
	{
		if( terminalWindow ) {
			terminalWindow->SetInputAdapter(manager->ActiveInputAdapter());
			terminalWindow->SetModel(manager->ActiveModel());
			needsFullTerminalRepaint = true;
		}
	}

	void BindSecondaryModel()
	{
		if( secondaryTerminalWindow ) {
			secondaryTerminalWindow->SetInputAdapter(secondaryTabId ? manager->InputAdapter(*secondaryTabId) : nullptr);
			secondaryTerminalWindow->SetModel(secondaryTabId ? manager->Model(*secondaryTabId) : nullptr);
			needsFullSecondaryRepaint = true;
		}
	}

	void DestroySecondaryRenderer() noexcept
	{
		if( secondaryTerminalWindow ) {
			secondaryTerminalWindow->SetInputAdapter(nullptr);
			secondaryTerminalWindow->SetModel(nullptr);
			secondaryTerminalWindow->Close();
			secondaryTerminalWindow.reset();
		}
		needsFullSecondaryRepaint = false;
		secondarySynchronizedOutputSince = 0;
		if( synchronizedOutputSince == 0 && window ) ::KillTimer(window, kSynchronizedOutputTimer);
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
		if( secondaryTabId && secondaryTerminalWindow ) {
			const HWND secondary = secondaryTerminalWindow->GetHwnd();
			if( focused == secondary || (secondary && focused && ::IsChild(secondary, focused)) ) return secondaryTabId;
		}
		return manager->ActiveTabId();
	}

	std::wstring HeaderProfileLabel()
	{
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

	LONG PaneDividerLeft( const RECT& content ) const noexcept
	{
		const LONG divider = ScaleDip(kPaneDividerDip, dpi);
		const LONG available = std::max<LONG>(0, content.right - content.left - divider);
		const LONG minimum = std::min<LONG>(available / 2, ScaleDip(kMinimumPaneWidthDip, dpi));
		const LONG requested = static_cast<LONG>((static_cast<long long>(available) * splitRatioPermille) / 1000);
		return content.left + std::clamp<LONG>(requested, minimum, available - minimum);
	}

	RECT ContentRect() const noexcept
	{
		RECT content{};
		if( window ) ::GetClientRect(window, &content);
		content.top = panelActions.renderHeader ? HeaderLayout().header.bottom : content.top;
		return content;
	}

	bool HitTestPaneDivider( int x, int y ) const noexcept
	{
		if( !secondaryTabId || !window ) return false;
		const RECT content = ContentRect();
		if( y < content.top || y >= content.bottom ) return false;
		const LONG left = PaneDividerLeft(content);
		return x >= left && x < left + ScaleDip(kPaneDividerDip, dpi);
	}

	void SetPaneDividerFromMouse( int x )
	{
		if( !window || !secondaryTabId ) return;
		const RECT content = ContentRect();
		const LONG divider = ScaleDip(kPaneDividerDip, dpi);
		const LONG available = std::max<LONG>(1, content.right - content.left - divider);
		const LONG minimum = std::min<LONG>(available / 2, ScaleDip(kMinimumPaneWidthDip, dpi));
		const LONG position = std::clamp<LONG>(x - content.left, minimum, available - minimum);
		splitRatioPermille = std::clamp(static_cast<int>((static_cast<long long>(position) * 1000) / available), 1, 999);
		LayoutChildren();
	}

	void LayoutChildren()
	{
		if( !window || !terminalWindow ) return;
		const RECT content = ContentRect();
		if( secondaryTabId && EnsureSecondaryTerminalWindow() && secondaryTerminalWindow ) {
			const LONG divider = ScaleDip(kPaneDividerDip, dpi);
			const LONG midpoint = PaneDividerLeft(content);
			RECT primary = content;
			primary.right = midpoint;
			RECT secondary = content;
			secondary.left = std::min(content.right, midpoint + divider);
			terminalWindow->Layout(primary, dpi);
			secondaryTerminalWindow->Layout(secondary, dpi);
		} else {
			terminalWindow->Layout(content, dpi);
		}
	}

	void PaintHeaderTo( HDC dc, const RECT& bounds, unsigned int paintDpi,
		bool includePanelActions, bool drawTitle, int headerHeightDip )
	{
		if( !dc ) return;
		const unsigned int effectiveDpi = paintDpi == 0 ? kDefaultDpi : paintDpi;
		const auto layout = CalculateTerminalHeaderLayout(bounds, effectiveDpi,
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
			::DrawTextW(memory, L"TERMINAL", -1, &title,
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
		if( secondaryTabId ) {
			RECT divider = client;
			divider.top = panelActions.renderHeader ? HeaderLayout().header.bottom : 0;
			divider.left = PaneDividerLeft(divider);
			divider.right = divider.left + ScaleDip(kPaneDividerDip, dpi);
			const HBRUSH dividerBrush = ::CreateSolidBrush(palette.border.ToColorRef());
			if( dividerBrush ) {
				::FillRect(dc, &divider, dividerBrush);
				::DeleteObject(dividerBrush);
			}
		}
		::EndPaint(window, &paint);
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
			if( secondarySynchronizedOutputSince == 0 && synchronizedOutputSince == 0 && window ) {
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
		if( result.titleChanged ) InvalidateTabs();
		if( result.active && terminalWindow ) {
			PaintTerminalOutput(*terminalWindow, manager->ActiveModel(), result,
				needsFullTerminalRepaint, synchronizedOutputSince);
		}
		if( secondaryTabId == tabId && secondaryTerminalWindow ) {
			PaintTerminalOutput(*secondaryTerminalWindow, manager->Model(tabId), result,
				needsFullSecondaryRepaint, secondarySynchronizedOutputSince);
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
		outputFrameScheduled = ::SetTimer(window, kOutputFrameTimer, kOutputFrameMilliseconds, nullptr) != 0;
		if( !outputFrameScheduled ) {
			auto pending = std::move(pendingOutputTabs);
			pendingOutputTabs.clear();
			for( const auto pendingId : pending ) HandleOutput(pendingId);
		}
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
				std::wstring label = tab.label.empty() ? tab.profileLabel : tab.label;
				if( secondaryTabId && tab.id == *secondaryTabId ) label += L"  (Right)";
				::AppendMenuW(sessionsMenu, flags, command, label.c_str());
				tabCommandIds.push_back(tab.id);
			}
			if( tabCommandIds.empty() ) {
				::AppendMenuW(sessionsMenu, MF_STRING | MF_GRAYED, kCommandTabFirst, L"No terminal sessions");
			}
			::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sessionsMenu), L"Terminal Sessions");
			::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		}
		::AppendMenuW(menu, MF_STRING, kCommandNewTerminal, L"New Terminal");
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandRestartTerminal, L"Restart Terminal");
		::AppendMenuW(menu, MF_STRING | (focusedId ? 0 : MF_GRAYED), kCommandCloseTerminal, L"Kill Terminal");
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(menu, MF_STRING | (secondaryTabId ? MF_GRAYED : 0), kCommandSplitTerminal, L"Split Terminal Right");
		::AppendMenuW(menu, MF_STRING | (secondaryTabId ? 0 : MF_GRAYED), kCommandCloseSplit, L"Close Right Terminal");
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
			if( secondaryTabId && secondaryTerminalWindow ) secondaryTerminalWindow->Focus();
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
		const RECT& bounds, unsigned int headerDpi )
	{
		static_cast<void>(wParam);
		if( !headerHost || bounds.right <= bounds.left || bounds.bottom <= bounds.top ) return false;
		hostedHeaderBounds = bounds;
		hostedHeaderDpi = headerDpi == 0 ? kDefaultDpi : headerDpi;
		const auto layout = CalculateTerminalHeaderLayout(bounds, hostedHeaderDpi, false, 34);
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
			} else if( terminalWindow ) {
				terminalWindow->Focus();
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
				if( released == pressed ) ExecuteHeaderTarget(pressed, headerHost, &bounds, hostedHeaderDpi);
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
			if( terminalWindow ) terminalWindow->Focus();
			return 0;
		case WM_LBUTTONDOWN: {
			if( HitTestPaneDivider(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) ) {
				draggingPaneDivider = true;
				::SetCapture(window);
				return 0;
			}
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const auto layout = HeaderLayout();
			if( point.y >= layout.header.top && point.y < layout.header.bottom ) {
				const auto target = layout.HitTest(point);
				if( IsHeaderTargetEnabled(target) ) {
					pressedTarget = target;
					UpdateHotTarget(target);
					::SetCapture(window);
					InvalidateTabs();
				} else if( terminalWindow ) {
					terminalWindow->Focus();
				}
			}
			return 0;
		}
		case WM_MOUSEMOVE: {
			if( draggingPaneDivider ) {
				SetPaneDividerFromMouse(GET_X_LPARAM(lParam));
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
				draggingPaneDivider = false;
				if( ::GetCapture() == window ) ::ReleaseCapture();
				SetPaneDividerFromMouse(GET_X_LPARAM(lParam));
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
			draggingPaneDivider = false;
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
				::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
				return TRUE;
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
				if( synchronizedOutputSince != 0 && terminalWindow ) {
					terminalWindow->InvalidateAll();
					needsFullTerminalRepaint = false;
					synchronizedOutputSince = ::GetTickCount64();
				}
				if( secondarySynchronizedOutputSince != 0 && secondaryTerminalWindow ) {
					secondaryTerminalWindow->InvalidateAll();
					needsFullSecondaryRepaint = false;
					secondarySynchronizedOutputSince = ::GetTickCount64();
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
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return ::DefWindowProcW(window, message, wParam, lParam);
	}

	std::optional<std::uint64_t> AddTerminal()
	{
		// Explicit creation is allowed to materialize the renderer even before a
		// WM_SIZE; a hidden panel never calls this path on its own.
		static_cast<void>(EnsureTerminalWindow());
		const auto id = manager->AddTab(CurrentSize(), workingDirectory);
		BindActiveModel();
		if( id ) HandleOutput(*id);
		InvalidateTabs();
		if( active && terminalWindow ) terminalWindow->Focus();
		return id;
	}

	std::optional<std::uint64_t> EnsureSessionStarted()
	{
		static_cast<void>(EnsureTerminalWindow());
		const auto id = manager->Activate(CurrentSize(), workingDirectory);
		BindActiveModel();
		if( id ) HandleOutput(*id);
		InvalidateTabs();
		return id;
	}

	bool SelectTerminal( std::uint64_t tabId )
	{
		if( secondaryTabId == tabId ) {
			if( active && secondaryTerminalWindow ) secondaryTerminalWindow->Focus();
			return true;
		}
		if( !manager->SelectTab(tabId) ) return false;
		BindActiveModel();
		InvalidateTabs();
		if( active && terminalWindow ) terminalWindow->Focus();
		return true;
	}

	bool RestartTerminal( std::uint64_t tabId )
	{
		static_cast<void>(EnsureTerminalWindow());
		const bool secondary = secondaryTabId == tabId;
		const bool restarted = manager->RestartTab(tabId, secondary ? SecondarySize() : CurrentSize(), workingDirectory);
		if( manager->ActiveTabId() == tabId ) BindActiveModel();
		if( secondary ) BindSecondaryModel();
		HandleOutput(tabId);
		InvalidateTabs();
		return restarted;
	}

	bool DeleteTerminal( std::uint64_t tabId )
	{
		const bool deletingSecondary = secondaryTabId == tabId;
		if( deletingSecondary ) {
			DestroySecondaryRenderer();
			secondaryTabId.reset();
		}
		if( !manager->DeleteTab(tabId) ) return false;
		if( !deletingSecondary && secondaryTabId && manager->ActiveTabId() == secondaryTabId ) {
			// The right-hand session becomes the primary viewport when it is the
			// manager's only remaining choice after the left tab closes.
			DestroySecondaryRenderer();
			secondaryTabId.reset();
		}
		BindActiveModel();
		LayoutChildren();
		InvalidateTabs();
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
		synchronizedOutputSince = 0;
		secondarySynchronizedOutputSince = 0;
		needsFullTerminalRepaint = false;
		needsFullSecondaryRepaint = false;
		if( terminalWindow ) terminalWindow->ResetSessionInputState();
		if( secondaryTerminalWindow ) secondaryTerminalWindow->ResetSessionInputState();
		DestroySecondaryRenderer();
		secondaryTabId.reset();

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

		BindActiveModel();
		LayoutChildren();
		InvalidateTabs();
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

	bool SplitTerminalRight()
	{
		if( closed || secondaryTabId ) return false;
		static_cast<void>(EnsureTerminalWindow());
		const auto primaryId = manager->Activate(CurrentSize(), workingDirectory);
		if( !primaryId ) return false;
		BindActiveModel();
		const auto newId = manager->AddTab(CurrentSize(), workingDirectory);
		if( !newId ) return false;
		secondaryTabId = newId;
		static_cast<void>(manager->SelectTab(*primaryId));
		BindActiveModel();
		static_cast<void>(EnsureSecondaryTerminalWindow());
		BindSecondaryModel();
		LayoutChildren();
		InvalidateTabs();
		if( active && secondaryTerminalWindow ) secondaryTerminalWindow->Focus();
		return true;
	}

	bool CloseTerminalSplit()
	{
		return secondaryTabId && DeleteTerminal(*secondaryTabId);
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
}

void CTerminalTool::Activate()
{
	if( m_impl->closed ) return;
	m_impl->active = true;
	static_cast<void>(m_impl->EnsureSessionStarted());
	if( m_impl->terminalWindow ) m_impl->terminalWindow->Focus();
}

void CTerminalTool::Deactivate()
{
	// Deliberately preserve every tab, session, pipe and parser while hidden.
	m_impl->active = false;
}

bool CTerminalTool::PreTranslateMessage( MSG& message )
{
	if( m_impl->closed ) return false;
	if( m_impl->secondaryTerminalWindow && m_impl->secondaryTerminalWindow->PreTranslateMessage(message) ) return true;
	return m_impl->terminalWindow && m_impl->terminalWindow->PreTranslateMessage(message);
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
	if( m_impl->secondaryTerminalWindow ) {
		m_impl->secondaryTerminalWindow->SetInputAdapter(nullptr);
		m_impl->secondaryTerminalWindow->SetModel(nullptr);
	}
	if( m_impl->window ) ::KillTimer(m_impl->window, kOutputFrameTimer);
	if( m_impl->window ) ::KillTimer(m_impl->window, kSynchronizedOutputTimer);
	if( m_impl->window ) ::KillTimer(m_impl->window, kProtocolInputRetryTimer);
	m_impl->outputFrameScheduled = false;
	m_impl->pendingOutputTabs.clear();
	m_impl->synchronizedOutputSince = 0;
	m_impl->secondarySynchronizedOutputSince = 0;
	m_impl->manager->Close();
	if( m_impl->secondaryTerminalWindow ) m_impl->secondaryTerminalWindow->Close();
	m_impl->secondaryTerminalWindow.reset();
	m_impl->secondaryTabId.reset();
	if( m_impl->terminalWindow ) m_impl->terminalWindow->Close();
	m_impl->terminalWindow.reset();
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
	if( m_impl->terminalWindow ) m_impl->terminalWindow->SetPalette(palette);
	if( m_impl->secondaryTerminalWindow ) m_impl->secondaryTerminalWindow->SetPalette(palette);
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

bool CTerminalTool::CloseTerminalSplit()
{
	return !m_impl->closed && m_impl->CloseTerminalSplit();
}

bool CTerminalTool::HasTerminalSplit() const noexcept
{
	return !m_impl->closed && m_impl->secondaryTabId.has_value();
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
