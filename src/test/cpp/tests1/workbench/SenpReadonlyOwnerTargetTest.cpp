/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyOwnerTarget.h"
#include "workbench/editor/SenpOwnerTextResources.h"

#include <CommCtrl.h>

#include <deque>
#include <functional>
#include <string>

namespace workbench::editor::tests {
namespace {

//! Records every seam call and hands back scripted terminals. It never blocks,
//! mirroring the contract the production seam owes the UI thread.
class ScriptedToolReads final : public ISenpOwnerToolReads {
public:
	//! One recorded Start() call. A private, constructor-built record rather
	//! than an aggregate, so the fixture's recordings stay query-only outside
	//! this file, matching the "fake-recording fields become private members
	//! with query methods" fix for wire DTOs and test fakes.
	class Started {
	public:
		Started(senp::ContributionOwnerIdentity owner, senp::effect::OperationContext context,
			senp::effect::StartToolRead read)
			: m_owner(std::move(owner)), m_context(std::move(context)), m_read(std::move(read)) {
		}

		[[nodiscard]] const senp::ContributionOwnerIdentity& Owner() const noexcept { return m_owner; }
		[[nodiscard]] const senp::effect::OperationContext& Context() const noexcept { return m_context; }
		[[nodiscard]] const senp::effect::StartToolRead& Read() const noexcept { return m_read; }

	private:
		senp::ContributionOwnerIdentity m_owner;
		senp::effect::OperationContext m_context;
		senp::effect::StartToolRead m_read;
	};

	//! Only the text pump reaches these, so recording every call is what lets a
	//! test assert that publishing, completing and revoking make none of them.
	class Requested {
	public:
		Requested(std::wstring handle, std::uint64_t offset, std::uint32_t length)
			: m_handle(std::move(handle)), m_offset(offset), m_length(length) {
		}

		[[nodiscard]] const std::wstring& Handle() const noexcept { return m_handle; }
		[[nodiscard]] std::uint64_t Offset() const noexcept { return m_offset; }
		[[nodiscard]] std::uint32_t Length() const noexcept { return m_length; }

	private:
		std::wstring m_handle;
		std::uint64_t m_offset{};
		std::uint32_t m_length{};
	};

	[[nodiscard]] bool Start(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context,
		const senp::effect::StartToolRead& read) noexcept override
	{
		m_started.emplace_back(owner, context, read);
		return m_admit;
	}
	[[nodiscard]] std::optional<senp::effect::ToolCompleted> Take(
		const senp::ContributionOwnerIdentity&) noexcept override
	{
		++m_takeCalls;
		if (m_endless) return m_endless;
		if (m_completions.empty()) return {};
		auto value = m_completions.front();
		m_completions.pop_front();
		return value;
	}
	void Cancel(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context) noexcept override
	{
		m_cancelled.emplace_back(owner, context);
	}
	void CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept override
	{
		m_cancelledAll.push_back(owner);
	}
	//! An owner target never asks for the account fence, so these record the
	//! calls in order to assert that it does not.
	[[nodiscard]] SenpToolAccount Account() const noexcept override
	{
		++*m_accountCalls;
		return m_account;
	}
	void RefreshAccount() noexcept override { ++m_accountRefreshes; }
	//! Recorded for the same reason: the workspace is the window's business and
	//! an owner target must never declare one.
	void DeclareWorkspace(std::uint64_t, std::uint64_t, std::vector<std::wstring>) noexcept override
	{
		++m_workspaceDeclarations;
	}
	[[nodiscard]] bool ReadResource(const senp::ContributionOwnerIdentity&, std::wstring_view handle,
		const std::uint64_t offset, const std::uint32_t length) noexcept override
	{
		m_resourceReads.emplace_back(std::wstring(handle), offset, length);
		return m_admitResource;
	}
	[[nodiscard]] std::optional<SenpToolResourceAnswer> TakeResource(
		const senp::ContributionOwnerIdentity&) noexcept override
	{
		++m_resourceTakes;
		if (m_resourceAnswers.empty()) return {};
		auto value = std::move(m_resourceAnswers.front());
		m_resourceAnswers.pop_front();
		return value;
	}
	void ReleaseResource(const senp::ContributionOwnerIdentity&, std::wstring_view handle) noexcept override
	{
		m_resourceReleases.emplace_back(handle);
	}

	//! Query methods for the tests in this file. This fixture's recordings are
	//! read-only from outside; only the scripting mutators below let a test
	//! shape the next scripted answer.
	[[nodiscard]] const std::vector<Started>& StartedCalls() const noexcept { return m_started; }
	[[nodiscard]] const std::vector<std::pair<senp::ContributionOwnerIdentity, senp::effect::OperationContext>>&
	CancelledCalls() const noexcept { return m_cancelled; }
	[[nodiscard]] const std::vector<senp::ContributionOwnerIdentity>& CancelledAllCalls() const noexcept
	{
		return m_cancelledAll;
	}
	[[nodiscard]] int TakeCalls() const noexcept { return m_takeCalls; }
	[[nodiscard]] const std::vector<Requested>& ResourceReads() const noexcept { return m_resourceReads; }
	[[nodiscard]] const std::vector<std::wstring>& ResourceReleases() const noexcept { return m_resourceReleases; }
	[[nodiscard]] int ResourceTakes() const noexcept { return m_resourceTakes; }
	[[nodiscard]] int AccountCalls() const noexcept { return *m_accountCalls; }
	[[nodiscard]] int AccountRefreshes() const noexcept { return m_accountRefreshes; }
	[[nodiscard]] int WorkspaceDeclarations() const noexcept { return m_workspaceDeclarations; }

	//! Scripting mutators: shape the answers this fixture hands back.
	void PushCompletion(senp::effect::ToolCompleted completion) { m_completions.push_back(std::move(completion)); }
	//! When set, Take answers with this terminal forever. It models a seam that
	//! keeps offering a completion the target no longer owns.
	void SetEndless(senp::effect::ToolCompleted completion) { m_endless = std::move(completion); }
	void SetAdmit(const bool admit) noexcept { m_admit = admit; }
	void PushResourceAnswer(SenpToolResourceAnswer answer) { m_resourceAnswers.push_back(std::move(answer)); }
	void SetAdmitResource(const bool admit) noexcept { m_admitResource = admit; }

private:
	std::vector<Started> m_started;
	std::vector<std::pair<senp::ContributionOwnerIdentity, senp::effect::OperationContext>> m_cancelled;
	std::vector<senp::ContributionOwnerIdentity> m_cancelledAll;
	std::deque<senp::effect::ToolCompleted> m_completions;
	std::optional<senp::effect::ToolCompleted> m_endless;
	int m_takeCalls{};
	bool m_admit{ true };

	std::vector<Requested> m_resourceReads;
	std::deque<SenpToolResourceAnswer> m_resourceAnswers;
	std::vector<std::wstring> m_resourceReleases;
	int m_resourceTakes{};
	bool m_admitResource{ true };

	SenpToolAccount m_account;
	//! Owned indirectly so Account() can stay const per the interface without
	//! declaring the counter itself mutable: only the pointee changes.
	std::unique_ptr<int> m_accountCalls = std::make_unique<int>(0);
	int m_accountRefreshes{};
	int m_workspaceDeclarations{};
};

//! A document whose only section is a text resource, so its single page is the
//! text page and no selection is needed to reach it.
senp::effect::PublishDocument TextDocument()
{
	return { L"run/42", L"Workflow run", 1,
		{ senp::effect::TextResourceSection{ L"log-1", 0, senp::effect::TextStatus::Loading } } };
}

//! One chunk shaped exactly as the control-side store answers it. The revision
//! is the owner generation, which is what resources are created under.
senp::TextResourceChunk Chunk(std::string bytes, std::size_t offset, std::size_t length,
	senp::TextResourceState state, senp::TextResourceEnd end)
{
	senp::TextResourceChunk chunk;
	chunk.result = senp::TextResourceResult::Accepted;
	chunk.state = state;
	chunk.end = end;
	chunk.handle = L"log-1";
	chunk.revision = 3;
	chunk.offset = offset;
	chunk.length = length;
	chunk.bytes = std::move(bytes);
	return chunk;
}

SenpToolResourceAnswer Answer(std::optional<senp::TextResourceChunk> chunk, std::uint64_t offset = 0)
{
	return SenpToolResourceAnswer(L"log-1", offset, std::move(chunk));
}

//! The store refusing a read, which is an answer and not a lost connection.
SenpToolResourceAnswer Refused(senp::TextResourceResult result, std::uint64_t offset)
{
	senp::TextResourceChunk chunk;
	chunk.result = result;
	chunk.handle = L"log-1";
	return Answer(std::move(chunk), offset);
}

senp::effect::StartToolRead Read(std::wstring readId, std::wstring operation)
{
	return { std::move(readId), L"github", std::move(operation),
		{ senp::effect::Field{ L"repository", L"owner/project" } } };
}

class SenpReadonlyOwnerTargetTest : public testing::Test {
protected:
	HRESULT apartment{ E_FAIL };
	EditorCoreService core;
	std::unique_ptr<SenpReadonlyEditorController> controller;

	//! The surface every target in these tests presents on. The fixture owns the
	//! shell window behind it, so it is the fixture that says how a document host
	//! reaches it; a test never needs the window itself.
	SenpReadonlyOwnerSurface surface;

	//! Takes down what SetUp put up. The fixture keeps the closing step rather
	//! than the window it closes, so the shell can stay a local of the one
	//! function that creates it.
	std::function<void()> closeShell;

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
		// The shell and its legacy child are this fixture's own scaffolding, so
		// they stay local to the one function that raises them. What the tests
		// need of them is kept instead: the surface a document is presented on,
		// and the step that takes the whole thing down again.
		const auto shell = ::CreateWindowExW(0, L"STATIC", L"SENP owner target",
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 80, 80, 560, 380,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, shell);
		closeShell = [shell] { ::DestroyWindow(shell); EXPECT_FALSE(::IsWindow(shell)); };
		const auto legacyEditor = ::CreateWindowExW(0, L"EDIT", L"legacy",
			WS_CHILD | WS_TABSTOP | ES_MULTILINE, 0, 0, 0, 0,
			shell, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, legacyEditor);
		::ShowWindow(shell, SW_SHOWNOACTIVATE);
		controller = std::make_unique<SenpReadonlyEditorController>(
			core, shell, legacyEditor, legacyEditor, "legacy");
		ASSERT_EQ(SenpSurfaceProjection::Applied, controller->Layout({ 4, 8, 500, 320 }));
		surface = { [shell] { return ::IsWindow(shell) != FALSE; },
			[shell](SenpReadonlyDocumentHost& host) { return host.Create(shell); } };
	}

	void TearDown() override
	{
		if (controller) { (void)controller->Shutdown(); controller.reset(); }
		if (closeShell) closeShell();
		if (SUCCEEDED(apartment)) ::CoUninitialize();
	}

	static senp::ContributionOwnerIdentity Owner()
	{
		return { L"sample.details", std::wstring(64, L'a'), 3, 4, 5 };
	}

	//! The production authority, so a page is built on the scope the window
	//! would really project rather than on a shape invented for the test.
	CSenpOwnerTextResources resources{ L"0123456789abcdef0123456789abcdef" };

	//! Publishes the one-page text document and leaves it on screen, which is
	//! what makes the host willing to hand out a read.
	void PublishText(CSenpReadonlyOwnerTarget& target)
	{
		const senp::effect::OperationContext request{ L"text.document", 3, 4, 5, 1 };
		ASSERT_TRUE(target.BeginDocument(L"run/42", request));
		ASSERT_TRUE(target.PublishDocument(request, TextDocument())) << static_cast<int>(target.State());
		auto* const host = target.Host(L"run/42");
		ASSERT_NE(nullptr, host);
		ASSERT_EQ(SenpDocumentHostState::Ready, host->State()) << static_cast<int>(host->State());
	}

	//! Leaves the surface holding "partial" with more still expected, which is
	//! the only state in which losing the rest of a resource is observable.
	void PumpPartialText(CSenpReadonlyOwnerTarget& target, ScriptedToolReads& reads)
	{
		target.PumpText();
		ASSERT_EQ(1U, reads.ResourceReads().size());
		reads.PushResourceAnswer(Answer(Chunk("partial", 0, 15,
			senp::TextResourceState::Loading, senp::TextResourceEnd::None)));
		target.PumpText();
		ASSERT_EQ(2U, reads.ResourceReads().size());
		EXPECT_EQ(7U, reads.ResourceReads()[1].Offset());
		auto* const host = target.Host(L"run/42");
		ASSERT_NE(nullptr, host);
		host->SelectAll();
		ASSERT_EQ(L"partial", host->SelectedText());
	}
};

TEST_F(SenpReadonlyOwnerTargetTest, CarriesOneTextChunkPerTurnFromTheSeamToTheSurface)
{
	ScriptedToolReads reads;
	ASSERT_TRUE(resources.Admit(Owner()));
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 960000, &resources, {}, {}, {}, &reads);
	ASSERT_NO_FATAL_FAILURE(PublishText(target));

	// The first turn admits a read and settles nothing: no answer exists yet.
	target.PumpText();
	ASSERT_EQ(1U, reads.ResourceReads().size());
	EXPECT_EQ(L"log-1", reads.ResourceReads()[0].Handle());
	EXPECT_EQ(0U, reads.ResourceReads()[0].Offset());
	EXPECT_EQ(senp::SenpTextResourceStore::kChunkBytes, reads.ResourceReads()[0].Length());
	EXPECT_EQ(L"run/42", target.OutstandingTextResource());

	// A turn with no answer waiting leaves the read where it is rather than
	// asking a second time for a range already in flight.
	target.PumpText();
	EXPECT_EQ(1U, reads.ResourceReads().size());
	EXPECT_EQ(L"run/42", target.OutstandingTextResource());

	reads.PushResourceAnswer(Answer(Chunk("run step output", 0, 15,
		senp::TextResourceState::Complete, senp::TextResourceEnd::Complete)));
	target.PumpText();
	EXPECT_TRUE(target.OutstandingTextResource().empty());
	auto* const host = target.Host(L"run/42");
	ASSERT_NE(nullptr, host);
	host->SelectAll();
	EXPECT_EQ(L"run step output", host->SelectedText());

	// A resource that has arrived whole asks for nothing further.
	target.PumpText();
	EXPECT_EQ(1U, reads.ResourceReads().size());
}

TEST_F(SenpReadonlyOwnerTargetTest, AnswersAReadTheSeamWouldNotCarryInsteadOfLeavingItOutstanding)
{
	ScriptedToolReads reads;
	reads.SetAdmitResource(false);
	ASSERT_TRUE(resources.Admit(Owner()));
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 961000, &resources, {}, {}, {}, &reads);
	ASSERT_NO_FATAL_FAILURE(PublishText(target));

	// The host committed to the read the moment it handed it over, so a refused
	// admission has to end it here rather than leave the page waiting.
	target.PumpText();
	ASSERT_EQ(1U, reads.ResourceReads().size());
	EXPECT_TRUE(target.OutstandingTextResource().empty());

	// A failed page wants nothing further, and nothing is owed to this owner.
	target.PumpText();
	EXPECT_EQ(1U, reads.ResourceReads().size());
	EXPECT_EQ(0, reads.ResourceTakes());
}

TEST_F(SenpReadonlyOwnerTargetTest, KeepsWhatArrivedWhenNoAnswerAboutTheResourceCameBack)
{
	ScriptedToolReads reads;
	ASSERT_TRUE(resources.Admit(Owner()));
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 962000, &resources, {}, {}, {}, &reads);
	ASSERT_NO_FATAL_FAILURE(PublishText(target));
	ASSERT_NO_FATAL_FAILURE(PumpPartialText(target, reads));

	// A connection that went away says nothing about the resource, so the bytes
	// that did arrive stay on screen under a failed status.
	reads.PushResourceAnswer(Answer(std::nullopt, 7));
	target.PumpText();
	EXPECT_TRUE(target.OutstandingTextResource().empty());
	auto* const host = target.Host(L"run/42");
	ASSERT_NE(nullptr, host);
	host->SelectAll();
	EXPECT_EQ(L"partial", host->SelectedText());
}

TEST_F(SenpReadonlyOwnerTargetTest, ErasesTheBodyOfAResourceTheStoreHasLetGo)
{
	ScriptedToolReads reads;
	ASSERT_TRUE(resources.Admit(Owner()));
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 963000, &resources, {}, {}, {}, &reads);
	ASSERT_NO_FATAL_FAILURE(PublishText(target));
	ASSERT_NO_FATAL_FAILURE(PumpPartialText(target, reads));

	// Expired is the store saying the resource is gone, not that this read
	// failed. What arrived is no longer part of anything completable.
	reads.PushResourceAnswer(Refused(senp::TextResourceResult::Expired, 7));
	target.PumpText();
	EXPECT_TRUE(target.OutstandingTextResource().empty());
	auto* const host = target.Host(L"run/42");
	ASSERT_NE(nullptr, host);
	host->SelectAll();
	EXPECT_TRUE(host->SelectedText().empty());
}

TEST_F(SenpReadonlyOwnerTargetTest, IgnoresAnAnswerThatSettlesSomeOtherRead)
{
	ScriptedToolReads reads;
	ASSERT_TRUE(resources.Admit(Owner()));
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 966000, &resources, {}, {}, {}, &reads);
	ASSERT_NO_FATAL_FAILURE(PublishText(target));
	ASSERT_NO_FATAL_FAILURE(PumpPartialText(target, reads));

	// The offset names a range this owner never asked for. It cannot settle the
	// outstanding read, and no second answer is coming, so the read ends failed
	// and the bytes already shown are left alone.
	reads.PushResourceAnswer(Answer(Chunk("more", 0, 15,
		senp::TextResourceState::Loading, senp::TextResourceEnd::None), 0));
	target.PumpText();
	EXPECT_TRUE(target.OutstandingTextResource().empty());
	auto* const host = target.Host(L"run/42");
	ASSERT_NE(nullptr, host);
	host->SelectAll();
	EXPECT_EQ(L"partial", host->SelectedText());
}

TEST_F(SenpReadonlyOwnerTargetTest, MakesNoResourceCallWithoutAPumpTurnAndNoneAfterRevoke)
{
	ScriptedToolReads reads;
	ASSERT_TRUE(resources.Admit(Owner()));
	SenpReadonlyOwnerTextPump pump;
	{
		CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 964000, &resources, {}, {}, {}, &reads);
		ASSERT_NO_FATAL_FAILURE(PublishText(target));
		pump = target.TextPump();

		// Publishing a document and starting a tool read are the target's own
		// work; neither touches a resource.
		const senp::effect::OperationContext request{ L"tool.read", 3, 4, 5, 2 };
		ASSERT_TRUE(target.StartToolRead(request, Read(L"read-1", L"repositoryRead")));
		EXPECT_FALSE(target.TakeToolRead());
		EXPECT_TRUE(reads.ResourceReads().empty());
		EXPECT_EQ(0, reads.ResourceTakes());

		EXPECT_TRUE(pump());
		EXPECT_EQ(1U, reads.ResourceReads().size());
		EXPECT_EQ(L"run/42", target.OutstandingTextResource());

		target.Revoke();
		EXPECT_FALSE(pump());
		EXPECT_TRUE(target.OutstandingTextResource().empty());
		EXPECT_EQ(1U, reads.ResourceReads().size());
	}
	// The handle outlives the object it was taken from and says so plainly.
	EXPECT_FALSE(pump());
	EXPECT_EQ(1U, reads.ResourceReads().size());
}

TEST_F(SenpReadonlyOwnerTargetTest, ShowsNoTextPageForAnOwnerTheAuthorityDoesNotHold)
{
	ScriptedToolReads reads;
	// The authority was never told about this owner, so the cohort the document
	// names cannot be attributed and no surface is created to wait on bytes.
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 965000, &resources, {}, {}, {}, &reads);
	const senp::effect::OperationContext request{ L"text.document", 3, 4, 5, 1 };
	ASSERT_TRUE(target.BeginDocument(L"run/42", request));
	EXPECT_FALSE(target.PublishDocument(request, TextDocument()));
	EXPECT_EQ(SenpReadonlyOwnerTargetState::HostFailed, target.State());
	EXPECT_EQ(0U, target.DocumentCount());

	target.PumpText();
	EXPECT_TRUE(reads.ResourceReads().empty());
	EXPECT_TRUE(target.OutstandingTextResource().empty());
}

TEST_F(SenpReadonlyOwnerTargetTest, PublishesRefreshesAndRevokesNativeDocument)
{
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 920000);
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
		CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 940000);
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
		CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 950000);
		destroyed = target.StyleSink();
		ASSERT_TRUE(destroyed(palette, font, 96));
	}
	EXPECT_FALSE(destroyed(palette, font, 96));
}

TEST_F(SenpReadonlyOwnerTargetTest, RejectsForeignTerminalAndSurvivesExternalCoreClose)
{
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 930000);
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
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 960000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext issues{ L"tool.1", 3, 4, 5, 1 };
	const senp::effect::OperationContext comments{ L"tool.2", 3, 4, 5, 2 };
	ASSERT_TRUE(target.StartToolRead(issues, Read(L"issues:open:1", L"repositoryRead")));
	ASSERT_TRUE(target.StartToolRead(comments, Read(L"comments:1", L"repositoryRead")));
	ASSERT_EQ(2U, target.ToolReadCount());
	ASSERT_EQ(2U, reads.StartedCalls().size());
	// The seam is told which owner scope the read belongs to; the target never
	// lets the extension name it.
	EXPECT_EQ(Owner(), reads.StartedCalls()[0].Owner());
	EXPECT_EQ(issues, reads.StartedCalls()[0].Context());
	EXPECT_EQ(L"issues:open:1", reads.StartedCalls()[0].Read().readId);
	EXPECT_EQ(L"github", reads.StartedCalls()[0].Read().toolId);
	EXPECT_EQ(L"repositoryRead", reads.StartedCalls()[0].Read().operation);
	ASSERT_EQ(1U, reads.StartedCalls()[0].Read().arguments.size());
	EXPECT_EQ(L"owner/project", reads.StartedCalls()[0].Read().arguments[0].value);

	// Terminals arrive out of order; each one is paired with the context that
	// started that readId, not with the order it finished in.
	reads.PushCompletion({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]" });
	reads.PushCompletion({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]" });
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
	EXPECT_EQ(0, reads.AccountCalls());
	EXPECT_EQ(0, reads.AccountRefreshes());
	EXPECT_EQ(0, reads.WorkspaceDeclarations());
}

TEST_F(SenpReadonlyOwnerTargetTest, RefusesEveryToolReadItCannotAccountFor)
{
	CSenpReadonlyOwnerTarget without(Owner(), *controller, surface, 961000);
	const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
	// No seam at all: the boundary fails explicitly instead of pretending.
	EXPECT_FALSE(without.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	EXPECT_FALSE(without.TakeToolRead());

	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 962000, nullptr, {}, {}, {}, &reads);
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
	EXPECT_TRUE(reads.StartedCalls().empty());

	ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	// The same readId twice would make one terminal unroutable.
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	EXPECT_EQ(1U, reads.StartedCalls().size());

	// A refused dispatch leaves nothing behind that could never be drained.
	reads.SetAdmit(false);
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:2", L"repositoryRead")));
	EXPECT_EQ(1U, target.ToolReadCount());
	reads.SetAdmit(true);
	for (std::size_t index = 1; index < senp::CSenpRuntimeSession::kMaximumPending; ++index)
		ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:bulk:" + std::to_wstring(index),
			L"repositoryRead")));
	EXPECT_EQ(senp::CSenpRuntimeSession::kMaximumPending, target.ToolReadCount());
	EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:overflow", L"repositoryRead")));
}

TEST_F(SenpReadonlyOwnerTargetTest, DiscardsATerminalForAReadItNoLongerOwns)
{
	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 963000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
	ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
	target.CancelToolReads(context);
	EXPECT_EQ(0U, target.ToolReadCount());
	ASSERT_EQ(1U, reads.CancelledCalls().size());
	EXPECT_EQ(Owner(), reads.CancelledCalls()[0].first);
	EXPECT_EQ(context, reads.CancelledCalls()[0].second);

	// The read was already in flight, so its terminal still surfaces. Handing it
	// to the projection would be a protocol violation and would close the owner.
	reads.SetEndless(senp::effect::ToolCompleted{ L"issues:open:1",
		senp::effect::CompletionStatus::Succeeded, L"[1]" });
	EXPECT_FALSE(target.TakeToolRead());
	// Bounded drain: a seam that keeps offering an unknown terminal cannot spin
	// the UI thread.
	EXPECT_LE(reads.TakeCalls(), static_cast<int>(senp::CSenpRuntimeSession::kMaximumPending) + 1);
}

TEST_F(SenpReadonlyOwnerTargetTest, CancelsOnlyTheRequestLineageItWasGiven)
{
	ScriptedToolReads reads;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 964000, nullptr, {}, {}, {}, &reads);
	const senp::effect::OperationContext first{ L"tool.1", 3, 4, 5, 1 };
	const senp::effect::OperationContext second{ L"tool.2", 3, 4, 5, 2 };
	ASSERT_TRUE(target.StartToolRead(first, Read(L"issues:open:1", L"repositoryRead")));
	ASSERT_TRUE(target.StartToolRead(second, Read(L"comments:1", L"repositoryRead")));
	target.CancelToolReads(first);
	EXPECT_EQ(1U, target.ToolReadCount());

	reads.PushCompletion({ L"issues:open:1", senp::effect::CompletionStatus::Cancelled, L"" });
	reads.PushCompletion({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]" });
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
		CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 965000, nullptr, {}, {}, {}, &reads);
		const senp::effect::OperationContext context{ L"tool.1", 3, 4, 5, 1 };
		ASSERT_TRUE(target.StartToolRead(context, Read(L"issues:open:1", L"repositoryRead")));
		reads.PushCompletion({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]" });
		target.Revoke();
		ASSERT_EQ(1U, reads.CancelledAllCalls().size());
		EXPECT_EQ(Owner(), reads.CancelledAllCalls()[0]);
		EXPECT_EQ(0U, target.ToolReadCount());
		EXPECT_FALSE(target.StartToolRead(context, Read(L"issues:open:2", L"repositoryRead")));
		// The queued terminal is never taken: after CancelAll the seam is released.
		EXPECT_FALSE(target.TakeToolRead());
		EXPECT_EQ(0, reads.TakeCalls());
		EXPECT_EQ(1U, reads.StartedCalls().size());
	}
	// Destruction re-enters Revoke; the seam must not be told twice.
	EXPECT_EQ(1U, reads.CancelledAllCalls().size());
}

namespace {
senp::effect::CompleteCommand Completion(senp::effect::CompletionStatus status, std::wstring message = {})
{
	return { status, std::move(message) };
}
constexpr wchar_t kShown = 0xfffd;
constexpr wchar_t kMore = 0x2026;
}

TEST(SenpCommandCompletionStatusTest, SaysNothingForASilentSuccessAndNamesTheExtensionOtherwise)
{
	using Status = senp::effect::CompletionStatus;
	// A command that succeeded and said nothing has already shown its result in
	// what it changed. Saying so again would make every tree click write a line.
	EXPECT_TRUE(SenpCommandCompletionStatus(L"sample.details", Completion(Status::Succeeded)).empty());
	// A success carrying a message is the extension speaking, so the line is its
	// message under its own name, with no phrase invented around it.
	EXPECT_EQ(L"sample.details: 3 runs refreshed",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::Succeeded, L"3 runs refreshed")));
	// Every other outcome is always told: one that showed nothing would be
	// indistinguishable from the click having done nothing at all.
	EXPECT_EQ(L"sample.details: the command failed.",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::Failed)));
	EXPECT_EQ(L"sample.details: the command failed - rate limit reached",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::Failed, L"rate limit reached")));
	EXPECT_EQ(L"sample.details: the command was cancelled.",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::Cancelled)));
	EXPECT_EQ(L"sample.details: the command timed out.",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::TimedOut)));
	EXPECT_EQ(L"sample.details: the command could not reach its host.",
		SenpCommandCompletionStatus(L"sample.details", Completion(Status::HostUnavailable)));
}

TEST(SenpCommandCompletionStatusTest, FlattensAnUntrustedMessageIntoOneBoundedLine)
{
	using Status = senp::effect::CompletionStatus;
	const auto Line = [](std::wstring message) {
		return SenpCommandCompletionStatus(L"ext", Completion(Status::Succeeded, std::move(message)));
	};
	// The wire checks the message only for well-formed UTF-16 and a byte budget,
	// so newlines and tabs reach here. One status line cannot hold them: runs of
	// whitespace collapse to one space and both ends are trimmed.
	EXPECT_EQ(L"ext: line one line two", Line(L"  line one\r\n\tline two  "));
	// A control unit that is not whitespace is shown, not deleted: text that
	// carried one must never be presented as if it had not.
	EXPECT_EQ(std::wstring(L"ext: a") + kShown + L"b", Line(std::wstring(L"a\0b", 3)));
	EXPECT_EQ(std::wstring(L"ext: ") + kShown + kShown, Line(L"\x01\x7f"));
	// 200 units survive and an ellipsis states that more was said.
	EXPECT_EQ(L"ext: " + std::wstring(200, L'x') + kMore, Line(std::wstring(250, L'x')));
	EXPECT_EQ(L"ext: " + std::wstring(200, L'x'), Line(std::wstring(200, L'x')));
	// Truncation lands between the halves of one character here. A high surrogate
	// left alone is not a character, so it goes with the rest of the message.
	std::wstring split(199, L'x');
	split.push_back(static_cast<wchar_t>(0xd83d));
	split.push_back(static_cast<wchar_t>(0xde00));
	EXPECT_EQ(L"ext: " + std::wstring(199, L'x') + kMore, Line(split));
}

TEST(SenpCommandCompletionStatusTest, NamesAnExtensionItCannotIdentifyRatherThanSpeakingUnattributed)
{
	using Status = senp::effect::CompletionStatus;
	// The name is what stops extension text from being read as the editor's own,
	// so an absent one is stated. A line with no speaker is the one thing this
	// must never produce.
	EXPECT_EQ(L"unknown extension: the command failed.",
		SenpCommandCompletionStatus(L"", Completion(Status::Failed)));
	EXPECT_EQ(L"unknown extension: the command failed.",
		SenpCommandCompletionStatus(L" \t\n", Completion(Status::Failed)));
	// Bounded well below the message, so a long identity cannot crowd out what
	// the extension actually said.
	EXPECT_EQ(std::wstring(64, L'e') + kMore + L": the command failed - why",
		SenpCommandCompletionStatus(std::wstring(80, L'e'), Completion(Status::Failed, L"why")));
}

TEST_F(SenpReadonlyOwnerTargetTest, TellsTheWindowOfACommandItOwnsAndRefusesOneItDoesNot)
{
	std::vector<std::pair<senp::effect::OperationContext, senp::effect::CompleteCommand>> told;
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 966000, nullptr, {},
		[&told](const senp::effect::OperationContext& context,
			const senp::effect::CompleteCommand& completion) {
			told.emplace_back(context, completion);
			return true;
		});
	const senp::effect::OperationContext context{ L"cmd.1", 3, 4, 5, 1 };
	const auto completion = Completion(senp::effect::CompletionStatus::Succeeded, L"done");
	ASSERT_TRUE(target.CompleteCommand(context, completion));
	ASSERT_EQ(1U, told.size());
	EXPECT_EQ(context, told[0].first);
	EXPECT_EQ(completion, told[0].second);
	// A rejected effect fails the owner's whole coordinator, so what reaches the
	// window has to be exactly what this owner's committed generations admit.
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.2", 9, 4, 5, 1 }, completion));
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.3", 3, 9, 5, 1 }, completion));
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.4", 3, 4, 9, 1 }, completion));
	EXPECT_FALSE(target.CompleteCommand({ L"", 3, 4, 5, 1 }, completion));
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.5", 3, 4, 5, 0 }, completion));
	EXPECT_EQ(1U, told.size());
	target.Revoke();
	EXPECT_FALSE(target.CompleteCommand(context, completion));
	EXPECT_EQ(1U, told.size());
}

TEST_F(SenpReadonlyOwnerTargetTest, RefusesACompletionWhenNothingIsThereToTellItTo)
{
	// Production held no sink until it was wired, and this is what that did:
	// refuse, which fails the coordinator and kills the owner on its first
	// completed command. The refusal is still correct; having no sink is not.
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 967000);
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.1", 3, 4, 5, 1 },
		Completion(senp::effect::CompletionStatus::Succeeded, L"done")));
}

TEST_F(SenpReadonlyOwnerTargetTest, KeepsTheOwnerAliveWhenTellingTheWindowThrows)
{
	CSenpReadonlyOwnerTarget target(Owner(), *controller, surface, 968000, nullptr, {},
		[](const senp::effect::OperationContext&, const senp::effect::CompleteCommand&) -> bool {
			throw std::bad_alloc();
		});
	// The seam is noexcept, so an escaping exception becomes a refusal of that
	// one completion rather than a terminate of the window that owns the target.
	EXPECT_FALSE(target.CompleteCommand({ L"cmd.1", 3, 4, 5, 1 },
		Completion(senp::effect::CompletionStatus::Failed, L"boom")));
}

} // namespace workbench::editor::tests
