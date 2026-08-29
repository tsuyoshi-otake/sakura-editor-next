/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/tmux/TmuxRuntimeAdapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace terminal::tmux {
namespace {

std::atomic<std::uint64_t> g_nextOperationId{ 1 };

[[nodiscard]] HarnessOperationId MakeOperationId(const std::uint64_t value) noexcept
{
	HarnessOperationId result;
	for (std::size_t index = 0; index < sizeof(value); ++index) {
		result.value[index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
	return result;
}

template<typename Scalar>
void AppendUtf8(std::string& output, Scalar scalar)
{
	char32_t value = static_cast<char32_t>(scalar);
	if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
	if (value <= 0x7f) {
		output.push_back(static_cast<char>(value));
	} else if (value <= 0x7ff) {
		output.push_back(static_cast<char>(0xc0 | (value >> 6)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else if (value <= 0xffff) {
		output.push_back(static_cast<char>(0xe0 | (value >> 12)));
		output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else {
		output.push_back(static_cast<char>(0xf0 | (value >> 18)));
		output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	}
}

[[nodiscard]] std::string WideToUtf8(std::wstring_view value)
{
	std::string result;
	result.reserve(value.size());
	if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto first = static_cast<char32_t>(value[index]);
			if (first >= 0xd800 && first <= 0xdbff && index + 1 < value.size()) {
				const auto second = static_cast<char32_t>(value[index + 1]);
				if (second >= 0xdc00 && second <= 0xdfff) {
					AppendUtf8(result, 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00));
					++index;
					continue;
				}
			}
			AppendUtf8(result, first);
		}
	} else {
		for (const auto character : value) AppendUtf8(result, character);
	}
	return result;
}

[[nodiscard]] std::string Utf16ToUtf8(std::u16string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto first = static_cast<char32_t>(value[index]);
		if (first >= 0xd800 && first <= 0xdbff && index + 1 < value.size()) {
			const auto second = static_cast<char32_t>(value[index + 1]);
			if (second >= 0xdc00 && second <= 0xdfff) {
				AppendUtf8(result, 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00));
				++index;
				continue;
			}
		}
		AppendUtf8(result, first);
	}
	return result;
}

[[nodiscard]] std::optional<std::u16string> Utf8ToUtf16(std::string_view value)
{
	std::u16string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		std::uint32_t codePoint{};
		std::size_t count{};
		if (first <= 0x7f) {
			codePoint = first;
			count = 1;
		} else if (first >= 0xc2 && first <= 0xdf) {
			codePoint = first & 0x1f;
			count = 2;
		} else if (first >= 0xe0 && first <= 0xef) {
			codePoint = first & 0x0f;
			count = 3;
		} else if (first >= 0xf0 && first <= 0xf4) {
			codePoint = first & 0x07;
			count = 4;
		} else {
			return std::nullopt;
		}
		if (index + count > value.size()) return std::nullopt;
		for (std::size_t offset = 1; offset < count; ++offset) {
			const auto continuation = static_cast<unsigned char>(value[index + offset]);
			if ((continuation & 0xc0) != 0x80) return std::nullopt;
			codePoint = (codePoint << 6) | (continuation & 0x3f);
		}
		const auto minimum = count == 1 ? 0U : count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff
			|| (codePoint >= 0xd800 && codePoint <= 0xdfff)) return std::nullopt;
		if (codePoint <= 0xffff) {
			result.push_back(static_cast<char16_t>(codePoint));
		} else {
			codePoint -= 0x10000;
			result.push_back(static_cast<char16_t>(0xd800 + (codePoint >> 10)));
			result.push_back(static_cast<char16_t>(0xdc00 + (codePoint & 0x3ff)));
		}
		index += count;
	}
	return result;
}

[[nodiscard]] std::optional<TerminalNamedKey> ToNamedKey(std::string_view value) noexcept
{
	static constexpr std::pair<std::string_view, TerminalNamedKey> names[] = {
		std::pair<std::string_view, TerminalNamedKey>{ "Enter", TerminalNamedKey::Enter },
		{ "Escape", TerminalNamedKey::Escape }, { "Tab", TerminalNamedKey::Tab },
		{ "BSpace", TerminalNamedKey::BSpace }, { "Space", TerminalNamedKey::Space },
		{ "Up", TerminalNamedKey::Up }, { "Down", TerminalNamedKey::Down },
		{ "Left", TerminalNamedKey::Left }, { "Right", TerminalNamedKey::Right },
		{ "Home", TerminalNamedKey::Home }, { "End", TerminalNamedKey::End },
		{ "PageUp", TerminalNamedKey::PageUp }, { "PageDown", TerminalNamedKey::PageDown },
		{ "Insert", TerminalNamedKey::Insert }, { "Delete", TerminalNamedKey::Delete },
		{ "F1", TerminalNamedKey::F1 }, { "F2", TerminalNamedKey::F2 },
		{ "F3", TerminalNamedKey::F3 }, { "F4", TerminalNamedKey::F4 },
		{ "F5", TerminalNamedKey::F5 }, { "F6", TerminalNamedKey::F6 },
		{ "F7", TerminalNamedKey::F7 }, { "F8", TerminalNamedKey::F8 },
		{ "F9", TerminalNamedKey::F9 }, { "F10", TerminalNamedKey::F10 },
		{ "F11", TerminalNamedKey::F11 }, { "F12", TerminalNamedKey::F12 },
	};
	for (const auto& [name, key] : names) if (name == value) return key;
	return std::nullopt;
}

[[nodiscard]] std::optional<std::u16string> ToControlKey(std::string_view value)
{
	if (value.size() == 3 && value[0] == 'C' && value[1] == '-') {
		const auto key = static_cast<unsigned char>(value[2]);
		if (key >= 'a' && key <= 'z') return std::u16string(1, static_cast<char16_t>(key - 'a' + 1));
		if (key >= 'A' && key <= 'Z') return std::u16string(1, static_cast<char16_t>(key - 'A' + 1));
		switch (key) {
		case '@': return std::u16string(1, u'\0');
		case '[': return std::u16string(1, u'\x1b');
		case '\\': return std::u16string(1, u'\x1c');
		case ']': return std::u16string(1, u'\x1d');
		case '^': return std::u16string(1, u'\x1e');
		case '_': return std::u16string(1, u'\x1f');
		case '?': return std::u16string(1, u'\x7f');
		default: break;
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsDead(const TerminalInstanceSnapshot& snapshot) noexcept
{
	return snapshot.state == TerminalInstanceState::Terminalized
		|| snapshot.state == TerminalInstanceState::Retired
		|| snapshot.state == TerminalInstanceState::Closing;
}

void AppendLayout(const runtime::topology::TerminalLayoutNode& node, std::string& output)
{
	if (node.IsLeaf()) {
		output.push_back('%');
		output += std::to_string(node.pane.paneId.value);
		return;
	}
	output.push_back('(');
	const auto separator = node.orientation == TerminalPaneOrientation::Horizontal ? '|' : '-';
	for (std::size_t index = 0; index < node.children.size(); ++index) {
		if (index != 0) output.push_back(separator);
		AppendLayout(node.children[index], output);
	}
	output.push_back(')');
}

[[nodiscard]] TmuxRuntimeResult Result(const TmuxRuntimeCode code,
	const TerminalTopologyRevision revision = {}) noexcept
{
	TmuxRuntimeResult result;
	result.code = code;
	result.revision = revision;
	return result;
}

[[nodiscard]] TmuxRuntimeCode MapOperationCode(const TerminalRuntimeOperationCode code) noexcept
{
	switch (code) {
	case TerminalRuntimeOperationCode::Succeeded: return TmuxRuntimeCode::Succeeded;
	case TerminalRuntimeOperationCode::InvalidRequest: return TmuxRuntimeCode::InvalidRequest;
	case TerminalRuntimeOperationCode::TargetMissing: return TmuxRuntimeCode::TargetMissing;
	case TerminalRuntimeOperationCode::TopologyChanged: return TmuxRuntimeCode::TopologyChanged;
	case TerminalRuntimeOperationCode::NotRunning: return TmuxRuntimeCode::NotRunning;
	case TerminalRuntimeOperationCode::Denied: return TmuxRuntimeCode::Denied;
	case TerminalRuntimeOperationCode::DeadlineExceeded: return TmuxRuntimeCode::DeadlineExceeded;
	case TerminalRuntimeOperationCode::Cancelled:
	case TerminalRuntimeOperationCode::ServerStopping: return TmuxRuntimeCode::Stopped;
	case TerminalRuntimeOperationCode::ResourceExhausted: return TmuxRuntimeCode::ResourceExhausted;
	case TerminalRuntimeOperationCode::Ambiguous: return TmuxRuntimeCode::Ambiguous;
	case TerminalRuntimeOperationCode::Unsupported: return TmuxRuntimeCode::Unsupported;
	case TerminalRuntimeOperationCode::OperationUnknown:
	case TerminalRuntimeOperationCode::AlreadyTerminal:
	case TerminalRuntimeOperationCode::InternalError: return TmuxRuntimeCode::Unavailable;
	}
	return TmuxRuntimeCode::Unavailable;
}

} // namespace

std::optional<HarnessOperationId> TmuxOperationIdAllocator::Allocate() noexcept
{
	for (;;) {
		auto current = g_nextOperationId.load(std::memory_order_relaxed);
		if (current == 0) return std::nullopt;
		const auto next = current == (std::numeric_limits<std::uint64_t>::max)() ? 0 : current + 1;
		if (g_nextOperationId.compare_exchange_weak(current, next,
			std::memory_order_relaxed, std::memory_order_relaxed)) return MakeOperationId(current);
	}
}

TmuxRuntimeAdapter::TmuxRuntimeAdapter(
	ITerminalRuntimeService& runtime,
	TmuxCollectionSnapshotProvider collectionSnapshot,
	TmuxInstanceSnapshotProvider instanceSnapshot,
	TmuxWaitChannelService& waitChannels,
	TmuxRuntimeAdapterLimits limits)
	: m_runtime(runtime)
	, m_collectionSnapshot(std::move(collectionSnapshot))
	, m_instanceSnapshot(std::move(instanceSnapshot))
	, m_waitChannels(waitChannels)
	, m_limits(std::move(limits))
{
	if (m_limits.maximumInputBytes == 0) m_limits.maximumInputBytes = 1;
	if (m_limits.captureTimeout <= std::chrono::seconds::zero()) m_limits.captureTimeout = std::chrono::seconds{ 1 };
	if (m_limits.maximumWait <= std::chrono::seconds::zero()) m_limits.maximumWait = std::chrono::seconds{ 1 };
}

TmuxRuntimeAdapter::~TmuxRuntimeAdapter()
{
	BeginShutdown();
}

std::optional<runtime::topology::TerminalCollectionSnapshot> TmuxRuntimeAdapter::CollectionSnapshot() const
{
	if (!m_collectionSnapshot) return std::nullopt;
	try {
		return m_collectionSnapshot();
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<TerminalInstanceSnapshot> TmuxRuntimeAdapter::InstanceSnapshot(const TerminalInstanceId id) const
{
	if (!m_instanceSnapshot || !id.IsValid()) return std::nullopt;
	try {
		return m_instanceSnapshot(id);
	} catch (...) {
		return std::nullopt;
	}
}

TmuxRuntimeSnapshot TmuxRuntimeAdapter::Snapshot() const
{
	TmuxRuntimeSnapshot result;
	try {
		const auto collection = CollectionSnapshot();
		if (!collection) return result;
		result.revision = collection->revision;
		result.activeSession = collection->activeSession;
		result.sessions.reserve(collection->sessions.size());
		for (std::size_t sessionIndex = 0; sessionIndex < collection->sessions.size(); ++sessionIndex) {
			const auto& sourceSession = collection->sessions[sessionIndex];
			TmuxSessionView session;
			session.id = sourceSession.id;
			session.name = sourceSession.name;
			session.index = sessionIndex;
			session.active = collection->activeSession && *collection->activeSession == sourceSession.id;
			session.attached = false;
			session.windows.reserve(sourceSession.windows.size());
			for (std::size_t windowIndex = 0; windowIndex < sourceSession.windows.size(); ++windowIndex) {
				const auto& sourceWindow = sourceSession.windows[windowIndex];
				TmuxWindowView window;
				window.id = sourceWindow.id;
				window.name = sourceWindow.name;
				window.index = windowIndex;
				window.active = sourceSession.activeWindow == sourceWindow.id;
				window.panes.reserve(sourceWindow.panes.size());
				for (std::size_t paneIndex = 0; paneIndex < sourceWindow.panes.size(); ++paneIndex) {
					const auto& sourcePane = sourceWindow.panes[paneIndex];
					TmuxPaneView pane;
					pane.id = sourcePane.id;
					pane.instanceId = sourcePane.instanceId;
					pane.index = paneIndex;
					pane.active = sourceWindow.activePane == sourcePane.id;
					pane.coordinate.sessionId = sourceSession.id;
					pane.coordinate.windowId = sourceWindow.id;
					pane.coordinate.paneId = sourcePane.id;
					pane.coordinate.instanceId = sourcePane.instanceId;
					if (const auto instance = InstanceSnapshot(sourcePane.instanceId)) {
						pane.coordinate = instance->coordinate;
						pane.coordinate.sessionId = sourceSession.id;
						pane.coordinate.windowId = sourceWindow.id;
						pane.coordinate.paneId = sourcePane.id;
						pane.coordinate.instanceId = sourcePane.instanceId;
						pane.width = instance->columns;
						pane.height = instance->rows;
						pane.historySize = instance->scrollbackSize;
						pane.historyLimit = instance->scrollbackLimit;
						pane.title = WideToUtf8(instance->sequenceTitle);
						pane.currentCommand = WideToUtf8(instance->processName);
						pane.dead = IsDead(*instance);
						if (instance->outcome && instance->outcome->processExitCode) pane.deadStatus = *instance->outcome->processExitCode;
					}
					window.width = (std::max)(window.width, pane.width);
					window.height = (std::max)(window.height, pane.height);
					window.panes.push_back(std::move(pane));
				}
				AppendLayout(sourceWindow.root, window.layout);
				session.windows.push_back(std::move(window));
			}
			result.sessions.push_back(std::move(session));
		}
	} catch (...) {
		return {};
	}
	return result;
}

TmuxRuntimeResult TmuxRuntimeAdapter::CheckRevision(const TerminalTopologyRevision expected) const
{
	const auto snapshot = CollectionSnapshot();
	if (!snapshot) return Result(TmuxRuntimeCode::Unavailable);
	if (!expected.IsValid()) return Result(TmuxRuntimeCode::InvalidRequest, snapshot->revision);
	if (snapshot->revision != expected) return Result(TmuxRuntimeCode::TopologyChanged, snapshot->revision);
	return Result(TmuxRuntimeCode::Succeeded, snapshot->revision);
}

std::optional<HarnessOperationId> TmuxRuntimeAdapter::OperationId() noexcept
{
	return m_operationIds.Allocate();
}

TerminalTargetCoordinate TmuxRuntimeAdapter::TargetCoordinate(const TmuxResolvedTarget& target)
{
	auto coordinate = target.coordinate;
	coordinate.sessionId = target.sessionId;
	coordinate.windowId = target.windowId;
	if (target.paneId.IsValid()) coordinate.paneId = target.paneId;
	return coordinate;
}

TmuxRuntimeResult TmuxRuntimeAdapter::MapTopology(const TerminalTopologyResult& source)
{
	TmuxRuntimeResult result = Result(MapOperationCode(source.code), source.revision);
	result.sessionId = source.sessionId;
	result.windowId = source.windowId;
	result.paneId = source.paneId;
	return result;
}

TmuxRuntimeResult TmuxRuntimeAdapter::MapInput(const TerminalInputResult& source)
{
	TmuxRuntimeResult result;
	switch (source.code) {
	case TerminalInputResultCode::Accepted: result.code = TmuxRuntimeCode::Succeeded; break;
	case TerminalInputResultCode::InvalidInput: result.code = TmuxRuntimeCode::InvalidRequest; break;
	case TerminalInputResultCode::UnsupportedKey: result.code = TmuxRuntimeCode::Unsupported; break;
	case TerminalInputResultCode::TargetMissing: result.code = TmuxRuntimeCode::TargetMissing; break;
	case TerminalInputResultCode::StaleGeneration: result.code = TmuxRuntimeCode::TopologyChanged; break;
	case TerminalInputResultCode::NotRunning: result.code = TmuxRuntimeCode::NotRunning; break;
	case TerminalInputResultCode::QueueFull: result.code = TmuxRuntimeCode::ResourceExhausted; break;
	case TerminalInputResultCode::Denied: result.code = TmuxRuntimeCode::Denied; break;
	case TerminalInputResultCode::DeadlineExceeded: result.code = TmuxRuntimeCode::DeadlineExceeded; break;
	case TerminalInputResultCode::BrokerStopping: result.code = TmuxRuntimeCode::Stopped; break;
	case TerminalInputResultCode::Ambiguous: result.code = TmuxRuntimeCode::Ambiguous; break;
	}
	return result;
}

TmuxCaptureResult TmuxRuntimeAdapter::MapCapture(
	const TerminalCaptureResult& source, const TmuxRuntimeAdapterLimits& limits)
{
	TmuxCaptureResult result;
	switch (source.code) {
	case TerminalCaptureResultCode::Succeeded: result.code = TmuxRuntimeCode::Succeeded; break;
	case TerminalCaptureResultCode::TargetMissing: result.code = TmuxRuntimeCode::TargetMissing; break;
	case TerminalCaptureResultCode::StaleCursor: result.code = TmuxRuntimeCode::TopologyChanged; result.gap = true; break;
	case TerminalCaptureResultCode::InvalidRequest: result.code = TmuxRuntimeCode::InvalidRequest; break;
	case TerminalCaptureResultCode::NotRunning: result.code = TmuxRuntimeCode::NotRunning; break;
	case TerminalCaptureResultCode::DeadlineExceeded: result.code = TmuxRuntimeCode::DeadlineExceeded; break;
	case TerminalCaptureResultCode::ResourceExhausted: result.code = TmuxRuntimeCode::ResourceExhausted; result.truncated = true; break;
	case TerminalCaptureResultCode::Denied: result.code = TmuxRuntimeCode::Denied; break;
	}
	result.gap = result.gap || source.gap || source.resyncSnapshot;
	result.truncated = result.truncated || source.truncated;
	std::size_t bytes{};
	for (const auto& line : source.lines) {
		const auto text = Utf16ToUtf8(line.text);
		if (text.size() > limits.captureLimits.maximumUtf8Bytes
			|| bytes > limits.captureLimits.maximumUtf8Bytes - text.size()) {
			result.truncated = true;
			break;
		}
		bytes += text.size();
		result.lines.push_back({ text, line.wrapped, line.joined });
	}
	result.complete = result.code == TmuxRuntimeCode::Succeeded && !result.gap && !result.truncated;
	return result;
}

TmuxRuntimeResult TmuxRuntimeAdapter::CreateSession(const TmuxCreateSessionRequest& request)
{
	if (!request.directory.empty()) return Result(TmuxRuntimeCode::Unsupported);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	TerminalSessionCreateRequest common;
	common.operationId = *operation;
	common.name = request.name;
	common.detached = request.detached;
	return MapTopology(m_runtime.CreateSession(common));
}

TmuxRuntimeResult TmuxRuntimeAdapter::CreateTerminalWindow(const TmuxCreateWindowRequest& request)
{
	if (!request.sessionId.IsValid()) return Result(TmuxRuntimeCode::InvalidRequest);
	if (!request.directory.empty()) return Result(TmuxRuntimeCode::Unsupported);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	TerminalWindowCreateRequest common;
	common.operationId = *operation;
	common.sessionId = request.sessionId;
	common.name = request.name;
	common.detached = request.detached;
	return MapTopology(m_runtime.CreateTerminalWindow(common));
}

TmuxRuntimeResult TmuxRuntimeAdapter::SplitWindow(const TmuxSplitWindowRequest& request)
{
	if (!request.target.paneId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	if (request.directory || request.length || request.percentage) return Result(TmuxRuntimeCode::Unsupported);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	TerminalPaneSplitRequest common;
	common.operationId = *operation;
	common.paneId = request.target.paneId;
	common.orientation = request.orientation;
	common.detached = request.detached;
	return MapTopology(m_runtime.SplitPane(common));
}

TmuxRuntimeResult TmuxRuntimeAdapter::SelectWindow(const TmuxSelectRequest& request)
{
	if (!request.target.windowId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	return MapTopology(m_runtime.SelectWindow({ *operation, request.target.windowId }));
}

TmuxRuntimeResult TmuxRuntimeAdapter::SelectPane(const TmuxSelectRequest& request)
{
	if (!request.target.paneId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	return MapTopology(m_runtime.SelectPane({ *operation, request.target.paneId }));
}

TmuxRuntimeResult TmuxRuntimeAdapter::ClosePane(const TmuxCloseRequest& request)
{
	if (!request.target.paneId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	return MapTopology(m_runtime.ClosePane({ *operation, request.target.paneId }));
}

TmuxRuntimeResult TmuxRuntimeAdapter::CloseWindow(const TmuxCloseRequest& request)
{
	if (!request.target.windowId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	return MapTopology(m_runtime.CloseWindow({ *operation, request.target.windowId }));
}

TmuxRuntimeResult TmuxRuntimeAdapter::CloseSession(const TmuxCloseRequest& request)
{
	if (!request.target.sessionId.IsValid()) return Result(TmuxRuntimeCode::TargetMissing);
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	return MapTopology(m_runtime.CloseSession({ *operation, request.target.sessionId }));
}

TmuxRuntimeResult TmuxRuntimeAdapter::SendKeys(const TmuxInputBatch& request)
{
	if (!request.target.paneId.IsValid() || !request.target.coordinate.instanceId.IsValid()) {
		return Result(TmuxRuntimeCode::TargetMissing);
	}
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) return revision;
	if (request.repeatCount == 0 || request.repeatCount > 1000) return Result(TmuxRuntimeCode::InvalidRequest, revision.revision);
	const auto operation = OperationId();
	if (!operation) return Result(TmuxRuntimeCode::IdentityExhausted, revision.revision);
	TerminalInputBatch common;
	common.operationId = *operation;
	common.target = TargetCoordinate(request.target);
	common.repeatCount = request.repeatCount;
	std::size_t inputBytes{};
	if (request.tokens.empty()) common.actions.push_back({ TerminalInputActionKind::LiteralText, {}, TerminalNamedKey::Enter });
	for (const auto& token : request.tokens) {
		if (token.text.size() > m_limits.maximumInputBytes
			|| inputBytes > m_limits.maximumInputBytes - token.text.size()) return Result(TmuxRuntimeCode::ResourceExhausted, revision.revision);
		inputBytes += token.text.size();
		if (token.kind == TmuxInputTokenKind::LiteralText) {
			const auto text = Utf8ToUtf16(token.text);
			if (!text) return Result(TmuxRuntimeCode::InvalidRequest, revision.revision);
			common.actions.push_back({ TerminalInputActionKind::LiteralText, *text, TerminalNamedKey::Enter });
		} else {
			if (const auto named = ToNamedKey(token.text)) {
				common.actions.push_back({ TerminalInputActionKind::NamedKey, {}, *named });
			} else if (const auto control = ToControlKey(token.text)) {
				// The common DTO has no modifier-bearing NamedKey variant. A
				// control byte is the existing TerminalInputAdapter key-path's
				// wire result, not a printable "^C" string.
				common.actions.push_back({ TerminalInputActionKind::LiteralText, *control, TerminalNamedKey::Enter });
			} else {
				return Result(TmuxRuntimeCode::Unsupported, revision.revision);
			}
		}
	}
	if (inputBytes > m_limits.maximumInputBytes / request.repeatCount) return Result(TmuxRuntimeCode::ResourceExhausted, revision.revision);
	return MapInput(m_runtime.QueueInputBatch(common));
}

TmuxCaptureResult TmuxRuntimeAdapter::CapturePane(const TmuxCaptureRequest& request)
{
	if (!request.target.paneId.IsValid() || !request.target.coordinate.instanceId.IsValid()) {
		TmuxCaptureResult result;
		result.code = TmuxRuntimeCode::TargetMissing;
		return result;
	}
	const auto revision = CheckRevision(request.expectedRevision);
	if (!revision.Succeeded()) {
		TmuxCaptureResult result;
		result.code = revision.code;
		return result;
	}
	const auto operation = OperationId();
	if (!operation) {
		TmuxCaptureResult result;
		result.code = TmuxRuntimeCode::IdentityExhausted;
		return result;
	}
	TerminalCaptureRequest common;
	common.operationId = *operation;
	common.target = TargetCoordinate(request.target);
	common.startLine = request.startAtHistoryBeginning
		? std::optional<std::int64_t>((std::numeric_limits<std::int64_t>::min)()) : request.startLine;
	common.endLine = request.endAtScreenEnd ? std::nullopt : request.endLine;
	common.joinWrappedLines = request.joinWrapped;
	common.limits = m_limits.captureLimits;
	common.deadline = std::chrono::steady_clock::now() + m_limits.captureTimeout;
	return MapCapture(m_runtime.Capture(common), m_limits);
}

TmuxRuntimeResult TmuxRuntimeAdapter::WaitFor(const TmuxWaitRequest& request)
{
	if (request.expectedRevision.IsValid()) {
		const auto revision = CheckRevision(request.expectedRevision);
		if (!revision.Succeeded()) return revision;
	}
	const auto now = std::chrono::steady_clock::now();
	const auto cap = now + m_limits.maximumWait;
	const auto deadline = request.deadline == std::chrono::steady_clock::time_point{}
		? cap : (request.deadline < cap ? request.deadline : cap);
	TmuxWaitResult result;
	switch (request.operation) {
	case TmuxWaitOperation::Wait: result = m_waitChannels.Wait(request.channel, deadline); break;
	case TmuxWaitOperation::Signal: result = m_waitChannels.Signal(request.channel); break;
	case TmuxWaitOperation::Lock: result = m_waitChannels.Lock(request.channel, deadline); break;
	case TmuxWaitOperation::Unlock: result = m_waitChannels.Unlock(request.channel); break;
	}
	TmuxRuntimeResult mapped;
	switch (result.code) {
	case TmuxWaitCode::Succeeded: mapped.code = TmuxRuntimeCode::Succeeded; break;
	case TmuxWaitCode::InvalidRequest: mapped.code = TmuxRuntimeCode::InvalidRequest; break;
	case TmuxWaitCode::ResourceExhausted: mapped.code = TmuxRuntimeCode::ResourceExhausted; break;
	case TmuxWaitCode::DeadlineExceeded: mapped.code = TmuxRuntimeCode::DeadlineExceeded; break;
	case TmuxWaitCode::Stopped: mapped.code = TmuxRuntimeCode::Stopped; break;
	}
	return mapped;
}

void TmuxRuntimeAdapter::BeginShutdown() noexcept
{
	m_waitChannels.BeginShutdown();
}

} // namespace terminal::tmux
