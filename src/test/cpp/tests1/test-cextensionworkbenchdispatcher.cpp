/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "config/BuiltinConfigurationDescriptors.h"
#include "extension/CExtensionSecretStorage.h"
#include "extension/CExtensionWorkbenchDispatcher.h"
#include "extension/CExtensionWorkbenchServiceBridge.h"
#include "platform/filesystem/IFileService.h"
#include "platform/profiles/ProfileBootstrapSnapshot.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include "platform/serialization/JsoncDocument.h"
#include "workbench/CWorkbenchRuntime.h"
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"

#include <filesystem>
#include <optional>
#include <string>

namespace {

class CExtensionWorkbenchDispatcherTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-workbench-dispatcher-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()));
		m_secrets = std::make_unique<CExtensionSecretStorage>(m_root);
		m_bridge = std::make_unique<CExtensionWorkbenchServiceBridge>(&m_markerService, &m_outputService);
		m_dispatcher = std::make_unique<CExtensionWorkbenchDispatcher>(
			m_context, m_commands, m_status, m_notifications, m_views, *m_secrets,
			m_diagnostics, m_quickInput, m_output, m_progress, m_bridge.get());
	}

	void TearDown() override
	{
		m_dispatcher.reset();
		m_secrets.reset();
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	SExtensionWorkbenchDispatchResult Dispatch(
		std::string method,
		std::string params,
		EExtensionRpcMessageKind kind = EExtensionRpcMessageKind::Notification)
	{
		return m_dispatcher->Dispatch({
			.eKind = kind,
			.sMethod = std::move(method),
			.sParamsJson = std::move(params),
		});
	}

	std::filesystem::path m_root;
	CExtensionContextKeys m_context;
	CExtensionCommandPalette m_commands;
	CExtensionStatusBar m_status;
	CExtensionNotificationCenter m_notifications;
	CExtensionViewRegistry m_views;
	CExtensionDiagnostics m_diagnostics;
	CExtensionQuickInput m_quickInput;
	CExtensionOutputChannel m_output;
	CExtensionProgressCenter m_progress;
	workbench::problems::MarkerService m_markerService;
	workbench::output::OutputService m_outputService;
	std::unique_ptr<CExtensionWorkbenchServiceBridge> m_bridge;
	std::unique_ptr<CExtensionSecretStorage> m_secrets;
	std::unique_ptr<CExtensionWorkbenchDispatcher> m_dispatcher;
};

// workspace/configuration/update ---------------------------------------------------------
//
// A real, started workbench::CWorkbenchRuntime is required to exercise the actual
// writeback path (CSettingsWritebackCoordinator -> CJsoncConfigurationEditor ->
// IFileService::ReadVersioned/ConditionalAtomicReplace), not just the "no runtime bound"
// short-circuit the plain CExtensionWorkbenchDispatcherTest fixture above can reach. This
// fake models exactly one backing document (the profile settings document the bridge's
// WriteGlobalConfiguration always targets) with real read-modify-write and Missing/Current
// conditional-replace semantics, so a sequence of dispatched updates round-trips like a
// real settings.json file would. Read() always fails with NotFound: CWorkbenchRuntime::Start()
// only needs that much to complete its own startup settings load with defaults, and the
// writeback path exclusively uses ReadVersioned/ConditionalAtomicReplace instead.
class ConfigurationWriteFakeFileService final : public platform::filesystem::IFileService {
public:
	platform::filesystem::FileResult<platform::filesystem::FileStat> Stat(const platform::uri::Uri&) override
	{
		return platform::filesystem::FileResult<platform::filesystem::FileStat>::Failure(
			platform::filesystem::EFileResultStatus::Unsupported);
	}

	platform::filesystem::FileResult<std::vector<platform::filesystem::DirectoryEntry>> Enumerate(
		const platform::uri::Uri&) override
	{
		return platform::filesystem::FileResult<std::vector<platform::filesystem::DirectoryEntry>>::Failure(
			platform::filesystem::EFileResultStatus::Unsupported);
	}

	platform::filesystem::FileResult<platform::filesystem::FileBytes> Read(
		const platform::uri::Uri&, const platform::filesystem::FileReadOptions&) override
	{
		return platform::filesystem::FileResult<platform::filesystem::FileBytes>::Failure(
			platform::filesystem::EFileResultStatus::NotFound);
	}

	platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri&, const platform::filesystem::FileReadOptions&) override
	{
		++m_readCalls;
		if (!m_document.has_value()) {
			return platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot>::Failure(
				platform::filesystem::EFileResultStatus::NotFound);
		}
		return platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot>::Success(
			{ platform::filesystem::FileBytes(m_document->begin(), m_document->end()), Version(m_version) });
	}

	platform::filesystem::FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri&, const platform::filesystem::FileBytes& bytes,
		const platform::filesystem::FileConditionalReplaceOptions& options) override
	{
		++m_replaceCalls;
		const bool expectsMissing =
			options.expectation == platform::filesystem::EFileConditionalReplaceExpectation::Missing;
		if (expectsMissing == m_document.has_value()) {
			return platform::filesystem::FileConditionalReplaceResult::Conflict();
		}
		++m_version;
		m_document = std::string(bytes.begin(), bytes.end());
		return platform::filesystem::FileConditionalReplaceResult::Success(Version(m_version));
	}

	platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>> Watch(
		const platform::uri::Uri&, const platform::filesystem::FileWatchOptions&) override
	{
		return platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>>::Failure(
			platform::filesystem::EFileResultStatus::Unsupported);
	}

	[[nodiscard]] std::string Document() const { return m_document.value_or(std::string()); }
	[[nodiscard]] int ReplaceCalls() const noexcept { return m_replaceCalls; }

private:
	static platform::filesystem::FileVersionToken Version(std::uint8_t value)
	{
		const std::uint8_t bytes[] { value };
		auto token = platform::filesystem::FileVersionToken::FromOpaqueBytes(bytes);
		return token.value_or(platform::filesystem::FileVersionToken());
	}

	std::optional<std::string> m_document;
	std::uint8_t m_version = 0;
	int m_readCalls = 0;
	int m_replaceCalls = 0;
};

constexpr char kConfigurationTestProfileId[] = "0123456789abcdef0123456789abcdef";

platform::profiles::ProfileBootstrapSnapshot ConfigurationTestProfile()
{
	auto resolved = platform::profiles::ResolveProfileBootstrapSnapshot(
		kConfigurationTestProfileId, 7, L"C:\\Profiles\\Sakura");
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

platform::profiles::UserDataProfileBootstrapSnapshot ConfigurationTestUserDataProfile()
{
	platform::profiles::UserDataProfileRegistry registry;
	platform::profiles::UserDataProfileBootstrapRequest request {
		{ kConfigurationTestProfileId, 7 }, L"C:\\Profiles\\Sakura", {},
		platform::profiles::UserDataProfileResourceRootMode::LegacyControlRootForDefault,
	};
	auto resolved = platform::profiles::ResolveUserDataProfileBootstrap(request, registry);
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

workbench::WorkbenchBootstrapContext ConfigurationTestBootstrap()
{
	workbench::WorkbenchBootstrapRequest request {
		ConfigurationTestProfile(), ConfigurationTestUserDataProfile(), L"extension-configuration-update-test",
		std::nullopt, std::nullopt, {}, std::nullopt, std::nullopt,
	};
	auto resolved = workbench::ResolveWorkbenchBootstrapContext(std::move(request));
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.context);
}

//! Exercises workspace/configuration/update through a real, started CWorkbenchRuntime so the
//! full CExtensionWorkbenchServiceBridge::WriteGlobalConfiguration -> CWorkbenchRuntime::WriteSetting
//! -> CSettingsWritebackCoordinator::Write -> CJsoncConfigurationEditor::Edit chain is actually
//! traversed, not just the not-bound short circuit the plain dispatcher fixture above can reach.
class CExtensionWorkbenchDispatcherConfigurationUpdateTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-workbench-dispatcher-config-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()));
		m_secrets = std::make_unique<CExtensionSecretStorage>(m_root);

		auto ownedFiles = std::make_unique<ConfigurationWriteFakeFileService>();
		m_files = ownedFiles.get();
		workbench::WorkbenchRuntimeDependencies dependencies;
		dependencies.fileService = std::move(ownedFiles);
		m_runtime = std::make_unique<workbench::CWorkbenchRuntime>(
			ConfigurationTestBootstrap(), config::BuiltinConfigurationDescriptors(), std::move(dependencies));
		ASSERT_TRUE(m_runtime->Start().IsUsable());

		m_bridge = std::make_unique<CExtensionWorkbenchServiceBridge>(nullptr, nullptr, m_runtime.get());
		m_dispatcher = std::make_unique<CExtensionWorkbenchDispatcher>(
			m_context, m_commands, m_status, m_notifications, m_views, *m_secrets,
			m_diagnostics, m_quickInput, m_output, m_progress, m_bridge.get());
	}

	void TearDown() override
	{
		m_dispatcher.reset();
		m_bridge.reset();
		if (m_runtime) static_cast<void>(m_runtime->Stop());
		m_runtime.reset();
		m_secrets.reset();
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	SExtensionWorkbenchDispatchResult Dispatch(
		std::string method,
		std::string params,
		EExtensionRpcMessageKind kind = EExtensionRpcMessageKind::Request)
	{
		return m_dispatcher->Dispatch({
			.eKind = kind,
			.sMethod = std::move(method),
			.sParamsJson = std::move(params),
		});
	}

	std::filesystem::path m_root;
	CExtensionContextKeys m_context;
	CExtensionCommandPalette m_commands;
	CExtensionStatusBar m_status;
	CExtensionNotificationCenter m_notifications;
	CExtensionViewRegistry m_views;
	CExtensionDiagnostics m_diagnostics;
	CExtensionQuickInput m_quickInput;
	CExtensionOutputChannel m_output;
	CExtensionProgressCenter m_progress;
	ConfigurationWriteFakeFileService* m_files = nullptr;
	std::unique_ptr<workbench::CWorkbenchRuntime> m_runtime;
	std::unique_ptr<CExtensionWorkbenchServiceBridge> m_bridge;
	std::unique_ptr<CExtensionSecretStorage> m_secrets;
	std::unique_ptr<CExtensionWorkbenchDispatcher> m_dispatcher;
};

TEST_F(CExtensionWorkbenchDispatcherTest, RoutesCommandsStatusBarAndTreeViewsWithOwnership)
{
	auto registration = Dispatch("workbench/extensions/register", R"({
		"extensionId":"test.sample","generation":4,
		"commands":[{"id":"test.run","title":"Run","category":"Test","enablement":"sample.enabled"}]
	})");
	ASSERT_TRUE(registration.success) << registration.errorMessage;
	EXPECT_TRUE(m_commands.Contains(L"test.run"));

	auto context = Dispatch("workbench/context/set",
		R"({"key":"sample.enabled","value":true,"extensionId":"test.sample","generation":4})",
		EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(context.success) << context.errorMessage;
	const auto palette = m_commands.Search(L"run", m_context);
	ASSERT_EQ(1u, palette.size());
	EXPECT_TRUE(palette.front().enabled);

	auto status = Dispatch("workbench/statusBar/update", R"json({
		"handle":"status:test.sample:4:1","itemId":"test.status","extensionId":"test.sample","generation":4,
		"alignment":"right","priority":20,"name":"Sample Status","text":"$(check) Ready","tooltip":{"markdown":"All good"},
		"command":{"command":"test.run"},"accessibilityInformation":{"label":"Ready"},"visible":true
	})json");
	ASSERT_TRUE(status.success) << status.errorMessage;
	const auto statusItems = m_status.Snapshot();
	ASSERT_EQ(1u, statusItems.size());
	EXPECT_EQ(L"Sample Status", statusItems.front().name);
	EXPECT_EQ(L"test.run", statusItems.front().command);
	EXPECT_EQ(L"All good", statusItems.front().tooltip);
	EXPECT_FALSE(Dispatch("workbench/statusBar/remove",
		R"({"handle":"status:test.sample:4:1","extensionId":"other.extension","generation":4})").success);

	auto view = Dispatch("workbench/views/register", R"({
		"handle":"view:test.sample:4:2","viewId":"test.view","extensionId":"test.sample","generation":4,
		"title":"Sample View","canSelectMany":true,"showCollapseAll":true
	})");
	ASSERT_TRUE(view.success) << view.errorMessage;
	auto children = m_dispatcher->ApplyTreeChildrenResult(
		L"view:test.sample:4:2", L"", L"test.sample", 4,
		R"({"items":[{"handle":"item:1","label":"Root","id":"root","description":"one",
			"tooltip":{"markdown":"Root item"},"command":{"command":"test.run"},"collapsibleState":1}]})");
	ASSERT_TRUE(children.success) << children.errorMessage;
	const auto roots = m_views.Children(L"view:test.sample:4:2");
	ASSERT_EQ(1u, roots.size());
	EXPECT_EQ(L"Root", roots.front().label);
	EXPECT_EQ(L"test.run", roots.front().command);
}

TEST_F(CExtensionWorkbenchDispatcherTest, PreservesTrustedStatusBarTooltipCommands)
{
	auto status = Dispatch("workbench/statusBar/update", R"json({
		"handle":"status:test.sample:4:1","itemId":"test.status","extensionId":"test.sample","generation":4,
		"alignment":"right","priority":20,"text":"Ready",
		"tooltip":{"markdown":"[Run](command:test.run \"Run\")",
			"isTrusted":{"enabledCommands":["test.run","workbench.action.openSettings"]},
			"supportThemeIcons":false},
		"visible":true
	})json");
	ASSERT_TRUE(status.success) << status.errorMessage;
	const auto statusItems = m_status.Snapshot();
	ASSERT_EQ(1u, statusItems.size());
	EXPECT_FALSE(statusItems.front().tooltipIsTrusted);
	ASSERT_EQ(2u, statusItems.front().tooltipTrustedCommands.size());
	EXPECT_EQ(L"test.run", statusItems.front().tooltipTrustedCommands[0]);
	EXPECT_EQ(L"workbench.action.openSettings", statusItems.front().tooltipTrustedCommands[1]);
}

TEST_F(CExtensionWorkbenchDispatcherTest, RoutesLegacyDpapiSecretsAndRejectsEnumeration)
{
	auto stored = Dispatch("secrets/store",
		R"({"extensionId":"test.sample","key":"token","value":"\u79d8\u5bc6-value"})",
		EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(stored.success) << stored.errorMessage;
	const auto persisted = m_secrets->Get(L"test.sample", L"token");
	ASSERT_TRUE(persisted.success && persisted.value.has_value());
	EXPECT_EQ(L"秘密-value", *persisted.value);
	auto read = Dispatch("secrets/get",
		R"({"extensionId":"test.sample","key":"token"})",
		EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(read.success) << read.errorMessage;
	EXPECT_NE(std::string::npos, read.resultJson.find("value"));
	auto keys = Dispatch("secrets/keys",
		R"({"extensionId":"test.sample"})", EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(keys.success);
	EXPECT_EQ(-32601, keys.errorCode);
	EXPECT_NE(std::string::npos, keys.errorMessage.find("UnsupportedCapability"));
	EXPECT_EQ(std::string::npos, keys.errorMessage.find("token"));
	EXPECT_EQ(std::string::npos, keys.resultJson.find("token"));
	EXPECT_TRUE(Dispatch("secrets/delete",
		R"({"extensionId":"test.sample","key":"token"})", EExtensionRpcMessageKind::Request).success);
}

TEST_F(CExtensionWorkbenchDispatcherTest, ResolvesNotificationActionsAndCleansGenerationState)
{
	m_dispatcher->SetNotificationHandler([](const SExtensionNotification& notification) {
		EXPECT_EQ(EExtensionNotificationSeverity::Warning, notification.severity);
		EXPECT_EQ(2u, notification.actions.size());
		return std::optional<std::size_t>(1);
	});
	auto notification = Dispatch("workbench/notification/show", R"({
		"extensionId":"test.sample","generation":9,"severity":"warning","message":"Continue?",
		"detail":"Choose","actions":[{"title":"Yes"},{"title":"No"}],"modal":true
	})", EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(notification.success) << notification.errorMessage;
	EXPECT_EQ(R"({"selectedIndex":1})", notification.resultJson);
	EXPECT_TRUE(m_notifications.Pending().empty());

	ASSERT_TRUE(Dispatch("workbench/extensions/register",
		R"({"extensionId":"test.sample","generation":9,"commands":[{"id":"test.clean","title":"Clean"}]})").success);
	ASSERT_TRUE(Dispatch("workbench/statusBar/update",
		R"({"handle":"status:9","extensionId":"test.sample","generation":9,"text":"active","visible":true})").success);
	auto removed = Dispatch("workbench/extensions/removeGeneration",
		R"({"extensionId":"test.sample","generation":9})");
	ASSERT_TRUE(removed.success) << removed.errorMessage;
	EXPECT_FALSE(m_commands.Contains(L"test.clean"));
	EXPECT_TRUE(m_status.Snapshot().empty());
}

TEST_F(CExtensionWorkbenchDispatcherTest, DefersNonModalNotificationUntilUiResolvesIt)
{
	std::uint64_t presentedId = 0;
	std::string presentedRequestId;
	m_dispatcher->SetDeferredNotificationHandler(
		[&](const SExtensionNotification& notification, const SExtensionRpcMessage& request) {
			presentedId = notification.id;
			presentedRequestId = request.sIdJson;
			return true;
		});

	const auto result = m_dispatcher->Dispatch({
		.eKind = EExtensionRpcMessageKind::Request,
		.sIdJson = "17",
		.sMethod = "workbench/notification/show",
		.sParamsJson = R"({"extensionId":"test.sample","generation":3,
			"message":"Copied","detail":"Markdown format"})",
	});
	ASSERT_TRUE(result.success) << result.errorMessage;
	EXPECT_TRUE(result.responseDeferred);
	EXPECT_EQ("17", presentedRequestId);
	ASSERT_EQ(1u, m_notifications.Pending().size());
	EXPECT_EQ(presentedId, m_notifications.Pending().front().id);

	ASSERT_TRUE(m_notifications.Resolve(presentedId, std::nullopt));
	EXPECT_TRUE(m_notifications.TakeCompletion(presentedId).has_value());
}

TEST_F(CExtensionWorkbenchDispatcherTest, RoutesDiagnosticsOutputAndProgressWithGenerationCleanup)
{
	auto diagnostics = Dispatch("languages/diagnostics/set", R"({
		"extensionId":"test.sample","generation":12,"collection":"lint","uri":"file:///test.md",
		"diagnostics":[{"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":7}},
		"message":"Problem","severity":1,"source":"sample","code":"W1"}]
	})");
	ASSERT_TRUE(diagnostics.success) << diagnostics.errorMessage;
	ASSERT_EQ(1u, m_diagnostics.ForUri(L"file:///test.md").size());

	ASSERT_TRUE(Dispatch("workbench/output/create", R"({
		"handle":"output:12","extensionId":"test.sample","generation":12,"name":"Sample","operationId":"output-create-12"
	})").success);
	ASSERT_TRUE(Dispatch("workbench/output/append", R"({
		"handle":"output:12","extensionId":"test.sample","generation":12,"value":"line one\n","operationId":"output-append-12"
	})").success);
	ASSERT_TRUE(Dispatch("workbench/output/show", R"({
		"handle":"output:12","extensionId":"test.sample","generation":12,"operationId":"output-show-12"
	})").success);
	auto channels = m_output.Snapshot();
	ASSERT_EQ(1u, channels.size());
	EXPECT_TRUE(channels.front().visible);
	EXPECT_EQ(L"line one\n", channels.front().text);

	ASSERT_TRUE(Dispatch("workbench/progress/start", R"({
		"handle":"progress:12","extensionId":"test.sample","generation":12,
		"options":{"title":"Indexing","cancellable":true}
	})").success);
	ASSERT_TRUE(Dispatch("workbench/progress/report", R"({
		"handle":"progress:12","extensionId":"test.sample","generation":12,
		"value":{"message":"Halfway","increment":50}
	})").success);
	ASSERT_EQ(1u, m_progress.Snapshot().size());
	EXPECT_EQ(L"Halfway", m_progress.Snapshot().front().message);

	auto removed = Dispatch("workbench/extensions/removeGeneration",
		R"({"extensionId":"test.sample","generation":12})");
	ASSERT_TRUE(removed.success) << removed.errorMessage;
	EXPECT_TRUE(m_diagnostics.Problems().empty());
	EXPECT_TRUE(m_output.Snapshot().empty());
	EXPECT_TRUE(m_progress.Snapshot().empty());
	EXPECT_TRUE(m_markerService.Snapshot().resources.empty());
	EXPECT_TRUE(m_outputService.Snapshot().channels.empty());
}

TEST_F(CExtensionWorkbenchDispatcherTest, UsesMarkerServiceForAtomicCollectionClearAndOwnerGenerations)
{
	const auto set = [this](std::string_view owner, const std::uint64_t generation, std::string_view collection,
		std::string_view uri, std::string_view message) {
		return Dispatch("languages/diagnostics/set", std::string(R"({"extensionId":")") + std::string(owner) +
			R"(","generation":)" + std::to_string(generation) + R"(,"collection":")" + std::string(collection) +
			R"(","uri":")" + std::string(uri) + R"(","diagnostics":[{"range":{"start":{"line":0,"character":1},"end":{"line":0,"character":3}},"message":")" +
			std::string(message) + R"(","severity":1,"source":"lint","code":"W1"}]})");
	};
	ASSERT_TRUE(set("one.extension", 4, "lint", "file:///one.md", "one").success);
	ASSERT_TRUE(set("one.extension", 4, "lint", "file:///two.md", "two").success);
	ASSERT_TRUE(set("one.extension", 4, "single", "file:///delete.md", "delete").success);
	ASSERT_TRUE(set("sibling.extension", 3, "lint", "file:///sibling.md", "sibling").success);
	ASSERT_TRUE(Dispatch("languages/diagnostics/delete",
		R"({"extensionId":"one.extension","generation":4,"collection":"single","uri":"file:///delete.md"})").success);
	EXPECT_TRUE(Dispatch("languages/diagnostics/delete",
		R"({"extensionId":"one.extension","generation":4,"collection":"single","uri":"file:///delete.md"})").success);
	ASSERT_TRUE(Dispatch("languages/diagnostics/clear",
		R"({"extensionId":"one.extension","generation":4,"collection":"lint"})").success);
	EXPECT_TRUE(Dispatch("languages/diagnostics/clear",
		R"({"extensionId":"one.extension","generation":4,"collection":"lint"})").success);
	const auto cleared = m_markerService.Snapshot();
	ASSERT_EQ(1u, cleared.resources.size());
	EXPECT_EQ(L"file:///sibling.md", cleared.resources.front().resource.ToString());
	EXPECT_TRUE(m_diagnostics.ForUri(L"file:///one.md").empty());
	EXPECT_FALSE(set("one.extension", 3, "lint", "file:///stale.md", "stale").success);
	EXPECT_FALSE(Dispatch("languages/diagnostics/set", R"({
		"extensionId":"one.extension","generation":5,"collection":"lint","uri":"not a uri","diagnostics":[]
	})").success);
}

TEST_F(CExtensionWorkbenchDispatcherTest, UsesOutputServiceForAllOperationsReplayAndPreserveFocus)
{
	const auto dispatch = [this](std::string_view method, std::string_view operationId, std::string_view extra = {}) {
		return Dispatch(std::string(method), std::string(R"({"handle":"output:bridge","extensionId":"test.sample","generation":17,"operationId":")") +
			std::string(operationId) + R"(")" + std::string(extra) + "}");
	};
	ASSERT_TRUE((dispatch("workbench/output/create", "bridge-create", R"(,"name":"Bridge","languageId":"plaintext","source":"test")")).success);
	ASSERT_TRUE((dispatch("workbench/output/append", "bridge-append", R"(,"value":"one")")).success);
	ASSERT_TRUE((dispatch("workbench/output/append", "bridge-append", R"(,"value":"one")")).success);
	EXPECT_FALSE((dispatch("workbench/output/append", "bridge-append", R"(,"value":"conflict")")).success);
	ASSERT_TRUE((dispatch("workbench/output/replace", "bridge-replace", R"(,"value":"replacement")")).success);
	ASSERT_TRUE((dispatch("workbench/output/clear", "bridge-clear")).success);
	ASSERT_TRUE((dispatch("workbench/output/append", "bridge-append-two", R"(,"value":"two")")).success);
	ASSERT_TRUE((dispatch("workbench/output/show", "bridge-show", R"(,"preserveFocus":true)")).success);
	ASSERT_TRUE((dispatch("workbench/output/show", "bridge-show-again", R"(,"preserveFocus":true)")).success);
	ASSERT_TRUE((dispatch("workbench/output/hide", "bridge-hide")).success);
	const auto hidden = m_outputService.Snapshot();
	ASSERT_EQ(1u, hidden.channels.size());
	EXPECT_FALSE(hidden.channels.front().visible);
	EXPECT_TRUE(hidden.channels.front().lastShowPreservedFocus);
	EXPECT_EQ("two", hidden.channels.front().text);
	EXPECT_EQ(L"two", m_output.Snapshot().front().text); // replay did not duplicate the legacy projection.
	ASSERT_TRUE(dispatch("workbench/output/dispose", "bridge-dispose").success);
	EXPECT_TRUE(m_outputService.Snapshot().channels.empty());
}

TEST_F(CExtensionWorkbenchDispatcherTest, DisposesOnlyTheExactBridgeOwnerAndAllOwnersOnHostClear)
{
	const auto create = [this](std::string_view owner, const std::uint64_t generation, std::string_view handle, std::string_view operationId) {
		return Dispatch("workbench/output/create", std::string(R"({"handle":")") + std::string(handle) + R"(","extensionId":")" +
			std::string(owner) + R"(","generation":)" + std::to_string(generation) + R"(,"name":"Channel","operationId":")" + std::string(operationId) + R"("})");
	};
	ASSERT_TRUE(create("one.extension", 1, "output-one", "dispose-create-one").success);
	ASSERT_TRUE(create("two.extension", 1, "output-two", "dispose-create-two").success);
	ASSERT_TRUE(Dispatch("workbench/extensions/removeGeneration",
		R"({"extensionId":"one.extension","generation":1})").success);
	auto snapshot = m_outputService.Snapshot();
	ASSERT_EQ(1u, snapshot.channels.size());
	EXPECT_EQ("output-two", snapshot.channels.front().channelId);
	ASSERT_TRUE(create("three.extension", 1, "output-three", "dispose-create-three").success);
	ASSERT_TRUE(m_bridge->DisposeAll(m_diagnostics, m_output));
	EXPECT_TRUE(m_outputService.Snapshot().channels.empty());
}

TEST_F(CExtensionWorkbenchDispatcherTest, RecordsActivationFailureIntoTheExtensionHostLogChannelWithoutFailingTheRpc)
{
	// Real VS Code routes a normal-mode activation failure to the "Extension Host" log channel
	// instead of a modal; Dispatch must ack the RPC either way and only report the Output change
	// when the diagnostic was actually recorded.
	ASSERT_TRUE(m_outputService.Snapshot().channels.empty());
	auto failure = Dispatch("workbench/extensions/didFailActivation", R"({
		"extensionId":"test.sample","generation":4,"message":"TypeError: something went wrong"
	})");
	ASSERT_TRUE(failure.success) << failure.errorMessage;
	EXPECT_EQ(EExtensionWorkbenchChange::Output, failure.changes);

	const auto snapshot = m_outputService.Snapshot();
	ASSERT_EQ(1u, snapshot.channels.size());
	const auto& channel = snapshot.channels.front();
	EXPECT_EQ("Extension Host", channel.label);
	EXPECT_EQ(workbench::output::EOutputChannelKind::Log, channel.kind);
	ASSERT_EQ(1u, channel.logEntries.size());
	EXPECT_EQ(workbench::output::EOutputLogLevel::Error, channel.logEntries.front().level);
	EXPECT_NE(std::string::npos, channel.logEntries.front().message.find("test.sample"));
	EXPECT_NE(std::string::npos, channel.logEntries.front().message.find("4"));
	EXPECT_NE(std::string::npos, channel.logEntries.front().message.find("something went wrong"));

	// The channel is host-owned, not extension-owned: neither the exact owner-generation cleanup
	// that fires when the failed extension itself is torn down, nor a full host clear, may remove
	// it -- unlike every extension-owned output channel the bridge otherwise tracks.
	auto removed = Dispatch("workbench/extensions/removeGeneration",
		R"({"extensionId":"test.sample","generation":4})");
	ASSERT_TRUE(removed.success) << removed.errorMessage;
	ASSERT_EQ(1u, m_outputService.Snapshot().channels.size());
	ASSERT_TRUE(m_bridge->DisposeAll(m_diagnostics, m_output));
	ASSERT_EQ(1u, m_outputService.Snapshot().channels.size());

	// A second failure (from any extension) appends to the same lazily-created channel rather than
	// creating another one.
	ASSERT_TRUE(Dispatch("workbench/extensions/didFailActivation",
		R"({"extensionId":"other.sample","generation":1,"message":"second failure"})").success);
	const auto afterSecond = m_outputService.Snapshot();
	ASSERT_EQ(1u, afterSecond.channels.size());
	ASSERT_EQ(2u, afterSecond.channels.front().logEntries.size());
	EXPECT_NE(std::string::npos, afterSecond.channels.front().logEntries[1].message.find("second failure"));
}

TEST_F(CExtensionWorkbenchDispatcherTest, MapsLanguageStatusToNativeStatusBarAndCleansGeneration)
{
	auto updated = Dispatch("workbench/languageStatus/update", R"({
		"extensionId":"esbenp.prettier-vscode","generation":15,"id":"prettier.status",
		"severity":1,"text":"Prettier","detail":"Formatting is available","busy":true,
		"command":{"command":"prettier.open-output"},"accessibilityInformation":{"label":"Prettier ready"}
	})");
	ASSERT_TRUE(updated.success) << updated.errorMessage;
	ASSERT_EQ(EExtensionWorkbenchChange::StatusBar, updated.changes);
	const auto items = m_status.Snapshot();
	ASSERT_EQ(1u, items.size());
	EXPECT_EQ(L"$(sync~spin) Prettier", items.front().text);
	EXPECT_EQ(L"Formatting is available", items.front().tooltip);
	EXPECT_EQ(L"prettier.open-output", items.front().command);
	EXPECT_EQ(EExtensionStatusBarAlignment::Right, items.front().alignment);

	auto removed = Dispatch("workbench/extensions/removeGeneration",
		R"({"extensionId":"esbenp.prettier-vscode","generation":15})");
	ASSERT_TRUE(removed.success) << removed.errorMessage;
	EXPECT_TRUE(m_status.Snapshot().empty());
}

TEST_F(CExtensionWorkbenchDispatcherTest, MakesUnsupportedCapabilitiesObservableOncePerExtensionGeneration)
{
	const auto params = R"({
		"viewId":"sample.webview","extensionId":"sample.extension","generation":6,
		"error":{"code":"UnsupportedCapability","capability":"window.registerWebviewViewProvider"}
	})";
	auto first = Dispatch("workbench/webview/registerUnsupported", params);
	ASSERT_TRUE(first.success) << first.errorMessage;
	EXPECT_EQ(EExtensionWorkbenchChange::Output, first.changes);
	auto duplicate = Dispatch("workbench/webview/registerUnsupported", params);
	ASSERT_TRUE(duplicate.success) << duplicate.errorMessage;
	EXPECT_EQ(EExtensionWorkbenchChange::None, duplicate.changes);
	const auto channels = m_output.Snapshot();
	ASSERT_EQ(1u, channels.size());
	EXPECT_TRUE(channels.front().visible);
	EXPECT_NE(std::wstring::npos, channels.front().text.find(L"sample.extension"));
	EXPECT_NE(std::wstring::npos, channels.front().text.find(L"window.registerWebviewViewProvider"));
	EXPECT_EQ(channels.front().text.find(L"UnsupportedCapability"),
		channels.front().text.rfind(L"UnsupportedCapability"));
}

TEST_F(CExtensionWorkbenchDispatcherTest, ExplicitlyAcceptsHostBackedLanguageProviderRegistration)
{
	auto registered = Dispatch("languages/provider/register", R"({
		"handle":"provider:sample.extension:4:1","extensionId":"sample.extension",
		"generation":4,"kind":"formatDocument","selector":"plaintext"
	})");
	ASSERT_TRUE(registered.success) << registered.errorMessage;
	EXPECT_EQ(EExtensionWorkbenchChange::None, registered.changes);
}

TEST_F(CExtensionWorkbenchDispatcherTest, ResolvesQuickPickAndInputBoxThroughNativeHandlers)
{
	m_dispatcher->SetQuickInputHandler([](const SExtensionQuickInputRequest& request) {
		SExtensionQuickInputCompletion result{ .id = request.id, .state = EExtensionQuickInputState::Accepted };
		if (request.kind == EExtensionQuickInputKind::QuickPick) result.selectedIndices = { 0, 2 };
		else result.value = L"typed value";
		return result;
	});
	auto pick = Dispatch("workbench/quickInput/showQuickPick", R"({
		"extensionId":"test.sample","generation":3,"options":{"title":"Choose","canPickMany":true},
		"items":[{"index":7,"label":"A"},{"index":8,"label":"B"},{"index":9,"label":"C"}]
	})", EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(pick.success) << pick.errorMessage;
	EXPECT_NE(std::string::npos, pick.resultJson.find("7"));
	EXPECT_NE(std::string::npos, pick.resultJson.find("9"));

	auto input = Dispatch("workbench/quickInput/showInputBox", R"({
		"extensionId":"test.sample","generation":3,"options":{"title":"Name","password":true}
	})", EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(input.success) << input.errorMessage;
	EXPECT_NE(std::string::npos, input.resultJson.find("typed value"));
}

TEST_F(CExtensionWorkbenchDispatcherTest, RejectsAttemptsToClaimBuiltInCommands)
{
	ASSERT_TRUE(m_commands.Register({ .id = L"sakura.file.open", .title = L"Open File", .builtIn = true }));
	auto result = Dispatch("workbench/extensions/register", R"({
		"extensionId":"evil.extension","generation":1,
		"commands":[{"id":"sakura.file.open","title":"Hijack"}]
	})");
	EXPECT_FALSE(result.success);
	EXPECT_EQ(-32011, result.errorCode);
}

TEST_F(CExtensionWorkbenchDispatcherTest, RejectsConfigurationUpdateWhenNoWorkbenchRuntimeIsBound)
{
	// This fixture's bridge is constructed with no CWorkbenchRuntime bound (the production
	// default before CEditWnd wires one in -- see extension/CLAUDE.md). Real VS Code throws
	// rather than silently succeeding when a target cannot be honored, so this must reject
	// with a typed failure rather than reporting success.
	auto result = Dispatch("workspace/configuration/update",
		R"({"key":"otakUsage.statusBarMode","value":"detailed","configurationTarget":1})",
		EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(result.success);
	EXPECT_EQ(-32001, result.errorCode);
	EXPECT_NE(std::string::npos, result.errorMessage.find("workbench settings owner is not available"));
}

TEST_F(CExtensionWorkbenchDispatcherTest,
	RejectsWorkspaceScopedAndAbsentConfigurationTargetsAsUnsupportedRegardlessOfRuntimeBinding)
{
	// ConfigurationTarget.Workspace (2), ConfigurationTarget.WorkspaceFolder (3), the boolean
	// `false` form, and both flavors of an absent target (JSON null and the field omitted
	// entirely -- real VS Code's own default-target rule resolves an absent target to either
	// Workspace or WorkspaceFolder, never Global) are all rejected as an explicit typed
	// UnsupportedCapability failure. This runs against a bridge with no runtime bound at all,
	// proving the rejection happens before ever touching runtime or workspace state -- so it
	// also covers "no workspace is open" as a subset of "Workspace/WorkspaceFolder is never
	// supported by this bridge," since there is no separate code path that first checks
	// whether a workspace happens to be open.
	const char* unsupportedParams[] = {
		R"({"key":"sample.key","value":"v","configurationTarget":2})",
		R"({"key":"sample.key","value":"v","configurationTarget":3})",
		R"({"key":"sample.key","value":"v","configurationTarget":false})",
		R"({"key":"sample.key","value":"v","configurationTarget":null})",
		R"({"key":"sample.key","value":"v"})",
	};
	for (const char* params : unsupportedParams) {
		auto result = Dispatch("workspace/configuration/update", params, EExtensionRpcMessageKind::Request);
		EXPECT_FALSE(result.success) << params;
		EXPECT_EQ(-32601, result.errorCode) << params;
		EXPECT_NE(std::string::npos, result.errorMessage.find("UnsupportedCapability")) << params;
	}
}

TEST_F(CExtensionWorkbenchDispatcherTest, RejectsMalformedConfigurationTargetValuesWithAGenericError)
{
	// A string, a non-integer number, and out-of-range integers are not merely an unsupported
	// *scope* -- they are not a ConfigurationTarget at all, so they get the generic malformed-
	// request error rather than the UnsupportedCapability wording reserved for well-formed but
	// unimplemented targets.
	const char* malformedParams[] = {
		R"({"key":"sample.key","value":"v","configurationTarget":"Global"})",
		R"({"key":"sample.key","value":"v","configurationTarget":1.5})",
		R"({"key":"sample.key","value":"v","configurationTarget":4})",
		R"({"key":"sample.key","value":"v","configurationTarget":-1})",
	};
	for (const char* params : malformedParams) {
		auto result = Dispatch("workspace/configuration/update", params, EExtensionRpcMessageKind::Request);
		EXPECT_FALSE(result.success) << params;
		EXPECT_EQ(-32602, result.errorCode) << params;
		EXPECT_EQ(std::string::npos, result.errorMessage.find("UnsupportedCapability")) << params;
	}
}

TEST_F(CExtensionWorkbenchDispatcherTest, RejectsConfigurationUpdatesWithMissingEmptyOrOversizedKeys)
{
	auto missingKey = Dispatch("workspace/configuration/update",
		R"({"value":"v","configurationTarget":1})", EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(missingKey.success);
	EXPECT_EQ(-32602, missingKey.errorCode);

	auto emptyKey = Dispatch("workspace/configuration/update",
		R"({"key":"","value":"v","configurationTarget":1})", EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(emptyKey.success);

	auto numericKey = Dispatch("workspace/configuration/update",
		R"({"key":42,"value":"v","configurationTarget":1})", EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(numericKey.success);

	// One byte past CJsoncDocument::kMaximumObjectKeyLength (64 KiB); the extension-supplied
	// key is untrusted and must be bounded before it ever reaches the document editor.
	const std::string oversizedKey(platform::serialization::CJsoncDocument::kMaximumObjectKeyLength + 1, 'k');
	const std::string oversizedKeyParams =
		R"({"key":")" + oversizedKey + R"(","value":"v","configurationTarget":1})";
	auto oversized = Dispatch("workspace/configuration/update", oversizedKeyParams, EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(oversized.success);
}

TEST_F(CExtensionWorkbenchDispatcherConfigurationUpdateTest,
	WritesAGlobalSettingUnderNumericAndBooleanConfigurationTargetsAndPersistsAcrossWrites)
{
	auto first = Dispatch("workspace/configuration/update",
		R"({"key":"otakUsage.statusBarMode","value":"detailed","configurationTarget":1})");
	ASSERT_TRUE(first.success) << first.errorMessage;
	EXPECT_EQ("{}", first.resultJson);
	EXPECT_EQ(EExtensionWorkbenchChange::None, first.changes);
	EXPECT_NE(std::string::npos, m_files->Document().find("otakUsage.statusBarMode"));
	EXPECT_NE(std::string::npos, m_files->Document().find("detailed"));

	// The boolean `true` form is equivalent to ConfigurationTarget.Global (1) and must reach
	// the same already-created document -- exercising the fake's "Current" conditional-replace
	// branch, as opposed to the first write's "Missing" branch.
	auto second = Dispatch("workspace/configuration/update",
		R"({"key":"otakUsage.statusBarMode","value":"compact","configurationTarget":true})");
	ASSERT_TRUE(second.success) << second.errorMessage;
	EXPECT_NE(std::string::npos, m_files->Document().find("compact"));
	EXPECT_EQ(std::string::npos, m_files->Document().find("detailed"));
	EXPECT_EQ(2, m_files->ReplaceCalls());
}

TEST_F(CExtensionWorkbenchDispatcherConfigurationUpdateTest, TreatsAnAbsentValueFieldAsRemovalOfAnExistingKey)
{
	auto created = Dispatch("workspace/configuration/update",
		R"({"key":"sample.toRemove","value":"temp","configurationTarget":1})");
	ASSERT_TRUE(created.success) << created.errorMessage;
	ASSERT_NE(std::string::npos, m_files->Document().find("sample.toRemove"));

	// A JSON payload with no "value" member mirrors Configuration.update(key, undefined):
	// vscode-api.cjs never serializes a "value" property when the caller's value is undefined,
	// so its absence in the wire payload -- not a literal JSON null -- is the removal signal.
	auto removed = Dispatch("workspace/configuration/update",
		R"({"key":"sample.toRemove","configurationTarget":1})");
	ASSERT_TRUE(removed.success) << removed.errorMessage;
	EXPECT_EQ(std::string::npos, m_files->Document().find("sample.toRemove"));
}

TEST_F(CExtensionWorkbenchDispatcherConfigurationUpdateTest,
	WritesIntoALanguageOverrideBlockWhenOverrideInLanguageIsSupplied)
{
	auto result = Dispatch("workspace/configuration/update",
		R"({"key":"editor.tabSize","value":2,"configurationTarget":1,"overrideInLanguage":"typescript"})");
	ASSERT_TRUE(result.success) << result.errorMessage;
	const auto document = m_files->Document();
	EXPECT_NE(std::string::npos, document.find("[typescript]"));
	EXPECT_NE(std::string::npos, document.find("editor.tabSize"));
}

TEST_F(CExtensionWorkbenchDispatcherConfigurationUpdateTest,
	RejectsAnOversizedOrTooDeeplyNestedConfigurationValuePayloadWithoutWritingAnything)
{
	// A string value one byte past the 1 MiB bound this dispatcher reuses from
	// CJsoncDocument::kMaximumStringLength for its own value-conversion budget.
	const std::string oversizedValue(1024 * 1024 + 1, 'a');
	auto oversized = Dispatch("workspace/configuration/update",
		R"({"key":"sample.big","value":")" + oversizedValue + R"(","configurationTarget":1})");
	EXPECT_FALSE(oversized.success);
	EXPECT_EQ(-32602, oversized.errorCode);
	EXPECT_TRUE(m_files->Document().empty());

	// An array nested 80 levels deep exceeds the 64-level depth bound.
	std::string deepValue = "0";
	for (int i = 0; i < 80; ++i) deepValue = "[" + deepValue + "]";
	auto deep = Dispatch("workspace/configuration/update",
		R"({"key":"sample.deep","value":)" + deepValue + R"(,"configurationTarget":1})");
	EXPECT_FALSE(deep.success);
	EXPECT_EQ(-32602, deep.errorCode);
	EXPECT_TRUE(m_files->Document().empty());
}

TEST_F(CExtensionWorkbenchDispatcherConfigurationUpdateTest,
	RejectsConfigurationUpdatesAfterTheWorkbenchRuntimeIsStopped)
{
	const auto stopped = m_runtime->Stop();
	ASSERT_EQ(workbench::EWorkbenchRuntimeResultCode::Stopped, stopped.code);

	auto result = Dispatch("workspace/configuration/update",
		R"({"key":"sample.key","value":"v","configurationTarget":1})");
	EXPECT_FALSE(result.success);
	EXPECT_EQ(-32001, result.errorCode);
	EXPECT_NE(std::string::npos, result.errorMessage.find("workbench settings owner is not available"));
	EXPECT_TRUE(m_files->Document().empty());
}

} // namespace
