#include <sakura/editor/win32/Win32EditorFrameAdapter.h>

#include <iostream>

using namespace sakura::editor;
using namespace sakura::editor::win32;

namespace {

int failures = 0;

void Expect(bool condition, const char* label)
{
	if (!condition) {
		std::cerr << "FAIL: " << label << '\n';
		++failures;
	}
}

} // namespace

int main()
{
	const auto activated = Win32EditorFrameAdapter::Translate(WM_ACTIVATEAPP, TRUE, 0);
	Expect(activated && activated->Kind() == EditorFrameEventKind::Activated, "activate translation");

	const auto resized = Win32EditorFrameAdapter::Translate(WM_SIZE, SIZE_RESTORED, MAKELPARAM(800, 600));
	Expect(resized && resized->Kind() == EditorFrameEventKind::Resized, "resize translation");
	Expect(resized && resized->Size().Width() == 800 && resized->Size().Height() == 600, "resize values");

	const auto moved = Win32EditorFrameAdapter::Translate(WM_MOVE, 0, MAKELPARAM(-20, 30));
	Expect(moved && moved->Point().X() == -20 && moved->Point().Y() == 30, "signed move translation");
	Expect(!Win32EditorFrameAdapter::Translate(WM_PAINT, 0, 0), "unsupported message fails closed");

	const wchar_t* className = L"SakuraEditorFrameAdapterContract";
	WNDCLASSW windowClass{};
	windowClass.hInstance = ::GetModuleHandleW(nullptr);
	windowClass.lpfnWndProc = ::DefWindowProcW;
	windowClass.lpszClassName = className;
	const ATOM atom = ::RegisterClassW(&windowClass);
	Expect(atom != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "hidden class registration");
	auto window = ::CreateWindowExW(0, className, L"", WS_OVERLAPPED,
		0, 0, 32, 32, nullptr, nullptr, windowClass.hInstance, nullptr);
	Expect(window != nullptr, "hidden window created");
	if (window != nullptr) {
		const auto effect = EditorFrameEffect{ EditorFrameEffectKind::Handled, 23 };
		Expect(Win32EditorFrameAdapter::Apply(window, WM_NULL, 0, 0, effect) == 23,
			"typed effect applied on real hidden window");
		::DestroyWindow(window);
	}
	return failures == 0 ? 0 : 1;
}
