/*! @file
	@brief 同梱した codicon.ttf をプロセス private フォントとして登録する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/icons/CCodiconFont.h"

#include <cstddef>
#include <vector>

namespace workbench::icons {

namespace {

//! sakura_rc.rc2 が `CODICONFONT RCDATA "workbench/icons/codicon.ttf"` で埋め込む名前。
//! 名前付きリソースにしてあるので、既存の数値 ID 空間とは衝突し得ない。
constexpr const wchar_t* kCodiconResourceName = L"CODICONFONT";

//! 埋め込みリソースを 1 個読み出す。無い/空なら false（fail closed）。
[[nodiscard]] bool ReadEmbeddedFont(std::vector<std::byte>& outBytes) noexcept
{
	const HMODULE module = ::GetModuleHandleW(nullptr);
	if (module == nullptr) return false;
	const HRSRC found = ::FindResourceW(module, kCodiconResourceName, RT_RCDATA);
	if (found == nullptr) return false;
	const DWORD size = ::SizeofResource(module, found);
	if (size == 0) return false;
	const HGLOBAL loaded = ::LoadResource(module, found);
	if (loaded == nullptr) return false;
	const void* data = ::LockResource(loaded);
	if (data == nullptr) return false;
	const auto* const first = static_cast<const std::byte*>(data);
	try {
		outBytes.assign(first, first + size);
	}
	catch (const std::bad_alloc&) {
		return false;
	}
	return true;
}

} // namespace

CCodiconFont::CCodiconFont() noexcept
{
	std::vector<std::byte> raw;
	if (!ReadEmbeddedFont(raw)) return;
	DWORD fontCount = 0;
	m_fontResourceHandle = ::AddFontMemResourceEx(raw.data(), static_cast<DWORD>(raw.size()), nullptr, &fontCount);
	if (m_fontResourceHandle == nullptr || fontCount == 0) {
		m_fontResourceHandle = nullptr;
		return;
	}
	m_fontBytes = std::move(raw);
	m_faceName = L"codicon";
}

CCodiconFont::~CCodiconFont()
{
	if (m_fontResourceHandle != nullptr) {
		::RemoveFontMemResourceEx(m_fontResourceHandle);
		m_fontResourceHandle = nullptr;
	}
}

const CCodiconFont& CCodiconFont::Instance() noexcept
{
	static const CCodiconFont instance;
	return instance;
}

} // namespace workbench::icons
