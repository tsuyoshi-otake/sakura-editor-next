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
#include "util/WicCompatibility.h"

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
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

namespace workbench::explorer {
namespace {

constexpr wchar_t kExplorerWindowClass[] = L"SakuraNativeExplorerTool";
constexpr wchar_t kExplorerScrollbarClass[] = L"SakuraExplorerOverlayScrollbar";
constexpr UINT kWorkerResultMessage = WM_APP + 0x571;
constexpr UINT kDirectoryChangedMessage = WM_APP + 0x572;
constexpr UINT kActivateFileMessage = WM_APP + 0x573;
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
					wic_compat::kHighQualityCubicInterpolation))) return nullptr;
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

struct ExplorerPathLess {
	[[nodiscard]] bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
	{
		return CompareNoCase(left, right) < 0;
	}
};

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
	HWND scrollbar{};
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
	// Expansion is owned by stable filesystem paths. Refresh reconciles existing
	// TreeView items in place so expanded descendants, selection, and scroll
	// metrics do not oscillate while filesystem watcher results arrive.
	std::set<std::wstring, ExplorerPathLess> expandedPaths;
	std::uint64_t nextNodeId = 1;
	std::uint64_t nextRequestGeneration = 1;
	std::uint64_t currentRootGeneration = 1;
	bool active{};
	bool closed{};
	bool scrollbarHover{};
	bool scrollbarDragging{};
	bool trackingScrollbarMouseLeave{};
	int scrollbarThumbGrabOffset{};
	int wheelDeltaRemainder{};
	HTREEITEM pointerDownItem{};
	HTREEITEM pointerHoverItem{};
	ExplorerFileActivationKind pointerActivationKind{ ExplorerFileActivationKind::Preview };
	HTREEITEM clickNotificationItem{};
	bool clickNotificationItemReady{};
	std::wstring pendingActivationPath;
	ExplorerFileActivationKind pendingActivationKind{ ExplorerFileActivationKind::Preview };
	bool fileActivationPosted{};

	struct ScrollbarLayout {
		RECT track{};
		RECT thumb{};
		int totalRows{};
		int visibleRows{};
		int topRow{};
		int maximumTop{};
		bool scrollable{};
	};

	[[nodiscard]] int ScaleDip(int value) const noexcept
	{
		return std::max(1, ::MulDiv(value, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 96));
	}

	[[nodiscard]] ScrollbarLayout GetScrollbarLayout() const noexcept
	{
		ScrollbarLayout layout;
		if (tree == nullptr || scrollbar == nullptr) return layout;
		SCROLLINFO info{ sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
		if (!::GetScrollInfo(tree, SB_VERT, &info)) return layout;
		layout.totalRows = std::max(0, info.nMax - info.nMin + 1);
		layout.visibleRows = std::max(1, static_cast<int>(info.nPage));
		layout.maximumTop = std::max(0, layout.totalRows - layout.visibleRows);
		layout.topRow = std::clamp(info.nPos - info.nMin, 0, layout.maximumTop);
		if (layout.maximumTop == 0) return layout;

		RECT client{};
		if (!::GetClientRect(scrollbar, &client) || client.bottom <= client.top) return layout;
		layout.track = client;
		const int height = client.bottom - client.top;
		const int minimumThumb = std::min(height, ScaleDip(20));
		const int proportionalThumb = static_cast<int>(
			(static_cast<long long>(height) * layout.visibleRows) / std::max(1, layout.totalRows));
		const int thumbHeight = std::clamp(std::max(minimumThumb, proportionalThumb), 1, height);
		const int travel = height - thumbHeight;
		const int offset = layout.maximumTop == 0 ? 0 : static_cast<int>(
			(static_cast<long long>(travel) * layout.topRow) / layout.maximumTop);
		layout.thumb = RECT{ client.left, client.top + offset, client.right, client.top + offset + thumbHeight };
		layout.scrollable = true;
		return layout;
	}

	void EndScrollbarDrag(bool releaseCapture) noexcept
	{
		const bool wasInteractive = scrollbarDragging;
		scrollbarDragging = false;
		scrollbarThumbGrabOffset = 0;
		if (releaseCapture && scrollbar != nullptr && ::GetCapture() == scrollbar) ::ReleaseCapture();
		if (wasInteractive && scrollbar != nullptr) ::InvalidateRect(scrollbar, nullptr, FALSE);
	}

	void SetFirstVisibleRow(int target)
	{
		if (tree == nullptr) return;
		const auto layout = GetScrollbarLayout();
		target = std::clamp(target, 0, layout.maximumTop);
		auto item = TreeView_GetRoot(tree);
		for (int index = 0; item != nullptr && index < target; ++index) item = TreeView_GetNextVisible(tree, item);
		if (item != nullptr) (void)TreeView_SelectSetFirstVisible(tree, item);
		UpdateOverlayScrollbar();
	}

	void DragScrollbarTo(int pointerY)
	{
		if (!scrollbarDragging) return;
		const auto layout = GetScrollbarLayout();
		if (!layout.scrollable) {
			EndScrollbarDrag(true);
			return;
		}
		const int thumbHeight = layout.thumb.bottom - layout.thumb.top;
		const int travel = (layout.track.bottom - layout.track.top) - thumbHeight;
		if (travel <= 0) return;
		const int position = std::clamp(
			pointerY - scrollbarThumbGrabOffset - static_cast<int>(layout.track.top), 0, travel);
		const int target = static_cast<int>((static_cast<long long>(layout.maximumTop) * position) / travel);
		SetFirstVisibleRow(target);
	}

	void PaintScrollbar(HDC dc) const
	{
		const auto layout = GetScrollbarLayout();
		if (dc == nullptr || !layout.scrollable) return;
		const HBRUSH background = ::CreateSolidBrush(scrollbarHover || scrollbarDragging
			? palette.scrollbarTrackHover : palette.background);
		if (background != nullptr) {
			::FillRect(dc, &layout.track, background);
			::DeleteObject(background);
		}
		RECT thumb = layout.thumb;
		thumb.left = std::max(thumb.left, thumb.right - ScaleDip(6));
		const HBRUSH thumbBrush = ::CreateSolidBrush(scrollbarHover || scrollbarDragging
			? palette.scrollbarThumbHover : palette.scrollbarThumb);
		if (thumbBrush != nullptr) {
			::FillRect(dc, &thumb, thumbBrush);
			::DeleteObject(thumbBrush);
		}
	}

	void UpdateScrollbarHover(POINT point)
	{
		const auto layout = GetScrollbarLayout();
		const bool hover = layout.scrollable && point.x >= layout.track.left && point.x < layout.track.right
			&& point.y >= layout.track.top && point.y < layout.track.bottom;
		if (scrollbarHover != hover) {
			scrollbarHover = hover;
			if (scrollbar != nullptr) ::InvalidateRect(scrollbar, nullptr, FALSE);
		}
		if (hover && !trackingScrollbarMouseLeave && scrollbar != nullptr) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, scrollbar, 0 };
			trackingScrollbarMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
	}

	void UpdateOverlayScrollbar()
	{
		if (tree == nullptr || scrollbar == nullptr || window == nullptr) return;
		const LONG_PTR style = ::GetWindowLongPtrW(tree, GWL_STYLE);
		if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0) (void)::ShowScrollBar(tree, SB_BOTH, FALSE);
		RECT client{};
		if (!::GetClientRect(window, &client)) return;
		const int width = ScaleDip(10);
		const int clientWidth = std::max(0, static_cast<int>(client.right - client.left));
		const int clientHeight = std::max(0, static_cast<int>(client.bottom - client.top));
		const int overlayWidth = std::min(width, clientWidth);
		::SetWindowPos(scrollbar, HWND_TOP, static_cast<int>(client.right) - overlayWidth,
			static_cast<int>(client.top), overlayWidth, clientHeight,
			SWP_NOACTIVATE);
		const bool show = GetScrollbarLayout().scrollable;
		::ShowWindow(scrollbar, show ? SW_SHOWNOACTIVATE : SW_HIDE);
		if (!show) {
			scrollbarHover = false;
			trackingScrollbarMouseLeave = false;
			EndScrollbarDrag(true);
		}
		if (show) ::InvalidateRect(scrollbar, nullptr, FALSE);
	}

	[[nodiscard]] HTREEITEM HitTestFileActivationItem(POINT point) const noexcept
	{
		if (tree == nullptr) return nullptr;
		TVHITTESTINFO hit{};
		hit.pt = point;
		const auto item = TreeView_HitTest(tree, &hit);
		constexpr UINT kActivationFlags = TVHT_ONITEMICON | TVHT_ONITEMLABEL;
		return item != nullptr && (hit.flags & kActivationFlags) != 0 ? item : nullptr;
	}

	[[nodiscard]] HTREEITEM HitTestFileActivationAtCursor() const noexcept
	{
		if (tree == nullptr) return nullptr;
		const DWORD position = ::GetMessagePos();
		POINT messagePoint{ GET_X_LPARAM(position), GET_Y_LPARAM(position) };
		if (::ScreenToClient(tree, &messagePoint)) {
			if (const auto item = HitTestFileActivationItem(messagePoint); item != nullptr) return item;
		}
		// NM_CLICK is sent synchronously from the TreeView's pointer handling, but
		// synthetic input and some capture transitions can leave GetMessagePos on
		// the preceding queued message. The live cursor is a safe fallback because
		// this route is mouse-only and still requires an icon/label hit.
		POINT cursorPoint{};
		if (!::GetCursorPos(&cursorPoint) || !::ScreenToClient(tree, &cursorPoint)) return nullptr;
		return HitTestFileActivationItem(cursorPoint);
	}

	[[nodiscard]] HTREEITEM ResolveClickNotificationItem() noexcept
	{
		if (clickNotificationItemReady) {
			clickNotificationItemReady = false;
			const auto item = clickNotificationItem;
			clickNotificationItem = nullptr;
			return item;
		}
		if (const auto item = HitTestFileActivationAtCursor(); item != nullptr) return item;
		return pointerHoverItem;
	}

	void QueueFileActivation(HTREEITEM item, ExplorerFileActivationKind kind)
	{
		if (closed || !activateFile || window == nullptr || tree == nullptr || item == nullptr) return;
		TVITEMW info{};
		info.mask = TVIF_PARAM;
		info.hItem = item;
		if (!TreeView_GetItem(tree, &info)) return;
		const auto* node = FindNode(static_cast<std::uint64_t>(info.lParam));
		if (node == nullptr || node->isDirectory) return;
		if (pendingActivationPath != node->path) {
			pendingActivationPath = node->path;
			pendingActivationKind = kind;
		} else if (kind == ExplorerFileActivationKind::Pinned) {
			// A double-click can arrive while the first click's preview dispatch is
			// posted. Explicit activation must dominate the queued preview.
			pendingActivationKind = ExplorerFileActivationKind::Pinned;
		}
		if (fileActivationPosted) return;
		fileActivationPosted = ::PostMessageW(window, kActivateFileMessage, 0, 0) != FALSE;
		if (!fileActivationPosted) {
			pendingActivationPath.clear();
			pendingActivationKind = ExplorerFileActivationKind::Preview;
		}
	}

	void DispatchQueuedFileActivation()
	{
		fileActivationPosted = false;
		auto path = std::move(pendingActivationPath);
		const auto kind = pendingActivationKind;
		pendingActivationPath.clear();
		pendingActivationKind = ExplorerFileActivationKind::Preview;
		if (!closed && activateFile && !path.empty()) activateFile(path, kind);
	}

	void ScrollByMouseWheel(WPARAM wheelState)
	{
		if (tree == nullptr) return;
		wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wheelState);
		const int notches = wheelDeltaRemainder / WHEEL_DELTA;
		wheelDeltaRemainder %= WHEEL_DELTA;
		if (notches == 0) return;

		UINT configuredLines = 3;
		(void)::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &configuredLines, 0);
		if (configuredLines == 0) return;
		RECT client{};
		(void)::GetClientRect(tree, &client);
		const int itemHeight = std::max(1, static_cast<int>(TreeView_GetItemHeight(tree)));
		const int pageRows = std::max(1, static_cast<int>(client.bottom - client.top) / itemHeight);
		const bool scrollByPage = configuredLines == WHEEL_PAGESCROLL;
		const int rowsPerNotch = scrollByPage
			? 1
			: std::min(pageRows, static_cast<int>(std::min(configuredLines, static_cast<UINT>(pageRows))));
		const int notchCount = notches < 0 ? -notches : notches;
		const int requestCount = notchCount * rowsPerNotch;
		const UINT request = notches > 0
			? (scrollByPage ? SB_PAGEUP : SB_LINEUP)
			: (scrollByPage ? SB_PAGEDOWN : SB_LINEDOWN);
		for (int index = 0; index < requestCount; ++index) {
			(void)::SendMessageW(tree, WM_VSCROLL, static_cast<WPARAM>(request), 0);
		}
		UpdateOverlayScrollbar();
	}

	static LRESULT CALLBACK TreeSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR subclassId, DWORD_PTR referenceData)
	{
		auto* impl = reinterpret_cast<Impl*>(referenceData);
		if (message == WM_NCDESTROY) {
			::RemoveWindowSubclass(hwnd, TreeSubclassProc, subclassId);
			return ::DefSubclassProc(hwnd, message, wParam, lParam);
		}
		if (impl != nullptr && message == WM_MOUSEWHEEL) {
			impl->ScrollByMouseWheel(wParam);
			return 0;
		}
		HTREEITEM releasedItem = nullptr;
		HTREEITEM pressedItem = nullptr;
		if (impl != nullptr) {
			if (message == WM_MOUSEMOVE) {
				impl->pointerHoverItem = impl->HitTestFileActivationItem(
					POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
			} else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
				impl->pointerDownItem = impl->HitTestFileActivationItem(
					POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
				impl->pointerHoverItem = impl->pointerDownItem;
				impl->pointerActivationKind = message == WM_LBUTTONDBLCLK
					? ExplorerFileActivationKind::Pinned : ExplorerFileActivationKind::Preview;
				impl->clickNotificationItemReady = false;
				impl->clickNotificationItem = nullptr;
			} else if (message == WM_LBUTTONUP) {
				pressedItem = impl->pointerDownItem;
				releasedItem = impl->HitTestFileActivationItem(
					POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
				impl->pointerDownItem = nullptr;
				impl->pointerHoverItem = releasedItem;
				impl->clickNotificationItem = pressedItem != nullptr && pressedItem == releasedItem
					? releasedItem : nullptr;
				impl->clickNotificationItemReady = true;
			} else if (message == WM_CANCELMODE) {
				// TreeView releases capture synchronously during a normal button-up
				// sequence. WM_CAPTURECHANGED is therefore not cancellation authority:
				// clearing here can erase the matching down item before WM_LBUTTONUP.
				impl->pointerDownItem = nullptr;
				impl->clickNotificationItem = nullptr;
				impl->clickNotificationItemReady = false;
			}
		}
		const LRESULT result = ::DefSubclassProc(hwnd, message, wParam, lParam);
		// TreeView's NM_CLICK notification carries no item or pointer coordinates.
		// Resolve the physical down/up gesture at the control boundary instead of
		// opening whichever item happened to be selected when the notification ran.
		if (impl != nullptr && message == WM_LBUTTONUP
			&& pressedItem != nullptr && pressedItem == releasedItem) {
			impl->QueueFileActivation(releasedItem, impl->pointerActivationKind);
		}
		if (impl != nullptr && message == WM_LBUTTONUP) {
			impl->clickNotificationItem = nullptr;
			impl->clickNotificationItemReady = false;
		}
		if (impl != nullptr && (message == WM_VSCROLL || message == WM_KEYDOWN
			|| message == TVM_SELECTITEM || message == TVM_EXPAND || message == TVM_DELETEITEM
			|| message == TVM_INSERTITEMW)) {
			impl->UpdateOverlayScrollbar();
		}
		return result;
	}

	static LRESULT CALLBACK ScrollbarWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NCCREATE) {
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}
		auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (impl == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);
		switch (message) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT: {
			PAINTSTRUCT paint{};
			const HDC dc = ::BeginPaint(hwnd, &paint);
			impl->PaintScrollbar(dc);
			::EndPaint(hwnd, &paint);
			return 0;
		}
		case WM_MOUSEMOVE: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			impl->UpdateScrollbarHover(point);
			impl->DragScrollbarTo(point.y);
			return 0;
		}
		case WM_MOUSELEAVE:
			impl->trackingScrollbarMouseLeave = false;
			if (!impl->scrollbarDragging && impl->scrollbarHover) {
				impl->scrollbarHover = false;
				::InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		case WM_LBUTTONDOWN: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const auto layout = impl->GetScrollbarLayout();
			if (!layout.scrollable) return 0;
			::SetFocus(impl->tree);
			if (point.y >= layout.thumb.top && point.y < layout.thumb.bottom) {
				impl->scrollbarDragging = true;
				impl->scrollbarThumbGrabOffset = point.y - layout.thumb.top;
				::SetCapture(hwnd);
			} else {
				impl->SetFirstVisibleRow(layout.topRow + (point.y < layout.thumb.top
					? -layout.visibleRows : layout.visibleRows));
			}
			impl->UpdateScrollbarHover(point);
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		case WM_LBUTTONUP:
			impl->EndScrollbarDrag(true);
			return 0;
		case WM_MOUSEWHEEL:
			if (impl->tree != nullptr) (void)::SendMessageW(impl->tree, message, wParam, lParam);
			impl->UpdateOverlayScrollbar();
			return 0;
		case WM_NCDESTROY:
			impl->EndScrollbarDrag(true);
			impl->scrollbar = nullptr;
			::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			break;
		default:
			break;
		}
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	static bool EnsureScrollbarClass(HINSTANCE instance)
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = ScrollbarWindowProc;
		windowClass.hInstance = instance;
		windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
		windowClass.lpszClassName = kExplorerScrollbarClass;
		return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	}

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
		const auto opaqueBackground = 0xFF000000u | (static_cast<std::uint32_t>(palette.background) & 0x00FFFFFFu);
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
		// intentionally opaque so the icon has the same Explorer background as
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

	HTREEITEM InsertNode(HTREEITEM parent, ExplorerEntry entry, HTREEITEM insertAfter = TVI_LAST)
	{
		const bool canExpand = CanExpand(entry);
		Node node;
		node.id = nextNodeId++;
		node.name = entry.name;
		node.path = std::move(entry.path);
		node.isDirectory = entry.isDirectory;
		node.isReparsePoint = entry.isReparsePoint;
		node.isWorkspaceRoot = parent == TVI_ROOT;
		TVINSERTSTRUCTW insert{};
		insert.hParent = parent;
		insert.hInsertAfter = insertAfter;
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
		if (inserted && canExpand) {
			InsertPlaceholder(item);
			if (expandedPaths.contains(it->second.path)) {
				(void)TreeView_Expand(tree, item, TVE_EXPAND);
			}
		}
		return item;
	}

	void PopulateRoot()
	{
		if (tree == nullptr) return;
		TreeView_DeleteAllItems(tree);
		nodes.clear();
		if (root.empty()) {
			UpdateOverlayScrollbar();
			return;
		}
		ExplorerEntry entry;
		entry.name = CExplorerTool::WorkspaceDisplayName(root);
		entry.path = root;
		entry.isDirectory = true;
		const auto item = InsertNode(TVI_ROOT, std::move(entry));
		if (item != nullptr) (void)TreeView_Expand(tree, item, TVE_EXPAND);
		UpdateOverlayScrollbar();
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

	void DeleteNode(HTREEITEM item)
	{
		if (item == nullptr) return;
		DeleteChildNodes(item);
		TVITEMW info{};
		info.mask = TVIF_PARAM;
		info.hItem = item;
		if (TreeView_GetItem(tree, &info) && info.lParam != 0) nodes.erase(static_cast<std::uint64_t>(info.lParam));
		(void)TreeView_DeleteItem(tree, item);
	}

	void ApplyResult(std::unique_ptr<WorkerResult> result)
	{
		if (!result || !CExplorerTool::IsCurrentGeneration(currentRootGeneration, result->rootGeneration)) return;
		auto* node = FindNode(result->nodeId);
		if (node == nullptr || !CExplorerTool::IsCurrentGeneration(node->requestGeneration, result->requestGeneration)) return;
		node->queued = false;
		const bool refreshAgain = node->refreshAfterQueued;
		node->refreshAfterQueued = false;
		const auto parentItem = node->item;
		const auto parentNodeId = node->id;
		struct ExistingChild {
			HTREEITEM item{};
			std::uint64_t nodeId{};
		};
		std::map<std::wstring, ExistingChild, ExplorerPathLess> existing;
		std::vector<HTREEITEM> placeholders;
		for (auto child = TreeView_GetChild(tree, parentItem); child != nullptr; child = TreeView_GetNextSibling(tree, child)) {
			TVITEMW info{};
			info.mask = TVIF_PARAM;
			info.hItem = child;
			if (!TreeView_GetItem(tree, &info) || info.lParam == 0) {
				placeholders.push_back(child);
				continue;
			}
			const auto childId = static_cast<std::uint64_t>(info.lParam);
			const auto* childNode = FindNode(childId);
			if (childNode != nullptr) existing.emplace(childNode->path, ExistingChild{ child, childId });
		}

		std::set<std::uint64_t> retained;
		bool changed = !placeholders.empty();
		(void)::SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
		HTREEITEM insertAfter = TVI_FIRST;
		for (auto& entry : result->entries) {
			const auto found = existing.find(entry.path);
			Node* childNode = found == existing.end() ? nullptr : FindNode(found->second.nodeId);
			if (childNode != nullptr && childNode->isDirectory == entry.isDirectory
				&& childNode->isReparsePoint == entry.isReparsePoint) {
				if (childNode->name != entry.name) {
					TVITEMW update{};
					update.hItem = childNode->item;
					update.mask = TVIF_TEXT;
					update.pszText = entry.name.data();
					(void)TreeView_SetItem(tree, &update);
					changed = true;
				}
				childNode->name = entry.name;
				childNode->path = std::move(entry.path);
				retained.insert(childNode->id);
				insertAfter = childNode->item;
				continue;
			}
			const auto inserted = InsertNode(parentItem, std::move(entry), insertAfter);
			if (inserted != nullptr) insertAfter = inserted;
			changed = true;
		}
		for (const auto item : placeholders) (void)TreeView_DeleteItem(tree, item);
		for (const auto& [path, child] : existing) {
			(void)path;
			if (!retained.contains(child.nodeId)) {
				DeleteNode(child.item);
				changed = true;
			}
		}
		(void)::SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
		if (changed) {
			(void)::RedrawWindow(tree, nullptr, nullptr,
				RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
		}
		UpdateOverlayScrollbar();
		if (refreshAgain) {
			if (auto* refreshedParent = FindNode(parentNodeId); refreshedParent != nullptr) QueueEnumeration(*refreshedParent, true);
		}
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
		QueueFileActivation(tree == nullptr ? nullptr : TreeView_GetSelection(tree),
			ExplorerFileActivationKind::Pinned);
	}

	void UpdateRoot(std::wstring newRoot)
	{
		root = std::move(newRoot);
		expandedPaths.clear();
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
	if (!EnsureExplorerClass(instance) || !Impl::EnsureScrollbarClass(instance)) return false;
	INITCOMMONCONTROLSEX common{};
	common.dwSize = sizeof(common);
	common.dwICC = ICC_TREEVIEW_CLASSES;
	(void)::InitCommonControlsEx(&common);
	m_impl->window = ::CreateWindowExW(0, kExplorerWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_impl->window == nullptr) return false;
	m_impl->tree = ::CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_NOHSCROLL,
		0, 0, 0, 0, m_impl->window, nullptr, instance, nullptr);
	if (m_impl->tree == nullptr) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		return false;
	}
	m_impl->scrollbar = ::CreateWindowExW(0, kExplorerScrollbarClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
		0, 0, 0, 0, m_impl->window, nullptr, instance, m_impl.get());
	if (m_impl->scrollbar == nullptr || !::SetWindowSubclass(m_impl->tree, Impl::TreeSubclassProc, 1,
		reinterpret_cast<DWORD_PTR>(m_impl.get()))) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		m_impl->tree = nullptr;
		m_impl->scrollbar = nullptr;
		return false;
	}
	::SendMessageW(m_impl->tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
	TreeView_SetBkColor(m_impl->tree, m_impl->palette.background);
	TreeView_SetTextColor(m_impl->tree, m_impl->palette.text);
	m_impl->RebuildIconImages();
	m_impl->SetNotificationWindow(m_impl->window, true);
	m_impl->StartWorker();
	m_impl->UpdateRoot(m_impl->root);
	m_impl->UpdateOverlayScrollbar();
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
	m_impl->scrollbar = nullptr;
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
	if (CompareNoCase(m_impl->root, root) == 0) return;
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
		TreeView_SetBkColor(m_impl->tree, palette.background);
		TreeView_SetTextColor(m_impl->tree, palette.text);
	}
	m_impl->RebuildIconImages();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, TRUE);
	if (m_impl->scrollbar != nullptr) ::InvalidateRect(m_impl->scrollbar, nullptr, FALSE);
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
		impl.UpdateOverlayScrollbar();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if (dc != nullptr) {
			const HBRUSH brush = ::CreateSolidBrush(impl.palette.background);
			::FillRect(dc, &paint.rcPaint, brush);
			::DeleteObject(brush);
			::EndPaint(window, &paint);
		}
		return 0;
	}
	case kWorkerResultMessage:
		impl.ApplyResult(impl.TakePendingResult(reinterpret_cast<WorkerResult*>(lParam)));
		return 0;
	case kActivateFileMessage:
		impl.DispatchQueuedFileActivation();
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
				if (expanded) {
					impl.expandedPaths.insert(node->path);
				} else {
					impl.expandedPaths.erase(node->path);
				}
				impl.UpdateNodeIcon(*node, expanded);
				if (expanded) impl.QueueEnumeration(*node, false);
			}
			impl.UpdateOverlayScrollbar();
			return 0;
		}
		if (notification->code == TVN_SELCHANGEDW) {
			const auto* changed = reinterpret_cast<const NMTREEVIEWW*>(lParam);
			if (changed->action == TVC_BYKEYBOARD) {
				impl.QueueFileActivation(changed->itemNew.hItem, ExplorerFileActivationKind::Preview);
			}
			return 0;
		}
		if (notification->code == NM_CLICK) {
			// TreeView selects on button-down but emits NM_CLICK on button-up. Queueing
			// from this semantic notification keeps file opening outside the native
			// control's input stack and also survives focus/capture transitions.
			impl.QueueFileActivation(impl.ResolveClickNotificationItem(), ExplorerFileActivationKind::Preview);
			return 0;
		}
		if (notification->code == NM_DBLCLK) {
			impl.QueueFileActivation(impl.HitTestFileActivationAtCursor(), ExplorerFileActivationKind::Pinned);
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
		impl.scrollbar = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::explorer
