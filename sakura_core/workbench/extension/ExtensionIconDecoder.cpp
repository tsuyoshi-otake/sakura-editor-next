/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extension/ExtensionIconDecoder.h"

#include <shlwapi.h>
#include <wincodec.h>

#include "cxx/com_pointer.hpp"
#include "util/WicCompatibility.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace workbench::extension {
namespace {

//! An extension icon is a small marketplace asset. These bounds only guard
//! against a hostile/broken response; they are not a claim about typical size.
constexpr std::uint64_t kMaximumEncodedBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSourcePixels = 16ULL * 1024ULL * 1024ULL;

} // namespace

DecodedExtensionIcon DecodeExtensionIconBitmap(std::span<const std::byte> encodedBytes, int maxEdgePixels) noexcept
{
	DecodedExtensionIcon result;
	if (encodedBytes.empty() || encodedBytes.size() > kMaximumEncodedBytes || maxEdgePixels <= 0) return result;

	try {
		// SHCreateMemStream copies the buffer into its own IStream, so the caller's
		// span does not need to outlive this call. It already returns one owned
		// reference, so Attach with addref=false takes ownership without leaking it.
		cxx::com_pointer<IStream> stream;
		stream.Attach(::SHCreateMemStream(
			reinterpret_cast<const BYTE*>(encodedBytes.data()), static_cast<UINT>(encodedBytes.size())), false);
		if (!stream) return result;

		cxx::com_pointer<IWICImagingFactory> factory;
		if (FAILED(factory.CreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER))) return result;
		cxx::com_pointer<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) return result;
		cxx::com_pointer<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return result;

		UINT width = 0;
		UINT height = 0;
		if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0
			|| static_cast<std::uint64_t>(width) * height > kMaximumSourcePixels) return result;

		UINT scaledWidth = width;
		UINT scaledHeight = height;
		const UINT maxEdge = static_cast<UINT>(maxEdgePixels);
		if (scaledWidth > maxEdge || scaledHeight > maxEdge) {
			if (scaledWidth >= scaledHeight) {
				scaledHeight = (std::max)(1U, static_cast<UINT>(
					(static_cast<std::uint64_t>(scaledHeight) * maxEdge) / scaledWidth));
				scaledWidth = maxEdge;
			} else {
				scaledWidth = (std::max)(1U, static_cast<UINT>(
					(static_cast<std::uint64_t>(scaledWidth) * maxEdge) / scaledHeight));
				scaledHeight = maxEdge;
			}
		}

		cxx::com_pointer<IWICBitmapScaler> scaler;
		IWICBitmapSource* source = frame;
		if (scaledWidth != width || scaledHeight != height) {
			if (FAILED(factory->CreateBitmapScaler(&scaler))
				|| FAILED(scaler->Initialize(frame, scaledWidth, scaledHeight,
					wic_compat::kHighQualityCubicInterpolation))) return result;
			source = scaler;
		}
		cxx::com_pointer<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter))
			|| FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return result;

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(scaledWidth);
		bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(scaledHeight); // top-down
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		const HBITMAP bitmap = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) ::DeleteObject(bitmap);
			return result;
		}
		const UINT stride = scaledWidth * 4;
		const auto byteCount = static_cast<std::uint64_t>(stride) * scaledHeight;
		if (byteCount > (std::numeric_limits<UINT>::max)()
			|| FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(byteCount), static_cast<BYTE*>(bits)))) {
			::DeleteObject(bitmap);
			return result;
		}
		result.bitmap = bitmap;
		result.width = static_cast<int>(scaledWidth);
		result.height = static_cast<int>(scaledHeight);
	}
	catch (...) {
		if (result.bitmap != nullptr) ::DeleteObject(result.bitmap);
		return {};
	}
	return result;
}

} // namespace workbench::extension
