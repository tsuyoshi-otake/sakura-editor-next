/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionWorkbenchDispatcher.h"
#include "extension/CExtensionWorkbenchServiceBridge.h"

#include <sakura/serialization/JsoncDocument.h>

#include <picojson/picojson.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "util/string_ex.h"

namespace {

const picojson::value* Find(const picojson::object& object, const char* key)
{
	const auto found = object.find(key);
	return found == object.end() ? nullptr : &found->second;
}

SExtensionWorkbenchDispatchResult Failure(std::string message, int code = -32602)
{
	return { .handled = true, .success = false, .errorCode = code, .errorMessage = std::move(message) };
}

SExtensionWorkbenchDispatchResult Success(
	EExtensionWorkbenchChange changes = EExtensionWorkbenchChange::None,
	std::string resultJson = "{}")
{
	return { .handled = true, .success = true, .resultJson = std::move(resultJson), .changes = changes };
}

bool ParseObject(std::string_view json, picojson::object& object, std::string& error)
{
	if (json.empty()) {
		object.clear();
		return true;
	}
	picojson::value value;
	error = picojson::parse(value, std::string(json));
	if (!error.empty() || !value.is<picojson::object>()) {
		if (error.empty()) error = "params must be a JSON object";
		return false;
	}
	object = value.get<picojson::object>();
	return true;
}

bool RequiredString(const picojson::object& object, const char* key, std::wstring& value, std::string& error)
{
	const auto* source = Find(object, key);
	if (!source || !source->is<std::string>() || source->get<std::string>().empty()) {
		error = std::string(key) + " must be a non-empty string";
		return false;
	}
	value = u8stowcs(source->get<std::string>());
	if (value.empty() || value.find(L'\0') != std::wstring::npos) {
		error = std::string(key) + " is not a valid UTF-8 string";
		return false;
	}
	return true;
}

std::wstring OptionalString(const picojson::object& object, const char* key)
{
	const auto* source = Find(object, key);
	return source && source->is<std::string>() ? u8stowcs(source->get<std::string>()) : std::wstring();
}

std::wstring PresentationString(const picojson::object& object, const char* key)
{
	const auto* source = Find(object, key);
	if (!source) return {};
	if (source->is<std::string>()) return u8stowcs(source->get<std::string>());
	if (source->is<picojson::object>()) {
		const auto& nested = source->get<picojson::object>();
		for (const char* nestedKey : { "markdown", "label", "value" }) {
			if (const auto* text = Find(nested, nestedKey); text && text->is<std::string>()) {
				return u8stowcs(text->get<std::string>());
			}
		}
	}
	return {};
}

//! `MarkdownString` として送られてきたプレゼンテーション値の supportThemeIcons を読む。
//! VS Code は supportThemeIcons が真のときだけ `$(name)` をコディコンとして描画する。
//! プレーン文字列で送られた値（MarkdownString ではない）は常に偽。
bool PresentationSupportsThemeIcons(const picojson::object& object, const char* key)
{
	const auto* source = Find(object, key);
	if (!source || !source->is<picojson::object>()) return false;
	const auto& nested = source->get<picojson::object>();
	const auto* flag = Find(nested, "supportThemeIcons");
	return flag && flag->is<bool>() && flag->get<bool>();
}

bool PresentationIsTrusted(const picojson::object& object, const char* key)
{
	const auto* source = Find(object, key);
	if (!source || !source->is<picojson::object>()) return false;
	const auto& nested = source->get<picojson::object>();
	constexpr char trustedKey[] = { 'i', 's', 'T', 'r', 'u', 's', 't', 'e', 'd', '\0' };
	const auto* flag = Find(nested, trustedKey);
	return flag && flag->is<bool>() && flag->get<bool>();
}

std::vector<std::wstring> PresentationTrustedCommands(const picojson::object& object, const char* key)
{
	const auto* source = Find(object, key);
	if (!source || !source->is<picojson::object>()) return {};
	const auto& nested = source->get<picojson::object>();
	constexpr char trustedKey[] = { 'i', 's', 'T', 'r', 'u', 's', 't', 'e', 'd', '\0' };
	const auto* trusted = Find(nested, trustedKey);
	if (!trusted || !trusted->is<picojson::object>()) return {};
	const auto& trustedObject = trusted->get<picojson::object>();
	constexpr char enabledCommandsKey[] = {
		'e', 'n', 'a', 'b', 'l', 'e', 'd', 'C', 'o', 'm', 'm', 'a', 'n', 'd', 's', '\0'
	};
	const auto* enabledCommands = Find(trustedObject, enabledCommandsKey);
	if (!enabledCommands || !enabledCommands->is<picojson::array>()) return {};
	std::vector<std::wstring> result;
	for (const auto& command : enabledCommands->get<picojson::array>()) {
		if (!command.is<std::string>()) continue;
		const auto value = u8stowcs(command.get<std::string>());
		if (!value.empty()) result.push_back(value);
	}
	return result;
}

std::wstring CommandString(const picojson::object& object)
{
	const auto* command = Find(object, "command");
	if (!command) return {};
	if (command->is<std::string>()) return u8stowcs(command->get<std::string>());
	if (command->is<picojson::object>()) return OptionalString(command->get<picojson::object>(), "command");
	return {};
}

bool Generation(const picojson::object& object, std::uint64_t& generation, std::string& error)
{
	const auto* source = Find(object, "generation");
	if (!source || !source->is<double>()) {
		error = "generation must be a positive integer";
		return false;
	}
	const double number = source->get<double>();
	if (!std::isfinite(number) || std::floor(number) != number || number <= 0 ||
		number > static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) {
		error = "generation must be a positive integer";
		return false;
	}
	generation = static_cast<std::uint64_t>(number);
	return true;
}

bool OptionalBool(const picojson::object& object, const char* key, bool fallback = false)
{
	const auto* source = Find(object, key);
	return source && source->is<bool>() ? source->get<bool>() : fallback;
}

bool RequiredOutputOperationId(const picojson::object& object, std::string& operationId, std::string& error)
{
	const auto* source = Find(object, "operationId");
	if (!source || !source->is<std::string>() || !workbench::output::OutputService::IsValidOperationId(source->get<std::string>())) {
		error = "operationId must be a non-empty bounded stable string";
		return false;
	}
	operationId = source->get<std::string>();
	return true;
}

bool UInt32(const picojson::object& object, const char* key, std::uint32_t& value)
{
	const auto* source = Find(object, key);
	if (!source || !source->is<double>()) return false;
	const auto number = source->get<double>();
	if (!std::isfinite(number) || std::floor(number) != number || number < 0 ||
		number > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())) return false;
	value = static_cast<std::uint32_t>(number);
	return true;
}

bool ScmInputBox(const picojson::value* source, workbench::scm::ScmInputBoxState& input, std::string& error)
{
	if (!source || !source->is<picojson::object>()) {
		error = "inputBox must be an object";
		return false;
	}
	const auto& object = source->get<picojson::object>();
	for (const auto& [key, target] : object) {
		if (key == "value" || key == "placeholder") {
			if (!target.is<std::string>()) {
				error = "inputBox text fields must be strings";
				return false;
			}
			if (key == "value") input.value = target.get<std::string>();
			else input.placeholder = target.get<std::string>();
		} else if (key == "enabled" || key == "visible") {
			if (!target.is<bool>()) {
				error = "inputBox enabled/visible fields must be booleans";
				return false;
			}
			if (key == "enabled") input.enabled = target.get<bool>();
			else input.visible = target.get<bool>();
		}
	}
	return true;
}

bool ScmCommand(const picojson::value* source, workbench::scm::ScmCommand& command, std::string& error)
{
	if (!source || !source->is<picojson::object>()) {
		error = "SCM command must be an object";
		return false;
	}
	const auto& object = source->get<picojson::object>();
	const auto* id = Find(object, "command");
	const auto* title = Find(object, "title");
	if (!id || !id->is<std::string>() || id->get<std::string>().empty() ||
		!title || !title->is<std::string>() || title->get<std::string>().empty()) {
		error = "SCM command requires non-empty command and title";
		return false;
	}
	command.command = id->get<std::string>();
	command.title = title->get<std::string>();
	if (const auto* arguments = Find(object, "arguments")) {
		if (!arguments->is<picojson::array>()) {
			error = "SCM command arguments must be an array";
			return false;
		}
		command.argumentsJson = arguments->serialize();
	} else {
		command.argumentsJson = "[]";
	}
	return true;
}

bool ScmIconPath(const picojson::value* source, std::optional<std::string>& path, std::string& error)
{
	if (!source || source->is<picojson::null>()) {
		path.reset();
		return true;
	}
	if (source->is<std::string>()) {
		path = source->get<std::string>();
		return true;
	}
	if (source->is<picojson::object>()) {
		const auto* themeIcon = Find(source->get<picojson::object>(), "themeIcon");
		if (themeIcon && themeIcon->is<std::string>() && !themeIcon->get<std::string>().empty()) {
			path = "$(" + themeIcon->get<std::string>() + ")";
			return true;
		}
	}
	error = "SCM iconPath must be a string, URI, or ThemeIcon";
	return false;
}

bool ScmResource(
	const picojson::value& source,
	std::optional<workbench::scm::ScmResourceState>& resource,
	std::string& error)
{
	if (!source.is<picojson::object>()) {
		error = "SCM resource state must be an object";
		return false;
	}
	const auto& object = source.get<picojson::object>();
	const auto* uri = Find(object, "resourceUri");
	if (!uri || !uri->is<std::string>() || uri->get<std::string>().empty()) {
		error = "SCM resourceUri must be a non-empty string";
		return false;
	}
	const auto parsed = platform::uri::Uri::Parse(u8stowcs(uri->get<std::string>()));
	if (!parsed) {
		error = "SCM resourceUri is not a valid URI";
		return false;
	}
	resource.emplace(*parsed.value);
	if (const auto* command = Find(object, "command")) {
		workbench::scm::ScmCommand parsedCommand;
		if (!ScmCommand(command, parsedCommand, error)) return false;
		resource->command = std::move(parsedCommand);
	}
	if (const auto* context = Find(object, "contextValue")) {
		if (!context->is<std::string>()) {
			error = "SCM contextValue must be a string";
			return false;
		}
		resource->contextValue = context->get<std::string>();
	}
	const auto* decorations = Find(object, "decorations");
	if (decorations) {
		if (!decorations->is<picojson::object>()) {
			error = "SCM decorations must be an object";
			return false;
		}
		const auto& decoration = decorations->get<picojson::object>();
		if (const auto* strike = Find(decoration, "strikeThrough")) {
			if (!strike->is<bool>()) { error = "SCM strikeThrough must be a boolean"; return false; }
			resource->strikeThrough = strike->get<bool>();
		}
		if (const auto* faded = Find(decoration, "faded")) {
			if (!faded->is<bool>()) { error = "SCM faded must be a boolean"; return false; }
			resource->faded = faded->get<bool>();
		}
		if (const auto* tooltip = Find(decoration, "tooltip")) {
			if (!tooltip->is<std::string>()) { error = "SCM tooltip must be a string"; return false; }
			resource->tooltip = tooltip->get<std::string>();
		}
		std::optional<std::string> commonIcon;
		if (!ScmIconPath(Find(decoration, "iconPath"), commonIcon, error)) return false;
		if (const auto* light = Find(decoration, "light")) {
			if (!light->is<picojson::object>()) { error = "SCM light decoration must be an object"; return false; }
			if (!ScmIconPath(Find(light->get<picojson::object>(), "iconPath"), resource->lightIconPath, error)) return false;
		}
		if (const auto* dark = Find(decoration, "dark")) {
			if (!dark->is<picojson::object>()) { error = "SCM dark decoration must be an object"; return false; }
			if (!ScmIconPath(Find(dark->get<picojson::object>(), "iconPath"), resource->darkIconPath, error)) return false;
		}
		if (commonIcon) {
			if (!resource->lightIconPath) resource->lightIconPath = commonIcon;
			if (!resource->darkIconPath) resource->darkIconPath = commonIcon;
		}
	}
	return true;
}

bool ScmCount(const picojson::object& object, std::optional<std::int32_t>& count, std::string& error)
{
	const auto* source = Find(object, "count");
	if (!source || source->is<picojson::null>()) return true;
	if (!source->is<double>()) { error = "SCM count must be an integer"; return false; }
	const auto number = source->get<double>();
	if (!std::isfinite(number) || std::floor(number) != number ||
		number < static_cast<double>((std::numeric_limits<std::int32_t>::min)()) ||
		number > static_cast<double>((std::numeric_limits<std::int32_t>::max)())) {
		error = "SCM count is outside the supported integer range";
		return false;
	}
	count = static_cast<std::int32_t>(number);
	return true;
}

bool ScmCommands(const picojson::value* source, std::vector<workbench::scm::ScmCommand>& commands, std::string& error)
{
	if (!source || !source->is<picojson::array>()) { error = "SCM commands must be an array"; return false; }
	for (const auto& value : source->get<picojson::array>()) {
		workbench::scm::ScmCommand command;
		if (!ScmCommand(&value, command, error)) return false;
		commands.push_back(std::move(command));
	}
	return true;
}

std::string ScmFailureText(const workbench::scm::EScmOperationStatus status)
{
	using enum workbench::scm::EScmOperationStatus;
	switch (status) {
	case InvalidOwner: return "SCM owner is invalid or stale";
	case OwnerGenerationConflict: return "SCM owner generation conflict";
	case InvalidProvider: return "SCM provider is not registered";
	case InvalidGroup: return "SCM resource group is not registered";
	case InvalidResource: return "SCM resource state is invalid";
	case InvalidPayload: return "SCM payload is invalid";
	case OwnerLimitExceeded: return "SCM owner limit exceeded";
	case ProviderLimitExceeded: return "SCM provider limit exceeded";
	case GroupLimitExceeded: return "SCM resource-group limit exceeded";
	case ResourceLimitExceeded: return "SCM resource limit exceeded";
	case PayloadLimitExceeded: return "SCM payload limit exceeded";
	case RevisionExhausted: return "SCM revision space exhausted";
	case Stopped: return "SCM service is stopped";
	case Succeeded:
	case Replayed:
	case NotApplicable:
		return "";
	}
	return "SCM operation failed";
}

bool ParsePosition(const picojson::value* value, SExtensionTextPosition& position)
{
	if (!value || !value->is<picojson::object>()) return false;
	const auto& object = value->get<picojson::object>();
	return UInt32(object, "line", position.line) && UInt32(object, "character", position.character);
}

bool ParseRange(const picojson::value* value, SExtensionTextRange& range)
{
	if (!value || !value->is<picojson::object>()) return false;
	const auto& object = value->get<picojson::object>();
	return ParsePosition(Find(object, "start"), range.start) && ParsePosition(Find(object, "end"), range.end) &&
		range.start <= range.end;
}

picojson::value WideString(std::wstring_view value)
{
	return picojson::value(wcstou8s(std::wstring(value)));
}

std::string SecretFailure(const SExtensionSecretStorageResult& result)
{
	return "secret storage operation failed (status " + std::to_string(static_cast<int>(result.status)) +
		", error " + std::to_string(result.errorCode) + ")";
}

// The host-supplied failure message originates from whatever the extension threw (for example an
// uncaught TypeError's stack). Bound it before it is ever composed into a diagnostic string; the
// service bridge applies its own defensive bound too, but truncating here keeps the extension-supplied
// portion clearly separated from the surrounding attribution text this dispatcher adds.
constexpr std::size_t kMaximumActivationFailureMessageCodeUnits = 2000;

std::wstring BoundedActivationFailureMessage(std::wstring value)
{
	if (value.size() <= kMaximumActivationFailureMessageCodeUnits) return value;
	value.resize(kMaximumActivationFailureMessageCodeUnits);
	value += L"…[truncated]";
	return value;
}

// workspace/configuration/update ---------------------------------------------------------
//
// Real VS Code's ConfigurationTarget is Global = 1, Workspace = 2, WorkspaceFolder = 3, and
// its update() also accepts a plain boolean (true => Global, false => Workspace) in place of
// the enum. An absent target resolves to WorkspaceFolder when the call is resource-scoped,
// otherwise Workspace. This dispatcher only ever accepts Global: the workbench runtime has no
// safe accessor to its dynamically-assigned workspace/folder settings documents from this
// bridge (see extension/CLAUDE.md), so every non-Global outcome -- including both branches of
// the absent-target default, since neither is Global -- is rejected as an explicit typed
// UnsupportedCapability failure rather than silently accepted or silently dropped.
enum class EConfigurationUpdateTarget : std::uint8_t {
	Global,
	Unsupported,
	Malformed,
};

EConfigurationUpdateTarget ResolveConfigurationUpdateTarget(const picojson::value* target)
{
	if (!target || target->is<picojson::null>()) return EConfigurationUpdateTarget::Unsupported;
	if (target->is<bool>()) {
		return target->get<bool>() ? EConfigurationUpdateTarget::Global : EConfigurationUpdateTarget::Unsupported;
	}
	if (target->is<double>()) {
		const double number = target->get<double>();
		if (!std::isfinite(number) || std::floor(number) != number) return EConfigurationUpdateTarget::Malformed;
		if (number == 1.0) return EConfigurationUpdateTarget::Global;
		if (number == 2.0 || number == 3.0) return EConfigurationUpdateTarget::Unsupported;
		return EConfigurationUpdateTarget::Malformed;
	}
	return EConfigurationUpdateTarget::Malformed;
}

// The extension-supplied value tree is untrusted and otherwise unbounded (an extension can
// send an arbitrarily deep or wide JSON value). Bound both the node count and the nesting
// depth defensively before ever building a config::ConfigurationValue, reusing the same
// budgets CJsoncDocument enforces for a parsed settings document so this conversion can never
// admit a value the document editor would not have accepted anyway.
constexpr int kMaximumConfigurationValueDepth = 64;
constexpr std::size_t kMaximumConfigurationValueNodes = 65536;
constexpr std::size_t kMaximumConfigurationValueStringLength = 1024 * 1024;

bool ToConfigurationValue(
	const picojson::value& source, int depth, std::size_t& nodeBudget, config::ConfigurationValue& out)
{
	if (depth > kMaximumConfigurationValueDepth || nodeBudget == 0) return false;
	--nodeBudget;
	if (source.is<picojson::null>()) { out = config::ConfigurationValue(nullptr); return true; }
	if (source.is<bool>()) { out = config::ConfigurationValue(source.get<bool>()); return true; }
	if (source.is<double>()) {
		const double number = source.get<double>();
		if (!std::isfinite(number)) return false;
		if (std::floor(number) == number &&
			number >= static_cast<double>((std::numeric_limits<std::int64_t>::min)()) &&
			number <= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
			out = config::ConfigurationValue(static_cast<std::int64_t>(number));
		} else {
			out = config::ConfigurationValue(number);
		}
		return true;
	}
	if (source.is<std::string>()) {
		const auto& text = source.get<std::string>();
		if (text.size() > kMaximumConfigurationValueStringLength) return false;
		out = config::ConfigurationValue(u8stowcs(text));
		return true;
	}
	if (source.is<picojson::array>()) {
		config::ConfigurationValue::Array array;
		for (const auto& element : source.get<picojson::array>()) {
			config::ConfigurationValue converted;
			if (!ToConfigurationValue(element, depth + 1, nodeBudget, converted)) return false;
			array.push_back(std::move(converted));
		}
		out = config::ConfigurationValue(std::move(array));
		return true;
	}
	if (source.is<picojson::object>()) {
		config::ConfigurationValue::Object object;
		for (const auto& [key, value] : source.get<picojson::object>()) {
			if (key.size() > kMaximumConfigurationValueStringLength) return false;
			config::ConfigurationValue converted;
			if (!ToConfigurationValue(value, depth + 1, nodeBudget, converted)) return false;
			object.emplace(u8stowcs(key), std::move(converted));
		}
		out = config::ConfigurationValue(std::move(object));
		return true;
	}
	return false;
}

// The coordinator's diagnostic string is already documented as category-only (never a URI,
// path, key, or value), but this dispatcher still owns its own fixed, English, per-status RPC
// error message rather than forwarding it verbatim -- an extra margin against ever leaking
// something the coordinator's own contract did not intend to expose across the RPC boundary.
SExtensionWorkbenchDispatchResult ConfigurationWriteFailure(const config::SettingsWritebackResult& result)
{
	switch (result.status) {
	case config::ESettingsWritebackStatus::Conflict:
		return Failure("configuration update conflicted with a concurrent write and could not be applied", -32011);
	case config::ESettingsWritebackStatus::InvalidRequest:
		return Failure("configuration update request is invalid", -32602);
	case config::ESettingsWritebackStatus::EditRejected:
		return Failure("configuration document edit was rejected", -32011);
	case config::ESettingsWritebackStatus::ResnapshotRejected:
		return Failure("configuration update was written but could not be reloaded into the effective configuration model", -32011);
	case config::ESettingsWritebackStatus::Stopped:
		return Failure("workbench settings owner is not available", -32001);
	case config::ESettingsWritebackStatus::Applied:
	case config::ESettingsWritebackStatus::NoChange:
	case config::ESettingsWritebackStatus::Replayed:
	case config::ESettingsWritebackStatus::Failed:
	default:
		return Failure("configuration update failed", -32011);
	}
}

/*
	contributes の写し取り。

	方針は「1 項目の型崩れで拡張全体の登録を落とさない」。拡張ホスト側で既に形は
	検証済みなので、ここで型が違う値に出会うのは異常だが、そのとき正常な contribution
	まで巻き添えにすると、拡張が丸ごと使えなくなる。壊れた項目だけ落として先へ進む。
	その代わり件数には上限を置き、悪意ある拡張ホストがメモリを食い潰せないようにする。
*/
constexpr std::size_t kMaxContributionEntries = 4096;

std::vector<std::wstring> StringArray(const picojson::object& object, const char* key)
{
	std::vector<std::wstring> result;
	const auto* source = Find(object, key);
	if (!source || !source->is<picojson::array>()) return result;
	for (const auto& value : source->get<picojson::array>()) {
		if (!value.is<std::string>() || value.get<std::string>().empty()) continue;
		if (result.size() >= kMaxContributionEntries) break;
		result.push_back(u8stowcs(value.get<std::string>()));
	}
	return result;
}

/*!
	@brief `"navigation@1"` を group 名と並び順へ分解する

	`@` が無ければ順序は 0。`@` の後ろが数値でなければ、順序の指定が無かったものとして
	扱い、group 名だけを採る（`@` 以降を group 名に含めると、同じ group の項目が
	別グループに散らばる）。
*/
void SplitMenuGroup(const std::wstring& group, std::wstring& groupName, int& groupOrder)
{
	groupOrder = 0;
	const std::size_t at = group.rfind(L'@');
	if (at == std::wstring::npos) {
		groupName = group;
		return;
	}
	groupName = group.substr(0, at);
	const std::wstring order = group.substr(at + 1);
	if (order.empty() || order.size() > 9 ||
		order.find_first_not_of(L"0123456789") != std::wstring::npos) {
		return;
	}
	groupOrder = std::stoi(order);
}

/*!
	@brief マニフェスト由来のビュー宣言

	identity はレイアウトレジストリ、表示属性は contribution レジストリ、と行き先が
	分かれるので、パース結果もその 2 つに分けて返す。
*/
struct SParsedViewDeclarations {
	std::vector<SExtensionViewContainerDeclaration>	containers;
	std::vector<SExtensionViewDeclaration>			views;
};

/*
	レイアウトレジストリは 1 バッチ 128 件までしか受け取らない（WorkbenchContributionRegistry.cpp
	の kMaxBatchContributions）。ここで超過を切っておかないと、バッチ全体が
	BatchLimitExceeded で落ち、1 件も登録されない。
*/
constexpr std::size_t kMaxLayoutBatchContributions = 128;

SParsedViewDeclarations ParseViewDeclarations(
	const picojson::object& params, SExtensionContributions& contributions)
{
	SParsedViewDeclarations declarations;
	const auto batchFull = [&]() noexcept {
		return declarations.containers.size() + declarations.views.size() >= kMaxLayoutBatchContributions;
	};

	if (const auto* containers = Find(params, "viewsContainers"); containers && containers->is<picojson::array>()) {
		for (const auto& value : containers->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring id = OptionalString(object, "id");
			if (id.empty() || batchFull()) continue;
			const std::wstring location = OptionalString(object, "location");
			declarations.containers.push_back({
				.id = id,
				.title = OptionalString(object, "title"),
				.location = location == L"panel"
					? EExtensionViewContainerLocation::Panel : EExtensionViewContainerLocation::ActivityBar,
			});
			contributions.containerPresentations.push_back({
				.id = id,
				.iconPath = OptionalString(object, "icon"),
				.codicon = OptionalString(object, "codicon"),
			});
		}
	}

	if (const auto* views = Find(params, "views"); views && views->is<picojson::array>()) {
		for (const auto& value : views->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring id = OptionalString(object, "id");
			const std::wstring containerId = OptionalString(object, "containerId");
			if (id.empty() || containerId.empty() || batchFull()) continue;
			declarations.views.push_back({
				.id = id, .containerId = containerId, .title = OptionalString(object, "name") });
			contributions.viewPresentations.push_back({
				.id = id,
				.iconPath = OptionalString(object, "icon"),
				.codicon = OptionalString(object, "codicon"),
				.whenClause = OptionalString(object, "when"),
				.contextualTitle = OptionalString(object, "contextualTitle"),
				// `"type": "webview"` を落とすとツリーとして登録され、Claude Code の
				// サイドバーは永久に空のまま出る。既定はツリー（VS Code と同じ）。
				.kind = OptionalString(object, "type") == L"webview"
					? EExtensionViewKind::Webview : EExtensionViewKind::Tree,
			});
		}
	}

	return declarations;
}

SExtensionContributions ParseContributions(const picojson::object& params)
{
	SExtensionContributions contributions;

	if (const auto* menus = Find(params, "menus"); menus && menus->is<picojson::object>()) {
		for (const auto& [location, entries] : menus->get<picojson::object>()) {
			if (!entries.is<picojson::array>() || location.empty()) continue;
			const std::wstring locationName = u8stowcs(location);
			for (const auto& value : entries.get<picojson::array>()) {
				if (!value.is<picojson::object>()) continue;
				const auto& object = value.get<picojson::object>();
				const std::wstring commandId = OptionalString(object, "command");
				const std::wstring submenuId = OptionalString(object, "submenu");
				if ((commandId.empty() && submenuId.empty()) ||
					contributions.menuItems.size() >= kMaxContributionEntries) continue;
				SExtensionMenuItem item{
					.location = locationName,
					.commandId = commandId,
					.submenuId = submenuId,
					.altCommandId = OptionalString(object, "alt"),
					.whenClause = OptionalString(object, "when"),
				};
				SplitMenuGroup(OptionalString(object, "group"), item.groupName, item.groupOrder);
				contributions.menuItems.push_back(std::move(item));
			}
		}
	}

	if (const auto* submenus = Find(params, "submenus"); submenus && submenus->is<picojson::array>()) {
		for (const auto& value : submenus->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring id = OptionalString(object, "id");
			if (id.empty() || contributions.submenus.size() >= kMaxContributionEntries) continue;
			contributions.submenus.push_back({
				.id = id, .label = OptionalString(object, "label"), .iconPath = OptionalString(object, "icon") });
		}
	}

	if (const auto* keybindings = Find(params, "keybindings"); keybindings && keybindings->is<picojson::array>()) {
		for (const auto& value : keybindings->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring commandId = OptionalString(object, "command");
			const std::wstring keyChord = OptionalString(object, "key");
			if (commandId.empty() || keyChord.empty() ||
				contributions.keybindings.size() >= kMaxContributionEntries) continue;
			// args は任意の JSON。コマンド実行時にそのまま返送するので、解釈せず生のまま持つ。
			const auto* args = Find(object, "args");
			contributions.keybindings.push_back({
				.commandId = commandId,
				.keyChord = keyChord,
				.whenClause = OptionalString(object, "when"),
				.argumentsJson = args ? args->serialize() : std::string(),
			});
		}
	}

	if (const auto* languages = Find(params, "languages"); languages && languages->is<picojson::array>()) {
		for (const auto& value : languages->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring id = OptionalString(object, "id");
			if (id.empty() || contributions.languages.size() >= kMaxContributionEntries) continue;
			contributions.languages.push_back({
				.id = id,
				.aliases = StringArray(object, "aliases"),
				.extensions = StringArray(object, "extensions"),
				.filenames = StringArray(object, "filenames"),
				.filenamePatterns = StringArray(object, "filenamePatterns"),
				.mimetypes = StringArray(object, "mimetypes"),
				.firstLinePattern = OptionalString(object, "firstLine"),
				.configurationPath = OptionalString(object, "configuration"),
			});
		}
	}

	if (const auto* snippets = Find(params, "snippets"); snippets && snippets->is<picojson::array>()) {
		for (const auto& value : snippets->get<picojson::array>()) {
			if (!value.is<picojson::object>()) continue;
			const auto& object = value.get<picojson::object>();
			const std::wstring languageId = OptionalString(object, "language");
			const std::wstring path = OptionalString(object, "path");
			if (languageId.empty() || path.empty() ||
				contributions.snippets.size() >= kMaxContributionEntries) continue;
			contributions.snippets.push_back({ .languageId = languageId, .path = path });
		}
	}

	contributions.acknowledged = StringArray(params, "acknowledgedContributions");
	return contributions;
}

} // namespace

CExtensionWorkbenchDispatcher::CExtensionWorkbenchDispatcher(
	CExtensionContextKeys& contextKeys,
	CExtensionCommandPalette& commands,
	CExtensionStatusBar& statusBar,
	CExtensionNotificationCenter& notifications,
	CExtensionViewRegistry& views,
	IExtensionSecretStorage& secrets,
	CExtensionDiagnostics& diagnostics,
	CExtensionQuickInput& quickInput,
	CExtensionOutputChannel& output,
	CExtensionProgressCenter& progress,
	CExtensionWorkbenchServiceBridge* serviceBridge,
	CExtensionContributionRegistry* contributions)
	: m_contextKeys(contextKeys)
	, m_commands(commands)
	, m_statusBar(statusBar)
	, m_notifications(notifications)
	, m_views(views)
	, m_secrets(secrets)
	, m_diagnostics(diagnostics)
	, m_quickInput(quickInput)
	, m_output(output)
	, m_progress(progress)
	, m_serviceBridge(serviceBridge)
	, m_contributions(contributions)
{
}

void CExtensionWorkbenchDispatcher::SetQuickInputHandler(QuickInputHandler handler)
{
	m_quickInputHandler = std::move(handler);
}

void CExtensionWorkbenchDispatcher::SetNotificationHandler(NotificationHandler handler)
{
	m_notificationHandler = std::move(handler);
}

void CExtensionWorkbenchDispatcher::SetDeferredNotificationHandler(DeferredNotificationHandler handler)
{
	m_deferredNotificationHandler = std::move(handler);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::Dispatch(const SExtensionRpcMessage& message)
{
	if (message.eKind != EExtensionRpcMessageKind::Request &&
		message.eKind != EExtensionRpcMessageKind::Notification) return {};
	const auto method = std::string_view(message.sMethod);
	if (method == "workbench/extensions/register") return DispatchExtensionRegistration(message.sParamsJson);
	if (method == "workbench/extensions/removeGeneration") return DispatchRemoveGeneration(message.sParamsJson);
	if (method == "workbench/extensions/didActivate") return Success();
	if (method == "workbench/extensions/didFailActivation") return DispatchActivationFailure(message.sParamsJson);
	if (method == "workbench/commands/registerHandler" || method == "workbench/commands/unregisterHandler") {
		return DispatchCommandHandler(method, message.sParamsJson);
	}
	if (method == "workbench/context/set") return DispatchContextSet(message.sParamsJson);
	if (method == "workbench/commands/list") return DispatchCommandList(message.sParamsJson);
	if (method.starts_with("workbench/statusBar/")) return DispatchStatusBar(method, message.sParamsJson);
	if (method.starts_with("workbench/views/")) return DispatchView(method, message.sParamsJson);
	if (method.starts_with("workbench/scm/")) return DispatchScm(method, message.sParamsJson);
	if (message.eKind == EExtensionRpcMessageKind::Request && method.starts_with("secrets/")) {
		return DispatchSecret(method, message.sParamsJson);
	}
	if (method == "workbench/notification/show") return DispatchNotification(message);
	if (method.starts_with("languages/diagnostics/")) return DispatchDiagnostics(method, message.sParamsJson);
	if (method.starts_with("workbench/quickInput/")) return DispatchQuickInput(method, message.sParamsJson);
	if (method.starts_with("workbench/output/")) return DispatchOutput(method, message.sParamsJson);
	if (method.starts_with("workbench/progress/")) return DispatchProgress(method, message.sParamsJson);
	if (method.starts_with("workbench/languageStatus/")) return DispatchLanguageStatus(method, message.sParamsJson);
	if (method.starts_with("languages/provider/")) return DispatchCapabilityRegistration(method, message.sParamsJson);
	if (method == "workspace/configuration/update") return DispatchConfigurationUpdate(message.sParamsJson);
	if (method.starts_with("workbench/tasks/") || method.starts_with("workbench/terminal/") ||
		method.starts_with("workbench/webview/") || method.starts_with("window/editor/")) {
		return DispatchUnsupportedCapability(method, message.sParamsJson);
	}
	if (message.eKind == EExtensionRpcMessageKind::Notification) {
		return DispatchUnsupportedCapability(method, message.sParamsJson);
	}
	return {};
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchExtensionRegistration(std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) return Failure(error);
	const auto* commands = Find(params, "commands");
	if (commands && !commands->is<picojson::array>()) return Failure("commands must be an array");
	if (commands) {
		for (const auto& value : commands->get<picojson::array>()) {
			if (!value.is<picojson::object>()) return Failure("command contribution must be an object");
			const auto& object = value.get<picojson::object>();
			std::wstring id;
			if (!RequiredString(object, "id", id, error)) return Failure(error);
			const std::wstring title = OptionalString(object, "title");
			if (title.empty()) return Failure("command title must be a non-empty string");
			const auto existingState = m_commandStates.find(id);
			if (m_commands.Contains(id) && existingState == m_commandStates.end()) {
				return Failure("command contribution conflicts with an existing command", -32011);
			}
			if (!m_commands.Contains(id) && !m_commands.Register({
				.id = id,
				.title = title,
				.category = OptionalString(object, "category"),
				.enablementClause = OptionalString(object, "enablement"),
				.extensionId = extensionId,
				.generation = generation,
			})) return Failure("command contribution conflicts with an existing command", -32011);
			auto& state = m_commandStates[id];
			if (!state.extensionId.empty() && (state.extensionId != extensionId || state.generation != generation)) {
				return Failure("command contribution ownership conflict", -32011);
			}
			state.extensionId = extensionId;
			state.generation = generation;
			state.contributed = true;
		}
	}

	if (!m_contributions) return Success(EExtensionWorkbenchChange::Commands);

	SExtensionContributions contributions = ParseContributions(params);
	const SParsedViewDeclarations viewDeclarations = ParseViewDeclarations(params, contributions);
	const std::vector<std::wstring> acknowledged = contributions.acknowledged;
	m_contributions->Register({ .extensionId = extensionId, .generation = generation }, std::move(contributions));

	/*
		コンテナとビューの identity はワークベンチのレイアウトレジストリが唯一の出所。
		拒否されても拡張の登録自体は成功させる（コンテナが出ないだけに留め、
		拡張が丸ごと使えなくなることは避ける）。
	*/
	if (m_serviceBridge && !(viewDeclarations.containers.empty() && viewDeclarations.views.empty())) {
		(void)m_serviceBridge->RegisterViewContributions(
			extensionId, generation, viewDeclarations.containers, viewDeclarations.views);
	}

	/*
		「宣言は受理したが未実装」を Extension Host ログへ 1 行だけ残す。
		黙って受理すると欠落が見えなくなり、UnsupportedCapability を出すと
		正常な拡張が壊れているように見える。第 3 の扱いとして記録に留める。
	*/
	bool recorded = false;
	if (!acknowledged.empty() && m_serviceBridge) {
		std::wstring names;
		for (const auto& name : acknowledged) {
			if (!names.empty()) names += L", ";
			names += name;
		}
		recorded = m_serviceBridge->AppendExtensionHostLog(
			workbench::output::EOutputLogLevel::Info,
			L"Accepted but not yet implemented for " + extensionId + L": " + names);
	}
	return Success(EExtensionWorkbenchChange::Commands | EExtensionWorkbenchChange::Contributions |
		(recorded ? EExtensionWorkbenchChange::Output : EExtensionWorkbenchChange::None));
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchRemoveGeneration(std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) return Failure(error);
	if (m_serviceBridge && !m_serviceBridge->DisposeOwner(extensionId, generation, m_diagnostics, m_output)) {
		return Failure("workbench service owner disposal failed", -32012);
	}
	m_commands.RemoveOwnedBy(extensionId, generation);
	if (m_contributions) m_contributions->RemoveOwnedBy(extensionId, generation);
	m_contextKeys.RemoveOwnedBy(extensionId);
	m_statusBar.RemoveOwnedBy(extensionId, generation);
	m_views.RemoveOwnedBy(extensionId, generation);
	m_notifications.NotifyHostLost(extensionId, generation);
	m_diagnostics.RemoveOwnedBy(extensionId, generation);
	m_quickInput.RemoveOwnedBy(extensionId, generation, EExtensionQuickInputState::HostLost);
	m_output.RemoveOwnedBy(extensionId, generation);
	m_progress.RemoveOwnedBy(extensionId, generation);
	std::erase_if(m_commandStates, [&](const auto& entry) {
		return entry.second.extensionId == extensionId && entry.second.generation == generation;
	});
	std::erase_if(m_viewDescriptors, [&](const auto& entry) {
		return entry.second.extensionId == extensionId && entry.second.generation == generation;
	});
	const std::wstring unsupportedPrefix = extensionId + L"\n" + std::to_wstring(generation) + L"\n";
	std::erase_if(m_reportedUnsupportedCapabilities, [&](const auto& key) {
		return key.starts_with(unsupportedPrefix);
	});
	return Success(EExtensionWorkbenchChange::Commands | EExtensionWorkbenchChange::StatusBar |
		EExtensionWorkbenchChange::Views | EExtensionWorkbenchChange::Notifications |
		EExtensionWorkbenchChange::Diagnostics | EExtensionWorkbenchChange::Output |
		EExtensionWorkbenchChange::Progress | EExtensionWorkbenchChange::QuickInput |
		EExtensionWorkbenchChange::Scm | EExtensionWorkbenchChange::Contributions);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchActivationFailure(std::string_view paramsJson)
{
	// Real VS Code does not show a modal for a normal-mode activation failure; AbstractExtensionService
	// only surfaces one when running under the extension development host, and otherwise routes it to
	// the "Extension Host" log channel. Match that landing point instead of a TaskDialog: parse
	// leniently (this is host-originated telemetry about a failure, not a request whose ownership must
	// be exact) and always ack the RPC, so a malformed or partially-missing notification can never make
	// the extension host think this method failed.
	picojson::object params;
	std::string ignored;
	(void)ParseObject(paramsJson, params, ignored);
	std::wstring extensionId = OptionalString(params, "extensionId");
	if (extensionId.empty() || extensionId.size() > 255 || extensionId.find(L'\0') != std::wstring::npos) {
		extensionId = L"unknown-extension";
	}
	std::uint64_t generation = 1;
	std::string generationError;
	(void)Generation(params, generation, generationError);
	const std::wstring message = BoundedActivationFailureMessage(OptionalString(params, "message"));

	if (!m_serviceBridge) return Success();
	const std::wstring diagnostic = L"Activation failed for " + extensionId +
		L" (generation " + std::to_wstring(generation) + L"): " +
		(message.empty() ? L"(no message provided)" : message);
	const bool recorded = m_serviceBridge->AppendExtensionHostLog(workbench::output::EOutputLogLevel::Error, diagnostic);
	return Success(recorded ? EExtensionWorkbenchChange::Output : EExtensionWorkbenchChange::None);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchCommandHandler(
	std::string_view method, std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring command;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "command", command, error) ||
		!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) return Failure(error);
	if (method == "workbench/commands/registerHandler") {
		auto found = m_commandStates.find(command);
		if (found != m_commandStates.end() &&
			(found->second.extensionId != extensionId || found->second.generation != generation)) {
			return Failure("command handler ownership conflict", -32011);
		}
		if (m_commands.Contains(command) && found == m_commandStates.end()) {
			return Failure("command handler conflicts with an existing command", -32011);
		}
		if (!m_commands.Contains(command) && !m_commands.Register({
			.id = command, .title = command, .extensionId = extensionId, .generation = generation })) {
			return Failure("cannot register command handler", -32011);
		}
		auto& state = m_commandStates[command];
		state.extensionId = extensionId;
		state.generation = generation;
		state.hasHandler = true;
		return Success(EExtensionWorkbenchChange::Commands);
	}
	const auto found = m_commandStates.find(command);
	if (found == m_commandStates.end() || found->second.extensionId != extensionId || found->second.generation != generation) {
		return Failure("command handler is not owned by this extension generation", -32012);
	}
	found->second.hasHandler = false;
	if (!found->second.contributed) {
		m_commands.Unregister(command, extensionId);
		m_commandStates.erase(found);
	}
	return Success(EExtensionWorkbenchChange::Commands);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchContextSet(std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring key;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "key", key, error) || !RequiredString(params, "extensionId", extensionId, error) ||
		!Generation(params, generation, error)) return Failure(error);
	const auto* source = Find(params, "value");
	ExtensionContextValue value;
	if (!source || source->is<picojson::null>()) value = std::monostate{};
	else if (source->is<bool>()) value = source->get<bool>();
	else if (source->is<std::string>()) value = u8stowcs(source->get<std::string>());
	else if (source->is<double>()) {
		const double number = source->get<double>();
		if (std::isfinite(number) && std::floor(number) == number &&
			number >= static_cast<double>((std::numeric_limits<std::int64_t>::min)()) &&
			number <= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
			value = static_cast<std::int64_t>(number);
		} else {
			value = number;
		}
	} else return Failure("context value must be null, boolean, number, or string");
	if (!m_contextKeys.Set(std::move(key), std::move(value), std::move(extensionId))) return Failure("invalid context key");
	return Success(EExtensionWorkbenchChange::Commands);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchCommandList(std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	picojson::array commands;
	for (const auto& id : m_commands.CommandIds(OptionalBool(params, "filterInternal"))) commands.emplace_back(WideString(id));
	picojson::object result;
	result["commands"] = picojson::value(std::move(commands));
	return Success(EExtensionWorkbenchChange::None, picojson::value(std::move(result)).serialize());
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchStatusBar(
	std::string_view method, std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "handle", handle, error) ||
		!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) return Failure(error);
	if (method.ends_with("remove")) {
		if (!m_statusBar.Remove(handle, extensionId, generation)) return Failure("status bar item ownership mismatch", -32012);
		return Success(EExtensionWorkbenchChange::StatusBar);
	}
	const auto* priority = Find(params, "priority");
	constexpr char tooltipKey[] = { 't', 'o', 'o', 'l', 't', 'i', 'p', '\0' };
	SExtensionStatusBarItem item{
		.handle = std::move(handle),
		.itemId = OptionalString(params, "itemId"),
		.name = OptionalString(params, "name"),
		.extensionId = std::move(extensionId),
		.generation = generation,
		.alignment = OptionalString(params, "alignment") == L"right"
			? EExtensionStatusBarAlignment::Right : EExtensionStatusBarAlignment::Left,
		.priority = priority && priority->is<double>() ? priority->get<double>() : 0.0,
		.text = OptionalString(params, "text"),
		.tooltip = PresentationString(params, "tooltip"),
		.tooltipSupportsThemeIcons = PresentationSupportsThemeIcons(params, "tooltip"),
		.tooltipIsTrusted = PresentationIsTrusted(params, tooltipKey),
		.tooltipTrustedCommands = PresentationTrustedCommands(params, tooltipKey),
		.command = CommandString(params),
		.accessibilityLabel = PresentationString(params, "accessibilityInformation"),
		.visible = OptionalBool(params, "visible"),
	};
	if (!m_statusBar.Upsert(std::move(item))) return Failure("invalid or conflicting status bar item", -32011);
	return Success(EExtensionWorkbenchChange::StatusBar);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchView(
	std::string_view method, std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "handle", handle, error) ||
		!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) return Failure(error);
	if (method == "workbench/views/register") {
		std::wstring viewId;
		if (!RequiredString(params, "viewId", viewId, error)) return Failure(error);
		SExtensionViewDescriptor descriptor{
			.handle = handle,
			.viewId = std::move(viewId),
			.title = OptionalString(params, "title"),
			.extensionId = extensionId,
			.generation = generation,
			.canSelectMany = OptionalBool(params, "canSelectMany"),
			.showCollapseAll = OptionalBool(params, "showCollapseAll"),
		};
		if (descriptor.title.empty()) descriptor.title = descriptor.viewId;
		// VS Code renders a tree view inside the ViewContainer its manifest declared. Only a view
		// that was never declared falls back to the host's own bucket, so a contributed container
		// gets its views instead of rendering empty while they pile into Extensions.
		if (m_serviceBridge != nullptr) {
			if (auto container = m_serviceBridge->ViewContainerOf(descriptor.viewId); !container.empty()) {
				descriptor.containerId = std::move(container);
			}
		}
		if (!m_views.Register(descriptor)) return Failure("invalid or conflicting tree view", -32011);
		m_viewDescriptors.emplace(handle, std::move(descriptor));
		return Success(EExtensionWorkbenchChange::Views);
	}
	const auto found = m_viewDescriptors.find(handle);
	if (found == m_viewDescriptors.end() || found->second.extensionId != extensionId || found->second.generation != generation) {
		return Failure("tree view ownership mismatch", -32012);
	}
	if (method == "workbench/views/unregister") {
		if (!m_views.Unregister(handle, extensionId, generation)) return Failure("cannot unregister tree view", -32012);
		m_viewDescriptors.erase(found);
		return Success(EExtensionWorkbenchChange::Views);
	}
	if (method == "workbench/views/refresh") {
		if (!m_views.Invalidate(handle, OptionalString(params, "itemHandle"))) return Failure("cannot refresh tree view", -32012);
		return Success(EExtensionWorkbenchChange::Views);
	}
	if (method == "workbench/views/update") {
		auto descriptor = found->second;
		if (Find(params, "title")) descriptor.title = OptionalString(params, "title");
		if (Find(params, "description")) descriptor.description = OptionalString(params, "description");
		if (Find(params, "message")) descriptor.message = PresentationString(params, "message");
		if (const auto* badge = Find(params, "badge"); badge && badge->is<picojson::object>()) {
			const auto& object = badge->get<picojson::object>();
			if (const auto* value = Find(object, "value"); value && value->is<double>() &&
				std::isfinite(value->get<double>()) && value->get<double>() >= 0) {
				descriptor.badgeValue = static_cast<std::uint32_t>((std::min)(
					value->get<double>(), static_cast<double>((std::numeric_limits<std::uint32_t>::max)())));
			}
			descriptor.badgeTooltip = OptionalString(object, "tooltip");
		}
		if (!m_views.Update(descriptor)) return Failure("invalid tree view update", -32011);
		found->second = std::move(descriptor);
		return Success(EExtensionWorkbenchChange::Views);
	}
	if (method == "workbench/views/reveal") {
		std::wstring itemHandle;
		if (!RequiredString(params, "itemHandle", itemHandle, error)) return Failure(error);
		const bool accepted = !m_views.RevealPath(handle, itemHandle).empty();
		picojson::object result;
		result["accepted"] = picojson::value(accepted);
		return Success(EExtensionWorkbenchChange::Views, picojson::value(std::move(result)).serialize());
	}
	return {};
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchScm(
	std::string_view method, std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) {
		return Failure(error);
	}
	if (!m_serviceBridge) return Failure("SCM service is unavailable", -32001);
	auto* scm = m_serviceBridge->Scm();
	if (!scm) return Failure("SCM service is unavailable", -32001);
	const workbench::scm::ScmOwner owner{ .extensionId = wcstou8s(extensionId), .generation = generation };

	const auto complete = [&](const workbench::scm::ScmOperationResult& result, const bool allowNotApplicable = false) {
		if (result.Succeeded() || (allowNotApplicable && result.status == workbench::scm::EScmOperationStatus::NotApplicable)) {
			return Success(EExtensionWorkbenchChange::Scm);
		}
		return Failure(ScmFailureText(result.status), result.status == workbench::scm::EScmOperationStatus::Stopped ? -32001 : -32011);
	};

	if (method == "workbench/scm/provider/create") {
		std::wstring handle;
		std::wstring id;
		std::wstring label;
		if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "id", id, error) ||
			!RequiredString(params, "label", label, error)) return Failure(error);
		workbench::scm::ScmProviderState provider{
			.owner = owner,
			.handle = wcstou8s(handle),
			.id = wcstou8s(id),
			.label = wcstou8s(label),
		};
		if (const auto* root = Find(params, "rootUri"); root && !root->is<picojson::null>()) {
			if (!root->is<std::string>()) return Failure("rootUri must be a URI string");
			const auto parsed = platform::uri::Uri::Parse(u8stowcs(root->get<std::string>()));
			if (!parsed) return Failure("rootUri is not a valid URI");
			provider.rootUri = *parsed.value;
		}
		if (const auto* input = Find(params, "inputBox")) {
			if (!ScmInputBox(input, provider.inputBox, error)) return Failure(error);
		}
		if (!ScmCount(params, provider.count, error)) return Failure(error);
		if (const auto* commitTemplate = Find(params, "commitTemplate"); commitTemplate && !commitTemplate->is<picojson::null>()) {
			if (!commitTemplate->is<std::string>()) return Failure("commitTemplate must be a string");
			provider.commitTemplate = commitTemplate->get<std::string>();
		}
		if (const auto* accept = Find(params, "acceptInputCommand"); accept && !accept->is<picojson::null>()) {
			workbench::scm::ScmCommand command;
			if (!ScmCommand(accept, command, error)) return Failure(error);
			provider.acceptInputCommand = std::move(command);
		}
		if (const auto* status = Find(params, "statusBarCommands"); status && !status->is<picojson::null>()) {
			if (!ScmCommands(status, provider.statusBarCommands, error)) return Failure(error);
		}
		return complete(scm->CreateProvider({ .provider = std::move(provider) }));
	}

	if (method == "workbench/scm/provider/update") {
		std::wstring handle;
		if (!RequiredString(params, "handle", handle, error)) return Failure(error);
		workbench::scm::ScmUpdateProviderRequest request{ .owner = owner, .handle = wcstou8s(handle) };
		if (const auto* label = Find(params, "label")) {
			if (!label->is<std::string>()) return Failure("label must be a string");
			request.label = label->get<std::string>();
		}
		if (const auto* input = Find(params, "inputBox")) {
			workbench::scm::ScmInputBoxState state;
			if (!ScmInputBox(input, state, error)) return Failure(error);
			request.inputBox = std::move(state);
		}
		if (Find(params, "count")) {
			if (Find(params, "count")->is<picojson::null>()) request.clearCount = true;
			else if (!ScmCount(params, request.count, error)) return Failure(error);
		}
		if (const auto* commitTemplate = Find(params, "commitTemplate")) {
			if (commitTemplate->is<picojson::null>()) request.clearCommitTemplate = true;
			else if (!commitTemplate->is<std::string>()) return Failure("commitTemplate must be a string or null");
			else request.commitTemplate = commitTemplate->get<std::string>();
		}
		if (const auto* accept = Find(params, "acceptInputCommand")) {
			if (accept->is<picojson::null>()) request.clearAcceptInputCommand = true;
			else {
				workbench::scm::ScmCommand command;
				if (!ScmCommand(accept, command, error)) return Failure(error);
				request.acceptInputCommand = std::move(command);
			}
		}
		if (const auto* status = Find(params, "statusBarCommands")) {
			if (status->is<picojson::null>()) request.clearStatusBarCommands = true;
			else {
				request.statusBarCommands.emplace();
				if (!ScmCommands(status, *request.statusBarCommands, error)) return Failure(error);
			}
		}
		return complete(scm->UpdateProvider(request));
	}

	if (method == "workbench/scm/provider/dispose") {
		std::wstring handle;
		if (!RequiredString(params, "handle", handle, error)) return Failure(error);
		return complete(scm->DisposeProvider({ .owner = owner, .handle = wcstou8s(handle) }), true);
	}

	if (method == "workbench/scm/group/create") {
		std::wstring handle;
		std::wstring groupId;
		std::wstring label;
		if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "groupId", groupId, error) ||
			!RequiredString(params, "label", label, error)) return Failure(error);
		workbench::scm::ScmResourceGroupState group{
			.owner = owner,
			.providerHandle = wcstou8s(handle),
			.id = wcstou8s(groupId),
			.label = wcstou8s(label),
			.hideWhenEmpty = OptionalBool(params, "hideWhenEmpty"),
		};
		if (const auto* context = Find(params, "contextValue")) {
			if (!context->is<std::string>()) return Failure("contextValue must be a string");
			group.contextValue = context->get<std::string>();
		}
		return complete(scm->CreateGroup({ .group = std::move(group) }));
	}

	if (method == "workbench/scm/group/update") {
		std::wstring handle;
		std::wstring groupId;
		if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "groupId", groupId, error)) return Failure(error);
		workbench::scm::ScmUpdateGroupRequest request{
			.owner = owner,
			.providerHandle = wcstou8s(handle),
			.groupId = wcstou8s(groupId),
		};
		if (const auto* label = Find(params, "label")) {
			if (!label->is<std::string>()) return Failure("label must be a string");
			request.label = label->get<std::string>();
		}
		if (const auto* hide = Find(params, "hideWhenEmpty")) {
			if (!hide->is<bool>()) return Failure("hideWhenEmpty must be a boolean");
			request.hideWhenEmpty = hide->get<bool>();
		}
		if (const auto* context = Find(params, "contextValue")) {
			if (!context->is<std::string>()) return Failure("contextValue must be a string");
			request.contextValue = context->get<std::string>();
		}
		return complete(scm->UpdateGroup(request));
	}

	if (method == "workbench/scm/group/dispose") {
		std::wstring handle;
		std::wstring groupId;
		if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "groupId", groupId, error)) return Failure(error);
		return complete(scm->DisposeGroup({ .owner = owner, .providerHandle = wcstou8s(handle), .groupId = wcstou8s(groupId) }), true);
	}

	if (method == "workbench/scm/resources/replace") {
		std::wstring handle;
		std::wstring groupId;
		if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "groupId", groupId, error)) return Failure(error);
		const auto* resources = Find(params, "resources");
		if (!resources || !resources->is<picojson::array>()) return Failure("resources must be an array");
		workbench::scm::ScmReplaceResourcesRequest request{
			.owner = owner,
			.providerHandle = wcstou8s(handle),
			.groupId = wcstou8s(groupId),
		};
		request.resources.reserve(resources->get<picojson::array>().size());
		for (const auto& value : resources->get<picojson::array>()) {
			std::optional<workbench::scm::ScmResourceState> resource;
			if (!ScmResource(value, resource, error)) return Failure(error);
			if (!resource) return Failure("SCM resource state was not constructed", -32603);
			request.resources.push_back(std::move(*resource));
		}
		return complete(scm->ReplaceResources(request));
	}

	if (method == "workbench/scm/input/update") {
		std::wstring handle;
		if (!RequiredString(params, "handle", handle, error)) return Failure(error);
		workbench::scm::ScmInputBoxState input;
		if (!ScmInputBox(Find(params, "inputBox"), input, error)) return Failure(error);
		return complete(scm->UpdateInputBox({
			.owner = owner,
			.handle = wcstou8s(handle),
			.inputBox = std::move(input),
			.global = OptionalBool(params, "global"),
		}));
	}

	return {};
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchSecret(
	std::string_view method, std::string_view paramsJson)
{
	if (method == "secrets/keys") {
		return Failure("UnsupportedCapability: SecretStorage.keys is not available", -32601);
	}
	if (method != "secrets/get" && method != "secrets/store" && method != "secrets/delete") return {};
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	if (!RequiredString(params, "extensionId", extensionId, error)) return Failure(error);
	std::wstring key;
	if (!RequiredString(params, "key", key, error)) return Failure(error);
	if (method == "secrets/get") {
		const auto result = m_secrets.Get(extensionId, key);
		if (!result.success) return Failure(SecretFailure(result), -32020);
		picojson::object response;
		if (result.value) response["value"] = WideString(*result.value);
		return Success(EExtensionWorkbenchChange::None, picojson::value(std::move(response)).serialize());
	}
	if (method == "secrets/store") {
		std::wstring value;
		if (!RequiredString(params, "value", value, error) && OptionalString(params, "value").empty()) {
			const auto* raw = Find(params, "value");
			if (!raw || !raw->is<std::string>()) return Failure("value must be a string");
			value.clear();
		}
		const auto result = m_secrets.Store(extensionId, key, value);
		return result.success ? Success() : Failure(SecretFailure(result), -32020);
	}
	if (method == "secrets/delete") {
		const auto result = m_secrets.Delete(extensionId, key);
		return result.success ? Success() : Failure(SecretFailure(result), -32020);
	}
	return {};
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchNotification(
	const SExtensionRpcMessage& message)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(message.sParamsJson, params, error)) return Failure(error);
	SExtensionNotification notification;
	if (!RequiredString(params, "extensionId", notification.extensionId, error) ||
		!Generation(params, notification.generation, error) ||
		!RequiredString(params, "message", notification.message, error)) return Failure(error);
	notification.detail = OptionalString(params, "detail");
	notification.modal = OptionalBool(params, "modal");
	const auto severity = OptionalString(params, "severity");
	if (severity == L"warning") notification.severity = EExtensionNotificationSeverity::Warning;
	else if (severity == L"error") notification.severity = EExtensionNotificationSeverity::Error;
	if (const auto* actions = Find(params, "actions"); actions) {
		if (!actions->is<picojson::array>()) return Failure("actions must be an array");
		for (const auto& action : actions->get<picojson::array>()) {
			if (!action.is<picojson::object>()) return Failure("notification action must be an object");
			const auto title = OptionalString(action.get<picojson::object>(), "title");
			if (title.empty()) return Failure("notification action title must be non-empty");
			notification.actions.push_back(title);
		}
	}
	const auto id = m_notifications.Show(notification);
	if (!id) return Failure("notification queue is unavailable", -32030);
	notification.id = *id;
	if (!notification.modal) {
		if (m_deferredNotificationHandler && m_deferredNotificationHandler(notification, message)) {
			auto result = Success(EExtensionWorkbenchChange::Notifications);
			result.responseDeferred = message.eKind == EExtensionRpcMessageKind::Request;
			return result;
		}
		m_notifications.Resolve(*id, std::nullopt);
		(void)m_notifications.TakeCompletion(*id);
		return Failure("notification presentation is unavailable", -32030);
	}
	std::optional<std::size_t> selected;
	if (m_notificationHandler) selected = m_notificationHandler(notification);
	m_notifications.Resolve(*id, selected);
	(void)m_notifications.TakeCompletion(*id);
	picojson::object response;
	if (selected && *selected < notification.actions.size()) {
		response["selectedIndex"] = picojson::value(static_cast<double>(*selected));
	}
	return Success(EExtensionWorkbenchChange::Notifications, picojson::value(std::move(response)).serialize());
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchDiagnostics(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::wstring collection;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) ||
		!Generation(params, generation, error) || !RequiredString(params, "collection", collection, error)) return Failure(error);
	if (method.ends_with("clear")) {
		const bool accepted = m_serviceBridge
			? m_serviceBridge->ClearDiagnosticsCollection(extensionId, generation, collection, m_diagnostics)
			: (m_diagnostics.ClearCollection(extensionId, generation, collection), true);
		return accepted ? Success(EExtensionWorkbenchChange::Diagnostics) : Failure("invalid diagnostic collection", -32011);
	}
	std::wstring uri;
	if (!RequiredString(params, "uri", uri, error)) return Failure(error);
	if (method.ends_with("delete")) {
		const bool accepted = m_serviceBridge
			? m_serviceBridge->DeleteDiagnostics(extensionId, generation, collection, uri, m_diagnostics)
			: m_diagnostics.Delete(extensionId, generation, collection, uri);
		return accepted ? Success(EExtensionWorkbenchChange::Diagnostics) : Failure("invalid diagnostic collection", -32011);
	}
	const auto* values = Find(params, "diagnostics");
	if (!values || !values->is<picojson::array>()) return Failure("diagnostics must be an array");
	std::vector<SExtensionDiagnostic> diagnostics;
	diagnostics.reserve(values->get<picojson::array>().size());
	for (const auto& value : values->get<picojson::array>()) {
		if (!value.is<picojson::object>()) return Failure("diagnostic must be an object");
		const auto& object = value.get<picojson::object>();
		SExtensionDiagnostic diagnostic;
		if (!ParseRange(Find(object, "range"), diagnostic.range) ||
			!RequiredString(object, "message", diagnostic.message, error)) return Failure("diagnostic range or message is invalid");
		if (const auto* severity = Find(object, "severity"); severity && severity->is<double>() &&
			std::isfinite(severity->get<double>())) {
			const auto number = static_cast<int>(severity->get<double>());
			if (number >= 0 && number <= 3) diagnostic.severity = static_cast<EExtensionDiagnosticSeverity>(number);
		}
		diagnostic.source = OptionalString(object, "source");
		if (const auto* code = Find(object, "code"); code) {
			if (code->is<std::string>()) diagnostic.code = u8stowcs(code->get<std::string>());
			else if (code->is<double>() && std::isfinite(code->get<double>())) diagnostic.code = std::to_wstring(code->get<double>());
		}
		diagnostics.emplace_back(std::move(diagnostic));
	}
	const bool accepted = m_serviceBridge
		? m_serviceBridge->SetDiagnostics(extensionId, generation, collection, uri, diagnostics, m_diagnostics)
		: m_diagnostics.Set(std::move(extensionId), generation, std::move(collection), std::move(uri), std::move(diagnostics));
	if (!accepted) {
		return Failure("invalid diagnostic collection", -32011);
	}
	return Success(EExtensionWorkbenchChange::Diagnostics);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchQuickInput(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	SExtensionQuickInputRequest request;
	if (!RequiredString(params, "extensionId", request.extensionId, error) ||
		!Generation(params, request.generation, error)) return Failure(error);
	const auto* optionsValue = Find(params, "options");
	const picojson::object empty;
	const auto& options = optionsValue && optionsValue->is<picojson::object>() ? optionsValue->get<picojson::object>() : empty;
	request.title = OptionalString(options, "title");
	request.placeholder = OptionalString(options, "placeHolder");
	request.value = OptionalString(options, "value");
	request.canPickMany = OptionalBool(options, "canPickMany");
	request.password = OptionalBool(options, "password");
	if (method.ends_with("showQuickPick")) {
		request.kind = EExtensionQuickInputKind::QuickPick;
		const auto* items = Find(params, "items");
		if (!items || !items->is<picojson::array>()) return Failure("quick pick items must be an array");
		for (const auto& value : items->get<picojson::array>()) {
			if (!value.is<picojson::object>()) return Failure("quick pick item must be an object");
			const auto& object = value.get<picojson::object>();
			std::wstring label;
			if (!RequiredString(object, "label", label, error)) return Failure(error);
			std::uint32_t sourceIndex = static_cast<std::uint32_t>(request.items.size());
			(void)UInt32(object, "index", sourceIndex);
			request.items.push_back({
				.sourceIndex = sourceIndex,
				.label = std::move(label),
				.description = OptionalString(object, "description"),
				.detail = OptionalString(object, "detail"),
				.picked = OptionalBool(object, "picked"),
			});
		}
	} else if (method.ends_with("showInputBox")) {
		request.kind = EExtensionQuickInputKind::InputBox;
	} else {
		return {};
	}
	const auto id = m_quickInput.Show(std::move(request));
	if (!id) return Failure("quick input queue limit exceeded", -32030);
	const auto pending = m_quickInput.Pending();
	const auto found = std::ranges::find(pending, *id, &SExtensionQuickInputRequest::id);
	if (found == pending.end()) return Failure("quick input request was lost", -32603);
	const auto completion = m_quickInputHandler
		? m_quickInputHandler(*found)
		: SExtensionQuickInputCompletion{ .id = *id, .state = EExtensionQuickInputState::Cancelled };
	if (completion.state == EExtensionQuickInputState::Accepted) {
		if (!m_quickInput.Resolve(*id, completion.selectedIndices, completion.value)) {
			(void)m_quickInput.Cancel(*id);
		}
	} else {
		(void)m_quickInput.Cancel(*id, completion.state);
	}
	const auto resolved = m_quickInput.TakeCompletion(*id);
	if (!resolved) return Failure("quick input did not reach a terminal state", -32603);
	picojson::object response;
	if (resolved->state == EExtensionQuickInputState::Accepted) {
		if (found->kind == EExtensionQuickInputKind::InputBox && resolved->value) {
			response["value"] = WideString(*resolved->value);
		} else if (found->canPickMany) {
			picojson::array selected;
			for (const auto index : resolved->selectedIndices) {
				selected.emplace_back(static_cast<double>(found->items[index].sourceIndex));
			}
			response["selectedIndices"] = picojson::value(std::move(selected));
		} else if (!resolved->selectedIndices.empty()) {
			response["selectedIndex"] = picojson::value(static_cast<double>(found->items[resolved->selectedIndices.front()].sourceIndex));
		}
	}
	return Success(EExtensionWorkbenchChange::QuickInput, picojson::value(std::move(response)).serialize());
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchOutput(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "extensionId", extensionId, error) ||
		!Generation(params, generation, error)) return Failure(error);
	std::string operationId;
	const bool useOutputService = m_serviceBridge && m_serviceBridge->HasOutputService();
	if (useOutputService && !RequiredOutputOperationId(params, operationId, error)) return Failure(error);
	bool accepted = false;
	if (method.ends_with("create")) {
		std::wstring name;
		if (!RequiredString(params, "name", name, error)) return Failure(error);
		workbench::output::EOutputChannelKind kind = workbench::output::EOutputChannelKind::Output;
		if (const auto* rawKind = Find(params, "kind"); rawKind) {
			if (!rawKind->is<std::string>()) return Failure("output kind must be a string");
			if (rawKind->get<std::string>() == "log") kind = workbench::output::EOutputChannelKind::Log;
			else if (rawKind->get<std::string>() != "output") return Failure("output kind is invalid");
		}
		accepted = m_serviceBridge
			? m_serviceBridge->CreateOutput(handle, extensionId, generation, name, OptionalString(params, "languageId"),
				OptionalString(params, "source"), kind, operationId, m_output)
			: m_output.Create({ .handle = std::move(handle), .extensionId = std::move(extensionId), .generation = generation,
				.name = std::move(name), .languageId = OptionalString(params, "languageId") });
	} else if (method.ends_with("append") || method.ends_with("replace")) {
		const auto* raw = Find(params, "value");
		if (!raw || !raw->is<std::string>()) return Failure("output value must be a string");
		auto value = u8stowcs(raw->get<std::string>());
		accepted = m_serviceBridge
			? (method.ends_with("append")
				? m_serviceBridge->AppendOutput(handle, extensionId, generation, value, operationId, m_output)
				: m_serviceBridge->ReplaceOutput(handle, extensionId, generation, value, operationId, m_output))
			: (method.ends_with("append") ? m_output.Append(handle, extensionId, generation, value)
				: m_output.Replace(handle, extensionId, generation, std::move(value)));
	} else if (method.ends_with("clear")) accepted = m_serviceBridge
		? m_serviceBridge->ClearOutput(handle, extensionId, generation, operationId, m_output)
		: m_output.Clear(handle, extensionId, generation);
	else if (method.ends_with("show")) accepted = m_serviceBridge
		? m_serviceBridge->ShowOutput(handle, extensionId, generation, OptionalBool(params, "preserveFocus"), operationId, m_output)
		: m_output.SetVisible(handle, extensionId, generation, true);
	else if (method.ends_with("hide")) accepted = m_serviceBridge
		? m_serviceBridge->HideOutput(handle, extensionId, generation, operationId, m_output)
		: m_output.SetVisible(handle, extensionId, generation, false);
	else if (method.ends_with("dispose")) accepted = m_serviceBridge
		? m_serviceBridge->DisposeOutput(handle, extensionId, generation, operationId, m_output)
		: m_output.Dispose(handle, extensionId, generation);
	else return {};
	return accepted ? Success(EExtensionWorkbenchChange::Output) : Failure("output channel ownership mismatch", -32012);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchProgress(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "handle", handle, error) || !RequiredString(params, "extensionId", extensionId, error) ||
		!Generation(params, generation, error)) return Failure(error);
	bool accepted = false;
	if (method.ends_with("start")) {
		const auto* optionsValue = Find(params, "options");
		const picojson::object empty;
		const auto& options = optionsValue && optionsValue->is<picojson::object>() ? optionsValue->get<picojson::object>() : empty;
		accepted = m_progress.Start({
			.handle = std::move(handle), .extensionId = std::move(extensionId), .generation = generation,
			.title = OptionalString(options, "title"), .cancellable = OptionalBool(options, "cancellable"),
		});
	} else if (method.ends_with("report")) {
		const auto* reportValue = Find(params, "value");
		const picojson::object empty;
		const auto& report = reportValue && reportValue->is<picojson::object>() ? reportValue->get<picojson::object>() : empty;
		const auto* increment = Find(report, "increment");
		accepted = m_progress.Report(handle, extensionId, generation, OptionalString(report, "message"),
			increment && increment->is<double>() ? increment->get<double>() : 0.0);
	} else if (method.ends_with("end")) {
		accepted = m_progress.End(handle, extensionId, generation);
	} else return {};
	return accepted ? Success(EExtensionWorkbenchChange::Progress) : Failure("progress ownership mismatch", -32012);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchLanguageStatus(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::wstring id;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) ||
		!Generation(params, generation, error) || !RequiredString(params, "id", id, error)) return Failure(error);
	const std::wstring handle = L"languageStatus:" + extensionId + L":" + std::to_wstring(generation) + L":" + id;
	if (method.ends_with("remove")) {
		if (!m_statusBar.Remove(handle, extensionId, generation)) {
			return Failure("language status item ownership mismatch", -32012);
		}
		return Success(EExtensionWorkbenchChange::StatusBar);
	}
	if (!method.ends_with("update")) return {};
	const auto* severityValue = Find(params, "severity");
	const int severity = severityValue && severityValue->is<double>() && std::isfinite(severityValue->get<double>())
		? static_cast<int>(severityValue->get<double>()) : 0;
	std::wstring text = OptionalString(params, "text");
	if (OptionalBool(params, "busy")) text.insert(0, L"$(sync~spin) ");
	SExtensionStatusBarItem item{
		.handle = handle,
		.itemId = std::move(id),
		.extensionId = std::move(extensionId),
		.generation = generation,
		.alignment = EExtensionStatusBarAlignment::Right,
		.priority = 100.0 + static_cast<double>((std::clamp)(severity, 0, 2)),
		.text = std::move(text),
		.tooltip = OptionalString(params, "detail"),
		.command = CommandString(params),
		.accessibilityLabel = PresentationString(params, "accessibilityInformation"),
		.visible = true,
	};
	if (!m_statusBar.Upsert(std::move(item))) return Failure("invalid or conflicting language status item", -32011);
	return Success(EExtensionWorkbenchChange::StatusBar);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchCapabilityRegistration(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);
	std::wstring extensionId;
	std::uint64_t generation = 0;
	if (!RequiredString(params, "extensionId", extensionId, error) || !Generation(params, generation, error)) {
		return Failure(error);
	}
	std::wstring handle;
	if (!RequiredString(params, "handle", handle, error)) return Failure(error);
	if (!method.ends_with("register") && !method.ends_with("unregister")) return {};
	// Providers execute in the shared Node host. Native records the registration as an
	// explicitly accepted routing capability; provider invocation remains request based.
	return Success();
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchUnsupportedCapability(
	std::string_view method,
	std::string_view paramsJson)
{
	picojson::object params;
	std::string ignored;
	(void)ParseObject(paramsJson, params, ignored);
	std::wstring extensionId = OptionalString(params, "extensionId");
	if (extensionId.empty() || extensionId.size() > 255 || extensionId.find(L'\0') != std::wstring::npos) {
		extensionId = L"unknown-extension";
	}
	std::uint64_t generation = 1;
	std::string generationError;
	(void)Generation(params, generation, generationError);
	std::wstring capability;
	if (const auto* errorValue = Find(params, "error"); errorValue && errorValue->is<picojson::object>()) {
		capability = OptionalString(errorValue->get<picojson::object>(), "capability");
	}
	if (capability.empty()) {
		if (method.starts_with("workbench/tasks/")) capability = L"tasks.registerTaskProvider";
		else if (method.starts_with("workbench/terminal/")) capability = L"window.registerTerminalProfileProvider";
		else capability = u8stowcs(std::string(method));
	}
	if (method.ends_with("unregisterProvider") || method.ends_with("unregisterUnsupported") ||
		method.ends_with("removeDecorationType")) return Success();
	const std::wstring reportKey = extensionId + L"\n" + std::to_wstring(generation) + L"\n" + capability;
	if (!m_reportedUnsupportedCapabilities.emplace(reportKey).second) return Success();
	const std::wstring handle = L"compatibility:" + extensionId + L":" + std::to_wstring(generation);
	if (!m_output.Create({
		.handle = handle,
		.extensionId = extensionId,
		.generation = generation,
		.name = L"Extension Compatibility",
		.visible = true,
	})) return Failure("cannot create extension compatibility output", -32040);
	const std::wstring diagnostic = L"UnsupportedCapability: " + extensionId + L" requires " + capability + L"\r\n";
	if (!m_output.Append(handle, extensionId, generation, diagnostic) ||
		!m_output.SetVisible(handle, extensionId, generation, true)) {
		return Failure("cannot append extension compatibility output", -32040);
	}
	return Success(EExtensionWorkbenchChange::Output);
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::DispatchConfigurationUpdate(std::string_view paramsJson)
{
	picojson::object params;
	std::string error;
	if (!ParseObject(paramsJson, params, error)) return Failure(error);

	const auto* keyValue = Find(params, "key");
	if (!keyValue || !keyValue->is<std::string>()) return Failure("key must be a non-empty string");
	const std::string& key = keyValue->get<std::string>();
	if (key.empty() || key.size() > platform::serialization::CJsoncDocument::kMaximumObjectKeyLength) {
		return Failure("key must be a non-empty bounded string");
	}

	const auto targetKind = ResolveConfigurationUpdateTarget(Find(params, "configurationTarget"));
	if (targetKind == EConfigurationUpdateTarget::Malformed) {
		return Failure("configurationTarget must be a ConfigurationTarget value or boolean");
	}
	if (targetKind == EConfigurationUpdateTarget::Unsupported) {
		return Failure(
			"UnsupportedCapability: workspace and workspace-folder configuration targets are not yet supported",
			-32601);
	}

	if (!m_serviceBridge || !m_serviceBridge->HasWorkbenchRuntime()) {
		return Failure("workbench settings owner is not available", -32001);
	}

	std::optional<config::ConfigurationValue> value;
	if (const auto* rawValue = Find(params, "value")) {
		config::ConfigurationValue converted;
		std::size_t nodeBudget = kMaximumConfigurationValueNodes;
		if (!ToConfigurationValue(*rawValue, 0, nodeBudget, converted)) {
			return Failure("value is malformed or exceeds the configuration value bounds");
		}
		value = std::move(converted);
	}

	const std::wstring overrideLanguageId = OptionalString(params, "overrideInLanguage");

	const auto result = m_serviceBridge->WriteGlobalConfiguration(key, value, overrideLanguageId);
	if (!result.Succeeded()) return ConfigurationWriteFailure(result);
	return Success();
}

SExtensionWorkbenchDispatchResult CExtensionWorkbenchDispatcher::ApplyTreeChildrenResult(
	std::wstring_view viewHandle,
	std::wstring_view parentHandle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::string_view resultJson)
{
	picojson::object result;
	std::string error;
	if (!ParseObject(resultJson, result, error)) return Failure(error);
	const auto* items = Find(result, "items");
	if (!items || !items->is<picojson::array>()) return Failure("tree children result must contain an items array");
	std::vector<SExtensionTreeItem> children;
	children.reserve(items->get<picojson::array>().size());
	for (const auto& value : items->get<picojson::array>()) {
		if (!value.is<picojson::object>()) return Failure("tree item must be an object");
		const auto& object = value.get<picojson::object>();
		SExtensionTreeItem item;
		if (!RequiredString(object, "handle", item.handle, error) ||
			!RequiredString(object, "label", item.label, error)) return Failure(error);
		item.viewHandle = viewHandle;
		item.parentHandle = parentHandle;
		item.stableId = OptionalString(object, "id");
		item.description = OptionalString(object, "description");
		item.tooltip = PresentationString(object, "tooltip");
		item.contextValue = OptionalString(object, "contextValue");
		item.command = CommandString(object);
		if (const auto* command = Find(object, "command"); command && command->is<picojson::object>()) {
			const auto* arguments = Find(command->get<picojson::object>(), "arguments");
			if (arguments && arguments->is<picojson::array>()) item.commandArgumentsJson = arguments->serialize();
		}
		if (const auto* state = Find(object, "collapsibleState"); state && state->is<double>()) {
			if (state->get<double>() == 1) item.collapsibleState = EExtensionTreeItemCollapsibleState::Collapsed;
			else if (state->get<double>() == 2) item.collapsibleState = EExtensionTreeItemCollapsibleState::Expanded;
		}
		if (const auto* checkbox = Find(object, "checkboxState"); checkbox) {
			if (checkbox->is<bool>()) item.checkboxState = checkbox->get<bool>();
			else if (checkbox->is<double>()) item.checkboxState = checkbox->get<double>() != 0;
		}
		children.emplace_back(std::move(item));
	}
	if (!m_views.ReplaceChildren(viewHandle, parentHandle, extensionId, generation, std::move(children))) {
		return Failure("tree children ownership or shape mismatch", -32012);
	}
	return Success(EExtensionWorkbenchChange::Views);
}
