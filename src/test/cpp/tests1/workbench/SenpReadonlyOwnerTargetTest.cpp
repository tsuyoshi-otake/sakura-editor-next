/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyOwnerTarget.h"

#include <CommCtrl.h>

namespace workbench::editor::tests {
namespace {

class SenpReadonlyOwnerTargetTest : public testing::Test {
protected:
	HRESULT apartment{ E_FAIL };
	EditorCoreService core;
	HWND parent{}, legacy{};
	std::unique_ptr<SenpReadonlyEditorController> controller;

	void SetUp() override
	{
		apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		ASSERT_TRUE(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE);
		INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
		ASSERT_TRUE(::InitCommonControlsEx(&controls));
		const EditorDocumentIdentity identity{ .opaqueId = "owner-target-legacy" };
		ASSERT_EQ(EEditorOperationStatus::Succeeded, core.OpenResolvedInput({
			.operation = { "owner-target.fixture.open" }, .input = { "legacy", identity },
			.resolvedDocument = ResolvedEditorDocument{ identity, 7, true },
		}).status);
		parent = ::CreateWindowExW(0, L"STATIC", L"SENP owner target",
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 80, 80, 560, 380,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		legacy = ::CreateWindowExW(0, L"EDIT", L"legacy", WS_CHILD | WS_TABSTOP | ES_MULTILINE,
			0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, legacy);
		::ShowWindow(parent, SW_SHOWNOACTIVATE);
		controller = std::make_unique<SenpReadonlyEditorController>(core, parent, legacy, legacy, "legacy");
		ASSERT_EQ(SenpSurfaceProjection::Applied, controller->Layout({ 4, 8, 500, 320 }));
	}

	void TearDown() override
	{
		if (controller) { (void)controller->Shutdown(); controller.reset(); }
		if (parent) { ::DestroyWindow(parent); EXPECT_FALSE(::IsWindow(parent)); }
		if (SUCCEEDED(apartment)) ::CoUninitialize();
	}

	static senp::ContributionOwnerIdentity Owner()
	{
		return { L"sample.details", std::wstring(64, L'a'), 3, 4, 5 };
	}
};

TEST_F(SenpReadonlyOwnerTargetTest, PublishesRefreshesAndRevokesNativeDocument)
{
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 920000);
	const senp::effect::OperationContext first{ L"document.1", 3, 4, 5, 1 };
	ASSERT_TRUE(target.BeginDocument(L"issue/296", first));
	ASSERT_TRUE(target.PublishDocument(first, { L"issue/296", L"Issue 296", 1,
		{ senp::effect::MarkdownSection{ L"# First\n\nBody" } } })) << static_cast<int>(target.State());
	ASSERT_EQ(1U, target.DocumentCount());
	const auto input = target.InputId(L"issue/296"); ASSERT_TRUE(input);
	auto* host = target.Host(L"issue/296"); ASSERT_NE(nullptr, host);
	EXPECT_EQ(SenpDocumentHostState::Ready, host->State());
	EXPECT_EQ(*input, *core.Snapshot().group.activeInputId);
	EXPECT_TRUE(controller->IsReadonlyActive());

	const senp::effect::OperationContext second{ L"document.2", 3, 4, 5, 2 };
	ASSERT_TRUE(target.BeginDocument(L"issue/296", second));
	ASSERT_TRUE(target.PublishDocument(second, { L"issue/296", L"Issue 296", 2,
		{ senp::effect::MarkdownSection{ L"# Refreshed" } } }));
	EXPECT_EQ(input, target.InputId(L"issue/296"));
	EXPECT_EQ(host, target.Host(L"issue/296"));

	target.Revoke();
	EXPECT_EQ(0U, target.DocumentCount());
	EXPECT_FALSE(controller->IsReadonlyActive());
	EXPECT_EQ(std::string("legacy"), *core.Snapshot().group.activeInputId);
}

TEST_F(SenpReadonlyOwnerTargetTest, RejectsForeignTerminalAndSurvivesExternalCoreClose)
{
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 930000);
	const senp::effect::OperationContext request{ L"document.1", 3, 4, 5, 1 };
	ASSERT_TRUE(target.BeginDocument(L"issue/296", request));
	auto foreign = request; foreign.accountGeneration = 6;
	EXPECT_FALSE(target.PublishDocument(foreign, { L"issue/296", L"Issue 296", 1, {} }));
	senp::effect::PublishDocument published{ L"issue/296", L"Issue 296", 1,
		{ senp::effect::MetadataSection{ { { L"State", L"Ready" } } } } };
	ASSERT_TRUE(target.PublishDocument(request, std::move(published))) << static_cast<int>(target.State());
	const auto input = target.InputId(L"issue/296"); ASSERT_TRUE(input);
	const auto snapshot = core.Snapshot();
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		core.CloseInput({ { "owner-target.external-close", snapshot.revision }, *input }).status);
	EXPECT_EQ(SenpSurfaceProjection::Applied, controller->Apply());
	EXPECT_EQ(0U, target.DocumentCount());
	EXPECT_EQ(nullptr, target.Host(L"issue/296"));
}

} // namespace
} // namespace workbench::editor::tests
