/*! @file
	@brief Register a font that is embedded in this executable as a process-private face

	Every icon font this product draws with is compiled into the binary as a named
	RCDATA resource and registered with AddFontMemResourceEx. It is never written to
	disk and never installed into the system font collection, so no other process --
	and no later run of this one -- can observe or depend on it.

	Both bundled faces need exactly this sequence, and they must fail the same way: a
	missing, empty, or unreadable resource leaves the caller with no face name, and the
	caller falls back to something it already has. Approximating the missing glyphs
	would turn a packaging defect into a silently wrong picture.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <new>
#include <vector>

namespace workbench::icons {

/*!
	@brief Read one RCDATA resource and register it as a process-private font

	@param resourceName A named resource declared in sakura_rc.rc2, which cannot
	       collide with the existing numeric resource id space.
	@param outBytes Receives the font bytes on success. GDI keeps referring to this
	       buffer for as long as the returned handle lives, so the caller must keep it
	       alive and unmoved until it calls UnregisterEmbeddedFontResource.
	@return The AddFontMemResourceEx handle, or nullptr if anything failed.
*/
[[nodiscard]] inline void* RegisterEmbeddedFontResource(
	const wchar_t* resourceName, std::vector<std::byte>& outBytes) noexcept
{
	const HMODULE module = ::GetModuleHandleW(nullptr);
	if (module == nullptr) return nullptr;
	const HRSRC found = ::FindResourceW(module, resourceName, RT_RCDATA);
	if (found == nullptr) return nullptr;
	const DWORD size = ::SizeofResource(module, found);
	if (size == 0) return nullptr;
	const HGLOBAL loaded = ::LoadResource(module, found);
	if (loaded == nullptr) return nullptr;
	const void* data = ::LockResource(loaded);
	if (data == nullptr) return nullptr;
	const auto* const first = static_cast<const std::byte*>(data);
	std::vector<std::byte> bytes;
	try {
		bytes.assign(first, first + size);
	}
	catch (const std::bad_alloc&) {
		return nullptr;
	}
	DWORD faceCount = 0;
	const HANDLE handle = ::AddFontMemResourceEx(bytes.data(), size, nullptr, &faceCount);
	if (handle == nullptr || faceCount == 0) {
		if (handle != nullptr) ::RemoveFontMemResourceEx(handle);
		return nullptr;
	}
	outBytes = std::move(bytes);
	return handle;
}

//! Releases a handle from RegisterEmbeddedFontResource. Accepts nullptr.
inline void UnregisterEmbeddedFontResource(void* handle) noexcept
{
	if (handle != nullptr) ::RemoveFontMemResourceEx(handle);
}

} // namespace workbench::icons
