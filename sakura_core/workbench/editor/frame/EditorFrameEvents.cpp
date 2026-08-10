#include <sakura/editor/EditorFrameEvents.h>

namespace sakura::editor {

EditorFrameEffect DispatchEditorFrameEvent(
	IEditorFrameEventSink& sink, const EditorFrameEvent& event)
{
	return sink.HandleEditorFrameEvent(event);
}

} // namespace sakura::editor
