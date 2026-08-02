/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/explorer/CExplorerTool.h"

#include <CommCtrl.h>
#include <wincodec.h>

#include "cxx/com_pointer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

namespace workbench::explorer {
namespace {

constexpr wchar_t kExplorerWindowClass[] = L"SakuraNativeExplorerTool";
constexpr UINT kWorkerResultMessage = WM_APP + 0x571;
constexpr UINT kDirectoryChangedMessage = WM_APP + 0x572;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr unsigned int kDefaultDpi = 96;

[[nodiscard]] int IconSizeForDpi(unsigned int dpi) noexcept
{
	return std::max(12, ::MulDiv(16, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 96));
}

//! Loads a raster icon into a transparent, square 32-bit DIB for the TreeView image list.
//! WIC intentionally fails closed for formats such as SVG that have no native decoder here.
[[nodiscard]] HBITMAP LoadRasterBitmap(const std::filesystem::path& path, unsigned int iconSize) noexcept
{
	try {
		cxx::com_pointer<IWICImagingFactory> factory;
		if (FAILED(factory.CreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER))) return nullptr;
		cxx::com_pointer<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnLoad, &decoder))) return nullptr;
		cxx::com_pointer<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
		UINT width = 0;
		UINT height = 0;
		if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0
			|| width > 4096 || height > 4096) return nullptr;

		UINT scaledWidth = iconSize;
		UINT scaledHeight = iconSize;
		if (width > height) {
			scaledHeight = (std::max)(1u, static_cast<UINT>((static_cast<std::uint64_t>(height) * iconSize) / width));
		} else if (height > width) {
			scaledWidth = (std::max)(1u, static_cast<UINT>((static_cast<std::uint64_t>(width) * iconSize) / height));
		}

		cxx::com_pointer<IWICBitmapScaler> scaler;
		IWICBitmapSource* source = frame;
		if (scaledWidth != width || scaledHeight != height) {
			if (FAILED(factory->CreateBitmapScaler(&scaler))
				|| FAILED(scaler->Initialize(frame, scaledWidth, scaledHeight,
					WICBitmapInterpolationModeHighQualityCubic))) return nullptr;
			source = scaler;
		}
		cxx::com_pointer<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter))
			|| FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;

		const UINT stride = scaledWidth * 4;
		std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * scaledHeight);
		if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return nullptr;

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(iconSize);
		bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(iconSize);
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		HDC screen = ::GetDC(nullptr);
		if (screen == nullptr) return nullptr;
		void* bits = nullptr;
		HBITMAP bitmap = ::CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		::ReleaseDC(nullptr, screen);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) ::DeleteObject(bitmap);
			return nullptr;
		}
		const auto destinationBytes = static_cast<std::size_t>(iconSize) * iconSize * 4;
		std::memset(bits, 0, destinationBytes);
		const UINT left = (iconSize - scaledWidth) / 2;
		const UINT top = (iconSize - scaledHeight) / 2;
		auto* destination = static_cast<BYTE*>(bits);
		for (UINT row = 0; row < scaledHeight; ++row) {
			const auto* sourceRow = pixels.data() + static_cast<std::size_t>(row) * stride;
			std::memcpy(destination + (static_cast<std::size_t>(top + row) * iconSize + left) * 4,
				sourceRow, stride);
		}
		return bitmap;
	}
	catch (...) {
		return nullptr;
	}
}

struct Job {
	std::uint64_t rootGeneration{};
	std::uint64_t nodeId{};
	std::uint64_t requestGeneration{};
	std::wstring path;
};

struct WorkerResult {
	std::uint64_t rootGeneration{};
	std::uint64_t nodeId{};
	std::uint64_t requestGeneration{};
	std::vector<ExplorerEntry> entries;
};

struct NotificationGate {
	std::mutex mutex;
	HWND window{};
	bool alive{};
	std::vector<std::unique_ptr<WorkerResult>> pendingResults;
};

struct SharedWorkerState {
	std::mutex mutex;
	std::deque<Job> jobs;
	std::wstring root;
	std::atomic<std::uint64_t> rootGeneration{ 1 };
	std::atomic<ExplorerWorkerState> state{ ExplorerWorkerState::Idle };
	std::shared_ptr<NotificationGate> gate = std::make_shared<NotificationGate>();
	HANDLE stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE wakeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
};

[[nodiscard]] int CompareNoCase(const std::wstring& left, const std::wstring& right) noexcept
{
	return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) - CSTR_EQUAL;
}

[[nodiscard]] std::wstring JoinPath(const std::wstring& directory, const wchar_t* child)
{
	if (directory.empty()) return child;
	if (directory.back() == L'\\' || directory.back() == L'/') return directory + child;
	return directory + L"\\" + child;
}

void PostResult(const std::shared_ptr<SharedWorkerState>& shared, std::unique_ptr<WorkerResult> result)
{
	const auto gate = shared->gate;
	const auto raw = result.get();
	std::lock_guard lock(gate->mutex);
	if (!gate->alive || gate->window == nullptr) return;
	gate->pendingResults.emplace_back(std::move(result));
	if (!::PostMessageW(gate->window, kWorkerResultMessage, 0, reinterpret_cast<LPARAM>(raw))) {
		gate->pendingResults.pop_back();
	}
}

void PostDirectoryChanged(const std::shared_ptr<SharedWorkerState>& shared, std::uint64_t rootGeneration)
{
	const auto gate = shared->gate;
	std::lock_guard lock(gate->mutex);
	if (gate->alive && gate->window != nullptr) {
		(void)::PostMessageW(gate->window, kDirectoryChangedMessage, static_cast<WPARAM>(rootGeneration), 0);
	}
}

[[nodiscard]] std::vector<ExplorerEntry> EnumerateDirectory(const Job& job, const std::shared_ptr<SharedWorkerState>& shared)
{
	std::vector<ExplorerEntry> entries;
	WIN32_FIND_DATAW findData{};
	const auto pattern = JoinPath(job.path, L"*");
	HANDLE find = ::FindFirstFileW(pattern.c_str(), &findData);
	if (find == INVALID_HANDLE_VALUE) return entries;

	do {
		if (::WaitForSingleObject(shared->stopEvent, 0) == WAIT_OBJECT_0 ||
			shared->rootGeneration.load(std::memory_order_acquire) != job.rootGeneration) {
			break;
		}
		if (std::wcscmp(findData.cFileName, L".") == 0 || std::wcscmp(findData.cFileName, L"..") == 0) continue;
		const DWORD attributes = findData.dwFileAttributes;
		ExplorerEntry entry;
		entry.name = findData.cFileName;
		entry.path = JoinPath(job.path, findData.cFileName);
		entry.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		entry.isReparsePoint = CExplorerTool::IsReparsePoint(attributes);
		entries.emplace_back(std::move(entry));
	} while (::FindNextFileW(find, &findData));
	::FindClose(find);
	return CExplorerTool::SortEntries(std::move(entries));
}

[[nodiscard]] HANDLE OpenWatchDirectory(const std::wstring& root) noexcept
{
	if (root.empty()) return INVALID_HANDLE_VALUE;
	return ::CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
}

void WorkerMain(std::shared_ptr<SharedWorkerState> shared)
{
	HANDLE watch = INVALID_HANDLE_VALUE;
	HANDLE watchEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	OVERLAPPED overlapped{};
	std::vector<std::byte> changeBuffer(8192);
	std::uint64_t watchedGeneration = 0;
	bool readPending = false;

	auto closeWatch = [&] {
		if (watch != INVALID_HANDLE_VALUE) {
			if (readPending) (void)::CancelIoEx(watch, &overlapped);
			::CloseHandle(watch);
			watch = INVALID_HANDLE_VALUE;
			readPending = false;
		}
		::ResetEvent(watchEvent);
		std::memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = watchEvent;
	};

	if (watchEvent == nullptr || shared->stopEvent == nullptr || shared->wakeEvent == nullptr) {
		shared->state.store(ExplorerWorkerState::Stopped, std::memory_order_release);
		if (watchEvent != nullptr) ::CloseHandle(watchEvent);
		return;
	}
	overlapped.hEvent = watchEvent;

	for (;;) {
		if (::WaitForSingleObject(shared->stopEvent, 0) == WAIT_OBJECT_0) break;
		const auto currentGeneration = shared->rootGeneration.load(std::memory_order_acquire);
		if (currentGeneration != watchedGeneration) {
			closeWatch();
			std::wstring root;
			{
				const std::lock_guard lock(shared->mutex);
				root = shared->root;
			}
			watch = OpenWatchDirectory(root);
			watchedGeneration = currentGeneration;
		}

		Job job;
		bool hasJob = false;
		{
			const std::lock_guard lock(shared->mutex);
			while (!shared->jobs.empty() && shared->jobs.front().rootGeneration != currentGeneration) shared->jobs.pop_front();
			if (!shared->jobs.empty()) {
				job = std::move(shared->jobs.front());
				shared->jobs.pop_front();
				hasJob = true;
			}
		}
		if (hasJob) {
			shared->state.store(ExplorerWorkerState::Running, std::memory_order_release);
			auto result = std::make_unique<WorkerResult>();
			result->rootGeneration = job.rootGeneration;
			result->nodeId = job.nodeId;
			result->requestGeneration = job.requestGeneration;
			result->entries = EnumerateDirectory(job, shared);
			if (shared->rootGeneration.load(std::memory_order_acquire) == job.rootGeneration &&
				::WaitForSingleObject(shared->stopEvent, 0) != WAIT_OBJECT_0) {
				PostResult(shared, std::move(result));
			}
			continue;
		}

		shared->state.store(ExplorerWorkerState::Idle, std::memory_order_release);
		if (watch != INVALID_HANDLE_VALUE && !readPending) {
			DWORD ignored{};
			::ResetEvent(watchEvent);
			readPending = ::ReadDirectoryChangesW(watch, changeBuffer.data(), static_cast<DWORD>(changeBuffer.size()), TRUE,
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
				&ignored, &overlapped, nullptr) != FALSE;
		}
		HANDLE events[] = { shared->stopEvent, shared->wakeEvent, watchEvent };
		const DWORD count = watch != INVALID_HANDLE_VALUE && readPending ? 3 : 2;
		const DWORD wait = ::WaitForMultipleObjects(count, events, FALSE, INFINITE);
		if (wait == WAIT_OBJECT_0) break;
		if (wait == WAIT_OBJECT_0 + 1) {
			::ResetEvent(shared->wakeEvent);
			continue;
		}
		if (wait == WAIT_OBJECT_0 + 2) {
			DWORD bytes{};
			if (::GetOverlappedResult(watch, &overlapped, &bytes, FALSE) && bytes != 0) PostDirectoryChanged(shared, watchedGeneration);
			readPending = false;
		}
	}
	shared->state.store(ExplorerWorkerState::CancelRequested, std::memory_order_release);
	closeWatch();
	shared->state.store(ExplorerWorkerState::Stopped, std::memory_order_release);
	::CloseHandle(watchEvent);
}

[[nodiscard]] bool EnsureExplorerClass(HINSTANCE instance)
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = CExplorerTool::WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kExplorerWindowClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

struct CExplorerTool::Impl {
	struct Node {
		std::uint64_t id{};
		std::uint64_t requestGeneration{};
		HTREEITEM item{};
		std::wstring name;
		std::wstring path;
		bool isDirectory{};
		bool isReparsePoint{};
		bool isWorkspaceRoot{};
		bool queued{};
		bool refreshAfterQueued{};
	};

	std::shared_ptr<SharedWorkerState> shared = std::make_shared<SharedWorkerState>();
	std::thread worker;
	HWND window{};
	HWND tree{};
	RECT bounds{};
	unsigned int dpi{ kDefaultDpi };
	ExplorerPalette palette{};
	std::shared_ptr<const icons::FileIconThemeSnapshot> fileIconTheme;
	icons::FileIconThemeVariant fileIconThemeVariant{ icons::FileIconThemeVariant::Default };
	HIMAGELIST iconImages{};
	std::map<std::wstring, int, std::less<>> iconIndices;
	FileActivationCallback activateFile;
	std::wstring root;
	std::unordered_map<std::uint64_t, Node> nodes;
	std::uint64_t nextNodeId = 1;
	std::uint64_t nextRequestGeneration = 1;
	std::uint64_t currentRootGeneration = 1;
	bool active{};
	bool closed{};

	void StartWorker()
	{
		if (!worker.joinable()) worker = std::thread(WorkerMain, shared);
	}

	void StopWorker()
	{
		if (!worker.joinable()) return;
		shared->state.store(ExplorerWorkerState::CancelRequested, std::memory_order_release);
		::SetEvent(shared->stopEvent);
		::SetEvent(shared->wakeEvent);
		worker.join();
	}

	void SetNotificationWindow(HWND target, bool alive)
	{
		const auto gate = shared->gate;
		std::lock_guard lock(gate->mutex);
		gate->window = target;
		gate->alive = alive;
		if (!alive) gate->pendingResults.clear();
	}

	[[nodiscard]] Node* FindNode(std::uint64_t id)
	{
		const auto it = nodes.find(id);
		return it == nodes.end() ? nullptr : &it->second;
	}

	void DestroyIconImages() noexcept
	{
		if (tree != nullptr) TreeView_SetImageList(tree, nullptr, TVSIL_NORMAL);
		if (iconImages != nullptr) {
			::ImageList_Destroy(iconImages);
			iconImages = nullptr;
		}
		iconIndices.clear();
	}

	void ApplyArrowVisibility() noexcept
	{
		if (tree == nullptr) return;
		LONG_PTR style = ::GetWindowLongPtrW(tree, GWL_STYLE);
		if (fileIconTheme != nullptr && fileIconTheme->hidesExplorerArrows) {
			style &= ~static_cast<LONG_PTR>(TVS_HASBUTTONS);
		} else {
			style |= TVS_HASBUTTONS;
		}
		::SetWindowLongPtrW(tree, GWL_STYLE, style);
		::SetWindowPos(tree, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	[[nodiscard]] HBITMAP CreateFontBitmap(const icons::FileIconDefinition& definition) const noexcept
	{
		if (!definition.HasFont()) return nullptr;
		const int iconSize = IconSizeForDpi(dpi);
		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = iconSize;
		bitmapInfo.bmiHeader.biHeight = -iconSize;
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		HDC screen = ::GetDC(nullptr);
		if (screen == nullptr) return nullptr;
		void* bits = nullptr;
		HBITMAP bitmap = ::CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		::ReleaseDC(nullptr, screen);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) ::DeleteObject(bitmap);
			return nullptr;
		}

		const auto pixelCount = static_cast<std::size_t>(iconSize) * iconSize;
		auto* pixels = static_cast<std::uint32_t*>(bits);
		const auto opaqueBackground = 0xFF000000u | (static_cast<std::uint32_t>(palette.panel) & 0x00FFFFFFu);
		std::fill_n(pixels, pixelCount, opaqueBackground);

		HDC dc = ::CreateCompatibleDC(nullptr);
		if (dc == nullptr) {
			::DeleteObject(bitmap);
			return nullptr;
		}
		const HGDIOBJ previousBitmap = ::SelectObject(dc, bitmap);
		LOGFONTW logFont{};
		logFont.lfHeight = -static_cast<LONG>((std::max)(8, ::MulDiv(iconSize, 13, 16)));
		logFont.lfWeight = FW_NORMAL;
		logFont.lfCharSet = DEFAULT_CHARSET;
		logFont.lfOutPrecision = OUT_TT_PRECIS;
		logFont.lfQuality = CLEARTYPE_QUALITY;
		logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
		(void)::wcsncpy_s(logFont.lfFaceName, LF_FACESIZE, definition.font->faceName.c_str(), _TRUNCATE);
		HFONT font = ::CreateFontIndirectW(&logFont);
		if (font != nullptr) {
			const HGDIOBJ previousFont = ::SelectObject(dc, font);
			::SetBkMode(dc, TRANSPARENT);
			::SetTextColor(dc, static_cast<COLORREF>(definition.fontColor.value_or(palette.text)));
			RECT textRect{ 0, 0, iconSize, iconSize };
			(void)::DrawTextW(dc, definition.glyph.c_str(), static_cast<int>(definition.glyph.size()),
				&textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			::SelectObject(dc, previousFont);
			::DeleteObject(font);
		}
		::SelectObject(dc, previousBitmap);
		::DeleteDC(dc);
		// GDI text drawing may leave the alpha byte untouched. The image-list tile is
		// intentionally opaque so the icon has the same panel-colored background as
		// the TreeView rather than becoming a black transparent rectangle.
		for (std::size_t index = 0; index < pixelCount; ++index) pixels[index] |= 0xFF000000u;
		return bitmap;
	}

	[[nodiscard]] std::wstring IconCacheKey(const icons::FileIconDefinition& definition) const
	{
		if (definition.HasImage()) return L"image:" + definition.iconPath.wstring();
		if (!definition.HasFont()) return {};
		std::wstring key = L"font:" + definition.font->faceName + L"\x1F" + definition.glyph;
		if (definition.fontColor) key += L"\x1F" + std::to_wstring(*definition.fontColor);
		return key;
	}

	[[nodiscard]] int EnsureIconIndex(const icons::FileIconDefinition& definition)
	{
		if (iconImages == nullptr) return -1;
		try {
			const auto key = IconCacheKey(definition);
			if (key.empty()) return -1;
			if (const auto found = iconIndices.find(key); found != iconIndices.end()) return found->second;
			HBITMAP bitmap = definition.HasImage()
				? LoadRasterBitmap(definition.iconPath, static_cast<unsigned int>(IconSizeForDpi(dpi)))
				: CreateFontBitmap(definition);
			if (bitmap == nullptr) return -1;
			const int index = ::ImageList_Add(iconImages, bitmap, nullptr);
			::DeleteObject(bitmap);
			if (index < 0) return -1;
			iconIndices.emplace(key, index);
			return index;
		}
		catch (...) {
			return -1;
		}
	}

	[[nodiscard]] int ResolveIcon(const Node& node, bool expanded)
	{
		if (fileIconTheme == nullptr) return -1;
		const auto* definition = fileIconTheme->Resolve(node.name, node.path,
			node.isDirectory, expanded, node.isWorkspaceRoot, fileIconThemeVariant);
		return definition == nullptr ? -1 : EnsureIconIndex(*definition);
	}

	void UpdateNodeIcon(Node& node, bool expanded)
	{
		if (tree == nullptr || node.item == nullptr) return;
		TVITEMW item{};
		item.hItem = node.item;
		item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
		item.iImage = ResolveIcon(node, expanded);
		item.iSelectedImage = item.iImage;
		(void)TreeView_SetItem(tree, &item);
	}

	void UpdateAllItemIcons()
	{
		for (auto& [id, node] : nodes) {
			(void)id;
			const bool expanded = tree != nullptr
				&& (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
			UpdateNodeIcon(node, expanded);
		}
	}

	void RebuildIconImages()
	{
		DestroyIconImages();
		ApplyArrowVisibility();
		if (tree == nullptr || fileIconTheme == nullptr) return;
		const int iconSize = IconSizeForDpi(dpi);
		iconImages = ::ImageList_Create(iconSize, iconSize, ILC_COLOR32, 32, 16);
		if (iconImages == nullptr) return;
		TreeView_SetImageList(tree, iconImages, TVSIL_NORMAL);
		UpdateAllItemIcons();
	}

	void InsertPlaceholder(HTREEITEM parent)
	{
		TVINSERTSTRUCTW insert{};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT;
		insert.item.pszText = const_cast<wchar_t*>(L"");
		(void)TreeView_InsertItem(tree, &insert);
	}

	HTREEITEM InsertNode(HTREEITEM parent, ExplorerEntry entry)
	{
		Node node;
		node.id = nextNodeId++;
		node.name = entry.name;
		node.path = std::move(entry.path);
		node.isDirectory = entry.isDirectory;
		node.isReparsePoint = entry.isReparsePoint;
		node.isWorkspaceRoot = parent == TVI_ROOT;
		TVINSERTSTRUCTW insert{};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT | TVIF_PARAM;
		insert.item.pszText = entry.name.data();
		insert.item.lParam = static_cast<LPARAM>(node.id);
		const int icon = ResolveIcon(node, false);
		if (icon >= 0) {
			insert.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
			insert.item.iImage = icon;
			insert.item.iSelectedImage = icon;
		}
		const auto item = TreeView_InsertItem(tree, &insert);
		if (item == nullptr) return nullptr;
		node.item = item;
		const auto [it, inserted] = nodes.emplace(node.id, std::move(node));
		if (inserted && CanExpand(entry)) InsertPlaceholder(item);
		return item;
	}

	void PopulateRoot()
	{
		if (tree == nullptr) return;
		TreeView_DeleteAllItems(tree);
		nodes.clear();
		if (root.empty()) return;
		ExplorerEntry entry;
		entry.name = CExplorerTool::WorkspaceDisplayName(root);
		entry.path = root;
		entry.isDirectory = true;
		const auto item = InsertNode(TVI_ROOT, std::move(entry));
		if (item != nullptr) (void)TreeView_Expand(tree, item, TVE_EXPAND);
	}

	void QueueEnumeration(Node& node, bool refresh)
	{
		if (closed || !node.isDirectory || node.isReparsePoint) return;
		if (node.queued) {
			node.refreshAfterQueued = node.refreshAfterQueued || refresh;
			return;
		}
		if (!refresh && node.requestGeneration != 0) return;
		node.queued = true;
		node.requestGeneration = nextRequestGeneration++;
		Job job;
		job.rootGeneration = currentRootGeneration;
		job.nodeId = node.id;
		job.requestGeneration = node.requestGeneration;
		job.path = node.path;
		{
			const std::lock_guard lock(shared->mutex);
			shared->jobs.emplace_back(std::move(job));
		}
		::SetEvent(shared->wakeEvent);
	}

	void QueueExpandedNodes()
	{
		for (auto& [id, node] : nodes) {
			if (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) QueueEnumeration(node, true);
		}
	}

	void DeleteChildNodes(HTREEITEM parent)
	{
		while (const auto child = TreeView_GetChild(tree, parent)) {
			DeleteChildNodes(child);
			TVITEMW info{};
			info.mask = TVIF_PARAM;
			info.hItem = child;
			if (TreeView_GetItem(tree, &info) && info.lParam != 0) nodes.erase(static_cast<std::uint64_t>(info.lParam));
			(void)TreeView_DeleteItem(tree, child);
		}
	}

	void ApplyResult(std::unique_ptr<WorkerResult> result)
	{
		if (!result || !CExplorerTool::IsCurrentGeneration(currentRootGeneration, result->rootGeneration)) return;
		auto* node = FindNode(result->nodeId);
		if (node == nullptr || !CExplorerTool::IsCurrentGeneration(node->requestGeneration, result->requestGeneration)) return;
		node->queued = false;
		const bool refreshAgain = node->refreshAfterQueued;
		node->refreshAfterQueued = false;
		DeleteChildNodes(node->item);
		for (auto& entry : result->entries) (void)InsertNode(node->item, std::move(entry));
		if (refreshAgain) QueueEnumeration(*node, true);
	}

	[[nodiscard]] std::unique_ptr<WorkerResult> TakePendingResult(WorkerResult* raw)
	{
		const auto gate = shared->gate;
		std::lock_guard lock(gate->mutex);
		const auto it = std::find_if(gate->pendingResults.begin(), gate->pendingResults.end(), [raw](const auto& value) { return value.get() == raw; });
		if (it == gate->pendingResults.end()) return {};
		auto result = std::move(*it);
		gate->pendingResults.erase(it);
		return result;
	}

	void ActivateSelectedFile()
	{
		if (!activateFile || tree == nullptr) return;
		const auto item = TreeView_GetSelection(tree);
		TVITEMW info{};
		info.mask = TVIF_PARAM;
		info.hItem = item;
		if (item == nullptr || !TreeView_GetItem(tree, &info)) return;
		const auto* node = FindNode(static_cast<std::uint64_t>(info.lParam));
		if (node != nullptr && !node->isDirectory) activateFile(node->path);
	}

	void UpdateRoot(std::wstring newRoot)
	{
		root = std::move(newRoot);
		currentRootGeneration = shared->rootGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (currentRootGeneration == 0) currentRootGeneration = shared->rootGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		{
			const std::lock_guard lock(shared->mutex);
			shared->root = root;
			shared->jobs.clear();
		}
		::SetEvent(shared->wakeEvent);
		PopulateRoot();
	}
};

CExplorerTool::CExplorerTool()
	: m_impl(std::make_unique<Impl>())
{
}

CExplorerTool::~CExplorerTool()
{
	Close();
}

bool CExplorerTool::Create(HWND parent)
{
	if (m_impl->closed || m_impl->window != nullptr || parent == nullptr) return false;
	const HINSTANCE instance = ::GetModuleHandleW(nullptr);
	if (!EnsureExplorerClass(instance)) return false;
	INITCOMMONCONTROLSEX common{};
	common.dwSize = sizeof(common);
	common.dwICC = ICC_TREEVIEW_CLASSES;
	(void)::InitCommonControlsEx(&common);
	m_impl->window = ::CreateWindowExW(0, kExplorerWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_impl->window == nullptr) return false;
	m_impl->tree = ::CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
		0, 0, 0, 0, m_impl->window, nullptr, instance, nullptr);
	if (m_impl->tree == nullptr) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		return false;
	}
	::SendMessageW(m_impl->tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
	TreeView_SetBkColor(m_impl->tree, m_impl->palette.panel);
	TreeView_SetTextColor(m_impl->tree, m_impl->palette.text);
	m_impl->RebuildIconImages();
	m_impl->SetNotificationWindow(m_impl->window, true);
	m_impl->StartWorker();
	m_impl->UpdateRoot(m_impl->root);
	return true;
}

void CExplorerTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (m_impl->closed) return;
	m_impl->bounds = contentRect;
	const unsigned int nextDpi = dpi == 0 ? kDefaultDpi : dpi;
	const bool dpiChanged = m_impl->dpi != nextDpi;
	m_impl->dpi = nextDpi;
	if (dpiChanged) m_impl->RebuildIconImages();
	if (m_impl->window != nullptr) {
		::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top,
			std::max(0L, contentRect.right - contentRect.left), std::max(0L, contentRect.bottom - contentRect.top), SWP_NOACTIVATE | SWP_NOZORDER);
	}
}

void CExplorerTool::Activate()
{
	if (m_impl->closed) return;
	m_impl->active = true;
	if (m_impl->tree != nullptr) ::SetFocus(m_impl->tree);
}

void CExplorerTool::Deactivate()
{
	m_impl->active = false;
}

bool CExplorerTool::PreTranslateMessage(MSG& message)
{
	if (m_impl->closed || m_impl->tree == nullptr || !m_impl->active) return false;
	if (message.hwnd != m_impl->tree) return false;
	if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
		m_impl->ActivateSelectedFile();
		return true;
	}
	return false;
}

void CExplorerTool::Close()
{
	if (m_impl == nullptr || m_impl->closed) return;
	m_impl->closed = true;
	if (m_impl->window != nullptr) ::KillTimer(m_impl->window, kRefreshTimer);
	m_impl->SetNotificationWindow(nullptr, false);
	m_impl->StopWorker();
	m_impl->DestroyIconImages();
	if (m_impl->window != nullptr && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->tree = nullptr;
	if (m_impl->shared->stopEvent != nullptr) { ::CloseHandle(m_impl->shared->stopEvent); m_impl->shared->stopEvent = nullptr; }
	if (m_impl->shared->wakeEvent != nullptr) { ::CloseHandle(m_impl->shared->wakeEvent); m_impl->shared->wakeEvent = nullptr; }
}

void CExplorerTool::SetRoot(std::wstring root)
{
	if (m_impl->closed) return;
	if (m_impl->window == nullptr) {
		m_impl->root = std::move(root);
		return;
	}
	m_impl->UpdateRoot(std::move(root));
}

const std::wstring& CExplorerTool::GetRoot() const noexcept { return m_impl->root; }

void CExplorerTool::SetFileActivationCallback(FileActivationCallback callback)
{
	m_impl->activateFile = std::move(callback);
}

void CExplorerTool::SetPalette(ExplorerPalette palette)
{
	m_impl->palette = palette;
	if (m_impl->tree != nullptr) {
		TreeView_SetBkColor(m_impl->tree, palette.panel);
		TreeView_SetTextColor(m_impl->tree, palette.text);
	}
	m_impl->RebuildIconImages();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, TRUE);
}

void CExplorerTool::SetFileIconTheme(
	std::shared_ptr<const icons::FileIconThemeSnapshot> theme,
	icons::FileIconThemeVariant variant)
{
	if (m_impl->closed) return;
	m_impl->fileIconTheme = std::move(theme);
	m_impl->fileIconThemeVariant = variant;
	m_impl->RebuildIconImages();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, TRUE);
}

ExplorerPalette CExplorerTool::GetPalette() const noexcept { return m_impl->palette; }
ExplorerWorkerState CExplorerTool::GetWorkerState() const noexcept { return m_impl->shared->state.load(std::memory_order_acquire); }
HWND CExplorerTool::GetHwnd() const noexcept { return m_impl->window; }

std::vector<ExplorerEntry> CExplorerTool::SortEntries(std::vector<ExplorerEntry> entries)
{
	std::stable_sort(entries.begin(), entries.end(), [](const ExplorerEntry& left, const ExplorerEntry& right) {
		if (left.isDirectory != right.isDirectory) return left.isDirectory;
		const int insensitive = CompareNoCase(left.name, right.name);
		return insensitive != 0 ? insensitive < 0 : left.name < right.name;
	});
	return entries;
}

std::wstring CExplorerTool::WorkspaceDisplayName(std::wstring_view root)
{
	if (root.empty()) return {};
	size_t end = root.size();
	while (end > 0 && (root[end - 1] == L'\\' || root[end - 1] == L'/')) --end;
	if (end == 0) return {};
	const size_t separator = root.find_last_of(L"\\/", end - 1);
	const size_t start = separator == std::wstring_view::npos ? 0 : separator + 1;
	std::wstring displayName(root.substr(start, end - start));
	if (!displayName.empty()) {
		(void)::CharUpperBuffW(displayName.data(), static_cast<DWORD>(displayName.size()));
	}
	return displayName;
}

bool CExplorerTool::IsCurrentGeneration(std::uint64_t current, std::uint64_t candidate) noexcept
{
	return current != 0 && current == candidate;
}

bool CExplorerTool::IsReparsePoint(DWORD attributes) noexcept { return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
bool CExplorerTool::CanExpand(const ExplorerEntry& entry) noexcept { return entry.isDirectory && !entry.isReparsePoint; }

LRESULT CALLBACK CExplorerTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* tool = static_cast<CExplorerTool*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tool));
	}
	auto* tool = reinterpret_cast<CExplorerTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (tool == nullptr || tool->m_impl == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *tool->m_impl;
	switch (message) {
	case WM_SIZE:
		if (impl.tree != nullptr) ::MoveWindow(impl.tree, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if (dc != nullptr) {
			const HBRUSH brush = ::CreateSolidBrush(impl.palette.panel);
			::FillRect(dc, &paint.rcPaint, brush);
			::DeleteObject(brush);
			::EndPaint(window, &paint);
		}
		return 0;
	}
	case kWorkerResultMessage:
		impl.ApplyResult(impl.TakePendingResult(reinterpret_cast<WorkerResult*>(lParam)));
		return 0;
	case kDirectoryChangedMessage:
		if (static_cast<std::uint64_t>(wParam) == impl.currentRootGeneration) ::SetTimer(window, kRefreshTimer, 150, nullptr);
		return 0;
	case WM_TIMER:
		if (wParam == kRefreshTimer) {
			::KillTimer(window, kRefreshTimer);
			impl.QueueExpandedNodes();
		}
		return 0;
	case WM_NOTIFY: {
		const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
		if (notification == nullptr || notification->hwndFrom != impl.tree) break;
		if (notification->code == TVN_ITEMEXPANDINGW) {
			const auto* expanding = reinterpret_cast<const NMTREEVIEWW*>(lParam);
			const bool expanded = (expanding->action & TVE_EXPAND) != 0;
			auto* node = impl.FindNode(static_cast<std::uint64_t>(expanding->itemNew.lParam));
			if (node != nullptr) {
				impl.UpdateNodeIcon(*node, expanded);
				if (expanded) impl.QueueEnumeration(*node, false);
			}
			return 0;
		}
		if (notification->code == NM_DBLCLK) {
			impl.ActivateSelectedFile();
			return 0;
		}
		if (notification->code == NM_CUSTOMDRAW) {
			auto* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
			if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
			if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && (draw->nmcd.uItemState & (CDIS_SELECTED | CDIS_FOCUS)) != 0) {
				draw->clrText = impl.palette.text;
				draw->clrTextBk = impl.palette.focus;
				return CDRF_NEWFONT;
			}
		}
		break;
	}
	case WM_NCDESTROY:
		impl.SetNotificationWindow(nullptr, false);
		impl.window = nullptr;
		impl.tree = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::explorer
