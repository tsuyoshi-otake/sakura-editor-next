/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionWorkbenchDispatcher.h"

#include <filesystem>

namespace {

class CExtensionWorkbenchDispatcherTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-workbench-dispatcher-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()));
		m_secrets = std::make_unique<CExtensionSecretStorage>(m_root);
		m_dispatcher = std::make_unique<CExtensionWorkbenchDispatcher>(
			m_context, m_commands, m_status, m_notifications, m_views, *m_secrets,
			m_diagnostics, m_quickInput, m_output, m_progress);
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

	auto status = Dispatch("workbench/statusBar/update", R"({
		"handle":"status:test.sample:4:1","itemId":"test.status","extensionId":"test.sample","generation":4,
		"alignment":"right","priority":20,"text":"$(check) Ready","tooltip":{"markdown":"All good"},
		"command":{"command":"test.run"},"accessibilityInformation":{"label":"Ready"},"visible":true
	})");
	ASSERT_TRUE(status.success) << status.errorMessage;
	const auto statusItems = m_status.Snapshot();
	ASSERT_EQ(1u, statusItems.size());
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

TEST_F(CExtensionWorkbenchDispatcherTest, RoutesDpapiSecretsWithoutLeakingValuesIntoErrors)
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
	ASSERT_TRUE(keys.success) << keys.errorMessage;
	EXPECT_NE(std::string::npos, keys.resultJson.find("token"));
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
		"handle":"output:12","extensionId":"test.sample","generation":12,"name":"Sample"
	})").success);
	ASSERT_TRUE(Dispatch("workbench/output/append", R"({
		"handle":"output:12","extensionId":"test.sample","generation":12,"value":"line one\n"
	})").success);
	ASSERT_TRUE(Dispatch("workbench/output/show", R"({
		"handle":"output:12","extensionId":"test.sample","generation":12
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

TEST_F(CExtensionWorkbenchDispatcherTest, ResolvesQuickPickInputBoxAndTrustThroughNativeHandlers)
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

	m_dispatcher->SetTrustHandler([](std::wstring_view id, std::wstring_view version,
		std::wstring_view name, std::wstring_view path) {
		EXPECT_EQ(L"test.sample", id);
		EXPECT_EQ(L"1.2.3", version);
		EXPECT_EQ(L"Sample", name);
		EXPECT_EQ(L"C:\\extensions\\sample", path);
		return true;
	});
	auto trust = Dispatch("workbench/extensions/ensureTrusted", R"({
		"extensionId":"test.sample","version":"1.2.3","displayName":"Sample",
		"extensionPath":"C:\\extensions\\sample"
	})", EExtensionRpcMessageKind::Request);
	ASSERT_TRUE(trust.success) << trust.errorMessage;
	EXPECT_NE(std::string::npos, trust.resultJson.find("true"));
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

} // namespace
