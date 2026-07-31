/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "platform/uri/UriIdentity.h"

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace config {

//! Configuration scope precedence is the declaration order below.
enum class EConfigurationScope : std::uint8_t {
	Default,
	Application,
	Profile,
	Workspace,
	Folder,
	LanguageOverride,
};

//! Result of a state-changing configuration request.
enum class EConfigurationOutcome : std::uint8_t {
	Applied,
	NoChange,
	Replayed,
	// The operation identifier was already used for a different request.
	OperationIdConflict,
	Conflict,
	InvalidKey,
	InvalidScope,
	InvalidValue,
	InvalidSource,
	Unsupported,
};

//! A recursive, JSON-compatible configuration value. Null is represented by
//! std::monostate. Object member order is stable for deterministic comparison.
class ConfigurationValue final {
public:
	using Array = std::vector<ConfigurationValue>;
	using Object = std::map<std::wstring, ConfigurationValue, std::less<>>;
	using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::wstring, Array, Object>;

	ConfigurationValue() = default;
	ConfigurationValue(std::nullptr_t) noexcept : m_value(std::monostate{}) {}
	ConfigurationValue(bool value) : m_value(value) {}
	ConfigurationValue(std::int64_t value) : m_value(value) {}
	ConfigurationValue(int value) : m_value(static_cast<std::int64_t>(value)) {}
	ConfigurationValue(double value) : m_value(value) {}
	ConfigurationValue(const wchar_t* value) : m_value(std::wstring(value)) {}
	ConfigurationValue(std::wstring value) : m_value(std::move(value)) {}
	ConfigurationValue(Array value) : m_value(std::move(value)) {}
	ConfigurationValue(Object value) : m_value(std::move(value)) {}

	const Storage& Value() const noexcept { return m_value; }
	bool IsValid() const noexcept;

	friend bool operator==(const ConfigurationValue& left, const ConfigurationValue& right) noexcept;
	friend bool operator!=(const ConfigurationValue& left, const ConfigurationValue& right) noexcept { return !(left == right); }

private:
	Storage m_value;
};

//! Public target identity. URI values deliberately remain URI values, rather
//! than filesystem paths, so non-file workspaces are supported as well.
struct ConfigurationTarget final {
	std::wstring profileId;
	std::optional<platform::uri::Uri> workspaceUri;
	std::optional<platform::uri::Uri> folderUri;
	std::optional<std::wstring> languageId;
};

//! A schema entry. Keys are canonical dotted ASCII identifiers.
enum class EConfigurationValueKind : std::uint8_t {
	Any,
	Null,
	Boolean,
	Integer,
	Number,
	String,
	Array,
	Object,
};

//! Declarative value contract attached to a descriptor.  This deliberately
//! contains data only: configuration writes never invoke extension or UI code
//! while the service lock is held.  The array fields describe a bounded array
//! whose members are strings when arrayItemsMustBeStrings is set.
struct ConfigurationValueConstraint final {
	EConfigurationValueKind kind = EConfigurationValueKind::Any;
	std::optional<std::size_t> maximumStringLength;
	std::vector<std::wstring> allowedStringValues;
	std::optional<std::int64_t> minimumInteger;
	std::optional<std::int64_t> maximumInteger;
	std::optional<std::size_t> maximumArrayLength;
	bool arrayItemsMustBeStrings = false;
	std::optional<std::size_t> maximumArrayItemStringLength;
};

struct ConfigurationDescriptor final {
	std::string key;
	ConfigurationValue defaultValue;
	std::vector<EConfigurationScope> permittedScopes;
	ConfigurationValueConstraint valueConstraint;
};

//! Identifies one replaceable source at a configuration scope.
struct ConfigurationSource final {
	EConfigurationScope scope = EConfigurationScope::Application;
	ConfigurationTarget target;
	std::string sourceId;
	// Higher values win within one scope/layer. Equal priorities are ordered by
	// sourceId, so writes never alter layer precedence as a side effect.
	std::int32_t priority = 0;
};

struct ConfigurationEntry final {
	std::string key;
	ConfigurationValue value;
};

//! The write request removes the key when value is empty.
struct ConfigurationUpdate final {
	ConfigurationSource source;
	std::string key;
	std::optional<ConfigurationValue> value;
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

struct ConfigurationReplaceSource final {
	ConfigurationSource source;
	std::vector<ConfigurationEntry> entries;
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

//! One source replacement within an atomic multi-source transaction. A
//! settings.json document uses this to commit its base layer and every
//! language override as one semantic change.
struct ConfigurationSourceReplacement final {
	ConfigurationSource source;
	std::vector<ConfigurationEntry> entries;
	std::optional<std::uint64_t> expectedRevision;
};

struct ConfigurationReplaceSources final {
	std::vector<ConfigurationSourceReplacement> replacements;
	std::string operationId;
};

struct ConfigurationResult final {
	EConfigurationOutcome outcome = EConfigurationOutcome::Unsupported;
	std::uint64_t revision = 0;
	std::string diagnostic;
};

struct ConfigurationSourceRevision final {
	ConfigurationSource source;
	std::uint64_t revision = 0;
};

//! Terminal result for an all-or-nothing multi-source replacement. Revisions
//! are returned in request order, including unchanged sources.
struct ConfigurationBatchResult final {
	EConfigurationOutcome outcome = EConfigurationOutcome::Unsupported;
	std::vector<ConfigurationSourceRevision> revisions;
	std::string diagnostic;
};

struct ConfigurationLookupResult final {
	EConfigurationOutcome outcome = EConfigurationOutcome::Unsupported;
	std::optional<ConfigurationValue> value;
	std::string diagnostic;
};

//! A coherent set of effective values from one committed service revision.
//! Values retain the caller's request order, including duplicate keys.
struct ConfigurationReadSnapshot final {
	std::uint64_t revision = 0;
	std::vector<ConfigurationValue> values;
};

//! Result of an all-or-nothing multi-key configuration read. A failed read
//! never returns a partial snapshot.
struct ConfigurationReadSnapshotResult final {
	EConfigurationOutcome outcome = EConfigurationOutcome::Unsupported;
	std::optional<ConfigurationReadSnapshot> snapshot;
	std::string diagnostic;
};

//! One contribution considered by Inspect, in low-to-high precedence order.
struct ConfigurationProvenance final {
	EConfigurationScope scope = EConfigurationScope::Default;
	std::string sourceId;
	std::uint64_t revision = 0;
	ConfigurationValue value;
	std::int32_t priority = 0;
};

struct ConfigurationInspection final {
	EConfigurationOutcome outcome = EConfigurationOutcome::Unsupported;
	std::optional<ConfigurationValue> effectiveValue;
	std::vector<ConfigurationProvenance> provenance;
	std::string diagnostic;
};

struct ConfigurationChange final {
	std::string key;
	ConfigurationTarget target;
	ConfigurationValue previousValue;
	ConfigurationValue effectiveValue;
};

using ConfigurationListener = std::function<void(const std::vector<ConfigurationChange>&)>;

//! Move-only RAII subscription. Destroying it unregisters the listener.
class ConfigurationSubscription final {
public:
	ConfigurationSubscription() = default;
	explicit ConfigurationSubscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}
	~ConfigurationSubscription() { Reset(); }
	ConfigurationSubscription(const ConfigurationSubscription&) = delete;
	ConfigurationSubscription& operator=(const ConfigurationSubscription&) = delete;
	ConfigurationSubscription(ConfigurationSubscription&& other) noexcept;
	ConfigurationSubscription& operator=(ConfigurationSubscription&& other) noexcept;

	void Reset() noexcept;
	bool IsActive() const noexcept { return static_cast<bool>(m_unsubscribe); }

private:
	std::function<void()> m_unsubscribe;
};

} // namespace config
