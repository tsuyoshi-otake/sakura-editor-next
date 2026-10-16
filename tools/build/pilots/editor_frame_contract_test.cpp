#include <sakura/editor/EditorFrameEvents.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace sakura::editor;

namespace {

bool Expect(bool condition, const char* label)
{
	if (!condition) std::cerr << "FAIL: " << label << '\n';
	return condition;
}

class RecordingSink final : public IEditorFrameEventSink {
public:
	EditorFrameEffect HandleEditorFrameEvent(const EditorFrameEvent& event) override
	{
		m_last.emplace(event);
		++m_calls;
		return { EditorFrameEffectKind::Handled, 17 };
	}

	[[nodiscard]] const EditorFrameEvent& Last() const { return *m_last; }
	[[nodiscard]] std::uint32_t Calls() const noexcept { return m_calls; }

private:
	std::optional<EditorFrameEvent> m_last;
	std::uint32_t m_calls{};
};

bool DispatchDeliversOneEventAndReturnsTheTypedEffect()
{
	RecordingSink sink;
	const EditorFrameEvent resize{
		EditorFrameEventKind::Resized,
		{},
		{ 1280, 720 },
		2,
	};
	const auto effect = DispatchEditorFrameEvent(sink, resize);
	if (!Expect(sink.Calls() == 1, "sink receives exactly one event")) return false;
	bool passed = Expect(sink.Last().Kind() == EditorFrameEventKind::Resized, "event kind retained");
	passed = Expect(sink.Last().Size().Width() == 1280 && sink.Last().Size().Height() == 720, "size retained") && passed;
	passed = Expect(sink.Last().Detail() == 2, "detail retained") && passed;
	return Expect(effect.Kind() == EditorFrameEffectKind::Handled && effect.Result() == 17, "typed effect retained") && passed;
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
	TestCase{"DispatchDeliversOneEventAndReturnsTheTypedEffect", DispatchDeliversOneEventAndReturnsTheTypedEffect},
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
	std::string_view filter = "EditorFrame.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "EditorFrame.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "EditorFrame." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
