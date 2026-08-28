/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalWnd.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/window/TerminalBuiltinGlyphRenderer.h"
#include "terminal/window/TerminalClipboardPaste.h"
#include "terminal/window/TerminalColorResolver.h"
#include "terminal/window/TerminalDWriteRenderer.h"
#include "terminal/window/TerminalFontMetrics.h"
#include "terminal/window/TerminalInput.h"
#include "terminal/window/TerminalRenderPlan.h"
#include "terminal/window/TerminalRenderMapping.h"
#include "terminal/window/TerminalScrollbarLayout.h"
#include "terminal/window/TerminalNativeFrameBridge.h"
#include "terminal/TerminalWorkerRetirementService.h"
#include "theme/CThemeService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windowsx.h>
#include <imm.h>

namespace terminal {
namespace {

constexpr wchar_t kTerminalWindowClass[] = L"SakuraNativeTerminalWindow";
constexpr unsigned int kDefaultDpi = 96;
constexpr UINT_PTR kInputRetryTimer = 0x5345;
constexpr UINT kInputRetryMilliseconds = 30;
constexpr UINT kNativeFrameReadyMessage = WM_APP + 0x5346;
constexpr std::size_t kPendingInteractiveInputLimit = CTerminalSession::kInputLimitBytes;
constexpr std::size_t kTerminalFallbackFontCount = 5;
constexpr WORD kMissingGlyph = 0xffff;
constexpr std::size_t kTerminalPrimaryCoverageCacheLimit = 512;
constexpr std::size_t kTerminalFallbackCoverageCacheLimit = 512;
constexpr std::size_t kTerminalBackBufferBytesPerPixel = sizeof(std::uint32_t);

static_assert(kTerminalBackBufferBytesPerPixel == 4U);

//! GDI does not accept a CSS-style font stack.  Keep the terminal's primary
//! face fixed, then select a real installed face per grapheme when the primary
//! face has no glyph.  This is the native equivalent of xterm.js's browser
//! font fallback for symbols, CJK, and emoji.
bool FontContainsText( HDC dc, HFONT candidate, std::wstring_view text ) noexcept
{
	if( dc == nullptr || candidate == nullptr || text.empty() ) return candidate != nullptr;
	// TerminalModel stores at most twelve UTF-16 code units in one cell.  Keep
	// this probe allocation-free so a repaint does not allocate per visible cell.
	std::array<WORD, 16> glyphs{};
	if( text.size() > glyphs.size() ) return false;
	const auto previous = ::SelectObject(dc, candidate);
	if( previous == nullptr || previous == HGDI_ERROR ) return false;
	const auto count = ::GetGlyphIndicesW(dc, text.data(), static_cast<int>(text.size()), glyphs.data(),
		GGI_MARK_NONEXISTING_GLYPHS);
	::SelectObject(dc, previous);
	if( count == GDI_ERROR || count != text.size() ) return false;
	return std::all_of(glyphs.begin(), glyphs.begin() + static_cast<std::ptrdiff_t>(count),
		[](WORD glyph) { return glyph != kMissingGlyph; });
}

bool EnsureTerminalClass( HINSTANCE instance )
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = CTerminalWnd::WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_IBEAM);
	windowClass.lpszClassName = kTerminalWindowClass;
	if( ::RegisterClassExW(&windowClass) != 0 ) return true;
	return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ReadNativeImeResult( HWND window, std::wstring& result )
{
	const HIMC context = ::ImmGetContext(window);
	if( context == nullptr ) return false;
	const LONG byteCount = ::ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
	if( byteCount < 0 || (byteCount % static_cast<LONG>(sizeof(wchar_t))) != 0 ) {
		::ImmReleaseContext(window, context);
		return false;
	}
	try {
		result.assign(static_cast<std::size_t>(byteCount) / sizeof(wchar_t), L'\0');
	} catch( ... ) {
		::ImmReleaseContext(window, context);
		return false;
	}
	if( byteCount == 0 ) {
		::ImmReleaseContext(window, context);
		return true;
	}
	const LONG copied = ::ImmGetCompositionStringW(context, GCS_RESULTSTR, result.data(), byteCount);
	::ImmReleaseContext(window, context);
	if( copied < 0 || copied > byteCount || (copied % static_cast<LONG>(sizeof(wchar_t))) != 0 ) return false;
	result.resize(static_cast<std::size_t>(copied) / sizeof(wchar_t));
	return true;
}

TerminalKeyEvent KeyEventFromMessage( const MSG& message ) noexcept
{
	TerminalKeyEvent event;
	event.virtualKey = static_cast<std::uint32_t>(message.wParam);
	event.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	event.control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	// The context-code bit belongs to this queued keyboard message.  Reading
	// process-global key state here can attach a stale Alt modifier to a later
	// printable WM_KEYDOWN and produce an unexpected ESC-prefixed character.
	event.alt = (static_cast<ULONG_PTR>(message.lParam) & (1ULL << 29)) != 0;
	event.scanCode = static_cast<std::uint16_t>((message.lParam >> 16) & 0xff);
	event.repeatCount = static_cast<std::uint16_t>(message.lParam & 0xffff);
	event.keyDown = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
	event.enhanced = (message.lParam & (1LL << 24)) != 0;
	event.capsLock = (::GetKeyState(VK_CAPITAL) & 1) != 0;
	event.numLock = (::GetKeyState(VK_NUMLOCK) & 1) != 0;
	event.rightAlt = (::GetKeyState(VK_RMENU) & 0x8000) != 0;
	event.rightControl = (::GetKeyState(VK_RCONTROL) & 0x8000) != 0;
	return event;
}

bool MapsToCharacter( std::uint32_t virtualKey ) noexcept
{
	// Only keys that carry a character in the current layout qualify. A bare modifier,
	// VK_APPS, and the VK_PROCESSKEY an IME sends while composing all map to zero here
	// and keep their existing routing.
	return ::MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_CHAR) != 0;
}

std::string EncodeAltPrintable( const MSG& message )
{
	BYTE keyboardState[256]{};
	if( !::GetKeyboardState(keyboardState) ) return {};
	wchar_t text[8]{};
	const auto scanCode = static_cast<UINT>((message.lParam >> 16) & 0xff);
	const auto count = ::ToUnicodeEx(static_cast<UINT>(message.wParam), scanCode, keyboardState, text,
		static_cast<int>(std::size(text)), 0, ::GetKeyboardLayout(0));
	if( count <= 0 ) return {};
	auto result = EncodeTerminalText(std::wstring_view(text, static_cast<std::size_t>(count)));
	result.insert(result.begin(), '\x1b');
	return result;
}

//! A UI-owned DIB capture is handed to one fixed worker slot. The worker does
//! only CPU conversion and publishes an immutable compact dirty payload to a
//! UI-message mailbox; only that UI message drains the runtime bridge, and the
//! presentation owner remains the sole caller of D3D/DComp APIs.
struct TerminalNativeSurfaceCapture final {
	workbench::rendering::FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t displayEpoch = 1;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t pitch = 0;
	RECT dirtyRect{};
	// The vector contains only dirtyRect.height rows. Its pitch is the compact
	// source pitch (dirtyRect.width * 4), so no per-frame full-surface allocation
	// is needed for a row or rectangle update.
	std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

class TerminalNativeSurfacePublisher final {
public:
	TerminalNativeSurfacePublisher() = default;
	~TerminalNativeSurfacePublisher() noexcept
	{
		Close();
	}

	TerminalNativeSurfacePublisher(const TerminalNativeSurfacePublisher&) = delete;
	TerminalNativeSurfacePublisher& operator=(const TerminalNativeSurfacePublisher&) = delete;

	[[nodiscard]] bool Start() noexcept
	{
		if (m_state != nullptr || m_worker.joinable()) return true;
		try {
			auto reservation = terminal::TerminalWorkerRetirementService::Instance().TryReserve();
			if (!reservation) return false;
			auto state = std::make_shared<State>();
			m_state = state;
			m_reservation = std::move(reservation);
			try {
				m_worker = std::thread([state] { WorkerMain(std::move(state)); });
			} catch (...) {
				m_reservation.reset();
				m_state.reset();
				return false;
			}
			return true;
		} catch (...) {
			return false;
		}
	}

	[[nodiscard]] bool Started() const noexcept
	{
		return m_state != nullptr;
	}

	void SetNotifyWindow(HWND window) noexcept
	{
		if (m_state == nullptr) return;
		std::lock_guard lock(m_state->mutex);
		if (m_state->closed) return;
		m_state->notifyWindow = window;
	}

	[[nodiscard]] std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame>
	TakeReady() noexcept
	{
		if (m_state == nullptr) return {};
		std::lock_guard lock(m_state->mutex);
		m_state->notifyPosted = false;
		return std::exchange(m_state->ready, {});
	}

	[[nodiscard]] bool Submit(TerminalNativeSurfaceCapture capture) noexcept
	{
		if (m_state == nullptr || !capture.pixels) return false;
		try {
			std::lock_guard lock(m_state->mutex);
			if (m_state->closed) return false;
			// Latest-only admission keeps the worker and runtime mailbox bounded;
			// a slow GPU owner never grows terminal-side work.
			m_state->pending = std::move(capture);
			m_state->wake.notify_one();
			return true;
		} catch (...) {
			return false;
		}
	}

	void Close() noexcept
	{
		auto state = std::exchange(m_state, {});
		if (state == nullptr) return;
		{
			std::lock_guard lock(state->mutex);
			state->closed = true;
			state->pending.reset();
			state->ready.reset();
			state->notifyWindow = nullptr;
			state->notifyPosted = false;
		}
		state->wake.notify_one();
		if (!m_worker.joinable()) {
			m_reservation.reset();
			return;
		}

		// Never join from a terminal window destructor. Transfer ownership to the
		// fixed reaper; if its queue is temporarily full, use the admitted task
		// fallback rather than detaching or leaking a live std::thread.
		auto worker = std::move(m_worker);
		if (!m_reservation) {
			std::terminate();
		}
		auto reservation = std::move(*m_reservation);
		m_reservation.reset();
		const auto lifetime = std::static_pointer_cast<void>(state);
		const auto retired = terminal::TerminalWorkerRetirementService::Instance().Retire(
			std::move(worker), std::move(reservation), lifetime);
		if (retired == terminal::TerminalWorkerRetirementStatus::Retired) return;
		// Retire did not consume a valid reservation or the worker handle, so the
		// same bounded reservation can carry the join task.
		std::shared_ptr<std::thread> workerOwner;
		try {
			workerOwner = std::make_shared<std::thread>(std::move(worker));
		} catch (...) {
			std::terminate();
		}
		const auto taskStatus = terminal::TerminalWorkerRetirementService::Instance().RetireTask(
			std::move(reservation),
			[workerOwner]() mutable {
				if (workerOwner->joinable()) workerOwner->join();
			}, lifetime);
		if (taskStatus != terminal::TerminalWorkerRetirementStatus::Retired) {
			std::terminate();
		}
	}

private:
	struct State final {
		std::mutex mutex;
		std::condition_variable wake;
		bool closed = false;
		std::optional<TerminalNativeSurfaceCapture> pending;
		std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame> ready;
		HWND notifyWindow = nullptr;
		bool notifyPosted = false;
	};

	static std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame>
	BuildFrame(const TerminalNativeSurfaceCapture& capture) noexcept
	{
		if (!capture.pixels || capture.width == 0 || capture.height == 0) return {};
		try {
			const RECT full{ 0, 0, static_cast<LONG>(capture.width),
				static_cast<LONG>(capture.height) };
			RECT dirty = capture.dirtyRect;
			if (dirty.left == 0 && dirty.top == 0 && dirty.right == 0 && dirty.bottom == 0) {
				dirty = full;
			}
			if (dirty.left < full.left || dirty.top < full.top
				|| dirty.right > full.right || dirty.bottom > full.bottom
				|| dirty.left >= dirty.right || dirty.top >= dirty.bottom) return {};
			const auto dirtyWidth = static_cast<std::uint32_t>(dirty.right - dirty.left);
			const auto dirtyHeight = static_cast<std::uint32_t>(dirty.bottom - dirty.top);
			if (dirtyWidth > (std::numeric_limits<std::uint32_t>::max)() / 4u
				|| capture.pitch == 0 || capture.pitch < dirtyWidth * 4u
				|| dirtyHeight > (std::numeric_limits<std::size_t>::max)() / capture.pitch
				|| static_cast<std::size_t>(capture.pitch) * dirtyHeight > capture.pixels->size()) {
				return {};
			}
			auto pixels = std::make_shared<std::vector<std::uint8_t>>(
				static_cast<std::size_t>(capture.pitch) * dirtyHeight, 0);
			for (std::uint32_t row = 0; row < dirtyHeight; ++row) {
				const auto* source = capture.pixels->data()
					+ static_cast<std::size_t>(row) * capture.pitch;
				auto* destination = pixels->data()
					+ static_cast<std::size_t>(row) * capture.pitch;
				for (std::uint32_t column = 0; column < dirtyWidth; ++column) {
					// BI_RGB 32-bit DIBs are BGRX. Terminal pixels are opaque;
					// explicitly supply alpha 255 to make the payload premultiplied.
					destination[0] = source[0];
					destination[1] = source[1];
					destination[2] = source[2];
					destination[3] = 0xff;
					source += 4;
					destination += 4;
				}
			}
			return std::make_shared<const workbench::rendering::FrameNativeSurfaceFrame>(
				workbench::rendering::FrameNativeSurfaceFrame{
					.surfaceId = capture.surfaceId,
					.surfaceLifetimeEpoch = capture.surfaceLifetimeEpoch,
					.deviceEpoch = capture.deviceEpoch,
					.displayEpoch = capture.displayEpoch,
					.layoutEpoch = capture.layoutEpoch,
					.requestId = capture.requestId,
					.width = capture.width,
					.height = capture.height,
					.pitch = capture.pitch,
					.dirtyRect = dirty,
					.compactDirtyPayload = true,
					.pixels = std::move(pixels),
				});
		} catch (...) {
			return {};
		}
	}

	static void WorkerMain(std::shared_ptr<State> state) noexcept
	{
		for (;;) {
			std::optional<TerminalNativeSurfaceCapture> capture;
			{
				std::unique_lock lock(state->mutex);
				state->wake.wait(lock, [&state] {
					return state->closed || state->pending.has_value();
				});
				if (state->closed) return;
				capture = std::move(state->pending);
			}
			const auto frame = capture ? BuildFrame(*capture) : nullptr;
			if (!frame) continue;
			HWND notifyWindow = nullptr;
			{
				std::lock_guard lock(state->mutex);
				if (state->closed) return;
				state->ready = frame;
				if (!state->notifyPosted && state->notifyWindow != nullptr) {
					state->notifyPosted = true;
					notifyWindow = state->notifyWindow;
				}
			}
			if (notifyWindow != nullptr
				&& !::PostMessageW(notifyWindow, kNativeFrameReadyMessage, 0, 0)) {
				std::lock_guard lock(state->mutex);
				if (!state->closed) state->notifyPosted = false;
			}
		}
	}

	std::shared_ptr<State> m_state;
	std::thread m_worker;
	std::optional<terminal::TerminalWorkerRetirementService::Reservation> m_reservation;
};

} // namespace

struct CTerminalWnd::Impl final : ITerminalRenderClassifier {
	using NativeRegistration = workbench::rendering::FrameNativeSurfaceRegistration;
	using NativeFrame = workbench::rendering::FrameNativeSurfaceFrame;
	using FrameSnapshot = workbench::rendering::FrameSurfaceAdapterSnapshot;

	explicit Impl(const TerminalSurfaceAdapter::SurfaceId surfaceId) noexcept
		: frameSurface(surfaceId)
	{
	}

	HWND window{};
	HINSTANCE instance{};
	TerminalModel* model{};
	TerminalSurfaceAdapter frameSurface;
	SakuraTerminalInputAdapter* inputAdapter{};
	InputSink inputSink;
	ResizeSink resizeSink;
	FocusSink focusSink;
	CTerminalWnd::ImeResultReader imeResultReader;
	TerminalNativeFrameBridgePtr nativeFrameBridge;
	TerminalNativeSurfacePublisher nativeSurfacePublisher;
	std::function<void()> nativeFrameReadySink;
	HFONT font{};
	HFONT boldFont{};
	HDC backBufferDc{};
	HBITMAP backBufferBitmap{};
	HGDIOBJ backBufferOriginalBitmap{};
	std::uint32_t* backBufferBits{};
	std::ptrdiff_t backBufferStridePixels{};
	std::size_t backBufferByteCount{};
	std::array<HFONT, kTerminalFallbackFontCount> fallbackFonts{};
	std::array<HFONT, kTerminalFallbackFontCount> fallbackBoldFonts{};
	std::array<const wchar_t*, kTerminalFallbackFontCount> fallbackFontFamilies{};
	bool fallbackFontsCreated{};
	std::unordered_map<std::uint32_t, HFONT> glyphFontCache;
	std::unordered_map<std::uint32_t, bool> primaryGlyphCoverage;
	TerminalRenderPlan renderPlan;
	TerminalBuiltinGlyphRenderer builtinGlyphRenderer;
	TerminalDWriteRenderer dwriteRenderer;
	std::wstring terminalFontFamily;
	std::wstring terminalLocale;
	std::uint64_t rendererGeneration{ 1 };
	int terminalFontPixelHeight{ 1 };
	int terminalFontWeight{ FW_NORMAL };
	HDC classifierDc{};
	COLORREF paintDefaultBackground{};
	COLORREF paintDefaultForeground{};
	SIZE backBufferSize{};
	unsigned int dpi{ kDefaultDpi };
	int cellWidth{ 8 };
	int cellHeight{ 16 };
	std::size_t visibleRows{ 1 };
	std::size_t scrollOffset{};
	TerminalSize terminalSize{ 1, 1 };
	//! True once terminalSize came from a client rectangle with real extent.
	//! Until then it is only the placeholder a viewport starts with, and it must
	//! never be published to a session or a model.
	bool terminalSizeMeasured{};
	bool scrollbarHover{};
	bool scrollbarButtonPressed{};
	bool scrollbarDragging{};
	bool trackingScrollbarMouseLeave{};
	int scrollbarThumbGrabOffset{};
	unsigned int scrollbarSuppressedButtons{};
	bool selecting{};
	bool selectionMoved{};
	bool caretShown{};
	TerminalSelectionPoint selectionOrigin{};
	TerminalSelectionPoint selectionAnchor{};
	TerminalSelectionPoint selectionActive{};
	unsigned int pressedMouseButton{ 3 };
	wchar_t pendingHighSurrogate{};
	std::vector<std::uint8_t> pendingInteractiveInput;
	bool inputBackpressured{};
	bool inputRejected{};
	bool imeComposing{};
	bool closed{};
	bool nativeFullDirty{ true };
	RECT nativeDirtyRect{};
	// SetWindowPos normally delivers WM_SIZE synchronously. Layout resets this
	// marker so one public layout call advances the logical epoch once even
	// though the native message path also observes the resize.
	bool frameLayoutMessageSeen{};
	bool useTerminalProfileColors{ true };
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);

	void MarkNativeFullDirty() noexcept
	{
		nativeFullDirty = true;
		nativeDirtyRect = {};
	}

	void AccumulateNativeDirtyRect(const RECT& rectangle) noexcept
	{
		if (nativeFullDirty || rectangle.right <= rectangle.left
			|| rectangle.bottom <= rectangle.top) return;
		if (nativeDirtyRect.right <= nativeDirtyRect.left
			|| nativeDirtyRect.bottom <= nativeDirtyRect.top) {
			nativeDirtyRect = rectangle;
			return;
		}
		nativeDirtyRect.left = std::min(nativeDirtyRect.left, rectangle.left);
		nativeDirtyRect.top = std::min(nativeDirtyRect.top, rectangle.top);
		nativeDirtyRect.right = std::max(nativeDirtyRect.right, rectangle.right);
		nativeDirtyRect.bottom = std::max(nativeDirtyRect.bottom, rectangle.bottom);
	}

	[[nodiscard]] std::optional<NativeRegistration>
	NativeSurfaceRegistration() const noexcept
	{
		if (!window || !frameSurface.IsOpen() || !nativeFrameBridge) return std::nullopt;
		RECT client{};
		if (!::GetClientRect(window, &client)) return std::nullopt;
		const auto width = std::max<LONG>(1, client.right - client.left);
		const auto height = std::max<LONG>(1, client.bottom - client.top);
		const auto snapshot = frameSurface.Snapshot();
		if (snapshot.surfaceId == 0 || snapshot.surfaceLifetimeEpoch == 0
			|| snapshot.deviceEpoch == 0 || snapshot.layoutEpoch == 0) return std::nullopt;
		return NativeRegistration{
			.presentation = workbench::rendering::FramePresentationSurfaceSpec{
				.surfaceId = snapshot.surfaceId,
				.surfaceLifetimeEpoch = snapshot.surfaceLifetimeEpoch,
				.deviceEpoch = snapshot.deviceEpoch,
				.layoutEpoch = snapshot.layoutEpoch,
				.width = static_cast<std::uint32_t>(width),
				.height = static_cast<std::uint32_t>(height),
				.visible = snapshot.visible,
			},
			.targetWindow = window,
		};
	}

	void PublishNativeSurfaceRegistration(const bool update) noexcept
	{
		const auto registration = NativeSurfaceRegistration();
		if (!registration || !nativeFrameBridge) return;
		if (update) nativeFrameBridge->Update(*registration);
		else nativeFrameBridge->Register(*registration);
	}

	void SetNativeFrameBridge(TerminalNativeFrameBridgePtr bridge) noexcept
	{
		nativeFrameBridge = std::move(bridge);
		nativeSurfacePublisher.SetNotifyWindow(window);
		if (nativeFrameBridge) PublishNativeSurfaceRegistration(false);
	}

	void SetNativeFrameReadySink(std::function<void()> sink) noexcept
	{
		nativeFrameReadySink = std::move(sink);
	}

	void DrainNativeFrame() noexcept
	{
		if (!nativeFrameReadySink) return;
		nativeFrameReadySink();
	}

	[[nodiscard]] std::shared_ptr<const NativeFrame> TakeNativeFrame() noexcept
	{
		return nativeSurfacePublisher.TakeReady();
	}

	void CaptureNativeFrame(const FrameSnapshot& committed) noexcept
	{
		if (!nativeSurfacePublisher.Started() || !HasUsableBackBufferState()) return;
		RECT dirty = nativeFullDirty ? RECT{} : nativeDirtyRect;
		if (!nativeFullDirty && (dirty.right <= dirty.left || dirty.bottom <= dirty.top)) return;
		try {
			const auto width = static_cast<std::uint32_t>(backBufferSize.cx);
			const auto height = static_cast<std::uint32_t>(backBufferSize.cy);
			if (nativeFullDirty) dirty = RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
			dirty.left = std::clamp<LONG>(dirty.left, 0, static_cast<LONG>(width));
			dirty.top = std::clamp<LONG>(dirty.top, 0, static_cast<LONG>(height));
			dirty.right = std::clamp<LONG>(dirty.right, 0, static_cast<LONG>(width));
			dirty.bottom = std::clamp<LONG>(dirty.bottom, 0, static_cast<LONG>(height));
			if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return;
			const auto dirtyWidth = static_cast<std::size_t>(dirty.right - dirty.left);
			const auto dirtyHeight = static_cast<std::size_t>(dirty.bottom - dirty.top);
			if (dirtyWidth > (std::numeric_limits<std::uint32_t>::max)() / 4u
				|| dirtyWidth > (std::numeric_limits<std::size_t>::max)() / 4u) return;
			const auto pitch = static_cast<std::uint32_t>(dirtyWidth * 4u);
			if (dirtyHeight > (std::numeric_limits<std::size_t>::max)() / pitch) return;
			auto raw = std::make_shared<std::vector<std::uint8_t>>(
				dirtyHeight * pitch, 0);
			for (LONG y = dirty.top; y < dirty.bottom; ++y) {
				const auto* source = reinterpret_cast<const std::uint8_t*>(backBufferBits)
					+ static_cast<std::size_t>(y) * static_cast<std::size_t>(backBufferStridePixels) * 4u
					+ static_cast<std::size_t>(dirty.left) * 4u;
				auto* destination = raw->data()
					+ static_cast<std::size_t>(y - dirty.top) * pitch;
				std::memcpy(destination, source,
					static_cast<std::size_t>(dirty.right - dirty.left) * 4u);
			}
			const bool submitted = nativeSurfacePublisher.Submit(TerminalNativeSurfaceCapture{
				.surfaceId = committed.surfaceId,
				.surfaceLifetimeEpoch = committed.surfaceLifetimeEpoch,
				.deviceEpoch = committed.deviceEpoch,
				.displayEpoch = nativeFrameBridge ? nativeFrameBridge->DisplayEpoch() : 1,
				.layoutEpoch = committed.layoutEpoch,
				.requestId = committed.committedRequestId,
				.width = width,
				.height = height,
				.pitch = pitch,
				.dirtyRect = dirty,
				.pixels = std::move(raw),
			});
			if (submitted) {
				nativeFullDirty = false;
				nativeDirtyRect = {};
			}
		} catch (...) {
			// GDI remains authoritative until a complete native payload reaches
			// the runtime; a capture allocation failure must not damage the paint.
		}
	}

	void ReleaseRenderFonts() noexcept
	{
		if( font ) {
			::DeleteObject(font);
			font = nullptr;
		}
		if( boldFont ) {
			::DeleteObject(boldFont);
			boldFont = nullptr;
		}
		for( auto& fallback : fallbackFonts ) {
			if( fallback ) {
				::DeleteObject(fallback);
				fallback = nullptr;
			}
		}
		for( auto& fallback : fallbackBoldFonts ) {
			if( fallback ) {
				::DeleteObject(fallback);
				fallback = nullptr;
			}
		}
		fallbackFontsCreated = false;
		glyphFontCache.clear();
		primaryGlyphCoverage.clear();
	}

	bool AppendPendingInteractiveInput( std::span<const std::uint8_t> bytes )
	{
		if( bytes.size() > kPendingInteractiveInputLimit - pendingInteractiveInput.size() ) {
			inputRejected = true;
			inputBackpressured = false;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		pendingInteractiveInput.insert(pendingInteractiveInput.end(), bytes.begin(), bytes.end());
		inputBackpressured = true;
		if( window ) {
			::SetTimer(window, kInputRetryTimer, kInputRetryMilliseconds, nullptr);
			::InvalidateRect(window, nullptr, FALSE);
		}
		return true;
	}

	bool Send( std::string_view bytes )
	{
		if( bytes.empty() ) return true;
		const auto input = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
		if( !inputSink ) {
			inputRejected = true;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		if( !pendingInteractiveInput.empty() ) return AppendPendingInteractiveInput(input);
		switch( inputSink(input) ) {
		case TerminalQueueInputResult::Accepted:
			inputBackpressured = false;
			inputRejected = false;
			return true;
		case TerminalQueueInputResult::QueueFull:
			return AppendPendingInteractiveInput(input);
		case TerminalQueueInputResult::NotRunning:
			inputRejected = true;
			inputBackpressured = false;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		return false;
	}

	void RetryPendingInteractiveInput()
	{
		if( pendingInteractiveInput.empty() || !inputSink ) {
			if( window ) ::KillTimer(window, kInputRetryTimer);
			return;
		}
		switch( inputSink(pendingInteractiveInput) ) {
		case TerminalQueueInputResult::Accepted:
			pendingInteractiveInput.clear();
			inputBackpressured = false;
			inputRejected = false;
			if( window ) {
				::KillTimer(window, kInputRetryTimer);
				::InvalidateRect(window, nullptr, FALSE);
			}
			break;
		case TerminalQueueInputResult::QueueFull:
			// Keep the bounded buffer intact and retry on the same low-rate timer.
			inputBackpressured = true;
			break;
		case TerminalQueueInputResult::NotRunning:
			// Ownership ends with this viewport once the session has stopped.  The
			// visible warning records the rejected input; retaining it could leak a
			// large paste across a tab restart into a different process.
			pendingInteractiveInput.clear();
			inputBackpressured = false;
			inputRejected = true;
			if( window ) {
				::KillTimer(window, kInputRetryTimer);
				::InvalidateRect(window, nullptr, FALSE);
			}
			break;
		}
	}

	TerminalViewport Viewport() const noexcept
	{
		return model ? CalculateTerminalViewport(*model, visibleRows, scrollOffset) : TerminalViewport{};
	}

	[[nodiscard]] TerminalViewportGeometry Geometry() const noexcept
	{
		return TerminalViewportGeometry::FromDpi(dpi);
	}

	static void DestroyBackBuffer(HDC dc, HBITMAP bitmap, HGDIOBJ originalBitmap) noexcept
	{
		if( dc != nullptr && bitmap != nullptr && originalBitmap != nullptr ) {
			static_cast<void>(::SelectObject(dc, originalBitmap));
		}
		const bool bitmapDeleted = bitmap != nullptr && ::DeleteObject(bitmap) != FALSE;
		if( dc != nullptr ) ::DeleteDC(dc);
		// DeleteObject cannot delete a selected bitmap.  A failed restore above
		// is therefore retried after the DC is gone instead of leaking the DIB.
		if( bitmap != nullptr && !bitmapDeleted ) static_cast<void>(::DeleteObject(bitmap));
	}

	[[nodiscard]] bool HasUsableBackBufferState() const noexcept
	{
		if( backBufferDc == nullptr || backBufferBitmap == nullptr || backBufferOriginalBitmap == nullptr ||
			backBufferBits == nullptr || backBufferSize.cx <= 0 || backBufferSize.cy <= 0 ) {
			return false;
		}
		const auto width = static_cast<std::size_t>(backBufferSize.cx);
		const auto height = static_cast<std::size_t>(backBufferSize.cy);
		const auto maximumSize = std::numeric_limits<std::size_t>::max();
		if( width > maximumSize / kTerminalBackBufferBytesPerPixel ||
			width > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ) {
			return false;
		}
		const auto strideBytes = width * kTerminalBackBufferBytesPerPixel;
		if( height > maximumSize / strideBytes ) return false;
		return backBufferStridePixels == static_cast<std::ptrdiff_t>(width) &&
			backBufferByteCount == strideBytes * height;
	}

	[[nodiscard]] bool HasAnyBackBufferState() const noexcept
	{
		return backBufferDc != nullptr || backBufferBitmap != nullptr || backBufferOriginalBitmap != nullptr ||
			backBufferBits != nullptr || backBufferStridePixels != 0 || backBufferByteCount != 0 ||
			backBufferSize.cx != 0 || backBufferSize.cy != 0;
	}

	bool EnsureBackBuffer( HDC referenceDc )
	{
		RECT client{};
		if( referenceDc == nullptr || window == nullptr || !::GetClientRect(window, &client) ) return false;
		const auto widthValue = static_cast<long long>(client.right) - static_cast<long long>(client.left);
		const auto heightValue = static_cast<long long>(client.bottom) - static_cast<long long>(client.top);
		if( widthValue <= 0 || heightValue <= 0 ||
			widthValue > std::numeric_limits<LONG>::max() || heightValue > std::numeric_limits<LONG>::max() ) {
			return false;
		}
		const auto width = static_cast<LONG>(widthValue);
		const auto height = static_cast<LONG>(heightValue);
		const auto widthPixels = static_cast<std::size_t>(width);
		const auto heightPixels = static_cast<std::size_t>(height);
		const auto maximumSize = std::numeric_limits<std::size_t>::max();
		if( widthPixels > maximumSize / kTerminalBackBufferBytesPerPixel ||
			widthPixels > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ) {
			return false;
		}
		const auto strideBytes = widthPixels * kTerminalBackBufferBytesPerPixel;
		if( heightPixels > maximumSize / strideBytes ) return false;
		const auto byteCount = strideBytes * heightPixels;
		const auto stridePixels = static_cast<std::ptrdiff_t>(widthPixels);

		if( HasUsableBackBufferState() && backBufferSize.cx == width && backBufferSize.cy == height &&
			backBufferStridePixels == stridePixels && backBufferByteCount == byteCount ) {
			return true;
		}
		if( HasAnyBackBufferState() && !HasUsableBackBufferState() ) ReleaseBackBuffer();

		// Create the new DC and DIB independently.  A resize/create failure never
		// changes the still-valid old backbuffer or publishes partial DIB state.
		const HDC replacementDc = ::CreateCompatibleDC(referenceDc);
		if( replacementDc == nullptr ) return false;
		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
		bitmapInfo.bmiHeader.biWidth = width;
		bitmapInfo.bmiHeader.biHeight = -height; // top-down: bits starts at logical row zero.
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		void* replacementBits{};
		const HBITMAP replacementBitmap = ::CreateDIBSection(referenceDc, &bitmapInfo, DIB_RGB_COLORS,
			&replacementBits, nullptr, 0);
		if( replacementBitmap == nullptr || replacementBits == nullptr ) {
			DestroyBackBuffer(replacementDc, replacementBitmap, nullptr);
			return false;
		}
		const auto replacementOriginalBitmap = ::SelectObject(replacementDc, replacementBitmap);
		if( replacementOriginalBitmap == nullptr || replacementOriginalBitmap == HGDI_ERROR ) {
			DestroyBackBuffer(replacementDc, replacementBitmap, nullptr);
			return false;
		}

		const HDC oldDc = backBufferDc;
		const HBITMAP oldBitmap = backBufferBitmap;
		const HGDIOBJ oldOriginalBitmap = backBufferOriginalBitmap;
		backBufferDc = replacementDc;
		backBufferBitmap = replacementBitmap;
		backBufferOriginalBitmap = replacementOriginalBitmap;
		backBufferBits = static_cast<std::uint32_t*>(replacementBits);
	backBufferStridePixels = stridePixels;
	backBufferByteCount = byteCount;
	backBufferSize = { width, height };
	MarkNativeFullDirty();
	DestroyBackBuffer(oldDc, oldBitmap, oldOriginalBitmap);
		return true;
	}

	void ReleaseBackBuffer() noexcept
	{
		const HDC dc = std::exchange(backBufferDc, nullptr);
		const HBITMAP bitmap = std::exchange(backBufferBitmap, nullptr);
		const HGDIOBJ originalBitmap = std::exchange(backBufferOriginalBitmap, nullptr);
		backBufferBits = nullptr;
		backBufferStridePixels = 0;
		backBufferByteCount = 0;
		backBufferSize = {};
		DestroyBackBuffer(dc, bitmap, originalBitmap);
	}

	void RecreateFont()
	{
		ReleaseRenderFonts();
		const auto fontSpec = theme::CThemeService::FontSpec(theme::ThemeFontKind::Terminal);
		const auto metrics = CalculateTerminalFontMetrics(fontSpec.pointSize, dpi);
		const wchar_t* face = theme::CThemeService::ResolveFontFamily(theme::ThemeFontKind::Terminal);
		fallbackFontFamilies = {
			fontSpec.fallbackFamily,
			L"Segoe UI Symbol",
			L"Segoe UI Emoji",
			L"Yu Gothic UI",
			L"Meiryo UI",
		};
		const auto createFont = [&](const wchar_t* family, int weight, DWORD pitch) {
			if( family == nullptr || *family == L'\0' ) return static_cast<HFONT>(nullptr);
			// lfWidth == 0 preserves the typeface's natural outlines.  The terminal
			// grid still supplies explicit cell advances to ExtTextOutW, so this
			// removes horizontal glyph distortion without changing PTY dimensions.
			return ::CreateFontW(-metrics.fontPixelHeight, 0, 0, 0, weight,
				FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_NATURAL_QUALITY, pitch, family);
		};
		font = createFont(face, fontSpec.weight, FIXED_PITCH | FF_MODERN);
		boldFont = createFont(face, FW_BOLD, FIXED_PITCH | FF_MODERN);
		cellWidth = metrics.cellWidth;
		cellHeight = metrics.cellHeight;
		// DirectWrite consumes model-owned cells for shaped fallback only.  It
		// never measures a font back into the terminal's PTY grid geometry.
		terminalFontFamily.assign(face == nullptr ? L"" : face);
		terminalFontPixelHeight = metrics.fontPixelHeight;
		terminalFontWeight = fontSpec.weight;
		RefreshDWriteConfiguration(true);
		RecreateCaret();
	}

	void EnsureFallbackFonts() noexcept
	{
		if( fallbackFontsCreated ) return;
		// This path is reached only after DirectWrite declined/failed for a shaped
		// cluster and the primary GDI face has no matching glyph.  Record the
		// one permitted construction attempt before issuing Win32 calls so a
		// missing font cannot turn repaint into a creation retry loop.
		fallbackFontsCreated = true;
		const int fontHeight = std::max(1, terminalFontPixelHeight);
		const auto createFont = [fontHeight](const wchar_t* family, int weight) noexcept {
			if( family == nullptr || *family == L'\0' ) return static_cast<HFONT>(nullptr);
			return ::CreateFontW(-fontHeight, 0, 0, 0, weight,
				FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
		};
		const int boldWeight = std::max(terminalFontWeight, static_cast<int>(FW_BOLD));
		for( std::size_t index = 0; index < kTerminalFallbackFontCount; ++index ) {
			fallbackFonts[index] = createFont(fallbackFontFamilies[index], terminalFontWeight);
			fallbackBoldFonts[index] = createFont(fallbackFontFamilies[index], boldWeight);
		}
	}

	void RefreshDWriteConfiguration( bool force )
	{
		std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> locale{};
		std::wstring currentLocale;
		const int length = ::GetUserDefaultLocaleName(locale.data(), static_cast<int>(locale.size()));
		if( length > 1 ) currentLocale.assign(locale.data(), static_cast<std::size_t>(length - 1));
		if( currentLocale != terminalLocale ) {
			terminalLocale = std::move(currentLocale);
			force = true;
		}
		if( !force ) return;
		++rendererGeneration;
		dwriteRenderer.Configure({ terminalFontFamily, terminalLocale, terminalFontPixelHeight,
			terminalFontWeight, dpi, rendererGeneration });
	}

	[[nodiscard]] TerminalRenderClassification Classify(std::wstring_view text, bool bold) noexcept override
	{
		if( text.size() != 1 || classifierDc == nullptr ) return TerminalRenderClassification::ShapedFallback;
		const HFONT primary = bold && boldFont ? boldFont : font;
		if( primary == nullptr ) return TerminalRenderClassification::ShapedFallback;
		const auto key = static_cast<std::uint32_t>(text.front()) | (bold ? 0x10000U : 0U);
		if( const auto cached = primaryGlyphCoverage.find(key); cached != primaryGlyphCoverage.end() ) {
			return cached->second ? TerminalRenderClassification::GdiSimple : TerminalRenderClassification::ShapedFallback;
		}
		const bool covered = FontContainsText(classifierDc, primary, text);
		try {
			if( primaryGlyphCoverage.size() >= kTerminalPrimaryCoverageCacheLimit ) primaryGlyphCoverage.clear();
			primaryGlyphCoverage.emplace(key, covered);
		} catch( const std::bad_alloc& ) {
			// A cache allocation must not make a terminal repaint fail.  A
			// conservative shaped route is safe when the bounded cache cannot grow.
			return TerminalRenderClassification::ShapedFallback;
		}
		return covered ? TerminalRenderClassification::GdiSimple : TerminalRenderClassification::ShapedFallback;
	}

	[[nodiscard]] std::uint64_t Generation() const noexcept override
	{
		return rendererGeneration;
	}

	[[nodiscard]] static TerminalRenderStyle ResolvePlanStyle(void* context,
		const TerminalAttributes& attributes, bool selected) noexcept
	{
		const auto& self = *static_cast<const Impl*>(context);
		auto background = ResolveTerminalColor(attributes.background, self.palette, self.paintDefaultBackground,
			TerminalColorRole::Background);
		auto foreground = attributes.inverse
			? ResolveTerminalColor(attributes.foreground, self.palette, self.paintDefaultForeground,
				TerminalColorRole::Background)
			: ResolveTerminalForeground(attributes.foreground, self.palette, self.paintDefaultForeground, background);
		if( attributes.inverse ) std::swap(foreground, background);
		if( selected ) background = self.palette.accent.ToColorRef();
		const bool usesSurfaceDefaultBackground = !selected && !attributes.inverse &&
			attributes.background.kind == TerminalColorKind::Default;
		return { foreground, background, attributes.bold, attributes.underline, attributes.inverse, selected,
			usesSurfaceDefaultBackground };
	}

	HFONT FallbackFontForText( HDC dc, std::wstring_view text, bool bold )
	{
		const HFONT primary = bold && boldFont ? boldFont : font;
		if( primary == nullptr || text.empty() || (text.size() == 1 && text.front() < 0x80) ) return primary;

		// Most terminal output is a single BMP codepoint.  Cache that hot path by
		// code unit and weight; multi-codepoint graphemes are probed directly so a
		// cached key can never retain a pointer into a mutable TerminalCell.
		if( text.size() == 1 ) {
			const auto key = static_cast<std::uint32_t>(text.front()) | (bold ? 0x10000U : 0U);
			if( const auto cached = glyphFontCache.find(key); cached != glyphFontCache.end() ) return cached->second;
			HFONT selected = primary;
			if( !FontContainsText(dc, primary, text) ) {
				EnsureFallbackFonts();
				const auto& fallbacks = bold ? fallbackBoldFonts : fallbackFonts;
				for( const auto candidate : fallbacks ) {
					if( candidate && FontContainsText(dc, candidate, text) ) {
						selected = candidate;
						break;
					}
				}
			}
			try {
				if( glyphFontCache.size() >= kTerminalFallbackCoverageCacheLimit ) glyphFontCache.clear();
				glyphFontCache.emplace(key, selected);
			} catch( const std::bad_alloc& ) {
				// The cache is only an allocation-avoidance optimization.  The fallback
				// decision remains valid even if this repaint cannot retain it.
			}
			return selected;
		}

		if( FontContainsText(dc, primary, text) ) return primary;
		EnsureFallbackFonts();
		const auto& fallbacks = bold ? fallbackBoldFonts : fallbackFonts;
		for( const auto candidate : fallbacks ) {
			if( candidate && FontContainsText(dc, candidate, text) ) return candidate;
		}
		return primary;
	}

	void RecreateCaret()
	{
		if( window == nullptr || ::GetFocus() != window ) return;
		caretShown = false;
		::DestroyCaret();
		if( ::CreateCaret(window, nullptr, std::max(1, cellWidth / 8), cellHeight) ) {
			UpdateCaret();
		}
	}

	void UpdateImeWindowPosition()
	{
		if( window == nullptr || !imeComposing ) return;
		const HIMC context = ::ImmGetContext(window);
		if( context == nullptr ) return;
		const auto row = std::min(model ? model->CursorRow() : 0u, visibleRows == 0 ? 0u : visibleRows - 1);
		const auto position = Geometry().ImeWindowPosition(
			model ? model->CursorColumn() : 0u, row, cellWidth, cellHeight);

		COMPOSITIONFORM composition{};
		composition.dwStyle = CFS_POINT;
		composition.ptCurrentPos = position.composition;
		::ImmSetCompositionWindow(context, &composition);

		CANDIDATEFORM candidate{};
		candidate.dwIndex = 0;
		candidate.dwStyle = CFS_EXCLUDE;
		candidate.ptCurrentPos = position.caret;
		candidate.rcArea = position.candidateArea;
		::ImmSetCandidateWindow(context, &candidate);
		::ImmReleaseContext(window, context);
	}

	void UpdateCaret()
	{
		if( window == nullptr || ::GetFocus() != window ) return;
		if( !model || !model->Modes().cursorVisible || (!model->IsAlternateScreen() && scrollOffset != 0) ) {
			if( caretShown ) {
				::HideCaret(window);
				caretShown = false;
			}
			return;
		}
		const auto row = std::min(model->CursorRow(), visibleRows == 0 ? 0 : visibleRows - 1);
		const auto geometry = Geometry();
		::SetCaretPos(geometry.GridOriginX() + static_cast<int>(model->CursorColumn()) * cellWidth,
			geometry.GridOriginY() + static_cast<int>(row) * cellHeight);
		UpdateImeWindowPosition();
		if( !caretShown ) {
			::ShowCaret(window);
			caretShown = true;
		}
	}

	void NotifySize()
	{
		if( window == nullptr ) return;
		RECT client{};
		::GetClientRect(window, &client);
		// A client with no extent carries no measurement at all: a minimized frame
		// lays every child out at 0x0, and a hidden bottom Panel reaches this the
		// same way. Publishing the 1x1 grid such an extent produces would truncate
		// every retained row through TerminalModel::Resize, destroying the session's
		// text before the window is ever restored, so the last real measurement
		// stays authoritative instead.
		const auto measured = Geometry().MeasureGrid(
			static_cast<int>(client.right - client.left),
			static_cast<int>(client.bottom - client.top),
			cellWidth, cellHeight);
		if( !measured ) return;
		visibleRows = measured->rows;
		const TerminalSize next{ measured->columns, measured->rows };
		const bool firstMeasurement = !terminalSizeMeasured;
		terminalSizeMeasured = true;
		if( firstMeasurement || next.columns != terminalSize.columns || next.rows != terminalSize.rows ) {
			terminalSize = next;
			if( resizeSink ) resizeSink(next);
		}
		UpdateScrollbar();
		UpdateCaret();
	}

	void UpdateScrollbar()
	{
		if( window == nullptr ) return;
		if( ScrollbarLayout().scrollable ) return;
		const bool wasInteractive = scrollbarHover || scrollbarButtonPressed || scrollbarDragging;
		const bool ownedCapture = scrollbarButtonPressed || scrollbarDragging;
		scrollbarHover = false;
		scrollbarButtonPressed = false;
		scrollbarDragging = false;
		scrollbarThumbGrabOffset = 0;
		if( ownedCapture && ::GetCapture() == window ) ::ReleaseCapture();
		if( wasInteractive ) ::InvalidateRect(window, nullptr, FALSE);
	}

	[[nodiscard]] TerminalScrollbarLayout ScrollbarLayout() const noexcept
	{
		RECT client{};
		if( window == nullptr || !::GetClientRect(window, &client) ) return {};
		const auto viewport = Viewport();
		return CalculateTerminalScrollbarLayout(client, viewport.totalRows, viewport.visibleRows, viewport.topRow, dpi);
	}

	void PaintScrollbar( HDC dc ) const
	{
		const auto layout = ScrollbarLayout();
		if( !layout.scrollable ) return;
		const auto dcBrush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		if( scrollbarHover || scrollbarButtonPressed ) {
			::SetDCBrushColor(dc, palette.raised.ToColorRef());
			::FillRect(dc, &layout.track, dcBrush);
		}
		::SetDCBrushColor(dc, (scrollbarHover || scrollbarDragging ? palette.secondaryText : palette.border).ToColorRef());
		::FillRect(dc, &layout.thumb, dcBrush);
	}

	[[nodiscard]] bool UpdateScrollbarHover( POINT point )
	{
		const bool hover = ScrollbarLayout().HitTest(point);
		if( scrollbarHover != hover ) {
			scrollbarHover = hover;
			::InvalidateRect(window, nullptr, FALSE);
		}
		if( hover && !trackingScrollbarMouseLeave ) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
			trackingScrollbarMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		return hover;
	}

	void EndScrollbarButtonPress( bool releaseCapture )
	{
		const bool wasInteractive = scrollbarButtonPressed || scrollbarDragging;
		scrollbarButtonPressed = false;
		scrollbarDragging = false;
		scrollbarThumbGrabOffset = 0;
		if( releaseCapture && ::GetCapture() == window ) ::ReleaseCapture();
		if( wasInteractive ) ::InvalidateRect(window, nullptr, FALSE);
	}

	[[nodiscard]] bool BeginScrollbarButtonPress( POINT point, unsigned int button )
	{
		const auto layout = ScrollbarLayout();
		if( !layout.HitTest(point) ) return false;
		::SetFocus(window);
		scrollbarButtonPressed = true;
		scrollbarSuppressedButtons |= 1u << button;
		scrollbarDragging = button == 0 && layout.ThumbHitTest(point);
		scrollbarThumbGrabOffset = scrollbarDragging ? point.y - layout.thumb.top : 0;
		if( button == 0 && !scrollbarDragging ) {
			const auto viewport = Viewport();
			if( point.y < layout.thumb.top ) SetScrollTop(viewport.topRow > viewport.visibleRows ? viewport.topRow - viewport.visibleRows : 0);
			else if( point.y >= layout.thumb.bottom ) SetScrollTop(viewport.topRow + viewport.visibleRows);
		}
		::SetCapture(window);
		static_cast<void>(UpdateScrollbarHover(point));
		::InvalidateRect(window, nullptr, FALSE);
		return true;
	}

	void DragScrollbarTo( int pointerY )
	{
		if( !scrollbarDragging ) return;
		const auto layout = ScrollbarLayout();
		if( !layout.scrollable ) {
			EndScrollbarButtonPress(true);
			return;
		}
		SetScrollTop(TerminalScrollbarTopRowFromDrag(layout, pointerY, scrollbarThumbGrabOffset));
	}

	void SetScrollTop( std::size_t top )
	{
		// Keep the main screen's scroll position intact while a TUI owns the
		// alternate screen.  Wheel events may still arrive after the overlay
		// scrollbar has disappeared; they must not reset the position restored
		// when DECSET 1049 is later cleared.
		if( model && model->IsAlternateScreen() ) return;
		const auto viewport = Viewport();
		const auto bottomTop = viewport.totalRows > viewport.visibleRows ? viewport.totalRows - viewport.visibleRows : 0;
		top = std::min(top, bottomTop);
		scrollOffset = bottomTop - top;
		UpdateScrollbar();
		UpdateCaret();
		::InvalidateRect(window, nullptr, FALSE);
	}

	void ScrollLines( int lines )
	{
		const auto viewport = Viewport();
		const auto current = static_cast<long long>(viewport.topRow);
		const auto maximumTop = viewport.totalRows > viewport.visibleRows ? viewport.totalRows - viewport.visibleRows : 0;
		const auto target = std::clamp<long long>(current + lines, 0, static_cast<long long>(maximumTop));
		SetScrollTop(static_cast<std::size_t>(target));
	}

	void InvalidateScrollbarStrip()
	{
		if( window == nullptr ) return;
		RECT client{};
		if( !::GetClientRect(window, &client) ) return;
		const int width = std::max(1, ::MulDiv(6, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi));
		RECT strip{ std::max(client.left, client.right - width), client.top, client.right, client.bottom };
		AccumulateNativeDirtyRect(strip);
		::InvalidateRect(window, &strip, FALSE);
	}

	void ApplyScrollbackChange( const TerminalScrollbackChange& change, bool invalidate )
	{
		if( model == nullptr || !change.Changed() ) return;
		const auto previousOffset = scrollOffset;
		const auto totalRows = model->ScrollbackSize() + model->RowCount();
		const auto maximumOffset = totalRows > visibleRows ? totalRows - visibleRows : 0;
		const auto anchor = UpdateTerminalViewportAnchor(scrollOffset, change.Appended(), maximumOffset);
		scrollOffset = anchor.scrollOffset;

		if( change.Cleared() && change.Evicted() == 0 ) {
			ClearSelection();
		} else if( change.Evicted() != 0 ) {
			const auto discarded = change.Evicted();
			if( HasSelection() && std::max(selectionAnchor.row, selectionActive.row) < discarded ) {
				ClearSelection();
			} else {
				const auto adjust = [discarded]( TerminalSelectionPoint& point ) noexcept {
					if( point.row < discarded ) {
						point.row = 0;
						point.column = 0;
					} else {
						point.row -= discarded;
					}
				};
				adjust(selectionOrigin);
				adjust(selectionAnchor);
				adjust(selectionActive);
			}
		}

		UpdateScrollbar();
		UpdateCaret();
		if( window == nullptr || !invalidate ) return;
		if( previousOffset != 0 || anchor.retainedContentDiscarded ) {
			MarkNativeFullDirty();
			::InvalidateRect(window, nullptr, FALSE);
		} else {
			InvalidateScrollbarStrip();
		}
	}

	TerminalSelectionPoint PointToCell( int x, int y ) const noexcept
	{
		return TerminalCellFromPoint(Viewport(), x, y, cellWidth, cellHeight, model ? model->Columns() : 0, Geometry());
	}

	void UpdateSelection( TerminalSelectionPoint point ) noexcept
	{
		selectionMoved = point != selectionOrigin;
		if( !selectionMoved ) {
			selectionAnchor = selectionActive = selectionOrigin;
			return;
		}
		const auto range = NormalizeTerminalSelection(*model, selectionOrigin, point);
		selectionAnchor = range.anchor;
		selectionActive = range.active;
	}

	bool HasSelection() const noexcept
	{
		return model != nullptr && selectionAnchor != selectionActive;
	}

	void ClearSelection()
	{
		selectionAnchor = selectionActive;
		selectionOrigin = selectionActive;
		selectionMoved = false;
		::InvalidateRect(window, nullptr, FALSE);
	}

	void SendMouse( TerminalMouseAction action, int x, int y, unsigned int button, WPARAM keys )
	{
		if( !model ) return;
		const auto cell = PointToCell(x, y);
		const auto viewport = Viewport();
		TerminalMouseEvent event;
		event.action = action;
		event.button = button;
		event.column = cell.column;
		event.row = cell.row >= viewport.topRow ? cell.row - viewport.topRow : 0;
		event.shift = (keys & MK_SHIFT) != 0;
		event.control = (keys & MK_CONTROL) != 0;
		event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
		if( inputAdapter ) {
			if( const auto encoded = inputAdapter->EncodeMouse(event) ) Send(*encoded);
		} else {
			Send(EncodeTerminalMouse(event, model->Modes()));
		}
	}

	bool MouseReporting() const noexcept
	{
		if( !model ) return false;
		const auto& modes = model->Modes();
		return modes.mouseButtonTracking || modes.mouseDragTracking || modes.mouseAnyEventTracking;
	}

	void DrawUnderline( HDC memory, const RECT& rect, COLORREF color ) noexcept
	{
		const RECT underline{ rect.left, std::max(rect.top, rect.bottom - 2), rect.right, rect.bottom };
		if( underline.right <= underline.left || underline.bottom <= underline.top ) return;
		const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		::SetDCBrushColor(memory, color);
		::FillRect(memory, &underline, brush);
	}

	void PaintPlanBackgrounds( HDC memory ) noexcept
	{
		const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		for( const auto& span : renderPlan.BackgroundSpans() ) {
			::SetDCBrushColor(memory, span.style.background);
			::FillRect(memory, &span.rect, brush);
		}
	}

	void PaintBuiltinGlyphs( HDC memory, const RECT& paintRect ) noexcept
	{
		const auto commands = renderPlan.BuiltinGlyphs();
		if( commands.empty() ) return;
		if( memory == backBufferDc && HasUsableBackBufferState() ) {
			const TerminalBuiltinGlyphPixelSurface surface{
				backBufferBits,
				backBufferStridePixels,
				backBufferSize.cx,
				backBufferSize.cy,
				paintRect,
			};
			// CreateDIBSection requires a GDI flush before application code touches
			// the DIB bits.  Subsequent GDI (and the existing DWrite handoff) sees
			// the same selected DIB; the window-DC and rejected-surface paths retain
			// the exact HDC batch/scalar fallback.
			if( ::GdiFlush() != FALSE && TerminalBuiltinGlyphRenderer::DrawBatch(surface, commands) ) return;
		}
		builtinGlyphRenderer.DrawBatch(memory, commands);
	}

	void PaintGdiRuns( HDC memory, HFONT& selectedFont ) noexcept
	{
		::SetBkMode(memory, TRANSPARENT);
		for( const auto& run : renderPlan.GdiRuns() ) {
			const HFONT desiredFont = run.style.bold && boldFont ? boldFont : font;
			if( desiredFont == nullptr ) continue;
			if( desiredFont != selectedFont ) {
				::SelectObject(memory, desiredFont);
				selectedFont = desiredFont;
			}
			const auto text = renderPlan.Text(run.textOffset, run.textLength);
			const auto advances = renderPlan.Advances(run.advanceOffset, run.advanceCount);
			if( text.empty() || advances.size() != text.size() ) continue;
			::SetTextColor(memory, run.style.foreground);
			::ExtTextOutW(memory, run.rect.left, run.rect.top, ETO_CLIPPED, &run.rect,
				text.data(), static_cast<UINT>(text.size()), advances.data());
			if( run.style.underline ) DrawUnderline(memory, run.rect, run.style.foreground);
		}
	}

	void PaintShapedGdiFallbacks( HDC memory, HFONT& selectedFont )
	{
		// A failed EndDraw can leave successfully submitted clusters visible in the
		// DC.  Restore only the shaped commands' exact model rectangles before any
		// GDI fallback text is emitted, so no partial DirectWrite pixels survive.
		const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		for( const auto& cluster : renderPlan.ShapedClusters() ) {
			::SetDCBrushColor(memory, cluster.style.background);
			::FillRect(memory, &cluster.rect, brush);
		}
		::SetBkMode(memory, TRANSPARENT);
		for( const auto& cluster : renderPlan.ShapedClusters() ) {
			const auto text = renderPlan.Text(cluster.textOffset, cluster.textLength);
			if( text.empty() ) continue;
			const HFONT desiredFont = FallbackFontForText(memory, text, cluster.style.bold);
			if( desiredFont == nullptr ) continue;
			if( desiredFont != selectedFont ) {
				::SelectObject(memory, desiredFont);
				selectedFont = desiredFont;
			}
			::SetTextColor(memory, cluster.style.foreground);
			// DirectWrite retry is deliberately not attempted here.  The model-owned
			// rectangle still clips this degraded GDI fallback to the same VT cells.
			::ExtTextOutW(memory, cluster.rect.left, cluster.rect.top, ETO_CLIPPED, &cluster.rect,
				text.data(), static_cast<UINT>(text.size()), nullptr);
			if( cluster.style.underline ) DrawUnderline(memory, cluster.rect, cluster.style.foreground);
		}
	}

	[[nodiscard]] bool PaintShapedDirectWrite( HDC memory, const RECT& targetRect ) noexcept
	{
		TerminalDWriteFrame frame;
		::GdiFlush();
		if( dwriteRenderer.BeginFrame(memory, targetRect, frame) != TerminalDWriteFrameOutcome::Rendered ) return false;
		for( const auto& cluster : renderPlan.ShapedClusters() ) {
			const auto text = renderPlan.Text(cluster.textOffset, cluster.textLength);
			if( text.empty() || !dwriteRenderer.DrawCluster(cluster, text) ) {
				dwriteRenderer.AbortFrame(frame);
				return false;
			}
		}
		return dwriteRenderer.FinalizeFrame(frame) == TerminalDWriteFrameOutcome::Rendered;
	}

	void Paint()
	{
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if( dc == nullptr ) return;
		AccumulateNativeDirtyRect(paint.rcPaint);
		const int width = paint.rcPaint.right - paint.rcPaint.left;
		const int height = paint.rcPaint.bottom - paint.rcPaint.top;
		if( width <= 0 || height <= 0 ) {
			::EndPaint(window, &paint);
			return;
		}
		const bool buffered = EnsureBackBuffer(dc);
		const HDC memory = buffered ? backBufferDc : dc;
		RECT dwriteTargetRect{};
		if( buffered ) dwriteTargetRect = { 0, 0, backBufferSize.cx, backBufferSize.cy };
		else ::GetClientRect(window, &dwriteTargetRect);
		const auto defaultColors = ResolveTerminalRenderDefaults(palette, useTerminalProfileColors);
		const COLORREF defaultBackground = defaultColors.background;
		const COLORREF defaultForeground = defaultColors.foreground;
		const auto dcBrush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		const auto previousBrush = ::SelectObject(memory, dcBrush);
		::SetDCBrushColor(memory, defaultBackground);
		::FillRect(memory, &paint.rcPaint, dcBrush);
		const auto dcPen = static_cast<HPEN>(::GetStockObject(DC_PEN));
		const auto previousPen = ::SelectObject(memory, dcPen);
		const auto previousFont = font ? ::SelectObject(memory, font) : nullptr;
		HFONT selectedFont = font;
		paintDefaultBackground = defaultBackground;
		paintDefaultForeground = defaultForeground;
		classifierDc = memory;
		const TerminalRenderPlanBuildInput planInput{
			model,
			Viewport(),
			paint.rcPaint,
			cellWidth,
			cellHeight,
			HasSelection(),
			selectionAnchor,
			selectionActive,
			this,
			&Impl::ResolvePlanStyle,
			this,
			true,
			Geometry(),
		};
		const bool hasRenderPlan = renderPlan.Build(planInput);
		classifierDc = nullptr;
		if( hasRenderPlan ) {
			const int savedDc = ::SaveDC(memory);
			if( savedDc != 0 ) ::IntersectClipRect(memory, paint.rcPaint.left, paint.rcPaint.top,
				paint.rcPaint.right, paint.rcPaint.bottom);
			// The normal plan is consumed in its fixed pass order: resolved
			// backgrounds, deterministic terminal glyphs, simple primary GDI, then
			// model-owned shaped DirectWrite.  Only failure recovery restores shaped
			// backgrounds before repainting those clusters through GDI.
			PaintPlanBackgrounds(memory);
			PaintBuiltinGlyphs(memory, paint.rcPaint);
			PaintGdiRuns(memory, selectedFont);
			if( !renderPlan.ShapedClusters().empty() ) {
				// Locale refresh and DirectWrite factory creation are both gated behind
				// an eligible shaped cluster, so ASCII-only terminal frames remain GDI.
				RefreshDWriteConfiguration(false);
				// ID2D1DCRenderTarget uses the bound rectangle as its target geometry.
				// Model-owned glyph rectangles are absolute client coordinates, so binding
				// only a dirty row can clip every shaped glyph below that row's local
				// target height. Keep the target at the stable client/DIB bounds while the
				// render plan and DC clip still restrict actual work to paint.rcPaint.
				if( !PaintShapedDirectWrite(memory, dwriteTargetRect) ) PaintShapedGdiFallbacks(memory, selectedFont);
			}
			if( savedDc != 0 ) ::RestoreDC(memory, savedDc);
		}
		if( previousFont ) ::SelectObject(memory, previousFont);
		if( inputBackpressured || inputRejected ) {
			const auto geometry = Geometry();
			const auto grid = geometry.GridRect(dwriteTargetRect);
			RECT warning{ grid.left, std::max(grid.top, grid.bottom - cellHeight), grid.right, grid.bottom };
			::SetBkColor(memory, inputRejected ? RGB(128, 40, 40) : palette.raised.ToColorRef());
			::SetTextColor(memory, palette.primaryText.ToColorRef());
			const wchar_t* message = inputRejected ? LS(STR_TERMINAL_INPUT_REJECTED)
				: LS(STR_TERMINAL_INPUT_BACKPRESSURE);
			::ExtTextOutW(memory, warning.left + 4, warning.top, ETO_OPAQUE | ETO_CLIPPED, &warning, message,
				static_cast<UINT>(wcslen(message)), nullptr);
		}
		PaintScrollbar(memory);
		if( previousPen ) ::SelectObject(memory, previousPen);
		if( previousBrush ) ::SelectObject(memory, previousBrush);
		if( buffered ) ::BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, width, height, memory,
			paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
		::EndPaint(window, &paint);
	}

	void HandleChar( wchar_t value, bool alt )
	{
		std::wstring text;
		if( value >= 0xd800 && value <= 0xdbff ) {
			if( pendingHighSurrogate ) text.push_back(pendingHighSurrogate);
			pendingHighSurrogate = value;
			if( !text.empty() ) Send(EncodeTerminalText(text));
			return;
		}
		if( pendingHighSurrogate ) {
			text.push_back(pendingHighSurrogate);
			pendingHighSurrogate = 0;
		}
		text.push_back(value);
		auto bytes = EncodeTerminalText(text);
		if( alt ) bytes.insert(bytes.begin(), '\x1b');
		Send(bytes);
	}

	bool HandleImeResult()
	{
		std::wstring result;
		try {
			if( !imeResultReader || !imeResultReader(window, result) ) return false;
		} catch( ... ) {
			return false;
		}
		if( pendingHighSurrogate ) {
			result.insert(result.begin(), pendingHighSurrogate);
			pendingHighSurrogate = 0;
		}
		if( !result.empty() ) Send(EncodeTerminalText(result));
		return true;
	}

	void ResetSessionInputState() noexcept
	{
		if( window ) {
			::KillTimer(window, kInputRetryTimer);
			if( imeComposing ) {
				if( const HIMC context = ::ImmGetContext(window) ) {
					::ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
					::ImmReleaseContext(window, context);
				}
			}
		}
		pendingHighSurrogate = 0;
		pendingInteractiveInput.clear();
		inputBackpressured = false;
		inputRejected = false;
		imeComposing = false;
	}

	LRESULT HandleMessage( UINT message, WPARAM wParam, LPARAM lParam )
	{
		switch( message ) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
			Paint();
			return 0;
		case WM_SIZE:
			MarkNativeFullDirty();
			NotifySize();
			if( frameSurface.IsOpen() ) {
				static_cast<void>(frameSurface.NotifyLayout());
				frameLayoutMessageSeen = true;
				PublishNativeSurfaceRegistration(true);
			}
			// A terminal is first created at 0x0 while its panel is materialized.
			// Resizing it into view must schedule a paint even when it has not
			// received focus yet; otherwise the first prompt remains hidden until
			// a click happens to invalidate the window.
			::InvalidateRect(window, nullptr, FALSE);
			return 0;
		case WM_SHOWWINDOW:
			if( frameSurface.IsOpen() ) {
				static_cast<void>(frameSurface.SetVisible(wParam != FALSE));
				PublishNativeSurfaceRegistration(true);
			}
			MarkNativeFullDirty();
			if( wParam != FALSE ) ::InvalidateRect(window, nullptr, FALSE);
			return ::DefWindowProcW(window, message, wParam, lParam);
		case WM_TIMER:
			if( wParam == kInputRetryTimer ) {
				RetryPendingInteractiveInput();
				return 0;
			}
			return ::DefWindowProcW(window, message, wParam, lParam);
		case WM_SETFOCUS:
			RecreateCaret();
			if( focusSink ) focusSink();
			if( inputAdapter ) {
				if( const auto encoded = inputAdapter->EncodeFocus(true) ) Send(*encoded);
			}
			return 0;
		case WM_KILLFOCUS:
			if( inputAdapter ) {
				if( const auto encoded = inputAdapter->EncodeFocus(false) ) Send(*encoded);
			}
			if( caretShown ) ::HideCaret(window);
			caretShown = false;
			::DestroyCaret();
			return 0;
		case WM_CHAR:
			HandleChar(static_cast<wchar_t>(wParam), false);
			return 0;
		case WM_SYSCHAR:
			HandleChar(static_cast<wchar_t>(wParam), true);
			return 0;
		case WM_IME_STARTCOMPOSITION: {
			imeComposing = true;
			const auto result = ::DefWindowProcW(window, message, wParam, lParam);
			UpdateImeWindowPosition();
			return result;
		}
		case WM_IME_COMPOSITION:
			// Some IMEs deliver committed text only through GCS_RESULTSTR. Relying
			// on DefWindowProc to synthesize WM_CHAR makes input depend on the IME
			// and on whether the child is hosting a full-screen terminal program.
			if( (lParam & GCS_RESULTSTR) != 0 && HandleImeResult() ) return 0;
			{
				const auto result = ::DefWindowProcW(window, message, wParam, lParam);
				UpdateImeWindowPosition();
				return result;
			}
		case WM_IME_NOTIFY:
		{
			const auto result = ::DefWindowProcW(window, message, wParam, lParam);
			if( wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE ) UpdateImeWindowPosition();
			return result;
		}
		case WM_IME_ENDCOMPOSITION:
		{
			const auto result = ::DefWindowProcW(window, message, wParam, lParam);
			imeComposing = false;
			return result;
		}
		case WM_IME_CHAR:
			// Own the fallback path too instead of asking DefWindowProc to post a
			// second message whose behavior varies between Unicode IMEs.
			HandleChar(static_cast<wchar_t>(wParam), false);
			return 0;
		case WM_LBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_RBUTTONDOWN: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			const auto button = message == WM_LBUTTONDOWN ? 0u : message == WM_MBUTTONDOWN ? 1u : 2u;
			if( BeginScrollbarButtonPress(point, button) ) return 0;
			::SetFocus(window);
			pressedMouseButton = button;
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) {
				SendMouse(TerminalMouseAction::Press, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), button, wParam);
			} else if( message == WM_LBUTTONDOWN ) {
				selecting = true;
				selectionMoved = false;
				selectionOrigin = PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				selectionAnchor = selectionActive = selectionOrigin;
				::SetCapture(window);
				::InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		case WM_MOUSEMOVE: {
			const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if( scrollbarButtonPressed ) {
				DragScrollbarTo(point.y);
				return 0;
			}
			if( UpdateScrollbarHover(point) ) return 0;
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) {
				SendMouse(TerminalMouseAction::Move, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), pressedMouseButton, wParam);
			} else if( selecting ) {
				UpdateSelection(PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
				::InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		case WM_LBUTTONUP:
		case WM_MBUTTONUP:
		case WM_RBUTTONUP: {
			const auto button = message == WM_LBUTTONUP ? 0u : message == WM_MBUTTONUP ? 1u : 2u;
			const auto buttonMask = 1u << button;
			if( scrollbarButtonPressed && (scrollbarSuppressedButtons & buttonMask) != 0 ) {
				if( scrollbarDragging ) DragScrollbarTo(GET_Y_LPARAM(lParam));
				scrollbarSuppressedButtons &= ~buttonMask;
				EndScrollbarButtonPress(true);
				return 0;
			}
			if( (scrollbarSuppressedButtons & buttonMask) != 0 ) {
				scrollbarSuppressedButtons &= ~buttonMask;
				return 0;
			}
			if( button == 2 ) {
				switch( ResolveTerminalRightClick(HasSelection(), MouseReporting(), (wParam & MK_SHIFT) != 0) ) {
				case TerminalRightClickAction::CopySelection:
					if( CopySelectionToClipboard() ) ClearSelection();
					pressedMouseButton = 3;
					return 0;
				case TerminalRightClickAction::PasteClipboard:
					PasteFromClipboard();
					pressedMouseButton = 3;
					return 0;
				case TerminalRightClickAction::SendToApplication:
					break;
				}
			}
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) SendMouse(TerminalMouseAction::Release, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), button, wParam);
			if( selecting ) {
				UpdateSelection(PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
				selecting = false;
				::ReleaseCapture();
				::InvalidateRect(window, nullptr, FALSE);
			}
			pressedMouseButton = 3;
			return 0;
		}
		case WM_MOUSELEAVE:
			trackingScrollbarMouseLeave = false;
			if( scrollbarHover ) {
				scrollbarHover = false;
				::InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		case WM_CAPTURECHANGED:
			EndScrollbarButtonPress(false);
			if( selecting ) {
				selecting = false;
				selectionMoved = false;
				::InvalidateRect(window, nullptr, FALSE);
			}
			pressedMouseButton = 3;
			return 0;
		case WM_MOUSEWHEEL: {
			const auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
			POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			::ScreenToClient(window, &point);
			if( MouseReporting() ) SendMouse(delta > 0 ? TerminalMouseAction::WheelUp : TerminalMouseAction::WheelDown, point.x, point.y, 0, GET_KEYSTATE_WPARAM(wParam));
			else ScrollLines(delta > 0 ? -3 : 3);
			return 0;
		}
		case WM_SETCURSOR: {
			if( LOWORD(lParam) != HTCLIENT ) return ::DefWindowProcW(window, message, wParam, lParam);
			POINT point{};
			::GetCursorPos(&point);
			::ScreenToClient(window, &point);
			if( ScrollbarLayout().HitTest(point) ) {
				::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
				return TRUE;
			}
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		case WM_PASTE:
			PasteFromClipboard();
			return 0;
		case WM_DPICHANGED:
			dpi = HIWORD(wParam) == 0 ? kDefaultDpi : HIWORD(wParam);
			RecreateFont();
			MarkNativeFullDirty();
			NotifySize();
			if( frameSurface.IsOpen() ) {
				static_cast<void>(frameSurface.NotifyLayout());
				frameLayoutMessageSeen = true;
				PublishNativeSurfaceRegistration(true);
			}
			::InvalidateRect(window, nullptr, FALSE);
			return 0;
		case kNativeFrameReadyMessage:
			DrainNativeFrame();
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
	}

	bool CopySelectionToClipboard()
	{
		if( !HasSelection() ) return false;
		const auto text = ExtractTerminalSelection(*model, selectionAnchor, selectionActive);
		if( !::OpenClipboard(window) ) return false;
		bool copied = false;
		if( ::EmptyClipboard() ) {
			const auto bytes = (text.size() + 1) * sizeof(wchar_t);
			const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
			if( memory ) {
				if( void* destination = ::GlobalLock(memory) ) {
					std::memcpy(destination, text.c_str(), bytes);
					::GlobalUnlock(memory);
					if( ::SetClipboardData(CF_UNICODETEXT, memory) ) copied = true;
				}
				if( !copied ) ::GlobalFree(memory);
			}
		}
		::CloseClipboard();
		return copied;
	}

	bool PasteFromClipboard()
	{
		if( !model ) return false;

		// Prefer Unicode text when the clipboard carries a real string. Screenshots
		// typically leave CF_UNICODETEXT empty, so image / file-drop paths follow.
		if( ::OpenClipboard(window) ) {
			bool pasted = false;
			if( const HANDLE data = ::GetClipboardData(CF_UNICODETEXT) ) {
				if( const auto* text = static_cast<const wchar_t*>(::GlobalLock(data)) ) {
					const auto capacity = ::GlobalSize(data) / sizeof(wchar_t);
					const auto length = wcsnlen_s(text, capacity);
					std::wstring_view view(text, length);
					while( !view.empty() && (view.back() == L'\0' || view.back() == L' '
						|| view.back() == L'\t' || view.back() == L'\r' || view.back() == L'\n') ) {
						view.remove_suffix(1);
					}
					if( !view.empty() ) {
						pasted = Send(EncodeTerminalPaste(view, model->Modes().bracketedPaste));
					}
					::GlobalUnlock(data);
				}
			}
			::CloseClipboard();
			if( pasted ) return true;
		}

		const auto dropped = ReadClipboardFileDropPaths(window);
		if( !dropped.empty() ) {
			std::wstring payload;
			for( const auto& path : dropped ) {
				if( path.empty() ) continue;
				if( !payload.empty() ) payload.push_back(L' ');
				payload += FormatTerminalPastePath(path);
			}
			if( !payload.empty() ) {
				return Send(EncodeTerminalPaste(payload, model->Modes().bracketedPaste));
			}
		}

		// Claude Code / Codex / Cursor CLI read image *files*. Persist the clipboard
		// bitmap as a temp PNG and paste the absolute path rather than inventing a
		// proprietary image OSC the shell tools would ignore.
		if( const auto imagePath = SaveClipboardImageAsPng(window) ) {
			return Send(EncodeTerminalPaste(FormatTerminalPastePath(*imagePath),
				model->Modes().bracketedPaste));
		}
		return false;
	}
};

CTerminalWnd::CTerminalWnd()
	: CTerminalWnd(kTerminalSurfaceId, ReadNativeImeResult)
{
}

CTerminalWnd::CTerminalWnd( ImeResultReader imeResultReader )
	: CTerminalWnd(kTerminalSurfaceId, std::move(imeResultReader))
{
}

CTerminalWnd::CTerminalWnd( const FrameSurfaceId surfaceId )
	: CTerminalWnd(surfaceId, ReadNativeImeResult)
{
}

CTerminalWnd::CTerminalWnd( const FrameSurfaceId surfaceId, ImeResultReader imeResultReader )
	: m_impl(std::make_unique<Impl>(surfaceId))
{
	m_impl->imeResultReader = std::move(imeResultReader);
}

CTerminalWnd::~CTerminalWnd()
{
	Close();
}

bool CTerminalWnd::Create( HWND parent, HINSTANCE instance )
{
	if( m_impl->closed || m_impl->window || parent == nullptr || instance == nullptr || !EnsureTerminalClass(instance) ) return false;
	m_impl->instance = instance;
	m_impl->window = ::CreateWindowExW(0, kTerminalWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, m_impl.get());
	if( !m_impl->window ) return false;
		const auto opened = m_impl->frameSurface.Open(kTerminalDefaultFrameHostId, true);
	if( !opened.Accepted() ) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		return false;
	}
		// A newly materialized child has no prior GDI content. Request its first
		// projection, but leave presentation to the enclosing post-paint boundary.
		static_cast<void>(m_impl->frameSurface.RequestCurrent());
		(void)m_impl->nativeSurfacePublisher.Start();
		m_impl->nativeSurfacePublisher.SetNotifyWindow(m_impl->window);
		m_impl->RecreateFont();
	m_impl->NotifySize();
	return true;
}

void CTerminalWnd::Layout( const RECT& bounds, unsigned int dpi )
{
	if( m_impl->closed || !m_impl->window ) return;
	RECT previousClient{};
	(void)::GetClientRect(m_impl->window, &previousClient);
	const bool wasDrawable = previousClient.right > previousClient.left
		&& previousClient.bottom > previousClient.top;
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	if( m_impl->dpi != effectiveDpi ) {
		m_impl->dpi = effectiveDpi;
		m_impl->RecreateFont();
	}
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	m_impl->frameLayoutMessageSeen = false;
	m_impl->MarkNativeFullDirty();
	::SetWindowPos(m_impl->window, nullptr, bounds.left, bounds.top, width,
		height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
	m_impl->NotifySize();
	if( m_impl->frameSurface.IsOpen() && !m_impl->frameLayoutMessageSeen ) {
		static_cast<void>(m_impl->frameSurface.NotifyLayout());
	}
	m_impl->PublishNativeSurfaceRegistration(true);
	if (!wasDrawable && width > 0 && height > 0) {
		(void)::RedrawWindow(m_impl->window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
	} else {
		::InvalidateRect(m_impl->window, nullptr, FALSE);
	}
}

void CTerminalWnd::SetModel( TerminalModel* model )
{
	const bool changed = m_impl->model != model;
	m_impl->model = model;
	m_impl->MarkNativeFullDirty();
	m_impl->scrollOffset = 0;
	m_impl->selectionAnchor = {};
	m_impl->selectionActive = {};
	m_impl->UpdateScrollbar();
	m_impl->UpdateCaret();
	if( changed && model != nullptr && m_impl->frameSurface.IsOpen() ) {
		static_cast<void>(m_impl->frameSurface.NotifyContent());
	}
	InvalidateAll();
}

void CTerminalWnd::SetInputAdapter( SakuraTerminalInputAdapter* inputAdapter )
{
	m_impl->inputAdapter = inputAdapter;
}

void CTerminalWnd::SetInputSink( InputSink sink )
{
	m_impl->inputSink = std::move(sink);
	if( !m_impl->pendingInteractiveInput.empty() ) m_impl->RetryPendingInteractiveInput();
}

void CTerminalWnd::SetResizeSink( ResizeSink sink )
{
	m_impl->resizeSink = std::move(sink);
	// Replay only a real measurement. A pane renderer is created and bound before
	// its first Layout(), so replaying the placeholder size here would resize an
	// existing model to 1x1 during a group rebuild and truncate its retained rows.
	if( m_impl->resizeSink && m_impl->terminalSizeMeasured ) m_impl->resizeSink(m_impl->terminalSize);
}

void CTerminalWnd::SetFocusSink( FocusSink sink )
{
	m_impl->focusSink = std::move(sink);
}

void CTerminalWnd::SetPalette( const theme::ThemePalette& palette )
{
	m_impl->palette = palette;
	m_impl->useTerminalProfileColors = !theme::CThemeService::IsHighContrastActive();
	m_impl->MarkNativeFullDirty();
	// Palette transitions are an explicit renderer generation boundary.  Glyph
	// geometry is font-owned, but invalidating the bounded shaped-run cache here
	// keeps all DirectWrite state and its foreground/background style keys in
	// lockstep with the workbench theme.
	m_impl->dwriteRenderer.Invalidate();
	if( m_impl->frameSurface.IsOpen() ) {
		static_cast<void>(m_impl->frameSurface.NotifyLayout());
		m_impl->PublishNativeSurfaceRegistration(true);
	}
	if( m_impl->window ) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CTerminalWnd::ResetSessionInputState() noexcept
{
	m_impl->ResetSessionInputState();
}

void CTerminalWnd::ApplyScrollbackChange( const TerminalScrollbackChange& change, bool invalidate )
{
	m_impl->ApplyScrollbackChange(change, invalidate);
}

void CTerminalWnd::InvalidateDirtyRows( const std::vector<std::size_t>& dirtyScreenRows )
{
	if( !m_impl->window || !m_impl->model ) return;
	m_impl->UpdateScrollbar();
	const auto viewport = m_impl->Viewport();
	const auto visible = MapDirtyRowsToViewport(*m_impl->model, viewport, dirtyScreenRows);
	RECT client{};
	::GetClientRect(m_impl->window, &client);
	const auto geometry = m_impl->Geometry();
	for( std::size_t index = 0; index < visible.size(); ) {
		const auto first = visible[index];
		auto last = first;
		while( ++index < visible.size() && visible[index] == last + 1 ) last = visible[index];
		RECT rectangle{ geometry.GridOriginX(),
			static_cast<LONG>(geometry.GridOriginY() + first * m_impl->cellHeight), client.right,
			static_cast<LONG>(geometry.GridOriginY() + (last + 1) * m_impl->cellHeight) };
		m_impl->AccumulateNativeDirtyRect(rectangle);
		::InvalidateRect(m_impl->window, &rectangle, FALSE);
	}
	m_impl->UpdateCaret();
}

void CTerminalWnd::InvalidateAll()
{
	if( m_impl->window ) {
		m_impl->MarkNativeFullDirty();
		m_impl->UpdateScrollbar();
		m_impl->UpdateCaret();
		::InvalidateRect(m_impl->window, nullptr, FALSE);
	}
}

CTerminalWnd::FrameSurfaceResult CTerminalWnd::SetFrameHost(
	const std::string_view hostId ) noexcept
{
	return m_impl->frameSurface.SetHost(hostId);
}

CTerminalWnd::FrameSurfaceResult CTerminalWnd::SetFrameVisible(
	const bool visible ) noexcept
{
	const auto result = m_impl->frameSurface.SetVisible(visible);
	if (result.Accepted()) m_impl->PublishNativeSurfaceRegistration(true);
	return result;
}

CTerminalWnd::FrameSurfaceResult CTerminalWnd::NotifyFrameContent() noexcept
{
	return m_impl->frameSurface.NotifyContent();
}

CTerminalWnd::FrameSurfaceResult CTerminalWnd::NotifyFrameLayout() noexcept
{
	m_impl->MarkNativeFullDirty();
	const auto result = m_impl->frameSurface.NotifyLayout();
	if (result.Accepted()) m_impl->PublishNativeSurfaceRegistration(true);
	return result;
}

CTerminalWnd::FrameSurfaceResult CTerminalWnd::NotifyFrameDeviceEpoch(
	const std::uint64_t epoch ) noexcept
{
	m_impl->MarkNativeFullDirty();
	const auto result = m_impl->frameSurface.NotifyDeviceEpoch(epoch);
	if (result.Accepted()) m_impl->PublishNativeSurfaceRegistration(true);
	return result;
}

std::optional<CTerminalWnd::FrameSurfaceSnapshotType> CTerminalWnd::CommitGdiFrame() noexcept
{
	const auto committed = m_impl->frameSurface.CommitGdiFrame();
	if (committed) m_impl->CaptureNativeFrame(*committed);
	return committed;
}

CTerminalWnd::FrameSurfaceSnapshotType CTerminalWnd::FrameSurfaceSnapshot() const noexcept
{
	return m_impl->frameSurface.Snapshot();
}

CTerminalWnd::FrameSurfaceSnapshotType CTerminalWnd::GetFrameSurfaceSnapshot() const noexcept
{
	return FrameSurfaceSnapshot();
}

CTerminalWnd::FrameSurfaceId CTerminalWnd::GetFrameSurfaceId() const noexcept
{
	return m_impl->frameSurface.StableSurfaceId();
}

CTerminalWnd::FrameSurfaceId CTerminalWnd::StableSurfaceId() const noexcept
{
	return GetFrameSurfaceId();
}

bool CTerminalWnd::HasPendingFrame() const noexcept
{
	return m_impl->frameSurface.HasPendingFrame();
}

void CTerminalWnd::SetNativeFrameBridge(TerminalNativeFrameBridgePtr bridge) noexcept
{
	m_impl->SetNativeFrameBridge(std::move(bridge));
}

void CTerminalWnd::SetNativeFrameReadySink(std::function<void()> sink) noexcept
{
	m_impl->SetNativeFrameReadySink(std::move(sink));
}

std::shared_ptr<const CTerminalWnd::NativeFrame> CTerminalWnd::TakeNativeFrame() noexcept
{
	return m_impl->TakeNativeFrame();
}

bool CTerminalWnd::PreTranslateMessage( MSG& message )
{
	if( m_impl->closed || !m_impl->window || message.hwnd != m_impl->window ||
		(message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN &&
		 message.message != WM_KEYUP && message.message != WM_SYSKEYUP) ) return false;
	const auto event = KeyEventFromMessage(message);
	if( event.alt && (event.virtualKey == VK_F4 || event.virtualKey == VK_SPACE) ) return false;
	if( event.keyDown ) {
		switch( ResolveTerminalShortcut(event, m_impl->HasSelection()) ) {
		case TerminalShortcutAction::Copy:
			static_cast<void>(CopySelectionToClipboard());
			return true;
		case TerminalShortcutAction::Paste:
			static_cast<void>(PasteFromClipboard());
			return true;
		case TerminalShortcutAction::SendInterrupt:
			m_impl->Send(std::string_view("\x03", 1));
			return true;
		case TerminalShortcutAction::None:
			break;
		}
	}
	if( m_impl->inputAdapter ) {
		// Printable keys are completed by TranslateMessage/WM_CHAR. The VT input
		// adapter can return an engaged but empty value for their WM_KEYDOWN because
		// UnicodeChar is not available yet. Consuming it would suppress WM_CHAR.
		if( const auto encoded = m_impl->inputAdapter->EncodeKey(event); encoded && !encoded->empty() ) {
			m_impl->Send(*encoded);
			return true;
		}
	}
	if( event.keyDown ) {
		const auto bytes = EncodeTerminalKey(event);
		if( !bytes.empty() ) {
			m_impl->Send(bytes);
			return true;
		}
	}
	if( event.keyDown && message.message == WM_SYSKEYDOWN && event.alt ) {
		const auto printable = EncodeAltPrintable(message);
		if( !printable.empty() ) {
			m_impl->Send(printable);
			return true;
		}
	}
	// Finish the text-input path inside this hook. The frame consults the legacy
	// accelerator table after this, and the default key assignments bind bare Space to
	// F_INDENT_SPACE and Shift+Space to F_UNINDENT_SPACE. Once TranslateAccelerator
	// consumes the WM_KEYDOWN, TranslateMessage never runs and the WM_CHAR this
	// terminal forwards to the shell is never produced. VS Code likewise sends text
	// input to the shell except for bindings explicitly excluded through
	// terminal.integrated.commandsToSkipShell.
	if( TerminalKeyNeedsTextDelivery(event, MapsToCharacter(event.virtualKey)) ) {
		::TranslateMessage(&message);
		::DispatchMessageW(&message);
		return true;
	}
	return false;
}

void CTerminalWnd::Focus()
{
	if( m_impl->window ) ::SetFocus(m_impl->window);
}

void CTerminalWnd::Close() noexcept
{
	if( !m_impl || m_impl->closed ) return;
	m_impl->closed = true;
	m_impl->nativeFrameReadySink = {};
	if (m_impl->nativeFrameBridge) {
		const auto snapshot = m_impl->frameSurface.Snapshot();
		m_impl->nativeFrameBridge->CloseSurface(
			snapshot.surfaceId, snapshot.surfaceLifetimeEpoch);
		m_impl->nativeFrameBridge->Close();
	}
	m_impl->nativeSurfacePublisher.Close();
	m_impl->nativeFrameBridge.reset();
	static_cast<void>(m_impl->frameSurface.Close());
	if( m_impl->window ) ::KillTimer(m_impl->window, kInputRetryTimer);
	if( m_impl->window ) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->ReleaseBackBuffer();
	m_impl->dwriteRenderer.Close();
	m_impl->ReleaseRenderFonts();
	m_impl->model = nullptr;
	m_impl->inputAdapter = nullptr;
	m_impl->inputSink = {};
	m_impl->resizeSink = {};
}

HWND CTerminalWnd::GetHwnd() const noexcept
{
	return m_impl->window;
}

TerminalSize CTerminalWnd::GetTerminalSize() const noexcept
{
	return m_impl->terminalSize;
}

TerminalViewportDiagnosticSnapshot CTerminalWnd::GetViewportDiagnostic() const noexcept
{
	const auto viewport = m_impl->Viewport();
	return {
		.scrollOffset = m_impl->scrollOffset,
		.topRow = viewport.topRow,
		.totalRows = viewport.totalRows,
		.visibleRows = viewport.visibleRows,
	};
}

bool CTerminalWnd::HasSelection() const noexcept
{
	return m_impl->HasSelection();
}

bool CTerminalWnd::CopySelectionToClipboard()
{
	return m_impl->CopySelectionToClipboard();
}

bool CTerminalWnd::PasteFromClipboard()
{
	return m_impl->PasteFromClipboard();
}

LRESULT CALLBACK CTerminalWnd::WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	if( message == WM_NCCREATE ) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* impl = static_cast<Impl*>(create->lpCreateParams);
		if( impl ) {
			impl->window = window;
			::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
		}
	}
	auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if( impl ) {
		if( message == WM_NCDESTROY ) {
			// Fence the logical lifetime even when the parent destroys the native
			// child directly. Close() normally performs this first, but this branch
			// is the final ownership boundary for late frame work.
			static_cast<void>(impl->frameSurface.Close());
			impl->window = nullptr;
			::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return impl->HandleMessage(message, wParam, lParam);
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace terminal
