#include <sakura/editor/win32/Win32EditorFrameAdapter.h>

#include <windowsx.h>

namespace sakura::editor::win32 {

std::optional<EditorFrameEvent> Win32EditorFrameAdapter::Translate(
	std::uint32_t message, std::uintptr_t wordParameter,
	std::intptr_t longParameter) noexcept
{
	switch (message) {
	case WM_ACTIVATEAPP:
		return EditorFrameEvent{ wordParameter != FALSE
			? EditorFrameEventKind::Activated
			: EditorFrameEventKind::Deactivated };
	case WM_SETFOCUS:
		return EditorFrameEvent{ EditorFrameEventKind::FocusGained };
	case WM_KILLFOCUS:
		return EditorFrameEvent{ EditorFrameEventKind::FocusLost };
	case WM_ENABLE:
		return EditorFrameEvent{ wordParameter != FALSE
			? EditorFrameEventKind::Enabled
			: EditorFrameEventKind::Disabled };
	case WM_SIZE:
		return EditorFrameEvent{
			EditorFrameEventKind::Resized, {},
			EditorFrameSize{ static_cast<std::int32_t>(LOWORD(longParameter)),
				static_cast<std::int32_t>(HIWORD(longParameter)) },
			static_cast<std::uint32_t>(wordParameter) };
	case WM_MOVE:
		return EditorFrameEvent{
			EditorFrameEventKind::Moved,
			EditorFramePoint{ static_cast<std::int16_t>(LOWORD(longParameter)),
				static_cast<std::int16_t>(HIWORD(longParameter)) } };
	case WM_CLOSE:
		return EditorFrameEvent{ EditorFrameEventKind::CloseRequested };
	case WM_DPICHANGED:
		return EditorFrameEvent{ EditorFrameEventKind::DpiChanged, {}, {},
			static_cast<std::uint32_t>(HIWORD(wordParameter)) };
	default:
		return std::nullopt;
	}
}

std::intptr_t Win32EditorFrameAdapter::Apply(
	NativeWindowHandle window, std::uint32_t message,
	std::uintptr_t wordParameter, std::intptr_t longParameter,
	const EditorFrameEffect& effect) noexcept
{
	switch (effect.Kind()) {
	case EditorFrameEffectKind::Handled:
	case EditorFrameEffectKind::CloseAccepted:
	case EditorFrameEffectKind::CloseRefused:
		return effect.Result();
	case EditorFrameEffectKind::ForwardToDefault:
		return ::DefWindowProcW(window, message, wordParameter, longParameter);
	}
	return ::DefWindowProcW(window, message, wordParameter, longParameter);
}

} // namespace sakura::editor::win32
