/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/TaskConfigurationCatalog.h"

#include "platform/uri/UriIdentity.h"

#include <algorithm>
#include <set>
#include <utility>

namespace workbench::tasks {
namespace {

constexpr std::size_t kMaximumTasks = 256U;
constexpr std::size_t kMaximumArguments = 64U;
constexpr std::size_t kMaximumDependencies = 64U;
constexpr std::size_t kMaximumProblemMatchers = 64U;
constexpr std::size_t kMaximumLabelLength = 256U;
constexpr std::size_t kMaximumTypeLength = 256U;
constexpr std::size_t kMaximumValueLength = 4096U;

TaskConfigurationCatalogResult Failure(ETaskConfigurationCatalogStatus status, const char* diagnostic)
{
	return { status, diagnostic };
}

bool IsBoundedText(std::wstring_view value, std::size_t maximum = kMaximumValueLength) noexcept
{
	return !value.empty() && value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](wchar_t character) { return character <= 0x1f || character == 0x7f; });
}

const std::wstring* StringValue(const platform::serialization::JsoncValue& value) noexcept
{
	return std::get_if<std::wstring>(&value.Value());
}

const platform::serialization::JsoncValue::Object* ObjectValue(const platform::serialization::JsoncValue& value) noexcept
{
	return std::get_if<platform::serialization::JsoncValue::Object>(&value.Value());
}

const platform::serialization::JsoncValue::Array* ArrayValue(const platform::serialization::JsoncValue& value) noexcept
{
	return std::get_if<platform::serialization::JsoncValue::Array>(&value.Value());
}

bool ReadStringArray(const platform::serialization::JsoncValue& value, std::size_t maximum, std::vector<std::wstring>& result)
{
	const auto* entries = ArrayValue(value);
	if (!entries || entries->size() > maximum) return false;
	result.clear();
	result.reserve(entries->size());
	for (const auto& entry : *entries) {
		const auto* string = StringValue(entry);
		if (!string || !IsBoundedText(*string)) return false;
		result.push_back(*string);
	}
	return true;
}

bool ReadStringOrArray(const platform::serialization::JsoncValue& value, std::size_t maximum, std::vector<std::wstring>& result)
{
	if (const auto* string = StringValue(value)) {
		if (!IsBoundedText(*string)) return false;
		result.assign(1U, *string);
		return true;
	}
	return ReadStringArray(value, maximum, result);
}

bool ReadOptionalString(
	const platform::serialization::JsoncValue::Object& object, const wchar_t* member, std::size_t maximum,
	std::optional<std::wstring>& result)
{
	const auto found = object.find(member);
	if (found == object.end()) return true;
	const auto* value = StringValue(found->second);
	if (!value || !IsBoundedText(*value, maximum)) return false;
	result = *value;
	return true;
}

bool ReadOptionalBool(
	const platform::serialization::JsoncValue::Object& object, const wchar_t* member, std::optional<bool>& result)
{
	const auto found = object.find(member);
	if (found == object.end()) return true;
	const auto* value = std::get_if<bool>(&found->second.Value());
	if (!value) return false;
	result = *value;
	return true;
}

bool ReadGroup(const platform::serialization::JsoncValue& value, std::optional<TaskGroupMetadata>& result)
{
	if (const auto* string = StringValue(value)) {
		if (!IsBoundedText(*string, kMaximumLabelLength)) return false;
		result = TaskGroupMetadata { *string, false };
		return true;
	}
	const auto* object = ObjectValue(value);
	if (!object) return false;
	const auto kind = object->find(L"kind");
	if (kind == object->end()) return false;
	const auto* groupId = StringValue(kind->second);
	if (!groupId || !IsBoundedText(*groupId, kMaximumLabelLength)) return false;
	TaskGroupMetadata group { *groupId, false };
	if (const auto defaultValue = object->find(L"isDefault"); defaultValue != object->end()) {
		const auto* isDefault = std::get_if<bool>(&defaultValue->second.Value());
		if (!isDefault) return false;
		group.isDefault = *isDefault;
	}
	result = std::move(group);
	return true;
}

bool ReadPresentation(const platform::serialization::JsoncValue& value, std::optional<TaskPresentationMetadata>& result)
{
	const auto* object = ObjectValue(value);
	if (!object) return false;
	TaskPresentationMetadata presentation;
	if (!ReadOptionalString(*object, L"reveal", kMaximumLabelLength, presentation.reveal)
		|| !ReadOptionalString(*object, L"panel", kMaximumLabelLength, presentation.panel)
		|| !ReadOptionalBool(*object, L"focus", presentation.focus)
		|| !ReadOptionalBool(*object, L"clear", presentation.clear)
		|| !ReadOptionalBool(*object, L"close", presentation.close)) return false;
	result = std::move(presentation);
	return true;
}

TaskConfigurationCatalogResult ParseDefinition(
	const platform::serialization::JsoncValue& value, const workspace::WorkspaceArtifactDocument& document,
	TaskConfigurationDefinition& definition)
{
	const auto* object = ObjectValue(value);
	if (!object) return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task entry must be an object");
	const auto label = object->find(L"label");
	if (label == object->end()) return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task label is required");
	const auto* labelValue = StringValue(label->second);
	if (!labelValue || !IsBoundedText(*labelValue, kMaximumLabelLength)) {
		return Failure(ETaskConfigurationCatalogStatus::EntryTooLarge, "task label is invalid or too large");
	}
	definition.label = *labelValue;
	definition.sourceUri = document.resource;
	definition.generation = document.generation;
	definition.revision = document.revision;

	std::wstring type = L"shell";
	if (const auto typeMember = object->find(L"type"); typeMember != object->end()) {
		const auto* typeValue = StringValue(typeMember->second);
		if (!typeValue || !IsBoundedText(*typeValue, kMaximumTypeLength)) {
			return Failure(ETaskConfigurationCatalogStatus::EntryTooLarge, "task type is invalid or too large");
		}
		type = *typeValue;
	}
	if (type == L"shell") definition.executionKind = ETaskExecutionKind::Shell;
	else if (type == L"process") definition.executionKind = ETaskExecutionKind::Process;
	else {
		definition.executionKind = ETaskExecutionKind::Custom;
		definition.customType = std::move(type);
		definition.unsupportedCapabilities = definition.unsupportedCapabilities | ETaskUnsupportedCapability::CustomExecution;
	}

	if (const auto command = object->find(L"command"); command != object->end()) {
		const auto* commandValue = StringValue(command->second);
		if (!commandValue || !IsBoundedText(*commandValue)) return Failure(ETaskConfigurationCatalogStatus::EntryTooLarge, "task command is invalid or too large");
		definition.command = *commandValue;
	}
	if (definition.executionKind != ETaskExecutionKind::Custom && definition.command.empty()) {
		return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "shell and process tasks require a command");
	}
	if (const auto arguments = object->find(L"args"); arguments != object->end()
		&& !ReadStringArray(arguments->second, kMaximumArguments, definition.arguments)) {
		return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task arguments must be a bounded string array");
	}
	if (const auto options = object->find(L"options"); options != object->end()) {
		const auto* optionObject = ObjectValue(options->second);
		if (!optionObject || !ReadOptionalString(*optionObject, L"cwd", kMaximumValueLength, definition.workingDirectory)) {
			return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task options.cwd must be a bounded string");
		}
	}
	if (const auto group = object->find(L"group"); group != object->end() && !ReadGroup(group->second, definition.group)) {
		return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task group is invalid");
	}
	if (const auto presentation = object->find(L"presentation"); presentation != object->end()
		&& !ReadPresentation(presentation->second, definition.presentation)) {
		return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task presentation is invalid");
	}
	if (const auto dependencies = object->find(L"dependsOn"); dependencies != object->end()) {
		if (!ReadStringOrArray(dependencies->second, kMaximumDependencies, definition.dependencies)) {
			return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task dependencies must be bounded strings");
		}
		definition.unsupportedCapabilities = definition.unsupportedCapabilities | ETaskUnsupportedCapability::Dependencies;
	}
	if (!ReadOptionalString(*object, L"dependsOrder", kMaximumLabelLength, definition.dependencyOrder)) {
		return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task dependency order is invalid");
	}
	if (definition.dependencyOrder) definition.unsupportedCapabilities = definition.unsupportedCapabilities | ETaskUnsupportedCapability::Dependencies;
	if (const auto background = object->find(L"isBackground"); background != object->end()) {
		const auto* backgroundValue = std::get_if<bool>(&background->second.Value());
		if (!backgroundValue) return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task background flag must be boolean");
		definition.isBackground = *backgroundValue;
		if (*backgroundValue) definition.unsupportedCapabilities = definition.unsupportedCapabilities | ETaskUnsupportedCapability::Background;
	}
	if (const auto matcher = object->find(L"problemMatcher"); matcher != object->end()) {
		if (!ReadStringOrArray(matcher->second, kMaximumProblemMatchers, definition.problemMatchers)) {
			return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "task problem matcher must be bounded strings");
		}
		definition.unsupportedCapabilities = definition.unsupportedCapabilities | ETaskUnsupportedCapability::ProblemMatcher;
	}
	return { ETaskConfigurationCatalogStatus::Applied, {} };
}

bool IsAcceptedTasksDocument(const workspace::WorkspaceArtifactDocument& document) noexcept
{
	return document.kind == workspace::EWorkspaceArtifactDocumentKind::Tasks && document.generation != 0 && document.revision != 0
		&& !document.resource.Scheme().empty() && !document.resource.Path().empty() && !document.resource.Query()
		&& !document.resource.Fragment() && !platform::uri::UriIdentityService::MakeComparisonKey(document.resource).empty();
}

} // namespace

TaskConfigurationCatalogResult CTaskConfigurationCatalog::Apply(const workspace::TasksDocumentSnapshot& snapshot)
{
	if (!snapshot.document) return Failure(ETaskConfigurationCatalogStatus::InvalidSnapshot, "an accepted tasks document is required; use Clear for a confirmed absence");
	const auto& document = *snapshot.document;
	if (!IsAcceptedTasksDocument(document)) return Failure(ETaskConfigurationCatalogStatus::InvalidArtifact, "tasks document identity is invalid");
	const auto tasks = document.artifact.find(L"tasks");
	if (tasks == document.artifact.end()) return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "tasks artifact requires a tasks array");
	const auto* entries = ArrayValue(tasks->second);
	if (!entries) return Failure(ETaskConfigurationCatalogStatus::InvalidSchema, "tasks must be an array");
	if (entries->size() > kMaximumTasks) return Failure(ETaskConfigurationCatalogStatus::MaximumTasksExceeded, "task limit exceeded");

	std::vector<TaskConfigurationDefinition> parsed;
	parsed.reserve(entries->size());
	std::set<std::wstring, std::less<>> labels;
	for (const auto& entry : *entries) {
		TaskConfigurationDefinition definition;
		const auto result = ParseDefinition(entry, document, definition);
		if (!result.Succeeded()) return result;
		if (!labels.emplace(definition.label).second) return Failure(ETaskConfigurationCatalogStatus::DuplicateLabel, "task labels must be unique");
		parsed.push_back(std::move(definition));
	}
	std::sort(parsed.begin(), parsed.end(), [](const auto& left, const auto& right) { return left.label < right.label; });
	const auto sourceIdentity = platform::uri::UriIdentityService::MakeComparisonKey(document.resource);

	std::lock_guard lock(m_mutex);
	if (m_stopped) return Failure(ETaskConfigurationCatalogStatus::Stopped, "task catalog is stopped");
	if (document.generation < m_generation) return Failure(ETaskConfigurationCatalogStatus::StaleGeneration, "task document belongs to an older generation");
	if (document.generation == m_generation && m_sourceIdentity && *m_sourceIdentity == sourceIdentity && document.revision <= m_revision) {
		return Failure(ETaskConfigurationCatalogStatus::StaleRevision, "task document revision is not newer");
	}
	m_generation = document.generation;
	m_revision = document.revision;
	m_sourceIdentity = sourceIdentity;
	m_sourceUri = document.resource;
	m_definitions = std::move(parsed);
	return { ETaskConfigurationCatalogStatus::Applied, {} };
}

TaskConfigurationCatalogResult CTaskConfigurationCatalog::Clear(std::uint64_t generation, std::uint64_t revision) noexcept
{
	if (generation == 0 || revision == 0) return Failure(ETaskConfigurationCatalogStatus::InvalidSnapshot, "task clear generation and revision are required");
	std::lock_guard lock(m_mutex);
	if (m_stopped) return Failure(ETaskConfigurationCatalogStatus::Stopped, "task catalog is stopped");
	if (generation < m_generation) return Failure(ETaskConfigurationCatalogStatus::StaleGeneration, "task clear belongs to an older generation");
	if (generation == m_generation && revision <= m_revision) return Failure(ETaskConfigurationCatalogStatus::StaleRevision, "task clear revision is not newer");
	m_generation = generation;
	m_revision = revision;
	m_sourceIdentity.reset();
	m_sourceUri.reset();
	m_definitions.clear();
	return { ETaskConfigurationCatalogStatus::Cleared, {} };
}

TaskConfigurationCatalogResult CTaskConfigurationCatalog::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopped) return Failure(ETaskConfigurationCatalogStatus::Stopped, "task catalog is stopped");
	m_stopped = true;
	m_sourceIdentity.reset();
	m_sourceUri.reset();
	m_definitions.clear();
	return { ETaskConfigurationCatalogStatus::Cleared, "task catalog stopped" };
}

TaskConfigurationCatalogSnapshot CTaskConfigurationCatalog::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return { m_generation, m_revision, m_stopped, m_sourceUri, m_definitions };
}

} // namespace workbench::tasks
