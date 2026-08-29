/*! @file @brief HWND-free logical terminal topology model. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "terminal/runtime/TerminalCollectionModel.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace terminal::runtime::topology {
namespace {

template <typename T>
T NonZeroLimit(T value) noexcept
{
	return value == 0 ? T{ 1 } : value;
}

bool IsValidUtf8(const std::string& value) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80U) {
			if (first == 0 || first < 0x20U || first == 0x7fU) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2U && first <= 0xdfU) {
			continuationCount = 1;
			codePoint = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			continuationCount = 2;
			codePoint = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			continuationCount = 3;
			codePoint = first & 0x07U;
		} else {
			return false;
		}
		if (continuationCount >= value.size() - index) return false;
		for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
			const auto continuation = static_cast<unsigned char>(value[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) return false;
			codePoint = (codePoint << 6U) | (continuation & 0x3fU);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffffU
			|| (codePoint >= 0xd800U && codePoint <= 0xdfffU)
			|| (codePoint >= 0x80U && codePoint <= 0x9fU)) return false;
		index += continuationCount + 1;
	}
	return true;
}

template <typename TId>
bool Contains(const std::vector<TId>& ids, const TId id) noexcept
{
	return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool IsZeroWeight(const std::uint16_t weight) noexcept
{
	return weight == 0;
}

} // namespace

TerminalLayoutNode TerminalLayoutNode::Leaf(const TerminalPaneId paneId, const TerminalInstanceId instanceId) noexcept
{
	TerminalLayoutNode node;
	node.kind = TerminalLayoutNodeKind::Pane;
	node.pane = { paneId, instanceId };
	return node;
}

TerminalLayoutNode TerminalLayoutNode::Split(const TerminalPaneOrientation orientation,
	std::vector<TerminalLayoutNode> children, std::vector<std::uint16_t> weights)
{
	TerminalLayoutNode node;
	node.kind = TerminalLayoutNodeKind::Split;
	node.orientation = orientation;
	node.children = std::move(children);
	if (weights.size() != node.children.size() || std::any_of(weights.begin(), weights.end(), IsZeroWeight)) {
		weights.assign(node.children.size(), 1);
	}
	node.weights = std::move(weights);
	return node;
}

TerminalCollectionModel::TerminalCollectionModel()
	: TerminalCollectionModel(TerminalCollectionLimits{})
{
}

TerminalCollectionModel::TerminalCollectionModel(TerminalCollectionLimits limits)
	: m_limits(std::move(limits))
{
	m_limits.maximumSessions = NonZeroLimit(m_limits.maximumSessions);
	m_limits.maximumWindows = NonZeroLimit(m_limits.maximumWindows);
	m_limits.maximumPanes = NonZeroLimit(m_limits.maximumPanes);
	m_limits.maximumNameBytes = NonZeroLimit(m_limits.maximumNameBytes);
	m_limits.maximumSessionId = NonZeroLimit(m_limits.maximumSessionId);
	m_limits.maximumWindowId = NonZeroLimit(m_limits.maximumWindowId);
	m_limits.maximumPaneId = NonZeroLimit(m_limits.maximumPaneId);
	m_limits.maximumInstanceId = NonZeroLimit(m_limits.maximumInstanceId);
	m_limits.maximumTopologyRevision = NonZeroLimit(m_limits.maximumTopologyRevision);
}

TerminalTopologyResult TerminalCollectionModel::BaseResult(const ETerminalTopologyResultCode code) const noexcept
{
	TerminalTopologyResult result;
	result.code = code;
	result.revision = m_revision;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::CheckMutation(
	const std::optional<TerminalTopologyRevision> expected) const noexcept
{
	if (expected && *expected != m_revision) return BaseResult(ETerminalTopologyResultCode::StaleRevision);
	return BaseResult(ETerminalTopologyResultCode::Succeeded);
}

bool TerminalCollectionModel::CanAdvanceRevision() const noexcept
{
	return m_revision.value != m_limits.maximumTopologyRevision;
}

void TerminalCollectionModel::AdvanceRevision() noexcept
{
	if (CanAdvanceRevision()) ++m_revision.value;
}

bool TerminalCollectionModel::IsNameAvailable(const std::string& name,
	const std::optional<TerminalSessionId> except) const noexcept
{
	for (const auto id : m_order) {
		if (except && id == *except) continue;
		const auto it = m_sessions.find(id);
		if (it != m_sessions.end() && it->second.name == name) return false;
	}
	return true;
}

bool TerminalCollectionModel::IsWindowNameAvailable(const SessionState& session, const std::string& name,
	const std::optional<TerminalWindowId> except) const noexcept
{
	for (const auto id : session.order) {
		if (except && id == *except) continue;
		const auto it = m_windows.find(id);
		if (it != m_windows.end() && it->second.name == name) return false;
	}
	return true;
}

bool TerminalCollectionModel::AllocateSessionId(TerminalSessionId& id) noexcept
{
	if (m_nextSessionId == 0 || m_nextSessionId > m_limits.maximumSessionId) return false;
	id = { m_nextSessionId };
	m_nextSessionId = m_nextSessionId == m_limits.maximumSessionId ? 0 : m_nextSessionId + 1;
	return true;
}

bool TerminalCollectionModel::AllocateWindowId(TerminalWindowId& id) noexcept
{
	if (m_nextWindowId == 0 || m_nextWindowId > m_limits.maximumWindowId) return false;
	id = { m_nextWindowId };
	m_nextWindowId = m_nextWindowId == m_limits.maximumWindowId ? 0 : m_nextWindowId + 1;
	return true;
}

bool TerminalCollectionModel::AllocatePaneId(TerminalPaneId& id) noexcept
{
	if (m_nextPaneId == 0 || m_nextPaneId > m_limits.maximumPaneId) return false;
	id = { m_nextPaneId };
	m_nextPaneId = m_nextPaneId == m_limits.maximumPaneId ? 0 : m_nextPaneId + 1;
	return true;
}

bool TerminalCollectionModel::AllocateInstanceId(TerminalInstanceId& id) noexcept
{
	if (m_nextInstanceId == 0 || m_nextInstanceId > m_limits.maximumInstanceId) return false;
	id = { m_nextInstanceId };
	m_nextInstanceId = m_nextInstanceId == m_limits.maximumInstanceId ? 0 : m_nextInstanceId + 1;
	return true;
}

bool TerminalCollectionModel::IsValidOrientation(const TerminalPaneOrientation orientation) noexcept
{
	return orientation == TerminalPaneOrientation::Horizontal || orientation == TerminalPaneOrientation::Vertical;
}

bool TerminalCollectionModel::IsValidName(const std::string& value, const std::size_t maximumBytes) noexcept
{
	return !value.empty() && value.size() <= maximumBytes && IsValidUtf8(value);
}

bool TerminalCollectionModel::HasPane(const TerminalLayoutNode& node, const TerminalPaneId id) const noexcept
{
	if (node.IsLeaf()) return node.pane.paneId == id;
	return std::any_of(node.children.begin(), node.children.end(), [this, id](const auto& child) {
		return HasPane(child, id);
	});
}

bool TerminalCollectionModel::FindPanePath(const TerminalLayoutNode& node, const TerminalPaneId id,
	std::vector<std::size_t>& path) const
{
	if (node.IsLeaf()) return node.pane.paneId == id;
	for (std::size_t index = 0; index < node.children.size(); ++index) {
		path.push_back(index);
		if (FindPanePath(node.children[index], id, path)) return true;
		path.pop_back();
	}
	return false;
}

std::vector<TerminalPaneId> TerminalCollectionModel::Leaves(const TerminalLayoutNode& node)
{
	if (node.IsLeaf()) return { node.pane.paneId };
	std::vector<TerminalPaneId> result;
	for (const auto& child : node.children) {
		auto leaves = Leaves(child);
		result.insert(result.end(), leaves.begin(), leaves.end());
	}
	return result;
}

bool TerminalCollectionModel::RemoveAtPath(TerminalLayoutNode& node, const std::vector<std::size_t>& path,
	const std::size_t depth)
{
	if (node.IsLeaf() || depth >= path.size()) return false;
	const auto childIndex = path[depth];
	if (childIndex >= node.children.size()) return false;
	if (depth + 1 == path.size()) {
		node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(childIndex));
		if (node.children.size() == 1) {
			TerminalLayoutNode replacement = std::move(node.children.front());
			node = std::move(replacement);
		} else if (!node.children.empty()) {
			node.weights.erase(node.weights.begin() + static_cast<std::ptrdiff_t>(childIndex));
		}
		return true;
	}
	return RemoveAtPath(node.children[childIndex], path, depth + 1);
}

bool TerminalCollectionModel::SplitAtPath(TerminalLayoutNode& node, const std::vector<std::size_t>& path,
	const std::size_t depth, const TerminalPaneOrientation orientation, const TerminalLayoutNode& newLeaf)
{
	if (depth == path.size()) return false;
	if (node.IsLeaf()) return false;
	const auto childIndex = path[depth];
	if (childIndex >= node.children.size()) return false;
	if (depth + 1 == path.size()) {
		if (!node.children[childIndex].IsLeaf()) return false;
		if (node.orientation == orientation) {
			node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(childIndex + 1), newLeaf);
			node.weights.assign(node.children.size(), 1);
		} else {
			std::vector<TerminalLayoutNode> children;
			children.push_back(node.children[childIndex]);
			children.push_back(newLeaf);
			node.children[childIndex] = TerminalLayoutNode::Split(orientation, std::move(children), { 1, 1 });
		}
		return true;
	}
	return SplitAtPath(node.children[childIndex], path, depth + 1, orientation, newLeaf);
}

TerminalTopologyResult TerminalCollectionModel::CreateSession(const TerminalSessionCreateRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	if (m_order.size() >= m_limits.maximumSessions || !CanAdvanceRevision()) {
		return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	}
	if (!request.name.empty() && !IsValidName(request.name, m_limits.maximumNameBytes)) {
		return BaseResult(ETerminalTopologyResultCode::InvalidRequest);
	}
	if (!request.initialWindowName.empty() && !IsValidName(request.initialWindowName, m_limits.maximumNameBytes)) {
		return BaseResult(ETerminalTopologyResultCode::InvalidRequest);
	}
	if (!request.name.empty() && !IsNameAvailable(request.name)) {
		return BaseResult(ETerminalTopologyResultCode::NameConflict);
	}
	if (request.createInitialWindow
		&& (m_windows.size() >= m_limits.maximumWindows || m_panes.size() >= m_limits.maximumPanes)) {
		return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	}

	TerminalSessionId sessionId;
	if (!AllocateSessionId(sessionId)) return BaseResult(ETerminalTopologyResultCode::IdentityExhausted);
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	TerminalInstanceId instanceId;
	if (request.createInitialWindow
		&& (!AllocateWindowId(windowId) || !AllocatePaneId(paneId) || !AllocateInstanceId(instanceId))) {
		return BaseResult(ETerminalTopologyResultCode::IdentityExhausted);
	}

	const auto sessionName = request.name.empty() ? std::to_string(sessionId.value) : request.name;
	if (!IsValidName(sessionName, m_limits.maximumNameBytes) || !IsNameAvailable(sessionName)) {
		return BaseResult(ETerminalTopologyResultCode::NameConflict);
	}
	SessionState session{ sessionId, sessionName, {}, {} };
	if (request.createInitialWindow) {
		const auto windowName = request.initialWindowName.empty() ? std::to_string(windowId.value) : request.initialWindowName;
		if (!IsValidName(windowName, m_limits.maximumNameBytes)
			|| !IsWindowNameAvailable(session, windowName)) return BaseResult(ETerminalTopologyResultCode::NameConflict);
		session.order.push_back(windowId);
		session.activeWindow = windowId;
		m_windows.emplace(windowId, WindowState{ windowId, windowName, TerminalLayoutNode::Leaf(paneId, instanceId), paneId });
		m_panes.emplace(paneId, PaneState{ paneId, instanceId, sessionId, windowId });
	}
	m_sessions.emplace(sessionId, std::move(session));
	m_order.push_back(sessionId);
	m_activeSession = sessionId;
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = sessionId;
	if (request.createInitialWindow) {
		result.windowId = windowId;
		result.paneId = paneId;
		result.instanceId = instanceId;
	}
	return result;
}

TerminalTopologyResult TerminalCollectionModel::CreateSession(std::string name,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	TerminalSessionCreateRequest request;
	request.name = std::move(name);
	request.expectedRevision = expectedRevision;
	return CreateSession(request);
}

TerminalTopologyResult TerminalCollectionModel::CreateTerminalWindow(const TerminalWindowCreateRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	const auto sessionIt = m_sessions.find(request.sessionId);
	if (sessionIt == m_sessions.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	if (m_windows.size() >= m_limits.maximumWindows || m_panes.size() >= m_limits.maximumPanes || !CanAdvanceRevision()) {
		return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	}
	if (!request.name.empty() && !IsValidName(request.name, m_limits.maximumNameBytes)) {
		return BaseResult(ETerminalTopologyResultCode::InvalidRequest);
	}
	if (!request.name.empty() && !IsWindowNameAvailable(sessionIt->second, request.name)) {
		return BaseResult(ETerminalTopologyResultCode::NameConflict);
	}
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	TerminalInstanceId instanceId;
	if (!AllocateWindowId(windowId) || !AllocatePaneId(paneId) || !AllocateInstanceId(instanceId)) {
		return BaseResult(ETerminalTopologyResultCode::IdentityExhausted);
	}
	const auto windowName = request.name.empty() ? std::to_string(windowId.value) : request.name;
	if (!IsValidName(windowName, m_limits.maximumNameBytes)
		|| !IsWindowNameAvailable(sessionIt->second, windowName)) {
		return BaseResult(ETerminalTopologyResultCode::NameConflict);
	}
	sessionIt->second.order.push_back(windowId);
	sessionIt->second.activeWindow = windowId;
	m_windows.emplace(windowId, WindowState{ windowId, windowName, TerminalLayoutNode::Leaf(paneId, instanceId), paneId });
	m_panes.emplace(paneId, PaneState{ paneId, instanceId, request.sessionId, windowId });
	m_activeSession = request.sessionId;
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = request.sessionId;
	result.windowId = windowId;
	result.paneId = paneId;
	result.instanceId = instanceId;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::CreateTerminalWindow(TerminalSessionId sessionId, std::string name,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	TerminalWindowCreateRequest request;
	request.sessionId = sessionId;
	request.name = std::move(name);
	request.expectedRevision = expectedRevision;
	return CreateTerminalWindow(request);
}

TerminalTopologyResult TerminalCollectionModel::SplitPane(const TerminalPaneSplitRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	if (!IsValidOrientation(request.orientation)) return BaseResult(ETerminalTopologyResultCode::InvalidRequest);
	const auto sessionIt = m_sessions.find(request.sessionId);
	const auto windowIt = m_windows.find(request.windowId);
	const auto paneIt = m_panes.find(request.paneId);
	if (sessionIt == m_sessions.end() || windowIt == m_windows.end() || paneIt == m_panes.end()
		|| paneIt->second.sessionId != request.sessionId || paneIt->second.windowId != request.windowId
		|| !Contains(sessionIt->second.order, request.windowId) || !HasPane(windowIt->second.root, request.paneId)) {
		return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	}
	if (m_panes.size() >= m_limits.maximumPanes || !CanAdvanceRevision()) {
		return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	}
	std::vector<std::size_t> path;
	if (!FindPanePath(windowIt->second.root, request.paneId, path)) {
		return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	}
	TerminalPaneId newPaneId;
	TerminalInstanceId newInstanceId;
	if (!AllocatePaneId(newPaneId) || !AllocateInstanceId(newInstanceId)) {
		return BaseResult(ETerminalTopologyResultCode::IdentityExhausted);
	}
	const auto newLeaf = TerminalLayoutNode::Leaf(newPaneId, newInstanceId);
	TerminalLayoutNode replacement = windowIt->second.root;
	if (path.empty()) {
		std::vector<TerminalLayoutNode> children;
		children.push_back(replacement);
		children.push_back(newLeaf);
		replacement = TerminalLayoutNode::Split(request.orientation, std::move(children), { 1, 1 });
	} else if (!SplitAtPath(replacement, path, 0, request.orientation, newLeaf)) {
		return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	}
	windowIt->second.root = std::move(replacement);
	windowIt->second.activePane = newPaneId;
	m_panes.emplace(newPaneId, PaneState{ newPaneId, newInstanceId, request.sessionId, request.windowId });
	sessionIt->second.activeWindow = request.windowId;
	m_activeSession = request.sessionId;
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = request.sessionId;
	result.windowId = request.windowId;
	result.paneId = newPaneId;
	result.instanceId = newInstanceId;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::SplitPane(TerminalSessionId sessionId, TerminalWindowId windowId,
	TerminalPaneId paneId, const TerminalPaneOrientation orientation,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	TerminalPaneSplitRequest request;
	request.sessionId = sessionId;
	request.windowId = windowId;
	request.paneId = paneId;
	request.orientation = orientation;
	request.expectedRevision = expectedRevision;
	return SplitPane(request);
}

TerminalTopologyResult TerminalCollectionModel::SelectSession(const TerminalSessionSelectRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	if (m_sessions.find(request.sessionId) == m_sessions.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.sessionId = request.sessionId;
	if (!m_activeSession || *m_activeSession != request.sessionId) {
		if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
		m_activeSession = request.sessionId;
		AdvanceRevision();
		result.revision = m_revision;
		result.changed = true;
	}
	return result;
}

TerminalTopologyResult TerminalCollectionModel::SelectSession(TerminalSessionId sessionId,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	return SelectSession({ sessionId, expectedRevision });
}

TerminalTopologyResult TerminalCollectionModel::SelectWindow(const TerminalWindowSelectRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	const auto sessionIt = m_sessions.find(request.sessionId);
	const auto windowIt = m_windows.find(request.windowId);
	if (sessionIt == m_sessions.end() || windowIt == m_windows.end() || !Contains(sessionIt->second.order, request.windowId)) {
		return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	}
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.sessionId = request.sessionId;
	result.windowId = request.windowId;
	if (!m_activeSession || *m_activeSession != request.sessionId || sessionIt->second.activeWindow != request.windowId) {
		if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
		m_activeSession = request.sessionId;
		sessionIt->second.activeWindow = request.windowId;
		AdvanceRevision();
		result.revision = m_revision;
		result.changed = true;
	}
	return result;
}

TerminalTopologyResult TerminalCollectionModel::SelectWindow(TerminalSessionId sessionId, TerminalWindowId windowId,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	return SelectWindow({ sessionId, windowId, expectedRevision });
}

TerminalTopologyResult TerminalCollectionModel::SelectPane(const TerminalPaneSelectRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	const auto paneIt = m_panes.find(request.paneId);
	if (paneIt == m_panes.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	if (request.sessionId && *request.sessionId != paneIt->second.sessionId) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	if (request.windowId && *request.windowId != paneIt->second.windowId) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	const auto sessionIt = m_sessions.find(paneIt->second.sessionId);
	const auto windowIt = m_windows.find(paneIt->second.windowId);
	if (sessionIt == m_sessions.end() || windowIt == m_windows.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.sessionId = paneIt->second.sessionId;
	result.windowId = paneIt->second.windowId;
	result.paneId = request.paneId;
	if (!m_activeSession || *m_activeSession != paneIt->second.sessionId
		|| sessionIt->second.activeWindow != paneIt->second.windowId || windowIt->second.activePane != request.paneId) {
		if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
		m_activeSession = paneIt->second.sessionId;
		sessionIt->second.activeWindow = paneIt->second.windowId;
		windowIt->second.activePane = request.paneId;
		AdvanceRevision();
		result.revision = m_revision;
		result.changed = true;
	}
	return result;
}

TerminalTopologyResult TerminalCollectionModel::SelectPane(TerminalPaneId paneId,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	TerminalPaneSelectRequest request;
	request.paneId = paneId;
	request.expectedRevision = expectedRevision;
	return SelectPane(request);
}

void TerminalCollectionModel::RemoveWindowRecords(const TerminalSessionId sessionId, const TerminalWindowId windowId)
{
	const auto sessionIt = m_sessions.find(sessionId);
	const auto windowIt = m_windows.find(windowId);
	if (sessionIt == m_sessions.end() || windowIt == m_windows.end()) return;
	const auto leaves = Leaves(windowIt->second.root);
	for (const auto paneId : leaves) m_panes.erase(paneId);
	const auto removedIndex = static_cast<std::size_t>(std::distance(sessionIt->second.order.begin(),
		std::find(sessionIt->second.order.begin(), sessionIt->second.order.end(), windowId)));
	sessionIt->second.order.erase(sessionIt->second.order.begin() + static_cast<std::ptrdiff_t>(removedIndex));
	m_windows.erase(windowIt);
	if (sessionIt->second.order.empty()) {
		RemoveSessionRecords(sessionId);
		return;
	}
	if (sessionIt->second.activeWindow == windowId) {
		const auto fallbackIndex = std::min(removedIndex, sessionIt->second.order.size() - 1);
		sessionIt->second.activeWindow = sessionIt->second.order[fallbackIndex];
	}
}

void TerminalCollectionModel::RemoveSessionRecords(const TerminalSessionId sessionId)
{
	const auto sessionIt = m_sessions.find(sessionId);
	if (sessionIt == m_sessions.end()) return;
	for (const auto windowId : sessionIt->second.order) {
		const auto windowIt = m_windows.find(windowId);
		if (windowIt == m_windows.end()) continue;
		for (const auto paneId : Leaves(windowIt->second.root)) m_panes.erase(paneId);
		m_windows.erase(windowIt);
	}
	m_sessions.erase(sessionIt);
	const auto removedIt = std::find(m_order.begin(), m_order.end(), sessionId);
	if (removedIt == m_order.end()) return;
	const auto removedIndex = static_cast<std::size_t>(std::distance(m_order.begin(), removedIt));
	m_order.erase(removedIt);
	if (m_activeSession && *m_activeSession == sessionId) {
		if (m_order.empty()) m_activeSession.reset();
		else m_activeSession = m_order[std::min(removedIndex, m_order.size() - 1)];
	}
}

TerminalTopologyResult TerminalCollectionModel::ClosePane(const TerminalPaneCloseRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	const auto paneIt = m_panes.find(request.paneId);
	const auto sessionIt = m_sessions.find(request.sessionId);
	const auto windowIt = m_windows.find(request.windowId);
	if (paneIt == m_panes.end() || sessionIt == m_sessions.end() || windowIt == m_windows.end()
		|| paneIt->second.sessionId != request.sessionId || paneIt->second.windowId != request.windowId
		|| !Contains(sessionIt->second.order, request.windowId) || !HasPane(windowIt->second.root, request.paneId)) {
		return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	}
	if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	const auto leaves = Leaves(windowIt->second.root);
	const auto targetIt = std::find(leaves.begin(), leaves.end(), request.paneId);
	if (targetIt == leaves.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	const auto targetIndex = static_cast<std::size_t>(std::distance(leaves.begin(), targetIt));
	if (leaves.size() == 1) {
		RemoveWindowRecords(request.sessionId, request.windowId);
	} else {
		std::vector<std::size_t> path;
		if (!FindPanePath(windowIt->second.root, request.paneId, path) || path.empty()
			|| !RemoveAtPath(windowIt->second.root, path, 0)) {
			return BaseResult(ETerminalTopologyResultCode::TargetMissing);
		}
		m_panes.erase(request.paneId);
		const auto remaining = Leaves(windowIt->second.root);
		if (windowIt->second.activePane == request.paneId) {
			windowIt->second.activePane = remaining[std::min(targetIndex, remaining.size() - 1)];
		}
		sessionIt->second.activeWindow = request.windowId;
		m_activeSession = request.sessionId;
	}
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = request.sessionId;
	result.windowId = request.windowId;
	result.paneId = request.paneId;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::ClosePane(TerminalSessionId sessionId, TerminalWindowId windowId,
	TerminalPaneId paneId, const std::optional<TerminalTopologyRevision> expectedRevision)
{
	return ClosePane({ sessionId, windowId, paneId, expectedRevision });
}

TerminalTopologyResult TerminalCollectionModel::CloseWindow(const TerminalWindowCloseRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	const auto sessionIt = m_sessions.find(request.sessionId);
	if (sessionIt == m_sessions.end() || m_windows.find(request.windowId) == m_windows.end()
		|| !Contains(sessionIt->second.order, request.windowId)) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	RemoveWindowRecords(request.sessionId, request.windowId);
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = request.sessionId;
	result.windowId = request.windowId;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::CloseWindow(TerminalSessionId sessionId, TerminalWindowId windowId,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	return CloseWindow({ sessionId, windowId, expectedRevision });
}

TerminalTopologyResult TerminalCollectionModel::CloseSession(const TerminalSessionCloseRequest& request)
{
	auto result = CheckMutation(request.expectedRevision);
	if (!result.Succeeded()) return result;
	if (m_sessions.find(request.sessionId) == m_sessions.end()) return BaseResult(ETerminalTopologyResultCode::TargetMissing);
	if (!CanAdvanceRevision()) return BaseResult(ETerminalTopologyResultCode::ResourceExhausted);
	RemoveSessionRecords(request.sessionId);
	AdvanceRevision();
	result = BaseResult(ETerminalTopologyResultCode::Succeeded);
	result.changed = true;
	result.sessionId = request.sessionId;
	return result;
}

TerminalTopologyResult TerminalCollectionModel::CloseSession(TerminalSessionId sessionId,
	const std::optional<TerminalTopologyRevision> expectedRevision)
{
	return CloseSession({ sessionId, expectedRevision });
}

TerminalWindowSnapshot TerminalCollectionModel::MakeWindowSnapshot(const TerminalSessionId sessionId,
	const WindowState& window) const
{
	TerminalWindowSnapshot snapshot;
	snapshot.id = window.id;
	snapshot.name = window.name;
	snapshot.root = window.root;
	snapshot.activePane = window.activePane;
	for (const auto paneId : Leaves(window.root)) {
		const auto paneIt = m_panes.find(paneId);
		if (paneIt != m_panes.end()) {
			snapshot.panes.push_back({ paneIt->second.id, paneIt->second.instanceId, sessionId, window.id });
		}
	}
	return snapshot;
}

TerminalSessionSnapshot TerminalCollectionModel::MakeSessionSnapshot(const SessionState& session) const
{
	TerminalSessionSnapshot snapshot;
	snapshot.id = session.id;
	snapshot.name = session.name;
	snapshot.order = session.order;
	snapshot.activeWindow = session.activeWindow;
	for (const auto windowId : session.order) {
		const auto windowIt = m_windows.find(windowId);
		if (windowIt != m_windows.end()) snapshot.windows.push_back(MakeWindowSnapshot(session.id, windowIt->second));
	}
	return snapshot;
}

TerminalCollectionSnapshot TerminalCollectionModel::Snapshot() const
{
	TerminalCollectionSnapshot snapshot;
	snapshot.revision = m_revision;
	snapshot.activeSession = m_activeSession;
	snapshot.order = m_order;
	for (const auto sessionId : m_order) {
		const auto sessionIt = m_sessions.find(sessionId);
		if (sessionIt != m_sessions.end()) snapshot.sessions.push_back(MakeSessionSnapshot(sessionIt->second));
	}
	return snapshot;
}

std::optional<TerminalSessionSnapshot> TerminalCollectionModel::FindSession(const TerminalSessionId id) const
{
	const auto it = m_sessions.find(id);
	if (it == m_sessions.end()) return std::nullopt;
	return MakeSessionSnapshot(it->second);
}

std::optional<TerminalWindowSnapshot> TerminalCollectionModel::FindWindow(const TerminalSessionId sessionId,
	const TerminalWindowId id) const
{
	const auto sessionIt = m_sessions.find(sessionId);
	const auto windowIt = m_windows.find(id);
	if (sessionIt == m_sessions.end() || windowIt == m_windows.end() || !Contains(sessionIt->second.order, id)) {
		return std::nullopt;
	}
	return MakeWindowSnapshot(sessionId, windowIt->second);
}

std::optional<TerminalPaneSnapshot> TerminalCollectionModel::FindPane(const TerminalPaneId id) const
{
	const auto it = m_panes.find(id);
	if (it == m_panes.end()) return std::nullopt;
	return TerminalPaneSnapshot{ it->second.id, it->second.instanceId, it->second.sessionId, it->second.windowId };
}

bool TerminalCollectionModel::ValidateInvariants() const noexcept
{
	if (m_order.size() != m_sessions.size() || m_sessions.size() > m_limits.maximumSessions
		|| m_windows.size() > m_limits.maximumWindows || m_panes.size() > m_limits.maximumPanes) return false;
	if (m_activeSession && !Contains(m_order, *m_activeSession)) return false;
	std::vector<TerminalSessionId> seenSessions;
	std::vector<TerminalWindowId> seenWindows;
	std::vector<TerminalPaneId> seenPanes;
	for (const auto sessionId : m_order) {
		const auto sessionIt = m_sessions.find(sessionId);
		if (sessionIt == m_sessions.end() || !sessionId.IsValid()
			|| Contains(seenSessions, sessionId) || !IsValidName(sessionIt->second.name, m_limits.maximumNameBytes)) return false;
		seenSessions.push_back(sessionId);
		for (std::size_t i = 0; i < sessionIt->second.order.size(); ++i) {
			const auto windowId = sessionIt->second.order[i];
			if (!windowId.IsValid() || Contains(seenWindows, windowId)
				|| m_windows.find(windowId) == m_windows.end()) return false;
			seenWindows.push_back(windowId);
			for (std::size_t j = 0; j < i; ++j) if (sessionIt->second.order[j] == windowId) return false;
		}
		if (sessionIt->second.order.empty()) {
			if (sessionIt->second.activeWindow.IsValid()) return false;
		} else if (!Contains(sessionIt->second.order, sessionIt->second.activeWindow)) return false;
		for (const auto windowId : sessionIt->second.order) {
			const auto windowIt = m_windows.find(windowId);
			if (windowIt == m_windows.end() || !IsValidName(windowIt->second.name, m_limits.maximumNameBytes)) return false;
			const auto leaves = Leaves(windowIt->second.root);
			if (leaves.empty() || !Contains(leaves, windowIt->second.activePane)) return false;
			for (const auto paneId : leaves) {
				if (!paneId.IsValid() || Contains(seenPanes, paneId)) return false;
				const auto paneIt = m_panes.find(paneId);
				if (paneIt == m_panes.end() || paneIt->second.sessionId != sessionId || paneIt->second.windowId != windowId
					|| paneIt->second.instanceId.value == 0) return false;
				seenPanes.push_back(paneId);
			}
		}
	}
	if (seenWindows.size() != m_windows.size() || seenPanes.size() != m_panes.size()) return false;
	for (const auto& [id, window] : m_windows) {
		if (!id.IsValid() || window.root.IsLeaf() && (!window.root.pane.paneId.IsValid() || window.root.pane.instanceId.value == 0)) return false;
		const auto validateNode = [](const auto& self, const TerminalLayoutNode& node) -> bool {
			if (node.IsLeaf()) return node.children.empty() && node.weights.empty() && node.pane.paneId.IsValid()
				&& node.pane.instanceId.IsValid();
			if (!TerminalCollectionModel::IsValidOrientation(node.orientation) || node.children.size() < 2
				|| node.weights.size() != node.children.size()) return false;
			for (std::size_t i = 0; i < node.children.size(); ++i) {
				if (node.weights[i] == 0 || !self(self, node.children[i])) return false;
			}
			return true;
		};
		if (!validateNode(validateNode, window.root)) return false;
	}
	return true;
}

} // namespace terminal::runtime::topology
