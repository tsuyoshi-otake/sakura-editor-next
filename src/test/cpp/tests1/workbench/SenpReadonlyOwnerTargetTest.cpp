/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyOwnerTarget.h"

#include <CommCtrl.h>

#include <deque>

namespace workbench::editor::tests {
namespace {

//! Records every seam call and hands back scripted terminals. It never blocks,
//! mirroring the contract the production seam owes the UI thread.
class ScriptedToolReads final : public ISenpOwnerToolReads {
public:
	struct Started {
		senp::ContributionOwnerIdentity owner;
		senp::effect::OperationContext context;
		senp::effect::StartToolRead read;
	};
	std::vector<Started> started;
	std::vector<std::pair<senp::ContributionOwnerIdentity, senp::effect::OperationContext>> cancelled;
	std::vector<senp::ContributionOwnerIdentity> cancelledAll;
	std::deque<senp::effect::ToolCompleted> completions;
	//! When set, Take answers with this terminal forever. It models a seam that
	//! keeps offering a completion the target no longer owns.
	std::optional<senp::effect::ToolCompleted> endless;
	int takeCalls{};
	bool admit{ true };

	[[nodiscard]] bool Start(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context,
		const senp::effect::StartToolRead& read) noexcept override
	{
		started.push_back({ owner, context, read });
		return admit;
	}
	[[nodiscard]] std::optional<senp::effect::ToolCompleted> Take(
		const senp::ContributionOwnerIdentity&) noexcept override
	{
		++takeCalls;
		if (endless) return endless;
		if (completions.empty()) return {};
		auto value = completions.front();
		completions.pop_front();
		return value;
	}
	void Cancel(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context) noexcept override
	{
		cancelled.emplace_back(owner, context);
	}
	void CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept override
	{
		cancelledAll.push_back(owner);
	}
	//! An owner target never asks for the account fence, so these record the
	//! calls in order to assert that it does not.
	[[nodiscard]] SenpToolAccount Account() const noexcept override
	{
		++accountCalls;
		return account;
	}
	void RefreshAccount() noexcept override { ++accountRefreshes; }
	//! Recorded for the same reason: the workspace is the window's business and
	//! an owner target must never declare one.
	void DeclareWorkspace(std::uint64_t, std::uint64_t, std::vector<std::wstring>) noexcept override
	{
		++workspaceDeclarations;
	}

	SenpToolAccount account;
	mutable int accountCalls{};
	int accountRefreshes{};
	int workspaceDeclarations{};
};

senp::effect::StartToolRead Read(std::wstring readId, std::wstring operation)
{
	return { std::move(readId), L"github", std::move(operation),
		{ senp::effect::Field{ L"repository", L"owner/project" } } };
}

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

TEST_F(SenpReadonlyOwnerTargetTest, StyleObserverExpiresOnRevokeAndDestruction)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Light);
	LOGFONT font{};
	(void)::GetObjectW(::GetStockObject(DEFAULT_GUI_FONT), sizeof(font), &font);
	SenpReadonlyOwnerStyleSink revoked, destroyed;
	{
		CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 940000);
		revoked = target.StyleSink();
		ASSERT_TRUE(revoked(palette, font, 144));
		const senp::effect::OperationContext request{ L"styled.document", 3, 4, 5, 1 };
		ASSERT_TRUE(target.BeginDocument(L"issue/296", request));
		ASSERT_TRUE(target.PublishDocument(request, { L"issue/296", L"Styled document", 1,
			{ senp::effect::MetadataSection{ { { L"State", L"Ready" } } } } }));
		ASSERT_TRUE(revoked(palette, font, 192));
		EXPECT_EQ(1U, target.DocumentCount());
		target.Revoke();
		EXPECT_FALSE(revoked(palette, font, 96));
		EXPECT_EQ(0U, target.DocumentCount());
	}
	EXPECT_FALSE(revoked(palette, font, 96));
	{
		CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 950000);
		destroyed = target.StyleSink();
		ASSERT_TRUE(destroyed(palette, font, 96));
	}
	EXPECT_FALSE(destroyed(palette, font, 96));
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

TEST_F(SenpReadonlyOwnerTargetTest, RoutesAdmittedToolReadsToTheSeamAndReturnsTheirTerminals)
{
	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 960000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext issues{ L"tool.1", 3, 4, 5, 1 };
	const senp::effect::OperationContext comments{ L"tool.2", 3, 4, 5, 2 };
	ASSERT_TRUE(target.StartToolRead(issues, Read(L"issues:open:1", L"repositoryRead")));
	ASSERT_TRUE(target.StartToolRead(comments, Read(L"comments:1", L"repositoryRead")));
	ASSERT_EQ(2U, target.ToolReadCount());
	ASSERT_EQ(2U, reads.started.size());
	// The seam is told which owner scope the read belongs to; the target never
	// lets the extension name it.
	EXPECT_EQ(Owner(), reads.started[0].owner);
	EXPECT_EQ(issues, reads.started[0].context);
	EXPECT_EQ(L"issues:open:1", reads.started[0].read.readId);
	EXPECT_EQ(L"github", reads.started[0].read.toolId);
	EXPECT_EQ(L"repositoryRead", reads.started[0].read.operation);
	ASSERT_EQ(1U, reads.started[0].read.arguments.size());
	EXPECT_EQ(L"owner/project", reads.started[0].read.arguments[0].value);

	// Terminals arrive out of order; each one is paired with the context that
	// started that readId, not with the order it finished in.
	reads.completions.push_back({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]" });
	reads.completions.push_back({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]" });
	const auto firstTerminal = target.TakeToolRead();
	ASSERT_TRUE(firstTerminal);
	EXPECT_EQ(comments, firstTerminal->Context());
	EXPECT_EQ(L"comments:1", firstTerminal->Completion().readId);
	EXPECT_EQ(L"[]", firstTerminal->Completion().data);
	const auto secondTerminal = target.TakeToolRead();
	ASSERT_TRUE(secondTerminal);
	EXPECT_EQ(issues, secondTerminal->Context());
	EXPECT_EQ(L"[1]", secondTerminal->Completion().data);
	EXPECT_EQ(0U, target.ToolReadCount());
	EXPECT_FALSE(target.TakeToolRead());

	// The account fence and the workspace are the window's concern. A target that
	// asked for either would be reading authority it is already scoped by, or
	// answering for a workspace it does not own, so it never does.
	EXPECT_EQ(0, reads.accountCalls);
	EXPECT_EQ(0, reads.accountRefreshes);
	EXPECT_EQ(0, reads.workspaceDeclarations);
}

TEST_F(SenpReadonlyOwnerTargetTest, RefusesEveryToolReadItCannotAccountFor)
{
	CSenpReadonlyOwnerTarget without(Owner(), *controller, parent, 961000);
	const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
	// No seam at all: the boundary fails explicitly instead of pretending.
	EXPECT_FALSE(without.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	EXPECT_FALSE(without.TakeToolRead());

	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 962000, nullptr, {}, {}, {}, &reads);
	const std::vector<senp::effect::OperationContext> foreign{
		{ L"tool.1", 9, 4, 5, 1 },   // another owner generation
		{ L"tool.1", 3, 9, 5, 1 },   // another workspace revision
		{ L"tool.1", 3, 4, 9, 1 },   // another account generation
		{ L"tool.1", 3, 4, 5, 0 },   // no request lineage
		{ L"", 3, 4, 5, 1 },         // no operation
	};
	for (const auto& rejected : foreign)
		EXPECT_FALSE(target.StartToolRead(rejected, Read(L"issues:open:1", L"repositoryRead")));
	EXPECT_FALSE(target.StartToolRead(context, Read(L"", L"repositoryRead")));
	EXPECT_FALSE(target.StartToolRead(context, Read(std::wstring(513, L'r'), L"repositoryRead")));
	EXPECT_TRUE(reads.started.empty());

	ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	// The same readId twice would make one terminal unroutable.
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	EXPECT_EQ(1U, reads.started.size());

	// A refused dispatch leaves nothing behind that could never be drained.
	reads.admit = false;
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:2", L"repositoryRead")));
	EXPECT_EQ(1U, target.ToolReadCount());
	reads.admit = true;
	for (std::size_t index = 1; index < senp::CSenpRuntimeSession::kMaximumPending; ++index)
		ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:bulk:" + std::to_wstring(index),
			L"repositoryRead")));
	EXPECT_EQ(senp::CSenpRuntimeSession::kMaximumPending, target.ToolReadCount());
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:overflow", L"repositoryRead")));
}

TEST_F(SenpReadonlyOwnerTargetTest, DiscardsATerminalForAReadItNoLongerOwns)
{
	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 963000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
	ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	target.CancelToolReads(context);
	EXPECT_EQ(0U, target.ToolReadCount());
	ASSERT_EQ(1U, reads.cancelled.size());
	EXPECT_EQ(Owner(), reads.cancelled[0].first);
	EXPECT_EQ(context, reads.cancelled[0].second);

	// The read was already in flight, so its terminal still surfaces. Handing it
	// to the projection would be a protocol violation and would close the owner.
	reads.endless = senp::effect::ToolCompleted{ L"issues:open:1",
		senp::effect::CompletionStatus::Succeeded, L"[1]" };
	EXPECT_FALSE(target.TakeToolRead());
	// Bounded drain: a seam that keeps offering an unknown terminal cannot spin
	// the UI thread.
	EXPECT_LE(reads.takeCalls, static_cast<int>(senp::CSenpRuntimeSession::kMaximumPending) + 1);
}

TEST_F(SenpReadonlyOwnerTargetTest, CancelsOnlyTheRequestLineageItWasGiven)
{
	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 964000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext first{ L"tool.1", 3, 4, 5, 1 };
	const senp::effect::OperationContext second{ L"tool.2", 3, 4, 5, 2 };
	ASSERT_TRUE(target.StartToolRead(first, Read(L"issues:open:1", L"repositoryRead")));
	ASSERT_TRUE(target.StartToolRead(second, Read(L"comments:1", L"repositoryRead")));
	target.CancelToolReads(first);
	EXPECT_EQ(1U, target.ToolReadCount());

	reads.completions.push_back({ L"issues:open:1", senp::effect::CompletionStatus::Cancelled, L"" });
	reads.completions.push_back({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]" });
	const auto terminal = target.TakeToolRead();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(second, terminal->Context());
	EXPECT_EQ(L"comments:1", terminal->Completion().readId);
	EXPECT_EQ(0U, target.ToolReadCount());
}

TEST_F(SenpReadonlyOwnerTargetTest, RevocationCancelsEveryReadBeforeAnotherCanBeRouted)
{
	ScriptedToolReads reads;
	{
		CSenpReadonlyOwnerTarget target(Owner(), *controller, parent, 965000, nullptr, {}, {}, {}, &reads);
		const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
		ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
		reads.completions.push_back({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]" });
		target.Revoke();
		ASSERT_EQ(1U, reads.cancelledAll.size());
		EXPECT_EQ(Owner(), reads.cancelledAll[0]);
		EXPECT_EQ(0U, target.ToolReadCount());
		EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:2", L"repositoryRead")));
		// The queued terminal is never taken: after CancelAll the seam is released.
		EXPECT_FALSE(target.TakeToolRead());
		EXPECT_EQ(0, reads.takeCalls);
		EXPECT_EQ(1U, reads.started.size());
	}
	// Destruction re-enters Revoke; the seam must not be told twice.
	EXPECT_EQ(1U, reads.cancelledAll.size());
}

} // namespace workbench::editor::tests
