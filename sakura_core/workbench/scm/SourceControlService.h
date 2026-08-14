/*! @file
 * @brief VS Code-compatible source-control provider model.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! A source-control provider is fenced to the extension generation that created it.
struct ScmOwner final {
	std::string ownerId;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const ScmOwner&) const noexcept = default;
};

//! The public VS Code SourceControlInputBox state, copied at the workbench boundary.
struct ScmInputBoxState final {
	std::string value;
	std::string placeholder;
	bool enabled{ true };
	bool visible{ true };

	[[nodiscard]] bool operator==(const ScmInputBoxState&) const noexcept = default;
};

//! A serialized VS Code Command attached to a source-control resource.
struct ScmCommand final {
	std::string command;
	std::string title;
	//! `vscode.Command.tooltip`. Upstream's status bar and the repository row's
	//! toolbar both render it as the hover text, and it is where the Git
	//! extension puts the remote name and the commit counts that the short
	//! title has no room for. Empty means the command contributes no tooltip.
	std::string tooltip;
	std::string argumentsJson{ "[]" };

	[[nodiscard]] bool operator==(const ScmCommand&) const noexcept = default;
};

//! The stable public SourceControlResourceState projection.
struct ScmResourceState final {
	platform::uri::Uri resourceUri;
	std::optional<ScmCommand> command;
	std::optional<std::string> tooltip;
	std::optional<std::string> lightIconPath;
	std::optional<std::string> darkIconPath;
	bool strikeThrough{};
	bool faded{};
	std::string contextValue;
};

struct ScmResourceGroupState final {
	ScmOwner owner;
	std::string providerHandle;
	std::string id;
	std::string label;
	bool hideWhenEmpty{};
	std::string contextValue;
	std::vector<ScmResourceState> resources;
};

//! The provider fields that are visible to a native SCM projection.
struct ScmProviderState final {
	ScmOwner owner;
	std::string handle;
	std::string id;
	//! `ISCMProvider.label`: what the source-control system calls itself, e.g.
	//! "Git". It identifies the provider, not the repository.
	std::string label;
	/*!
		@brief `ISCMProvider.name`: what this particular repository is called.

		`MainThreadSCMProvider` derives it from the workspace folder whose URI
		matches `rootUri`, else from `basename(rootUri)`, and its getter falls
		back to `label` when neither applies. `RepositoryRenderer` renders
		`name` as the row's label and `label` only inside the row's title, so
		collapsing the two would make the repository row read "Git" instead of
		naming the repository. Empty here is upstream's `undefined`; read it
		through `Name()` rather than the field so the fallback is not skipped.
	*/
	std::string name;
	std::optional<platform::uri::Uri> rootUri;
	ScmInputBoxState inputBox;
	std::optional<std::int32_t> count;
	std::optional<std::string> commitTemplate;
	std::optional<ScmCommand> acceptInputCommand;
	std::vector<ScmCommand> statusBarCommands;
	std::vector<ScmResourceGroupState> groups;

	//! Upstream's `get name()`: the derived name, or `label` when there is none.
	[[nodiscard]] const std::string& Name() const noexcept { return name.empty() ? label : name; }
};

struct ScmGlobalInputState final {
	ScmOwner owner;
	std::string handle;
	ScmInputBoxState inputBox;
};

struct ScmServiceSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::optional<ScmGlobalInputState> globalInput;
	std::vector<ScmProviderState> providers;
};

struct ScmCreateProviderRequest final {
	ScmProviderState provider;
};

struct ScmUpdateProviderRequest final {
	ScmOwner owner;
	std::string handle;
	std::optional<std::string> label;
	std::optional<ScmInputBoxState> inputBox;
	std::optional<std::int32_t> count;
	bool clearCount{};
	std::optional<std::string> commitTemplate;
	bool clearCommitTemplate{};
	std::optional<ScmCommand> acceptInputCommand;
	bool clearAcceptInputCommand{};
	std::optional<std::vector<ScmCommand>> statusBarCommands;
	bool clearStatusBarCommands{};
};

struct ScmCreateGroupRequest final {
	ScmResourceGroupState group;
};

struct ScmUpdateGroupRequest final {
	ScmOwner owner;
	std::string providerHandle;
	std::string groupId;
	std::optional<std::string> label;
	std::optional<bool> hideWhenEmpty;
	std::optional<std::string> contextValue;
};

struct ScmReplaceResourcesRequest final {
	ScmOwner owner;
	std::string providerHandle;
	std::string groupId;
	std::vector<ScmResourceState> resources;
};

struct ScmInputBoxUpdateRequest final {
	ScmOwner owner;
	std::string handle;
	ScmInputBoxState inputBox;
	bool global{};
};

struct ScmDisposeProviderRequest final {
	ScmOwner owner;
	std::string handle;
};

struct ScmDisposeGroupRequest final {
	ScmOwner owner;
	std::string providerHandle;
	std::string groupId;
};

enum class EScmOperationStatus : std::uint8_t {
	Succeeded,
	Replayed,
	NotApplicable,
	Stopped,
	InvalidOwner,
	InvalidProvider,
	InvalidGroup,
	InvalidResource,
	InvalidPayload,
	OwnerLimitExceeded,
	ProviderLimitExceeded,
	GroupLimitExceeded,
	ResourceLimitExceeded,
	PayloadLimitExceeded,
	OwnerGenerationConflict,
	RevisionExhausted,
};

struct ScmOperationResult final {
	EScmOperationStatus status{ EScmOperationStatus::InvalidPayload };
	std::uint64_t revision{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EScmOperationStatus::Succeeded || status == EScmOperationStatus::Replayed;
	}
};

enum class EScmChangeKind : std::uint8_t {
	ProviderCreated,
	ProviderUpdated,
	ProviderDisposed,
	GroupCreated,
	GroupUpdated,
	GroupDisposed,
	ResourcesReplaced,
	InputBoxChanged,
	OwnerDisposed,
};

struct ScmServiceChange final {
	std::uint64_t revision{};
	EScmChangeKind kind{ EScmChangeKind::ProviderUpdated };
	std::optional<std::string> providerHandle;
	std::optional<std::string> groupId;
};

using ScmServiceSubscriptionId = std::uint64_t;
using ScmServiceListener = std::function<void(const ScmServiceChange&)>;

struct SourceControlServiceLimits final {
	std::size_t maximumOwners{ 128 };
	std::size_t maximumProviders{ 128 };
	std::size_t maximumGroupsPerProvider{ 128 };
	std::size_t maximumResourcesPerGroup{ 4096 };
	std::size_t maximumPayloadBytes{ 1U << 20 };
	std::size_t maximumSubscriptions{ 256 };
};

//!
//! @brief Thread-safe SourceControl/SourceControlResourceGroup authority.
//!
//! This model deliberately owns no HWND, extension object, pipe, or filesystem handle.
//! Providers send stable snapshots into it; native projections consume immutable
//! snapshots. Owner generations remain fenced for the lifetime of the service so a late
//! callback from a disposed provider cannot resurrect it.
//!
class SourceControlService final {
public:
	explicit SourceControlService(SourceControlServiceLimits limits = {});
	~SourceControlService();

	SourceControlService(const SourceControlService&) = delete;
	SourceControlService& operator=(const SourceControlService&) = delete;

	[[nodiscard]] ScmOperationResult CreateProvider(const ScmCreateProviderRequest& request);
	[[nodiscard]] ScmOperationResult UpdateProvider(const ScmUpdateProviderRequest& request);
	[[nodiscard]] ScmOperationResult CreateGroup(const ScmCreateGroupRequest& request);
	[[nodiscard]] ScmOperationResult UpdateGroup(const ScmUpdateGroupRequest& request);
	[[nodiscard]] ScmOperationResult ReplaceResources(const ScmReplaceResourcesRequest& request);
	[[nodiscard]] ScmOperationResult UpdateInputBox(const ScmInputBoxUpdateRequest& request);
	[[nodiscard]] ScmOperationResult DisposeProvider(const ScmDisposeProviderRequest& request);
	[[nodiscard]] ScmOperationResult DisposeGroup(const ScmDisposeGroupRequest& request);
	[[nodiscard]] ScmOperationResult DisposeOwner(const ScmOwner& owner);
	[[nodiscard]] ScmOperationResult DisposeAll();
	[[nodiscard]] ScmOperationResult Stop() noexcept;

	[[nodiscard]] ScmServiceSnapshot Snapshot() const;
	[[nodiscard]] std::optional<ScmServiceSubscriptionId> Subscribe(ScmServiceListener listener);
	void Unsubscribe(ScmServiceSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::scm
