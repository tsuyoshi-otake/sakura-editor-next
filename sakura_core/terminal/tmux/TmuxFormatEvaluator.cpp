/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/tmux/TmuxFormatEvaluator.h"

#include <charconv>
#include <cctype>
#include <limits>
#include <string>

namespace terminal::tmux {
namespace {

[[nodiscard]] TmuxFormatResult FormatError(TmuxFormatCode code) noexcept
{
	TmuxFormatResult result;
	result.code = code;
	return result;
}

[[nodiscard]] bool IsSafeText(std::string_view value) noexcept
{
	for (const auto ch : value) {
		const auto byte = static_cast<unsigned char>(ch);
		if (byte == 0 || byte < 0x20 || byte == 0x7f) return false;
	}
	return true;
}

[[nodiscard]] std::string IdText(char prefix, std::uint64_t value)
{
	return std::string(1, prefix) + std::to_string(value);
}

[[nodiscard]] std::optional<std::string> Variable(std::string_view name,
	const TmuxFormatContext& context)
{
	if (name == "session_id" && context.session) return IdText('$', context.session->id.value);
	if (name == "session_name" && context.session) return context.session->name;
	if (name == "session_windows" && context.session) return std::to_string(context.session->windows.size());
	if (name == "session_created" && context.session) return std::to_string(context.session->createdSeconds);
	if (name == "session_activity" && context.session) return std::to_string(context.session->activitySeconds);
	if (name == "session_attached" && context.session) return context.session->attached ? "1" : "0";
	if (name == "window_id" && context.window) return IdText('@', context.window->id.value);
	if (name == "window_index" && context.window) return std::to_string(context.window->index);
	if (name == "window_name" && context.window) return context.window->name;
	if (name == "window_active" && context.window) return context.window->active ? "1" : "0";
	if (name == "window_panes" && context.window) return std::to_string(context.window->panes.size());
	if (name == "window_width" && context.window) return std::to_string(context.window->width);
	if (name == "window_height" && context.window) return std::to_string(context.window->height);
	if (name == "window_layout" && context.window) return context.window->layout;
	if (name == "pane_id" && context.pane) return IdText('%', context.pane->id.value);
	if (name == "pane_index" && context.pane) return std::to_string(context.pane->index);
	if (name == "pane_active" && context.pane) return context.pane->active ? "1" : "0";
	if (name == "pane_width" && context.pane) return std::to_string(context.pane->width);
	if (name == "pane_height" && context.pane) return std::to_string(context.pane->height);
	if (name == "pane_title" && context.pane) return context.pane->title;
	if (name == "pane_current_command" && context.pane) return context.pane->currentCommand;
	if (name == "pane_dead" && context.pane) return context.pane->dead ? "1" : "0";
	if (name == "pane_dead_status" && context.pane) {
		return context.pane->deadStatus ? std::optional<std::string>(std::to_string(*context.pane->deadStatus)) : std::optional<std::string>("");
	}
	if (name == "history_size" && context.pane) return std::to_string(context.pane->historySize);
	if (name == "history_limit" && context.pane) return std::to_string(context.pane->historyLimit);
	return std::nullopt;
}

[[nodiscard]] TmuxFormatResult EvaluateInternal(std::string_view format,
	const TmuxFormatContext& context, const TmuxFormatLimits& limits, std::size_t depth)
{
	if (format.size() > limits.maximumInputBytes || depth > limits.maximumNestingDepth || !IsSafeText(format)) {
		return FormatError(TmuxFormatCode::InvalidFormat);
	}
	TmuxFormatResult result;
	result.code = TmuxFormatCode::Succeeded;
	for (std::size_t index = 0; index < format.size(); ++index) {
		if (format[index] != '#') {
			result.value.push_back(format[index]);
		} else if (index + 1 < format.size() && format[index + 1] == '#') {
			result.value.push_back('#');
			++index;
		} else if (index + 1 < format.size() && format[index + 1] == '{') {
			const auto close = format.find('}', index + 2);
			if (close == std::string_view::npos || close == index + 2) return FormatError(TmuxFormatCode::InvalidFormat);
			const auto name = format.substr(index + 2, close - index - 2);
			if (name.find(':') != std::string_view::npos || name.find('{') != std::string_view::npos) {
				return FormatError(TmuxFormatCode::InvalidFormat);
			}
			const auto value = Variable(name, context);
			if (!value) return FormatError(TmuxFormatCode::UnknownVariable);
			result.value += *value;
			index = close;
		} else {
			return FormatError(TmuxFormatCode::InvalidFormat);
		}
		if (result.value.size() > limits.maximumExpandedBytes) return FormatError(TmuxFormatCode::ResourceExhausted);
	}
	return result;
}

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
	return value;
}

[[nodiscard]] bool IsWrapped(std::string_view value) noexcept
{
	if (value.size() < 2 || value.front() != '(' || value.back() != ')') return false;
	std::size_t depth = 0;
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '(') ++depth;
		else if (value[i] == ')' && --depth == 0 && i + 1 != value.size()) return false;
	}
	return depth == 0;
}

[[nodiscard]] std::optional<std::size_t> FindOperator(std::string_view value, std::string_view op) noexcept
{
	int parentheses = 0;
	std::size_t formatDepth = 0;
	for (std::size_t i = 0; i + op.size() <= value.size(); ++i) {
		if (value.substr(i, 2) == "#{") ++formatDepth;
		else if (value[i] == '}' && formatDepth != 0) --formatDepth;
		else if (value[i] == '(') ++parentheses;
		else if (value[i] == ')' && parentheses > 0) --parentheses;
		if (formatDepth == 0 && parentheses == 0 && value.substr(i, op.size()) == op) return i;
	}
	return std::nullopt;
}

[[nodiscard]] bool ParseInteger(std::string_view value, std::int64_t& output) noexcept
{
	if (value.empty()) return false;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] std::vector<std::string_view> SplitArguments(std::string_view value)
{
	std::vector<std::string_view> parts;
	std::size_t begin = 0;
	std::size_t depth = 0;
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value.substr(i, 2) == "#{") ++depth;
		else if (value[i] == '}' && depth != 0) --depth;
		else if (value[i] == ',' && depth == 0) {
			parts.push_back(value.substr(begin, i - begin));
			begin = i + 1;
		}
	}
	parts.push_back(value.substr(begin));
	return parts;
}

[[nodiscard]] TmuxFilterResult FilterError(TmuxFormatCode code) noexcept
{
	TmuxFilterResult result;
	result.code = code;
	return result;
}

[[nodiscard]] TmuxFilterResult EvaluateFilterInternal(std::string_view expression,
	const TmuxFormatContext& context, const TmuxFormatLimits& limits, std::size_t depth);

[[nodiscard]] TmuxFilterResult EvaluateFilterOperator(std::string_view inner,
	const TmuxFormatContext& context, const TmuxFormatLimits& limits, std::size_t depth)
{
	const auto colon = inner.find(':');
	if (colon == std::string_view::npos) return FilterError(TmuxFormatCode::InvalidFormat);
	const auto op = inner.substr(0, colon);
	const auto args = SplitArguments(inner.substr(colon + 1));
	if ((op == "&&" || op == "||") && args.size() == 2) {
		const auto left = EvaluateFilterInternal(args[0], context, limits, depth + 1);
		if (!left.Succeeded()) return left;
		if (op == "&&" && !left.value) return left;
		if (op == "||" && left.value) return left;
		return EvaluateFilterInternal(args[1], context, limits, depth + 1);
	}
	if (op == "!" && args.size() == 1) {
		auto result = EvaluateFilterInternal(args[0], context, limits, depth + 1);
		if (result.Succeeded()) result.value = !result.value;
		return result;
	}
	if ((op == "==" || op == "!=") && args.size() == 2) {
		const auto left = EvaluateInternal(Trim(args[0]), context, limits, depth + 1);
		const auto right = EvaluateInternal(Trim(args[1]), context, limits, depth + 1);
		if (!left.Succeeded()) return FilterError(left.code);
		if (!right.Succeeded()) return FilterError(right.code);
		const bool equal = left.value == right.value;
		return { TmuxFormatCode::Succeeded, op == "==" ? equal : !equal };
	}
	return FilterError(TmuxFormatCode::InvalidFormat);
}

[[nodiscard]] TmuxFilterResult EvaluateFilterInternal(std::string_view expression,
	const TmuxFormatContext& context, const TmuxFormatLimits& limits, std::size_t depth)
{
	expression = Trim(expression);
	if (expression.empty() || depth > limits.maximumNestingDepth) return FilterError(TmuxFormatCode::InvalidFormat);
	if (IsWrapped(expression)) return EvaluateFilterInternal(expression.substr(1, expression.size() - 2), context, limits, depth + 1);
	if (expression.size() >= 3 && expression.substr(0, 2) == "#{" && expression.back() == '}') {
		return EvaluateFilterOperator(expression.substr(2, expression.size() - 3), context, limits, depth);
	}
	if (expression.front() == '!') {
		auto result = EvaluateFilterInternal(expression.substr(1), context, limits, depth + 1);
		if (result.Succeeded()) result.value = !result.value;
		return result;
	}
	for (const auto op : { std::string_view("||"), std::string_view("&&") }) {
		if (const auto position = FindOperator(expression, op)) {
			auto left = EvaluateFilterInternal(expression.substr(0, *position), context, limits, depth + 1);
			if (!left.Succeeded()) return left;
			if (op == "&&" && !left.value) return left;
			if (op == "||" && left.value) return left;
			return EvaluateFilterInternal(expression.substr(*position + op.size()), context, limits, depth + 1);
		}
	}
	for (const auto op : { std::string_view("=="), std::string_view("!="), std::string_view(">="), std::string_view("<="), std::string_view(">"), std::string_view("<") }) {
		if (const auto position = FindOperator(expression, op)) {
			const auto left = EvaluateInternal(Trim(expression.substr(0, *position)), context, limits, depth + 1);
			const auto right = EvaluateInternal(Trim(expression.substr(*position + op.size())), context, limits, depth + 1);
			if (!left.Succeeded()) return FilterError(left.code);
			if (!right.Succeeded()) return FilterError(right.code);
			std::int64_t leftNumber{}, rightNumber{};
			const bool numeric = ParseInteger(left.value, leftNumber) && ParseInteger(right.value, rightNumber);
			bool value = false;
			if (op == "==") value = left.value == right.value;
			else if (op == "!=") value = left.value != right.value;
			else if (numeric && op == ">=") value = leftNumber >= rightNumber;
			else if (numeric && op == "<=") value = leftNumber <= rightNumber;
			else if (numeric && op == ">") value = leftNumber > rightNumber;
			else if (numeric && op == "<") value = leftNumber < rightNumber;
			else return FilterError(TmuxFormatCode::InvalidFormat);
			return { TmuxFormatCode::Succeeded, value };
		}
	}
	const auto value = EvaluateInternal(expression, context, limits, depth + 1);
	if (!value.Succeeded()) return FilterError(value.code);
	if (value.value == "1" || value.value == "true" || value.value == "on" || value.value == "yes") {
		return { TmuxFormatCode::Succeeded, true };
	}
	if (value.value.empty() || value.value == "0" || value.value == "false" || value.value == "off" || value.value == "no") {
		return { TmuxFormatCode::Succeeded, false };
	}
	return FilterError(TmuxFormatCode::InvalidFormat);
}

} // namespace

TmuxFormatResult TmuxFormatEvaluator::Evaluate(std::string_view format,
	const TmuxFormatContext& context, const TmuxFormatLimits limits) noexcept
{
	try {
		return EvaluateInternal(format, context, limits, 0);
	} catch (...) {
		return FormatError(TmuxFormatCode::ResourceExhausted);
	}
}

TmuxFilterResult TmuxFormatEvaluator::EvaluateFilter(std::string_view expression,
	const TmuxFormatContext& context, const TmuxFormatLimits limits) noexcept
{
	try {
		if (expression.size() > limits.maximumInputBytes || !IsSafeText(expression)) return FilterError(TmuxFormatCode::InvalidFormat);
		return EvaluateFilterInternal(expression, context, limits, 0);
	} catch (...) {
		return FilterError(TmuxFormatCode::ResourceExhausted);
	}
}

} // namespace terminal::tmux
