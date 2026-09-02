/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/explorer/CExplorerTool.h"
#include "CSelectLang.h"

#include <CommCtrl.h>
#include <wincodec.h>

#include "cxx/com_pointer.hpp"
#include "theme/CThemeService.h"
#include "workbench/IconMetrics.h"
#include "workbench/ViewsWelcomeMetrics.h"
#include "workbench/commands/ExplorerCommandArguments.h"
#include "workbench/commands/ExplorerCommandIds.h"
#include "workbench/explorer/ExplorerContextMenuModel.h"
#include "workbench/explorer/ExplorerFileIcon.h"
#include "workbench/explorer/ExplorerResourcePath.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CSetiFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/SetiFileIcon.h"
#include "workbench/icons/SetiIconPainter.h"
#include "workbench/icons/ThemeIconResolver.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/WorkerRetirementService.h"

#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <array>
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
#include <vector>


namespace workbench::explorer {
namespace {

constexpr wchar_t kExplorerWindowClass[] = L"SakuraNativeExplorerTool";
constexpr UINT kWorkerResultMessage = WM_APP + 0x571;
constexpr UINT kDirectoryChangedMessage = WM_APP + 0x572;
constexpr UINT kActivateFileMessage = WM_APP + 0x573;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr unsigned int kDefaultDpi = 96;
//! Explorer and Source Control are Views in the same Primary Side Bar. Keep
//! their ViewPane headers at the same VS Code-like 30 DIP density.
constexpr int kPaneHeaderHeightDip = 30;
constexpr int kTreeRowHeightDip = 22;
constexpr int kHeaderInsetDip = 10;
// The pane header's disclosure twistie and the title inset it produces, matching
// the Outline header CViewContainerHost draws directly above this pane.
constexpr int kHeaderTwistieInsetDip = 6;
constexpr int kHeaderTwistieSideDip = 12;
constexpr int kHeaderTitleInsetDip = 22;
constexpr int kHeaderActionSideDip = 22;
constexpr int kHeaderActionGapDip = 1;
// `iconlabel.css` sets `.monaco-icon-label::after { margin: auto 16px 0 5px; }`, so a
// decoration badge is inset from the row's right edge and never touches the label.
constexpr int kDecorationBadgeRightInsetDip = 16;
constexpr int kDecorationBadgeLeftGapDip = 5;
// The bubble a folder row draws instead of its children's letters is a 14px codicon
// at `opacity: 0.4` with `margin-right: 14px`; the letters are drawn at `opacity: 0.75`.
constexpr int kDecorationBubbleSideDip = 14;
constexpr int kDecorationBubbleRightInsetDip = 14;
constexpr double kDecorationBadgeOpacity = 0.75;
constexpr double kDecorationBubbleOpacity = 0.4;
//! `font-size: 90%` of the row font, as a permille so the scaling is integral.
constexpr int kDecorationBadgeFontPermille = 900;
// ViewWelcome lays its content out as a top-flow column: 20px lateral
// padding, a 300px maximum column, and one em between direct children. Keep
// the native projection on the same geometry instead of centering a compact
// button block in the available view. The numbers themselves live in
// workbench/ViewsWelcomeMetrics.h, because Source Control renders the same
// upstream contribution and its buttons have to come out the same size.
constexpr std::string_view kOpenFolderCommandId = "workbench.action.files.openFolder";
constexpr std::string_view kAddRootFolderCommandId = "workbench.action.addRootFolder";
constexpr std::string_view kCloneRepositoryCommandId = "git.clone";

//! The ViewWelcome contribution's text is a full-width paragraph; only its
//! button containers have the 300px cap.  Keeping the strings in one place
//! makes measuring and painting use the same upstream variant.
[[nodiscard]] const wchar_t* WelcomeParagraphText(ExplorerWelcomeParagraph paragraph) noexcept
{
	switch (paragraph) {
	case ExplorerWelcomeParagraph::EmptyWorkspace:
		return LS(STR_WORKBENCH_EXPLORER_EMPTY_WORKSPACE);
	case ExplorerWelcomeParagraph::NoFolderWithEditors:
		return LS(STR_WORKBENCH_EXPLORER_NO_FOLDER_WITH_EDITORS);
	case ExplorerWelcomeParagraph::MultiRootUnavailable:
		return LS(STR_WORKBENCH_EXPLORER_MULTIROOT_UNAVAILABLE);
	case ExplorerWelcomeParagraph::NoFolder:
		return LS(STR_WORKBENCH_EXPLORER_NO_FOLDER);
	case ExplorerWelcomeParagraph::CloneRepositoryDescription:
		return LS(STR_WORKBENCH_EXPLORER_CLONE_REPOSITORY_DESCRIPTION);
	}
	return L"";
}

struct HeaderActionSpec final {
	const std::string_view commandId{};
	const std::wstring_view iconId{};
};

constexpr std::array<HeaderActionSpec, 4> kHeaderActions{
	HeaderActionSpec{ commands::kCreateFileFromExplorerCommandId, L"new-file" },
	HeaderActionSpec{ commands::kCreateFolderFromExplorerCommandId, L"new-folder" },
	HeaderActionSpec{ commands::kRefreshFilesExplorerCommandId, L"refresh" },
	HeaderActionSpec{ commands::kCollapseExplorerFoldersCommandId, L"collapse-all" },
};

[[nodiscard]] int IconSizeForDpi(unsigned int dpi) noexcept
{
	return std::max(12, ::MulDiv(16, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 96));
}

void DrawExplorerIcon(HDC dc, const RECT& bounds, std::wstring_view iconId, COLORREF color) noexcept
{
	if (dc == nullptr || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	const auto icon = icons::ResolveThemeIcon(iconId, icons::CCodiconFont::Instance().FaceName());
	if (!icon.font) {
		icons::codicons::Draw(dc, icons::IconRect{ bounds.left, bounds.top, bounds.right, bounds.bottom }, icon.builtin, color);
		return;
	}
	const int side = std::max(1, std::min(static_cast<int>(bounds.right - bounds.left),
		static_cast<int>(bounds.bottom - bounds.top)));
	const HFONT glyphFont = icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName, side);
	if (glyphFont == nullptr || icon.fontIcon.glyph.empty()) {
		if (glyphFont != nullptr) ::DeleteObject(glyphFont);
		return;
	}
	const HGDIOBJ previousFont = ::SelectObject(dc, glyphFont);
	const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF previousTextColor = ::SetTextColor(dc, color);
	RECT glyph = bounds;
	::DrawTextW(dc, icon.fontIcon.glyph.c_str(), static_cast<int>(icon.fontIcon.glyph.size()), &glyph,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::SetTextColor(dc, previousTextColor);
	::SetBkMode(dc, previousBackgroundMode);
	::SelectObject(dc, previousFont);
	::DeleteObject(glyphFont);
}

//! The Seti painter is shared with the Source Control view; see
//! icons/SetiIconPainter.h. These keep the unqualified call sites below.
using icons::seti::ColorRefFromThemeRgb;
using icons::seti::DrawSetiIcon;

//! The colour plane of the TreeView's icon-slot spacer. Its pixels never reach the
//! screen; CreateSpacerMaskBitmap supplies the mask that keeps them off it.
[[nodiscard]] HBITMAP CreateTransparentBitmap(int side) noexcept
{
	if (side <= 0) return nullptr;
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = side;
	info.bmiHeader.biHeight = -side;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	HDC screen = ::GetDC(nullptr);
	if (screen == nullptr) return nullptr;
	void* bits = nullptr;
	const HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	::ReleaseDC(nullptr, screen);
	if (bitmap == nullptr || bits == nullptr) {
		if (bitmap != nullptr) ::DeleteObject(bitmap);
		return nullptr;
	}
	std::memset(bits, 0, static_cast<std::size_t>(side) * side * sizeof(std::uint32_t));
	return bitmap;
}

/*!
	@brief Builds the spacer's mask: every bit set, so the spacer is transparent everywhere

	The image list exists only to reserve the native TreeView's icon slot, and the row's
	real glyph is painted afterwards in NM_CUSTOMDRAW. An ILC_MASK image list added with
	a null mask does not mean "no mask"; it means an all-zero mask, which is opaque, so
	the spacer's zeroed colour plane paints as a solid black square behind every row.
	A monochrome mask whose bits are all 1 is what makes the slot actually empty. The
	32-bit colour plane's zero alpha cannot do it: the list carries ILC_MASK, so the
	mask decides transparency and the alpha channel is ignored.
*/
[[nodiscard]] HBITMAP CreateSpacerMaskBitmap(int side) noexcept
{
	if (side <= 0) return nullptr;
	// CreateBitmap wants each monochrome scanline padded to a WORD boundary.
	const std::size_t stride = ((static_cast<std::size_t>(side) + 15u) / 16u) * 2u;
	const std::vector<std::uint8_t> bits(stride * static_cast<std::size_t>(side), 0xFFu);
	return ::CreateBitmap(side, side, 1, 1, bits.data());
}

//! Loads a raster icon into a transparent, square 32-bit DIB for the TreeView image list.
//! WIC intentionally fails closed for formats such as SVG that have no native decoder here.
[[nodiscard]] HBITMAP LoadRasterBitmap(const std::filesystem::path& path, unsigned int iconSize) noexcept
{
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
		// MinGW-w64's wincodec.h still stops at WICBitmapInterpolationModeFant;
		// the high-quality cubic mode was added to the Windows SDK in Windows 8.
		// The experimental MinGW build therefore scales with Fant, which is a
		// quality difference in that build only.
#if defined(_MSC_VER)
		constexpr WICBitmapInterpolationMode kScalerMode = WICBitmapInterpolationModeHighQualityCubic;
#else
		constexpr WICBitmapInterpolationMode kScalerMode = WICBitmapInterpolationModeFant;
#endif
		if (FAILED(factory->CreateBitmapScaler(&scaler))
			|| FAILED(scaler->Initialize(frame, scaledWidth, scaledHeight,
				kScalerMode))) return nullptr;
		source = scaler;
	}
	cxx::com_pointer<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(&converter))
		|| FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
			WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;

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

	// Decode straight into the centred region of the DIB. CopyPixels writes only each row's
	// own bytes at the given stride, so the zeroed padding around the image survives, and the
	// destination buffer removes the last allocation this function performed.
	const std::size_t destinationBytes = static_cast<std::size_t>(iconSize) * iconSize * 4;
	std::memset(bits, 0, destinationBytes);
	const UINT left = (iconSize - scaledWidth) / 2;
	const UINT top = (iconSize - scaledHeight) / 2;
	const std::size_t offset = (static_cast<std::size_t>(top) * iconSize + left) * 4;
	const UINT destinationStride = iconSize * 4;
	if (FAILED(converter->CopyPixels(nullptr, destinationStride,
		static_cast<UINT>(destinationBytes - offset), static_cast<BYTE*>(bits) + offset))) {
		::DeleteObject(bitmap);
		return nullptr;
	}
	return bitmap;
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

	~SharedWorkerState() noexcept
	{
		if (stopEvent != nullptr) {
			::CloseHandle(stopEvent);
			stopEvent = nullptr;
		}
		if (wakeEvent != nullptr) {
			::CloseHandle(wakeEvent);
			wakeEvent = nullptr;
		}
	}
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

/*!
	@brief Composites a translucent foreground over an opaque background.

	The decoration badge and the folder bubble are drawn at CSS `opacity` values,
	and GDI text has no alpha channel, so the blend has to happen here. The
	background is the row's own fill rather than the view's, or a badge on a
	selected row would be composited over a color that row does not show.
*/
[[nodiscard]] COLORREF BlendColor(COLORREF foreground, COLORREF background, double opacity) noexcept
{
	const auto mix = [opacity](int front, int back) {
		return static_cast<int>(front * opacity + back * (1.0 - opacity) + 0.5);
	};
	return RGB(mix(GetRValue(foreground), GetRValue(background)),
		mix(GetGValue(foreground), GetGValue(background)),
		mix(GetBValue(foreground), GetBValue(background)));
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
		// Re-arm the watch only once every queued enumeration has been served.
		// Opening the directory handle is a synchronous filesystem call, and a
		// root change queues its enumeration at the same moment it bumps the
		// generation.  Doing this at the top of the loop would put that call
		// ahead of the work the user is waiting to see, so a cold or busy
		// volume delays the first contents of the folder they just opened.
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
	std::optional<workbench::WorkerRetirementService::Reservation> workerRetirement;
	HWND window{};
	HWND tree{};
	//! Persistent composition target for the Explorer's own header/welcome surface.
	//! The child TreeView remains responsible for its native double-buffered rows.
	workbench::rendering::CGdiBackBuffer windowBuffer;
	workbench::rendering::FrameNativeSurfacePayloadAdapter nativeSurface;
	//! The VS Code-style overlay scrollbar, shared with the Source Control view.
	controls::COverlayScrollbar scrollbar;
	RECT bounds{};
	unsigned int dpi{ kDefaultDpi };
	ExplorerPalette palette{};
	theme::CThemeFont font;
	//! The published decoration table. Empty until a provider publishes one, which
	//! is also the state a workspace with no repository must stay in.
	decorations::FileDecorationTable fileDecorations;
	ExplorerDecorationOptions decorationOptions;
	HIMAGELIST iconImages{};
	std::map<std::wstring, int, std::less<>> iconIndices;
	std::array<RECT, kHeaderActions.size()> headerActionRects{};
	int hoveredHeaderAction{ -1 };
	// A header command or disclosure requires a complete click that started on
	// this surface. Project activation can reveal Explorer between another
	// control's button-down and button-up; that trailing up is not our click.
	bool headerClickArmed{};
	int armedHeaderAction{ -1 };
	bool trackingHeaderMouseLeave{};
	RECT openFolderButton{};
	RECT addFolderButton{};
	RECT welcomeMessageRect{};
	std::vector<ExplorerWelcomeBlock> welcomeBlocks;
	std::vector<RECT> welcomeBlockRects;
	ExplorerWelcomeState welcomeState = ExplorerWelcomeState::NoFolder;
	// The file view's pane header is a disclosure control in VS Code: its twistie
	// collapses the pane to its header alone.  Collapsing is real here -- the body
	// is hidden, not merely re-drawn -- so the twistie is not a painted decoration.
	bool filesPaneExpanded{ true };
	std::size_t hoveredWelcomeBlock{};
	FileActivationCallback activateFile;
	std::wstring root;
	std::unordered_map<std::uint64_t, Node> nodes;
	// Expansion is owned by stable filesystem paths. Refresh reconciles existing
	// TreeView items in place so expanded descendants, selection, and scroll
	// metrics do not oscillate while filesystem watcher results arrive.
	std::set<std::wstring, ExplorerPathLess> expandedPaths;
	std::uint64_t nextNodeId = 1;
	// The workspace root is a node without a TreeView item: VS Code renders a root
	// row only for a multi-root workspace, and this fork's multi-root boundary is
	// explicitly unsupported.  The node still exists because enumeration, refresh,
	// and the empty-area context menu all address the root folder as a resource.
	std::uint64_t workspaceRootNodeId{};
	std::uint64_t nextRequestGeneration = 1;
	std::uint64_t currentRootGeneration = 1;
	bool active{};
	bool closed{};
	bool trackingTreeMouseLeave{};
	int wheelDeltaRemainder{};
	HTREEITEM pointerDownItem{};
	HTREEITEM pointerHoverItem{};
	ExplorerFileActivationKind pointerActivationKind{ ExplorerFileActivationKind::Preview };
	HTREEITEM clickNotificationItem{};
	bool clickNotificationItemReady{};
	std::wstring pendingActivationPath;
	ExplorerFileActivationKind pendingActivationKind{ ExplorerFileActivationKind::Preview };
	bool fileActivationPosted{};
	CommandCallback commandCallback;
	MenuTitleResolver menuTitleResolver;
	RenameCommitCallback renameCommit;
	CreateCommitCallback createCommit;
	//! True while an inline label edit owns the tree.  Worker results are
	//! deferred for its duration because reconciliation would destroy TreeView
	//! items - possibly the edited one - under the live edit control.
	bool labelEditActive{};
	//! Set only by BeginRenameEntry/BeginCreateEntry immediately before
	//! TreeView_EditLabel.  TVN_BEGINLABELEDIT cancels any edit this tool did
	//! not arm, so TVS_EDITLABELS never accepts an ad-hoc label edit.
	bool labelEditArmed{};
	bool labelEditIsCreate{};
	bool createEntryIsDirectory{};
	//! Rename: the edited entry's full path.  Create: the parent directory.
	std::wstring labelEditPath;
	//! The temporary lParam-0 row an inline create edits.  It never becomes a
	//! real node: the filesystem is the truth and the watcher-driven refresh
	//! renders the committed outcome.
	HTREEITEM createEditItem{};
	std::vector<std::unique_ptr<WorkerResult>> deferredResults;

	[[nodiscard]] int ScaleDip(int value) const noexcept
	{
		return std::max(1, ::MulDiv(value, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 96));
	}

	[[nodiscard]] int HeaderHeight() const noexcept { return ScaleDip(kPaneHeaderHeightDip); }

	void ApplyTreeFontAndMetrics()
	{
		if (tree == nullptr) return;
		if (font.Dpi() != dpi) (void)font.Recreate(theme::ThemeFontKind::Chrome, dpi);
		if (font.Get() != nullptr) (void)::SendMessageW(tree, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), TRUE);
		TreeView_SetItemHeight(tree, ScaleDip(kTreeRowHeightDip));
		TreeView_SetIndent(tree, ScaleDip(12));
	}

	void UpdateHeaderActionRects()
	{
		for (auto& actionBounds : headerActionRects) actionBounds = RECT{};
		if (window == nullptr || root.empty()) return;
		RECT client{};
		if (!::GetClientRect(window, &client)) return;
		const int side = ScaleDip(kHeaderActionSideDip);
		const int gap = ScaleDip(kHeaderActionGapDip);
		int right = static_cast<int>(client.right) - ScaleDip(kHeaderInsetDip);
		const int top = std::max(0, (HeaderHeight() - side) / 2);
		for (std::size_t position = headerActionRects.size(); position != 0; --position) {
			const std::size_t index = position - 1;
			const int left = std::max(0, right - side);
			headerActionRects[index] = RECT{ left, top, right, top + side };
			right = left - gap;
		}
	}

	[[nodiscard]] int HeaderActionAt(POINT point) const noexcept
	{
		if (root.empty()) return -1;
		for (std::size_t index = 0; index < headerActionRects.size(); ++index) {
			const auto& actionBounds = headerActionRects[index];
			if (point.x >= actionBounds.left && point.x < actionBounds.right
				&& point.y >= actionBounds.top && point.y < actionBounds.bottom) {
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	void UpdateHeaderHover(POINT point)
	{
		const int hovered = HeaderActionAt(point);
		if (hoveredHeaderAction != hovered) {
			hoveredHeaderAction = hovered;
			if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
		}
		if (hovered >= 0 && !trackingHeaderMouseLeave && window != nullptr) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
			trackingHeaderMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
	}

	void LayoutEmptyState()
	{
		openFolderButton = RECT{};
		addFolderButton = RECT{};
		welcomeMessageRect = RECT{};
		welcomeBlocks = BuildExplorerWelcomeBlocks(welcomeState);
		welcomeBlockRects.assign(welcomeBlocks.size(), RECT{});
		if (window == nullptr || !root.empty()) return;
		RECT client{};
		if (!::GetClientRect(window, &client)) return;
		const int clientWidth = std::max(0L, client.right - client.left);
		const int inset = views::WelcomeHorizontalInset(dpi);
		const int messageWidth = std::max(0, clientWidth - inset * 2);
		if (messageWidth <= 0) return;
		const int buttonWidth = views::WelcomeButtonColumnWidth(messageWidth, dpi);
		const int messageLeft = static_cast<int>(client.left) + inset;
		const int buttonLeft = messageLeft + (messageWidth - buttonWidth) / 2;
		const int bodyTop = HeaderHeight();
		const int buttonHeight = views::WelcomeButtonHeight(dpi);
		const HDC dc = ::GetDC(window);
		if (dc != nullptr) {
			const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
			TEXTMETRICW metrics{};
			(void)::GetTextMetricsW(dc, &metrics);
			const int em = std::max(1, static_cast<int>(metrics.tmHeight));
			int cursorTop = bodyTop + em;
			for (std::size_t index = 0; index < welcomeBlocks.size(); ++index) {
				const auto& block = welcomeBlocks[index];
				if (block.kind == ExplorerWelcomeBlockKind::Paragraph) {
					RECT measured{ messageLeft, cursorTop, messageLeft + messageWidth, cursorTop };
					(void)::DrawTextW(dc, WelcomeParagraphText(block.paragraph), -1, &measured,
						DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
					welcomeBlockRects[index] = measured;
					cursorTop = measured.bottom + em;
				} else {
					welcomeBlockRects[index] = RECT{ buttonLeft, cursorTop, buttonLeft + buttonWidth, cursorTop + buttonHeight };
					cursorTop = welcomeBlockRects[index].bottom + em;
				}
			}
			if (previousFont != nullptr) ::SelectObject(dc, previousFont);
			::ReleaseDC(window, dc);
		}
		for (std::size_t index = 0; index < welcomeBlocks.size(); ++index) {
			if (welcomeBlocks[index].kind != ExplorerWelcomeBlockKind::Action) continue;
			if (welcomeBlocks[index].action == ExplorerWelcomeAction::OpenFolder) openFolderButton = welcomeBlockRects[index];
			if (welcomeBlocks[index].action == ExplorerWelcomeAction::AddFolder) addFolderButton = welcomeBlockRects[index];
		}
	}

	void LayoutChildren()
	{
		if (window == nullptr) return;
		RECT client{};
		if (!::GetClientRect(window, &client)) return;
		UpdateHeaderActionRects();
		LayoutEmptyState();
		if (tree != nullptr) {
			const int top = HeaderHeight();
			::SetWindowPos(tree, nullptr, client.left, top,
				std::max(0L, client.right - client.left),
				std::max(0L, client.bottom - top),
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
			::ShowWindow(tree, !root.empty() && filesPaneExpanded ? SW_SHOWNOACTIVATE : SW_HIDE);
		}
		// UpdateOverlayScrollbar keys the overlay off the tree's own WS_VISIBLE, so
		// hiding the tree above already withdraws the scrollbar.
		UpdateOverlayScrollbar();
	}

	//! The whole pane header is the twistie's click target, as it is in VS Code;
	//! the action buttons claim their own rectangles before this runs.
	void ToggleFilesPane()
	{
		if (closed || window == nullptr) return;
		filesPaneExpanded = !filesPaneExpanded;
		LayoutChildren();
		::InvalidateRect(window, nullptr, FALSE);
	}

	void InvokeHeaderAction(int index)
	{
		if (closed || !commandCallback || index < 0
			|| static_cast<std::size_t>(index) >= kHeaderActions.size()) return;
		(void)commandCallback(kHeaderActions[static_cast<std::size_t>(index)].commandId, "[]");
	}

	void InvokeOpenFolder()
	{
		if (!closed && commandCallback) (void)commandCallback(kOpenFolderCommandId, "[]");
	}

	void InvokeAddFolder()
	{
		if (!closed && commandCallback) (void)commandCallback(kAddRootFolderCommandId, "[]");
	}
	void InvokeCloneRepository()
	{
		if (!closed && commandCallback) (void)commandCallback(kCloneRepositoryCommandId, "[]");
	}

	void UpdateEmptyStateHover(POINT point)
	{
		const std::size_t hovered = root.empty() ? WelcomeActionAt(point) : 0;
		if (hoveredWelcomeBlock == hovered) return;
		hoveredWelcomeBlock = hovered;
		if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
	}
	[[nodiscard]] std::size_t WelcomeActionAt(POINT point) const noexcept
	{
		for (std::size_t index = 0; index < welcomeBlocks.size(); ++index) {
			if (welcomeBlocks[index].kind == ExplorerWelcomeBlockKind::Action
				&& ::PtInRect(&welcomeBlockRects[index], point)) return index + 1;
		}
		return 0;
	}

	void PaintHeader(HDC dc)
	{
		if (dc == nullptr || window == nullptr) return;
		RECT header{};
		if (!::GetClientRect(window, &header)) return;
		header.bottom = std::min<LONG>(header.bottom, HeaderHeight());
		if (header.bottom <= header.top) return;
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
		const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF previousTextColor = ::SetTextColor(dc, palette.text);
		// VS Code's pane header opens with a twistie that discloses the pane body.
		const int twistieSide = ScaleDip(kHeaderTwistieSideDip);
		const RECT twistie{ header.left + ScaleDip(kHeaderTwistieInsetDip),
			header.top + std::max(0L, (header.bottom - header.top - twistieSide) / 2),
			header.left + ScaleDip(kHeaderTwistieInsetDip) + twistieSide,
			header.top + std::max(0L, (header.bottom - header.top - twistieSide) / 2) + twistieSide };
		DrawExplorerIcon(dc, twistie, filesPaneExpanded ? L"chevron-down" : L"chevron-right", palette.text);
		RECT title{ header.left + ScaleDip(kHeaderTitleInsetDip), header.top,
			header.right - ScaleDip(kHeaderInsetDip), header.bottom };
		if (!root.empty() && headerActionRects.front().right > headerActionRects.front().left) {
			title.right = std::max(title.left, headerActionRects.front().left - ScaleDip(4));
		}
		const std::wstring titleText = root.empty() ? std::wstring(LS(STR_WORKBENCH_EXPLORER_NO_FOLDER_OPENED)) : CExplorerTool::WorkspaceDisplayName(root);
		::DrawTextW(dc, titleText.c_str(), static_cast<int>(titleText.size()), &title,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		if (!root.empty()) {
			for (std::size_t index = 0; index < headerActionRects.size(); ++index) {
				const RECT& action = headerActionRects[index];
				if (action.right <= action.left || action.bottom <= action.top) continue;
				if (hoveredHeaderAction == static_cast<int>(index)) {
					const HBRUSH brush = ::CreateSolidBrush(palette.hover);
					if (brush != nullptr) {
						::FillRect(dc, &action, brush);
						::DeleteObject(brush);
					}
				}
				const int side = std::min(IconSizeForDpi(dpi), std::min(
					static_cast<int>(action.right - action.left), static_cast<int>(action.bottom - action.top)));
				const int left = static_cast<int>(action.left) +
					(static_cast<int>(action.right - action.left) - side) / 2;
				const int top = static_cast<int>(action.top) +
					(static_cast<int>(action.bottom - action.top) - side) / 2;
				DrawExplorerIcon(dc, RECT{ left, top, left + side, top + side },
					kHeaderActions[index].iconId, palette.secondaryText);
			}
		}
		::SetTextColor(dc, previousTextColor);
		::SetBkMode(dc, previousBackgroundMode);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	}

	void PaintEmptyState(HDC dc)
	{
		if (dc == nullptr || !root.empty() || !filesPaneExpanded) return;
		LayoutEmptyState();
		const HGDIOBJ previousFont = font.Get() == nullptr ? nullptr : ::SelectObject(dc, font.Get());
		const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF previousTextColor = ::SetTextColor(dc, palette.secondaryText);
		auto drawButton = [&](const RECT& button, const wchar_t* label, std::size_t index) {
			if (button.right <= button.left || button.bottom <= button.top) return;
			const bool hovered = hoveredWelcomeBlock == index + 1;
			const HBRUSH fill = ::CreateSolidBrush(hovered ? palette.buttonHover : palette.button);
			const HPEN border = ::CreatePen(PS_SOLID, 1, hovered ? palette.buttonHover : palette.button);
			if (fill != nullptr && border != nullptr) {
				const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
				const HGDIOBJ previousPen = ::SelectObject(dc, border);
				const int radius = views::WelcomeButtonCornerRadius(dpi);
				::RoundRect(dc, button.left, button.top, button.right, button.bottom, radius, radius);
				::SelectObject(dc, previousPen);
				::SelectObject(dc, previousBrush);
			}
			if (border != nullptr) ::DeleteObject(border);
			if (fill != nullptr) ::DeleteObject(fill);
			::SetTextColor(dc, palette.buttonText);
			RECT labelRect = button;
			::DrawTextW(dc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		};
		for (std::size_t index = 0; index < welcomeBlocks.size(); ++index) {
			const auto& block = welcomeBlocks[index];
			if (block.kind == ExplorerWelcomeBlockKind::Paragraph) {
			RECT paragraph = welcomeBlockRects[index];
			::SetTextColor(dc, palette.secondaryText);
			::DrawTextW(dc, WelcomeParagraphText(block.paragraph), -1, &paragraph,
				DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
			} else {
				const wchar_t* label = block.action == ExplorerWelcomeAction::OpenFolder ? LS(STR_WORKBENCH_EXPLORER_OPEN_FOLDER)
					: block.action == ExplorerWelcomeAction::AddFolder ? (welcomeState == ExplorerWelcomeState::EmptyWorkspace
						? LS(STR_WORKBENCH_EXPLORER_ADD_FOLDER_TO_WORKSPACE) : LS(STR_WORKBENCH_EXPLORER_ADD_FOLDER))
					: LS(STR_WORKBENCH_GIT_CLONE_REPOSITORY);
				drawButton(welcomeBlockRects[index], label, index);
			}
		}
		::SetTextColor(dc, previousTextColor);
		::SetBkMode(dc, previousBackgroundMode);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	}

	/*!
		@brief Scrolls the tree so that `target` is its first visible row

		This is the overlay's one way back into the tree. A TreeView has no
		"set top row" message, so the row is reached by walking the visible items.
	*/
	void SetFirstVisibleRow(int target)
	{
		if (tree == nullptr) return;
		auto item = TreeView_GetRoot(tree);
		for (int index = 0; item != nullptr && index < std::max(0, target); ++index) {
			item = TreeView_GetNextVisible(tree, item);
		}
		if (item != nullptr) (void)TreeView_SelectSetFirstVisible(tree, item);
	}

	//! Feeds the overlay the current DPI and palette, then lets it place itself.
	void UpdateOverlayScrollbar()
	{
		scrollbar.SetDpi(dpi == 0 ? kDefaultDpi : dpi);
		scrollbar.SetColors(controls::OverlayScrollbarColors{ palette.background,
			palette.scrollbarTrackHover, palette.scrollbarThumb, palette.scrollbarThumbHover,
			palette.scrollbarThumbActive });
		scrollbar.Update();
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
		// During a label edit the edit control owns the keyboard; these
		// selection-scoped keys act only on the quiescent tree, matching VS
		// Code's F2 / Delete / Shift+Delete Explorer keybindings.
		if (impl != nullptr && message == WM_KEYDOWN && !impl->labelEditActive) {
			if (wParam == VK_F2) {
				impl->DispatchSelectionKeyCommand(commands::kRenameFileCommandId);
				return 0;
			}
			if (wParam == VK_DELETE) {
				impl->DispatchSelectionKeyCommand((::GetKeyState(VK_SHIFT) & 0x8000) != 0
					? commands::kDeleteFileCommandId : commands::kMoveFileToTrashCommandId);
				return 0;
			}
		}
		HTREEITEM releasedItem = nullptr;
		HTREEITEM pressedItem = nullptr;
		if (impl != nullptr) {
			if (message == WM_MOUSEMOVE) {
				const auto hovered = impl->HitTestFileActivationItem(
					POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
				if (impl->pointerHoverItem != hovered) {
					impl->pointerHoverItem = hovered;
					::InvalidateRect(hwnd, nullptr, FALSE);
				}
				if (!impl->trackingTreeMouseLeave) {
					TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 };
					impl->trackingTreeMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
				}
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
			} else if (message == WM_MOUSELEAVE) {
				impl->trackingTreeMouseLeave = false;
				if (impl->pointerHoverItem != nullptr) {
					impl->pointerHoverItem = nullptr;
					::InvalidateRect(hwnd, nullptr, FALSE);
				}
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
			|| message == TVM_INSERTITEMW || message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
			impl->UpdateOverlayScrollbar();
			if (message == WM_SETFOCUS || message == WM_KILLFOCUS) ::InvalidateRect(hwnd, nullptr, FALSE);
		}
		return result;
	}

	void StartWorker()
	{
		if (worker.joinable()) return;
		auto retirement = workbench::WorkerRetirementService::Instance().TryReserve();
		if (!retirement) return;
		worker = std::thread(WorkerMain, shared);
		workerRetirement.emplace(std::move(*retirement));
	}

	void StopWorker()
	{
		if (!worker.joinable()) return;
		shared->state.store(ExplorerWorkerState::CancelRequested, std::memory_order_release);
		::SetEvent(shared->stopEvent);
		::SetEvent(shared->wakeEvent);
		if (workerRetirement) {
			(void)workbench::WorkerRetirementService::Instance().Retire(
				std::move(worker), std::move(*workerRetirement), shared);
			workerRetirement.reset();
		}
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

	[[nodiscard]] Node* FindNodeByPath(std::wstring_view path)
	{
		for (auto& [id, node] : nodes) {
			(void)id;
			if (::CompareStringOrdinal(node.path.c_str(), static_cast<int>(node.path.size()),
					path.data(), static_cast<int>(path.size()), TRUE) == CSTR_EQUAL) {
				return &node;
			}
		}
		return nullptr;
	}

	//! Null for an unknown item and for the lParam-0 rows (the enumeration
	//! placeholder and an in-progress create's temporary row).
	[[nodiscard]] Node* FindNodeByItem(HTREEITEM item)
	{
		if (tree == nullptr || item == nullptr) return nullptr;
		TVITEMW info{};
		info.mask = TVIF_PARAM;
		info.hItem = item;
		if (!TreeView_GetItem(tree, &info) || info.lParam == 0) return nullptr;
		return FindNode(static_cast<std::uint64_t>(info.lParam));
	}

	//! Serializes the resource as the one-URI wire payload and invokes the
	//! stable command.  A path the URI boundary rejects dispatches nothing.
	void DispatchResourceCommand(std::string_view commandId, const std::wstring& path)
	{
		if (closed || !commandCallback || path.empty()) return;
		const auto uri = platform::uri::Uri::FromWindowsPath(path);
		if (!uri) return;
		const commands::ExplorerResourceArguments arguments{ uri.value->ToString() };
		(void)commandCallback(commandId, commands::BuildExplorerResourceArguments(arguments));
	}

	//! F2/Delete route: acts on the selection.  The workspace root is excluded
	//! exactly as upstream's `ExplorerRootContext.toNegated()` excludes it from
	//! rename and both deletions.
	void DispatchSelectionKeyCommand(std::string_view commandId)
	{
		if (tree == nullptr) return;
		const auto* node = FindNodeByItem(TreeView_GetSelection(tree));
		if (node == nullptr || node->isWorkspaceRoot) return;
		DispatchResourceCommand(commandId, node->path);
	}

	//! WM_CONTEXTMENU route.  A keyboard menu request (lParam -1) anchors on
	//! the selected row; a pointer request resolves the clicked row and selects
	//! it first so the row the user sees and the row the command receives are
	//! the same row; empty space targets the workspace root, as upstream's
	//! empty-area Explorer menu does, without moving the selection.
	void ShowContextMenu(LPARAM lParam)
	{
		if (closed || tree == nullptr || window == nullptr || labelEditActive
			|| !commandCallback || !menuTitleResolver) {
			return;
		}
		Node* node = nullptr;
		POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (lParam == -1) {
			const auto selected = TreeView_GetSelection(tree);
			node = FindNodeByItem(selected);
			if (node == nullptr) return;
			RECT itemRect{};
			if (!TreeView_GetItemRect(tree, selected, &itemRect, TRUE)) return;
			screen = POINT{ itemRect.left, itemRect.bottom };
			if (!::ClientToScreen(tree, &screen)) return;
		} else {
			POINT client = screen;
			if (!::ScreenToClient(tree, &client)) return;
			TVHITTESTINFO hit{};
			hit.pt = client;
			const auto item = TreeView_HitTest(tree, &hit);
			if (item != nullptr) {
				node = FindNodeByItem(item);
				if (node == nullptr) return;
				(void)TreeView_SelectItem(tree, item);
			} else {
				// Empty space targets the workspace root, which no longer owns a row.
				node = FindNode(workspaceRootNodeId);
				if (node == nullptr) return;
			}
		}
		const auto kind = node->isWorkspaceRoot ? EExplorerResourceKind::WorkspaceRoot
			: node->isDirectory ? EExplorerResourceKind::Folder
			: EExplorerResourceKind::File;
		// Only the local Win32 filesystem is served here and every local
		// resource can reach the Recycle Bin, so the menu carries the trash
		// item; the permanent deletion remains reachable through Shift+Delete
		// and the trash-failure fallback prompt.
		const auto rows = BuildExplorerContextMenuRows(kind, true);
		// A copy, not a reference: `TrackPopupMenu` pumps messages, so a worker
		// result could reconcile the node map while the menu is open.
		const std::wstring resourcePath = node->path;
		node = nullptr;

		std::vector<std::string_view> commandIds;
		const HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		for (const auto& row : rows) {
			if (row.Kind() == EExplorerContextMenuRowKind::Separator) {
				(void)::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				continue;
			}
			const std::wstring title = menuTitleResolver(row.CommandId());
			if (title.empty()) {
				// A command the registry cannot title means the registration
				// this menu depends on is absent.  Fail closed rather than
				// render a menu that promises a command that cannot run.
				::DestroyMenu(menu);
				return;
			}
			commandIds.push_back(row.CommandId());
			(void)::AppendMenuW(menu, MF_STRING,
				static_cast<UINT_PTR>(commandIds.size()), title.c_str());
		}

		// The owning window must be foreground or the menu never sees its own
		// dismissal; the trailing `WM_NULL` is that requirement's second half.
		::SetForegroundWindow(window);
		const auto chosen = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
			screen.x, screen.y, 0, window, nullptr);
		::DestroyMenu(menu);
		(void)::PostMessageW(window, WM_NULL, 0, 0);
		if (chosen <= 0 || static_cast<std::size_t>(chosen) > commandIds.size()) return;
		DispatchResourceCommand(commandIds[static_cast<std::size_t>(chosen) - 1], resourcePath);
	}

	bool BeginRename(std::wstring_view path)
	{
		if (closed || tree == nullptr || labelEditActive) return false;
		const auto* node = FindNodeByPath(path);
		if (node == nullptr || node->isWorkspaceRoot) return false;
		const auto item = node->item;
		const std::wstring name = node->name;
		const bool isDirectory = node->isDirectory;
		(void)TreeView_SelectItem(tree, item);
		::SetFocus(tree);
		labelEditArmed = true;
		labelEditIsCreate = false;
		labelEditPath.assign(path);
		const HWND edit = TreeView_EditLabel(tree, item);
		if (edit == nullptr) {
			labelEditArmed = false;
			labelEditActive = false;
			labelEditPath.clear();
			return false;
		}
		// VS Code preselects a file's base name without its extension (a
		// leading dot is not an extension separator); a folder selects fully.
		LPARAM selectionEnd = -1;
		if (!isDirectory) {
			const auto dot = name.find_last_of(L'.');
			if (dot != std::wstring::npos && dot != 0) selectionEnd = static_cast<LPARAM>(dot);
		}
		(void)::SendMessageW(edit, EM_SETSEL, 0, selectionEnd);
		return true;
	}

	bool BeginCreate(std::wstring_view parentDirectory, bool directory)
	{
		if (closed || tree == nullptr || labelEditActive) return false;
		const auto* parent = FindNodeByPath(parentDirectory);
		if (parent == nullptr || !parent->isDirectory || parent->isReparsePoint) return false;
		const auto parentItem = parent->item;
		const std::wstring parentPath = parent->path;
		(void)TreeView_Expand(tree, parentItem, TVE_EXPAND);
		TVINSERTSTRUCTW insert{};
		insert.hParent = parentItem;
		insert.hInsertAfter = TVI_FIRST;
		insert.item.mask = TVIF_TEXT | TVIF_PARAM;
		insert.item.pszText = const_cast<wchar_t*>(L"");
		insert.item.lParam = 0;
		const auto item = TreeView_InsertItem(tree, &insert);
		if (item == nullptr) return false;
		createEditItem = item;
		createEntryIsDirectory = directory;
		labelEditIsCreate = true;
		labelEditPath = parentPath;
		(void)TreeView_SelectItem(tree, item);
		::SetFocus(tree);
		labelEditArmed = true;
		if (TreeView_EditLabel(tree, item) == nullptr) {
			labelEditArmed = false;
			labelEditActive = false;
			labelEditIsCreate = false;
			createEntryIsDirectory = false;
			labelEditPath.clear();
			(void)TreeView_DeleteItem(tree, createEditItem);
			createEditItem = nullptr;
			return false;
		}
		return true;
	}

	bool BeginCreateFromSelection(bool directory)
	{
		if (closed || tree == nullptr || root.empty()) return false;
		const HTREEITEM selected = TreeView_GetSelection(tree);
		const Node* node = selected != nullptr ? FindNodeByItem(selected) : FindNode(workspaceRootNodeId);
		if (node == nullptr) node = FindNode(workspaceRootNodeId);
		if (node == nullptr) return false;
		if (node->isDirectory) return BeginCreate(node->path, directory);
		const auto separator = node->path.find_last_of(L"\\/");
		if (separator == std::wstring::npos) return false;
		return BeginCreate(std::wstring_view(node->path).substr(0, separator), directory);
	}

	//! TVN_ENDLABELEDIT terminal for both the rename and the create edit.  A
	//! null `text` is a cancelled edit.  The label is never applied here: the
	//! commit callbacks reach the filesystem boundary, and the watcher-driven
	//! refresh renders whatever the filesystem now holds.
	void FinishLabelEdit(const wchar_t* text)
	{
		if (!labelEditActive) return;
		labelEditActive = false;
		const bool isCreate = labelEditIsCreate;
		labelEditIsCreate = false;
		const bool directory = createEntryIsDirectory;
		createEntryIsDirectory = false;
		const std::wstring path = std::move(labelEditPath);
		labelEditPath.clear();
		if (createEditItem != nullptr) {
			if (tree != nullptr) (void)TreeView_DeleteItem(tree, createEditItem);
			createEditItem = nullptr;
		}
		if (closed) {
			deferredResults.clear();
			return;
		}
		DispatchDeferredResults();
		if (text == nullptr) return;
		const std::wstring_view entered(text);
		// An invalid name is no commit; the divergence from upstream's inline
		// error message is recorded in this directory's CLAUDE.md.
		if (!IsValidExplorerEntryName(entered)) return;
		if (isCreate) {
			if (createCommit) createCommit(path, entered, directory);
			return;
		}
		const auto separator = path.find_last_of(L"\\/");
		const std::wstring_view previousName = separator == std::wstring::npos
			? std::wstring_view(path) : std::wstring_view(path).substr(separator + 1);
		// Only the exact same name is a no-op: a case-differing entry is a
		// legitimate case-only rename on this case-preserving filesystem.
		if (entered == previousName) return;
		if (renameCommit) renameCommit(path, entered);
	}

	void DispatchDeferredResults()
	{
		auto results = std::move(deferredResults);
		deferredResults.clear();
		for (auto& result : results) ApplyResult(std::move(result));
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
		// TVS_LINESATROOT is what gives a top-level row its disclosure column; it
		// carries no lines of its own while TVS_HASLINES is off.  The tree's top
		// level is the open folder's children, so top-level folders need that column
		// exactly as nested ones do.
		style |= TVS_HASBUTTONS | TVS_LINESATROOT;
		// VS Code's Explorer uses disclosure chevrons, not the legacy dotted
		// connector network exposed by the default Win32 TreeView style.
		style &= ~TVS_HASLINES;
		::SetWindowLongPtrW(tree, GWL_STYLE, style);
		::SetWindowPos(tree, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	[[nodiscard]] int ResolveIcon(const Node& node, bool expanded)
	{
		(void)node;
		(void)expanded;
		// A transparent image reserves the native TreeView's icon slot. The real
		// glyph is painted in NM_CUSTOMDRAW so it stays theme-aware without
		// importing a second file-icon theme parser.
		//
		// Every row gets the slot, including a folder row the Seti theme does not
		// decorate. That is a known divergence from VS Code with a platform cause:
		// a native TreeView reserves the image width for every item as long as it
		// has an image list, and I_IMAGENONE does not opt one item out. Dropping
		// the slot per row needs the row drawn by hand; see explorer/CLAUDE.md.
		return iconImages == nullptr ? -1 : 0;
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
			if (node.item == nullptr) continue;
			const bool expanded = tree != nullptr
				&& (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
			UpdateNodeIcon(node, expanded);
		}
	}

	void RebuildIconImages()
	{
		DestroyIconImages();
		ApplyArrowVisibility();
		if (tree == nullptr) return;
		const int side = IconSizeForDpi(dpi);
		iconImages = ::ImageList_Create(side, side, ILC_COLOR32 | ILC_MASK, 1, 1);
		if (iconImages != nullptr) {
			const HBITMAP transparent = CreateTransparentBitmap(side);
			const HBITMAP mask = CreateSpacerMaskBitmap(side);
			if (transparent == nullptr || mask == nullptr
				|| ::ImageList_Add(iconImages, transparent, mask) == -1) {
				::ImageList_Destroy(iconImages);
				iconImages = nullptr;
			} else {
				TreeView_SetImageList(tree, iconImages, TVSIL_NORMAL);
			}
			if (transparent != nullptr) ::DeleteObject(transparent);
			if (mask != nullptr) ::DeleteObject(mask);
		}
		UpdateAllItemIcons();
	}

	/*!
		@brief Upstream's `getDecoration(uri, includeChildren)` for one row.

		Only a folder collects its descendants, because only a folder has any. The
		workspace-root node is never rendered, so it is not asked.
	*/
	[[nodiscard]] std::optional<decorations::ResolvedFileDecoration> DecorationFor(const Node& node) const
	{
		if (fileDecorations.Empty() || node.path.empty()) return std::nullopt;
		if (!decorationOptions.colors && !decorationOptions.badges) return std::nullopt;
		return fileDecorations.Resolve(node.path, node.isDirectory);
	}

	//! The decoration's label color, or nothing when the row must keep the view's.
	[[nodiscard]] std::optional<COLORREF> DecorationTextColor(
		const std::optional<decorations::ResolvedFileDecoration>& decoration) const
	{
		if (!decoration || !decorationOptions.colors) return std::nullopt;
		if (decoration->color == decorations::EFileDecorationColor::None) return std::nullopt;
		return palette.decorationColors[static_cast<std::size_t>(decoration->color)];
	}

	/*!
		@brief Draws the decoration badge at the row's right edge.

		A row whose decoration came from its descendants draws upstream's bubble
		codicon instead of a letter, because the letters describe resources this row
		is not. Both are composited over the row's own fill, since `iconlabel.css`
		draws them at an opacity GDI cannot carry in a text color.
	*/
	void PaintNodeBadge(HDC dc, const Node& node,
		const decorations::ResolvedFileDecoration& decoration, COLORREF rowBackground) const
	{
		if (dc == nullptr || tree == nullptr || node.item == nullptr) return;
		if (!decorationOptions.badges) return;
		const COLORREF base = decorationOptions.colors
				&& decoration.color != decorations::EFileDecorationColor::None
			? palette.decorationColors[static_cast<std::size_t>(decoration.color)]
			: palette.text;
		RECT row{};
		if (!TreeView_GetItemRect(tree, node.item, &row, FALSE)) return;
		RECT label{};
		if (!TreeView_GetItemRect(tree, node.item, &label, TRUE)) return;
		if (decoration.containsChildren) {
			const int side = ScaleDip(kDecorationBubbleSideDip);
			const int right = static_cast<int>(row.right) - ScaleDip(kDecorationBubbleRightInsetDip);
			const int top = static_cast<int>(row.top)
				+ std::max(0, (static_cast<int>(row.bottom - row.top) - side) / 2);
			if (right - side <= label.right) return;
			const RECT bubble{ right - side, top, right, top + side };
			DrawExplorerIcon(dc, bubble, L"circle-filled",
				BlendColor(base, rowBackground, kDecorationBubbleOpacity));
			return;
		}
		if (decoration.badge.empty()) return;
		const HFONT badgeFont = CreateBadgeFont();
		const HFONT previousFont = badgeFont == nullptr
			? nullptr : static_cast<HFONT>(::SelectObject(dc, badgeFont));
		RECT box{ std::max(static_cast<int>(label.right) + ScaleDip(kDecorationBadgeLeftGapDip),
				static_cast<int>(row.left)),
			row.top, std::max(static_cast<int>(row.left),
				static_cast<int>(row.right) - ScaleDip(kDecorationBadgeRightInsetDip)),
			row.bottom };
		if (box.right > box.left) {
			const int previousMode = ::SetBkMode(dc, TRANSPARENT);
			const COLORREF previousColor = ::SetTextColor(dc,
				BlendColor(base, rowBackground, kDecorationBadgeOpacity));
			::DrawTextW(dc, decoration.badge.c_str(), static_cast<int>(decoration.badge.size()), &box,
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			::SetTextColor(dc, previousColor);
			::SetBkMode(dc, previousMode);
		}
		if (badgeFont != nullptr) {
			(void)::SelectObject(dc, previousFont);
			::DeleteObject(badgeFont);
		}
	}

	//! `font-size: 90%; font-weight: 600` applied to the row font.
	[[nodiscard]] HFONT CreateBadgeFont() const
	{
		LOGFONTW description{};
		const HFONT source = font.Get();
		if (source == nullptr || ::GetObjectW(source, sizeof(description), &description) == 0) {
			return nullptr;
		}
		description.lfHeight = ::MulDiv(description.lfHeight, kDecorationBadgeFontPermille, 1000);
		description.lfWeight = FW_SEMIBOLD;
		return ::CreateFontIndirectW(&description);
	}

	//! The fill the row actually shows, which is what a translucent badge sits on.
	[[nodiscard]] COLORREF RowBackground(bool selected, bool hovered) const noexcept
	{
		if (selected) return ::GetFocus() == tree ? palette.focus : palette.inactiveSelection;
		return hovered ? palette.hover : palette.background;
	}

	void PaintNodeIcon(HDC dc, const Node& node) const
	{
		if (dc == nullptr || tree == nullptr || node.item == nullptr) return;
		RECT label{};
		if (!TreeView_GetItemRect(tree, node.item, &label, TRUE)) return;
		const int side = IconSizeForDpi(dpi);
		const int top = static_cast<int>(label.top) +
			std::max(0, (static_cast<int>(label.bottom - label.top) - side) / 2);
		const RECT box{ std::max(0, static_cast<int>(label.left) - side - ScaleDip(2)), top,
			std::max(0, static_cast<int>(label.left) - ScaleDip(2)), top + side };
		if (icons::CSetiFont::Instance().IsAvailable()) {
			// The bundled `vs-seti` theme, which is what VS Code selects by default.
			// It contributes no folder association at all, so a directory row draws
			// its twistie and its name and no glyph, exactly as upstream does.
			// The `light` section belongs to ColorThemeKind.Light alone, which is a
			// property of the active color theme rather than of this palette, so it
			// is read from the theme service the way the syntax overlay is.
			const auto icon = icons::seti::ResolveSetiFileIcon(node.name, node.isDirectory,
				theme::CThemeService::IsActiveColorThemeLightKind()
					? icons::seti::EIconVariant::Light
					: icons::seti::EIconVariant::Dark);
			if (!icon) return;
			DrawSetiIcon(dc, box, icon->character,
				icon->color == icons::seti::kInheritColor
					? palette.text
					: ColorRefFromThemeRgb(icon->color));
			return;
		}
		// seti.ttf could not be registered. Its code points mean nothing in another
		// face, so fall back to the first-party Codicon table instead of drawing
		// unrelated glyphs.
		const bool expanded = (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
		const std::wstring_view iconId = ResolveExplorerFileIconCodicon(
			node.name, node.isDirectory, expanded, node.isWorkspaceRoot);
		DrawExplorerIcon(dc, box, iconId, palette.text);
	}

	/*!
		@brief Draws a folder row without the icon slot the control reserves for it

		VS Code's default `vs-seti` icon theme contributes no folder association, so
		upstream leaves `hasFolderIcons` false and a folder row is its twistie and its
		name with nothing in between. A native TreeView cannot express that through the
		item model: it reserves the image-list width for every item, and `I_IMAGENONE`
		does not opt one item out. Reclaiming the slot therefore requires drawing the
		row by hand under `CDRF_SKIPDEFAULT`, which is what this does.

		Only folder rows take this path. A file row's label already sits exactly one
		icon width right of its twistie, which is where upstream puts it too, so it
		keeps the control's own drawing. A file row also has no twistie, so the chevron
		drawn here is the only disclosure glyph in the view and no row is left without
		one; `CDRF_SKIPDEFAULT` suppresses the control's button glyph, not its
		button hit region, so expanding by clicking the chevron is unaffected.

		The label is placed at the reserved slot's left edge rather than being nudged
		in post-paint over text the control already drew, and the highlight keeps the
		control's own width so a selected folder and a selected file still match.
	*/
	[[nodiscard]] bool PaintDirectoryRow(HDC dc, const Node& node, bool selected, bool hovered) const
	{
		if (dc == nullptr || tree == nullptr || node.item == nullptr) return false;
		RECT text{};
		RECT row{};
		if (!TreeView_GetItemRect(tree, node.item, &text, TRUE)) return false;
		if (!TreeView_GetItemRect(tree, node.item, &row, FALSE)) return false;
		const int side = IconSizeForDpi(dpi);
		const int labelLeft = std::max(0, static_cast<int>(text.left) - side);
		const bool focused = ::GetFocus() == tree;
		const COLORREF background = selected
			? (focused ? palette.focus : palette.inactiveSelection)
			: (hovered ? palette.hover : palette.background);
		// `iconlabel.css` gives a focused list's selected row `color: inherit
		// !important`, so a decoration never repaints the label there; everywhere
		// else the decoration's color wins over the view's text color.
		const auto decoration = DecorationFor(node);
		const auto decorated = DecorationTextColor(decoration);
		const COLORREF foreground = selected && focused
			? palette.selectionText
			: (decorated ? *decorated : palette.text);
		if (const HBRUSH brush = ::CreateSolidBrush(palette.background); brush != nullptr) {
			::FillRect(dc, &row, brush);
			::DeleteObject(brush);
		}
		if (background != palette.background) {
			const RECT fill{ labelLeft, row.top,
				labelLeft + static_cast<int>(text.right - text.left), row.bottom };
			if (const HBRUSH brush = ::CreateSolidBrush(background); brush != nullptr) {
				::FillRect(dc, &fill, brush);
				::DeleteObject(brush);
			}
		}
		const bool expanded = (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
		const int chevron = ScaleDip(12);
		const int chevronTop = static_cast<int>(row.top)
			+ std::max(0, static_cast<int>(row.bottom - row.top) - chevron) / 2;
		// The glyph sits back from the label by upstream's own twistie whitespace:
		// `.monaco-tl-twistie` is a 16-DIP box holding a chevron that inks about
		// two thirds of it, so the name never touches the glyph that discloses it.
		const int gap = ScaleDip(4);
		const RECT twistie{ std::max(0, labelLeft - chevron - gap), chevronTop,
			std::max(0, labelLeft - gap), chevronTop + chevron };
		DrawExplorerIcon(dc, twistie, expanded ? L"chevron-down" : L"chevron-right", foreground);
		RECT label{ labelLeft, row.top, std::max(labelLeft, static_cast<int>(row.right)), row.bottom };
		const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
		const COLORREF previousTextColor = ::SetTextColor(dc, foreground);
		::DrawTextW(dc, node.name.c_str(), static_cast<int>(node.name.size()), &label,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		::SetTextColor(dc, previousTextColor);
		::SetBkMode(dc, previousBackgroundMode);
		if (decoration) PaintNodeBadge(dc, node, *decoration, background);
		return true;
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
		// Every inserted row is a descendant of the workspace root; the root itself
		// is the item-less node PopulateRoot synthesizes.
		node.isWorkspaceRoot = false;
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
		hoveredHeaderAction = -1;
		hoveredWelcomeBlock = 0;
		TreeView_DeleteAllItems(tree);
		nodes.clear();
		workspaceRootNodeId = 0;
		if (root.empty()) {
			::ShowWindow(tree, SW_HIDE);
			LayoutChildren();
			if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
			return;
		}
		::ShowWindow(tree, SW_SHOWNOACTIVATE);
		// The tree starts at the root folder's children.  In VS Code the file view's
		// pane header names the open folder and the tree below it lists that folder's
		// contents; a row for the folder itself appears only in a multi-root
		// workspace, one row per root.  So the root here is a node with no item, and
		// its enumeration inserts directly under TVI_ROOT.
		Node rootNode;
		rootNode.id = nextNodeId++;
		rootNode.name = CExplorerTool::WorkspaceDisplayName(root);
		rootNode.path = root;
		rootNode.isDirectory = true;
		rootNode.isWorkspaceRoot = true;
		workspaceRootNodeId = rootNode.id;
		const auto [it, inserted] = nodes.emplace(rootNode.id, std::move(rootNode));
		if (inserted) QueueEnumeration(it->second, false);
		LayoutChildren();
		if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
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
		if (tree == nullptr || labelEditActive) return;
		for (auto& [id, node] : nodes) {
			(void)id;
			// The item-less workspace root is always disclosed: its children are the
			// tree's top level, so it refreshes unconditionally.
			if (node.item == nullptr) {
				if (node.isWorkspaceRoot) QueueEnumeration(node, true);
				continue;
			}
			if (TreeView_GetItemState(tree, node.item, TVIS_EXPANDED) & TVIS_EXPANDED) QueueEnumeration(node, true);
		}
	}

	void CollapseTreeItem(HTREEITEM item)
	{
		if (tree == nullptr || item == nullptr) return;
		for (auto child = TreeView_GetChild(tree, item); child != nullptr; child = TreeView_GetNextSibling(tree, child)) {
			CollapseTreeItem(child);
		}
		(void)TreeView_Expand(tree, item, TVE_COLLAPSE);
	}

	void CollapseAll()
	{
		if (closed || tree == nullptr) return;
		expandedPaths.clear();
		// Every top-level row is a root-folder child now, so all of them collapse.
		for (auto item = TreeView_GetRoot(tree); item != nullptr; item = TreeView_GetNextSibling(tree, item)) {
			CollapseTreeItem(item);
		}
		UpdateOverlayScrollbar();
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
		// A null item is the workspace root, whose children are the tree's top level.
		const auto parentItem = node->item;
		const auto insertParent = parentItem == nullptr ? TVI_ROOT : parentItem;
		const auto parentNodeId = node->id;
		struct ExistingChild {
			HTREEITEM item{};
			std::uint64_t nodeId{};
		};
		std::map<std::wstring, ExistingChild, ExplorerPathLess> existing;
		std::vector<HTREEITEM> placeholders;
		for (auto child = parentItem == nullptr ? TreeView_GetRoot(tree) : TreeView_GetChild(tree, parentItem);
			child != nullptr; child = TreeView_GetNextSibling(tree, child)) {
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
			const auto inserted = InsertNode(insertParent, std::move(entry), insertAfter);
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
				RDW_INVALIDATE | RDW_NOERASE | RDW_FRAME | RDW_ALLCHILDREN);
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
	if (!EnsureExplorerClass(instance)) return false;
	INITCOMMONCONTROLSEX common{};
	common.dwSize = sizeof(common);
	common.dwICC = ICC_TREEVIEW_CLASSES;
	(void)::InitCommonControlsEx(&common);
	m_impl->window = ::CreateWindowExW(0, kExplorerWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_impl->window == nullptr) return false;
	m_impl->dpi = std::max(1u, ::GetDpiForWindow(m_impl->window));
	m_impl->tree = ::CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_NOHSCROLL |
		TVS_EDITLABELS,
		0, 0, 0, 0, m_impl->window, nullptr, instance, nullptr);
	if (m_impl->tree == nullptr) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		return false;
	}
	Impl* const impl = m_impl.get();
	if (!m_impl->scrollbar.Create(m_impl->window, m_impl->tree,
			[impl](int topRow) { impl->SetFirstVisibleRow(topRow); })
		|| !::SetWindowSubclass(m_impl->tree, Impl::TreeSubclassProc, 1,
			reinterpret_cast<DWORD_PTR>(m_impl.get()))) {
		m_impl->scrollbar.Destroy();
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		m_impl->tree = nullptr;
		return false;
	}
	::SendMessageW(m_impl->tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
	TreeView_SetBkColor(m_impl->tree, m_impl->palette.background);
	TreeView_SetTextColor(m_impl->tree, m_impl->palette.text);
	m_impl->ApplyTreeFontAndMetrics();
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
	if (dpiChanged) {
		m_impl->ApplyTreeFontAndMetrics();
		m_impl->RebuildIconImages();
	}
	if (m_impl->window != nullptr) {
		::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top,
			std::max(0L, contentRect.right - contentRect.left),
			std::max(0L, contentRect.bottom - contentRect.top),
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
		m_impl->LayoutChildren();
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
	if (m_impl->closed || m_impl->tree == nullptr) return false;
	if (m_impl->labelEditActive) {
		// Commit/cancel the inline edit deterministically before downstream
		// accelerator translation can consume Enter or Escape.
		const HWND edit = TreeView_GetEditControl(m_impl->tree);
		if (edit != nullptr && message.hwnd == edit && message.message == WM_KEYDOWN
			&& (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) {
			(void)TreeView_EndEditLabelNow(m_impl->tree, message.wParam == VK_ESCAPE);
			return true;
		}
		return false;
	}
	if (!m_impl->active) return false;
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
	(void)m_impl->nativeSurface.Close();
	m_impl->DestroyIconImages();
	m_impl->scrollbar.Destroy();
	if (m_impl->window != nullptr && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->tree = nullptr;
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

void CExplorerTool::SetFilesPaneExpanded(const bool expanded)
{
	if (m_impl->closed || m_impl->filesPaneExpanded == expanded) return;
	m_impl->filesPaneExpanded = expanded;
	m_impl->LayoutChildren();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

bool CExplorerTool::IsFilesPaneExpanded() const noexcept
{
	return m_impl->filesPaneExpanded;
}

void CExplorerTool::SetWelcomeState(ExplorerWelcomeState state)
{
	if (m_impl->welcomeState == state) return;
	m_impl->welcomeState = state;
	if (m_impl->window != nullptr) {
		m_impl->LayoutChildren();
		::InvalidateRect(m_impl->window, nullptr, FALSE);
	}
}

ExplorerWelcomeState CExplorerTool::GetWelcomeState() const noexcept
{
	return m_impl->welcomeState;
}

void CExplorerTool::SetFileActivationCallback(FileActivationCallback callback)
{
	m_impl->activateFile = std::move(callback);
}

void CExplorerTool::SetCommandCallback(CommandCallback callback)
{
	m_impl->commandCallback = std::move(callback);
}

void CExplorerTool::SetMenuTitleResolver(MenuTitleResolver resolver)
{
	m_impl->menuTitleResolver = std::move(resolver);
}

void CExplorerTool::SetRenameCommitCallback(RenameCommitCallback callback)
{
	m_impl->renameCommit = std::move(callback);
}

void CExplorerTool::SetCreateCommitCallback(CreateCommitCallback callback)
{
	m_impl->createCommit = std::move(callback);
}

bool CExplorerTool::BeginRenameEntry(std::wstring_view path)
{
	return m_impl->BeginRename(path);
}

bool CExplorerTool::BeginCreateEntry(std::wstring_view parentDirectory, bool directory)
{
	return m_impl->BeginCreate(parentDirectory, directory);
}

bool CExplorerTool::CreateEntryFromSelection(bool directory)
{
	return m_impl->BeginCreateFromSelection(directory);
}

void CExplorerTool::Refresh()
{
	if (!m_impl->closed) m_impl->QueueExpandedNodes();
}

void CExplorerTool::RefreshStrings()
{
	if (m_impl->closed) return;
	m_impl->LayoutChildren();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CExplorerTool::CollapseAllFolders()
{
	if (!m_impl->closed) m_impl->CollapseAll();
}

void CExplorerTool::SetPalette(ExplorerPalette palette)
{
	m_impl->palette = palette;
	if (m_impl->tree != nullptr) {
		TreeView_SetBkColor(m_impl->tree, palette.background);
		TreeView_SetTextColor(m_impl->tree, palette.text);
	}
	m_impl->RebuildIconImages();
	if (m_impl->window != nullptr) ::InvalidateRect(m_impl->window, nullptr, FALSE);
	m_impl->UpdateOverlayScrollbar();
}

ExplorerPalette CExplorerTool::GetPalette() const noexcept { return m_impl->palette; }

void CExplorerTool::SetFileDecorations(decorations::FileDecorationTable decorations)
{
	m_impl->fileDecorations = std::move(decorations);
	// Every row can change at once, because the whole table was replaced. The tree
	// keeps its items; only what they paint differs.
	if (m_impl->tree != nullptr) ::InvalidateRect(m_impl->tree, nullptr, FALSE);
}

void CExplorerTool::SetDecorationOptions(ExplorerDecorationOptions options)
{
	if (m_impl->decorationOptions == options) return;
	m_impl->decorationOptions = options;
	if (m_impl->tree != nullptr) ::InvalidateRect(m_impl->tree, nullptr, FALSE);
}

ExplorerDecorationOptions CExplorerTool::GetDecorationOptions() const noexcept
{
	return m_impl->decorationOptions;
}
ExplorerWorkerState CExplorerTool::GetWorkerState() const noexcept { return m_impl->shared->state.load(std::memory_order_acquire); }
HWND CExplorerTool::GetHwnd() const noexcept { return m_impl->window; }

void CExplorerTool::SetNativeSurfaceSink(
	rendering::FrameNativeSurfacePayloadSink sink) noexcept
{
	m_impl->nativeSurface.SetSink(std::move(sink));
}

rendering::FrameNativeSurfacePayloadResult CExplorerTool::RegisterNativeSurface(
	const rendering::FrameNativeSurfacePayloadTarget& target) noexcept
{
	return m_impl->nativeSurface.Register(target);
}

rendering::FrameNativeSurfacePayloadResult CExplorerTool::UpdateNativeSurface(
	const rendering::FrameNativeSurfacePayloadTarget& target) noexcept
{
	return m_impl->nativeSurface.Update(target);
}

rendering::FrameNativeSurfacePayloadResult CExplorerTool::SubmitNativeSurface(
	const HDC sourceDc, const RECT& dirtyRect) noexcept
{
	return m_impl->nativeSurface.Submit(sourceDc, dirtyRect);
}

rendering::FrameNativeSurfacePayloadResult CExplorerTool::CloseNativeSurface() noexcept
{
	return m_impl->nativeSurface.Close();
}

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
	// VS Code's Explorer header uses the workspace folder's display label; it
	// must preserve the filesystem's casing rather than turn a local path into
	// an unrelated all-caps surrogate.
	return std::wstring(root.substr(start, end - start));
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
		impl.LayoutChildren();
		(void)::RedrawWindow(window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		return 0;
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (point.y < impl.HeaderHeight()) {
			impl.UpdateHeaderHover(point);
		} else if (impl.hoveredHeaderAction != -1) {
			impl.hoveredHeaderAction = -1;
			::InvalidateRect(window, nullptr, FALSE);
		}
		if (impl.root.empty() && impl.filesPaneExpanded) {
			impl.UpdateEmptyStateHover(point);
			if (impl.hoveredWelcomeBlock != 0 && !impl.trackingHeaderMouseLeave) {
				TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
				impl.trackingHeaderMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
			}
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		impl.trackingHeaderMouseLeave = false;
		if (impl.hoveredHeaderAction != -1 || impl.hoveredWelcomeBlock != 0) {
			impl.hoveredHeaderAction = -1;
			impl.hoveredWelcomeBlock = 0;
			::InvalidateRect(window, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (point.y >= 0 && point.y < impl.HeaderHeight()) {
			impl.headerClickArmed = true;
			impl.armedHeaderAction = impl.HeaderActionAt(point);
			(void)::SetCapture(window);
			return 0;
		}
		break;
	}
	case WM_LBUTTONUP: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (impl.headerClickArmed) {
			const int armedAction = impl.armedHeaderAction;
			impl.headerClickArmed = false;
			impl.armedHeaderAction = -1;
			if (::GetCapture() == window) (void)::ReleaseCapture();
			const int releasedAction = impl.HeaderActionAt(point);
			if (armedAction >= 0 && releasedAction == armedAction) {
				impl.InvokeHeaderAction(armedAction);
			} else if (armedAction < 0 && releasedAction < 0
				&& point.y >= 0 && point.y < impl.HeaderHeight()) {
				impl.ToggleFilesPane();
			}
			return 0;
		}
		if (impl.root.empty() && impl.filesPaneExpanded) {
			for (std::size_t index = 0; index < impl.welcomeBlocks.size(); ++index) {
				if (impl.welcomeBlocks[index].kind != ExplorerWelcomeBlockKind::Action
					|| !::PtInRect(&impl.welcomeBlockRects[index], point)) continue;
				switch (impl.welcomeBlocks[index].action) {
				case ExplorerWelcomeAction::OpenFolder: impl.InvokeOpenFolder(); break;
				case ExplorerWelcomeAction::AddFolder: impl.InvokeAddFolder(); break;
				case ExplorerWelcomeAction::CloneRepository: impl.InvokeCloneRepository(); break;
				}
				return 0;
			}
		}
		break;
	}
	case WM_CANCELMODE:
		impl.headerClickArmed = false;
		impl.armedHeaderAction = -1;
		if (::GetCapture() == window) (void)::ReleaseCapture();
		return 0;
	case WM_CAPTURECHANGED:
		impl.headerClickArmed = false;
		impl.armedHeaderAction = -1;
		break;
	case WM_SETCURSOR: {
		POINT point{};
		if (::GetCursorPos(&point) && ::ScreenToClient(window, &point)
			&& (impl.HeaderActionAt(point) >= 0
			|| (impl.root.empty() && impl.WelcomeActionAt(point) != 0))) {
			::SetCursor(::LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC target = ::BeginPaint(window, &paint);
		if (target != nullptr) {
			RECT client{};
			(void)::GetClientRect(window, &client);
			const int width = std::max(0L, client.right - client.left);
			const int height = std::max(0L, client.bottom - client.top);
			const bool buffered = impl.windowBuffer.Ensure(target, width, height);
			const HDC dc = buffered ? impl.windowBuffer.Dc() : target;
			const HBRUSH brush = ::CreateSolidBrush(impl.palette.background);
			if (brush != nullptr) {
				::FillRect(dc, &client, brush);
				::DeleteObject(brush);
			}
			impl.PaintHeader(dc);
			impl.PaintEmptyState(dc);
			if (buffered) (void)impl.windowBuffer.Present(target, paint.rcPaint);
			(void)impl.nativeSurface.Submit(dc, paint.rcPaint);
		}
		::EndPaint(window, &paint);
		return 0;
	}
	case kWorkerResultMessage: {
		auto result = impl.TakePendingResult(reinterpret_cast<WorkerResult*>(lParam));
		if (impl.labelEditActive) {
			// Reconciliation destroys and inserts TreeView items; doing that
			// under a live label edit could destroy the edited item.  Hold the
			// result until the edit ends.
			if (result) impl.deferredResults.emplace_back(std::move(result));
			return 0;
		}
		impl.ApplyResult(std::move(result));
		return 0;
	}
	case WM_CONTEXTMENU:
		if (reinterpret_cast<HWND>(wParam) == impl.tree) {
			impl.ShowContextMenu(lParam);
			return 0;
		}
		break;
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
		if (notification->code == TVN_BEGINLABELEDITW) {
			// TVS_EDITLABELS exists solely so TreeView_EditLabel works; an
			// edit this tool did not arm (a stray click-pause edit) is
			// cancelled by returning TRUE.
			if (!impl.labelEditArmed) return TRUE;
			impl.labelEditArmed = false;
			impl.labelEditActive = true;
			return FALSE;
		}
		if (notification->code == TVN_ENDLABELEDITW) {
			impl.FinishLabelEdit(reinterpret_cast<const NMTVDISPINFOW*>(lParam)->item.pszText);
			// FALSE: the tree never applies the entered label itself.  The
			// filesystem is the truth and the watcher-driven refresh renders
			// the committed outcome.
			return FALSE;
		}
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
			if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
				const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
				const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
				const bool hovered = !selected && impl.pointerHoverItem == item;
				// A folder row draws itself so its label can reclaim the icon slot the
				// control reserves but the Seti theme never fills; see PaintDirectoryRow.
				if (auto* directory = impl.FindNodeByItem(item);
					directory != nullptr && directory->isDirectory
					&& impl.PaintDirectoryRow(draw->nmcd.hdc, *directory, selected, hovered)) {
					return CDRF_SKIPDEFAULT;
				}
				auto* const node = impl.FindNodeByItem(item);
				const auto decoration = node == nullptr
					? std::nullopt : impl.DecorationFor(*node);
				const auto decorated = impl.DecorationTextColor(decoration);
				const bool focused = ::GetFocus() == impl.tree;
				if (selected) {
					draw->clrText = focused ? impl.palette.selectionText : impl.palette.text;
					draw->clrTextBk = focused ? impl.palette.focus : impl.palette.inactiveSelection;
				} else if (hovered) {
					draw->clrText = impl.palette.text;
					draw->clrTextBk = impl.palette.hover;
				}
				// A focused list's selected row keeps `color: inherit !important`;
				// every other row takes the decoration's color.
				if (decorated && !(selected && focused)) draw->clrText = *decorated;
				return CDRF_NOTIFYPOSTPAINT
					| (selected || hovered || decorated ? CDRF_NEWFONT : 0);
			}
			if (draw->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
				const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
				if (auto* node = impl.FindNodeByItem(item); node != nullptr) {
					impl.PaintNodeIcon(draw->nmcd.hdc, *node);
					// The control drew this row, so the badge follows it rather than
					// being drawn into a fill the control is about to overwrite.
					if (const auto decoration = impl.DecorationFor(*node); decoration) {
						const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
						impl.PaintNodeBadge(draw->nmcd.hdc, *node, *decoration,
							impl.RowBackground(selected, !selected && impl.pointerHoverItem == item));
					}
				}
				return CDRF_DODEFAULT;
			}
		}
		break;
	}
	case WM_NCDESTROY:
		impl.SetNotificationWindow(nullptr, false);
		impl.window = nullptr;
		impl.tree = nullptr;
		// The overlay is this window's child: it is already gone by now.
		impl.scrollbar.Detach();
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::explorer
