#pragma once

#include <optional>
#include <cstdint>

#include <Windows.h>

#include <sakura/editor/EditorFrameEvents.h>

namespace sakura::editor::win32 {

using NativeWindowHandle = decltype(::GetActiveWindow());

class Win32EditorFrameAdapter final {
public:
	[[nodiscard]] static std::optional<EditorFrameEvent> Translate(
		std::uint32_t message, std::uintptr_t wordParameter,
		std::intptr_t longParameter) noexcept;

	[[nodiscard]] static std::intptr_t Apply(
		NativeWindowHandle window, std::uint32_t message,
		std::uintptr_t wordParameter, std::intptr_t longParameter,
		const EditorFrameEffect& effect) noexcept;
};

} // namespace sakura::editor::win32
