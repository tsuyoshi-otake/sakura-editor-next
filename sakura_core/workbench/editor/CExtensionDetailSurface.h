/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/openvsx/OpenVsxProtocol.h"
#include "theme/CThemeService.h"
#include "window/CWnd.h"

#include <functional>
#include <string>

//! Typed, native metadata surface for one Open VSX extension.
//!
//! This is a typed native metadata surface. README content is supplied explicitly
//! by the composition root; this class never fetches remote content or interprets
//! links and HTML as executable content.
class CExtensionDetailSurface final : public CWnd
{
public:
	using InstallRequestedCallback = std::function<void()>;
	using CloseRequestedCallback = std::function<void()>;
	enum class ReadmeState {
		Unsupported,
		Loading,
		Ready,
		Error,
	};

	explicit CExtensionDetailSurface();
	~CExtensionDetailSurface() override;

	CExtensionDetailSurface(const CExtensionDetailSurface&) = delete;
	CExtensionDetailSurface& operator=(const CExtensionDetailSurface&) = delete;

	HWND Open(HINSTANCE hInstance, HWND hwndParent);
	void Destroy() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show() noexcept;
	void Hide() noexcept;
	void Focus() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept { return CWnd::GetHwnd(); }
	void ShowExtension(const SOpenVsxExtension& extension);
	void ClearExtension();
	//! Supplies the already retrieved README Markdown. No network access occurs here.
	void SetReadmeMarkdown(std::wstring markdown);
	void SetReadmeLoading();
	void SetReadmeError(std::wstring message);
	void SetReadmeUnsupported();
	[[nodiscard]] bool HasExtension() const noexcept { return m_hasExtension; }
	void SetOnInstallRequested(InstallRequestedCallback callback);
	void SetOnCloseRequested(CloseRequestedCallback callback);
	//! Returns true when hwndControl is this surface's close-affordance button.
	[[nodiscard]] bool IsCloseButton(HWND hwndControl) const noexcept { return hwndControl != nullptr && hwndControl == m_hwndClose; }

	LRESULT DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override;

private:
	static constexpr int kInstallButtonId = 0x5101;
	static constexpr int kCloseButtonId = 0x5102;

	[[nodiscard]] unsigned int Dpi() const noexcept;
	[[nodiscard]] int ScaleDip(int dip) const noexcept;
	void LayoutChildren();
	void EnsureFont();
	void ReleaseFont() noexcept;
	void Paint();
	void PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold = false);
	void PaintSectionHeading(HDC dc, const wchar_t* text, int left, int* top, int right);
	void PaintReadme(HDC dc, int left, int* top, int right, int bottom);
	void ScrollTo(int offset) noexcept;
	void InvokeInstall();
	void InvokeClose();

	SOpenVsxExtension m_extension;
	std::wstring m_readmeMarkdown;
	std::wstring m_readmeError;
	InstallRequestedCallback m_onInstallRequested;
	CloseRequestedCallback m_onCloseRequested;
	HWND m_hwndClose = nullptr;
	HWND m_hwndInstall = nullptr;
	HFONT m_font = nullptr;
	HFONT m_boldFont = nullptr;
	bool m_hasExtension = false;
	bool m_focused = false;
	int m_scrollOffset = 0;
	int m_contentHeight = 0;
	int m_maxScrollOffset = 0;
	ReadmeState m_readmeState = ReadmeState::Unsupported;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
};
