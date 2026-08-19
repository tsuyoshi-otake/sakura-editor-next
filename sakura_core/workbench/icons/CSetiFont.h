/*! @file
	@brief Register the bundled seti.ttf as a process-private font

	`vs-seti` is VS Code's default file icon theme, and its glyphs live in one font
	that the theme addresses by code point. Real VS Code ships that font inside the
	`theme-seti` extension; this product retired the extension host, so it ships the
	same artwork as a built-in and registers it the same way it registers codicon.ttf.

	Upstream ships `seti.woff`. WOFF is an sfnt whose tables are individually
	compressed, and neither GDI nor DirectWrite reads that container, so the committed
	seti.ttf is the same sfnt with the container removed --
	tools/generate-seti-icon-theme.py performs and verifies the conversion, and
	SETI-ATTRIBUTION.md records the provenance and the license.

	If registration fails, FaceName() is empty and the Explorer falls back to its
	first-party Codicon association table. Drawing Seti's code points in some other
	font would produce unrelated glyphs, not a slightly wrong picture.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::icons {

/*!
	@brief The process-wide owner of the registered seti.ttf

	A GDI font registration is a process resource, so it has one owner per process.
	The first Instance() call performs the registration and every later call reports
	the same outcome.
*/
class CSetiFont {
	using Me = CSetiFont;

public:
	//! One instance per process. The first call reads the resource and registers it.
	[[nodiscard]] static const CSetiFont& Instance() noexcept;

	//! True once the embedded font is registered and usable.
	[[nodiscard]] bool IsAvailable() const noexcept { return !m_faceName.empty(); }

	//! The registered family name (`seti`), or empty when registration failed.
	[[nodiscard]] std::wstring_view FaceName() const noexcept { return m_faceName; }

	CSetiFont(const Me&) = delete;
	Me& operator=(const Me&) = delete;
	CSetiFont(Me&&) = delete;
	Me& operator=(Me&&) = delete;

private:
	CSetiFont() noexcept;
	~CSetiFont();

	std::vector<std::byte> m_fontBytes;
	void* m_fontResourceHandle = nullptr;
	std::wstring m_faceName;
};

} // namespace workbench::icons
