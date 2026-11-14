/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionRestore.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <unordered_set>
#include <variant>

namespace workbench::projects {

namespace {

using platform::serialization::JsoncValue;

constexpr std::size_t kMaximumRestorePayloadBytes = 64u * 1024u;
constexpr std::size_t kMaximumRestoreFolderCharacters = 1024;

constexpr std::array<std::wstring_view, 4> kProviderKeys{ L"claude-code", L"codex", L"devin", L"pi" };
constexpr std::array<EAgentSessionProvider, 4> kProviders{
	EAgentSessionProvider::ClaudeCode, EAgentSessionProvider::Codex,
	EAgentSessionProvider::Devin, EAgentSessionProvider::Pi };

//! A valid folder string: non-empty, bounded, well-formed UTF-16, no control
//! characters (they cannot appear in a real path and would complicate the
//! stored record for no benefit).
bool IsValidRestoreFolder(const std::wstring_view folder) noexcept
{
	if (folder.empty() || folder.size() > kMaximumRestoreFolderCharacters) return false;
	for (std::size_t index = 0; index < folder.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(folder[index]);
		if (unit < 0x20U) return false;
		if (unit >= 0xd800U && unit <= 0xdbffU) {
			if (++index == folder.size()) return false;
			const auto low = static_cast<std::uint32_t>(folder[index]);
			if (low < 0xdc00U || low > 0xdfffU) return false;
		} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
			return false;
		}
	}
	return true;
}

bool IsValidSessionList(const std::vector<std::wstring>& sessions) noexcept
{
	if (sessions.size() > kMaximumAgentSessionRestoreSessions) return false;
	for (std::size_t index = 0; index < sessions.size(); ++index) {
		if (!IsValidAgentSessionIdentity(sessions[index])) return false;
		for (std::size_t earlier = 0; earlier < index; ++earlier) {
			if (sessions[earlier] == sessions[index]) return false;
		}
	}
	return true;
}

//! A stored layout (#415 Phase 3) is a set of groups whose recursive trees
//! reference the entry's leaves exactly once each. `sessions` must equal the
//! session-leaf identities in the trees' depth-first leaf order across the
//! groups in panel order. Every node of a group is reachable from its root:
//! the recorder writes compact trees, so a dead node only appears in a
//! malformed record.
bool IsValidRestoreLayout(const AgentSessionRestoreLayout& layout,
	const std::vector<std::wstring>& sessions)
{
	if (layout.leaves.empty() || layout.groups.empty()
		|| layout.groups.size() > layout.leaves.size()
		|| layout.leaves.size() > kMaximumAgentSessionRestoreSessions
			+ kMaximumAgentSessionRestoreShellLeaves) return false;
	if (layout.active && *layout.active >= layout.leaves.size()) return false;
	std::size_t shells = 0;
	for (const auto& leaf : layout.leaves) {
		if (leaf.session.has_value() == leaf.shell.has_value()) return false;
		if (leaf.session && !IsValidAgentSessionIdentity(*leaf.session)) return false;
		if (leaf.shell) {
			if (!IsValidRestoreFolder(*leaf.shell)) return false;
			if (++shells > kMaximumAgentSessionRestoreShellLeaves) return false;
		}
	}
	std::vector<bool> referenced(layout.leaves.size());
	std::vector<std::wstring> leafOrder;
	for (const auto& group : layout.groups) {
		if (group.nodes.empty() || group.nodes.size() > kMaximumAgentSessionRestoreLayoutNodes
			|| group.root >= group.nodes.size()) return false;
		std::unordered_set<std::size_t> visited;
		const std::function<bool(std::size_t)> walk = [&](const std::size_t nodeIndex) -> bool {
			if (nodeIndex >= group.nodes.size() || !visited.insert(nodeIndex).second) return false;
			const auto& node = group.nodes[nodeIndex];
			if (node.leaf) {
				// A leaf node carries no split data and names a live leaf.
				if (!node.children.empty() || !node.weights.empty()
					|| *node.leaf >= layout.leaves.size()) return false;
				if (referenced[*node.leaf]) return false;
				referenced[*node.leaf] = true;
				if (const auto& leaf = layout.leaves[*node.leaf]; leaf.session) {
					leafOrder.push_back(*leaf.session);
				}
				return true;
			}
			if (node.children.size() < 2 || node.weights.size() != node.children.size()) return false;
			for (const auto weight : node.weights) {
				if (weight <= 0) return false;
			}
			for (const auto child : node.children) {
				if (child == nodeIndex || !walk(child)) return false;
			}
			return true;
		};
		if (!walk(group.root) || visited.size() != group.nodes.size()) return false;
	}
	for (const auto used : referenced) {
		if (!used) return false;
	}
	return leafOrder == sessions;
}

bool IsValidState(const AgentSessionRestoreState& state)
{
	if (state.entries.size() > kMaximumAgentSessionRestoreProjects) return false;
	for (std::size_t index = 0; index < state.entries.size(); ++index) {
		const auto& entry = state.entries[index];
		// An empty list means "record nothing" and is never stored: SetEntry
		// removes the row instead, so no valid state carries one.
		if (!IsValidRestoreFolder(entry.folder) || entry.sessions.empty()
			|| !IsValidSessionList(entry.sessions)) return false;
		if (entry.layout && !IsValidRestoreLayout(*entry.layout, entry.sessions)) return false;
		for (std::size_t earlier = 0; earlier < index; ++earlier) {
			if (IsSameAgentSessionFolder(state.entries[earlier].folder, entry.folder)) return false;
		}
	}
	return true;
}

bool AppendUtf8(std::string& target, const std::uint32_t codePoint)
{
	if (codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return false;
	if (codePoint <= 0x7fU) target.push_back(static_cast<char>(codePoint));
	else if (codePoint <= 0x7ffU) {
		target.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else if (codePoint <= 0xffffU) {
		target.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else {
		target.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	}
	return true;
}

//! Folder strings can carry any UTF-16, so they need real JSON string quoting;
//! identities are ASCII by construction (IsValidAgentSessionIdentity) and are
//! appended bare.
bool AppendJsonString(std::string& out, const std::wstring_view value)
{
	out.push_back('"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit == L'"' || unit == L'\\') {
			out.push_back('\\');
			out.push_back(static_cast<char>(unit));
		} else if (unit == L'\b') out += "\\b";
		else if (unit == L'\f') out += "\\f";
		else if (unit == L'\n') out += "\\n";
		else if (unit == L'\r') out += "\\r";
		else if (unit == L'\t') out += "\\t";
		else if (unit <= 0x1fU) {
			static constexpr char hex[] = "0123456789abcdef";
			out += "\\u00";
			out.push_back(hex[(unit >> 4U) & 0xfU]);
			out.push_back(hex[unit & 0xfU]);
		} else if (unit >= 0xd800U && unit <= 0xdbffU) {
			const auto low = static_cast<std::uint32_t>(value[++index]);
			if (!AppendUtf8(out, 0x10000U + ((unit - 0xd800U) << 10U) + (low - 0xdc00U))) return false;
		} else if (!AppendUtf8(out, unit)) return false;
	}
	out.push_back('"');
	return true;
}

void AppendIdentityList(std::string& out, const std::vector<std::wstring>& sessions)
{
	out.push_back('[');
	for (std::size_t index = 0; index < sessions.size(); ++index) {
		if (index != 0) out.push_back(',');
		out.push_back('"');
		for (const wchar_t character : sessions[index]) out.push_back(static_cast<char>(character));
		out.push_back('"');
	}
	out.push_back(']');
}

void AppendIndexList(std::string& out, const std::vector<std::size_t>& indices)
{
	out.push_back('[');
	for (std::size_t index = 0; index < indices.size(); ++index) {
		if (index != 0) out.push_back(',');
		out += std::to_string(indices[index]);
	}
	out.push_back(']');
}

void AppendWeightList(std::string& out, const std::vector<int>& weights)
{
	out.push_back('[');
	for (std::size_t index = 0; index < weights.size(); ++index) {
		if (index != 0) out.push_back(',');
		out += std::to_string(weights[index]);
	}
	out.push_back(']');
}

bool AppendLayout(std::string& out, const AgentSessionRestoreLayout& layout)
{
	out += ",\"layout\":{\"leaves\":[";
	for (std::size_t index = 0; index < layout.leaves.size(); ++index) {
		const auto& leaf = layout.leaves[index];
		if (index != 0) out.push_back(',');
		if (leaf.session) {
			out += "{\"session\":";
		} else if (leaf.shell) {
			out += "{\"shell\":";
		} else {
			return false;
		}
		if (!AppendJsonString(out, leaf.session ? *leaf.session : *leaf.shell)) return false;
		out.push_back('}');
	}
	out += "],\"groups\":[";
	for (std::size_t groupIndex = 0; groupIndex < layout.groups.size(); ++groupIndex) {
		const auto& group = layout.groups[groupIndex];
		if (groupIndex != 0) out.push_back(',');
		out += "{\"nodes\":[";
		for (std::size_t nodeIndex = 0; nodeIndex < group.nodes.size(); ++nodeIndex) {
			const auto& node = group.nodes[nodeIndex];
			if (nodeIndex != 0) out.push_back(',');
			if (node.leaf) {
				out += "{\"leaf\":" + std::to_string(*node.leaf) + "}";
				continue;
			}
			out += "{\"split\":\"";
			out += node.orientation == AgentSessionRestoreOrientation::Vertical
				? "vertical" : "horizontal";
			out += "\",\"children\":";
			AppendIndexList(out, node.children);
			out += ",\"weights\":";
			AppendWeightList(out, node.weights);
			out.push_back('}');
		}
		out += "],\"root\":" + std::to_string(group.root) + "}";
	}
	out.push_back(']');
	if (layout.active) out += ",\"active\":" + std::to_string(*layout.active);
	out.push_back('}');
	return true;
}

std::optional<std::wstring> StringMember(const JsoncValue::Object& object,
	const std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	if (const auto* value = std::get_if<std::wstring>(&found->second.Value())) return *value;
	return std::nullopt;
}

//! A non-negative integer member; absent and present-but-wrong-type both
//! return nullopt. Callers distinguish "absent" through find() when needed.
std::optional<std::size_t> IndexMember(const JsoncValue::Object& object,
	const std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	const auto* value = std::get_if<std::int64_t>(&found->second.Value());
	if (value == nullptr || *value < 0) return std::nullopt;
	return static_cast<std::size_t>(*value);
}

//! Parses an optional array of non-negative integers; nullopt when the key
//! is absent or any element is not a non-negative integer.
std::optional<std::vector<std::size_t>> IndexArrayMember(const JsoncValue::Object& object,
	const std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&found->second.Value());
	if (array == nullptr) return std::nullopt;
	std::vector<std::size_t> indices;
	indices.reserve(array->size());
	for (const auto& item : *array) {
		const auto* value = std::get_if<std::int64_t>(&item.Value());
		if (value == nullptr || *value < 0) return std::nullopt;
		indices.push_back(static_cast<std::size_t>(*value));
	}
	return indices;
}

//! Parses an optional array of integers; nullopt when the key is absent or
//! any element is not an integer.
std::optional<std::vector<int>> IntArrayMember(const JsoncValue::Object& object,
	const std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&found->second.Value());
	if (array == nullptr) return std::nullopt;
	std::vector<int> weights;
	weights.reserve(array->size());
	for (const auto& item : *array) {
		const auto* value = std::get_if<std::int64_t>(&item.Value());
		if (value == nullptr || *value < std::numeric_limits<int>::min()
			|| *value > std::numeric_limits<int>::max()) return std::nullopt;
		weights.push_back(static_cast<int>(*value));
	}
	return weights;
}

//! Parses one layout node: `{"leaf":N}` or `{"split":"horizontal"|"vertical",
//! "children":[..],"weights":[..]}`. The shape is enforced here; the tree
//! semantics (indices in range, acyclic, positive weights, leaf coverage) are
//! IsValidRestoreLayout's.
std::optional<AgentSessionRestoreLayoutNode> DecodeLayoutNode(const JsoncValue& value)
{
	const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
	if (object == nullptr) return std::nullopt;
	const auto leaf = IndexMember(*object, L"leaf");
	if (leaf) {
		if (object->contains(L"split") || object->contains(L"children")
			|| object->contains(L"weights")) return std::nullopt;
		return AgentSessionRestoreLayoutNode{ .leaf = leaf };
	}
	const auto split = StringMember(*object, L"split");
	const auto children = IndexArrayMember(*object, L"children");
	const auto weights = IntArrayMember(*object, L"weights");
	if (!split || !children || !weights) return std::nullopt;
	AgentSessionRestoreOrientation orientation;
	if (*split == L"horizontal") orientation = AgentSessionRestoreOrientation::Horizontal;
	else if (*split == L"vertical") orientation = AgentSessionRestoreOrientation::Vertical;
	else return std::nullopt;
	return AgentSessionRestoreLayoutNode{
		.leaf = std::nullopt,
		.orientation = orientation,
		.children = std::move(*children),
		.weights = std::move(*weights),
	};
}

//! Parses the version-2 `layout` member: `leaves` + `groups` required,
//! `active` optional.
std::optional<AgentSessionRestoreLayout> DecodeRestoreLayout(const JsoncValue::Object& object)
{
	const auto leavesValue = object.find(L"leaves");
	const auto groupsValue = object.find(L"groups");
	if (leavesValue == object.end() || groupsValue == object.end()) return std::nullopt;
	const auto* leavesArray = std::get_if<JsoncValue::Array>(&leavesValue->second.Value());
	const auto* groupsArray = std::get_if<JsoncValue::Array>(&groupsValue->second.Value());
	if (leavesArray == nullptr || groupsArray == nullptr
		|| leavesArray->size() > kMaximumAgentSessionRestoreSessions
			+ kMaximumAgentSessionRestoreShellLeaves) return std::nullopt;
	AgentSessionRestoreLayout layout;
	layout.leaves.reserve(leavesArray->size());
	layout.groups.reserve(groupsArray->size());
	for (const auto& item : *leavesArray) {
		const auto* leafObject = std::get_if<JsoncValue::Object>(&item.Value());
		if (leafObject == nullptr) return std::nullopt;
		// Exactly one of the two members is present, and it is a string: a
		// wrong-typed member beside a valid one is malformed, not absent.
		const bool hasSession = leafObject->find(L"session") != leafObject->end();
		const bool hasShell = leafObject->find(L"shell") != leafObject->end();
		const auto session = StringMember(*leafObject, L"session");
		const auto shell = StringMember(*leafObject, L"shell");
		if (hasSession == hasShell || session.has_value() != hasSession
			|| shell.has_value() != hasShell) return std::nullopt;
		AgentSessionRestoreLeaf leaf;
		if (session) leaf.session = *session;
		else leaf.shell = *shell;
		layout.leaves.push_back(std::move(leaf));
	}
	for (const auto& item : *groupsArray) {
		const auto* groupObject = std::get_if<JsoncValue::Object>(&item.Value());
		if (groupObject == nullptr) return std::nullopt;
		const auto nodesValue = groupObject->find(L"nodes");
		const auto root = IndexMember(*groupObject, L"root");
		if (nodesValue == groupObject->end() || !root) return std::nullopt;
		const auto* nodesArray = std::get_if<JsoncValue::Array>(&nodesValue->second.Value());
		if (nodesArray == nullptr
			|| nodesArray->size() > kMaximumAgentSessionRestoreLayoutNodes) return std::nullopt;
		AgentSessionRestoreLayoutGroup group{ .root = *root };
		group.nodes.reserve(nodesArray->size());
		for (const auto& node : *nodesArray) {
			auto parsed = DecodeLayoutNode(node);
			if (!parsed) return std::nullopt;
			group.nodes.push_back(std::move(*parsed));
		}
		layout.groups.push_back(std::move(group));
	}
	const auto active = object.find(L"active");
	if (active != object.end()) {
		const auto* value = std::get_if<std::int64_t>(&active->second.Value());
		if (value == nullptr || *value < 0) return std::nullopt;
		layout.active = static_cast<std::size_t>(*value);
	}
	return layout;
}

} // namespace

std::optional<EAgentSessionProvider> AgentSessionIdentityProvider(
	const std::wstring_view identity) noexcept
{
	if (!IsValidAgentSessionIdentity(identity)) return std::nullopt;
	const auto colon = identity.find(L':');
	if (colon == std::wstring_view::npos) return std::nullopt;
	const auto key = identity.substr(0, colon);
	for (std::size_t index = 0; index < kProviderKeys.size(); ++index) {
		if (kProviderKeys[index] == key) return kProviders[index];
	}
	return std::nullopt;
}

std::vector<RecordedAgentSessionClaim> RecordLiveAgentSessionsByTab(
	const std::span<const LiveAgentSessionSource> agents,
	const std::span<const AgentSessionRecord> sessions,
	const std::size_t maximum)
{
	if (maximum == 0) return {};
	std::vector<RecordedAgentSessionClaim> recorded;
	const auto claim = [&recorded, maximum](const std::uint64_t tabId,
		const std::wstring_view identity) {
		if (recorded.size() == maximum) return false;
		for (const auto& claim : recorded) {
			if (claim.identity == identity) return true;
		}
		recorded.push_back({ .tabId = tabId, .identity = std::wstring(identity) });
		return true;
	};
	// Sessions Sakura resumed are known, not inferred; they claim first so a
	// hand-started agent of the same provider cannot take their identity.
	for (const auto& agent : agents) {
		if (!agent.knownSessionIdentity || !IsValidAgentSessionIdentity(*agent.knownSessionIdentity)) {
			continue;
		}
		const auto provider = AgentSessionIdentityProvider(*agent.knownSessionIdentity);
		if (provider && AgentSessionProviderDisplayName(*provider) == agent.name) {
			(void)claim(agent.tabId, *agent.knownSessionIdentity);
		}
	}
	for (const auto provider : kProviders) {
		const auto name = AgentSessionProviderDisplayName(provider);
		std::vector<const LiveAgentSessionSource*> inferable;
		for (const auto& agent : agents) {
			if (agent.name != name || agent.startedUnixSeconds <= 0) continue;
			if (agent.knownSessionIdentity && IsValidAgentSessionIdentity(*agent.knownSessionIdentity)
				&& AgentSessionIdentityProvider(*agent.knownSessionIdentity) == provider) {
				continue;	// already recorded above
			}
			inferable.push_back(&agent);
		}
		if (inferable.empty()) continue;
		// The latest agent is the hardest to cover, so it claims the newest
		// session first; a session too old for it may still cover an earlier
		// one (the same arbitration CountAgentsAwaitingSessionHistory uses).
		std::ranges::sort(inferable, [](const auto* left, const auto* right) {
			return left->startedUnixSeconds > right->startedUnixSeconds;
		});
		std::vector<const AgentSessionRecord*> candidates;
		for (const auto& session : sessions) {
			if (session.provider == provider && session.lastActivityUnixSeconds > 0) {
				candidates.push_back(&session);
			}
		}
		std::ranges::sort(candidates, [](const auto* left, const auto* right) {
			return left->lastActivityUnixSeconds > right->lastActivityUnixSeconds;
		});
		std::size_t next = 0;
		for (const auto* agent : inferable) {
			if (next == candidates.size()) break;
			// The newest unclaimed session is too old for this agent only; an
			// earlier-started agent may still claim it, so `next` advances on
			// a successful claim alone.
			if (candidates[next]->lastActivityUnixSeconds >= agent->startedUnixSeconds) {
				(void)claim(agent->tabId, AgentSessionIdentity(*candidates[next]));
				++next;
			}
		}
	}
	return recorded;
}

const AgentSessionRestoreEntry* FindAgentSessionRestoreEntry(
	const AgentSessionRestoreState& state, const std::wstring_view folder) noexcept
{
	if (folder.empty()) return nullptr;
	const auto found = std::ranges::find_if(state.entries, [folder](const auto& entry) {
		return IsSameAgentSessionFolder(entry.folder, folder);
	});
	return found == state.entries.end() ? nullptr : &*found;
}

bool IsValidAgentSessionRestoreLayout(const AgentSessionRestoreLayout& layout,
	const std::vector<std::wstring>& sessions)
{
	return IsValidRestoreLayout(layout, sessions);
}

namespace {

//! The encoded record's bytes around its entries: `{"version":N,"projects":[`
//! before them and `]}` after.
std::size_t EncodedStateFrameBytes()
{
	return std::string("{\"version\":").size() + std::to_string(kAgentSessionRestoreSchemaVersion).size()
		+ std::string(",\"projects\":[").size() + std::string("]}").size();
}

bool AppendEntry(std::string& out, const AgentSessionRestoreEntry& entry)
{
	out += "{\"folder\":";
	if (!AppendJsonString(out, entry.folder)) return false;
	out += ",\"sessions\":";
	AppendIdentityList(out, entry.sessions);
	if (entry.layout && !AppendLayout(out, *entry.layout)) return false;
	out.push_back('}');
	return true;
}

//! One entry's encoded size; nullopt when it cannot be encoded.
std::optional<std::size_t> EncodedEntryBytes(const AgentSessionRestoreEntry& entry)
{
	std::string encoded;
	if (!AppendEntry(encoded, entry)) return std::nullopt;
	return encoded.size();
}

//! Keeps the record inside kMaximumRestorePayloadBytes once `state`'s last
//! entry was written: the least recently written folders go first, as the
//! folder cap already does. When the written entry alone does not fit with
//! its layout, it keeps its sessions without the layout. Returns false when
//! even that does not fit; the caller then leaves the stored record alone.
bool FitStateToPayloadLimit(AgentSessionRestoreState& state)
{
	std::vector<std::size_t> sizes;
	sizes.reserve(state.entries.size());
	std::size_t total = EncodedStateFrameBytes();
	for (const auto& entry : state.entries) {
		const auto bytes = EncodedEntryBytes(entry);
		if (!bytes) return false;
		sizes.push_back(*bytes);
		total += *bytes + (sizes.size() > 1 ? 1U : 0U);
	}
	std::size_t evicted = 0;
	while (total > kMaximumRestorePayloadBytes && state.entries.size() - evicted > 1) {
		total -= sizes[evicted] + 1U;	// the entry and the comma after it
		++evicted;
	}
	state.entries.erase(state.entries.begin(),
		state.entries.begin() + static_cast<std::ptrdiff_t>(evicted));
	if (total <= kMaximumRestorePayloadBytes) return true;
	auto& written = state.entries.back();
	if (!written.layout) return false;
	written.layout.reset();
	const auto bytes = EncodedEntryBytes(written);
	return bytes && EncodedStateFrameBytes() + *bytes <= kMaximumRestorePayloadBytes;
}

} // namespace

bool SetAgentSessionRestoreEntry(AgentSessionRestoreState& state, const std::wstring_view folder,
	std::vector<std::wstring> sessions, std::optional<AgentSessionRestoreLayout> layout)
{
	if (!IsValidRestoreFolder(folder) || !IsValidSessionList(sessions)) return false;
	if (layout && !IsValidRestoreLayout(*layout, sessions)) return false;
	const auto found = std::ranges::find_if(state.entries, [folder](const auto& entry) {
		return IsSameAgentSessionFolder(entry.folder, folder);
	});
	if (sessions.empty()) {
		if (found == state.entries.end()) return false;
		state.entries.erase(found);
		return true;
	}
	if (found != state.entries.end()
		&& found->folder == folder && found->sessions == sessions && found->layout == layout) {
		return false;
	}
	// The write is built on a copy so that a write which cannot fit the
	// payload limit leaves `state` exactly as it was.
	auto next = state;
	if (found != state.entries.end()) {
		// Rewriting an entry moves it to the most recently written position,
		// identical sessions included: a rewrite still means the folder was
		// active most recently.
		next.entries.erase(next.entries.begin() + (found - state.entries.begin()));
	}
	else if (next.entries.size() == kMaximumAgentSessionRestoreProjects) {
		next.entries.erase(next.entries.begin());
	}
	next.entries.push_back({ std::wstring(folder), std::move(sessions), std::move(layout) });
	if (!FitStateToPayloadLimit(next)) return false;
	state = std::move(next);
	return true;
}

std::optional<std::string> EncodeAgentSessionRestoreState(const AgentSessionRestoreState& state)
{
	if (!IsValidState(state)) return std::nullopt;
	std::string result = "{\"version\":" + std::to_string(kAgentSessionRestoreSchemaVersion)
		+ ",\"projects\":[";
	for (std::size_t index = 0; index < state.entries.size(); ++index) {
		if (index != 0) result.push_back(',');
		if (!AppendEntry(result, state.entries[index])) return std::nullopt;
		if (result.size() > kMaximumRestorePayloadBytes) return std::nullopt;
	}
	result += "]}";
	return result.size() <= kMaximumRestorePayloadBytes
		? std::optional<std::string>(std::move(result)) : std::nullopt;
}

std::optional<AgentSessionRestoreState> DecodeAgentSessionRestoreState(const std::string_view payload)
{
	if (payload.empty() || payload.size() > kMaximumRestorePayloadBytes) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::Parse(payload);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* root = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (root == nullptr) return std::nullopt;
	const auto version = root->find(L"version");
	const auto entries = root->find(L"projects");
	if (version == root->end() || entries == root->end()) return std::nullopt;
	const auto* versionValue = std::get_if<std::int64_t>(&version->second.Value());
	const auto* array = std::get_if<JsoncValue::Array>(&entries->second.Value());
	if (versionValue == nullptr
		|| *versionValue < kMinimumAgentSessionRestoreSchemaVersion
		|| *versionValue > kAgentSessionRestoreSchemaVersion
		|| array == nullptr || array->size() > kMaximumAgentSessionRestoreProjects) {
		return std::nullopt;
	}
	AgentSessionRestoreState state;
	state.entries.reserve(array->size());
	for (const auto& value : *array) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return std::nullopt;
		const auto folder = StringMember(*object, L"folder");
		const auto sessionsValue = object->find(L"sessions");
		if (!folder || sessionsValue == object->end()) return std::nullopt;
		const auto* sessionsArray = std::get_if<JsoncValue::Array>(&sessionsValue->second.Value());
		if (sessionsArray == nullptr
			|| sessionsArray->size() > kMaximumAgentSessionRestoreSessions) return std::nullopt;
		AgentSessionRestoreEntry entry{ .folder = std::move(*folder) };
		entry.sessions.reserve(sessionsArray->size());
		for (const auto& item : *sessionsArray) {
			const auto* identity = std::get_if<std::wstring>(&item.Value());
			if (identity == nullptr) return std::nullopt;
			entry.sessions.push_back(*identity);
		}
		// The layout member exists only at version 2; a version-1 record's
		// absent layout plans one single-leaf group per session, as before.
		if (*versionValue == 2) {
			const auto layoutValue = object->find(L"layout");
			if (layoutValue != object->end()) {
				const auto* layoutObject = std::get_if<JsoncValue::Object>(
					&layoutValue->second.Value());
				if (layoutObject == nullptr) return std::nullopt;
				auto layout = DecodeRestoreLayout(*layoutObject);
				if (!layout) return std::nullopt;
				entry.layout = std::move(*layout);
			}
		}
		state.entries.push_back(std::move(entry));
	}
	if (!IsValidState(state)) return std::nullopt;
	return state;
}

namespace {

//! The layout a version-1 record implies: each recorded session its own
//! single-leaf group, in record order. It names no active leaf; the plan
//! activates the last surviving group, as Phase 2 did.
AgentSessionRestoreLayout SingleLeafGroups(const std::vector<std::wstring>& sessions)
{
	AgentSessionRestoreLayout layout;
	layout.leaves.reserve(sessions.size());
	layout.groups.reserve(sessions.size());
	for (const auto& identity : sessions) {
		const std::size_t index = layout.leaves.size();
		layout.leaves.push_back({ .session = identity });
		layout.groups.push_back({ .nodes = { AgentSessionRestoreLayoutNode{ .leaf = index } },
			.root = 0 });
	}
	return layout;
}

} // namespace

AgentSessionRestorePlan PlanAgentSessionRestore(
	const bool enabled,
	const AgentSessionRestoreEntry& recorded,
	const std::optional<std::vector<AgentSessionRecord>>& sessions,
	const std::size_t maximum)
{
	AgentSessionRestorePlan plan;
	if (!enabled) {
		plan.status = EAgentSessionRestoreStatus::Disabled;
		return plan;
	}
	if (recorded.sessions.empty()) {
		plan.status = EAgentSessionRestoreStatus::NothingRecorded;
		return plan;
	}
	if (!sessions) {
		plan.status = EAgentSessionRestoreStatus::DiscoveryFailed;
		return plan;
	}
	const auto layout = recorded.layout ? *recorded.layout
		: SingleLeafGroups(recorded.sessions);
	// Per recorded leaf: the fresh session record it resumes, its shell
	// directory, or nothing (a session the discovery did not return, or one
	// past the cap).
	std::vector<std::optional<AgentSessionRestorePlanLeaf>> leafPlans(layout.leaves.size());
	std::size_t ordinal = 0;
	for (std::size_t index = 0; index < layout.leaves.size(); ++index) {
		const auto& leaf = layout.leaves[index];
		if (leaf.shell) {
			leafPlans[index] = AgentSessionRestorePlanLeaf{
				.shellDirectory = *leaf.shell };
			continue;
		}
		if (!leaf.session) continue;
		if (ordinal++ >= maximum) {
			++plan.dropped;
			continue;
		}
		const auto found = std::ranges::find_if(*sessions, [&leaf](const auto& session) {
			return AgentSessionIdentity(session) == *leaf.session;
		});
		if (found == sessions->end()) {
			plan.skipped.push_back(*leaf.session);
			continue;
		}
		leafPlans[index] = AgentSessionRestorePlanLeaf{ .session = *found };
	}
	// Where each surviving recorded leaf lands in its planned group, used to
	// resolve the recorded active leaf at the end.
	std::vector<std::optional<std::pair<std::size_t, std::size_t>>> placed(layout.leaves.size());
	for (const auto& group : layout.groups) {
		AgentSessionRestorePlanGroup planned;
		std::vector<std::size_t> placedLeaves;
		// Emits the subtree at nodeIndex minus dropped leaves; nullopt means
		// nothing of it survives. A one-child split collapses to that child
		// and the surviving neighbours keep their recorded weights.
		std::unordered_set<std::size_t> visited;
		const std::function<std::optional<std::size_t>(std::size_t)> emit =
			[&](const std::size_t nodeIndex) -> std::optional<std::size_t> {
			if (nodeIndex >= group.nodes.size()
				|| !visited.insert(nodeIndex).second) return std::nullopt;
			const auto& node = group.nodes[nodeIndex];
			if (node.leaf) {
				if (!node.children.empty() || *node.leaf >= leafPlans.size()
					|| !leafPlans[*node.leaf]) return std::nullopt;
				const std::size_t newLeaf = planned.leaves.size();
				planned.leaves.push_back(*leafPlans[*node.leaf]);
				placed[*node.leaf] = std::pair{ plan.groups.size(), newLeaf };
				placedLeaves.push_back(*node.leaf);
				planned.nodes.push_back({ .leaf = newLeaf });
				return planned.nodes.size() - 1;
			}
			std::vector<std::size_t> children;
			std::vector<int> weights;
			for (std::size_t childIndex = 0; childIndex < node.children.size(); ++childIndex) {
				const auto emitted = emit(node.children[childIndex]);
				if (!emitted) continue;
				children.push_back(*emitted);
				weights.push_back(childIndex < node.weights.size() && node.weights[childIndex] > 0
					? node.weights[childIndex] : 1000);
			}
			if (children.empty()) return std::nullopt;
			if (children.size() == 1) return children.front();
			planned.nodes.push_back({
				.leaf = std::nullopt,
				.orientation = node.orientation,
				.children = std::move(children),
				.weights = std::move(weights),
			});
			return planned.nodes.size() - 1;
		};
		const auto root = group.root < group.nodes.size() ? emit(group.root) : std::nullopt;
		if (!root) continue;
		// Open decision 1: a group left holding shells alone is never
		// recreated; only groups with a surviving session leaf install. Its
		// leaves then place nowhere, so their recorded positions are undone.
		const auto hasSession = std::ranges::any_of(planned.leaves,
			[](const auto& leaf) { return leaf.session.has_value(); });
		if (!hasSession) {
			for (const auto placedLeaf : placedLeaves) placed[placedLeaf].reset();
			continue;
		}
		planned.root = *root;
		plan.groups.push_back(std::move(planned));
	}
	if (layout.active && *layout.active < placed.size() && placed[*layout.active]) {
		plan.activeGroup = placed[*layout.active]->first;
		plan.activeLeaf = placed[*layout.active]->second;
	} else if (!recorded.layout && !plan.groups.empty()) {
		// A version-1 record names no active terminal. Phase 2 resumed its
		// sessions one by one, each selected in turn, so the last resumed one
		// ended up active; the single-leaf groups keep that result.
		plan.activeGroup = plan.groups.size() - 1;
		plan.activeLeaf = 0;
	} else if (!plan.groups.empty()) {
		// The recorded active leaf is gone: fall back to the first session
		// leaf of the first group.
		for (std::size_t leaf = 0; leaf < plan.groups.front().leaves.size(); ++leaf) {
			if (plan.groups.front().leaves[leaf].session) {
				plan.activeGroup = 0;
				plan.activeLeaf = leaf;
				break;
			}
		}
	}
	return plan;
}

AgentSessionRestoreOutcome DispatchAgentSessionRestore(
	const AgentSessionRestorePlan& plan,
	const std::function<AgentSessionRestoreInstallResult(const AgentSessionRestorePlan& plan)>& install)
{
	AgentSessionRestoreOutcome outcome{
		.status = plan.status,
		.skipped = plan.skipped.size(),
		.dropped = plan.dropped,
	};
	if (plan.status != EAgentSessionRestoreStatus::Ready || !install) return outcome;
	try {
		const auto result = install(plan);
		outcome.resumed = result.resumed;
		outcome.failed = result.failed;
	} catch (...) {
		// A failed install counts every planned leaf; what it half-built is
		// the install operation's own cleanup duty.
		for (const auto& group : plan.groups) outcome.failed += group.leaves.size();
	}
	return outcome;
}

} // namespace workbench::projects
