/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/SenpOwnerComposition.h"
#include "workbench/SenpExtensionActivation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <stdexcept>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

class CompositionRuntime final : public senp::ISenpEffectRuntime {
public:
	explicit CompositionRuntime(senp::EffectRuntimeLaunch launch, std::function<bool()> confirmed = {})
		: m_confirmed(std::move(confirmed)), m_launch(std::move(launch)) {}
	senp::InvocationAdmission Start() override
	{
		m_state.phase = senp::RuntimePhase::Active;
		auto context = m_launch.context;
		context.operationId = L"activation";
		m_result = senp::InvocationResult{ context, true, senp::InvocationStatus::EffectsReady };
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	senp::InvocationAdmission Submit(senp::effect::OperationContext, senp::effect::Event,
		senp::CSenpRuntimeSession::Time) override { return { senp::AdmissionStatus::Busy }; }
	bool Cancel(std::wstring_view) override { return true; }
	void Stop(senp::effect::StopReason) override
	{
		m_state.phase = senp::RuntimePhase::Stopped;
		m_state.workerExited = m_state.processExitConfirmed = true;
	}
	void Join() override { if (m_confirmed && !m_confirmed()) throw std::runtime_error("unconfirmed exit"); }
	std::optional<senp::InvocationResult> TakeCompleted() override
	{
		auto result = std::move(m_result);
		m_result.reset();
		return result;
	}
	senp::EffectRuntimeSnapshot Snapshot() const override
	{
		auto state = m_state;
		if (m_confirmed) state.processExitConfirmed = m_confirmed();
		return state;
	}
private:
	std::function<bool()> m_confirmed;
	senp::EffectRuntimeLaunch m_launch;
	senp::EffectRuntimeSnapshot m_state;
	std::optional<senp::InvocationResult> m_result;
};

class CompositionTargetState final {
public:
	void Begin(std::wstring_view resource, const senp::effect::OperationContext& context)
	{
		m_resource.assign(resource); m_context = context; ++m_begins;
	}
	[[nodiscard]] bool Publish(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document)
	{
		if (context != m_context || document.resourceId != m_resource) return false;
		m_document = std::move(document); ++m_publishes; return true;
	}
	void Complete(senp::effect::CompleteCommand completion)
	{
		m_completion = std::move(completion); ++m_completions;
	}
	bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read)
	{
		// Both operations the GitHub tool boundary admits. A log is answered by
		// the same queue as a page because what distinguishes them here is the
		// shape of the answer, which is exactly what the extension has to tell
		// apart.
		if (read.toolId != L"github"
			|| (read.operation != L"repositoryRead" && read.operation != L"jobLog")
			|| m_toolTerminal || m_toolResponses.empty()) return false;
		m_lastRead = read;
		m_toolTerminal = SenpToolReadTerminal{ context, {
			read.readId, senp::effect::CompletionStatus::Succeeded,
			std::move(m_toolResponses.front()), L"" } };
		m_toolResponses.pop_front();
		++m_toolReads;
		return true;
	}
	std::optional<SenpToolReadTerminal> TakeToolRead()
	{
		auto terminal = std::move(m_toolTerminal);
		m_toolTerminal.reset();
		return terminal;
	}
	void CancelToolReads(const senp::effect::OperationContext& context)
	{
		if (m_toolTerminal && m_toolTerminal->Context().requestGeneration == context.requestGeneration)
			m_toolTerminal.reset();
	}
	void SetToolResponse(std::wstring value)
	{
		m_toolResponses.clear();
		m_toolResponses.push_back(std::move(value));
	}
	void EnqueueToolResponse(std::wstring value) { m_toolResponses.push_back(std::move(value)); }
	void Revoke() noexcept { ++m_revokes; }
	[[nodiscard]] int Begins() const noexcept { return m_begins; }
	[[nodiscard]] int Publishes() const noexcept { return m_publishes; }
	[[nodiscard]] int Completions() const noexcept { return m_completions; }
	[[nodiscard]] int Revokes() const noexcept { return m_revokes; }
	[[nodiscard]] int ToolReads() const noexcept { return m_toolReads; }
	[[nodiscard]] const senp::effect::StartToolRead& LastRead() const noexcept { return m_lastRead; }
	[[nodiscard]] const senp::effect::PublishDocument& Document() const noexcept { return m_document; }
private:
	std::wstring m_resource;
	senp::effect::OperationContext m_context;
	senp::effect::PublishDocument m_document;
	senp::effect::CompleteCommand m_completion;
	senp::effect::StartToolRead m_lastRead;
	std::optional<SenpToolReadTerminal> m_toolTerminal;
	std::deque<std::wstring> m_toolResponses;
	int m_begins{}, m_publishes{}, m_completions{}, m_revokes{}, m_toolReads{};
};

/*!
	@brief Builds the completion CSenpGitHubToolExecutor::Publish would send.

	Publish answers every repositoryRead - a page and a single item alike - with
	one envelope: the transfer it made, and the API response under "body". Canned
	data that left the envelope out let a detail read pass here while every real
	one failed to parse, so the fake tool target says the real shape or nothing.
*/
std::wstring Completion(const std::wstring_view body, const unsigned nextPage = 0)
{
	// The canned bodies are ASCII, so their length is the byte count Publish states.
	std::wstring data = LR"({"bytes":)" + std::to_wstring(body.size())
		+ LR"(,"httpStatus":200,"page":1)";
	if (nextPage != 0) data += LR"(,"nextPage":)" + std::to_wstring(nextPage);
	data += LR"(,"body":)";
	data += body;
	return data + L"}";
}

/*!
	@brief Asserts the shape and ids the last repository read named.

	The tool boundary admits a shape from a closed set plus the decimal ids that
	shape is keyed by; it refuses an argument list that names a path of its own.
	Asserting the names here is what proves the real extension speaks the
	vocabulary the real executor accepts. The two sides were once written apart
	and each passed its own tests while every real read was refused.
*/
void ExpectRead(const CompositionTargetState& target, std::wstring_view shape,
	std::wstring_view id = {}, std::wstring_view attempt = {})
{
	const auto& arguments = target.LastRead().arguments;
	ASSERT_FALSE(arguments.empty());
	EXPECT_EQ(L"shape", arguments[0].name);
	EXPECT_EQ(shape, arguments[0].value);
	const auto named = [&arguments](const std::wstring_view name) {
		const auto found = std::ranges::find(arguments, name, &senp::effect::Field::name);
		return found == arguments.end() ? std::wstring() : found->value;
	};
	EXPECT_EQ(id, named(L"id"));
	EXPECT_EQ(attempt, named(L"attempt"));
}

class CompositionTarget final : public ISenpOwnerProjectionTarget {
public:
	explicit CompositionTarget(std::shared_ptr<CompositionTargetState> state) noexcept
		: m_state(std::move(state)) {}
	bool BeginDocument(std::wstring_view resourceId,
		const senp::effect::OperationContext& context) noexcept override
	{
		m_state->Begin(resourceId, context); return true;
	}
	bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept override
	{
		return m_state->Publish(context, std::move(document));
	}
	bool FailDocument(const senp::effect::OperationContext&, senp::InvocationStatus) noexcept override
	{
		return false;
	}
	bool CompleteCommand(const senp::effect::OperationContext&,
		senp::effect::CompleteCommand completion) noexcept override
	{
		m_state->Complete(std::move(completion)); return true;
	}
	bool ReleaseResource(std::wstring_view) noexcept override { return true; }
	bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read) noexcept override
	{
		return m_state->StartToolRead(context, std::move(read));
	}
	std::optional<SenpToolReadTerminal> TakeToolRead() noexcept override
	{
		return m_state->TakeToolRead();
	}
	void CancelToolReads(const senp::effect::OperationContext& context) noexcept override
	{
		m_state->CancelToolReads(context);
	}
	void Revoke() noexcept override { m_state->Revoke(); }
private:
	std::shared_ptr<CompositionTargetState> m_state;
};

class CompositionBody final : public viewcontainer::ISenpViewBody {
public:
	explicit CompositionBody(viewcontainer::SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
			m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	~CompositionBody() override { Close(); }
	HWND Window() const noexcept override { return m_window; }
	void Layout(const RECT& bounds, unsigned int) noexcept override
	{
		if (m_window) (void)::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
	}
	void SetVisible(const bool visible) noexcept override
	{
		if (m_window) ::ShowWindow(m_window, visible ? SW_SHOWNA : SW_HIDE);
	}
	void SetPalette(const theme::ThemePalette&, layout::EViewContainerLocation) noexcept override {}
	bool Focus() noexcept override
	{
		if (!m_window) return false;
		::SetFocus(m_window);
		return ::GetFocus() == m_window;
	}
	bool PreTranslate(MSG&) noexcept override { return false; }
	void Close() noexcept override
	{
		m_host = {};
		if (m_window) { ::DestroyWindow(m_window); m_window = nullptr; }
	}
private:
	viewcontainer::SenpViewBodyHost m_host;
	HWND m_window{};
};

class SenpOwnerComposition : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }
	void SetUp() override
	{
		m_owner = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_owner);
	}
	void TearDown() override { if (m_owner) ::DestroyWindow(m_owner); }

	template<class Predicate>
	bool Await(CSenpOwnerComposition& composition, Predicate predicate) const
	{
		const auto deadline = Clock::now() + std::chrono::seconds(12);
		while (Clock::now() < deadline) {
			if (!composition.Poll(Clock::now())) return false;
			if (predicate()) return true;
			::Sleep(5);
		}
		return false;
	}

	static senp::ManagementSnapshot Packages()
	{
		senp::ExtensionDescriptor extension;
		extension.id = L"sample.factory";
		extension.enabled = true;
		extension.modulePath = L"extension.wasm";
		extension.moduleSha256.assign(64, L'a');
		extension.archiveSha256.assign(64, L'b');
		extension.runtime = { 2, L"sakura:senp/extension@2.0.0", { L"onView:sample.tree" }, {}, {} };
		extension.views = { { L"sample.tree", L"sample.container", L"Sample", L"senp.tree", 1 } };
		return { senp::EManagementState::Ready, 1, { std::move(extension) } };
	}

	SenpOwnerPublicationOptions NativeOptions(std::shared_ptr<CompositionTargetState> state,
		std::string suffix = {}) const
	{
		std::vector<SenpOwnerTreeContribution> trees;
		trees.emplace_back(layout::WorkbenchViewDescriptor{
			"sample.tree" + suffix, "sample.container" + suffix, "Sample", 1, true, true, "senp.tree" },
			std::vector<std::string>{});
		return { m_owner,
			{ { "sample.container" + suffix, "Sample", layout::EViewContainerLocation::Sidebar, 1,
				"beaker", false, { layout::EViewContainerLocation::Sidebar } } },
			std::move(trees), std::make_unique<CompositionTarget>(std::move(state)),
			[](std::string_view) { return true; },
			[](viewcontainer::SenpViewBodyHost host, std::shared_ptr<tree::SenpTreeProvider>, std::wstring) {
				return std::make_unique<CompositionBody>(std::move(host));
			} };
	}

	HWND m_owner{};
};

TEST_F(SenpOwnerComposition, ActivationIsLazyDeduplicatedAndReenabledWithNewScope)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	int starts{};
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		++starts;
		return std::make_unique<CompositionRuntime>(std::move(launch));
	});
	auto state = std::make_shared<CompositionTargetState>();
	CSenpExtensionActivation activation(composition, L"host.exe", [&](const auto&, const auto&) {
		return std::optional(NativeOptions(state));
	});
	auto packages = Packages();
	ASSERT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
	EXPECT_EQ(0, starts);
	EXPECT_EQ(SenpExtensionActivationState::Dormant, activation.State(L"sample.factory"));
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, activation.RequestView(L"foreign.tree", Clock::now()).status);
	const auto pending = activation.RequestView(L"sample.tree", Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, pending.status);
	EXPECT_EQ(pending.owner, activation.RequestView(L"sample.tree", Clock::now()).owner);
	ASSERT_TRUE(activation.Poll(Clock::now()));
	ASSERT_EQ(SenpExtensionActivationState::Active, activation.State(L"sample.factory"));
	EXPECT_EQ(pending.owner, activation.ActiveOwner(L"sample.factory"));
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, activation.RequestView(L"sample.tree", Clock::now(), true).status);
	EXPECT_EQ(1, starts);
	auto malformed = packages;
	++malformed.revision;
	malformed.extensions.push_back(malformed.extensions[0]);
	EXPECT_FALSE(activation.Synchronize(malformed, 7, 9, Clock::now()));
	EXPECT_EQ(pending.owner, activation.ActiveOwner(L"sample.factory"));
	EXPECT_TRUE(pages.Contains("sample.container"));
	++packages.revision;
	packages.extensions[0].enabled = false;
	ASSERT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Disabled, activation.State(L"sample.factory"));
	EXPECT_FALSE(pages.Contains("sample.container"));
	ASSERT_TRUE(activation.Poll(Clock::now()));
	++packages.revision;
	packages.extensions[0].enabled = true;
	ASSERT_TRUE(activation.Synchronize(packages, 8, 10, Clock::now()));
	ASSERT_TRUE(activation.Poll(Clock::now()));
	const auto current = activation.ActiveOwner(L"sample.factory");
	ASSERT_TRUE(current);
	EXPECT_NE(pending.owner.generation, current->generation);
	EXPECT_EQ(8, current->workspaceRevision);
	EXPECT_EQ(10, current->accountGeneration);
	EXPECT_EQ(2, starts);
	EXPECT_TRUE(activation.Close());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_FALSE(activation.Synchronize(packages, 8, 10, Clock::now()));
	EXPECT_EQ(senp::OwnerChangeStatus::Stopped, activation.RequestView(L"sample.tree", Clock::now()).status);
	pages.Close();
}

TEST_F(SenpOwnerComposition, ActivationSerializesDistinctOwnersWithoutRestartingPendingRequests)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	int starts{};
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		++starts;
		return std::make_unique<CompositionRuntime>(std::move(launch));
	});
	auto state = std::make_shared<CompositionTargetState>();
	CSenpExtensionActivation activation(composition, L"host.exe", [&](const auto& descriptor, const auto&) {
		return std::optional(NativeOptions(state, descriptor.id == L"sample.second" ? "2" : ""));
	});
	auto packages = Packages();
	auto second = packages.extensions[0];
	second.id = L"sample.second";
	second.views = { { L"sample.tree2", L"sample.container2", L"Sample", L"senp.tree", 1 } };
	second.runtime.activationEvents = { L"onView:sample.tree2" };
	packages.extensions.push_back(std::move(second));
	ASSERT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, activation.RequestView(L"sample.tree", Clock::now()).status);
	EXPECT_EQ(senp::OwnerChangeStatus::Busy, activation.RequestView(L"sample.tree2", Clock::now()).status);
	EXPECT_EQ(SenpExtensionActivationState::Queued, activation.State(L"sample.second"));
	EXPECT_EQ(1, starts);
	EXPECT_EQ(senp::OwnerChangeStatus::Busy, activation.RequestView(L"sample.tree2", Clock::now(), true).status);
	ASSERT_TRUE(activation.Poll(Clock::now()));
	EXPECT_EQ(2, starts);
	ASSERT_TRUE(activation.Poll(Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Active, activation.State(L"sample.factory"));
	EXPECT_EQ(SenpExtensionActivationState::Active, activation.State(L"sample.second"));
	EXPECT_EQ(2U, catalog.Snapshot().owners.size());
	EXPECT_TRUE(pages.Contains("sample.container"));
	EXPECT_TRUE(pages.Contains("sample.container2"));
	EXPECT_TRUE(activation.Close());
	pages.Close();
}

TEST_F(SenpOwnerComposition, ActivationFailuresRemainTerminalAndRejectMalformedAuthority)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	int starts{}, factories{};
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		++starts;
		return std::make_unique<CompositionRuntime>(std::move(launch));
	});
	CSenpExtensionActivation activation(composition, L"host.exe", [&](const auto&, const auto&) {
		++factories;
		return std::optional<SenpOwnerPublicationOptions>{};
	});
	auto packages = Packages();
	ASSERT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, activation.RequestView(L"sample.tree", Clock::now()).status);
	for (int n = 0; n < 5; ++n) {
		EXPECT_TRUE(activation.Poll(Clock::now()));
		EXPECT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
		EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, activation.RequestView(L"sample.tree", Clock::now()).status);
	}
	EXPECT_EQ(1, factories);
	EXPECT_EQ(0, starts);
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, activation.RequestView(L"sample.tree", Clock::now(), true).status);
	EXPECT_EQ(2, factories);
	auto invalid = packages;
	++invalid.revision;
	invalid.extensions.push_back(invalid.extensions[0]);
	EXPECT_FALSE(activation.Synchronize(invalid, 7, 9, Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Unsupported, activation.State(L"sample.factory"));
	invalid = packages;
	invalid.revision = 0;
	EXPECT_FALSE(activation.Synchronize(invalid, 7, 9, Clock::now()));
	++packages.revision;
	packages.extensions[0].runtime.activationEvents.clear();
	ASSERT_TRUE(activation.Synchronize(packages, 7, 9, Clock::now()));
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, activation.RequestView(L"sample.tree", Clock::now()).status);
	EXPECT_EQ(SenpExtensionActivationState::Unsupported, activation.State(L"sample.factory"));
	EXPECT_EQ(2, factories);
	packages.state = senp::EManagementState::Failed;
	EXPECT_FALSE(activation.Synchronize(packages, 7, 9, Clock::now()));
	EXPECT_FALSE(activation.State(L"sample.factory"));
	EXPECT_TRUE(activation.Close());
	pages.Close();
}

TEST_F(SenpOwnerComposition, CloseRetainsFailedRuntimeCleanupUntilExitIsConfirmed)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	bool exitConfirmed{};
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<CompositionRuntime>(std::move(launch), [&] { return exitConfirmed; });
	});
	auto state = std::make_shared<CompositionTargetState>();
	CSenpExtensionActivation activation(composition, L"host.exe", [&](const auto&, const auto&) {
		return std::optional(NativeOptions(state));
	});
	ASSERT_TRUE(activation.Synchronize(Packages(), 7, 9, Clock::now()));
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, activation.RequestView(L"sample.tree", Clock::now()).status);
	ASSERT_TRUE(activation.Poll(Clock::now()));
	EXPECT_FALSE(activation.Close());
	EXPECT_FALSE(activation.Close());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_FALSE(pages.Contains("sample.container"));
	exitConfirmed = true;
	EXPECT_TRUE(activation.Close());
	EXPECT_EQ(1, state->Revokes());
	pages.Close();
}

TEST_F(SenpOwnerComposition, FactoryUsesAllocatedOwnerAndRejectsReentrancy)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	std::optional<senp::EffectRuntimeLaunch> launched;
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		launched = launch;
		return std::make_unique<CompositionRuntime>(std::move(launch));
	});
	auto state = std::make_shared<CompositionTargetState>();
	std::optional<senp::ContributionOwnerIdentity> prepared;
	int calls{};
	const senp::EffectRuntimeLaunch launch{ .hostExecutable = L"host.exe", .modulePath = L"extension.wasm",
		.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.factory",
		.context = { .workspaceRevision = 7, .accountGeneration = 9 }, .generation = 999 };
	const auto accepted = composition.Activate(launch, std::wstring(64, L'b'),
		[&](const senp::ContributionOwnerIdentity& owner) -> std::optional<SenpOwnerPublicationOptions> {
			++calls;
			prepared = owner;
			EXPECT_FALSE(launched);
			EXPECT_EQ(senp::OwnerChangeStatus::Busy, composition.Activate(launch,
				std::wstring(64, L'b'), SenpOwnerPublicationFactory{}, Clock::now()).status);
			EXPECT_FALSE(composition.Close());
			std::vector<SenpOwnerTreeContribution> trees;
			trees.emplace_back(layout::WorkbenchViewDescriptor{
				"sample.tree", "sample.container", "Sample", 1, true, true, "senp.tree" },
				std::vector<std::string>{});
			return SenpOwnerPublicationOptions(m_owner,
				{ { "sample.container", "Sample", layout::EViewContainerLocation::Sidebar, 1,
					"beaker", false, { layout::EViewContainerLocation::Sidebar } } },
				std::move(trees), std::make_unique<CompositionTarget>(state),
				[](std::string_view) { return true; },
				[](viewcontainer::SenpViewBodyHost host, std::shared_ptr<tree::SenpTreeProvider>, std::wstring) {
					return std::make_unique<CompositionBody>(std::move(host));
				});
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	ASSERT_TRUE(prepared);
	ASSERT_TRUE(launched);
	EXPECT_EQ(*prepared, accepted.owner);
	EXPECT_EQ(prepared->generation, launched->generation);
	EXPECT_EQ(prepared->generation, launched->context.ownerGeneration);
	EXPECT_NE(999, prepared->generation);
	EXPECT_EQ(7, prepared->workspaceRevision);
	EXPECT_EQ(9, prepared->accountGeneration);
	EXPECT_TRUE(composition.Poll(Clock::now()));
	const auto terminal = composition.TakeTransition();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, terminal->status);
	EXPECT_EQ(1, calls);
	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(1, state->Revokes());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	pages.Close();
}

TEST_F(SenpOwnerComposition, FailedFactoryStartsNoRuntimeAndLeavesNoPublication)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	int starts{};
	CSenpOwnerComposition composition(catalog, pages, [&](senp::EffectRuntimeLaunch launch) {
		++starts;
		return std::make_unique<CompositionRuntime>(std::move(launch));
	});
	const senp::EffectRuntimeLaunch launch{ .hostExecutable = L"host.exe", .modulePath = L"extension.wasm",
		.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.factory" };
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, composition.Activate(launch, std::wstring(64, L'b'),
		SenpOwnerPublicationFactory{}, Clock::now()).status);
	EXPECT_EQ(senp::OwnerChangeStatus::Unsupported, composition.Activate(launch, std::wstring(64, L'b'),
		[](const senp::ContributionOwnerIdentity&) -> std::optional<SenpOwnerPublicationOptions> { return {}; },
		Clock::now()).status);
	EXPECT_EQ(senp::OwnerChangeStatus::Failed, composition.Activate(launch, std::wstring(64, L'b'),
		[](const senp::ContributionOwnerIdentity&) -> std::optional<SenpOwnerPublicationOptions> {
			throw std::runtime_error("factory failure");
		}, Clock::now()).status);
	EXPECT_EQ(0, starts);
	EXPECT_EQ(0U, composition.Snapshot().preparing);
	EXPECT_FALSE(composition.TakeTransition());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(senp::OwnerChangeStatus::Stopped, composition.Activate(launch, std::wstring(64, L'b'),
		SenpOwnerPublicationFactory{}, Clock::now()).status);
	pages.Close();
}

TEST_F(SenpOwnerComposition, RealSamplePublishesTwoTreesAndStructuredDocument)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"sample-extension.wasm";
	std::ifstream digestFile(fixtures / L"sample-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"sample.senp", "SENP Sample", layout::EViewContainerLocation::Sidebar, 90,
		"$(beaker)", false, { layout::EViewContainerLocation::Sidebar },
	};
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"sample.projects", "sample.senp", "Projects", 10, true, true, "senp.tree" },
		std::vector<std::string>{ "sample.openDetails" });
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"sample.states", "sample.senp", "States", 20, true, true, "senp.tree" },
		std::vector<std::string>{});
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-senp-sample",
		.context = { .workspaceRevision = 3, .accountGeneration = 0 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());
	ASSERT_TRUE(pages.Contains("sample.senp"));

	for (auto& [id, provider] : providers) provider->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"sample.projects")->Model().ItemCount() == 2
			&& providers.at(L"sample.states")->Model().ItemCount() == 3;
	}));
	auto projects = providers.at(L"sample.projects");
	ASSERT_TRUE(projects->Select(L"project:alpha"));
	ASSERT_TRUE(projects->Execute(L"project:alpha"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
	EXPECT_EQ(1, target->Begins());
	EXPECT_EQ(1, target->Completions());
	EXPECT_EQ(L"Alpha details", target->Document().title);
	ASSERT_EQ(4U, target->Document().sections.size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::TextResourceSection>(
		target->Document().sections.back()));

	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(1, target->Revokes());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	pages.Close();
}

TEST_F(SenpOwnerComposition, RealGithubIssuesAndPullRequestsReachNativeProviders)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"github-pull-requests-extension.wasm";
	std::ifstream digestFile(fixtures / L"github-pull-requests-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	target->SetToolResponse(Completion(LR"([{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2},{"id":12,"number":8,"title":"Filtered PR","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","comments":0,"pull_request":{}}])", 2));
	target->EnqueueToolResponse(Completion(LR"({"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2,"body":"Issue body","created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z"})"));
	target->EnqueueToolResponse(Completion(LR"([{"id":91,"user":{"login":"hubot"},"body":"Comment body","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"}])", 2));
	target->EnqueueToolResponse(Completion(LR"({"id":91,"user":{"login":"hubot"},"body":"Comment body","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"})"));
	target->EnqueueToolResponse(Completion(LR"([{"id":51,"number":8,"title":"Cross-fork change","state":"open","user":{"login":"contributor"},"labels":[{"name":"ready"}],"html_url":"https://github.com/base/project/pull/8","comments":0,"draft":false,"merged_at":null,"base":{"ref":"main","sha":"bbbb","repo":{"full_name":"base/project"}},"head":{"ref":"feature","sha":"hhhh","repo":{"full_name":"fork/project"}}}])"));
	target->EnqueueToolResponse(Completion(LR"({"id":51,"number":8,"title":"Cross-fork change","state":"closed","user":{"login":"contributor"},"labels":[{"name":"ready"}],"html_url":"https://github.com/base/project/pull/8","comments":0,"draft":false,"merged_at":"2026-09-03T00:00:00Z","base":{"ref":"main","sha":"bbbb","repo":{"full_name":"base/project"}},"head":{"ref":"feature","sha":"hhhh","repo":{"full_name":"fork/project"}},"body":"Pull request body","created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-03T00:00:00Z"})"));
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"github-pull-requests", "GitHub", layout::EViewContainerLocation::Sidebar, 6,
		"$(github)", false, { layout::EViewContainerLocation::Sidebar },
	};
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"pr:github", "github-pull-requests", "Pull Requests", 10, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openPullRequest" });
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"issues:github", "github-pull-requests", "Issues", 20, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openIssue", "github.openIssueComment" });
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-github-pull-requests",
		.context = { .workspaceRevision = 3, .accountGeneration = 4 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());

	providers.at(L"issues:github")->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"issues:github")->Model().ItemCount() == 1;
	}));
	EXPECT_EQ(1, target->ToolReads());
	EXPECT_EQ(L"issues:open:1", target->LastRead().readId);
	const auto& model = providers.at(L"issues:github")->Model();
	const auto issue = model.Node(L"issue:11:7");
	ASSERT_TRUE(issue);
	EXPECT_EQ(L"#7 Visible issue", issue->item.label);
	const auto root = model.Node(L"");
	ASSERT_TRUE(root);
	EXPECT_EQ(L"issues:open:2", root->nextCursor);

	ASSERT_TRUE(providers.at(L"issues:github")->Select(L"issue:11:7"));
	ASSERT_TRUE(providers.at(L"issues:github")->Execute(L"issue:11:7"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
	EXPECT_EQ(L"#7 Visible issue", target->Document().title);
	ASSERT_EQ(2U, target->Document().sections.size());
	ASSERT_TRUE(std::holds_alternative<senp::effect::MarkdownSection>(
		target->Document().sections[1]));
	EXPECT_EQ(L"Issue body", std::get<senp::effect::MarkdownSection>(
		target->Document().sections[1]).text);

	ASSERT_EQ(tree::TreeResult::Applied, providers.at(L"issues:github")->SetExpanded(
		L"issue:11:7", true, Clock::now()));
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"issues:github")->Model().Node(L"comment:91").has_value();
	}));
	const auto comment = providers.at(L"issues:github")->Model().Node(L"comment:91");
	ASSERT_TRUE(comment);
	EXPECT_EQ(L"comments:2", providers.at(L"issues:github")->Model().Node(
		L"issue:11:7")->nextCursor);
	ASSERT_TRUE(providers.at(L"issues:github")->Select(L"comment:91"));
	ASSERT_TRUE(providers.at(L"issues:github")->Execute(L"comment:91"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 2; }));
	EXPECT_EQ(L"Comment by @hubot", target->Document().title);
	ASSERT_TRUE(std::holds_alternative<senp::effect::MarkdownSection>(
		target->Document().sections[1]));
	EXPECT_EQ(L"Comment body", std::get<senp::effect::MarkdownSection>(
		target->Document().sections[1]).text);
	EXPECT_EQ(4, target->ToolReads());

	providers.at(L"pr:github")->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"pr:github")->Model().ItemCount() == 1;
	}));
	const auto pull = providers.at(L"pr:github")->Model().Node(L"pull:51:8");
	ASSERT_TRUE(pull);
	EXPECT_EQ(L"#8 Cross-fork change", pull->item.label);
	ASSERT_TRUE(providers.at(L"pr:github")->Select(L"pull:51:8"));
	ASSERT_TRUE(providers.at(L"pr:github")->Execute(L"pull:51:8"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 3; }));
	EXPECT_EQ(L"#8 Cross-fork change", target->Document().title);
	ASSERT_TRUE(std::holds_alternative<senp::effect::MetadataSection>(
		target->Document().sections[0]));
	const auto& fields = std::get<senp::effect::MetadataSection>(
		target->Document().sections[0]).fields;
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"State" && field.value == L"merged";
	}));
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"Base" && field.value.find(L"base/project:main") != std::wstring::npos;
	}));
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"Head" && field.value.find(L"fork/project:feature") != std::wstring::npos;
	}));
	ExpectRead(*target, L"pull", L"8");
	EXPECT_EQ(6, target->ToolReads());
	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(1, target->Revokes());
	pages.Close();
}

TEST_F(SenpOwnerComposition, RealGithubActionsReachNativeProviders)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"github-actions-extension.wasm";
	std::ifstream digestFile(fixtures / L"github-actions-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	target->EnqueueToolResponse(Completion(LR"({"total_count":1,"workflows":[{"id":31,"name":"Build","path":".github/workflows/build.yml","state":"active","html_url":"https://github.com/o/r/actions/workflows/build.yml"}]})"));
	target->EnqueueToolResponse(Completion(LR"({"total_count":1,"workflow_runs":[{"id":51,"workflow_id":31,"run_number":8,"run_attempt":2,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"}]})"));
	target->EnqueueToolResponse(Completion(LR"json({"total_count":1,"jobs":[{"id":72,"run_id":51,"run_attempt":2,"name":"Build (Windows)","status":"in_progress","conclusion":null,"started_at":null,"completed_at":null,"html_url":"https://github.com/o/r/actions/runs/51/job/72","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]}]})json"));
	target->EnqueueToolResponse(Completion(LR"({"id":51,"workflow_id":31,"run_number":8,"run_attempt":1,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"})"));
	target->EnqueueToolResponse(Completion(LR"json({"total_count":1,"jobs":[{"id":71,"run_id":51,"run_attempt":1,"name":"Build (Windows)","status":"completed","conclusion":"success","started_at":"2026-09-01T00:00:00Z","completed_at":"2026-09-01T00:01:30Z","html_url":"https://github.com/o/r/actions/runs/51/job/71","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]}]})json"));
	target->EnqueueToolResponse(Completion(LR"json({"id":71,"run_id":51,"run_attempt":1,"name":"Build (Windows)","status":"completed","conclusion":"success","started_at":"2026-09-01T00:00:00Z","completed_at":"2026-09-01T00:01:30Z","html_url":"https://github.com/o/r/actions/runs/51/job/71","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]})json"));
	target->EnqueueToolResponse(Completion(LR"json({"id":71,"run_id":51,"run_attempt":1,"name":"Build (Windows)","status":"completed","conclusion":"success","started_at":"2026-09-01T00:00:00Z","completed_at":"2026-09-01T00:01:30Z","html_url":"https://github.com/o/r/actions/runs/51/job/71","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]})json"));
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"github-actions", "GitHub Actions", layout::EViewContainerLocation::Sidebar, 6,
		"$(play-circle)", false, { layout::EViewContainerLocation::Sidebar },
	};
	// The manifest's one inline action: upstream draws "View job logs" on a job
	// row whose contextValue says it has completed.
	const std::vector<tree::SenpTreeItemAction> itemActions{ { L"github-actions.workflow.logs",
		L"View job logs", L"resources/icons/light/logs.svg", { L"job", L"completed" }, {} } };
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
        "github-actions.workflows", "github-actions", "Workflows", 10, true, true, "senp.tree" },
        std::vector<std::string>{ "github-actions.workflow.run.open",
            "sakura.githubActions.openJobDetails", "github-actions.workflow.logs" }, itemActions);
    trees.emplace_back(layout::WorkbenchViewDescriptor{
        "github-actions.current-branch", "github-actions", "Current Branch", 20, true, true, "senp.tree" },
        std::vector<std::string>{ "github-actions.workflow.run.open",
            "sakura.githubActions.openJobDetails", "github-actions.workflow.logs" }, itemActions);
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-github-actions",
		.context = { .workspaceRevision = 3, .accountGeneration = 4 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());


    const auto provider = providers.at(L"github-actions.workflows");
    provider->SetVisible(true, Clock::now());
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"workflow:31").has_value(); }));
    ExpectRead(*target, L"workflows");
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"workflow:31", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"run:51:2").has_value(); }));
    ExpectRead(*target, L"workflowRuns", L"31");
    // Upstream's run row: the number under WORKFLOWS, state as the icon, and
    // status and trigger in the tooltip rather than in the row text.
    const auto runItem = provider->Model().Node(L"run:51:2")->item;
    EXPECT_EQ(L"#8", runItem.label);
    EXPECT_EQ(L"resources/icons/workflowruns/wr_inprogress.svg", runItem.icon);
    EXPECT_TRUE(runItem.description.empty());
    EXPECT_EQ(L"Attempt #2 In progress\n\nRe-run", runItem.tooltip);
    EXPECT_EQ(L"run", runItem.contextValue);
    // As upstream, a run lists its latest attempt's jobs and then, for a rerun,
    // "Previous attempts". The earlier attempts need no read of their own.
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"run:51:2", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"previous:51:2").has_value(); }));
    ExpectRead(*target, L"runAttemptJobs", L"51", L"2");
    ASSERT_TRUE(provider->Model().Node(L"job:51:2:72").has_value());
    EXPECT_EQ(L"job", provider->Model().Node(L"job:51:2:72")->item.contextValue);
    // An unfinished job has no log yet, so its row draws no inline action.
    EXPECT_TRUE(provider->ItemActions(L"job:51:2:72").empty());
    EXPECT_EQ(3, target->ToolReads());
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"previous:51:2", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"attempt:51:1").has_value(); }));
    EXPECT_FALSE(provider->Model().Node(L"attempt:51:2").has_value());
    EXPECT_EQ(3, target->ToolReads());
    ASSERT_TRUE(provider->Select(L"attempt:51:1"));
    ASSERT_TRUE(provider->Execute(L"attempt:51:1"));
    ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
    EXPECT_EQ(L"github-actions-run:51:1", target->Document().resourceId);
    ASSERT_TRUE(std::holds_alternative<senp::effect::MetadataSection>(target->Document().sections[0]));
    const auto& fields = std::get<senp::effect::MetadataSection>(target->Document().sections[0]).fields;
    EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
        return field.name == L"Attempt" && field.value == L"1";
    }));
    EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
        return field.name == L"State" && field.value == L"in_progress";
    }));
    ExpectRead(*target, L"runAttempt", L"51", L"1");
    EXPECT_EQ(4, target->ToolReads());
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"attempt:51:1", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"job:51:1:71").has_value(); }));
    ExpectRead(*target, L"runAttemptJobs", L"51", L"1");
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"job:51:1:71", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"step:51:1:71:7").has_value(); }));
    const auto jobItem = provider->Model().Node(L"job:51:1:71")->item;
    EXPECT_EQ(L"resources/icons/workflowruns/wr_success.svg", jobItem.icon);
    EXPECT_EQ(L"Succeeded in 1m 30s", jobItem.tooltip);
    EXPECT_EQ(L"job completed", jobItem.contextValue);
    const auto stepItem = provider->Model().Node(L"step:51:1:71:7")->item;
    EXPECT_EQ(L"Compile", stepItem.label);
    EXPECT_EQ(L"resources/icons/steps/step_queued.svg", stepItem.icon);
    EXPECT_TRUE(stepItem.description.empty());
    EXPECT_EQ(L"step", stepItem.contextValue);
    ASSERT_TRUE(provider->Select(L"job:51:1:71"));
    ASSERT_TRUE(provider->Execute(L"job:51:1:71"));
    ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 2; }));
    EXPECT_EQ(L"github-actions-job:51:1:71", target->Document().resourceId);
    EXPECT_EQ(L"Build (Windows)", target->Document().title);
    ASSERT_EQ(2U, target->Document().sections.size());
    ASSERT_TRUE(std::holds_alternative<senp::effect::TableSection>(target->Document().sections[1]));
    const auto& table = std::get<senp::effect::TableSection>(target->Document().sections[1]);
    ASSERT_EQ(1U, table.rows.size());
    EXPECT_EQ((std::vector<std::wstring>{ L"7", L"Compile", L"queued", L"not started", L"not completed" }), table.rows[0].cells);
    ExpectRead(*target, L"job", L"71");
    EXPECT_EQ(7, target->ToolReads());

    // The job's log is upstream's inline row action: a separate command reading a
    // separate operation, and the document it publishes names bytes the
    // extension never held. Nothing else proves that end of the path: an
    // extension has no effect for reading a resource, so a log can only reach
    // the reader as a text-resource section.
    target->EnqueueToolResponse(LR"json({"resource":"text:9:1","bytes":8192,"log":true})json");
    EXPECT_FALSE(provider->Model().Node(L"joblog:51:1:71").has_value());
    const auto actions = provider->ItemActions(L"job:51:1:71");
    ASSERT_EQ(1U, actions.size());
    EXPECT_EQ(L"github-actions.workflow.logs", actions.front().commandId);
    ASSERT_TRUE(provider->ExecuteItemAction(L"job:51:1:71", L"github-actions.workflow.logs"));
    ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 3; }));
    EXPECT_EQ(L"github-actions-job-log:51:1:71", target->Document().resourceId);
    EXPECT_EQ(L"jobLog", target->LastRead().operation);
    ASSERT_EQ(1U, target->LastRead().arguments.size());
    EXPECT_EQ(L"id", target->LastRead().arguments.front().name);
    EXPECT_EQ(L"71", target->LastRead().arguments.front().value);
    ASSERT_EQ(2U, target->Document().sections.size());
    ASSERT_TRUE(std::holds_alternative<senp::effect::TextResourceSection>(target->Document().sections[1]));
    const auto& log = std::get<senp::effect::TextResourceSection>(target->Document().sections[1]);
    EXPECT_EQ(L"text:9:1", log.handle);
    EXPECT_EQ(8192, log.length);
    EXPECT_EQ(senp::effect::TextStatus::Complete, log.status);
    EXPECT_EQ(8, target->ToolReads());
    EXPECT_TRUE(composition.Close());
    EXPECT_EQ(1, target->Revokes());
    pages.Close();
}

} // namespace
} // namespace workbench
