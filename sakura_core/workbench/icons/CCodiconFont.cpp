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
#include <span>
#include <vector>

#include "workbench/icons/CExtensionIconFont.h"

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

	// 拡張の contributes.icons と同じ下回りを通す。codicon.ttf は素の sfnt なので
	// LoadFontAsSfnt はマジックを検証してそのまま返すだけだが、経路を分けないことで
	// 「拡張のフォントは通るのに同梱フォントだけ別扱い」という差を作らない。
	std::vector<std::byte> sfnt;
	if (detail::LoadFontAsSfnt(std::span<const std::byte>(raw), sfnt) != detail::EFontDecodeError::None) {
		return;
	}
	// codicon.ttf は nameID 3 を持つので実際には NotNeeded になるが、フォントを
	// 差し替えたときに GDI の要求（CExtensionIconFont.h の EnsureUniqueFontIdentifier
	// を参照）で黙って登録に失敗しないよう、同じ補完を通しておく。
	(void)detail::EnsureUniqueFontIdentifier(sfnt);

	std::wstring familyName;
	if (detail::ExtractFamilyName(std::span<const std::byte>(sfnt), familyName)
		!= detail::EFamilyNameError::None) {
		return;
	}

	try {
		auto font = std::make_unique<detail::CRegisteredMemoryFont>(std::move(sfnt));
		if (!font->IsValid()) return;
		m_font = std::move(font);
		m_faceName = std::move(familyName);
	}
	catch (const std::bad_alloc&) {
		m_font.reset();
		m_faceName.clear();
	}
}

CCodiconFont::~CCodiconFont() = default;

const CCodiconFont& CCodiconFont::Instance() noexcept
{
	static const CCodiconFont instance;
	return instance;
}

} // namespace workbench::icons
