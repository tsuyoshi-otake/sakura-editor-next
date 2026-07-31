/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/CConfigurationService.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace config {
namespace {

bool IsCanonicalKey(const std::string& key)
{
	if (key.empty() || key.front() == '.' || key.back() == '.') {
		return false;
	}
	bool needsSegmentStart = true;
	bool hasDot = false;
	for (const unsigned char character : key) {
		if (character == '.') {
			if (needsSegmentStart) {
				return false;
			}
			needsSegmentStart = true;
			hasDot = true;
			continue;
		}
		if (needsSegmentStart) {
			if (!std::isalpha(character)) {
				return false;
			}
			needsSegmentStart = false;
		} else if (!std::isalnum(character) && character != '-') {
			return false;
		}
	}
	return hasDot && !needsSegmentStart;
}

std::wstring UriKey(const std::optional<platform::uri::Uri>& uri)
{
	return uri ? platform::uri::UriIdentityService::MakeComparisonKey(*uri) : std::wstring{};
}

void AppendIdentityComponent(std::wstring& identity, std::wstring_view component)
{
	identity += std::to_wstring(component.size());
	identity += L':';
	identity += component;
}

std::wstring SourceIdKey(std::string_view sourceId)
{
	std::wstring result;
	result.reserve(sourceId.size());
	for (const unsigned char character : sourceId) {
		result.push_back(static_cast<wchar_t>(character));
	}
	return result;
}

std::wstring TargetIdentity(const ConfigurationTarget& target, bool includeLanguage = true)
{
	std::wstring identity;
	AppendIdentityComponent(identity, target.profileId);
	AppendIdentityComponent(identity, UriKey(target.workspaceUri));
	AppendIdentityComponent(identity, UriKey(target.folderUri));
	AppendIdentityComponent(identity, includeLanguage && target.languageId ? *target.languageId : std::wstring{});
	return identity;
}

bool IsContextValid(const ConfigurationTarget& target)
{
	if (target.folderUri && !target.workspaceUri) {
		return false;
	}
	if ((target.workspaceUri || target.folderUri || target.languageId) && target.profileId.empty()) {
		return false;
	}
	return !target.languageId || !target.languageId->empty();
}

bool IsSourceTargetValid(const ConfigurationSource& source, std::string& diagnostic)
{
	if (source.sourceId.empty()) {
		diagnostic = "sourceId is required";
		return false;
	}
	if (!IsContextValid(source.target)) {
		diagnostic = "target has an invalid URI/profile/language combination";
		return false;
	}
	const auto& target = source.target;
	switch (source.scope) {
	case EConfigurationScope::Default:
		diagnostic = "default values are descriptor-owned and cannot be written";
		return false;
	case EConfigurationScope::Application:
		if (!target.profileId.empty() || target.workspaceUri || target.folderUri || target.languageId) {
			diagnostic = "application scope does not accept profile, workspace, folder, or language fields";
			return false;
		}
		return true;
	case EConfigurationScope::Profile:
		if (target.profileId.empty()) {
			diagnostic = "profile scope requires profileId";
			return false;
		}
		if (target.workspaceUri || target.folderUri || target.languageId) {
			diagnostic = "profile scope does not accept workspace, folder, or language fields";
			return false;
		}
		return true;
	case EConfigurationScope::Workspace:
		if (target.profileId.empty() || !target.workspaceUri) {
			diagnostic = "workspace scope requires profileId and workspaceUri";
			return false;
		}
		if (target.folderUri || target.languageId) {
			diagnostic = "workspace scope does not accept folder or language fields";
			return false;
		}
		return true;
	case EConfigurationScope::Folder:
		if (target.profileId.empty() || !target.workspaceUri || !target.folderUri) {
			diagnostic = "folder scope requires profileId, workspaceUri, and folderUri";
			return false;
		}
		if (target.languageId) {
			diagnostic = "folder scope does not accept a language field";
			return false;
		}
		return true;
	case EConfigurationScope::LanguageOverride:
		if (target.profileId.empty() || !target.languageId || target.languageId->empty()) {
			diagnostic = "language override requires profileId and languageId";
			return false;
		}
		return true;
	}
	diagnostic = "unsupported configuration scope";
	return false;
}

bool IsScopePermitted(const ConfigurationDescriptor& descriptor, EConfigurationScope scope)
{
	return std::find(descriptor.permittedScopes.begin(), descriptor.permittedScopes.end(), scope) != descriptor.permittedScopes.end();
}

bool HasExpectedKind(const ConfigurationValue& value, EConfigurationValueKind kind)
{
	const auto& storage = value.Value();
	switch (kind) {
	case EConfigurationValueKind::Any:
		return true;
	case EConfigurationValueKind::Null:
		return std::holds_alternative<std::monostate>(storage);
	case EConfigurationValueKind::Boolean:
		return std::holds_alternative<bool>(storage);
	case EConfigurationValueKind::Integer:
		return std::holds_alternative<std::int64_t>(storage);
	case EConfigurationValueKind::Number:
		return std::holds_alternative<double>(storage);
	case EConfigurationValueKind::String:
		return std::holds_alternative<std::wstring>(storage);
	case EConfigurationValueKind::Array:
		return std::holds_alternative<ConfigurationValue::Array>(storage);
	case EConfigurationValueKind::Object:
		return std::holds_alternative<ConfigurationValue::Object>(storage);
	}
	return false;
}

bool IsConstraintWellFormed(const ConfigurationValueConstraint& constraint)
{
	if (constraint.minimumInteger && constraint.maximumInteger
		&& *constraint.minimumInteger > *constraint.maximumInteger) {
		return false;
	}
	if ((constraint.minimumInteger || constraint.maximumInteger)
		&& constraint.kind != EConfigurationValueKind::Integer) {
		return false;
	}
	if ((constraint.maximumStringLength || !constraint.allowedStringValues.empty())
		&& constraint.kind != EConfigurationValueKind::String) {
		return false;
	}
	if ((constraint.maximumArrayLength || constraint.arrayItemsMustBeStrings
		|| constraint.maximumArrayItemStringLength)
		&& constraint.kind != EConfigurationValueKind::Array) {
		return false;
	}
	if (constraint.maximumArrayItemStringLength && !constraint.arrayItemsMustBeStrings) {
		return false;
	}
	return true;
}

bool SatisfiesConstraint(const ConfigurationValue& value, const ConfigurationValueConstraint& constraint)
{
	if (!value.IsValid() || !HasExpectedKind(value, constraint.kind)) {
		return false;
	}
	const auto& storage = value.Value();
	if (const auto string = std::get_if<std::wstring>(&storage)) {
		if (constraint.maximumStringLength && string->size() > *constraint.maximumStringLength) {
			return false;
		}
		if (!constraint.allowedStringValues.empty()
			&& std::find(constraint.allowedStringValues.begin(), constraint.allowedStringValues.end(), *string)
				== constraint.allowedStringValues.end()) {
			return false;
		}
	}
	if (const auto integer = std::get_if<std::int64_t>(&storage)) {
		if ((constraint.minimumInteger && *integer < *constraint.minimumInteger)
			|| (constraint.maximumInteger && *integer > *constraint.maximumInteger)) {
			return false;
		}
	}
	if (const auto array = std::get_if<ConfigurationValue::Array>(&storage)) {
		if (constraint.maximumArrayLength && array->size() > *constraint.maximumArrayLength) {
			return false;
		}
		if (constraint.arrayItemsMustBeStrings) {
			for (const auto& item : *array) {
				const auto string = std::get_if<std::wstring>(&item.Value());
				if (!string || (constraint.maximumArrayItemStringLength
					&& string->size() > *constraint.maximumArrayItemStringLength)) {
					return false;
				}
			}
		}
	}
	return true;
}

std::wstring LayerIdentity(EConfigurationScope scope, const ConfigurationTarget& target)
{
	std::wstring identity;
	auto append = [&identity](std::wstring_view component) { AppendIdentityComponent(identity, component); };
	switch (scope) {
	case EConfigurationScope::Application:
		append(L"application");
		return identity;
	case EConfigurationScope::Profile:
		append(L"profile");
		append(target.profileId);
		return identity;
	case EConfigurationScope::Workspace:
		append(L"workspace");
		append(target.profileId);
		append(UriKey(target.workspaceUri));
		return identity;
	case EConfigurationScope::Folder:
		append(L"folder");
		append(target.profileId);
		append(UriKey(target.workspaceUri));
		append(UriKey(target.folderUri));
		return identity;
	case EConfigurationScope::LanguageOverride:
		append(L"language");
		identity += TargetIdentity(target);
		return identity;
	case EConfigurationScope::Default:
		append(L"default");
		return identity;
	}
	return {};
}

std::wstring SourceIdentity(const ConfigurationSource& source)
{
	const auto layer = LayerIdentity(source.scope, source.target);
	std::wstring key = layer;
	AppendIdentityComponent(key, SourceIdKey(source.sourceId));
	return key;
}

void AppendValueIdentity(std::wstring& identity, const ConfigurationValue& value)
{
	const auto& storage = value.Value();
	if (std::holds_alternative<std::monostate>(storage)) {
		AppendIdentityComponent(identity, L"null");
		return;
	}
	if (const auto boolean = std::get_if<bool>(&storage)) {
		AppendIdentityComponent(identity, *boolean ? L"true" : L"false");
		return;
	}
	if (const auto integer = std::get_if<std::int64_t>(&storage)) {
		AppendIdentityComponent(identity, L"integer");
		AppendIdentityComponent(identity, std::to_wstring(*integer));
		return;
	}
	if (const auto real = std::get_if<double>(&storage)) {
		AppendIdentityComponent(identity, L"double");
		const auto bits = *real == 0.0 ? std::uint64_t{} : std::bit_cast<std::uint64_t>(*real);
		AppendIdentityComponent(identity, std::to_wstring(bits));
		return;
	}
	if (const auto string = std::get_if<std::wstring>(&storage)) {
		AppendIdentityComponent(identity, L"string");
		AppendIdentityComponent(identity, *string);
		return;
	}
	if (const auto array = std::get_if<ConfigurationValue::Array>(&storage)) {
		AppendIdentityComponent(identity, L"array");
		AppendIdentityComponent(identity, std::to_wstring(array->size()));
		for (const auto& item : *array) {
			AppendValueIdentity(identity, item);
		}
		return;
	}
	const auto& object = std::get<ConfigurationValue::Object>(storage);
	AppendIdentityComponent(identity, L"object");
	AppendIdentityComponent(identity, std::to_wstring(object.size()));
	for (const auto& [key, item] : object) {
		AppendIdentityComponent(identity, key);
		AppendValueIdentity(identity, item);
	}
}

std::wstring UpdateRequestIdentity(const ConfigurationUpdate& request)
{
	std::wstring identity;
	AppendIdentityComponent(identity, L"update");
	AppendIdentityComponent(identity, SourceIdKey(request.key));
	AppendIdentityComponent(identity, std::to_wstring(request.source.priority));
	AppendIdentityComponent(identity, request.expectedRevision ? std::to_wstring(*request.expectedRevision) : L"none");
	AppendIdentityComponent(identity, request.value ? L"set" : L"remove");
	if (request.value) {
		AppendValueIdentity(identity, *request.value);
	}
	return identity;
}

std::wstring ReplaceRequestIdentity(
	const ConfigurationReplaceSource& request,
	const std::map<std::string, ConfigurationValue, std::less<>>& replacement
)
{
	std::wstring identity;
	AppendIdentityComponent(identity, L"replace");
	AppendIdentityComponent(identity, std::to_wstring(request.source.priority));
	AppendIdentityComponent(identity, request.expectedRevision ? std::to_wstring(*request.expectedRevision) : L"none");
	AppendIdentityComponent(identity, std::to_wstring(replacement.size()));
	for (const auto& [key, value] : replacement) {
		AppendIdentityComponent(identity, SourceIdKey(key));
		AppendValueIdentity(identity, value);
	}
	return identity;
}

struct PreparedBatchReplacement final {
	ConfigurationSource source;
	std::map<std::string, ConfigurationValue, std::less<>> entries;
	std::optional<std::uint64_t> expectedRevision;
	std::wstring sourceIdentity;
};

std::wstring ReplaceSourcesRequestIdentity(const std::vector<PreparedBatchReplacement>& replacements)
{
	std::vector<const PreparedBatchReplacement*> ordered;
	ordered.reserve(replacements.size());
	for (const auto& replacement : replacements) {
		ordered.push_back(&replacement);
	}
	std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
		return left->sourceIdentity < right->sourceIdentity;
	});

	std::wstring identity;
	AppendIdentityComponent(identity, L"replace-sources");
	AppendIdentityComponent(identity, std::to_wstring(ordered.size()));
	for (const auto* replacement : ordered) {
		AppendIdentityComponent(identity, replacement->sourceIdentity);
		AppendIdentityComponent(identity, std::to_wstring(replacement->source.priority));
		AppendIdentityComponent(identity,
			replacement->expectedRevision ? std::to_wstring(*replacement->expectedRevision) : L"none");
		AppendIdentityComponent(identity, std::to_wstring(replacement->entries.size()));
		for (const auto& [key, value] : replacement->entries) {
			AppendIdentityComponent(identity, SourceIdKey(key));
			AppendValueIdentity(identity, value);
		}
	}
	return identity;
}

std::wstring CompletedOperationIdentity(const std::wstring& sourceIdentity, std::string_view operationId)
{
	std::wstring identity = sourceIdentity;
	AppendIdentityComponent(identity, SourceIdKey(operationId));
	return identity;
}

ConfigurationResult Invalid(EConfigurationOutcome outcome, std::string diagnostic)
{
	return { outcome, 0, std::move(diagnostic) };
}

ConfigurationBatchResult InvalidBatch(EConfigurationOutcome outcome, std::string diagnostic)
{
	return { outcome, {}, std::move(diagnostic) };
}

} // namespace

struct CConfigurationService::State final {
	struct SourceRecord final {
		ConfigurationSource source;
		std::wstring layerIdentity;
		std::map<std::string, ConfigurationValue, std::less<>> entries;
		std::uint64_t revision = 0;
	};
	struct CompletedOperation final {
		std::wstring requestIdentity;
		ConfigurationResult result;
	};
	struct CompletedBatchOperation final {
		std::wstring requestIdentity;
		ConfigurationBatchResult result;
	};

	static constexpr std::size_t kMaximumCompletedOperations = 1024;
	mutable std::mutex mutex;
	std::map<std::string, ConfigurationDescriptor, std::less<>> descriptors;
	std::map<std::wstring, SourceRecord, std::less<>> sources;
	std::map<std::wstring, CompletedOperation, std::less<>> completedOperations;
	std::deque<std::wstring> completedOperationOrder;
	std::map<std::wstring, CompletedBatchOperation, std::less<>> completedBatchOperations;
	std::deque<std::wstring> completedBatchOperationOrder;
	std::map<std::uint64_t, ConfigurationListener> listeners;
	std::uint64_t nextListenerId = 1;
	// This tracks committed service transactions independently of the
	// per-source revisions used for compare-and-swap writes.
	std::uint64_t committedRevision = 0;
};

namespace {

using State = CConfigurationService::State;

std::optional<ConfigurationResult> ReplayOrOperationIdConflictLocked(
	const State& state,
	const std::wstring& sourceIdentity,
	std::string_view operationId,
	const std::wstring& requestIdentity
)
{
	const auto completed = state.completedOperations.find(CompletedOperationIdentity(sourceIdentity, operationId));
	if (completed == state.completedOperations.end()) {
		return std::nullopt;
	}
	if (completed->second.requestIdentity == requestIdentity) {
		return ConfigurationResult { EConfigurationOutcome::Replayed, completed->second.result.revision, "operationId was already completed" };
	}
	return ConfigurationResult { EConfigurationOutcome::OperationIdConflict, completed->second.result.revision, "operationId was already used for a different request" };
}

void RememberCompletedOperationLocked(
	State& state,
	const std::wstring& sourceIdentity,
	std::string_view operationId,
	std::wstring requestIdentity,
	const ConfigurationResult& result
)
{
	if (result.outcome != EConfigurationOutcome::Applied
		&& result.outcome != EConfigurationOutcome::NoChange
		&& result.outcome != EConfigurationOutcome::Conflict) {
		return;
	}
	const auto completedIdentity = CompletedOperationIdentity(sourceIdentity, operationId);
	if (state.completedOperations.find(completedIdentity) != state.completedOperations.end()) {
		return;
	}
	while (state.completedOperationOrder.size() >= State::kMaximumCompletedOperations) {
		state.completedOperations.erase(state.completedOperationOrder.front());
		state.completedOperationOrder.pop_front();
	}
	state.completedOperationOrder.push_back(completedIdentity);
	state.completedOperations.emplace(completedIdentity, State::CompletedOperation { std::move(requestIdentity), result });
}

std::wstring BatchCompletedOperationIdentity(std::string_view operationId)
{
	std::wstring identity;
	AppendIdentityComponent(identity, L"batch");
	AppendIdentityComponent(identity, SourceIdKey(operationId));
	return identity;
}

std::optional<ConfigurationBatchResult> ReplayOrBatchOperationIdConflictLocked(
	const State& state,
	std::string_view operationId,
	const std::wstring& requestIdentity
)
{
	const auto completed = state.completedBatchOperations.find(BatchCompletedOperationIdentity(operationId));
	if (completed == state.completedBatchOperations.end()) {
		return std::nullopt;
	}
	auto result = completed->second.result;
	if (completed->second.requestIdentity == requestIdentity) {
		result.outcome = EConfigurationOutcome::Replayed;
		result.diagnostic = "operationId was already completed";
		return result;
	}
	result.outcome = EConfigurationOutcome::OperationIdConflict;
	result.diagnostic = "operationId was already used for a different batch request";
	return result;
}

void RememberCompletedBatchOperationLocked(
	State& state,
	std::string_view operationId,
	std::wstring requestIdentity,
	const ConfigurationBatchResult& result
)
{
	if (result.outcome != EConfigurationOutcome::Applied
		&& result.outcome != EConfigurationOutcome::NoChange
		&& result.outcome != EConfigurationOutcome::Conflict) {
		return;
	}
	const auto completedIdentity = BatchCompletedOperationIdentity(operationId);
	if (state.completedBatchOperations.find(completedIdentity) != state.completedBatchOperations.end()) {
		return;
	}
	while (state.completedBatchOperationOrder.size() >= State::kMaximumCompletedOperations) {
		state.completedBatchOperations.erase(state.completedBatchOperationOrder.front());
		state.completedBatchOperationOrder.pop_front();
	}
	state.completedBatchOperationOrder.push_back(completedIdentity);
	state.completedBatchOperations.emplace(completedIdentity,
		State::CompletedBatchOperation { std::move(requestIdentity), result });
}

bool HasMatchingSourcePriority(const State::SourceRecord& record, const ConfigurationSource& source)
{
	return record.source.priority == source.priority;
}

std::vector<ConfigurationProvenance> CollectProvenanceLocked(
	const State& state,
	const std::string& key,
	const ConfigurationTarget& target
)
{
	std::vector<ConfigurationProvenance> result;
	const auto descriptor = state.descriptors.find(key);
	if (descriptor == state.descriptors.end()) {
		return result;
	}
	result.push_back({ EConfigurationScope::Default, "descriptor", 0, descriptor->second.defaultValue });
	for (const auto scope : { EConfigurationScope::Application, EConfigurationScope::Profile, EConfigurationScope::Workspace, EConfigurationScope::Folder, EConfigurationScope::LanguageOverride }) {
		const auto targetLayer = LayerIdentity(scope, target);
		std::vector<const State::SourceRecord*> candidates;
		for (const auto& [identity, source] : state.sources) {
			(void)identity;
			if (source.source.scope != scope || source.layerIdentity != targetLayer) {
				continue;
			}
			if (source.entries.find(key) != source.entries.end()) {
				candidates.push_back(&source);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
			return left->source.priority == right->source.priority
				? left->source.sourceId < right->source.sourceId
				: left->source.priority < right->source.priority;
		});
		for (const auto* source : candidates) {
			result.push_back({ scope, source->source.sourceId, source->revision, source->entries.find(key)->second, source->source.priority });
		}
	}
	return result;
}

ConfigurationValue EffectiveLocked(const State& state, const std::string& key, const ConfigurationTarget& target)
{
	auto provenance = CollectProvenanceLocked(state, key, target);
	return provenance.back().value;
}

void Notify(const std::shared_ptr<State>& state, const std::vector<ConfigurationChange>& changes)
{
	if (changes.empty()) {
		return;
	}
	std::vector<std::uint64_t> listenerIds;
	{
		std::lock_guard lock(state->mutex);
		for (const auto& [id, listener] : state->listeners) {
			(void)listener;
			listenerIds.push_back(id);
		}
	}
	for (const auto id : listenerIds) {
		ConfigurationListener listener;
		{
			std::lock_guard lock(state->mutex);
			const auto found = state->listeners.find(id);
			if (found == state->listeners.end()) {
				continue;
			}
			listener = found->second;
		}
		try {
			listener(changes);
		} catch (...) {
			// Listener code is an observer boundary. Its failure must not prevent
			// other listeners from seeing a committed configuration transition.
		}
	}
}

} // namespace

CConfigurationService::CConfigurationService(std::vector<ConfigurationDescriptor> descriptors)
	: m_state(std::make_shared<State>())
{
	for (auto& descriptor : descriptors) {
		if (!IsCanonicalKey(descriptor.key) || !descriptor.defaultValue.IsValid()
			|| !IsConstraintWellFormed(descriptor.valueConstraint)
			|| !SatisfiesConstraint(descriptor.defaultValue, descriptor.valueConstraint)
			|| descriptor.permittedScopes.empty()) {
			continue;
		}
		descriptor.permittedScopes.erase(std::remove(descriptor.permittedScopes.begin(), descriptor.permittedScopes.end(), EConfigurationScope::Default), descriptor.permittedScopes.end());
		if (!descriptor.permittedScopes.empty()) {
			m_state->descriptors.emplace(descriptor.key, std::move(descriptor));
		}
	}
}

CConfigurationService::~CConfigurationService() = default;

ConfigurationLookupResult CConfigurationService::GetValue(const std::string& key, const ConfigurationTarget& target) const
{
	const auto inspected = Inspect(key, target);
	return { inspected.outcome, inspected.effectiveValue, inspected.diagnostic };
}

ConfigurationReadSnapshotResult CConfigurationService::ReadSnapshot(
	const std::vector<std::string>& keys,
	const ConfigurationTarget& target
) const
{
	if (!IsContextValid(target)) {
		return { EConfigurationOutcome::InvalidScope, std::nullopt,
			"target has an invalid URI/profile/language combination" };
	}
	for (const auto& key : keys) {
		if (!IsCanonicalKey(key)) {
			return { EConfigurationOutcome::InvalidKey, std::nullopt,
				"every key must be a canonical dotted identifier" };
		}
	}

	std::lock_guard lock(m_state->mutex);
	for (const auto& key : keys) {
		if (m_state->descriptors.find(key) == m_state->descriptors.end()) {
			return { EConfigurationOutcome::InvalidKey, std::nullopt,
				"every key must be registered by a descriptor" };
		}
	}

	ConfigurationReadSnapshot snapshot;
	snapshot.revision = m_state->committedRevision;
	snapshot.values.reserve(keys.size());
	for (const auto& key : keys) {
		snapshot.values.push_back(EffectiveLocked(*m_state, key, target));
	}
	return { EConfigurationOutcome::Applied, std::move(snapshot), {} };
}

ConfigurationInspection CConfigurationService::Inspect(const std::string& key, const ConfigurationTarget& target) const
{
	if (!IsCanonicalKey(key)) {
		return { EConfigurationOutcome::InvalidKey, std::nullopt, {}, "key must be a canonical dotted identifier" };
	}
	if (!IsContextValid(target)) {
		return { EConfigurationOutcome::InvalidScope, std::nullopt, {}, "target has an invalid URI/profile/language combination" };
	}
	std::lock_guard lock(m_state->mutex);
	if (m_state->descriptors.find(key) == m_state->descriptors.end()) {
		return { EConfigurationOutcome::InvalidKey, std::nullopt, {}, "key is not registered by a descriptor" };
	}
	auto provenance = CollectProvenanceLocked(*m_state, key, target);
	return { EConfigurationOutcome::Applied, provenance.back().value, std::move(provenance), {} };
}

ConfigurationResult CConfigurationService::Update(const ConfigurationUpdate& request)
{
	if (request.source.scope == EConfigurationScope::Default) {
		return Invalid(EConfigurationOutcome::InvalidScope, "default values are descriptor-owned and cannot be written");
	}
	std::string sourceDiagnostic;
	if (!IsSourceTargetValid(request.source, sourceDiagnostic) || request.operationId.empty()) {
		return Invalid(EConfigurationOutcome::InvalidSource, request.operationId.empty() ? "operationId is required" : std::move(sourceDiagnostic));
	}
	if (!IsCanonicalKey(request.key)) {
		return Invalid(EConfigurationOutcome::InvalidKey, "key must be a canonical dotted identifier");
	}
	if (request.value && !request.value->IsValid()) {
		return Invalid(EConfigurationOutcome::InvalidValue, "configuration value is not finite JSON-compatible data");
	}

	std::vector<ConfigurationChange> changes;
	ConfigurationResult result;
	{
		std::unique_lock lock(m_state->mutex);
		const auto descriptor = m_state->descriptors.find(request.key);
		if (descriptor == m_state->descriptors.end()) {
			return Invalid(EConfigurationOutcome::InvalidKey, "key is not registered by a descriptor");
		}
		if (!IsScopePermitted(descriptor->second, request.source.scope)) {
			return Invalid(EConfigurationOutcome::InvalidScope, "descriptor does not permit this scope");
		}
		if (request.value && !SatisfiesConstraint(*request.value, descriptor->second.valueConstraint)) {
			return Invalid(EConfigurationOutcome::InvalidValue, "value does not satisfy the descriptor constraint");
		}

		const auto sourceId = SourceIdentity(request.source);
		const auto requestIdentity = UpdateRequestIdentity(request);
		if (const auto replayed = ReplayOrOperationIdConflictLocked(*m_state, sourceId, request.operationId, requestIdentity)) {
			return *replayed;
		}
		auto source = m_state->sources.find(sourceId);
		if (source != m_state->sources.end()) {
			if (!HasMatchingSourcePriority(source->second, request.source)) {
				return Invalid(EConfigurationOutcome::InvalidSource, "source priority cannot change after the source is created");
			}
		}
		const auto currentRevision = source == m_state->sources.end() ? 0 : source->second.revision;
		if (request.expectedRevision && *request.expectedRevision != currentRevision) {
			result = { EConfigurationOutcome::Conflict, currentRevision, "expectedRevision does not match source revision" };
			RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
			return result;
		}

		auto before = EffectiveLocked(*m_state, request.key, request.source.target);
		if (source == m_state->sources.end()) {
			if (!request.value) {
				result = { EConfigurationOutcome::NoChange, 0, "key is absent from source" };
				RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
				return result;
			}
			State::SourceRecord record;
			record.source = request.source;
			record.layerIdentity = LayerIdentity(request.source.scope, request.source.target);
			source = m_state->sources.emplace(sourceId, std::move(record)).first;
		}
		auto existing = source->second.entries.find(request.key);
		if ((!request.value && existing == source->second.entries.end()) || (request.value && existing != source->second.entries.end() && existing->second == *request.value)) {
			result = { EConfigurationOutcome::NoChange, source->second.revision, "source already has the requested value" };
			RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
			return result;
		}
		if (request.value) {
			source->second.entries.insert_or_assign(request.key, *request.value);
		} else {
			source->second.entries.erase(existing);
		}
		++source->second.revision;
		++m_state->committedRevision;
		result = { EConfigurationOutcome::Applied, source->second.revision, {} };
		RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
		auto after = EffectiveLocked(*m_state, request.key, request.source.target);
		if (before != after) {
			changes.push_back({ request.key, request.source.target, std::move(before), std::move(after) });
		}
	}
	Notify(m_state, changes);
	return result;
}

ConfigurationResult CConfigurationService::ReplaceSource(const ConfigurationReplaceSource& request)
{
	if (request.source.scope == EConfigurationScope::Default) {
		return Invalid(EConfigurationOutcome::InvalidScope, "default values are descriptor-owned and cannot be replaced");
	}
	std::string sourceDiagnostic;
	if (!IsSourceTargetValid(request.source, sourceDiagnostic) || request.operationId.empty()) {
		return Invalid(EConfigurationOutcome::InvalidSource, request.operationId.empty() ? "operationId is required" : std::move(sourceDiagnostic));
	}

	std::map<std::string, ConfigurationValue, std::less<>> replacement;
	for (const auto& entry : request.entries) {
		if (!IsCanonicalKey(entry.key)) {
			return Invalid(EConfigurationOutcome::InvalidKey, "replacement contains a non-canonical key");
		}
		if (!entry.value.IsValid()) {
			return Invalid(EConfigurationOutcome::InvalidValue, "replacement contains a non-finite value");
		}
		if (!replacement.emplace(entry.key, entry.value).second) {
			return Invalid(EConfigurationOutcome::InvalidSource, "replacement contains a duplicate key");
		}
	}

	std::vector<ConfigurationChange> changes;
	ConfigurationResult result;
	{
		std::unique_lock lock(m_state->mutex);
		for (const auto& [key, value] : replacement) {
			(void)value;
			const auto descriptor = m_state->descriptors.find(key);
			if (descriptor == m_state->descriptors.end()) {
				// File-backed sources retain canonical JSON values for descriptors that
				// are contributed later (for example, by an extension). These latent
				// entries are storage only: this service has no descriptor activation
				// API, so they cannot take part in lookup, precedence, or notification.
				continue;
			}
			if (!IsScopePermitted(descriptor->second, request.source.scope)) {
				return Invalid(EConfigurationOutcome::InvalidScope, "replacement contains a key not permitted at this scope");
			}
			if (!SatisfiesConstraint(value, descriptor->second.valueConstraint)) {
				return Invalid(EConfigurationOutcome::InvalidValue,
					"replacement contains a value that does not satisfy its descriptor constraint");
			}
		}
		const auto sourceId = SourceIdentity(request.source);
		const auto requestIdentity = ReplaceRequestIdentity(request, replacement);
		if (const auto replayed = ReplayOrOperationIdConflictLocked(*m_state, sourceId, request.operationId, requestIdentity)) {
			return *replayed;
		}
		auto source = m_state->sources.find(sourceId);
		if (source != m_state->sources.end()) {
			if (!HasMatchingSourcePriority(source->second, request.source)) {
				return Invalid(EConfigurationOutcome::InvalidSource, "source priority cannot change after the source is created");
			}
		}
		const auto currentRevision = source == m_state->sources.end() ? 0 : source->second.revision;
		if (request.expectedRevision && *request.expectedRevision != currentRevision) {
			result = { EConfigurationOutcome::Conflict, currentRevision, "expectedRevision does not match source revision" };
			RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
			return result;
		}
		if (source == m_state->sources.end() && replacement.empty()) {
			result = { EConfigurationOutcome::NoChange, 0, "source is already empty" };
			RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
			return result;
		}
		if (source != m_state->sources.end() && source->second.entries == replacement) {
			result = { EConfigurationOutcome::NoChange, source->second.revision, "source already has the replacement model" };
			RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
			return result;
		}

		std::set<std::string, std::less<>> affectedKeys;
		if (source != m_state->sources.end()) {
			for (const auto& [key, value] : source->second.entries) {
				(void)value;
				if (m_state->descriptors.find(key) != m_state->descriptors.end()) {
					affectedKeys.insert(key);
				}
			}
		}
		for (const auto& [key, value] : replacement) {
			(void)value;
			if (m_state->descriptors.find(key) != m_state->descriptors.end()) {
				affectedKeys.insert(key);
			}
		}
		std::map<std::string, ConfigurationValue, std::less<>> before;
		for (const auto& key : affectedKeys) {
			before.emplace(key, EffectiveLocked(*m_state, key, request.source.target));
		}
		if (source == m_state->sources.end()) {
			State::SourceRecord record;
			record.source = request.source;
			record.layerIdentity = LayerIdentity(request.source.scope, request.source.target);
			source = m_state->sources.emplace(sourceId, std::move(record)).first;
		}
		source->second.entries = std::move(replacement);
		++source->second.revision;
		++m_state->committedRevision;
		result = { EConfigurationOutcome::Applied, source->second.revision, {} };
		RememberCompletedOperationLocked(*m_state, sourceId, request.operationId, requestIdentity, result);
		for (const auto& key : affectedKeys) {
			auto after = EffectiveLocked(*m_state, key, request.source.target);
			if (before.find(key)->second != after) {
				changes.push_back({ key, request.source.target, before.find(key)->second, std::move(after) });
			}
		}
	}
	Notify(m_state, changes);
	return result;
}

ConfigurationBatchResult CConfigurationService::ReplaceSources(const ConfigurationReplaceSources& request)
{
	if (request.operationId.empty()) {
		return InvalidBatch(EConfigurationOutcome::InvalidSource, "operationId is required");
	}
	if (request.replacements.empty()) {
		return InvalidBatch(EConfigurationOutcome::InvalidSource, "at least one source replacement is required");
	}

	std::vector<PreparedBatchReplacement> prepared;
	prepared.reserve(request.replacements.size());
	std::set<std::wstring, std::less<>> sourceIdentities;
	for (const auto& requested : request.replacements) {
		if (requested.source.scope == EConfigurationScope::Default) {
			return InvalidBatch(EConfigurationOutcome::InvalidScope,
				"default values are descriptor-owned and cannot be replaced");
		}
		std::string sourceDiagnostic;
		if (!IsSourceTargetValid(requested.source, sourceDiagnostic)) {
			return InvalidBatch(EConfigurationOutcome::InvalidSource, std::move(sourceDiagnostic));
		}

		PreparedBatchReplacement replacement;
		replacement.source = requested.source;
		replacement.expectedRevision = requested.expectedRevision;
		replacement.sourceIdentity = SourceIdentity(requested.source);
		if (!sourceIdentities.emplace(replacement.sourceIdentity).second) {
			return InvalidBatch(EConfigurationOutcome::InvalidSource,
				"batch contains the same source more than once");
		}
		for (const auto& entry : requested.entries) {
			if (!IsCanonicalKey(entry.key)) {
				return InvalidBatch(EConfigurationOutcome::InvalidKey,
					"batch replacement contains a non-canonical key");
			}
			if (!entry.value.IsValid()) {
				return InvalidBatch(EConfigurationOutcome::InvalidValue,
					"batch replacement contains a non-finite value");
			}
			if (!replacement.entries.emplace(entry.key, entry.value).second) {
				return InvalidBatch(EConfigurationOutcome::InvalidSource,
					"batch replacement contains a duplicate key");
			}
		}
		prepared.emplace_back(std::move(replacement));
	}
	const auto requestIdentity = ReplaceSourcesRequestIdentity(prepared);

	struct AffectedValue final {
		std::string key;
		ConfigurationTarget target;
		ConfigurationValue before;
	};
	std::vector<ConfigurationChange> changes;
	ConfigurationBatchResult result;
	{
		std::unique_lock lock(m_state->mutex);
		for (const auto& replacement : prepared) {
			for (const auto& [key, value] : replacement.entries) {
				(void)value;
				const auto descriptor = m_state->descriptors.find(key);
				if (descriptor == m_state->descriptors.end()) {
					// Keep latent file-source entries for a future descriptor registry or
					// extension contribution. Unknown keys are intentionally inert until
					// such a registry exists; this is not an activation mechanism.
					continue;
				}
				if (!IsScopePermitted(descriptor->second, replacement.source.scope)) {
					return InvalidBatch(EConfigurationOutcome::InvalidScope,
						"batch replacement contains a key not permitted at this scope");
				}
				if (!SatisfiesConstraint(value, descriptor->second.valueConstraint)) {
					return InvalidBatch(EConfigurationOutcome::InvalidValue,
						"batch replacement contains a value that does not satisfy its descriptor constraint");
				}
			}
		}
		if (const auto replayed = ReplayOrBatchOperationIdConflictLocked(
				*m_state, request.operationId, requestIdentity)) {
			return *replayed;
		}

		result.revisions.reserve(prepared.size());
		bool revisionConflict = false;
		for (const auto& replacement : prepared) {
			const auto source = m_state->sources.find(replacement.sourceIdentity);
			if (source != m_state->sources.end()
				&& !HasMatchingSourcePriority(source->second, replacement.source)) {
				return InvalidBatch(EConfigurationOutcome::InvalidSource,
					"source priority cannot change after the source is created");
			}
			const auto currentRevision = source == m_state->sources.end() ? 0 : source->second.revision;
			result.revisions.push_back({ replacement.source, currentRevision });
			if (replacement.expectedRevision && *replacement.expectedRevision != currentRevision) {
				revisionConflict = true;
			}
		}
		if (revisionConflict) {
			result.outcome = EConfigurationOutcome::Conflict;
			result.diagnostic = "one or more expected revisions do not match their source revisions";
			RememberCompletedBatchOperationLocked(*m_state, request.operationId, requestIdentity, result);
			return result;
		}

		std::vector<bool> changed(prepared.size(), false);
		bool anyChanged = false;
		for (std::size_t index = 0; index < prepared.size(); ++index) {
			const auto source = m_state->sources.find(prepared[index].sourceIdentity);
			changed[index] = source == m_state->sources.end()
				? !prepared[index].entries.empty()
				: source->second.entries != prepared[index].entries;
			anyChanged = anyChanged || changed[index];
		}
		if (!anyChanged) {
			result.outcome = EConfigurationOutcome::NoChange;
			result.diagnostic = "every source already has the requested replacement model";
			RememberCompletedBatchOperationLocked(*m_state, request.operationId, requestIdentity, result);
			return result;
		}

		std::map<std::wstring, AffectedValue, std::less<>> affected;
		for (std::size_t index = 0; index < prepared.size(); ++index) {
			if (!changed[index]) {
				continue;
			}
			std::set<std::string, std::less<>> keys;
			const auto source = m_state->sources.find(prepared[index].sourceIdentity);
			if (source != m_state->sources.end()) {
				for (const auto& [key, value] : source->second.entries) {
					(void)value;
					if (m_state->descriptors.find(key) != m_state->descriptors.end()) {
						keys.insert(key);
					}
				}
			}
			for (const auto& [key, value] : prepared[index].entries) {
				(void)value;
				if (m_state->descriptors.find(key) != m_state->descriptors.end()) {
					keys.insert(key);
				}
			}
			for (const auto& key : keys) {
				std::wstring affectedIdentity = TargetIdentity(prepared[index].source.target);
				AppendIdentityComponent(affectedIdentity, SourceIdKey(key));
				if (affected.find(affectedIdentity) == affected.end()) {
					affected.emplace(std::move(affectedIdentity), AffectedValue {
						key,
						prepared[index].source.target,
						EffectiveLocked(*m_state, key, prepared[index].source.target),
					});
				}
			}
		}

		for (std::size_t index = 0; index < prepared.size(); ++index) {
			if (!changed[index]) {
				continue;
			}
			auto source = m_state->sources.find(prepared[index].sourceIdentity);
			if (source == m_state->sources.end()) {
				State::SourceRecord record;
				record.source = prepared[index].source;
				record.layerIdentity = LayerIdentity(
					prepared[index].source.scope, prepared[index].source.target);
				source = m_state->sources.emplace(
					prepared[index].sourceIdentity, std::move(record)).first;
			}
			source->second.entries = prepared[index].entries;
			++source->second.revision;
			result.revisions[index].revision = source->second.revision;
		}
		++m_state->committedRevision;
		result.outcome = EConfigurationOutcome::Applied;
		result.diagnostic.clear();
		RememberCompletedBatchOperationLocked(*m_state, request.operationId, requestIdentity, result);

		for (auto& [identity, value] : affected) {
			(void)identity;
			auto after = EffectiveLocked(*m_state, value.key, value.target);
			if (value.before != after) {
				changes.push_back({ value.key, value.target, std::move(value.before), std::move(after) });
			}
		}
	}
	Notify(m_state, changes);
	return result;
}

ConfigurationSubscription CConfigurationService::Subscribe(ConfigurationListener listener)
{
	if (!listener) {
		return {};
	}
	std::uint64_t id;
	{
		std::lock_guard lock(m_state->mutex);
		id = m_state->nextListenerId++;
		m_state->listeners.emplace(id, std::move(listener));
	}
	std::weak_ptr<State> state = m_state;
	return ConfigurationSubscription([state = std::move(state), id]() noexcept {
		if (const auto locked = state.lock()) {
			std::lock_guard lock(locked->mutex);
			locked->listeners.erase(id);
		}
	});
}

} // namespace config
