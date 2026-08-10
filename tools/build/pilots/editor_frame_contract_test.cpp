#include <sakura/editor/EditorFrameEvents.h>

#include <cstdint>
#include <iostream>
#include <optional>

using namespace sakura::editor;

namespace {

int failures = 0;

void Expect(bool condition, const char* label)
{
	if (!condition) {
		std::cerr << "FAIL: " << label << '\n';
		++failures;
	}
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

} // namespace

int main()
{
	RecordingSink sink;
	const EditorFrameEvent resize{
		EditorFrameEventKind::Resized,
		{},
		{ 1280, 720 },
		2,
	};
	const auto effect = DispatchEditorFrameEvent(sink, resize);
	Expect(sink.Calls() == 1, "sink receives exactly one event");
	Expect(sink.Last().Kind() == EditorFrameEventKind::Resized, "event kind retained");
	Expect(sink.Last().Size().Width() == 1280 && sink.Last().Size().Height() == 720, "size retained");
	Expect(sink.Last().Detail() == 2, "detail retained");
	Expect(effect.Kind() == EditorFrameEffectKind::Handled && effect.Result() == 17, "typed effect retained");
	return failures == 0 ? 0 : 1;
}
