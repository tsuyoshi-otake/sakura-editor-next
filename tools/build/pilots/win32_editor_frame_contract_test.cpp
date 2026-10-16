#include <sakura/editor/win32/Win32EditorFrameAdapter.h>

#include <array>
#include <iostream>
#include <string>
#include <string_view>

using namespace sakura::editor;
using namespace sakura::editor::win32;

namespace {

bool Expect(bool condition, const char* label)
{
	if (!condition) std::cerr << "FAIL: " << label << '\n';
	return condition;
}

bool TranslatesFrameMessagesAndFailsClosedOnOthers()
{
	const auto activated = Win32EditorFrameAdapter::Translate(WM_ACTIVATEAPP, TRUE, 0);
	bool passed = Expect(activated && activated->Kind() == EditorFrameEventKind::Activated, "activate translation");

	const auto resized = Win32EditorFrameAdapter::Translate(WM_SIZE, SIZE_RESTORED, MAKELPARAM(800, 600));
	passed = Expect(resized && resized->Kind() == EditorFrameEventKind::Resized, "resize translation") && passed;
	passed = Expect(resized && resized->Size().Width() == 800 && resized->Size().Height() == 600, "resize values") && passed;

	const auto moved = Win32EditorFrameAdapter::Translate(WM_MOVE, 0, MAKELPARAM(-20, 30));
	passed = Expect(moved && moved->Point().X() == -20 && moved->Point().Y() == 30, "signed move translation") && passed;
	return Expect(!Win32EditorFrameAdapter::Translate(WM_PAINT, 0, 0), "unsupported message fails closed") && passed;
}

bool AppliesTheTypedEffectToARealHiddenWindow()
{
	const wchar_t* className = L"SakuraEditorFrameAdapterContract";
	WNDCLASSW windowClass{};
	windowClass.hInstance = ::GetModuleHandleW(nullptr);
	windowClass.lpfnWndProc = ::DefWindowProcW;
	windowClass.lpszClassName = className;
	const ATOM atom = ::RegisterClassW(&windowClass);
	if (!Expect(atom != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "hidden class registration")) return false;
	auto window = ::CreateWindowExW(0, className, L"", WS_OVERLAPPED,
		0, 0, 32, 32, nullptr, nullptr, windowClass.hInstance, nullptr);
	if (!Expect(window != nullptr, "hidden window created")) return false;
	const auto effect = EditorFrameEffect{ EditorFrameEffectKind::Handled, 23 };
	const bool passed = Expect(Win32EditorFrameAdapter::Apply(window, WM_NULL, 0, 0, effect) == 23,
		"typed effect applied on real hidden window");
	::DestroyWindow(window);
	return passed;
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept : m_name(name), m_run(run) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	const std::string_view m_name;
	bool (*const m_run)();
};

constexpr std::array kTests{
	TestCase{"TranslatesFrameMessagesAndFailsClosedOnOthers", TranslatesFrameMessagesAndFailsClosedOnOthers},
	TestCase{"AppliesTheTypedEffectToARealHiddenWindow", AppliesTheTypedEffectToARealHiddenWindow},
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "Win32EditorFrameAdapter.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "Win32EditorFrameAdapter.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "Win32EditorFrameAdapter." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
