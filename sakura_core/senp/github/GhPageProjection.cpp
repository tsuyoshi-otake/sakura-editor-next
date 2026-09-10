/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhPageProjection.h"

#include <sakura/serialization/JsoncDocument.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace senp::github {
namespace {

using platform::serialization::CJsoncDocument;
using platform::serialization::JsoncValue;

struct ProjectionField;
using ProjectionFields = std::span<const ProjectionField>;

/*!
	@brief One member an answer of this shape consists of, and its own members.

	The rule is uniform, so a field list never has to say which kind of value it
	describes: a scalar keeps its value, an array is projected element by element,
	and an object keeps exactly the members named here. An empty list is therefore
	not "keep everything" - it keeps an array of scalars as it is, and reduces an
	object to `{}`, which is how a presence marker survives without its payload.
*/
struct ProjectionField final {
	std::wstring_view name;
	ProjectionFields members{};
};

constexpr std::array<ProjectionField, 1> kUser{ { { L"login" } } };
constexpr std::array<ProjectionField, 1> kLabel{ { { L"name" } } };
constexpr std::array<ProjectionField, 1> kRepository{ { { L"full_name" } } };
constexpr std::array<ProjectionField, 3> kRepositoryRef{ {
	{ L"ref" }, { L"repo", kRepository }, { L"sha" },
} };

constexpr std::array<ProjectionField, 9> kIssue{ {
	{ L"comments" }, { L"html_url" }, { L"id" }, { L"labels", kLabel }, { L"number" },
	// The marker is read as presence alone, so the object it names is kept and
	// emptied. Writing a null in its place would say "this issue is a pull
	// request", which is the opposite of what an absent member means.
	{ L"pull_request" }, { L"state" }, { L"title" }, { L"user", kUser },
} };
constexpr std::array<ProjectionField, 12> kIssueDetail{ {
	{ L"body" }, { L"comments" }, { L"created_at" }, { L"html_url" }, { L"id" },
	{ L"labels", kLabel }, { L"number" }, { L"pull_request" }, { L"state" }, { L"title" },
	{ L"updated_at" }, { L"user", kUser },
} };
constexpr std::array<ProjectionField, 6> kComment{ {
	{ L"body" }, { L"created_at" }, { L"html_url" }, { L"id" }, { L"updated_at" },
	{ L"user", kUser },
} };
constexpr std::array<ProjectionField, 12> kPullRequest{ {
	{ L"base", kRepositoryRef }, { L"comments" }, { L"draft" }, { L"head", kRepositoryRef },
	{ L"html_url" }, { L"id" }, { L"labels", kLabel }, { L"merged_at" }, { L"number" },
	{ L"state" }, { L"title" }, { L"user", kUser },
} };
constexpr std::array<ProjectionField, 15> kPullRequestDetail{ {
	{ L"base", kRepositoryRef }, { L"body" }, { L"comments" }, { L"created_at" }, { L"draft" },
	{ L"head", kRepositoryRef }, { L"html_url" }, { L"id" }, { L"labels", kLabel },
	{ L"merged_at" }, { L"number" }, { L"state" }, { L"title" }, { L"updated_at" },
	{ L"user", kUser },
} };

constexpr std::array<ProjectionField, 5> kWorkflow{ {
	{ L"html_url" }, { L"id" }, { L"name" }, { L"path" }, { L"state" },
} };
constexpr std::array<ProjectionField, 2> kWorkflows{ {
	{ L"total_count" }, { L"workflows", kWorkflow },
} };
constexpr std::array<ProjectionField, 15> kRun{ {
	{ L"conclusion" }, { L"created_at" }, { L"display_title" }, { L"event" }, { L"head_branch" },
	{ L"head_sha" }, { L"html_url" }, { L"id" }, { L"name" }, { L"run_attempt" },
	{ L"run_number" }, { L"run_started_at" }, { L"status" }, { L"updated_at" },
	{ L"workflow_id" },
} };
constexpr std::array<ProjectionField, 2> kRuns{ {
	{ L"total_count" }, { L"workflow_runs", kRun },
} };
constexpr std::array<ProjectionField, 6> kStep{ {
	{ L"completed_at" }, { L"conclusion" }, { L"name" }, { L"number" }, { L"started_at" },
	{ L"status" },
} };
constexpr std::array<ProjectionField, 16> kJob{ {
	{ L"completed_at" }, { L"conclusion" }, { L"head_sha" }, { L"html_url" }, { L"id" },
	// A job's labels are plain strings, so the empty list keeps them whole. The
	// same list under an issue's labels would be wrong, which is why that one
	// names the member it keeps.
	{ L"labels" }, { L"name" }, { L"run_attempt" }, { L"run_id" }, { L"runner_group_id" },
	{ L"runner_group_name" }, { L"runner_id" }, { L"runner_name" }, { L"started_at" },
	{ L"status" }, { L"steps", kStep },
} };
constexpr std::array<ProjectionField, 2> kJobs{ {
	{ L"jobs", kJob }, { L"total_count" },
} };

struct ShapeProjection final {
	std::wstring_view shape;
	ProjectionFields fields;
};

constexpr std::array<ShapeProjection, 14> kShapes{ {
	{ L"issues", kIssue },
	{ L"issue", kIssueDetail },
	{ L"issueComments", kComment },
	{ L"issueComment", kComment },
	{ L"pulls", kPullRequest },
	{ L"pull", kPullRequestDetail },
	{ L"workflows", kWorkflows },
	{ L"runs", kRuns },
	{ L"workflowRuns", kRuns },
	{ L"run", kRun },
	{ L"runAttempt", kRun },
	{ L"runJobs", kJobs },
	{ L"runAttemptJobs", kJobs },
	{ L"job", kJob },
} };

const ShapeProjection* FindShape(const std::wstring_view shape) noexcept
{
	for (const auto& entry : kShapes) {
		if (entry.shape == shape) return &entry;
	}
	return nullptr;
}

JsoncValue Project(const JsoncValue& value, ProjectionFields fields)
{
	if (const auto* object = std::get_if<JsoncValue::Object>(&value.Value())) {
		JsoncValue::Object kept;
		for (const auto& field : fields) {
			const auto member = object->find(field.name);
			if (member == object->end()) continue;
			kept.emplace(member->first, Project(member->second, field.members));
		}
		return JsoncValue(std::move(kept));
	}
	if (const auto* array = std::get_if<JsoncValue::Array>(&value.Value())) {
		JsoncValue::Array kept;
		kept.reserve(array->size());
		for (const auto& element : *array) kept.push_back(Project(element, fields));
		return JsoncValue(std::move(kept));
	}
	return value;
}

//! Strict narrowing. A string the response could not have carried as UTF-8 is
//! refused rather than written out with replacement characters.
bool AppendNarrowed(const std::wstring& value, std::string& output)
{
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return false;
	const auto offset = output.size();
	output.resize(offset + static_cast<std::size_t>(length));
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
		static_cast<int>(value.size()), output.data() + offset, length, nullptr, nullptr) == length;
}

bool AppendQuoted(const std::wstring& value, std::string& output)
{
	output.push_back('"');
	std::wstring pending;
	const auto flush = [&pending, &output] { return AppendNarrowed(std::exchange(pending, {}), output); };
	for (const auto character : value) {
		const auto escape = [&](const char* text) { return flush() && (output.append(text), true); };
		switch (character) {
		case L'"': if (!escape("\\\"")) return false; continue;
		case L'\\': if (!escape("\\\\")) return false; continue;
		case L'\b': if (!escape("\\b")) return false; continue;
		case L'\f': if (!escape("\\f")) return false; continue;
		case L'\n': if (!escape("\\n")) return false; continue;
		case L'\r': if (!escape("\\r")) return false; continue;
		case L'\t': if (!escape("\\t")) return false; continue;
		default: break;
		}
		if (character < 0x20) {
			if (!flush()) return false;
			static constexpr char kDigits[] = "0123456789abcdef";
			output.append("\\u00");
			output.push_back(kDigits[(character >> 4) & 0xF]);
			output.push_back(kDigits[character & 0xF]);
			continue;
		}
		pending.push_back(character);
	}
	if (!flush()) return false;
	output.push_back('"');
	return true;
}

bool Serialize(const JsoncValue& value, std::string& output)
{
	return std::visit([&output](const auto& held) {
		using Held = std::decay_t<decltype(held)>;
		if constexpr (std::is_same_v<Held, std::monostate>) {
			output.append("null");
			return true;
		} else if constexpr (std::is_same_v<Held, bool>) {
			output.append(held ? "true" : "false");
			return true;
		} else if constexpr (std::is_same_v<Held, std::int64_t>) {
			output.append(std::to_string(held));
			return true;
		} else if constexpr (std::is_same_v<Held, double>) {
			std::array<char, 32> buffer{};
			const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), held);
			if (converted.ec != std::errc{}) return false;
			output.append(buffer.data(), converted.ptr);
			return true;
		} else if constexpr (std::is_same_v<Held, std::wstring>) {
			return AppendQuoted(held, output);
		} else if constexpr (std::is_same_v<Held, JsoncValue::Array>) {
			output.push_back('[');
			for (std::size_t index = 0; index < held.size(); ++index) {
				if (index) output.push_back(',');
				if (!Serialize(held[index], output)) return false;
			}
			output.push_back(']');
			return true;
		} else {
			output.push_back('{');
			auto first = true;
			for (const auto& [name, member] : held) {
				if (!std::exchange(first, false)) output.push_back(',');
				if (!AppendQuoted(name, output) || (output.push_back(':'), !Serialize(member, output))) {
					return false;
				}
			}
			output.push_back('}');
			return true;
		}
	}, value.Value());
}

} // namespace

bool HasRepositoryPageProjection(const std::wstring_view shape) noexcept
{
	return FindShape(shape) != nullptr;
}

std::optional<std::string> ProjectRepositoryPage(const std::wstring_view shape, const std::string& body)
try {
	const auto* projection = FindShape(shape);
	if (!projection) return std::nullopt;
	const auto parsed = CJsoncDocument::ParseStrict(body);
	if (!parsed.Succeeded()) return std::nullopt;
	// A response is one object or one list of them. Anything else is not the
	// shape's answer, and projecting it would state a structure it never had.
	const auto& value = parsed.value->Value();
	if (!std::holds_alternative<JsoncValue::Object>(value)
		&& !std::holds_alternative<JsoncValue::Array>(value)) return std::nullopt;
	std::string projected;
	projected.reserve(body.size() / 8);
	if (!Serialize(Project(*parsed.value, projection->fields), projected)) return std::nullopt;
	return projected;
} catch (...) {
	return std::nullopt;
}

} // namespace senp::github
