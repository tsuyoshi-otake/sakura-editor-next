/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyDocumentView.h"
#include "workbench/editor/SenpEditorSurfaceSwitcher.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "markdown/MarkdownRemoteImageFetcher.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace workbench::editor::tests {
namespace {
using Model = editor::SenpReadonlyDocument;
using Context = senp::effect::OperationContext;
using Published = senp::effect::PublishDocument;
SenpReadonlyInput Input() { return { "detail", { "sample.details", 1, 2, 3 }, L"issue/7", L"Issue 7" }; }
Context Request(std::int64_t generation = 1) { return { L"document-" + std::to_wstring(generation), 1, 2, 3, generation }; }
Published Document(std::int64_t revision = 1) {
	return { L"issue/7", L"Issue 7: preserve unsaved work", revision, {
		senp::effect::MetadataSection{ { { L"Status", L"Open" }, { L"Author", L"sakura-user" } } },
		senp::effect::MarkdownSection{ L"## Description\n\nKeep **unsaved text** and the undo buffer when reading a detail.\n\n- Retain the editor\n- Restore focus\n\n```cpp\nreturn currentInput;\n```" },
		senp::effect::TableSection{ { L"Check", L"Result" }, { { { L"Source document", L"Retained" } }, { { L"Readonly detail", L"Available" } } } }
	} };
}
bool PumpUntil(const std::function<bool()>& predicate, DWORD timeout = 5000) {
	const auto deadline = ::GetTickCount64() + timeout;
	while (!predicate() && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG message{};
		for (int i = 0; i < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
			::TranslateMessage(&message); ::DispatchMessageW(&message);
		}
	}
	return predicate();
}
std::wstring WindowText(HWND window) {
	wchar_t text[1024]{}; ::GetWindowTextW(window, text, 1024); return text;
}
class SenpReadonlyDocument : public testing::Test {
protected:
	EditorCoreService core;
	editor::SenpReadonlyWorkbench inputs{ core };
	SenpReadonlyOpenResult opened{ inputs.Open(Input().scope, Input().resourceId, Input().title) };
	Model model{ *inputs.Find(opened.inputId) };
	HWND parent{};
	std::unique_ptr<SenpReadonlyDocumentView> view;
	std::unique_ptr<SenpEditorSurfaceSwitcher> switcher;
	HRESULT apartment{ E_FAIL };
	bool probeDone{};
	std::int64_t probeRevision{ 1 };
	unsigned int probeDpi{}, probeTheme{ 3 };
	HBRUSH probeBrush{};
	void TearDown() override {
		if (switcher) { switcher->Close(); switcher.reset(); }
		if (view) { view->Close(); view.reset(); }
		EXPECT_TRUE(PumpUntil([] { return markdown::MarkdownPreviewWorkerRetirement::Instance().ReservedOrPendingCount() == 0; }));
		if (parent) ::DestroyWindow(parent);
		if (probeBrush) ::DeleteObject(probeBrush);
		EXPECT_EQ(SenpReadonlyStatus::Succeeded, inputs.Shutdown());
		if (SUCCEEDED(apartment)) ::CoUninitialize();
	}
	void CreateNative() {
		apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		ASSERT_TRUE(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE);
		INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
		ASSERT_TRUE(::InitCommonControlsEx(&controls));
		parent = ::CreateWindowExW(0, L"STATIC", L"SENP native document verification", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			80, 80, 720, 540, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		view = std::make_unique<SenpReadonlyDocumentView>(model, 700296);
		ASSERT_TRUE(view->Create(parent));
		switcher = std::make_unique<SenpEditorSurfaceSwitcher>(inputs, parent);
		ASSERT_TRUE(switcher->Bind(opened.inputId, view->Window(), view->FocusWindow()));
		LOGFONT font{}; wcscpy_s(font.lfFaceName, L"Consolas");
		view->SetStyle(theme::CThemeService::PaletteFor(theme::ThemeMode::Dark), font, 96);
		ASSERT_EQ(SenpSurfaceProjection::Empty, switcher->Layout({ 0, 0, 660, 440 }));
		ASSERT_EQ(SenpSurfaceProjection::Applied, switcher->Show(opened.inputId, false));
		::ShowWindow(parent, SW_SHOW); ::UpdateWindow(parent);
		ASSERT_TRUE(PumpUntil([&] { return view->State() == SenpDocumentViewState::Prepared; }));
	}
	void Publish(Published document = Document(), Context context = Request()) {
		ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(context).result);
		ASSERT_EQ(SenpDocumentResult::Accepted, model.Apply(context, std::move(document)).result);
		if (view) {
			ASSERT_TRUE(view->Sync());
			ASSERT_TRUE(PumpUntil([&] { return view->State() != SenpDocumentViewState::Preparing; }));
		}
	}
	LRESULT Fingerprint() const {
		const auto viewport = view->ViewportSnapshot();
		std::uint64_t hash = 2166136261;
		for (const auto value : { static_cast<std::uint64_t>(viewport.scrollPosition), view->PreparedGeneration(),
			static_cast<std::uint64_t>(viewport.renderedLines), static_cast<std::uint64_t>(::IsWindowVisible(view->Window())) }) hash = (hash ^ value) * 16777619;
		for (const auto value : WindowText(view->FocusWindow())) hash = (hash ^ value) * 16777619;
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<SenpReadonlyDocument*>(context);
		if (message == WM_ERASEBKGND && self.probeBrush) {
			RECT rect{}; ::GetClientRect(window, &rect); ::FillRect(reinterpret_cast<HDC>(wParam), &rect, self.probeBrush); return 1;
		}
		if (message == WM_PAINT && self.probeBrush) {
			PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint);
			::FillRect(dc, &paint.rcPaint, self.probeBrush); ::EndPaint(window, &paint); return 0;
		}
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, wParam, lParam);
		try {
			switch (wParam) {
			case 0: return self.view && self.view->State() == SenpDocumentViewState::Prepared && !self.view->ViewportSnapshot().layoutPending;
			case 1: {
				const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff, themeId = HIWORD(lParam) >> 10;
				if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
				const bool styleChanged = self.probeDpi != dpi || self.probeTheme != themeId;
				if (styleChanged) {
					const auto palette = themeId == 2 ? theme::CThemeService::HighContrastPalette()
						: theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
					const auto brush = ::CreateSolidBrush(palette.canvas.ToColorRef()); if (!brush) return 0;
					if (self.probeBrush) ::DeleteObject(self.probeBrush); self.probeBrush = brush;
					LOGFONT font{}; wcscpy_s(font.lfFaceName, L"Consolas"); self.view->SetStyle(palette, font, dpi);
					self.probeDpi = dpi; self.probeTheme = themeId;
				}
				const auto result = self.switcher->Layout({ 0, 0, width + 200, 360 });
				if (styleChanged) ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
				return result == SenpSurfaceProjection::Applied;
			}
			case 2: self.switcher->SetVisible(lParam == 0); return 1;
			case 3: ::SendMessageW(self.view->FocusWindow(), WM_VSCROLL, lParam ? SB_BOTTOM : SB_TOP, 0); return 1;
			case 4: self.probeDone = true; return 1;
			case 5: return reinterpret_cast<LRESULT>(self.view->Window());
			case 7: return reinterpret_cast<LRESULT>(self.view->FocusWindow());
			case 8: {
				auto document = Document(++self.probeRevision); document.title = lParam ? L"Issue 7: refreshed detail" : L"Issue 7: preserve unsaved work";
				for (int row = 0; row < 24; ++row) std::get<senp::effect::TableSection>(document.sections[2]).rows.push_back({ { L"Verification " + std::to_wstring(row + 1), L"Passed" } });
				const auto request = Request(self.probeRevision);
				if (self.model.Begin(request).result != SenpDocumentResult::Accepted
					|| self.model.Apply(request, std::move(document)).result != SenpDocumentResult::Accepted) return 0;
				return self.view->Sync();
			}
			case 9: return self.Fingerprint();
			default: return 0;
			}
		} catch (...) { return 0; }
	}
};

TEST_F(SenpReadonlyDocument, MixedSectionsKeepMetadataAndTableValuesLiteral)
{
	auto document = Document();
	auto& metadata = std::get<senp::effect::MetadataSection>(document.sections[0]);
	metadata.fields[0] = { L"**literal**", L"<script>alert(1)</script> | [x](command:danger)" };
	auto& table = std::get<senp::effect::TableSection>(document.sections[2]);
	table.rows[0].cells[1] = L"**not bold**\n<script>literal</script>";
	const auto result = PrepareSenpReadonlyDocument(document);
	ASSERT_EQ(SenpDocumentResult::Accepted, result.result);
	ASSERT_GE(result.document.blocks.size(), 4u);
	EXPECT_EQ(document.title, result.document.blocks[0].text);
	const auto& fields = result.document.blocks[1].tableRows;
	ASSERT_EQ(3u, fields.size()); EXPECT_TRUE(fields[0].header);
	EXPECT_EQ(metadata.fields[0].name, fields[1].cells[0].text);
	EXPECT_EQ(metadata.fields[0].value, fields[1].cells[1].text);
	EXPECT_TRUE(fields[1].cells[1].inlineSpans.empty());
	EXPECT_EQ(table.rows[0].cells[1], result.document.blocks.back().tableRows[1].cells[1].text);
	EXPECT_TRUE(result.document.blocks.back().tableRows[1].cells[1].inlineSpans.empty());
}

TEST_F(SenpReadonlyDocument, ScriptUnsafeCommandsAndAllImageResourcesAreInert)
{
	auto document = Document();
	document.sections = { senp::effect::MarkdownSection{
		L"<script>secret_script_body</script>\n\n<style>secret_style_body</style>\n\n"
		L"[execute](command:danger) [script](javascript:bad) [relative](./private.txt) [anchor](#section)\n\n"
		L"![local](C:/private/image.png)\n\n![network](https://example.com/tracker.png)\n\n"
		L"<img src=\"file:///C:/private/image.png\" onerror=\"bad()\">\n\n"
		L"| Link |\n| --- |\n| [secret](file:///C:/private.txt) |" } };
	const auto result = PrepareSenpReadonlyDocument(document);
	ASSERT_EQ(SenpDocumentResult::Accepted, result.result);
	std::size_t references = 0;
	const auto denied = [&](const markdown::ResourceReference& resource) {
		++references; EXPECT_EQ(markdown::ResourceDisposition::ExternalBlocked, resource.disposition);
		EXPECT_TRUE(resource.allowedRoot.empty()); EXPECT_TRUE(resource.resolvedPath.empty());
	};
	for (const auto& block : result.document.blocks) {
		EXPECT_EQ(std::wstring::npos, block.text.find(L"secret_script_body"));
		EXPECT_EQ(std::wstring::npos, block.text.find(L"secret_style_body"));
		for (const auto& span : block.inlineSpans) if (span.resource) denied(*span.resource);
		for (const auto& image : block.images) denied(image.source);
		for (const auto& row : block.tableRows) for (const auto& cell : row.cells)
			for (const auto& span : cell.inlineSpans) if (span.resource) denied(*span.resource);
	}
	EXPECT_GE(references, 6u);
	EXPECT_EQ(markdown::CapabilityStatus::Unsupported, result.document.capabilities.localImageProjection);
	EXPECT_EQ(markdown::CapabilityStatus::Unsupported, result.document.capabilities.secureRemoteImageProjection);
	EXPECT_EQ(markdown::CapabilityStatus::Unsupported, result.document.capabilities.rawHtmlExecution);
}

TEST_F(SenpReadonlyDocument, MalformedUtf16ShapeAndAggregateLimitsPublishNothing)
{
	auto invalid = Document(); invalid.title = std::wstring(1, L'\xd800');
	EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(invalid).result);
	invalid = Document(); std::get<senp::effect::TableSection>(invalid.sections[2]).rows[0].cells.pop_back();
	EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(invalid).result);
	invalid = Document(); invalid.sections = { senp::effect::MarkdownSection{ std::wstring(262145, L'a') } };
	EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(invalid).result);
	invalid.sections.assign(5, senp::effect::MarkdownSection{ std::wstring(240000, L'a') });
	EXPECT_FALSE(senp::effect::ValidateDocument(invalid));
	EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(invalid).result);
	invalid = Document(); invalid.sections.assign(33, senp::effect::MarkdownSection{ L"a" });
	EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(invalid).result);
}

TEST_F(SenpReadonlyDocument, EmptyContentIsValidAndControlUnitsRemainVisible)
{
	auto document = Document(); document.sections.clear();
	auto result = PrepareSenpReadonlyDocument(document);
	ASSERT_EQ(SenpDocumentResult::Accepted, result.result); ASSERT_EQ(1u, result.document.blocks.size());
	document.sections = { senp::effect::MetadataSection{ { { L"control", std::wstring(L"a\0b", 3) } } } };
	result = PrepareSenpReadonlyDocument(document); ASSERT_EQ(SenpDocumentResult::Accepted, result.result);
	EXPECT_EQ(L"a\xfffd" L"b", result.document.blocks[1].tableRows[1].cells[1].text);
	document.title = L"bad\ntitle"; EXPECT_EQ(SenpDocumentResult::Invalid, PrepareSenpReadonlyDocument(document).result);
}

TEST_F(SenpReadonlyDocument, TextResourcesRemainTypedUnsupportedUntilTheirOwnRendererIsBound)
{
	auto document = Document(); document.sections.push_back(senp::effect::TextResourceSection{ L"log-1", 10, senp::effect::TextStatus::Complete });
	EXPECT_EQ(SenpDocumentResult::Unsupported, PrepareSenpReadonlyDocument(document).result);
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request()).result);
	auto result = model.Apply(Request(), document);
	EXPECT_EQ(SenpDocumentResult::Unsupported, result.result); EXPECT_TRUE(result.retired);
	EXPECT_EQ(SenpDocumentState::Failed, model.State()); EXPECT_FALSE(model.Content());
}

TEST_F(SenpReadonlyDocument, ReplacingARequestReturnsItsCancellationOwnerAndRejectsLateResults)
{
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request()).result);
	const auto replaced = model.Begin(Request(2));
	ASSERT_TRUE(replaced.retired); EXPECT_EQ(Request(), *replaced.retired);
	EXPECT_EQ(SenpDocumentResult::Stale, model.Apply(Request(), Document()).result);
	auto derived = Request(2); derived.operationId = L"tool-completed-2";
	EXPECT_EQ(SenpDocumentResult::Accepted, model.Apply(derived, Document()).result);
	EXPECT_EQ(SenpDocumentState::Ready, model.State()); ASSERT_TRUE(model.Content());
	EXPECT_EQ(Document(), *model.Content());
	EXPECT_EQ(SenpDocumentResult::Stale, model.Apply(derived, Document(2)).result);
}

TEST_F(SenpReadonlyDocument, AllScopeDimensionsFencePublicationAndAdmission)
{
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request()).result);
	for (int dimension = 0; dimension < 4; ++dimension) {
		auto foreign = Request();
		if (dimension == 0) ++foreign.ownerGeneration;
		if (dimension == 1) ++foreign.workspaceRevision;
		if (dimension == 2) ++foreign.accountGeneration;
		if (dimension == 3) ++foreign.requestGeneration;
		EXPECT_EQ(SenpDocumentResult::Stale, model.Apply(foreign, Document()).result);
		EXPECT_EQ(SenpDocumentState::Loading, model.State());
		if (dimension != 3) EXPECT_EQ(SenpDocumentResult::Stale, model.Begin(foreign).result);
	}
	EXPECT_EQ(SenpDocumentResult::Stale, model.Begin(Request()).result);
	auto invalid = Request(2); invalid.operationId = L"bad id";
	EXPECT_EQ(SenpDocumentResult::Invalid, model.Begin(invalid).result);
	EXPECT_EQ(SenpDocumentResult::Accepted, model.Apply(Request(), Document()).result);
}

TEST_F(SenpReadonlyDocument, SameRevisionRefreshMustBeIdenticalAndNewRevisionsKeepInputIdentity)
{
	Publish(); const auto input = model.Input().inputId;
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(2)).result);
	EXPECT_EQ(SenpDocumentResult::Accepted, model.Apply(Request(2), Document()).result);
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(3)).result);
	auto changed = Document(); changed.title = L"Forged same revision";
	EXPECT_EQ(SenpDocumentResult::Invalid, model.Apply(Request(3), changed).result);
	EXPECT_EQ(SenpDocumentState::Failed, model.State());
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(4)).result);
	changed.revision = INT64_MAX;
	EXPECT_EQ(SenpDocumentResult::Accepted, model.Apply(Request(4), changed).result);
	EXPECT_EQ(input, model.Input().inputId); EXPECT_EQ(changed.title, model.Content()->title);
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(5)).result);
	EXPECT_EQ(SenpDocumentResult::Stale, model.Apply(Request(5), Document(2)).result);
}

TEST_F(SenpReadonlyDocument, FailureExpiryAndCloseEachRetireExactlyOnePendingRequest)
{
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request()).result);
	EXPECT_TRUE(model.Fail(Request()).retired); EXPECT_FALSE(model.Fail(Request()).retired);
	EXPECT_EQ(SenpDocumentState::Failed, model.State());
	Publish(Document(2), Request(2));
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(3)).result);
	EXPECT_TRUE(model.Expire().retired); EXPECT_FALSE(model.Content());
	EXPECT_FALSE(model.Expire().retired); EXPECT_EQ(SenpDocumentState::Expired, model.State());
	EXPECT_EQ(SenpDocumentResult::Closed, model.Begin(Request(4)).result);
	EXPECT_EQ(SenpDocumentResult::Stale, model.Apply(Request(3), Document(3)).result);
	EXPECT_EQ(SenpDocumentResult::Accepted, model.Close().result);
	EXPECT_EQ(SenpDocumentState::Closed, model.State()); EXPECT_EQ(SenpDocumentResult::Closed, model.Close().result);
	Model other(Input()); ASSERT_EQ(SenpDocumentResult::Accepted, other.Begin(Request()).result);
	EXPECT_TRUE(other.Close().retired); EXPECT_FALSE(other.Close().retired);
}

TEST_F(SenpReadonlyDocument, NativeMixedContentUsesTheExistingPreviewAndRetainsItsWindow)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view);
	const auto root = view->Window(), preview = view->FocusWindow();
	Publish(); EXPECT_EQ(SenpDocumentViewState::Prepared, view->State());
	EXPECT_EQ(model.Generation(), view->PreparedGeneration()); EXPECT_EQ(Document().title, WindowText(preview));
	EXPECT_EQ(root, ::GetParent(preview));
	for (int i = 0; i < 10; ++i) EXPECT_TRUE(view->Sync());
	EXPECT_EQ(root, view->Window()); EXPECT_EQ(preview, view->FocusWindow());
	auto changed = Document(INT64_MAX); changed.title = L"Updated title";
	Publish(std::move(changed), Request(2)); EXPECT_EQ(L"Updated title", WindowText(preview));
	view->Show(false); EXPECT_FALSE(::IsWindowVisible(preview));
	view->Show(true); EXPECT_TRUE(::IsWindowVisible(preview));
	EXPECT_EQ(root, view->Window()); EXPECT_EQ(preview, view->FocusWindow());
	::SetFocus(root); EXPECT_EQ(preview, ::GetFocus());
}

TEST_F(SenpReadonlyDocument, NativeFailureAndExpiryReplacePreviousBodyWithExplicitStates)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view); Publish();
	ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(Request(2)).result);
	EXPECT_TRUE(view->Sync());
	ASSERT_TRUE(PumpUntil([&] { return view->State() == SenpDocumentViewState::Prepared; }));
	EXPECT_EQ(L"Loading...", WindowText(view->FocusWindow()));
	(void)model.Fail(Request(2)); EXPECT_TRUE(view->Sync());
	ASSERT_TRUE(PumpUntil([&] { return view->State() == SenpDocumentViewState::Prepared; }));
	EXPECT_NE(std::wstring::npos, WindowText(view->FocusWindow()).find(L"could not be loaded"));
	(void)model.Expire(); EXPECT_TRUE(view->Sync());
	ASSERT_TRUE(PumpUntil([&] { return view->State() == SenpDocumentViewState::Prepared; }));
	EXPECT_NE(std::wstring::npos, WindowText(view->FocusWindow()).find(L"no longer available"));
	const auto preview = view->FocusWindow(); view->Close(); view->Close();
	EXPECT_FALSE(::IsWindow(preview)); EXPECT_EQ(SenpDocumentViewState::Closed, view->State()); EXPECT_FALSE(view->Sync());
}

TEST_F(SenpReadonlyDocument, NativePreparationLimitIsTerminalAndCanBeExplicitlyRefreshed)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view);
	auto large = Document(); std::wstring text;
	for (int i = 0; i < 5000; ++i) text += L"# heading\n\n";
	large.sections = { senp::effect::MarkdownSection{ text } };
	Publish(std::move(large));
	EXPECT_EQ(SenpDocumentViewState::Failed, view->State());
	EXPECT_NE(std::wstring::npos, WindowText(view->FocusWindow()).find(L"could not be rendered"));
	EXPECT_FALSE(view->Sync());
	Publish(Document(2), Request(2)); EXPECT_EQ(SenpDocumentViewState::Prepared, view->State());
}

TEST_F(SenpReadonlyDocument, NativeParentDestructionClosesTheWorkerAndProjection)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view); Publish();
	const auto preview = view->FocusWindow(); ASSERT_TRUE(::DestroyWindow(parent)); parent = nullptr;
	EXPECT_FALSE(::IsWindow(preview)); EXPECT_EQ(SenpDocumentViewState::Closed, view->State());
	EXPECT_FALSE(view->Sync()); view->Close();
}

TEST_F(SenpReadonlyDocument, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1')
		GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet ReadonlyDocuments";
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view);
	Publish(); ASSERT_TRUE(::SetWindowSubclass(parent, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	ASSERT_TRUE(::SendMessageW(parent, WM_APP + 0x296, 8, 0));
	const auto deadline = ::GetTickCount64() + 300000;
	while (!probeDone && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG message{};
		for (int i = 0; i < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
			::TranslateMessage(&message); ::DispatchMessageW(&message);
		}
	}
	::RemoveWindowSubclass(parent, ProbeProcedure, 296); EXPECT_TRUE(probeDone);
}

TEST_F(SenpReadonlyDocument, NativePreparedWorkerCoalescesPendingValuesAndFencesStaleCallbacks)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view);
	markdown::CMarkdownPreviewWnd preview({}, 700297); ASSERT_TRUE(preview.Create(parent));
	std::mutex mutex; std::condition_variable condition; bool release = false;
	std::atomic_bool entered{ false }; std::atomic_int skipped{ 0 };
	std::vector<markdown::PreviewRenderKey> delivered;
	preview.SetPreparationCallback([&](auto key, bool success) { EXPECT_TRUE(success); delivered.push_back(key); });
	ASSERT_TRUE(preview.QueuePreparedDocument([&] {
		entered = true; std::unique_lock lock(mutex);
		condition.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
		return markdown::Document{};
	}, { 1, 0 }));
	const bool started = PumpUntil([&] { return entered.load(); });
	EXPECT_TRUE(started);
	EXPECT_TRUE(preview.QueuePreparedDocument([&] { ++skipped; return markdown::Document{}; }, { 2, 0 }));
	EXPECT_TRUE(preview.QueuePreparedDocument([] { return markdown::Document{}; }, { 3, 0 }));
	{ std::lock_guard lock(mutex); release = true; } condition.notify_all();
	EXPECT_TRUE(PumpUntil([&] { return !delivered.empty(); }));
	ASSERT_EQ(1u, delivered.size()); EXPECT_EQ(3u, delivered[0].generation); EXPECT_EQ(0, skipped.load());
	preview.Close(); EXPECT_FALSE(preview.QueuePreparedDocument([] { return markdown::Document{}; }, { 4, 0 }));
}

TEST_F(SenpReadonlyDocument, NativePreparedWorkerReportsFailuresAndDeferredCommitExactlyOnce)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); ASSERT_TRUE(view);
	markdown::CMarkdownPreviewWnd preview({}, 700298); ASSERT_TRUE(preview.Create(parent));
	std::vector<std::pair<std::uint64_t, bool>> delivered;
	preview.SetPreparationCallback([&](auto key, bool success) { delivered.emplace_back(key.generation, success); });
	ASSERT_TRUE(preview.QueuePreparedDocument([]() -> markdown::Document { throw std::runtime_error("failure"); }, { 1, 0 }));
	ASSERT_TRUE(PumpUntil([&] { return delivered.size() == 1; })); EXPECT_FALSE(delivered[0].second);
	preview.Layout({ 0, 0, 400, 200 }, 96, true);
	std::atomic_bool prepared{ false };
	ASSERT_TRUE(preview.QueuePreparedDocument([&] { prepared = true; return markdown::Document{}; }, { 2, 0 }));
	ASSERT_TRUE(PumpUntil([&] { return prepared.load(); }));
	// A following message-queue barrier lets the already prepared completion defer.
	(void)PumpUntil([&] { return delivered.size() > 1; }, 50);
	EXPECT_EQ(1u, delivered.size()); preview.Layout({ 0, 0, 400, 200 }, 96);
	ASSERT_TRUE(PumpUntil([&] { return delivered.size() == 2; })); EXPECT_TRUE(delivered[1].second);
	preview.Layout({ 0, 0, 450, 200 }, 96); EXPECT_EQ(2u, delivered.size()); preview.Close();
}

TEST_F(SenpReadonlyDocument, NativePreparedContentPerformsZeroRemoteImageFetches)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	class Fetcher final : public markdown::IMarkdownRemoteImageFetcher {
	public:
		std::atomic_int requests{ 0 };
		markdown::RemoteImageFetchResult Fetch(std::wstring_view, const platform::request::IRequestCancellation*) override {
			++requests; return {};
		}
	};
	const auto fetcher = std::make_shared<Fetcher>();
	markdown::CMarkdownPreviewWnd preview(fetcher, 700299); ASSERT_TRUE(preview.Create(parent));
	bool delivered = false;
	preview.SetPreparationCallback([&](auto, bool success) { EXPECT_TRUE(success); delivered = true; });
	auto document = Document();
	document.sections = { senp::effect::MarkdownSection{ L"![tracking](https://example.com/one.png)\n\n![local](C:/private/two.png)" } };
	ASSERT_TRUE(preview.QueuePreparedDocument([document] { return PrepareSenpReadonlyDocument(document).document; }, { 1, 0 }));
	EXPECT_TRUE(PumpUntil([&] { return delivered; })); EXPECT_EQ(0, fetcher->requests.load());
	preview.Close();
}

TEST_F(SenpReadonlyDocument, NativeDeferredResultCannotPublishAfterAReplacementWasQueued)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	markdown::CMarkdownPreviewWnd preview({}, 700300); ASSERT_TRUE(preview.Create(parent));
	std::vector<std::uint64_t> delivered;
	preview.SetPreparationCallback([&](auto key, bool success) { EXPECT_TRUE(success); delivered.push_back(key.generation); });
	preview.Layout({ 0, 0, 400, 200 }, 96, true);
	ASSERT_TRUE(preview.QueuePreparedDocument([] { return markdown::Document{}; }, { 1, 0 }));
	EXPECT_TRUE(PumpUntil([&] { return preview.ViewportSnapshot().layoutPending; }));
	(void)PumpUntil([&] { return !delivered.empty(); }, 50);
	EXPECT_TRUE(delivered.empty());
	ASSERT_TRUE(preview.QueuePreparedDocument([] { return markdown::Document{}; }, { 2, 0 }));
	preview.Layout({ 0, 0, 400, 200 }, 96);
	EXPECT_TRUE(PumpUntil([&] { return !delivered.empty(); }));
	preview.Close(); ASSERT_EQ(1u, delivered.size()); EXPECT_EQ(2u, delivered[0]);
}

} // namespace
} // namespace workbench::editor::tests
