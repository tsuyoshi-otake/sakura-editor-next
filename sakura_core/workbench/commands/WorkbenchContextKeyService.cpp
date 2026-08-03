/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/commands/WorkbenchContextKeyService.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <utility>
#include <vector>

namespace workbench::commands {
namespace {

bool IsBoundedValue(const WorkbenchContextValue& value) noexcept
{
	if (const auto* stringValue = std::get_if<std::string>(&value)) {
		return stringValue->size() <= kMaxWorkbenchContextValueLength;
	}
	return true;
}

bool IsTruthy(const std::optional<WorkbenchContextValue>& value) noexcept
{
	if (!value) {
		return false;
	}
	if (const auto* boolValue = std::get_if<bool>(&*value)) {
		return *boolValue;
	}
	if (const auto* integerValue = std::get_if<std::int64_t>(&*value)) {
		return *integerValue != 0;
	}
	return !std::get<std::string>(*value).empty();
}

struct RegexAtom {
	char literal{};
	bool any{};
	bool zeroOrMore{};
};

//! A deliberately bounded regex subset avoids std::regex backtracking on a command hot path.
bool SafeRegexSearch(std::string_view subject, std::string_view expression)
{
	if (subject.size() > kMaxWorkbenchContextValueLength || expression.empty()
		|| expression.size() > 256) {
		return false;
	}
	bool anchoredStart = expression.front() == '^';
	if (anchoredStart) expression.remove_prefix(1);
	bool anchoredEnd = !expression.empty() && expression.back() == '$'
		&& (expression.size() < 2 || expression[expression.size() - 2] != '\\');
	if (anchoredEnd) expression.remove_suffix(1);
	std::vector<RegexAtom> atoms;
	if (!anchoredStart) atoms.push_back({ {}, true, true });
	for (std::size_t index = 0; index < expression.size();) {
		char character = expression[index++];
		bool any = character == '.';
		if (character == '\\') {
			if (index >= expression.size()) return false;
			character = expression[index++];
			any = false;
		} else if (character == '*' || character == '+' || character == '?' || character == '[' || character == ']'
			|| character == '(' || character == ')' || character == '|' || character == '{' || character == '}') {
			return false;
		}
		bool zeroOrMore = index < expression.size() && expression[index] == '*';
		if (zeroOrMore) ++index;
		atoms.push_back({ character, any, zeroOrMore });
	}
	std::vector<bool> previous(atoms.size() + 1);
	std::vector<bool> current(atoms.size() + 1);
	previous[0] = true;
	for (std::size_t atom = 0; atom < atoms.size(); ++atom) {
		previous[atom + 1] = atoms[atom].zeroOrMore && previous[atom];
	}
	bool matched = previous.back();
	for (char character : subject) {
		current.assign(atoms.size() + 1, false);
		for (std::size_t atom = 0; atom < atoms.size(); ++atom) {
			const bool matches = atoms[atom].any || atoms[atom].literal == character;
			current[atom + 1] = atoms[atom].zeroOrMore
				? current[atom] || (matches && previous[atom + 1])
				: matches && previous[atom];
		}
		previous.swap(current);
		matched = matched || previous.back();
	}
	if (anchoredEnd) return previous.back();
	return matched;
}

enum class EToken : std::uint8_t {
	End,
	Identifier,
	Boolean,
	Integer,
	String,
	Not,
	And,
	Or,
	Equal,
	NotEqual,
	Matches,
	Less,
	LessEqual,
	Greater,
	GreaterEqual,
	LeftParenthesis,
	RightParenthesis,
	Invalid,
};

struct Token {
	EToken kind = EToken::Invalid;
	std::string value;
};

class Lexer final {
public:
	explicit Lexer(std::string_view input)
		: m_input(input)
	{
	}

	[[nodiscard]] Token Next()
	{
		SkipWhitespace();
		if (++m_tokenCount > 256 || m_position >= m_input.size()) {
			return m_position >= m_input.size() && m_tokenCount <= 256 ? Token{ EToken::End, {} } : Token{};
		}

		const char current = m_input[m_position];
		if (std::isalpha(static_cast<unsigned char>(current)) || current == '_' || current == '$') {
			const std::size_t start = m_position++;
			while (m_position < m_input.size()) {
				const char character = m_input[m_position];
				if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '.' || character == '-')) {
					break;
				}
				++m_position;
			}
			std::string value(m_input.substr(start, m_position - start));
			if (value == "true" || value == "false") {
				return { EToken::Boolean, std::move(value) };
			}
			return { EToken::Identifier, std::move(value) };
		}
		if (std::isdigit(static_cast<unsigned char>(current)) || (current == '-' && m_position + 1 < m_input.size()
			&& std::isdigit(static_cast<unsigned char>(m_input[m_position + 1])))) {
			const std::size_t start = m_position;
			if (current == '-') ++m_position;
			while (m_position < m_input.size()
				&& std::isdigit(static_cast<unsigned char>(m_input[m_position]))) {
				++m_position;
			}
			return { EToken::Integer, std::string(m_input.substr(start, m_position - start)) };
		}
		if (current == '\'' || current == '"') {
			const char quote = current;
			++m_position;
			std::string value;
			while (m_position < m_input.size() && value.size() <= kMaxWorkbenchContextValueLength) {
				const char character = m_input[m_position++];
				if (character == quote) {
					return { EToken::String, std::move(value) };
				}
				if (character == '\\') {
					if (m_position >= m_input.size()) {
						return {};
					}
					value.push_back(m_input[m_position++]);
				} else {
					value.push_back(character);
				}
			}
			return {};
		}
		++m_position;
		switch (current) {
		case '!':
			if (Take('=')) return { EToken::NotEqual, {} };
			return { EToken::Not, {} };
		case '&': return Take('&') ? Token{ EToken::And, {} } : Token{};
		case '|': return Take('|') ? Token{ EToken::Or, {} } : Token{};
		case '=':
			if (Take('=')) return { EToken::Equal, {} };
			if (Take('~')) return { EToken::Matches, {} };
			return {};
		case '<': return Take('=') ? Token{ EToken::LessEqual, {} } : Token{ EToken::Less, {} };
		case '>': return Take('=') ? Token{ EToken::GreaterEqual, {} } : Token{ EToken::Greater, {} };
		case '(': return { EToken::LeftParenthesis, {} };
		case ')': return { EToken::RightParenthesis, {} };
		default: return {};
		}
	}

private:
	void SkipWhitespace() noexcept
	{
		while (m_position < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_position]))) {
			++m_position;
		}
	}
	[[nodiscard]] bool Take(char expected) noexcept
	{
		if (m_position < m_input.size() && m_input[m_position] == expected) {
			++m_position;
			return true;
		}
		return false;
	}

	std::string_view m_input;
	std::size_t m_position{};
	std::size_t m_tokenCount{};
};

class Parser final {
public:
	Parser(std::string_view input, const WorkbenchContextKeySnapshot& context)
		: m_lexer(input)
		, m_context(context)
	{
		Advance();
	}

	[[nodiscard]] bool Parse()
	{
		const auto result = ParseOr(0);
		return m_valid && result && m_current.kind == EToken::End;
	}

private:
	using Value = std::optional<WorkbenchContextValue>;

	void Advance() { m_current = m_lexer.Next(); }

	[[nodiscard]] bool ParseOr(std::size_t depth)
	{
		bool value = ParseAnd(depth + 1);
		while (m_current.kind == EToken::Or) {
			Advance();
			const bool right = ParseAnd(depth + 1);
			value = value || right;
		}
		return value;
	}

	[[nodiscard]] bool ParseAnd(std::size_t depth)
	{
		bool value = ParseNot(depth + 1);
		while (m_current.kind == EToken::And) {
			Advance();
			const bool right = ParseNot(depth + 1);
			value = value && right;
		}
		return value;
	}

	[[nodiscard]] bool ParseNot(std::size_t depth)
	{
		if (depth > 32) {
			m_valid = false;
			return false;
		}
		if (m_current.kind == EToken::Not) {
			Advance();
			return !ParseNot(depth + 1);
		}
		return ParseComparison(depth + 1);
	}

	[[nodiscard]] bool ParseComparison(std::size_t depth)
	{
		Value left = ParsePrimary(depth + 1);
		if (!m_valid) {
			return false;
		}
		const EToken operation = m_current.kind;
		if (operation != EToken::Equal && operation != EToken::NotEqual && operation != EToken::Matches
			&& operation != EToken::Less && operation != EToken::LessEqual
			&& operation != EToken::Greater && operation != EToken::GreaterEqual) {
			return IsTruthy(left);
		}
		Advance();
		Value right = ParsePrimary(depth + 1);
		if (!m_valid || !right) {
			m_valid = false;
			return false;
		}
		if (operation == EToken::Matches) {
			const auto* subject = left ? std::get_if<std::string>(&*left) : nullptr;
			const auto* pattern = std::get_if<std::string>(&*right);
			if (subject == nullptr || pattern == nullptr || pattern->size() > kMaxWorkbenchContextValueLength) {
				return false;
			}
			return SafeRegexSearch(*subject, *pattern);
		}
		if (operation == EToken::Less || operation == EToken::LessEqual
			|| operation == EToken::Greater || operation == EToken::GreaterEqual) {
			const auto* leftInteger = left ? std::get_if<std::int64_t>(&*left) : nullptr;
			const auto* rightInteger = std::get_if<std::int64_t>(&*right);
			if (leftInteger == nullptr || rightInteger == nullptr) return false;
			switch (operation) {
			case EToken::Less: return *leftInteger < *rightInteger;
			case EToken::LessEqual: return *leftInteger <= *rightInteger;
			case EToken::Greater: return *leftInteger > *rightInteger;
			case EToken::GreaterEqual: return *leftInteger >= *rightInteger;
			default: break;
			}
		}
		const bool equal = left && *left == *right;
		return operation == EToken::Equal ? equal : !equal;
	}

	[[nodiscard]] Value ParsePrimary(std::size_t depth)
	{
		if (depth > 32) {
			m_valid = false;
			return std::nullopt;
		}
		switch (m_current.kind) {
		case EToken::Identifier: {
			const std::string key = std::move(m_current.value);
			Advance();
			const auto found = m_context.values.find(key);
			return found == m_context.values.end() ? std::nullopt : Value(found->second);
		}
		case EToken::Boolean: {
			const bool value = m_current.value == "true";
			Advance();
			return WorkbenchContextValue(value);
		}
		case EToken::Integer: {
			std::int64_t value{};
			const auto text = m_current.value;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
				m_valid = false;
				return std::nullopt;
			}
			Advance();
			return WorkbenchContextValue(value);
		}
		case EToken::String: {
			std::string value = std::move(m_current.value);
			Advance();
			return WorkbenchContextValue(std::move(value));
		}
		case EToken::LeftParenthesis: {
			Advance();
			const bool value = ParseOr(depth + 1);
			if (m_current.kind != EToken::RightParenthesis) {
				m_valid = false;
				return std::nullopt;
			}
			Advance();
			return WorkbenchContextValue(value);
		}
		default:
			m_valid = false;
			return std::nullopt;
		}
	}

	Lexer m_lexer;
	const WorkbenchContextKeySnapshot& m_context;
	Token m_current;
	bool m_valid = true;
};

} // namespace

bool WorkbenchCommandOwner::IsValid() const noexcept
{
	return !ownerId.empty() && ownerId.size() <= kMaxWorkbenchContextKeyLength && ownerId.find('\0') == std::string::npos
		&& generation != 0;
}

bool WorkbenchContextKeyService::IsValidKey(std::string_view key) noexcept
{
	return !key.empty() && key.size() <= kMaxWorkbenchContextKeyLength && key.find('\0') == std::string_view::npos;
}

bool WorkbenchContextKeyService::IsReservedCoreKey(std::string_view key) noexcept
{
	return key == "workbenchReady" || key.starts_with("workbench.")
		|| key == "workbenchState" || key == "workspaceFolderCount"
		|| key == "editorHasActiveEditor" || key == "editorIsDirty";
}

WorkbenchContextMutationResult WorkbenchContextKeyService::SetCoreProjection(
	const layout::WorkbenchLayoutStateSnapshot& snapshot)

{
	return SetCoreProjection(snapshot, {}, {}, false);
}

WorkbenchContextMutationResult WorkbenchContextKeyService::SetCoreProjection(
	const layout::WorkbenchLayoutStateSnapshot& snapshot,
	const config::WorkspaceContextSnapshot& workspace)
{
	return SetCoreProjection(snapshot, workspace, {}, false);
}

WorkbenchContextMutationResult WorkbenchContextKeyService::SetCoreProjection(
	const layout::WorkbenchLayoutStateSnapshot& snapshot,
	const config::WorkspaceContextSnapshot& workspace,
	WorkbenchEditorCommandContext editor,
	bool recentlyOpenedAvailable)
{
	WorkbenchContextKeyMap values;
	const auto sidebar = std::find_if(snapshot.parts.begin(), snapshot.parts.end(), [](const auto& part) {
		return part.partId == layout::ids::part::Sidebar;
	});
	values.emplace("workbenchReady", true);
	values.emplace("workbench.sidebarVisible", sidebar != snapshot.parts.end() && sidebar->visible);

	std::optional<std::string> activeView;
	if (snapshot.activeContainers.sideBar) {
		const auto container = std::find_if(snapshot.containers.begin(), snapshot.containers.end(), [&](const auto& candidate) {
			return candidate.containerId == *snapshot.activeContainers.sideBar;
		});
		if (container != snapshot.containers.end() && container->activeViewId) {
			activeView = *container->activeViewId;
		}
	}
	values.emplace("workbench.activeView", activeView.value_or(std::string()));
	values.emplace("workbench.explorerActive", snapshot.activeContainers.sideBar
		&& *snapshot.activeContainers.sideBar == layout::ids::viewContainer::Explorer);
	const char* workbenchState = "empty";
	switch (workspace.kind) {
	case config::EWorkspaceKind::Empty: break;
	case config::EWorkspaceKind::Folder: workbenchState = "folder"; break;
	case config::EWorkspaceKind::Workspace: workbenchState = "workspace"; break;
	}
	values.emplace("workbenchState", std::string(workbenchState));
	values.emplace("workspaceFolderCount", static_cast<std::int64_t>(workspace.folders.size()));
	values.emplace("editorHasActiveEditor", editor.hasActiveEditor);
	values.emplace("editorIsDirty", editor.activeEditorDirty);
	values.emplace("workbench.recentlyOpenedAvailable", recentlyOpenedAvailable);

	std::lock_guard lock(m_mutex);
	if (m_coreValues == values) {
		return { EWorkbenchContextMutationStatus::NotApplicable, m_revision };
	}
	m_coreValues = std::move(values);
	return { EWorkbenchContextMutationStatus::Succeeded, ++m_revision };
}

WorkbenchContextMutationResult WorkbenchContextKeyService::SetExtensionOverlay(
	const WorkbenchCommandOwner& owner, WorkbenchContextKeyMap values)
{
	if (!owner.IsValid()) {
		return { EWorkbenchContextMutationStatus::Invalid, Snapshot().revision };
	}
	for (const auto& [key, value] : values) {
		if (!IsValidKey(key) || IsReservedCoreKey(key) || !IsBoundedValue(value)) {
			return { EWorkbenchContextMutationStatus::Invalid, Snapshot().revision };
		}
	}
	std::lock_guard lock(m_mutex);
	const auto found = m_overlays.find(owner);
	if (found != m_overlays.end() && found->second.values == values) {
		return { EWorkbenchContextMutationStatus::NotApplicable, m_revision };
	}
	m_overlays.insert_or_assign(owner, Overlay{ std::move(values) });
	return { EWorkbenchContextMutationStatus::Succeeded, ++m_revision };
}

WorkbenchContextMutationResult WorkbenchContextKeyService::DisposeExtensionOverlay(const WorkbenchCommandOwner& owner)
{
	if (!owner.IsValid()) {
		return { EWorkbenchContextMutationStatus::Invalid, Snapshot().revision };
	}
	std::lock_guard lock(m_mutex);
	if (m_overlays.erase(owner) == 0) {
		return { EWorkbenchContextMutationStatus::NotApplicable, m_revision };
	}
	return { EWorkbenchContextMutationStatus::Succeeded, ++m_revision };
}

WorkbenchContextKeySnapshot WorkbenchContextKeyService::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	WorkbenchContextKeySnapshot snapshot;
	snapshot.revision = m_revision;
	snapshot.values = m_coreValues;
	for (const auto& [owner, overlay] : m_overlays) {
		(void)owner;
		for (const auto& [key, value] : overlay.values) {
			snapshot.values.insert_or_assign(key, value);
		}
	}
	return snapshot;
}

bool WorkbenchWhenClauseEvaluator::Evaluate(std::string_view expression,
	const WorkbenchContextKeySnapshot& context) noexcept
{
	if (expression.empty()) {
		return true;
	}
	if (expression.size() > kMaxWorkbenchContextValueLength) {
		return false;
	}
	try {
		return Parser(expression, context).Parse();
	} catch (...) {
		return false;
	}
}

} // namespace workbench::commands
