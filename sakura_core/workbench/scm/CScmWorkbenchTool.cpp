/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace workbench::scm {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraNativeScmTool";
constexpr UINT kResultMessage = WM_APP + 0x5a1;
constexpr UINT_PTR kRefreshTimer = 0x5a2;
	constexpr UINT kRefreshMilliseconds = 5000;
constexpr std::size_t kMaximumStatusBytes = 4u * 1024u * 1024u;

struct WorkerResult { std::uint64_t generation{}; GitScmState state; };
struct Gate {
	std::mutex mutex;
	HWND window{};
	bool alive{};
	std::vector<std::unique_ptr<WorkerResult>> pending;
};
struct SharedState {
	std::mutex mutex;
	std::wstring root;
	std::atomic<std::uint64_t> generation{ 1 };
	std::shared_ptr<Gate> gate = std::make_shared<Gate>();
	HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.hInstance = instance;
	wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	wc.lpfnWndProc = CScmWorkbenchTool::WindowProc;
	wc.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::wstring GitPath()
{
	wchar_t path[MAX_PATH]{};
	const DWORD length = ::SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, path, nullptr);
	return length != 0 && length < MAX_PATH ? std::wstring(path, length) : std::wstring{};
}

std::vector<std::uint8_t> RunGitStatus(const std::wstring& root, HANDLE stop, bool& succeeded)
{
	succeeded = false;
	const auto executable = GitPath();
	if (executable.empty() || root.empty()) return {};
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	HANDLE readPipe{};
	HANDLE writePipe{};
	if (!::CreatePipe(&readPipe, &writePipe, &security, 0)) return {};
	::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
	HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (nullInput == INVALID_HANDLE_VALUE) nullInput = nullptr;
	std::wstring command = L"\"" + executable + L"\" -C \"" + root
		+ L"\" status --porcelain=v2 --branch -z --untracked-files=normal";
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = nullInput;
	startup.hStdOutput = writePipe;
	startup.hStdError = writePipe;
	PROCESS_INFORMATION process{};
	const BOOL created = ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process);
	if (nullInput) ::CloseHandle(nullInput);
	::CloseHandle(writePipe);
	if (!created) { ::CloseHandle(readPipe); return {}; }

	std::vector<std::uint8_t> output;
	output.reserve(64u * 1024u);
	const auto deadline = ::GetTickCount64() + 3000;
	bool finished = false;
	while (!finished) {
		if (::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || ::GetTickCount64() >= deadline
			|| output.size() >= kMaximumStatusBytes) {
			::TerminateProcess(process.hProcess, ERROR_CANCELLED);
			(void)::WaitForSingleObject(process.hProcess, 500);
			break;
		}
		DWORD available{};
		if (::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available != 0) {
			std::array<std::uint8_t, 16384> buffer{};
			DWORD read{};
			if (::ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) && read != 0) {
				output.insert(output.end(), buffer.begin(), buffer.begin() + read);
			}
		}
		finished = ::WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0;
	}
	if (finished) {
		for (;;) {
			DWORD available{};
			if (!::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
			std::array<std::uint8_t, 16384> buffer{};
			DWORD read{};
			if (!::ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || read == 0) break;
			if (output.size() + read > kMaximumStatusBytes) break;
			output.insert(output.end(), buffer.begin(), buffer.begin() + read);
		}
		DWORD exitCode{};
		succeeded = ::GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;
	}
	::CloseHandle(readPipe);
	::CloseHandle(process.hThread);
	::CloseHandle(process.hProcess);
	return output;
}

void PostResult(const std::shared_ptr<SharedState>& shared, std::unique_ptr<WorkerResult> result)
{
	const auto raw = result.get();
	const auto gate = shared->gate;
	std::lock_guard lock(gate->mutex);
	if (!gate->alive || gate->window == nullptr) return;
	gate->pending.push_back(std::move(result));
	if (!::PostMessageW(gate->window, kResultMessage, 0, reinterpret_cast<LPARAM>(raw))) gate->pending.pop_back();
}

void WorkerMain(std::shared_ptr<SharedState> shared)
{
	HANDLE waits[] = { shared->stop, shared->wake };
	for (;;) {
		const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kRefreshMilliseconds);
		if (wait == WAIT_OBJECT_0) return;
		std::wstring root;
		{
			std::lock_guard lock(shared->mutex);
			root = shared->root;
		}
		const auto generation = shared->generation.load(std::memory_order_acquire);
		if (root.empty()) continue;
		bool succeeded{};
		const auto bytes = RunGitStatus(root, shared->stop, succeeded);
		auto result = std::make_unique<WorkerResult>();
		result->generation = generation;
		if (succeeded) result->state = ParsePorcelainV2({ reinterpret_cast<const char*>(bytes.data()), bytes.size() });
		PostResult(shared, std::move(result));
	}
}

} // namespace

struct CScmWorkbenchTool::Impl {
	std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
	std::thread worker;
	HWND window{};
	HWND list{};
	unsigned int dpi{ 96 };
	std::wstring root;
	GitScmState state;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont font;
	FileActivationCallback activateFile;
	StateChangedCallback stateChanged;
	bool active{};
	bool closed{};

	void Start() { if (!worker.joinable()) worker = std::thread(WorkerMain, shared); }
	void NotifyWindow(HWND target, bool alive) {
		std::lock_guard lock(shared->gate->mutex);
		shared->gate->window = target;
		shared->gate->alive = alive;
		if (!alive) shared->gate->pending.clear();
	}
	std::unique_ptr<WorkerResult> Take(WorkerResult* raw) {
		std::lock_guard lock(shared->gate->mutex);
		auto& values = shared->gate->pending;
		const auto found = std::find_if(values.begin(), values.end(), [raw](const auto& item) { return item.get() == raw; });
		if (found == values.end()) return {};
		auto value = std::move(*found);
		values.erase(found);
		return value;
	}
	void Populate() {
		if (!list) return;
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_RESETCONTENT, 0, 0);
		for (const auto& change : state.changes) {
			std::wstring label(1, change.status);
			label += L"   ";
			label += change.path;
			::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
		}
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		::InvalidateRect(list, nullptr, TRUE);
		::InvalidateRect(window, nullptr, TRUE);
	}
	void ActivateSelection() {
		if (!activateFile || !list) return;
		const auto index = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
		if (index < 0 || static_cast<std::size_t>(index) >= state.changes.size()) return;
		std::wstring path = root;
		if (!path.empty() && path.back() != L'\\') path += L'\\';
		path += state.changes[static_cast<std::size_t>(index)].path;
		activateFile(path);
	}
};

CScmWorkbenchTool::CScmWorkbenchTool() : m_impl(std::make_unique<Impl>()) {}
CScmWorkbenchTool::~CScmWorkbenchTool() { Close(); }

bool CScmWorkbenchTool::Create(HWND parent)
{
	if (m_impl->closed || m_impl->window || !parent) return false;
	auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (!instance) instance = ::GetModuleHandleW(nullptr);
	if (!EnsureClass(instance)) return false;
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (!m_impl->window) return false;
	m_impl->list = ::CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(1), instance, nullptr);
	if (!m_impl->list) { Close(); return false; }
	m_impl->NotifyWindow(m_impl->window, true);
	m_impl->Start();
	Refresh();
	return true;
}

void CScmWorkbenchTool::Layout(const RECT& rect, unsigned int dpi)
{
	if (m_impl->closed || !m_impl->window) return;
	m_impl->dpi = dpi == 0 ? 96 : dpi;
	if (m_impl->font.Dpi() != m_impl->dpi) (void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
	::SendMessageW(m_impl->list, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), TRUE);
	::SetWindowPos(m_impl->window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
	const int header = ::MulDiv(30, m_impl->dpi, 96);
	::MoveWindow(m_impl->list, 0, header, rect.right - rect.left, std::max(0L, rect.bottom - rect.top - header), TRUE);
}

void CScmWorkbenchTool::Activate() { m_impl->active = true; if (m_impl->list) ::SetFocus(m_impl->list); Refresh(); }
void CScmWorkbenchTool::Deactivate() { m_impl->active = false; }
bool CScmWorkbenchTool::PreTranslateMessage(MSG& message) {
	if (!m_impl->active || message.hwnd != m_impl->list) return false;
	if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) { m_impl->ActivateSelection(); return true; }
	return false;
}

void CScmWorkbenchTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	m_impl->closed = true;
	m_impl->NotifyWindow(nullptr, false);
	::SetEvent(m_impl->shared->stop);
	::SetEvent(m_impl->shared->wake);
	if (m_impl->worker.joinable()) m_impl->worker.join();
	if (m_impl->window && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->list = nullptr;
	if (m_impl->shared->stop) { ::CloseHandle(m_impl->shared->stop); m_impl->shared->stop = nullptr; }
	if (m_impl->shared->wake) { ::CloseHandle(m_impl->shared->wake); m_impl->shared->wake = nullptr; }
}

void CScmWorkbenchTool::SetRoot(std::wstring root)
{
	m_impl->root = std::move(root);
	{
		std::lock_guard lock(m_impl->shared->mutex);
		m_impl->shared->root = m_impl->root;
	}
	m_impl->shared->generation.fetch_add(1, std::memory_order_acq_rel);
	Refresh();
}
void CScmWorkbenchTool::SetPalette(const theme::ThemePalette& palette) { m_impl->palette = palette; if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, TRUE); }
void CScmWorkbenchTool::SetFileActivationCallback(FileActivationCallback callback) { m_impl->activateFile = std::move(callback); }
void CScmWorkbenchTool::SetStateChangedCallback(StateChangedCallback callback) { m_impl->stateChanged = std::move(callback); }
void CScmWorkbenchTool::SetVisible(bool visible) { if (m_impl->window) ::ShowWindow(m_impl->window, visible ? SW_SHOW : SW_HIDE); }
void CScmWorkbenchTool::Refresh() { if (!m_impl->closed && m_impl->shared->wake) ::SetEvent(m_impl->shared->wake); }
const GitScmState& CScmWorkbenchTool::State() const noexcept { return m_impl->state; }
HWND CScmWorkbenchTool::GetHwnd() const noexcept { return m_impl->window; }

LRESULT CALLBACK CScmWorkbenchTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CScmWorkbenchTool*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	auto* self = reinterpret_cast<CScmWorkbenchTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self || !self->m_impl) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *self->m_impl;
	switch (message) {
	case WM_SIZE: {
		const int header = ::MulDiv(30, impl.dpi, 96);
		if (impl.list) ::MoveWindow(impl.list, 0, header, LOWORD(lParam), std::max(0, static_cast<int>(HIWORD(lParam)) - header), TRUE);
		return 0;
	}
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		const HBRUSH brush = ::CreateSolidBrush(impl.palette.panel.ToColorRef());
		::FillRect(dc, &paint.rcPaint, brush);
		::DeleteObject(brush);
		if (impl.font.Get()) ::SelectObject(dc, impl.font.Get());
		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		RECT header{}; ::GetClientRect(window, &header); header.bottom = ::MulDiv(30, impl.dpi, 96); header.left += ::MulDiv(10, impl.dpi, 96);
		const auto title = L"SOURCE CONTROL  (" + std::to_wstring(impl.state.changes.size()) + L")";
		::DrawTextW(dc, title.c_str(), -1, &header, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_CTLCOLORLISTBOX: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		::SetBkColor(dc, impl.palette.panel.ToColorRef());
		::SetDCBrushColor(dc, impl.palette.panel.ToColorRef());
		return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == 1 && HIWORD(wParam) == LBN_DBLCLK) { impl.ActivateSelection(); return 0; }
		break;
	case kResultMessage: {
		auto result = impl.Take(reinterpret_cast<WorkerResult*>(lParam));
		if (result && result->generation == impl.shared->generation.load(std::memory_order_acquire)) {
			impl.state = std::move(result->state);
			impl.Populate();
			if (impl.stateChanged) impl.stateChanged(impl.state);
		}
		return 0;
	}
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::scm
