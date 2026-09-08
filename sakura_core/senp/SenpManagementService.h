/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace senp {

struct LanguageContribution final {
	std::wstring id;
	std::vector<std::wstring> aliases;
	std::vector<std::wstring> extensions;
	std::vector<std::wstring> filenames;
	std::vector<std::wstring> filenamePatterns;
	std::vector<std::wstring> mimetypes;
	std::wstring firstLine;
	std::wstring configuration;
	[[nodiscard]] bool operator==(const LanguageContribution&) const = default;
};

struct GrammarContribution final {
	std::wstring language;
	std::wstring scopeName;
	std::wstring path;
	std::vector<std::wstring> injectTo;
	[[nodiscard]] bool operator==(const GrammarContribution&) const = default;
};

struct ViewContainerContribution final {
	std::wstring id;
	std::wstring title;
	std::wstring icon;
	std::int32_t order{};
	[[nodiscard]] bool operator==(const ViewContainerContribution&) const = default;
};

struct ViewContribution final {
	std::wstring id;
	std::wstring containerId;
	std::wstring title;
	std::wstring provider;
	std::int32_t order{};
	[[nodiscard]] bool operator==(const ViewContribution&) const = default;
};

struct CommandContribution final {
	std::wstring command;
	std::wstring title;
	[[nodiscard]] bool operator==(const CommandContribution&) const = default;
};

//! Package-authority metadata, not permission to execute or issue tool grants.
struct RuntimeContribution final {
	std::uint32_t schemaVersion{ 1 };
	std::wstring abi;
	std::vector<std::wstring> activationEvents;
	std::vector<std::wstring> capabilities;
	std::vector<CommandContribution> commands;
	[[nodiscard]] bool operator==(const RuntimeContribution&) const = default;
};

enum class EManagementState : std::uint8_t {
	Created,
	Ready,
	ReadyWithDiagnostics,
	Failed,
	Stopped,
};

struct ExtensionDescriptor final {
	std::wstring id;
	std::wstring displayName;
	std::wstring version;
	std::wstring publisher;
	std::wstring description;
	std::wstring readme;
	std::wstring extensionPath;
	std::wstring modulePath;
	//! SHA-256 of the exact runtime component bytes validated by the package
	//! authority. Empty only for declarative extensions without a module.
	std::wstring moduleSha256;
	std::wstring archiveSha256;
	bool installed = true;
	bool builtIn = false;
	bool enabled = false;
	bool signedPackage = false;
	bool contributesIndentDecorations = false;
	std::vector<LanguageContribution> languages;
	std::vector<GrammarContribution> grammars;
	std::vector<ViewContainerContribution> viewContainers;
	std::vector<ViewContribution> views;
	std::wstring trust;
	RuntimeContribution runtime;
	[[nodiscard]] bool operator==(const ExtensionDescriptor&) const = default;
};

//! Decode bounded output from the package authority. These functions perform
//! no filesystem access and do not make unverified manifests installable.
[[nodiscard]] std::optional<std::vector<ExtensionDescriptor>> DecodeInstalledExtensions(std::string_view json);
[[nodiscard]] std::optional<ExtensionDescriptor> DecodeBuiltInExtension(std::string_view json);

struct ManagementSnapshot final {
	EManagementState state = EManagementState::Created;
	std::uint64_t revision = 0;
	std::vector<ExtensionDescriptor> extensions;
	std::wstring diagnostic;
};

enum class EManagementOperationStatus : std::uint8_t {
	Succeeded,
	AlreadyReady,
	InvalidRequest,
	Unavailable,
	Failed,
	Stopped,
};

struct ManagementOperationResult final {
	EManagementOperationStatus status = EManagementOperationStatus::Failed;
	ManagementSnapshot snapshot;
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EManagementOperationStatus::Succeeded
			|| status == EManagementOperationStatus::AlreadyReady;
	}
};

class ISenpManagementService {
public:
	virtual ~ISenpManagementService() = default;
	[[nodiscard]] virtual ManagementOperationResult Start() = 0;
	[[nodiscard]] virtual ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view packagePath, bool enable) = 0;
	[[nodiscard]] virtual ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view extensionId) = 0;
	[[nodiscard]] virtual ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view extensionId) = 0;
	[[nodiscard]] virtual ManagementOperationResult Refresh() = 0;
	virtual void Stop() noexcept = 0;
	[[nodiscard]] virtual ManagementSnapshot Snapshot() const = 0;
};

//! Owns profile-scoped SENP package state. It never loads extension code; the
//! runtime service consumes only enabled module paths from this authority.
class CWin32SenpManagementService final : public ISenpManagementService {
public:
	explicit CWin32SenpManagementService(std::wstring profileRoot);
	~CWin32SenpManagementService() override;

	[[nodiscard]] ManagementOperationResult Start() override;
	[[nodiscard]] ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view packagePath, bool enable) override;
	[[nodiscard]] ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view extensionId) override;
	[[nodiscard]] ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view extensionId) override;
	[[nodiscard]] ManagementOperationResult Refresh() override;
	void Stop() noexcept override;
	[[nodiscard]] ManagementSnapshot Snapshot() const override;

private:
	[[nodiscard]] ManagementOperationResult LoadBuiltInCatalog();
	[[nodiscard]] ManagementOperationResult ReloadInstalled();
	[[nodiscard]] ManagementOperationResult Terminal(
		EManagementOperationStatus status, std::wstring diagnostic = {});
	[[nodiscard]] bool IsStopped() const noexcept;

	const std::wstring m_profileRoot;
	const std::wstring m_installRoot;
	mutable std::mutex m_mutex;
	ManagementSnapshot m_snapshot;
	std::vector<ExtensionDescriptor> m_builtInCatalog;
	std::vector<std::wstring> m_uninstalledBuiltIns;
};

} // namespace senp
