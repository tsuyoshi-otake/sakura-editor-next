/*! @file
	@brief Bounded, native decode of an already-fetched extension icon image
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <span>

namespace workbench::extension {

//! Result of decoding an extension icon. `bitmap` is a top-down 32bpp BGRA
//! premultiplied-alpha DIB section (`CreateDIBSection` result) suitable for
//! `AlphaBlend`. The caller owns it and must release it with `DeleteObject`.
struct DecodedExtensionIcon {
	HBITMAP bitmap = nullptr;
	int width = 0;
	int height = 0;

	[[nodiscard]] bool IsValid() const noexcept { return bitmap != nullptr && width > 0 && height > 0; }
};

//! Decodes an in-memory encoded image (PNG/JPEG/GIF/BMP, whatever the system
//! WIC codecs installed on this machine support) into a bounded-size native
//! bitmap. This performs no I/O and no network access: `encodedBytes` must
//! already be a fully fetched payload supplied by the caller. Oversized,
//! malformed, empty, or unsupported input returns an invalid result rather
//! than throwing; this is a decode boundary, not a validation guarantee about
//! the image's origin.
//!
//! `maxEdgePixels` bounds both axes of the decoded output (the source image is
//! downscaled with high-quality interpolation when it exceeds this edge), so a
//! maliciously large or absurd source image cannot allocate unbounded memory.
//!
//! **The calling thread must already have a COM apartment.** This decodes through
//! WIC, so `CoCreateInstance(CLSID_WICImagingFactory, ...)` fails with
//! `CO_E_NOTINITIALIZED` on an uninitialized thread and the call then returns an
//! invalid result — the same shape as a malformed image. That is deliberate: this
//! function is `noexcept` and fails closed on every path. But it means a future
//! caller on a background download/worker thread would silently get no icon at
//! all rather than an error, so initialize the apartment on that thread rather
//! than reading the empty result as "this image was bad". The current production
//! caller is `CExtensionDetailSurface`, which runs on the OLE-initialized UI
//! thread.
[[nodiscard]] DecodedExtensionIcon DecodeExtensionIconBitmap(
	std::span<const std::byte> encodedBytes, int maxEdgePixels = 128) noexcept;

} // namespace workbench::extension
