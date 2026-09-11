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

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member, so it is declared alongside the (empty) private section rather
	// than as the struct's last public line.
	[[nodiscard]] friend bool operator==(const LanguageContribution&, const LanguageContribution&) = default;
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

//! One declared command. `icon` is the manifest's `$(codicon)` ThemeIcon or
//! package-relative image path, or empty when it names none; a path draws
//! only as an inline Tree item action. Immutable once constructed: a parsed
//! manifest command never changes shape after it is recorded.
class CommandContribution final {
public:
	CommandContribution(std::wstring command, std::wstring title, std::wstring icon = {}) noexcept
		: m_command(std::move(command)), m_title(std::move(title)), m_icon(std::move(icon))
	{
	}

	[[nodiscard]] const std::wstring& Command() const noexcept { return m_command; }
	[[nodiscard]] const std::wstring& Title() const noexcept { return m_title; }
	[[nodiscard]] const std::wstring& Icon() const noexcept { return m_icon; }

private:
	// The defaulted comparison is compiler-synthesized machinery, not part of
	// this type's public accessor surface, so it lives with the data it
	// compares rather than as the class's last public declaration.
	[[nodiscard]] friend bool operator==(const CommandContribution&, const CommandContribution&) = default;

	std::wstring m_command;
	std::wstring m_title;
	std::wstring m_icon;
};

//! One `menus["view/item/context"]` item: a declared, icon-bearing command drawn
//! inline on the rows whose contextValue contains every `contains` token and
//! equals every `equals` value. An empty `views` means every View of the package.
//! Built incrementally by its manifest parser (`command` is filled in only once
//! the enclosing declared command has been verified), hence the mutator methods
//! rather than a one-shot constructor.
class ViewItemMenuContribution final {
public:
	ViewItemMenuContribution() = default;
	//! Convenience constructor for a fully-known item (tests and fixtures that
	//! do not go through the incremental manifest parser).
	ViewItemMenuContribution(std::wstring command, std::vector<std::wstring> views,
		std::vector<std::wstring> contains, std::vector<std::wstring> equals) noexcept
		: m_command(std::move(command)), m_views(std::move(views)), m_contains(std::move(contains)),
		m_equals(std::move(equals))
	{
	}

	void SetCommand(std::wstring command) { m_command = std::move(command); }
	void AddView(std::wstring view) { m_views.push_back(std::move(view)); }
	void AddContains(std::wstring token) { m_contains.push_back(std::move(token)); }
	void AddEquals(std::wstring value) { m_equals.push_back(std::move(value)); }

	[[nodiscard]] const std::wstring& Command() const noexcept { return m_command; }
	[[nodiscard]] const std::vector<std::wstring>& Views() const noexcept { return m_views; }
	[[nodiscard]] const std::vector<std::wstring>& Contains() const noexcept { return m_contains; }
	[[nodiscard]] const std::vector<std::wstring>& Equals() const noexcept { return m_equals; }

private:
	// See CommandContribution: the defaulted comparison is not an accessor,
	// so it is declared with the private data it compares.
	[[nodiscard]] friend bool operator==(const ViewItemMenuContribution&, const ViewItemMenuContribution&) = default;

	std::wstring m_command;
	std::vector<std::wstring> m_views;
	std::vector<std::wstring> m_contains;
	std::vector<std::wstring> m_equals;
};

//! One `menus["view/title"]` item: a declared, icon-bearing command placed in
//! the inline `navigation` group of each View its `when` clause names.
class ViewTitleMenuContribution final {
public:
	ViewTitleMenuContribution(std::wstring command, std::vector<std::wstring> views) noexcept
		: m_command(std::move(command)), m_views(std::move(views))
	{
	}

	[[nodiscard]] const std::wstring& Command() const noexcept { return m_command; }
	[[nodiscard]] const std::vector<std::wstring>& Views() const noexcept { return m_views; }

private:
	// See CommandContribution: the defaulted comparison is not an accessor,
	// so it is declared with the private data it compares.
	[[nodiscard]] friend bool operator==(const ViewTitleMenuContribution&, const ViewTitleMenuContribution&) = default;

	std::wstring m_command;
	std::vector<std::wstring> m_views;
};

//! Package-authority metadata, not permission to execute or issue tool grants.
struct RuntimeContribution final {
	std::uint32_t schemaVersion{ 1 };
	std::wstring abi;
	std::vector<std::wstring> activationEvents;
	std::vector<std::wstring> capabilities;
	std::vector<CommandContribution> commands;
	//! Decided by sakura_senp, the only owner of the runtime ABI: false for an
	//! installed package built for another WIT world. It is listed so it can be
	//! refreshed or removed, and must never be activated or granted tools.
	bool compatible{ true };

	[[nodiscard]] const std::vector<ViewTitleMenuContribution>& ViewTitle() const noexcept { return m_viewTitle; }
	[[nodiscard]] std::vector<ViewTitleMenuContribution>& ViewTitle() noexcept { return m_viewTitle; }
	[[nodiscard]] const std::vector<ViewItemMenuContribution>& ViewItemContext() const noexcept
	{
		return m_viewItemContext;
	}
	[[nodiscard]] std::vector<ViewItemMenuContribution>& ViewItemContext() noexcept { return m_viewItemContext; }
	void AddViewTitle(ViewTitleMenuContribution item) { m_viewTitle.push_back(std::move(item)); }
	void AddViewItemContext(ViewItemMenuContribution item) { m_viewItemContext.push_back(std::move(item)); }

private:
	// See CommandContribution: the defaulted comparison is not an accessor,
	// so it is declared with the private data it compares.
	[[nodiscard]] friend bool operator==(const RuntimeContribution&, const RuntimeContribution&) = default;

	std::vector<ViewTitleMenuContribution> m_viewTitle;
	std::vector<ViewItemMenuContribution> m_viewItemContext;
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
