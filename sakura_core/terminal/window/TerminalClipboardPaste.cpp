/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "terminal/window/TerminalClipboardPaste.h"

#include "cxx/com_pointer.hpp"

#include <wincodec.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace terminal {
namespace {

std::atomic<std::uint32_t> g_pasteNameCounter{ 0 };

[[nodiscard]] bool PathNeedsQuotes(std::wstring_view path) noexcept
{
	if (path.empty()) return false;
	for (const wchar_t ch : path) {
		if (ch == L' ' || ch == L'\t' || ch == L'"' || ch == L'&' || ch == L'|'
			|| ch == L'<' || ch == L'>' || ch == L'^' || ch == L'%') {
			return true;
		}
	}
	return false;
}

[[nodiscard]] std::optional<std::filesystem::path> EnsurePasteDirectory()
{
	wchar_t tempRoot[MAX_PATH]{};
	const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(tempRoot)), tempRoot);
	if (length == 0 || length >= std::size(tempRoot)) return std::nullopt;
	std::error_code error;
	std::filesystem::path directory = std::filesystem::path(tempRoot) / L"sakura-editor" / L"terminal-paste";
	std::filesystem::create_directories(directory, error);
	if (error) return std::nullopt;
	return directory;
}

[[nodiscard]] std::filesystem::path MakePasteFilePath(const std::filesystem::path& directory)
{
	const auto now = std::chrono::system_clock::now();
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()).count();
	const auto sequence = g_pasteNameCounter.fetch_add(1, std::memory_order_relaxed);
	wchar_t name[80]{};
	swprintf_s(name, L"paste-%lld-%lu.png", static_cast<long long>(millis),
		static_cast<unsigned long>(sequence));
	return directory / name;
}

[[nodiscard]] std::int64_t FileWriteTimeMs(const std::filesystem::path& path)
{
	std::error_code error;
	const auto writeTime = std::filesystem::last_write_time(path, error);
	if (error) return 0;
	return static_cast<std::int64_t>(writeTime.time_since_epoch().count());
}

void PrunePasteDirectory(const std::filesystem::path& directory, const std::filesystem::path& keep)
{
	std::vector<TerminalPasteImageEntry> entries;
	std::error_code error;
	for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
		if (error) break;
		if (!item.is_regular_file(error) || error) continue;
		if (item.path().extension() != L".png") continue;
		entries.push_back({ item.path().wstring(), FileWriteTimeMs(item.path()) });
	}
	for (const auto& doomed : SelectTerminalPasteImagesToRemove(
		std::move(entries), kMaxRetainedTerminalPasteImages, keep.wstring())) {
		std::error_code removeError;
		std::filesystem::remove(doomed, removeError);
	}
}

[[nodiscard]] bool WriteBytesToFile(const std::filesystem::path& path, const void* bytes, std::size_t size)
{
	if (bytes == nullptr || size == 0) return false;
	const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const BOOL ok = ::WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr);
	::CloseHandle(file);
	return ok != FALSE && written == size;
}

[[nodiscard]] bool EncodeBitmapSourceToPngFile(IWICImagingFactory* factory, IWICBitmapSource* source,
	const std::filesystem::path& path)
{
	if (factory == nullptr || source == nullptr) return false;
	cxx::com_pointer<IWICStream> stream;
	if (FAILED(factory->CreateStream(&stream))
		|| FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
		return false;
	}
	cxx::com_pointer<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
		|| FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
		return false;
	}
	cxx::com_pointer<IWICBitmapFrameEncode> frame;
	cxx::com_pointer<IPropertyBag2> properties;
	if (FAILED(encoder->CreateNewFrame(&frame, &properties))
		|| FAILED(frame->Initialize(properties))
		|| FAILED(frame->WriteSource(source, nullptr))
		|| FAILED(frame->Commit())
		|| FAILED(encoder->Commit())) {
		return false;
	}
	return true;
}

[[nodiscard]] bool SavePngClipboardFormat(HWND owner, const std::filesystem::path& path)
{
	const UINT pngFormat = ::RegisterClipboardFormatW(L"PNG");
	if (pngFormat == 0 || !::IsClipboardFormatAvailable(pngFormat)) return false;
	if (::OpenClipboard(owner) == FALSE) return false;
	bool saved = false;
	if (const HANDLE data = ::GetClipboardData(pngFormat)) {
		if (const void* bytes = ::GlobalLock(data)) {
			saved = WriteBytesToFile(path, bytes, ::GlobalSize(data));
			::GlobalUnlock(data);
		}
	}
	::CloseClipboard();
	return saved;
}

[[nodiscard]] UINT DibPaletteByteCount(const BITMAPINFOHEADER& header) noexcept
{
	if (header.biCompression == BI_BITFIELDS) return 12;
	if (header.biClrUsed != 0) return header.biClrUsed * sizeof(RGBQUAD);
	if (header.biBitCount <= 8) return (1u << header.biBitCount) * static_cast<UINT>(sizeof(RGBQUAD));
	return 0;
}

[[nodiscard]] bool SaveDibClipboardAsPng(HWND owner, UINT format, const std::filesystem::path& path)
{
	if (!::IsClipboardFormatAvailable(format)) return false;
	if (::OpenClipboard(owner) == FALSE) return false;
	bool saved = false;
	if (const HANDLE data = ::GetClipboardData(format)) {
		const auto size = ::GlobalSize(data);
		const auto* bytes = static_cast<const std::uint8_t*>(::GlobalLock(data));
		if (bytes != nullptr && size >= sizeof(BITMAPINFOHEADER)) {
			const auto& header = *reinterpret_cast<const BITMAPINFOHEADER*>(bytes);
			if (header.biSize >= sizeof(BITMAPINFOHEADER) && header.biWidth > 0 && header.biHeight != 0) {
				const UINT paletteBytes = DibPaletteByteCount(header);
				const std::size_t pixelOffset = header.biSize + paletteBytes;
				if (pixelOffset < size) {
					BITMAPFILEHEADER fileHeader{};
					fileHeader.bfType = 0x4D42;
					fileHeader.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + pixelOffset);
					fileHeader.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + size);

					HGLOBAL streamMemory = ::GlobalAlloc(GMEM_MOVEABLE, fileHeader.bfSize);
					if (streamMemory != nullptr) {
						if (auto* destination = static_cast<std::uint8_t*>(::GlobalLock(streamMemory))) {
							std::memcpy(destination, &fileHeader, sizeof(fileHeader));
							std::memcpy(destination + sizeof(fileHeader), bytes, size);
							::GlobalUnlock(streamMemory);

							cxx::com_pointer<IStream> stream;
							if (SUCCEEDED(::CreateStreamOnHGlobal(streamMemory, TRUE, &stream))) {
								streamMemory = nullptr; // ownership transferred
								cxx::com_pointer<IWICImagingFactory> factory;
								if (SUCCEEDED(factory.CreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER))) {
									cxx::com_pointer<IWICBitmapDecoder> decoder;
									if (SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr,
										WICDecodeMetadataCacheOnLoad, &decoder))) {
										cxx::com_pointer<IWICBitmapFrameDecode> frame;
										if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
											saved = EncodeBitmapSourceToPngFile(factory, frame, path);
										}
									}
								}
							}
						}
						if (streamMemory != nullptr) ::GlobalFree(streamMemory);
					}
				}
			}
			::GlobalUnlock(data);
		}
	}
	::CloseClipboard();
	return saved;
}

} // namespace

std::vector<std::wstring> SelectTerminalPasteImagesToRemove(
	std::vector<TerminalPasteImageEntry> entries,
	std::size_t keepNewest,
	std::wstring_view keepPath)
{
	std::sort(entries.begin(), entries.end(),
		[](const TerminalPasteImageEntry& left, const TerminalPasteImageEntry& right) {
			if (left.lastWriteTimeMs != right.lastWriteTimeMs) {
				return left.lastWriteTimeMs > right.lastWriteTimeMs;
			}
			return left.path > right.path;
		});
	std::vector<std::wstring> doomed;
	for (std::size_t index = 0; index < entries.size(); ++index) {
		if (index < keepNewest) continue;
		if (!keepPath.empty() && entries[index].path == keepPath) continue;
		doomed.push_back(std::move(entries[index].path));
	}
	return doomed;
}

std::wstring FormatTerminalPastePath(std::wstring_view path)
{
	if (path.empty()) return {};
	if (!PathNeedsQuotes(path)) return std::wstring(path);
	std::wstring quoted;
	quoted.reserve(path.size() + 2);
	quoted.push_back(L'"');
	for (const wchar_t ch : path) {
		// Windows command-line convention: escape an embedded quote by doubling it.
		if (ch == L'"') quoted.append(L"\"\"");
		else quoted.push_back(ch);
	}
	quoted.push_back(L'"');
	return quoted;
}

std::optional<std::wstring> SaveClipboardImageAsPng(HWND owner)
{
	const auto directory = EnsurePasteDirectory();
	if (!directory) return std::nullopt;
	const auto path = MakePasteFilePath(*directory);

	bool saved = SavePngClipboardFormat(owner, path)
		|| SaveDibClipboardAsPng(owner, CF_DIBV5, path)
		|| SaveDibClipboardAsPng(owner, CF_DIB, path);
	if (!saved) {
		std::error_code error;
		std::filesystem::remove(path, error);
		return std::nullopt;
	}
	PrunePasteDirectory(*directory, path);
	return path.wstring();
}

std::vector<std::wstring> ReadClipboardFileDropPaths(HWND owner)
{
	std::vector<std::wstring> paths;
	if (!::IsClipboardFormatAvailable(CF_HDROP)) return paths;
	if (::OpenClipboard(owner) == FALSE) return paths;
	if (const HANDLE data = ::GetClipboardData(CF_HDROP)) {
		const auto drop = static_cast<HDROP>(data);
		const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		paths.reserve(count);
		for (UINT index = 0; index < count; ++index) {
			const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
			if (length == 0) continue;
			std::wstring path(length, L'\0');
			if (::DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
				path.resize(length);
				paths.push_back(std::move(path));
			}
		}
	}
	::CloseClipboard();
	return paths;
}

} // namespace terminal
