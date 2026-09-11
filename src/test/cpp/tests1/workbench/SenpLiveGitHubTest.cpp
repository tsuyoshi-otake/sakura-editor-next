/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "platform/controlipc/ControlPlatformClient.h"
#include "platform/controlipc/ControlSenpComposition.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include "platform/storage/CInMemoryStorageService.h"
#include "workbench/SenpOwnerComposition.h"
#include "workbench/editor/SenpControlToolReads.h"
#include "workbench/editor/SenpOwnerTextResources.h"
#include "workbench/editor/SenpReadonlyOwnerTarget.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/uri/UriIdentity.h>

#include <CommCtrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*!
	@file
	@brief End to end verification against the real GitHub API.

	Every other SENP test answers a tool read out of canned data. That leaves the
	one boundary this suite exists for untested: what the GitHub CLI actually
	returns, carried by the real control broker, parsed by the real componentized
	extension, and shown on a real editor surface. Two mismatches - the resource
	vocabulary and the completion envelope - lived through green tests on both
	sides of that boundary precisely because no test ever crossed it.

	It is gated on SAKURA_SENP_LIVE_GITHUB, whose value is the absolute path of a
	local git checkout whose remote is a GitHub repository. Without it these tests
	skip. With it, nothing else may skip: a missing runtime fixture, an
	unauthenticated GitHub CLI, a workspace that resolves to no repository or a
	repository with nothing to show is a failure, because a live suite that
	quietly passes on an unprepared machine is exactly the verification this work
	was told not to produce.
*/

namespace workbench::editor {
namespace {

namespace ipc = ::platform::controlipc;
using Clock = std::chrono::steady_clock;

//! The canonical control authority the storage Hello pins, and the profile hash
//! the endpoint publishes. Neither names a GitHub identity: the account, the
//! repository and every id below are read from the machine, never written here.
constexpr char kAuthorityId[] = "0123456789abcdef0123456789abcdef";
constexpr wchar_t kProfileHash[] =
	L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr wchar_t kProfile[] = L"github-live";
constexpr wchar_t kControlRoot[] = L"C:\\Control\\SenpLiveGitHub";
constexpr std::uint64_t kEndpointGeneration = 7;
constexpr ipc::ControlIpcSessionContext kConnection{ 31, 1701 };

//! Real work runs here: a GitHub CLI process per read, a Wasm host process for
//! the extension and a network round trip inside each. The budget is generous
//! because a slow answer is not a wrong one; nothing in these tests waits on a
//! wall clock for its result.
constexpr auto kLiveDeadline = std::chrono::seconds(90);

//! The componentized extension a test drives, with the digest the control side
//! will publish for it. The digest is the real one, so the real permission gate
//! decides whether this package may hold the GitHub capability.
struct LiveExtension final {
	std::wstring extensionId;
	std::filesystem::path host;
	std::filesystem::path component;
	std::wstring digest;
};

//! Node ids and resource ids are ASCII wherever this suite prints one, and the
//! only thing that reads them is a failure message.
std::string Narrow(std::wstring_view text)
{
	std::string narrow;
	narrow.reserve(text.size());
	for (const auto character : text) {
		narrow.push_back(character < 0x80 ? static_cast<char>(character) : '?');
	}
	return narrow;
}

//! Empty means the gate is closed and the suite skips.
std::filesystem::path LiveCheckout()
{
	const auto value = _wgetenv(L"SAKURA_SENP_LIVE_GITHUB");
	if (!value || !*value) return {};
	return std::filesystem::path(value);
}

void LoadExtension(const wchar_t* stem, std::wstring extensionId, LiveExtension& loaded)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	// Not a skip: the gate is already open, so an absent fixture set means the
	// machine was told to run live and cannot.
	ASSERT_TRUE(fixtureEnvironment && *fixtureEnvironment)
		<< "SAKURA_SENP_LIVE_GITHUB is set but SAKURA_SENP_RUNTIME_FIXTURES is not";
	const std::filesystem::path fixtures(fixtureEnvironment);
	loaded.extensionId = std::move(extensionId);
	loaded.host = fixtures / L"sakura-senp-host.exe";
	loaded.component = fixtures / (std::wstring(stem) + L".wasm");
	std::ifstream digestFile(fixtures / (std::wstring(stem) + L".sha256"));
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(loaded.host)) << loaded.host.string();
	ASSERT_TRUE(std::filesystem::is_regular_file(loaded.component)) << loaded.component.string();
	ASSERT_EQ(64U, digest.size()) << "componentized module digest is missing";
	loaded.digest.assign(digest.begin(), digest.end());
}

/*!
	@brief The one package the control side is told this profile has installed.

	Discovering packages means running the external package tool over a real
	profile home, which is the one step a live test stands in for. Everything the
	permission gate reads is real: the extension id the owner activates under and
	the digest of the module that will actually be loaded. A package whose digest
	did not match the module would be refused here exactly as in production.
*/
class LivePackages final : public ipc::IControlSenpPackageSource {
public:
	LivePackages(std::wstring extensionId, std::wstring digest)
	{
		senp::ExtensionDescriptor extension;
		extension.id = std::move(extensionId);
		extension.archiveSha256 = std::move(digest);
		extension.installed = true;
		extension.enabled = true;
		extension.runtime.schemaVersion = 2;
		extension.runtime.abi = L"sakura:senp/extension@3.0.0";
		extension.runtime.capabilities = { L"workbench.views.tree", L"tools.github.repository.read" };
		m_snapshot.state = senp::EManagementState::Ready;
		m_snapshot.revision = 1;
		m_snapshot.extensions = { std::move(extension) };
	}
	std::optional<senp::ManagementSnapshot> Refresh(const std::wstring&) override
	{
		if (m_closed) return std::nullopt;
		return m_snapshot;
	}
	void Close() noexcept override { m_closed = true; }
private:
	senp::ManagementSnapshot m_snapshot;
	std::atomic<bool> m_closed{ false };
};

/*!
	@brief The editor side of the wire, in process.

	It answers the storage Hello itself and hands every SENP frame to the control
	composition's own session handler, so the frames, the codec, the grant
	registry, the broker and the executor are all the production ones. Only the
	named pipe is absent.
*/
class LiveChannel final : public ipc::IControlPlatformClientChannel {
public:
	explicit LiveChannel(const std::shared_ptr<ipc::IControlIpcFrameHandler>& handler)
		: m_session(handler ? handler->CreateSession(kConnection) : nullptr) {}

	ipc::ControlIpcTransportResult Connect(const ipc::ControlPlatformEndpointSnapshot&,
		std::chrono::milliseconds) override
	{
		return Ok();
	}

	ipc::ControlIpcTransportResult Exchange(const ipc::ControlIpcFrame& request,
		std::vector<ipc::ControlIpcFrame>& responses, std::chrono::milliseconds) override
	{
		if (!m_session) return Lost();
		if (request.header.kind == ipc::EControlIpcKind::Hello) {
			auto hello = ipc::EncodeControlStorageHello(kAuthorityId);
			if (!hello) return Lost();
			responses.push_back({ { ipc::kControlIpcMajorVersion, ipc::kControlIpcMinorVersion,
				ipc::EControlIpcKind::HelloAck,
				ipc::EControlIpcFlags::Response | ipc::EControlIpcFlags::Terminal,
				request.header.requestId, kEndpointGeneration }, std::move(*hello) });
			return Ok();
		}
		auto dispatched = m_session->HandleFrame(kConnection, request);
		responses = std::move(dispatched.responseFrames);
		return Ok();
	}

	void Close() noexcept override { m_session.reset(); }

private:
	static ipc::ControlIpcTransportResult Ok() noexcept
	{
		return { true, ipc::EControlIpcTransportDisconnectReason::None, 0, L"" };
	}
	static ipc::ControlIpcTransportResult Lost() noexcept
	{
		return { false, ipc::EControlIpcTransportDisconnectReason::IoError, 0, L"live channel lost" };
	}

	std::unique_ptr<ipc::IControlIpcSessionHandler> m_session;
};

class LiveEndpointReader final : public ipc::IControlPlatformEndpointReader {
public:
	std::optional<ipc::ControlPlatformEndpointSnapshot> Read(
		const ipc::ControlPlatformEndpointReadRequirements&) override
	{
		return ipc::ControlPlatformEndpointSnapshot{ ::GetCurrentProcessId(), kEndpointGeneration,
			ipc::ControlPlatformEndpointLifecycle::Accepting, kProfileHash,
			ipc::BuildControlPipeName(kProfileHash), kAuthorityId };
	}
};

//! The child window a published tree view is parked in. A tree is projected
//! whether or not anything draws it, so this only has to be a real window.
class LiveBody final : public viewcontainer::ISenpViewBody {
public:
	explicit LiveBody(viewcontainer::SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
			m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	~LiveBody() override { Close(); }
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

//! The tail of a colon separated node id, which is how a document resource is
//! named from the node that opens it. Asserting on it is a cross check rather
//! than a convenience: it fails unless the tree and the document agree.
std::wstring Suffix(const std::wstring& id, const std::size_t segments)
{
	auto cut = id.size();
	for (std::size_t index = 0; index < segments; ++index) {
		const auto separator = id.rfind(L':', cut - 1);
		if (separator == std::wstring::npos) return {};
		cut = separator;
	}
	return id.substr(cut + 1);
}

class SenpLiveGitHub : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }

	void SetUp() override
	{
		m_apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		ASSERT_TRUE(SUCCEEDED(m_apartment) || m_apartment == RPC_E_CHANGED_MODE);
		INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
		ASSERT_TRUE(::InitCommonControlsEx(&controls));
		const EditorDocumentIdentity identity{ .opaqueId = "senp-live-github-legacy" };
		ASSERT_EQ(EEditorOperationStatus::Succeeded, m_core.OpenResolvedInput({
			.operation = { "senp-live-github.fixture.open" }, .input = { "legacy", identity },
			.resolvedDocument = ResolvedEditorDocument{ identity, 7, true },
		}).status);
		m_shell = ::CreateWindowExW(0, L"STATIC", L"SENP live GitHub",
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 80, 80, 720, 520,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_shell);
		m_legacy = ::CreateWindowExW(0, L"EDIT", L"legacy", WS_CHILD | WS_TABSTOP | ES_MULTILINE,
			0, 0, 0, 0, m_shell, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_legacy);
		::ShowWindow(m_shell, SW_SHOWNOACTIVATE);
		m_controller = std::make_unique<SenpReadonlyEditorController>(
			m_core, m_shell, m_legacy, m_legacy, "legacy");
		ASSERT_EQ(SenpSurfaceProjection::Applied, m_controller->Layout({ 4, 8, 660, 460 }));
		m_parking = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_parking);
	}

	void TearDown() override
	{
		m_pump = {};
		if (m_reads) { m_reads->Stop(); m_reads.reset(); }
		if (m_control) { m_control->Close(); m_control.reset(); }
		m_registry.reset();
		m_resources.reset();
		if (m_controller) { (void)m_controller->Shutdown(); m_controller.reset(); }
		if (m_parking) ::DestroyWindow(m_parking);
		if (m_shell) { ::DestroyWindow(m_shell); EXPECT_FALSE(::IsWindow(m_shell)); }
		if (SUCCEEDED(m_apartment)) ::CoUninitialize();
	}

	/*!
		@brief Brings up the whole control side and proves the machine is prepared.

		Everything the control process composes is the production object: the
		GitHub CLI platform, the connection lifecycle, the local repository
		inspection, the package authority, the grant registry, the broker and the
		tool executor. The repository is never named here - it is derived from the
		remotes of the checkout the window declares - and neither is the account.
		Each of the three is asserted before any extension is activated, so an
		unprepared machine fails at the step that is missing rather than at some
		later read that could be blamed on the extension.
	*/
	void StartControl(const LiveExtension& extension, const std::filesystem::path& checkout)
	{
		auto storage = std::make_shared<::platform::storage::CInMemoryStorageService>();
		m_registry = std::make_shared<::platform::profiles::ControlUserDataProfileRegistry>(storage);
		ASSERT_TRUE(m_registry->Start().Succeeded());
		ASSERT_TRUE(m_registry->CreateNamed(
			{ kProfile, L"GitHub live", ::platform::profiles::UserDataProfileKind::Normal, {}, {} },
			{ "create-live-profile", std::nullopt }).Succeeded());

		ipc::ControlSenpCompositionOptions options;
		options.controlProfileRoot = kControlRoot;
		options.controlAuthorityId = kAuthorityId;
		options.controlAuthorityGeneration = kEndpointGeneration;
		// Left empty on purpose: production resolves GH_CONFIG_DIR, and failing
		// to find the real one is how an unauthenticated machine is caught.
		options.ghConfigurationDirectory.clear();
		options.workingDirectory = checkout.native();
		options.hostname = L"github.com";
		ipc::ControlSenpCompositionDependencies dependencies;
		dependencies.packages = std::make_shared<LivePackages>(extension.extensionId, extension.digest);
		m_control = std::make_unique<ipc::CControlSenpComposition>(
			std::move(options), m_registry, std::move(dependencies));

		SenpControlToolReadsOptions readOptions;
		readOptions.authorityProfileId = kAuthorityId;
		readOptions.authorityProfileHash = kProfileHash;
		readOptions.senpProfileId = kProfile;
		readOptions.pollInterval = std::chrono::milliseconds(5);
		readOptions.exchangeDeadline = std::chrono::seconds(30);
		readOptions.accountRefreshInterval = std::chrono::milliseconds(200);
		auto handler = m_control->Handler();
		ASSERT_TRUE(handler);
		readOptions.channelFactory = [handler] { return std::make_unique<LiveChannel>(handler); };
		m_reads = std::make_unique<CSenpControlToolReads>(std::move(readOptions), m_endpoints);

		const auto folder = ::platform::uri::Uri::FromWindowsPath(checkout.native());
		ASSERT_TRUE(folder) << "live checkout is not a path a workspace folder can name";
		m_reads->DeclareWorkspace(1, 1, { folder.value->ToString() });

		ASSERT_TRUE(m_control->RequestRefresh(kProfile));
		ASSERT_TRUE(WaitFor([this] {
			return m_control->PublishedRevision(kProfile).has_value()
				&& m_control->PublishedRepository(kProfile).has_value()
				&& m_control->ConnectionState(kProfile) == senp::github::GhConnectionState::Connected;
		})) << "control state: packages="
			<< (m_control->PublishedRevision(kProfile).has_value() ? 1 : 0)
			<< " repository=" << (m_control->PublishedRepository(kProfile).has_value() ? 1 : 0)
			<< " connection=" << static_cast<int>(m_control->ConnectionState(kProfile));
		const auto repository = m_control->PublishedRepository(kProfile);
		ASSERT_TRUE(repository);
		EXPECT_FALSE(repository->Owner().empty());
		EXPECT_FALSE(repository->Repository().empty());

		ASSERT_TRUE(WaitFor([this] {
			// CEditWnd asks on every synchronization and every timer turn; no
			// owner exists yet to run that cadence, so the question is asked here.
			m_reads->RefreshAccount();
			return m_reads->Account().state == SenpToolAccountState::Connected
				&& m_reads->Account().generation > 0;
		})) << "the editor seam never saw a connected GitHub account: state="
			<< static_cast<int>(m_reads->Account().state)
			<< " generation=" << m_reads->Account().generation;
		m_accountGeneration = m_reads->Account().generation;

		m_resources = std::make_unique<CSenpOwnerTextResources>(kProfile);
		ASSERT_TRUE(m_resources->Usable());
	}

	//! Waits on control-side state, which settles on its own worker. The seam
	//! itself is never driven from here; only the owner composition is.
	template<class Predicate>
	bool WaitFor(Predicate predicate) const
	{
		const auto deadline = Clock::now() + kLiveDeadline;
		while (Clock::now() < deadline) {
			if (predicate()) return true;
			::Sleep(10);
		}
		return predicate();
	}

	//! Drains the queue the window's own loop drains. A document surface renders
	//! off a posted completion, so a turn that never dispatches a message leaves
	//! every surface blank no matter how long it is waited on.
	static void Dispatch()
	{
		MSG message{};
		while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
	}

	/*!
		@brief One turn of the window: the owner composition, the text pump, the
		messages both of them post.

		This is the cadence CEditWnd runs. Polling the composition moves the
		extension forward and drains tool terminals; pumping the target moves at
		most one chunk of one text resource toward a surface; dispatching lets the
		surfaces that were handed content actually render it.
	*/
	template<class Predicate>
	bool Await(CSenpOwnerComposition& composition, Predicate predicate) const
	{
		const auto deadline = Clock::now() + kLiveDeadline;
		bool wasLive = false;
		while (Clock::now() < deadline) {
			if (!composition.Poll(Clock::now())) return false;
			// An owner that retires mid-wait destroys its publication, and with
			// it the target, the surfaces and the text pump the caller holds.
			// The wait ends the moment that happens, so a retirement fails the
			// expectation that was waiting on it instead of being read back out
			// of freed memory on the next turn.
			const auto owners = composition.Snapshot();
			wasLive = wasLive || owners.active > 0 || owners.preparing > 0;
			if (wasLive && owners.active == 0 && owners.preparing == 0) return false;
			if (m_pump) m_pump();
			Dispatch();
			if (predicate()) return true;
			(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		}
		Dispatch();
		return predicate();
	}

	//! The publication a live owner is given: the production readonly target,
	//! wired to the production seam, so a document reaches a real surface.
	SenpOwnerPublicationFactory Publication(
		std::vector<layout::WorkbenchViewContainerDescriptor> containers,
		std::vector<SenpOwnerTreeContribution> trees,
		std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>>& providers,
		CSenpReadonlyOwnerTarget*& target)
	{
		return [this, containers = std::move(containers), trees = std::move(trees),
			&providers, &target](const senp::ContributionOwnerIdentity& owner)
			-> std::optional<SenpOwnerPublicationOptions> {
			if (!m_resources->Admit(owner)) return std::nullopt;
			// Both sinks are the ones CEditWnd installs, and an owner that has
			// neither is not the owner production runs: a refused effect fails the
			// coordinator for good, so an extension that merely finishes a command
			// would take its whole composition down before any document arrived.
			auto held = std::make_unique<CSenpReadonlyOwnerTarget>(owner, *m_controller, m_shell,
				980000, m_resources.get(), SenpTextResourceView::CopySink{},
				[this, owner](const senp::effect::OperationContext&,
					const senp::effect::CompleteCommand& completion) {
					m_completions.push_back({ completion.status, completion.message });
					return true;
				},
				[this, owner](const std::wstring_view handle) {
					m_reads->ReleaseResource(owner, handle);
					return true;
				},
				m_reads.get());
			target = held.get();
			m_pump = held->TextPump();
			return SenpOwnerPublicationOptions(m_parking, containers, trees, std::move(held),
				[](std::string_view) { return true; },
				[&providers](viewcontainer::SenpViewBodyHost host,
					std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
					providers.emplace(std::wstring(provider->ViewId()), provider);
					auto body = std::make_unique<LiveBody>(std::move(host));
					return body->Window()
						? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
				});
		};
	}

	senp::EffectRuntimeLaunch Launch(const LiveExtension& extension) const
	{
		return { .hostExecutable = extension.host.native(),
			.modulePath = extension.component.native(),
			.moduleSha256 = extension.digest,
			.extensionId = extension.extensionId,
			.context = { .workspaceRevision = 1, .accountGeneration = m_accountGeneration } };
	}

	//! Asserts that a document really reached a surface, and that the surface it
	//! reached is the one the tree node named.
	void ExpectDisplayed(CSenpOwnerComposition& composition, CSenpReadonlyOwnerTarget& target,
		const std::wstring& resourceId)
	{
		auto* const host = target.Host(resourceId);
		ASSERT_NE(nullptr, host) << "no surface holds " << Narrow(resourceId);
		ASSERT_TRUE(Await(composition, [host] { return host->State() == SenpDocumentHostState::Ready; }))
			<< "surface state " << static_cast<int>(host->State());
		ASSERT_TRUE(Await(composition, [host] {
			host->SelectAll();
			return !host->SelectedText().empty();
		})) << "the surface for a live document is empty";
	}

	HRESULT m_apartment{ E_FAIL };
	EditorCoreService m_core;
	HWND m_shell{}, m_legacy{}, m_parking{};
	std::unique_ptr<SenpReadonlyEditorController> m_controller;
	LiveEndpointReader m_endpoints;
	std::shared_ptr<::platform::profiles::ControlUserDataProfileRegistry> m_registry;
	std::unique_ptr<ipc::CControlSenpComposition> m_control;
	std::unique_ptr<CSenpControlToolReads> m_reads;
	std::unique_ptr<CSenpOwnerTextResources> m_resources;
	std::int64_t m_accountGeneration{};
	mutable SenpReadonlyOwnerTextPump m_pump;
	//! What the extension said about each command it was asked to run. The
	//! window puts these on the status line; the test only has to remember them.
	mutable std::vector<std::pair<senp::effect::CompletionStatus, std::wstring>> m_completions;
};

TEST_F(SenpLiveGitHub, RealIssuesAndPullRequestsReachTheTreeAndAnEditorSurface)
{
	const auto checkout = LiveCheckout();
	if (checkout.empty()) GTEST_SKIP() << "SAKURA_SENP_LIVE_GITHUB is not configured";
	LiveExtension extension;
	ASSERT_NO_FATAL_FAILURE(LoadExtension(L"github-pull-requests-extension",
		L"sakura-github-pull-requests", extension));
	ASSERT_NO_FATAL_FAILURE(StartControl(extension, checkout));

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_parking));
	CSenpOwnerComposition composition(catalog, pages);
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	CSenpReadonlyOwnerTarget* target = nullptr;
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"pr:github", "github-pull-requests", "Pull Requests", 10, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openPullRequest" });
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"issues:github", "github-pull-requests", "Issues", 20, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openIssue", "github.openIssueComment" });
	auto publication = Publication({ layout::WorkbenchViewContainerDescriptor{
		"github-pull-requests", "GitHub", layout::EViewContainerLocation::Sidebar, 6,
		"$(github)", false, { layout::EViewContainerLocation::Sidebar } } },
		std::move(trees), providers, target);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, composition.Activate(Launch(extension),
		extension.digest, std::move(publication), Clock::now()).status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());
	ASSERT_NE(nullptr, target);

	// Issues. The page is whatever the repository actually has open, so the test
	// reads the ids back out of the tree rather than naming any of them.
	const auto issues = providers.at(L"issues:github");
	issues->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] { return issues->Model().ItemCount() >= 1; }))
		<< "no open issue reached the tree from the live repository";
	const auto issueRoot = issues->Model().Node(L"");
	ASSERT_TRUE(issueRoot);
	ASSERT_FALSE(issueRoot->children.empty());
	const auto issueId = issueRoot->children.front();
	EXPECT_TRUE(issueId.starts_with(L"issue:")) << Narrow(issueId);
	const auto issue = issues->Model().Node(issueId);
	ASSERT_TRUE(issue);
	EXPECT_FALSE(issue->item.label.empty());
	const auto issueNumber = Suffix(issueId, 1);
	ASSERT_FALSE(issueNumber.empty());

	ASSERT_TRUE(issues->Select(issueId));
	ASSERT_TRUE(issues->Execute(issueId));
	ASSERT_TRUE(Await(composition, [&] { return target->DocumentCount() == 1; }))
		<< "the issue detail read never produced a document";
	ASSERT_NO_FATAL_FAILURE(ExpectDisplayed(composition, *target, L"github-issue:" + issueNumber));

	// Pull requests travel a different list and a different detail shape, and
	// they are the half of this extension the issues path never exercises.
	const auto pulls = providers.at(L"pr:github");
	pulls->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] { return pulls->Model().ItemCount() >= 1; }))
		<< "no open pull request reached the tree from the live repository";
	const auto pullRoot = pulls->Model().Node(L"");
	ASSERT_TRUE(pullRoot);
	ASSERT_FALSE(pullRoot->children.empty());
	const auto pullId = pullRoot->children.front();
	EXPECT_TRUE(pullId.starts_with(L"pull:")) << Narrow(pullId);
	const auto pullNumber = Suffix(pullId, 1);
	ASSERT_FALSE(pullNumber.empty());
	ASSERT_TRUE(pulls->Select(pullId));
	ASSERT_TRUE(pulls->Execute(pullId));
	ASSERT_TRUE(Await(composition, [&] { return target->DocumentCount() == 2; }))
		<< "the pull request detail read never produced a document";
	ASSERT_NO_FATAL_FAILURE(ExpectDisplayed(composition, *target,
		L"github-pull-request:" + pullNumber));

	m_pump = {};
	EXPECT_TRUE(composition.Close());
	pages.Close();
}

TEST_F(SenpLiveGitHub, RealWorkflowRunsAndOneRealJobLogReachTheTreeAndAnEditorSurface)
{
	const auto checkout = LiveCheckout();
	if (checkout.empty()) GTEST_SKIP() << "SAKURA_SENP_LIVE_GITHUB is not configured";
	LiveExtension extension;
	ASSERT_NO_FATAL_FAILURE(LoadExtension(L"github-actions-extension",
		L"sakura-github-actions", extension));
	ASSERT_NO_FATAL_FAILURE(StartControl(extension, checkout));

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_parking));
	CSenpOwnerComposition composition(catalog, pages);
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	CSenpReadonlyOwnerTarget* target = nullptr;
	const std::vector<std::string> commands{ "github-actions.workflow.run.open",
		"sakura.githubActions.openJobDetails", "github-actions.workflow.logs" };
	const std::vector<tree::SenpTreeItemAction> itemActions{ { L"github-actions.workflow.logs",
		L"View job logs", L"resources/icons/light/logs.svg", { L"job", L"completed" }, {} } };
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"github-actions.workflows", "github-actions", "Workflows", 10, true, true, "senp.tree" },
		commands, itemActions);
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"github-actions.current-branch", "github-actions", "Current Branch", 20, true, true,
		"senp.tree" }, commands, itemActions);
	auto publication = Publication({ layout::WorkbenchViewContainerDescriptor{
		"github-actions", "GitHub Actions", layout::EViewContainerLocation::Sidebar, 6,
		"$(play-circle)", false, { layout::EViewContainerLocation::Sidebar } } },
		std::move(trees), providers, target);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, composition.Activate(Launch(extension),
		extension.digest, std::move(publication), Clock::now()).status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());
	ASSERT_NE(nullptr, target);

	const auto workflows = providers.at(L"github-actions.workflows");
	workflows->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] { return workflows->Model().ItemCount() >= 1; }))
		<< "no workflow reached the tree from the live repository";
	const auto workflowRoot = workflows->Model().Node(L"");
	ASSERT_TRUE(workflowRoot);
	ASSERT_FALSE(workflowRoot->children.empty());

	const auto expand = [&](const std::wstring& id) {
		if (workflows->SetExpanded(id, true, Clock::now()) != tree::TreeResult::Applied) return false;
		return Await(composition, [&] {
			const auto node = workflows->Model().Node(id);
			return node && node->state != tree::TreeChildrenState::Unrequested
				&& node->state != tree::TreeChildrenState::Loading;
		});
	};
	// A workflow that has never run has nothing below it, and a run still in
	// progress may have no finished job; both are true answers rather than
	// failures, so the walk moves on. As upstream, a run lists its latest
	// attempt's jobs, and the log action is drawn only on a completed one. The
	// walk fails only if no run in the repository has a completed job at all.
	std::wstring jobId;
	for (const auto& workflowId : workflowRoot->children) {
		if (!workflowId.starts_with(L"workflow:")) continue;
		ASSERT_TRUE(expand(workflowId)) << "expanding a workflow never settled";
		const auto runs = workflows->Model().Node(workflowId)->children;
		for (const auto& runId : runs) {
			if (!runId.starts_with(L"run:")) continue;
			ASSERT_TRUE(expand(runId)) << "expanding a run never settled";
			for (const auto& id : workflows->Model().Node(runId)->children) {
				if (id.starts_with(L"job:") && !workflows->ItemActions(id).empty()) { jobId = id; break; }
			}
			if (!jobId.empty()) break;
		}
		if (!jobId.empty()) break;
	}
	ASSERT_FALSE(jobId.empty()) << "no completed job reached the tree from the live repository";

	// The job detail, whose table of steps is built entirely from the live answer.
	ASSERT_TRUE(workflows->Select(jobId));
	ASSERT_TRUE(workflows->Execute(jobId));
	ASSERT_TRUE(Await(composition, [&] { return target->DocumentCount() == 1; }))
		<< "the job detail read never produced a document";
	const auto jobSuffix = Suffix(jobId, 3);
	ASSERT_FALSE(jobSuffix.empty());
	ASSERT_NO_FATAL_FAILURE(ExpectDisplayed(composition, *target,
		L"github-actions-job:" + jobSuffix));

	/*
		The log is the one path no other test can reach. It is a separate tool
		operation, its answer names bytes the extension never holds, and those
		bytes only reach the reader by the window pumping text resource reads back
		through the seam to the control-side store. Everything in this assertion -
		the handle, the length and the characters on the surface - came out of the
		live job.
	*/
	// The log is the completed job row's inline "View job logs" action.
	ASSERT_TRUE(workflows->ExecuteItemAction(jobId, L"github-actions.workflow.logs"));
	ASSERT_TRUE(Await(composition, [&] { return target->DocumentCount() == 2; }))
		<< "the job log read never produced a document";
	ASSERT_NO_FATAL_FAILURE(ExpectDisplayed(composition, *target,
		L"github-actions-job-log:" + jobSuffix));

	m_pump = {};
	EXPECT_TRUE(composition.Close());
	pages.Close();
}

} // namespace
} // namespace workbench::editor
