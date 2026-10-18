/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

private:
	// Declared in the private section for the same reason as LanguageContribution's.
	[[nodiscard]] friend bool operator==(const GrammarContribution&, const GrammarContribution&) = default;
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
	//! Non-empty when this row is the package tool's `kind:"invalid"` marker:
	//! a profile state entry that could not be read or verified. The `code`
	//! string comes from the tool's `SenpError` code, or `L"invalidRow"` when
	//! the line itself could not be decoded. Every consumer must skip an
	//! invalid row: it is never enabled, activated, or granted authority.
	std::wstring invalidCode;
	//! Bounded detail text from the invalid row, for display only.
	std::wstring invalidMessage;
	//! The `profiles/` state file name a `kind:"invalid"` row emits. It is the
	//! uninstall identity for a state entry whose manifest id cannot be proven,
	//! because an invalid row's `id` may be empty or a device name. Empty for rows
	//! decoded from `kind:"installed"` lines (their state is `<id>.json`).
	std::wstring stateName;
	[[nodiscard]] bool operator==(const ExtensionDescriptor&) const = default;
};

//! Decode bounded output from the package authority. These functions perform
//! no filesystem access and do not make unverified manifests installable.
//! `list` emits JSON Lines: one object per package, so one bad line degrades
//! to an invalid descriptor instead of discarding the whole listing.
[[nodiscard]] std::optional<std::vector<ExtensionDescriptor>> DecodeInstalledExtensions(std::string_view output);
[[nodiscard]] std::optional<ExtensionDescriptor> DecodeBuiltInExtension(std::string_view json);

//! Stable IDs of every package embedded in this executable, in catalog order.
//! Presentation that keys built-in metadata by ID is checked against this list.
[[nodiscard]] std::vector<std::wstring_view> EmbeddedBuiltInExtensionIds();

//! Startup's decision for an embedded built-in that the profile lacks or holds
//! in an outdated copy: true when startup installs or refreshes it before the
//! runtime starts. An explicit uninstall is honored across product updates. An
//! installed-by-default built-in is installed whenever the profile lacks it, so
//! a product update brings it into an existing profile; an offered one is only
//! refreshed when the profile already holds it as a built-in.
[[nodiscard]] bool StartupInstallsBuiltIn(std::wstring_view extensionId,
	bool explicitlyUninstalled, bool installedAsBuiltIn) noexcept;

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
	//! VS Code Enable/Disable on an installed non-built-in extension.
	[[nodiscard]] virtual ManagementOperationResult SetExtensionEnabled(
		std::wstring_view extensionId, bool enabled) = 0;
	//! Removes one profile state entry. `stateName` is the `profiles/` file
	//! name: `<id>.json` for an installed row, the invalid row's `stateName`
	//! field for a corrupt state. Not for built-ins (tombstones belong to
	//! UninstallBuiltInPackage).
	[[nodiscard]] virtual ManagementOperationResult UninstallPackage(
		std::wstring_view stateName) = 0;
	[[nodiscard]] virtual ManagementOperationResult Refresh() = 0;
	virtual void Stop() noexcept = 0;
	[[nodiscard]] virtual ManagementSnapshot Snapshot() const = 0;
};

namespace detail {

//! The decoded output of one `sakura-senp-tool.exe` invocation. `output` and
//! `error` are cut at 4 MiB; the truncation flags record that the producer was
//! allowed to write more, so a cut stream is never parsed as complete output.
struct ToolResult final {
	bool launched = false;
	bool timedOut = false;
	unsigned long exitCode = 0x80004005L /* E_FAIL */;
	std::string output;
	std::string error;
	bool outputTruncated = false;
	bool errorTruncated = false;
};

//! Test seam for the two OS dependencies of CWin32SenpManagementService:
//! spawning the package tool and reading this executable's embedded package
//! resources. Hooks are constructor-injected per instance, never global; an
//! empty std::function means the production path.
struct SenpServiceHooks final {
	std::function<ToolResult(const std::vector<std::wstring>& arguments)> runTool;
	std::function<std::optional<std::vector<std::byte>>(int resourceId)> embeddedResource;
};

//! The 4 MiB bound applied to one tool output stream. `pipe` is a Win32
//! HANDLE passed as void* so this header stays free of windows.h.
void ReadPipeBounded(void* pipe, std::string& target, bool& overflow) noexcept;

} // namespace detail

//! Owns profile-scoped SENP package state. It never loads extension code; the
//! runtime service consumes only enabled module paths from this authority.
class CWin32SenpManagementService final : public ISenpManagementService {
public:
	explicit CWin32SenpManagementService(std::wstring profileRoot,
		detail::SenpServiceHooks hooks = {});
	~CWin32SenpManagementService() override;

	[[nodiscard]] ManagementOperationResult Start() override;
	[[nodiscard]] ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view packagePath, bool enable) override;
	[[nodiscard]] ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view extensionId) override;
	[[nodiscard]] ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view extensionId) override;
	[[nodiscard]] ManagementOperationResult SetExtensionEnabled(
		std::wstring_view extensionId, bool enabled) override;
	[[nodiscard]] ManagementOperationResult UninstallPackage(
		std::wstring_view stateName) override;
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
	//! Built-in ids whose last Start could not install them. A later Start
	//! retries exactly these instead of answering AlreadyReady.
	std::vector<std::wstring> m_failedBuiltIns;
	//! Constructor-injected test seam; both std::functions stay empty in
	//! production.
	detail::SenpServiceHooks m_hooks;
};

} // namespace senp
