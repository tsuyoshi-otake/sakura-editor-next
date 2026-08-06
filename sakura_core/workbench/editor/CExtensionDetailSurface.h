/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/openvsx/OpenVsxProtocol.h"
#include "theme/CThemeService.h"
#include "window/CWnd.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace markdown {
class CMarkdownPreviewWnd;
}

//! Typed, native metadata surface for one Open VSX extension.
//!
//! This is a typed native metadata surface. README content is supplied explicitly
//! by the composition root; this class never fetches remote content or interprets
//! links and HTML as executable content.
//!
//! The surface is a fixed metadata header plus a scrolling README body, matching
//! VS Code's extension editor, where the header does not scroll with the README.
//! The body is the shared native Markdown preview (`markdown::CMarkdownPreviewWnd`),
//! the same renderer the editor's Markdown preview uses, so a Marketplace README
//! gets real headings, lists, tables, inline styles, and syntax-highlighted code
//! rather than a reduced private approximation. That renderer resolves resources
//! without I/O: with no document path and no workspace root every image and link
//! in a Marketplace README is an external reference, so it is reported as blocked
//! instead of fetched. See `CLAUDE.md` in this directory for the divergence.
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
	//! Mirrors ReadmeState: the icon is supplied out-of-band by the composition
	//! root (already-fetched encoded bytes), never fetched by this class.
	enum class IconState {
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
	//! Decodes and displays an already-fetched icon image (PNG/JPEG/etc). No
	//! network access occurs here; `encodedBytes` must already be the fetched
	//! payload. An empty or undecodable payload becomes IconState::Error.
	void SetIconImage(std::vector<std::byte> encodedBytes);
	void SetIconLoading();
	void SetIconUnsupported();
	//! Reports the installed version for the extension currently shown, or
	//! std::nullopt when it is not installed. Drives the action button between
	//! "Install" (not installed), "Update" (installed at an older version), and
	//! a disabled "Installed" (installed and current) -- this class has no
	//! uninstall callback, so an up-to-date install intentionally disables the
	//! button rather than repurposing it for an action it cannot perform.
	void SetInstalledVersion(std::optional<std::wstring> installedVersion);
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
	void DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;
	void Paint();
	void PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold = false);
	void PaintSectionHeading(HDC dc, const wchar_t* text, int left, int* top, int right);
	//! Lays out the fixed metadata header and returns the y where the README body
	//! begins. A null `dc` measures without drawing, so the paint pass and the
	//! child-window layout pass cannot disagree about where the body starts.
	[[nodiscard]] int PaintHeader(HDC dc, const RECT& client);
	//! Paints the pinned FEATURES/CHANGELOG capability boundary at the bottom and
	//! returns the y where the README body ends. Also measure-only when `dc` is
	//! null, for the same reason as `PaintHeader`.
	[[nodiscard]] int PaintBoundaryFooter(HDC dc, const RECT& client);
	//! Paints the non-Ready README states. Ready is rendered by the preview child.
	void PaintReadmeStatus(HDC dc, const RECT& body);
	//! Hands the current Markdown to the preview child, or hides it when the state
	//! is not Ready. Every call uses a new render generation so a superseded parse
	//! cannot publish over a newer one.
	void PublishReadme();
	void LayoutReadmePreview();
	void InvokeInstall();
	void InvokeClose();
	void ReleaseIconBitmap() noexcept;
	//! Paints the decoded icon bitmap into `tile` with AlphaBlend when
	//! m_iconState == IconState::Ready. Returns false (no drawing performed) for
	//! every other state so the caller can fall back to the initials tile.
	[[nodiscard]] bool DrawIconBitmap(HDC dc, const RECT& tile) noexcept;
	//! Recomputes the action button's label/enabled state from m_hasExtension,
	//! m_installedVersion, m_extension.sVersion, and m_onInstallRequested.
	void RefreshInstallButtonState() noexcept;

	SOpenVsxExtension m_extension;
	std::wstring m_readmeMarkdown;
	std::wstring m_readmeError;
	std::optional<std::wstring> m_installedVersion;
	InstallRequestedCallback m_onInstallRequested;
	CloseRequestedCallback m_onCloseRequested;
	HWND m_hwndClose = nullptr;
	HWND m_hwndInstall = nullptr;
	std::unique_ptr<markdown::CMarkdownPreviewWnd> m_readmePreview;
	std::uint64_t m_readmeGeneration = 0;
	HFONT m_font = nullptr;
	HFONT m_boldFont = nullptr;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;
	bool m_hasExtension = false;
	bool m_focused = false;
	ReadmeState m_readmeState = ReadmeState::Unsupported;
	IconState m_iconState = IconState::Unsupported;
	HBITMAP m_iconBitmap = nullptr;
	int m_iconWidth = 0;
	int m_iconHeight = 0;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
};
