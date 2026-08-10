#pragma once

#include <cstdint>

namespace sakura::editor {

enum class EditorFrameEventKind : std::uint8_t {
	Activated,
	Deactivated,
	FocusGained,
	FocusLost,
	Enabled,
	Disabled,
	Resized,
	Moved,
	CloseRequested,
	DpiChanged,
};

class EditorFramePoint final {
public:
	constexpr EditorFramePoint(std::int32_t x = 0, std::int32_t y = 0) noexcept
		: m_x(x), m_y(y) {}
	[[nodiscard]] constexpr std::int32_t X() const noexcept { return m_x; }
	[[nodiscard]] constexpr std::int32_t Y() const noexcept { return m_y; }

private:
	const std::int32_t m_x;
	const std::int32_t m_y;
};

class EditorFrameSize final {
public:
	constexpr EditorFrameSize(std::int32_t width = 0, std::int32_t height = 0) noexcept
		: m_width(width), m_height(height) {}
	[[nodiscard]] constexpr std::int32_t Width() const noexcept { return m_width; }
	[[nodiscard]] constexpr std::int32_t Height() const noexcept { return m_height; }

private:
	const std::int32_t m_width;
	const std::int32_t m_height;
};

class EditorFrameEvent final {
public:
	constexpr EditorFrameEvent(
		EditorFrameEventKind kind = EditorFrameEventKind::Deactivated,
		EditorFramePoint point = {}, EditorFrameSize size = {},
		std::uint32_t detail = 0) noexcept
		: m_kind(kind), m_point(point), m_size(size), m_detail(detail) {}
	[[nodiscard]] constexpr EditorFrameEventKind Kind() const noexcept { return m_kind; }
	[[nodiscard]] constexpr const EditorFramePoint& Point() const noexcept { return m_point; }
	[[nodiscard]] constexpr const EditorFrameSize& Size() const noexcept { return m_size; }
	[[nodiscard]] constexpr std::uint32_t Detail() const noexcept { return m_detail; }

private:
	const EditorFrameEventKind m_kind;
	const EditorFramePoint m_point;
	const EditorFrameSize m_size;
	const std::uint32_t m_detail;
};

enum class EditorFrameEffectKind : std::uint8_t {
	Handled,
	ForwardToDefault,
	CloseAccepted,
	CloseRefused,
};

class EditorFrameEffect final {
public:
	constexpr EditorFrameEffect(
		EditorFrameEffectKind kind = EditorFrameEffectKind::ForwardToDefault,
		std::intptr_t result = 0) noexcept
		: m_kind(kind), m_result(result) {}
	[[nodiscard]] constexpr EditorFrameEffectKind Kind() const noexcept { return m_kind; }
	[[nodiscard]] constexpr std::intptr_t Result() const noexcept { return m_result; }

private:
	const EditorFrameEffectKind m_kind;
	const std::intptr_t m_result;
};

class IEditorFrameEventSink {
public:
	virtual ~IEditorFrameEventSink() = default;
	[[nodiscard]] virtual EditorFrameEffect HandleEditorFrameEvent(
		const EditorFrameEvent& event) = 0;
};

[[nodiscard]] EditorFrameEffect DispatchEditorFrameEvent(
	IEditorFrameEventSink& sink, const EditorFrameEvent& event);

} // namespace sakura::editor
