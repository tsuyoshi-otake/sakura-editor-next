/*! @file
 * @brief HWND-free logical terminal session/window/pane topology.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace terminal::runtime::topology {

// Re-export the shared logical IDs here for callers that work directly with
// topology records. They are not HWND or process handles, and they are never
// reused by a model instance.
using ::terminal::TerminalSessionId;
using ::terminal::TerminalWindowId;
using ::terminal::TerminalPaneId;
using ::terminal::TerminalInstanceId;
using ::terminal::TerminalTopologyRevision;
using ::terminal::TerminalPaneOrientation;

enum class TerminalLayoutNodeKind : std::uint8_t {
	Pane,
	Split,
};

struct TerminalPaneNode final {
	TerminalPaneId paneId;
	TerminalInstanceId instanceId;
	friend constexpr bool operator==(const TerminalPaneNode&, const TerminalPaneNode&) = default;
};

// A value tree is used deliberately: topology transactions can construct a
// replacement tree and swap it into a record without exposing parent pointers
// or UI handles. A split may have two or more same-axis children; this is the
// value equivalent of joining a split into its immediate sibling row/column.
struct TerminalLayoutNode final {
	TerminalLayoutNodeKind kind{ TerminalLayoutNodeKind::Pane };
	TerminalPaneNode pane;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	std::vector<std::uint16_t> weights;
	std::vector<TerminalLayoutNode> children;

	[[nodiscard]] static TerminalLayoutNode Leaf(TerminalPaneId paneId, TerminalInstanceId instanceId) noexcept;
	[[nodiscard]] static TerminalLayoutNode Split(TerminalPaneOrientation orientation,
		std::vector<TerminalLayoutNode> children, std::vector<std::uint16_t> weights = {});
	[[nodiscard]] bool IsLeaf() const noexcept { return kind == TerminalLayoutNodeKind::Pane; }
};

struct TerminalPaneSnapshot final {
	TerminalPaneId id;
	TerminalInstanceId instanceId;
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
};

struct TerminalWindowSnapshot final {
	TerminalWindowId id;
	std::string name;
	TerminalLayoutNode root;
	TerminalPaneId activePane;
	std::vector<TerminalPaneSnapshot> panes;
};

struct TerminalSessionSnapshot final {
	TerminalSessionId id;
	std::string name;
	std::vector<TerminalWindowId> order;
	TerminalWindowId activeWindow;
	std::vector<TerminalWindowSnapshot> windows;
};

struct TerminalCollectionSnapshot final {
	TerminalTopologyRevision revision;
	std::optional<TerminalSessionId> activeSession;
	std::vector<TerminalSessionId> order;
	std::vector<TerminalSessionSnapshot> sessions;
};

struct TerminalCollectionLimits final {
	std::size_t maximumSessions{ 4096 };
	std::size_t maximumWindows{ 4096 };
	std::size_t maximumPanes{ 4096 };
	std::size_t maximumNameBytes{ 128 };
	std::uint64_t maximumSessionId{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumWindowId{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumPaneId{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumInstanceId{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumTopologyRevision{ (std::numeric_limits<std::uint64_t>::max)() };
};

enum class ETerminalCollectionResultCode : std::uint8_t {
	Succeeded,
	InvalidRequest,
	StaleRevision,
	TargetMissing,
	NameConflict,
	ResourceExhausted,
	IdentityExhausted,
	Stopped,
};

struct TerminalCollectionTopologyResult final {
	ETerminalCollectionResultCode code{ ETerminalCollectionResultCode::InvalidRequest };
	TerminalTopologyRevision revision;
	std::optional<TerminalSessionId> sessionId;
	std::optional<TerminalWindowId> windowId;
	std::optional<TerminalPaneId> paneId;
	std::optional<TerminalInstanceId> instanceId;
	bool changed{};

	[[nodiscard]] bool Succeeded() const noexcept { return code == ETerminalCollectionResultCode::Succeeded; }
};

using ETerminalTopologyResultCode = ETerminalCollectionResultCode;
using TerminalTopologyResult = TerminalCollectionTopologyResult;
using TerminalCollectionResultCode = ETerminalCollectionResultCode;
using TerminalCollectionResult = TerminalCollectionTopologyResult;

struct TerminalCollectionSessionCreateRequest final {
	std::string name;
	std::string initialWindowName;
	std::optional<TerminalTopologyRevision> expectedRevision;
	bool createInitialWindow{ true };
};

struct TerminalCollectionWindowCreateRequest final {
	TerminalSessionId sessionId;
	std::string name;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionPaneSplitRequest final {
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionSessionSelectRequest final {
	TerminalSessionId sessionId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionWindowSelectRequest final {
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionPaneSelectRequest final {
	TerminalPaneId paneId;
	std::optional<TerminalSessionId> sessionId;
	std::optional<TerminalWindowId> windowId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionPaneCloseRequest final {
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionWindowCloseRequest final {
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

struct TerminalCollectionSessionCloseRequest final {
	TerminalSessionId sessionId;
	std::optional<TerminalTopologyRevision> expectedRevision;
};

// Transitional aliases keep the model pleasant to use while the runtime
// service's common request DTOs are being composed. They intentionally remain
// scoped to topology and do not redeclare the shared runtime DTOs.
using TerminalSessionCreateRequest = TerminalCollectionSessionCreateRequest;
using TerminalWindowCreateRequest = TerminalCollectionWindowCreateRequest;
using TerminalPaneSplitRequest = TerminalCollectionPaneSplitRequest;
using TerminalSessionSelectRequest = TerminalCollectionSessionSelectRequest;
using TerminalWindowSelectRequest = TerminalCollectionWindowSelectRequest;
using TerminalPaneSelectRequest = TerminalCollectionPaneSelectRequest;
using TerminalPaneCloseRequest = TerminalCollectionPaneCloseRequest;
using TerminalWindowCloseRequest = TerminalCollectionWindowCloseRequest;
using TerminalSessionCloseRequest = TerminalCollectionSessionCloseRequest;

//! The model is intended to be called from the runtime's serialized UI
//! executor. It owns only logical records; process/session/model/HWND lifetime
//! belongs to the runtime service and is not represented here.
class TerminalCollectionModel final {
public:
	TerminalCollectionModel();
	explicit TerminalCollectionModel(TerminalCollectionLimits limits);

	[[nodiscard]] TerminalCollectionSnapshot Snapshot() const;
	[[nodiscard]] TerminalTopologyRevision Revision() const noexcept { return m_revision; }
	[[nodiscard]] bool ValidateInvariants() const noexcept;
	//! Keeps the topology allocator ahead of IDs reserved by the runtime
	//! authority's standalone CreateInstance path. This does not revive an
	//! exhausted allocator (zero remains the exhausted sentinel).
	void EnsureNextInstanceId(std::uint64_t nextId) noexcept
	{
		if (nextId != 0 && m_nextInstanceId != 0 && nextId > m_nextInstanceId) {
			m_nextInstanceId = nextId;
		}
	}

	[[nodiscard]] TerminalTopologyResult CreateSession(const TerminalCollectionSessionCreateRequest& request);
	[[nodiscard]] TerminalTopologyResult CreateSession(std::string name = {},
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult CreateTerminalWindow(const TerminalCollectionWindowCreateRequest& request);
	[[nodiscard]] TerminalTopologyResult CreateTerminalWindow(TerminalSessionId sessionId, std::string name = {},
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult SplitPane(const TerminalCollectionPaneSplitRequest& request);
	[[nodiscard]] TerminalTopologyResult SplitPane(TerminalSessionId sessionId, TerminalWindowId windowId,
		TerminalPaneId paneId, TerminalPaneOrientation orientation,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);

	[[nodiscard]] TerminalTopologyResult SelectSession(const TerminalCollectionSessionSelectRequest& request);
	[[nodiscard]] TerminalTopologyResult SelectSession(TerminalSessionId sessionId,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult SelectWindow(const TerminalCollectionWindowSelectRequest& request);
	[[nodiscard]] TerminalTopologyResult SelectWindow(TerminalSessionId sessionId, TerminalWindowId windowId,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult SelectPane(const TerminalCollectionPaneSelectRequest& request);
	[[nodiscard]] TerminalTopologyResult SelectPane(TerminalPaneId paneId,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);

	[[nodiscard]] TerminalTopologyResult ClosePane(const TerminalCollectionPaneCloseRequest& request);
	[[nodiscard]] TerminalTopologyResult ClosePane(TerminalSessionId sessionId, TerminalWindowId windowId,
		TerminalPaneId paneId, std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult CloseWindow(const TerminalCollectionWindowCloseRequest& request);
	[[nodiscard]] TerminalTopologyResult CloseWindow(TerminalSessionId sessionId, TerminalWindowId windowId,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);
	[[nodiscard]] TerminalTopologyResult CloseSession(const TerminalCollectionSessionCloseRequest& request);
	[[nodiscard]] TerminalTopologyResult CloseSession(TerminalSessionId sessionId,
		std::optional<TerminalTopologyRevision> expectedRevision = std::nullopt);

	[[nodiscard]] std::optional<TerminalSessionSnapshot> FindSession(TerminalSessionId id) const;
	[[nodiscard]] std::optional<TerminalWindowSnapshot> FindWindow(TerminalSessionId sessionId,
		TerminalWindowId id) const;
	[[nodiscard]] std::optional<TerminalPaneSnapshot> FindPane(TerminalPaneId id) const;

private:
	struct SessionState final {
		TerminalSessionId id;
		std::string name;
		std::vector<TerminalWindowId> order;
		TerminalWindowId activeWindow;
	};
	struct WindowState final {
		TerminalWindowId id;
		std::string name;
		TerminalLayoutNode root;
		TerminalPaneId activePane;
	};
	struct PaneState final {
		TerminalPaneId id;
		TerminalInstanceId instanceId;
		TerminalSessionId sessionId;
		TerminalWindowId windowId;
	};

	template <typename T>
	struct IdHash final {
		std::size_t operator()(const T& id) const noexcept
		{
			return static_cast<std::size_t>(id.value ^ (id.value >> 32));
		}
	};

	[[nodiscard]] TerminalTopologyResult BaseResult(ETerminalTopologyResultCode code) const noexcept;
	[[nodiscard]] TerminalTopologyResult CheckMutation(std::optional<TerminalTopologyRevision> expected) const noexcept;
	[[nodiscard]] bool CanAdvanceRevision() const noexcept;
	void AdvanceRevision() noexcept;
	[[nodiscard]] bool IsNameAvailable(const std::string& name,
		std::optional<TerminalSessionId> except = std::nullopt) const noexcept;
	[[nodiscard]] bool IsWindowNameAvailable(const SessionState& session, const std::string& name,
		std::optional<TerminalWindowId> except = std::nullopt) const noexcept;
	[[nodiscard]] bool AllocateSessionId(TerminalSessionId& id) noexcept;
	[[nodiscard]] bool AllocateWindowId(TerminalWindowId& id) noexcept;
	[[nodiscard]] bool AllocatePaneId(TerminalPaneId& id) noexcept;
	[[nodiscard]] bool AllocateInstanceId(TerminalInstanceId& id) noexcept;
	[[nodiscard]] bool HasPane(const TerminalLayoutNode& node, TerminalPaneId id) const noexcept;
	[[nodiscard]] bool FindPanePath(const TerminalLayoutNode& node, TerminalPaneId id,
		std::vector<std::size_t>& path) const;
	[[nodiscard]] static std::vector<TerminalPaneId> Leaves(const TerminalLayoutNode& node);
	[[nodiscard]] static bool IsValidOrientation(TerminalPaneOrientation orientation) noexcept;
	[[nodiscard]] static bool IsValidName(const std::string& value, std::size_t maximumBytes) noexcept;
	[[nodiscard]] static bool RemoveAtPath(TerminalLayoutNode& node, const std::vector<std::size_t>& path,
		std::size_t depth);
	[[nodiscard]] static bool SplitAtPath(TerminalLayoutNode& node, const std::vector<std::size_t>& path,
		std::size_t depth, TerminalPaneOrientation orientation, const TerminalLayoutNode& newLeaf);
	void RemoveWindowRecords(TerminalSessionId sessionId, TerminalWindowId windowId);
	void RemoveSessionRecords(TerminalSessionId sessionId);
	[[nodiscard]] TerminalWindowSnapshot MakeWindowSnapshot(TerminalSessionId sessionId,
		const WindowState& window) const;
	[[nodiscard]] TerminalSessionSnapshot MakeSessionSnapshot(const SessionState& session) const;

	TerminalCollectionLimits m_limits;
	TerminalTopologyRevision m_revision;
	std::uint64_t m_nextSessionId{ 1 };
	std::uint64_t m_nextWindowId{ 1 };
	std::uint64_t m_nextPaneId{ 1 };
	std::uint64_t m_nextInstanceId{ 1 };
	std::vector<TerminalSessionId> m_order;
	std::optional<TerminalSessionId> m_activeSession;
	std::unordered_map<TerminalSessionId, SessionState, IdHash<TerminalSessionId>> m_sessions;
	std::unordered_map<TerminalWindowId, WindowState, IdHash<TerminalWindowId>> m_windows;
	std::unordered_map<TerminalPaneId, PaneState, IdHash<TerminalPaneId>> m_panes;
};

} // namespace terminal::runtime::topology
