/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/SenpOwnerPublication.h"
#include "workbench/SenpViewDeclarations.h"
#include "workbench/SenpExtensionActivation.h"
#include "workbench/SenpWindowExtensions.h"

#include <deque>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

struct TargetState final { int revoked{}; };
class Target final : public ISenpOwnerProjectionTarget {
public:
	explicit Target(std::shared_ptr<TargetState> state) : m_state(std::move(state)) {}
	bool BeginDocument(std::wstring_view, const senp::effect::OperationContext&) noexcept override { return true; }
	bool PublishDocument(const senp::effect::OperationContext&, senp::effect::PublishDocument) noexcept override { return true; }
	bool FailDocument(const senp::effect::OperationContext&, senp::InvocationStatus) noexcept override { return true; }
	bool CompleteCommand(const senp::effect::OperationContext&, senp::effect::CompleteCommand) noexcept override { return true; }
	bool ReleaseResource(std::wstring_view) noexcept override { return true; }
	void Revoke() noexcept override { ++m_state->revoked; }
private:
	std::shared_ptr<TargetState> m_state;
};

class Body final : public viewcontainer::ISenpViewBody {
public:
	explicit Body(viewcontainer::SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
			m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	~Body() override { Close(); }
	HWND Window() const noexcept override { return m_window; }
	void Layout(const RECT& bounds, unsigned int) noexcept override
	{
		if (m_window) (void)::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
	}
	void SetVisible(bool visible) noexcept override { if (m_window) ::ShowWindow(m_window, visible ? SW_SHOWNA : SW_HIDE); }
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

struct RuntimeLifecycle final {
	int starts{}, stops{}, joins{}, destroyed{};
	bool permitExit{ true };
	//! Off by default: most of these tests want a runtime that admits nothing, so
	//! that a publication's own bookkeeping is what they observe. A test that
	//! needs to read the events themselves opts in.
	bool admitEvents{};
	std::vector<senp::effect::Event> events;
	std::function<void()> onStop;
};

class Runtime final : public senp::ISenpEffectRuntime {
public:
	Runtime(senp::EffectRuntimeLaunch launch, std::vector<senp::effect::Effect> activation,
		std::shared_ptr<RuntimeLifecycle> lifecycle = {})
		: m_launch(std::move(launch)), m_activation(std::move(activation)), m_lifecycle(std::move(lifecycle)) {}
	~Runtime() override { if (m_lifecycle) ++m_lifecycle->destroyed; }
	senp::InvocationAdmission Start() override
	{
		if (m_lifecycle) ++m_lifecycle->starts;
		m_state.phase = senp::RuntimePhase::Active;
		auto context = m_launch.context;
		context.operationId = L"activation";
		m_results.push_back({ context, true, senp::InvocationStatus::EffectsReady, {}, std::move(m_activation) });
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	senp::InvocationAdmission Submit(senp::effect::OperationContext context, senp::effect::Event event,
		senp::CSenpRuntimeSession::Time) override
	{
		if (!m_lifecycle || !m_lifecycle->admitEvents) return { senp::AdmissionStatus::Busy };
		// An admitted request needs a result, or the owner keeps waiting for one
		// and the next turn is refused rather than merely quiet.
		context.operationId = L"request." + std::to_wstring(++m_sequence);
		m_lifecycle->events.push_back(std::move(event));
		m_results.push_back({ context, false, senp::InvocationStatus::EffectsReady });
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	bool Cancel(std::wstring_view) override { return true; }
	void Stop(senp::effect::StopReason) override
	{
		if (m_lifecycle) {
			++m_lifecycle->stops;
			if (m_lifecycle->onStop) m_lifecycle->onStop();
			if (!m_lifecycle->permitExit) return;
		}
		m_state.phase = senp::RuntimePhase::Stopped;
		m_state.workerExited = true;
		m_state.processExitConfirmed = true;
	}
	void Join() override
	{
		if (!m_lifecycle) return;
		++m_lifecycle->joins;
		if (!m_lifecycle->permitExit) throw std::runtime_error("injected join failure");
		m_state.phase = senp::RuntimePhase::Stopped;
		m_state.workerExited = true; m_state.processExitConfirmed = true;
	}
	std::optional<senp::InvocationResult> TakeCompleted() override
	{
		if (m_results.empty()) return {};
		auto result = std::move(m_results.front());
		m_results.pop_front();
		return result;
	}
	senp::EffectRuntimeSnapshot Snapshot() const override { return m_state; }
private:
	senp::EffectRuntimeLaunch m_launch;
	std::vector<senp::effect::Effect> m_activation;
	std::deque<senp::InvocationResult> m_results;
	senp::EffectRuntimeSnapshot m_state;
	std::shared_ptr<RuntimeLifecycle> m_lifecycle;
	std::int64_t m_sequence{};
};

struct DeclaredState final {
	std::optional<senp::ContributionOwnerIdentity> owner;
	std::vector<SenpOwnerBoundTree> bindings;
	bool permitCommit{ true };
	int pumped{};
	int cleared{};
};

class DeclaredPublication final : public ISenpDeclaredTreePublication {
public:
	DeclaredPublication(std::shared_ptr<DeclaredState> state,
		senp::CSenpContributionOwners& owners, senp::ContributionOwnerIdentity owner,
		std::vector<SenpOwnerBoundTree> bindings)
		: m_state(std::move(state)), m_owners(owners), m_owner(std::move(owner)),
		m_previous(m_state->owner), m_bindings(std::move(bindings)) {}
	bool CanCommit() const noexcept override
	{
		return !m_closed && !m_committed && m_state->permitCommit && m_state->owner == m_previous;
	}
	bool Commit() noexcept override
	{
		if (!CanCommit()) return false;
		m_state->owner = std::move(m_owner);
		m_state->bindings = std::move(m_bindings);
		m_generation = m_state->owner->generation;
		m_committed = true;
		return true;
	}
	bool Pump() noexcept override
	{
		if (!m_committed || m_closed || !m_state->owner
			|| m_state->owner->generation != m_generation) return false;
		EXPECT_TRUE(m_owners.IsCurrent(*m_state->owner));
		++m_state->pumped;
		return true;
	}
	void Close() noexcept override
	{
		if (m_closed) return;
		m_closed = true;
		if (!m_committed || !m_state->owner || m_state->owner->generation != m_generation) return;
		m_state->owner.reset();
		m_state->bindings.clear();
		++m_state->cleared;
	}
private:
	std::shared_ptr<DeclaredState> m_state;
	senp::CSenpContributionOwners& m_owners;
	senp::ContributionOwnerIdentity m_owner;
	std::optional<senp::ContributionOwnerIdentity> m_previous;
	std::vector<SenpOwnerBoundTree> m_bindings;
	std::int64_t m_generation{};
	bool m_committed{};
	bool m_closed{};
};

class SenpOwnerPublicationTest : public testing::Test, public env::ShareDataTestSuite {
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

	SenpOwnerPublicationOptions Options(std::shared_ptr<TargetState> target) const
	{
		layout::WorkbenchViewContainerDescriptor container{
			"sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
			{ layout::EViewContainerLocation::Sidebar },
		};
		layout::WorkbenchViewDescriptor view{
			"sample.projects", container.id, "Projects", 10, true, true, "senp.tree",
		};
		return {
			m_owner, { std::move(container) }, { { std::move(view), { "sample.open" } } },
			std::make_unique<Target>(std::move(target)), [](std::string_view) { return true; },
			[](viewcontainer::SenpViewBodyHost host, std::shared_ptr<tree::SenpTreeProvider>, std::wstring) {
				auto body = std::make_unique<Body>(std::move(host));
				return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
			},
		};
	}

	static senp::EffectRuntimeLaunch Launch()
	{
		return { .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.extension",
			.context = { .workspaceRevision = 7, .accountGeneration = 9 } };
	}
	static senp::ManagementSnapshot Packages()
	{
		senp::ExtensionDescriptor extension;
		extension.id = L"sample.extension"; extension.enabled = true;
		extension.modulePath = L"module.wasm"; extension.moduleSha256 = std::wstring(64, L'a');
		extension.archiveSha256 = std::wstring(64, L'b');
		extension.runtime = { 2, L"sakura:senp/extension@2.0.0", { L"onView:sample.projects" }, {},
			{ { L"sample.open", L"Open" } } };
		extension.viewContainers = { { L"sample.senp", L"Sample", L"$(github)", 10 } };
		extension.views = { { L"sample.projects", L"sample.senp", L"Projects", L"senp.tree", 10 } };
		return { senp::EManagementState::Ready, 1, { std::move(extension) } };
	}
	static senp::ManagementSnapshot PackagesWithRefresh()
	{
		auto snapshot = Packages();
		auto& runtime = snapshot.extensions.front().runtime;
		runtime.commands.push_back({ L"sample.refresh", L"Refresh", L"$(refresh)" });
		runtime.viewTitle = { { L"sample.refresh", { L"sample.projects" } } };
		return snapshot;
	}
	//! The pane whose header (control 1) carries the View title; its title
	//! actions are controls 2 onward.
	static HWND FindPane(HWND owner, const wchar_t* title)
	{
		struct Search final { const wchar_t* title; HWND pane; } search{ title, nullptr };
		::EnumChildWindows(owner, [](HWND window, LPARAM parameter) -> BOOL {
			auto& found = *reinterpret_cast<Search*>(parameter);
			wchar_t text[64]{};
			if (::GetDlgCtrlID(window) != 1 || !::GetWindowTextW(window, text, 64) || std::wcscmp(text, found.title) != 0) return TRUE;
			found.pane = ::GetParent(window);
			return FALSE;
		}, reinterpret_cast<LPARAM>(&search));
		return search.pane;
	}

	HWND m_owner{};
};

TEST_F(SenpOwnerPublicationTest, WindowPackagesKeepDeclarationsDormantAndRetireBeforeRemovingPages)
{
	layout::WorkbenchContributionRegistry catalog;
	// A removed foreign generation must remain in the allocation history.
	auto foreign = catalog.PrepareOwnerReplacement({ "foreign.extension", 100 }, 0, {}, {});
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(foreign.change)));
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.DisposeOwner({ "foreign.extension", 100 }));
	CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	auto target = std::make_shared<TargetState>(); auto runtime = std::make_shared<RuntimeLifecycle>();
	int factories{};
	runtime->onStop = [&] { EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_GT(target->revoked, 0); };
	CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
		[&](const auto& descriptor, const auto& owner) {
			++factories; EXPECT_EQ(descriptor.id, owner.extensionId);
			EXPECT_GT(owner.generation, 0); EXPECT_EQ(7, owner.workspaceRevision); EXPECT_EQ(9, owner.accountGeneration);
			return std::make_unique<Target>(target);
		}, [](std::string_view) { return true; },
		[runtime](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, runtime); });
	auto snapshot = Packages();
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_EQ(101U, catalog.Snapshot().owners.front().generation);
	EXPECT_EQ(SenpExtensionActivationState::Dormant, extensions.State(L"sample.extension"));
	ASSERT_TRUE(extensions.Poll(Clock::now())); EXPECT_EQ(0, factories); EXPECT_EQ(0, runtime->starts);
	const auto revision = catalog.Snapshot().revision;
	ASSERT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
	ASSERT_TRUE(extensions.Poll(Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Active, extensions.State(L"sample.extension"));
	EXPECT_EQ(1, factories); EXPECT_EQ(1, runtime->starts); EXPECT_EQ(revision, catalog.Snapshot().revision);
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_EQ(1, factories);
	snapshot.revision++; snapshot.extensions.front().enabled = false;
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_EQ(1, runtime->stops); EXPECT_EQ(1, target->revoked); EXPECT_FALSE(pages.Contains("sample.senp"));
	ASSERT_TRUE(extensions.Poll(Clock::now())); EXPECT_EQ(1, runtime->destroyed);
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	snapshot.revision++; snapshot.extensions.front().enabled = true;
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_EQ(102U, catalog.Snapshot().owners.front().generation); EXPECT_EQ(1, factories);
	EXPECT_EQ(SenpExtensionActivationState::Dormant, extensions.State(L"sample.extension"));
	EXPECT_TRUE(extensions.Close()); EXPECT_FALSE(pages.Contains("sample.senp"));
	EXPECT_EQ(SenpWindowExtensionsStatus::Stopped, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_FALSE(extensions.Poll(Clock::now())); pages.Close();
}

TEST_F(SenpOwnerPublicationTest, WindowPackagesCarryAnImagePathContainerIconVerbatim)
{
	// Upstream packages name their container icon by a package-relative image
	// path. The catalog keeps it verbatim for the Activity Bar's compiled-in
	// vocabulary; an unbounded path is refused before any declaration exists.
	const auto run = [&](std::wstring icon, std::string& published) {
		layout::WorkbenchContributionRegistry catalog;
		CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
		EXPECT_TRUE(pages.Create(m_owner));
		auto target = std::make_shared<TargetState>();
		CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
			[&](const auto&, const auto&) { return std::make_unique<Target>(target); },
			[](std::string_view) { return true; },
			[](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}); });
		auto snapshot = Packages();
		snapshot.extensions.front().viewContainers.front().icon = std::move(icon);
		const auto status = extensions.Synchronize(snapshot, 7, 9, Clock::now());
		for (const auto& registered : catalog.Snapshot().viewContainers) {
			if (registered.descriptor.id == "sample.senp") published = registered.descriptor.icon;
		}
		EXPECT_TRUE(extensions.Close()); pages.Close();
		return status;
	};
	std::string published;
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, run(L"resources/icons/light/explorer.svg", published));
	EXPECT_EQ("resources/icons/light/explorer.svg", published);
	published.clear();
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, run(L"$(github)", published));
	EXPECT_EQ("github", published);
	published.clear();
	EXPECT_NE(SenpWindowExtensionsStatus::Synchronized, run(L"../explorer.svg", published));
	EXPECT_TRUE(published.empty());
}

TEST_F(SenpOwnerPublicationTest, ViewTitleActionsReachOnlyTheBoundRuntime)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	auto target = std::make_shared<TargetState>(); auto runtime = std::make_shared<RuntimeLifecycle>();
	runtime->admitEvents = true;
	CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
		[target](const auto&, const auto&) { return std::make_unique<Target>(target); },
		[](std::string_view) { return true; },
		[runtime](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, runtime); });
	auto snapshot = PackagesWithRefresh();
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	const auto pane = FindPane(m_owner, L"Projects");
	ASSERT_NE(nullptr, pane);
	const auto refresh = ::GetDlgItem(pane, 2);
	ASSERT_NE(nullptr, refresh);
	// Mount the page in a visible host, so a refused click below is refused for
	// want of a binding rather than because the pane is not on screen.
	const auto host = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		0, 0, 320, 400, m_owner, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, host);
	ASSERT_TRUE(pages.Attach("sample.senp", { "test.host", layout::EViewContainerLocation::Sidebar,
		reinterpret_cast<viewcontainer::ViewContainerNativeHandle>(host) }).Succeeded());
	const RECT bounds{ 0, 0, 320, 400 };
	pages.LayoutPageProjection("sample.senp", bounds, bounds, 96);
	pages.SetPageVisible("sample.senp", true);
	const auto invoked = [&] {
		return std::ranges::count_if(runtime->events, [](const auto& event) {
			const auto* command = std::get_if<senp::effect::CommandInvoked>(&event);
			return command && command->commandId == L"sample.refresh" && command->arguments.empty();
		});
	};
	const auto click = [&] {
		::SendMessageW(pane, WM_COMMAND, MAKEWPARAM(2, BN_CLICKED), reinterpret_cast<LPARAM>(refresh));
		(void)extensions.Poll(Clock::now());
	};
	// SENP activates on `onView:` alone: a click with no bound runtime neither
	// starts the extension nor queues the command for a later one.
	click();
	EXPECT_EQ(0, runtime->starts); EXPECT_EQ(0, invoked());
	EXPECT_EQ(SenpExtensionActivationState::Dormant, extensions.State(L"sample.extension"));
	ASSERT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
	ASSERT_TRUE(extensions.Poll(Clock::now()));
	ASSERT_EQ(SenpExtensionActivationState::Active, extensions.State(L"sample.extension"));
	EXPECT_EQ(0, invoked());
	click();
	EXPECT_EQ(1, invoked());
	// The title bar is part of the declaration; changing it is a structural edit.
	const auto revision = catalog.Snapshot().revision;
	snapshot.revision++; snapshot.extensions.front().runtime.viewTitle.clear();
	EXPECT_EQ(SenpWindowExtensionsStatus::Conflict, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_EQ(revision, catalog.Snapshot().revision); EXPECT_EQ(0, runtime->stops);
	EXPECT_EQ(SenpExtensionActivationState::Active, extensions.State(L"sample.extension"));
	EXPECT_TRUE(extensions.Close()); pages.Close(); ::DestroyWindow(host);
	EXPECT_EQ(1, invoked());
}

TEST_F(SenpOwnerPublicationTest, ViewTitleActionsTheNativeTitleBarCannotShowAreRefused)
{
	const auto run = [&](const auto& mutate) {
		layout::WorkbenchContributionRegistry catalog;
		CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
		EXPECT_TRUE(pages.Create(m_owner));
		auto target = std::make_shared<TargetState>();
		CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
			[&](const auto&, const auto&) { return std::make_unique<Target>(target); },
			[](std::string_view) { return true; },
			[](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}); });
		auto snapshot = PackagesWithRefresh();
		mutate(snapshot.extensions.front().runtime);
		const auto status = extensions.Synchronize(snapshot, 7, 9, Clock::now());
		EXPECT_FALSE(pages.Contains("sample.senp") && status != SenpWindowExtensionsStatus::Synchronized);
		EXPECT_TRUE(extensions.Close()); pages.Close();
		return status;
	};
	EXPECT_EQ(SenpWindowExtensionsStatus::Synchronized, run([](auto&) {}));
	// A manifest ThemeIcon may be upper case; no bundled codicon is.
	EXPECT_EQ(SenpWindowExtensionsStatus::Unsupported, run([](auto& runtime) { runtime.commands[1].icon = L"$(Refresh)"; }));
	EXPECT_EQ(SenpWindowExtensionsStatus::Invalid, run([](auto& runtime) { runtime.viewTitle[0].views = { L"sample.other" }; }));
	EXPECT_EQ(SenpWindowExtensionsStatus::Invalid, run([](auto& runtime) { runtime.viewTitle[0].command = L"sample.missing"; }));
	EXPECT_EQ(SenpWindowExtensionsStatus::Invalid, run([](auto& runtime) {
		runtime.viewTitle.push_back({ L"sample.refresh", { L"sample.projects" } });
	}));
	EXPECT_EQ(SenpWindowExtensionsStatus::Unsupported, run([](auto& runtime) {
		for (int i = 0; i != 8; ++i) {
			const auto id = L"sample.extra" + std::to_wstring(i);
			runtime.commands.push_back({ id, L"Extra", L"$(refresh)" });
			runtime.viewTitle.push_back({ id, { L"sample.projects" } });
		}
	}));
}

TEST(SenpWindowExtensionsGeneration, CatalogSuggestionIsNonReservingAndExhaustsAtProtocolLimit)
{
	layout::WorkbenchContributionRegistry catalog;
	EXPECT_EQ(1U, catalog.NextOwnerGeneration()); EXPECT_EQ(1U, catalog.NextOwnerGeneration());
	auto candidate = catalog.PrepareOwnerReplacement({ "last.extension", INT64_MAX }, 0, {}, {});
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Prepared, candidate.status);
	EXPECT_EQ(1U, catalog.NextOwnerGeneration());
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(candidate.change)));
	EXPECT_EQ(0U, catalog.NextOwnerGeneration());
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.DisposeOwner({ "last.extension", INT64_MAX }));
	EXPECT_EQ(0U, catalog.NextOwnerGeneration());
}

TEST_F(SenpOwnerPublicationTest, WindowPackageConflictsPreserveLiveRuntimeAndRollbackNewDeclarations)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	auto target = std::make_shared<TargetState>(); auto runtime = std::make_shared<RuntimeLifecycle>();
	CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
		[target](const auto&, const auto&) { return std::make_unique<Target>(target); },
		[](std::string_view) { return true; },
		[runtime](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, runtime); });
	auto snapshot = Packages();
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	ASSERT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
	ASSERT_TRUE(extensions.Poll(Clock::now()));
	const auto revision = catalog.Snapshot().revision;
	snapshot.revision++; snapshot.extensions.front().views.front().title = L"Changed";
	EXPECT_EQ(SenpWindowExtensionsStatus::Conflict, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_EQ(revision, catalog.Snapshot().revision); EXPECT_EQ(0, runtime->stops);
	EXPECT_EQ(SenpExtensionActivationState::Active, extensions.State(L"sample.extension"));
	snapshot.extensions.front().views.front().title = L"Projects";
	snapshot.extensions.front().views.front().provider = L"unknown.provider";
	EXPECT_EQ(SenpWindowExtensionsStatus::Unsupported, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	snapshot.extensions.front().views.front().provider = L"senp.tree";
	auto added = snapshot.extensions.front(); added.id = L"added.extension";
	added.viewContainers.front().id = L"added.container";
	added.views.front().id = L"added.view"; added.views.front().containerId = L"added.container";
	snapshot.extensions.push_back(added);
	// Ordered after the successful new cohort: page collision exercises rollback.
	auto rejected = added; rejected.id = L"zzz.extension";
	rejected.viewContainers.front().id = L"reserved.page";
	rejected.views.front().id = L"rejected.view"; rejected.views.front().containerId = L"reserved.page";
	snapshot.extensions.push_back(rejected);
	ASSERT_TRUE(pages.RegisterContributedPages({ { "reserved.page", { layout::EViewContainerLocation::Sidebar },
		[]() -> std::unique_ptr<viewcontainer::IViewContainerPage> { return {}; } } }).Succeeded());
	EXPECT_EQ(SenpWindowExtensionsStatus::Failed, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
	EXPECT_FALSE(pages.Contains("added.container")); EXPECT_TRUE(pages.Contains("reserved.page"));
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_EQ(1U, catalog.Snapshot().owners.size());
	EXPECT_EQ(0, runtime->stops); EXPECT_EQ(1, runtime->starts);
	ASSERT_TRUE(extensions.Poll(Clock::now()));
	EXPECT_EQ(SenpExtensionActivationState::Active, extensions.State(L"sample.extension"));
	EXPECT_TRUE(extensions.Close()); EXPECT_TRUE(pages.Contains("reserved.page")); pages.Close();
}

TEST_F(SenpOwnerPublicationTest, WindowCloseRetainsFailedRuntimeCleanupWithoutPollingRetries)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	auto target = std::make_shared<TargetState>(); auto runtime = std::make_shared<RuntimeLifecycle>();
	runtime->permitExit = false;
	CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
		[target](const auto&, const auto&) { return std::make_unique<Target>(target); },
		[](std::string_view) { return true; },
		[runtime](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, runtime); });
	ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(Packages(), 7, 9, Clock::now()));
	ASSERT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
	ASSERT_TRUE(extensions.Poll(Clock::now()));
	EXPECT_FALSE(extensions.Close()); EXPECT_EQ(1, runtime->joins); EXPECT_EQ(0, runtime->destroyed);
	EXPECT_FALSE(pages.Contains("sample.senp")); EXPECT_TRUE(catalog.Snapshot().owners.empty());
	for (int i = 0; i != 3; ++i) EXPECT_FALSE(extensions.Poll(Clock::now()));
	EXPECT_EQ(1, runtime->joins);
	EXPECT_EQ(SenpExtensionActivationState::Stopped, extensions.RequestView(L"sample.projects", true, Clock::now()));
	runtime->permitExit = true;
	EXPECT_TRUE(extensions.Close()); EXPECT_EQ(2, runtime->joins); EXPECT_EQ(1, runtime->destroyed);
	EXPECT_EQ(1, target->revoked); pages.Close();
}

TEST_F(SenpOwnerPublicationTest, WindowDeclarationConflictsCannotSuppressAuthorityRevocation)
{
	for (const bool accountChanged : { false, true }) {
		layout::WorkbenchContributionRegistry catalog;
		CDlgFuncList dialog; viewcontainer::CViewContainerPages pages(dialog);
		ASSERT_TRUE(pages.Create(m_owner));
		auto target = std::make_shared<TargetState>(); auto runtime = std::make_shared<RuntimeLifecycle>();
		CSenpWindowExtensions extensions(catalog, pages, m_owner, L"host.exe",
			[target](const auto&, const auto&) { return std::make_unique<Target>(target); },
			[](std::string_view) { return true; },
			[runtime](auto launch) { return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, runtime); });
		auto snapshot = Packages();
		ASSERT_EQ(SenpWindowExtensionsStatus::Synchronized, extensions.Synchronize(snapshot, 7, 9, Clock::now()));
		ASSERT_EQ(SenpExtensionActivationState::Preparing, extensions.RequestView(L"sample.projects", false, Clock::now()));
		ASSERT_TRUE(extensions.Poll(Clock::now()));
		++snapshot.revision;
		if (accountChanged) snapshot.extensions.front().views.front().title = L"Conflicting update";
		else {
			snapshot.extensions.front().enabled = false;
			auto unsupported = Packages().extensions.front(); unsupported.id = L"unsupported.extension";
			unsupported.views.front().provider = L"unknown.provider";
			snapshot.extensions.push_back(unsupported);
		}
		EXPECT_EQ(SenpWindowExtensionsStatus::Failed,
			extensions.Synchronize(snapshot, 7, accountChanged ? 10 : 9, Clock::now()));
		EXPECT_EQ(1, runtime->stops); EXPECT_EQ(1, runtime->destroyed); EXPECT_EQ(1, target->revoked);
		EXPECT_EQ(SenpExtensionActivationState::Stopped, extensions.State(L"sample.extension"));
		EXPECT_FALSE(pages.Contains("sample.senp")); EXPECT_TRUE(catalog.Snapshot().owners.empty());
		EXPECT_TRUE(extensions.Close()); pages.Close();
	}
}

TEST_F(SenpOwnerPublicationTest, NativeDeclarationsPublishBeforeRuntimeAndSurviveItsRevocation)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	int activations{};
	CSenpViewDeclarations declarations(catalog, pages, m_owner,
		[&](std::wstring_view, bool) { ++activations; return SenpExtensionActivationState::Preparing; },
		[](std::string_view) { return true; });
	const std::vector<layout::WorkbenchViewContainerDescriptor> containers{
		{ "sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
			{ layout::EViewContainerLocation::Sidebar } } };
	const std::vector<layout::WorkbenchViewDescriptor> views{
		{ "sample.projects", "sample.senp", "Projects", 10, true, true, "senp.tree" } };
	ASSERT_EQ(SenpViewDeclarationStatus::Registered, declarations.Register({ "sample.extension", 100 }, containers, views));
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_EQ(0, activations);
	const auto revision = catalog.Snapshot().revision;
	EXPECT_EQ(SenpViewDeclarationStatus::Unchanged, declarations.Register({ "sample.extension", 101 }, containers, views));
	EXPECT_EQ(revision, catalog.Snapshot().revision);
	senp::CSenpContributionOwners owners([](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{});
	});
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, SenpOwnerPublicationOptions(
				{ { views.front(), { "sample.open" } } }, std::make_unique<Target>(target),
				[&](const auto& owner, auto trees) { return declarations.Bind(owner, std::move(trees)); }));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	owners.Poll(Clock::now()); ASSERT_TRUE(hub.Pump(Clock::now()));
	ASSERT_TRUE(owners.IsCurrent(change.owner));
	ASSERT_TRUE(declarations.Pump(L"sample.extension", SenpExtensionActivationState::Active));
	EXPECT_EQ(revision, catalog.Snapshot().revision);
	EXPECT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Disabled));
	ASSERT_TRUE(declarations.Pump(L"sample.extension", SenpExtensionActivationState::Disabled));
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_EQ(revision, catalog.Snapshot().revision);
	EXPECT_EQ(100U, catalog.Snapshot().owners.front().generation);
	EXPECT_EQ(1, target->revoked);
	hub.Close();
	ASSERT_TRUE(declarations.Remove(L"sample.extension"));
	EXPECT_FALSE(pages.Contains("sample.senp")); EXPECT_TRUE(catalog.Snapshot().owners.empty());
	ASSERT_EQ(SenpViewDeclarationStatus::Registered, declarations.Register({ "sample.extension", 101 }, containers, views));
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_EQ(0, activations);
	declarations.Close();
	EXPECT_FALSE(pages.Contains("sample.senp")); EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_EQ(SenpViewDeclarationStatus::Stopped, declarations.Register({ "sample.extension", 102 }, containers, views));
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, WorkspaceSnapshotsSeedLateCommitsAndThenReachThemDirectly)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpViewDeclarations declarations(catalog, pages, m_owner,
		[](std::wstring_view, bool) { return SenpExtensionActivationState::Preparing; },
		[](std::string_view) { return true; });
	const std::vector<layout::WorkbenchViewContainerDescriptor> containers{
		{ "sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
			{ layout::EViewContainerLocation::Sidebar } } };
	const std::vector<layout::WorkbenchViewDescriptor> views{
		{ "sample.projects", "sample.senp", "Projects", 10, true, true, "senp.tree" } };
	ASSERT_EQ(SenpViewDeclarationStatus::Registered,
		declarations.Register({ "sample.extension", 100 }, containers, views));
	auto lifecycle = std::make_shared<RuntimeLifecycle>();
	lifecycle->admitEvents = true;
	senp::CSenpContributionOwners owners([lifecycle](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{}, lifecycle);
	});
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	// Refused where it is published rather than where it would be admitted: a
	// root id with a space is not one the wire's charset accepts.
	EXPECT_FALSE(hub.PublishWorkspace({ { { L"root 0", L"main", {} } } }));
	// Published while no owner exists at all, which is the ordering the window
	// produces: the Source Control worker has a branch long before a package is
	// activated. Without the hub retaining it, this package would start with no
	// repositories and stay that way until the workspace next changed.
	EXPECT_TRUE(hub.PublishWorkspace({ { { L"root:0", L"main", {} } } }));
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, SenpOwnerPublicationOptions(
				{ { views.front(), { "sample.open" } } }, std::make_unique<Target>(target),
				[&](const auto& owner, auto trees) { return declarations.Bind(owner, std::move(trees)); }));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	owners.Poll(Clock::now());
	ASSERT_TRUE(hub.Pump(Clock::now()));
	ASSERT_EQ(1U, lifecycle->events.size());
	const auto* seeded = std::get_if<senp::effect::WorkspaceChanged>(&lifecycle->events[0]);
	ASSERT_NE(nullptr, seeded);
	ASSERT_EQ(1U, seeded->repositories.size());
	EXPECT_EQ(L"main", seeded->repositories[0].branch);

	// Once committed, the publication is an audience in its own right.
	EXPECT_TRUE(hub.PublishWorkspace({ { { L"root:0", L"feature", {} } } }));
	owners.Poll(Clock::now());
	ASSERT_TRUE(hub.Pump(Clock::now()));
	ASSERT_EQ(2U, lifecycle->events.size());
	const auto* replaced = std::get_if<senp::effect::WorkspaceChanged>(&lifecycle->events[1]);
	ASSERT_NE(nullptr, replaced);
	ASSERT_EQ(1U, replaced->repositories.size());
	EXPECT_EQ(L"feature", replaced->repositories[0].branch);

	hub.Close();
	declarations.Close();
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, NativeDeclarationConflictsPreserveCatalogAndOtherPages)
{
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpViewDeclarations declarations(catalog, pages, m_owner,
		[](std::wstring_view, bool) { return SenpExtensionActivationState::Preparing; },
		[](std::string_view) { return true; });
	std::vector<layout::WorkbenchViewContainerDescriptor> containers{
		{ "sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
			{ layout::EViewContainerLocation::Sidebar } } };
	std::vector<layout::WorkbenchViewDescriptor> views{
		{ "sample.projects", "sample.senp", "Projects", 10, true, true, "senp.tree" } };
	ASSERT_EQ(SenpViewDeclarationStatus::Registered, declarations.Register({ "sample.extension", 1 }, containers, views));
	const auto revision = catalog.Snapshot().revision;
	views.front().title = "Changed declaration";
	EXPECT_EQ(SenpViewDeclarationStatus::Conflict, declarations.Register({ "sample.extension", 2 }, containers, views));
	EXPECT_EQ(SenpViewDeclarationStatus::Failed, declarations.Register({ "foreign.extension", 2 }, containers, views));
	ASSERT_TRUE(pages.RegisterContributedPages({ { "reserved.page", { layout::EViewContainerLocation::Sidebar },
		[]() -> std::unique_ptr<viewcontainer::IViewContainerPage> { return {}; } } }).Succeeded());
	containers.front().id = "reserved.page"; views.front().id = "reserved.view"; views.front().containerId = "reserved.page";
	EXPECT_EQ(SenpViewDeclarationStatus::Failed, declarations.Register({ "foreign.extension", 2 }, containers, views));
	EXPECT_EQ(revision, catalog.Snapshot().revision); EXPECT_EQ(1U, catalog.Snapshot().owners.size());
	EXPECT_TRUE(pages.Contains("sample.senp")); EXPECT_TRUE(pages.Contains("reserved.page"));
	declarations.Close();
	EXPECT_FALSE(pages.Contains("sample.senp")); EXPECT_TRUE(pages.Contains("reserved.page"));
	EXPECT_TRUE(catalog.Snapshot().owners.empty()); pages.Close();
}

TEST_F(SenpOwnerPublicationTest, AtomicallyPublishesCatalogPagesAndQueuedActivation)
{
	std::vector<senp::effect::Effect> activation{ senp::effect::InvalidateTree{ L"sample.projects" } };
	senp::CSenpContributionOwners owners([&](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::move(activation));
	});
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	EXPECT_FALSE(pages.Contains("sample.senp"));

	owners.Poll(Clock::now());
	ASSERT_TRUE(hub.Pump(Clock::now()));
	ASSERT_TRUE(pages.Contains("sample.senp"));
	const std::vector<std::string> invalidRemoval{ "sample.senp", "workbench.view.explorer" };
	EXPECT_FALSE(pages.RemoveContributedPages(invalidRemoval));
	EXPECT_TRUE(pages.Contains("sample.senp"));
	const auto snapshot = catalog.Snapshot();
	ASSERT_EQ(1U, snapshot.owners.size());
	EXPECT_EQ("sample.extension", snapshot.owners[0].ownerId);
	EXPECT_EQ(static_cast<std::uint64_t>(change.owner.generation), snapshot.owners[0].generation);
	const auto view = std::ranges::find_if(snapshot.views,
		[](const auto& item) { return item.descriptor.id == "sample.projects"; });
	ASSERT_NE(snapshot.views.end(), view);
	EXPECT_EQ("senp.tree", view->descriptor.provider);
	auto terminal = owners.TakeTransition();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, terminal->status);

	ASSERT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Updated));
	owners.Poll(Clock::now());
	EXPECT_EQ(1, target->revoked);
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_FALSE(pages.Contains("sample.senp"));
	auto reenabled = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, reenabled.status);
	owners.Poll(Clock::now());
	auto activatedAgain = owners.TakeTransition();
	ASSERT_TRUE(activatedAgain);
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, activatedAgain->status);
	EXPECT_TRUE(pages.Contains("sample.senp"));
	EXPECT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Shutdown));
	EXPECT_FALSE(pages.Contains("sample.senp"));
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, RevisionConflictPublishesNeitherCandidate)
{
	std::vector<senp::effect::Effect> activation;
	senp::CSenpContributionOwners owners([&](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::move(activation));
	});
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	auto other = catalog.PrepareOwnerReplacement({ "other.extension", 1 }, 0, {}, {});
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Prepared, other.status);
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(other.change)));

	owners.Poll(Clock::now());
	EXPECT_FALSE(pages.Contains("sample.senp"));
	EXPECT_TRUE(std::ranges::none_of(catalog.Snapshot().owners,
		[](const auto& owner) { return owner.ownerId == "sample.extension"; }));
	auto terminal = owners.TakeTransition();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(senp::OwnerChangeStatus::Failed, terminal->status);
	EXPECT_EQ(1, target->revoked);
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, DeclaredBindingsRetainCatalogAcrossReplacementFailureAndRevocation)
{
	senp::CSenpContributionOwners owners([](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{});
	});
	layout::WorkbenchContributionRegistry catalog;
	const layout::WorkbenchContributionOwner declarationOwner{ "sample.extension", 1 };
	const layout::WorkbenchViewContainerDescriptor container{
		"sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
		{ layout::EViewContainerLocation::Sidebar } };
	const layout::WorkbenchViewDescriptor view{
		"sample.projects", container.id, "Projects", 10, true, true, "senp.tree" };
	auto declaration = catalog.PrepareOwnerReplacement(declarationOwner, 0,
		std::span(&container, 1), std::span(&view, 1));
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Prepared, declaration.status);
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(declaration.change)));
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto state = std::make_shared<DeclaredState>();
	auto target = std::make_shared<TargetState>();
	auto prepare = [&](wchar_t digest) {
		return owners.Prepare(Launch(), std::wstring(64, digest),
			[&](const auto& candidate, const auto* previous) {
				return hub.Prepare(candidate, previous, SenpOwnerPublicationOptions{
					{ { view, { "sample.open" } } }, std::make_unique<Target>(target),
					[&](const auto& owner, auto bindings) {
						return std::make_unique<DeclaredPublication>(state, owners, owner, std::move(bindings));
					} });
			}, Clock::now());
	};
	auto initial = prepare(L'b');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, initial.status);
	EXPECT_FALSE(state->owner);
	owners.Poll(Clock::now());
	ASSERT_TRUE(owners.IsCurrent(initial.owner));
	EXPECT_EQ(0, state->pumped);
	ASSERT_TRUE(hub.Pump(Clock::now()));
	EXPECT_EQ(1, state->pumped);
	ASSERT_EQ(1U, state->bindings.size());
	EXPECT_EQ(L"sample.projects", state->bindings.front().ViewId());
	auto originalProvider = state->bindings.front().Provider();
	ASSERT_TRUE(owners.TakeTransition());

	auto rejected = prepare(L'c');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, rejected.status);
	state->permitCommit = false;
	owners.Poll(Clock::now());
	auto failed = owners.TakeTransition();
	ASSERT_TRUE(failed);
	EXPECT_EQ(senp::OwnerChangeStatus::Failed, failed->status);
	EXPECT_TRUE(owners.IsCurrent(initial.owner));
	EXPECT_EQ(originalProvider, state->bindings.front().Provider());
	EXPECT_EQ(0, state->cleared);
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));

	state->permitCommit = true;
	auto replacement = prepare(L'd');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, replacement.status);
	owners.Poll(Clock::now());
	ASSERT_TRUE(owners.IsCurrent(replacement.owner));
	ASSERT_TRUE(state->owner);
	EXPECT_EQ(replacement.owner, *state->owner);
	EXPECT_NE(originalProvider, state->bindings.front().Provider());
	EXPECT_EQ(0, state->cleared);
	EXPECT_EQ(2, target->revoked);
	ASSERT_TRUE(hub.Pump(Clock::now()));
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));
	ASSERT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Disabled));
	EXPECT_FALSE(state->owner);
	EXPECT_TRUE(state->bindings.empty());
	EXPECT_EQ(1, state->cleared);
	EXPECT_EQ(3, target->revoked);
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));
	EXPECT_EQ(1, std::ranges::count_if(catalog.Snapshot().views,
		[](const auto& item) { return item.descriptor.id == "sample.projects"; }));
	EXPECT_TRUE(owners.Close());
	hub.Close();
	EXPECT_EQ(1, state->cleared);
}

} // namespace
} // namespace workbench
