/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpEditorSurfaceSwitcher.h"
#include "workbench/editor/EditorCommandIds.h"
#include "charset/charcode.h"
#include "cmd/COpeBlk.h"
#include "doc/CEditDoc.h"
#include "doc/logic/CDocLine.h"
#include "env/ShareDataTestSuite.hpp"
#include "theme/CThemeService.h"

#include <CommCtrl.h>
#include <algorithm>
#include <array>

namespace workbench::editor::tests {
namespace {
class SenpReadonlyWorkbench : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() {
		SetUpShareData();
		SelectCharWidthCache(CWM_FONT_EDIT, CWM_CACHE_SHARE);
		InitCharWidthCache(GetDllShareData().m_Common.m_sView.m_lf);
	}
	static void TearDownTestSuite() { TearDownShareData(); }
	EditorCoreService core;
	editor::SenpReadonlyWorkbench model{ core };
	SenpReadonlyScope scope{ "sample.details", 1, 2, 3 };
	HWND parent{}, legacy{}, detail{};
	std::unique_ptr<SenpEditorSurfaceSwitcher> native;
	std::string detailId;
	int nativeHook{};
	bool unbindAccepted{ true };
	int positionMessages{}, paintMessages{};
	bool probeDone{};
	int probeWidth{ 380 }, probeOffset{};
	unsigned int probeDpi{}, probeTheme{ 3 };
	theme::ThemePalette probePalette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	theme::CThemeFont probeFont;
	HBRUSH probeBrush{};
	LRESULT ProbeFingerprint() const {
		std::uint64_t hash = 2166136261;
		for (const auto surface : { legacy, detail }) {
			RECT rect{}; ::GetWindowRect(surface, &rect);
			for (const auto value : { rect.left, rect.top, rect.right, rect.bottom, static_cast<LONG>(::IsWindowVisible(surface)) })
				hash = (hash ^ static_cast<std::uint32_t>(value)) * 16777619;
		}
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& fixture = *reinterpret_cast<SenpReadonlyWorkbench*>(context);
		if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) && fixture.probeBrush) {
			const auto dc = reinterpret_cast<HDC>(wParam);
			::SetTextColor(dc, fixture.probePalette.primaryText.ToColorRef());
			::SetBkColor(dc, fixture.probePalette.canvas.ToColorRef());
			return reinterpret_cast<LRESULT>(fixture.probeBrush);
		}
		if (message == WM_ERASEBKGND && fixture.probeBrush) {
			RECT rect{}; ::GetClientRect(window, &rect);
			::FillRect(reinterpret_cast<HDC>(wParam), &rect, fixture.probeBrush); return 1;
		}
		if (message == WM_PAINT && fixture.probeBrush) {
			PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint);
			::FillRect(dc, &paint.rcPaint, fixture.probeBrush); ::EndPaint(window, &paint); return 0;
		}
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, wParam, lParam);
		try {
			switch (wParam) {
			case 0: return fixture.native && ::IsWindow(fixture.legacy) && ::IsWindow(fixture.detail);
			case 1: {
				const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff, themeId = HIWORD(lParam) >> 10;
				if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
				const bool styleChanged = fixture.probeDpi != dpi || fixture.probeTheme != themeId;
				if (styleChanged) {
					fixture.probePalette = themeId == 2 ? theme::CThemeService::HighContrastPalette()
						: theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
					const auto brush = ::CreateSolidBrush(fixture.probePalette.canvas.ToColorRef()); if (!brush) return 0;
					if (fixture.probeBrush) ::DeleteObject(fixture.probeBrush); fixture.probeBrush = brush;
					if (!fixture.probeFont.Recreate(theme::ThemeFontKind::Chrome, dpi)) return 0;
					for (const auto surface : { fixture.legacy, fixture.detail }) ::SendMessageW(surface, WM_SETFONT, reinterpret_cast<WPARAM>(fixture.probeFont.Get()), FALSE);
					fixture.probeDpi = dpi; fixture.probeTheme = themeId;
				}
				fixture.probeWidth = width;
				const auto projected = fixture.native->Layout({ fixture.probeOffset, fixture.probeOffset, width + 200 + fixture.probeOffset, 320 });
				// Matrix setup may repaint a changed test palette/font. Resize trials
				// keep both fixed, so only the production switcher can repair old pixels.
				if (styleChanged) ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
				return projected == SenpSurfaceProjection::Applied;
			}
			case 2: return fixture.native->Show(lParam ? fixture.detailId : "legacy", false) == SenpSurfaceProjection::Applied;
			case 3: fixture.native->SetVisible(lParam == 0); return 1;
			case 4: fixture.probeDone = true; return 1;
			case 5: return reinterpret_cast<LRESULT>(fixture.detail);
			case 7: return reinterpret_cast<LRESULT>(fixture.legacy);
			case 8:
				fixture.probeOffset = lParam ? 25 : 0;
				return fixture.native->Layout({ fixture.probeOffset, fixture.probeOffset, fixture.probeWidth + 200 + fixture.probeOffset, 320 }) == SenpSurfaceProjection::Applied;
			case 9: return fixture.ProbeFingerprint();
			default: return 0;
			}
		} catch (...) { return 0; }
	}
	static LRESULT CALLBACK Hook(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& fixture = *reinterpret_cast<SenpReadonlyWorkbench*>(context);
		if (message == WM_WINDOWPOSCHANGED) ++fixture.positionMessages;
		if (message == WM_PAINT) ++fixture.paintMessages;
		if (message == WM_SHOWWINDOW && wParam && fixture.nativeHook) {
			const auto action = std::exchange(fixture.nativeHook, 0);
			if (action == 1) fixture.native->Close();
			else fixture.unbindAccepted = fixture.native->Unbind(fixture.detailId);
		}
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	void SetUp() override {
		const EditorDocumentIdentity identity{ .opaqueId = "retained-document" };
		ASSERT_EQ(EEditorOperationStatus::Succeeded, core.OpenResolvedInput({
			.operation = { "fixture.open" }, .input = { "legacy", identity },
			.resolvedDocument = ResolvedEditorDocument{ identity, 42, true },
		}).status);
	}
	void TearDown() override {
		if (native) { native->Close(); native.reset(); }
		EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Shutdown());
		if (parent) { ::DestroyWindow(parent); EXPECT_FALSE(::IsWindow(parent)); }
		if (probeBrush) ::DeleteObject(probeBrush);
	}
	void CreateNative() {
		parent = ::CreateWindowExW(0, L"STATIC", L"SENP readonly editor fixture", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			180, 130, 620, 380, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		legacy = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
			0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, legacy);
		const auto opened = model.Open(scope, L"issue/7", L"Issue 7");
		ASSERT_EQ(SenpReadonlyStatus::Succeeded, opened.status); detailId = opened.inputId;
		detail = ::CreateWindowExW(0, L"EDIT", L"Readonly detail\r\nIssue body", WS_CHILD | WS_TABSTOP | WS_VSCROLL
			| ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, detail);
		native = std::make_unique<SenpEditorSurfaceSwitcher>(model, parent);
		ASSERT_TRUE(native->Bind("legacy", legacy));
		ASSERT_TRUE(native->Bind(detailId, detail));
		::ShowWindow(parent, SW_SHOWNOACTIVATE);
		ASSERT_EQ(SenpSurfaceProjection::Applied, native->Layout({ 0, 0, 590, 320 }));
	}
	static std::wstring Text(HWND window) {
		std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(window)) + 1, L'\0');
		const auto length = ::GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
		text.resize(static_cast<std::size_t>(length)); return text;
	}
	void ExpectLegacy() {
		const auto snapshot = core.Snapshot();
		const auto input = std::ranges::find_if(snapshot.group.inputs, [](const auto& value) { return value.descriptor.inputId == "legacy"; });
		ASSERT_NE(snapshot.group.inputs.end(), input);
		const auto document = std::ranges::find_if(snapshot.documents, [&](const auto& value) { return value.documentKey == input->documentKey; });
		ASSERT_NE(snapshot.documents.end(), document);
		EXPECT_TRUE(document->dirty); EXPECT_EQ(42U, document->documentRevision); EXPECT_EQ(1U, document->inputReferenceCount);
	}
};

TEST_F(SenpReadonlyWorkbench, OpensInactiveAndReusesExactScopedResourceInTheExistingGroup)
{
	const auto first = model.Open(scope, L"issues/7", L"First title");
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, first.status);
	EXPECT_EQ("legacy", *core.Snapshot().group.activeInputId);
	const auto same = model.Open(scope, L"issues/7", L"New title cannot change identity");
	EXPECT_EQ(SenpReadonlyStatus::Reused, same.status); EXPECT_EQ(first.inputId, same.inputId);
	EXPECT_EQ(2U, core.Snapshot().group.inputs.size());
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Show(first.inputId));
	EXPECT_EQ(first.inputId, *core.Snapshot().group.activeInputId);
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Show("legacy"));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, ScopeFencesAndResourceSpellingCannotAlias)
{
	const auto first = model.Open(scope, L"issues/7", L"Same title");
	std::array<SenpReadonlyScope, 4> others{ scope, scope, scope, scope };
	others[0].ownerGeneration++; others[1].workspaceRevision++; others[2].accountGeneration++;
	others[3].extensionId = "sample.actions";
	for (const auto& other : others) {
		const auto opened = model.Open(other, L"issues/7", L"Same title");
		EXPECT_EQ(SenpReadonlyStatus::Succeeded, opened.status); EXPECT_NE(first.inputId, opened.inputId);
	}
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Open(scope, L"issues%2f7", L"Same title").status);
	const auto snapshot = core.Snapshot();
	for (std::size_t i = 0; i < snapshot.documents.size(); ++i) {
		for (std::size_t j = i + 1; j < snapshot.documents.size(); ++j) EXPECT_NE(snapshot.documents[i].documentKey, snapshot.documents[j].documentKey);
	}
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, RejectsMalformedOrOversizedIdentityBeforeOpening)
{
	for (const auto& invalid : { L"", L"line\nbreak", L"\xd800" }) EXPECT_EQ(SenpReadonlyStatus::Invalid, model.Open(scope, invalid, L"Title").status);
	EXPECT_EQ(SenpReadonlyStatus::Invalid, model.Open(scope, std::wstring(513, L'a'), L"Title").status);
	EXPECT_EQ(SenpReadonlyStatus::Invalid, model.Open(scope, L"item", std::wstring(257, L'a')).status);
	auto invalid = scope; invalid.ownerGeneration = 0;
	EXPECT_EQ(SenpReadonlyStatus::Invalid, model.Open(invalid, L"item", L"Title").status);
	invalid = scope; invalid.extensionId = "../sample";
	EXPECT_EQ(SenpReadonlyStatus::Invalid, model.Open(invalid, L"item", L"Title").status);
	EXPECT_EQ(1U, core.Snapshot().group.inputs.size());
}

TEST_F(SenpReadonlyWorkbench, BoundsInputsAndRecoversSlotAfterExternalCoreClose)
{
	std::string first;
	for (std::size_t i = 0; i < editor::SenpReadonlyWorkbench::kMaximumInputs; ++i) {
		const auto opened = model.Open(scope, L"item/" + std::to_wstring(i), L"Item");
		ASSERT_EQ(SenpReadonlyStatus::Succeeded, opened.status); if (!i) first = opened.inputId;
	}
	EXPECT_EQ(SenpReadonlyStatus::LimitReached, model.Open(scope, L"overflow", L"Overflow").status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, core.CloseInput({ { "external.close" }, first }).status);
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Open(scope, L"replacement", L"Replacement").status);
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, CloseAndExactCohortRevocationPreserveOtherInputs)
{
	const auto first = model.Open(scope, L"one", L"One");
	const auto second = model.Open(scope, L"two", L"Two");
	auto other = scope; ++other.ownerGeneration;
	const auto third = model.Open(other, L"one", L"One");
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Show(first.inputId));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Close(first.inputId));
	EXPECT_EQ(second.inputId, *core.Snapshot().group.activeInputId);
	EXPECT_EQ(SenpReadonlyStatus::NotFound, model.Close("legacy"));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Revoke(scope));
	EXPECT_EQ(third.inputId, *core.Snapshot().group.activeInputId);
	EXPECT_NE(nullptr, model.Find(third.inputId));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Shutdown());
	EXPECT_EQ("legacy", *core.Snapshot().group.activeInputId);
	EXPECT_EQ(SenpReadonlyStatus::Closed, model.Open(other, L"later", L"Later").status);
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, ReadonlyCommandsCannotFallThroughToLegacyMutation)
{
	EXPECT_EQ(SenpEditorCommandRoute::Legacy, model.Route(command_ids::Save));
	const auto opened = model.Open(scope, L"one", L"One");
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Show(opened.inputId));
	for (const auto command : { command_ids::Save, command_ids::SaveAs, command_ids::Revert,
		std::string_view("undo"), std::string_view("redo"), std::string_view("type"), std::string_view("editor.action.clipboardPasteAction"), std::string_view("unknown.command") }) {
		EXPECT_EQ(SenpEditorCommandRoute::NotApplicable, model.Route(command));
	}
	for (const auto command : { "editor.action.clipboardCopyAction", "editor.action.selectAll", "actions.find",
		"editor.action.nextMatchFindAction", "editor.action.previousMatchFindAction" }) EXPECT_EQ(SenpEditorCommandRoute::ReadonlySurface, model.Route(command));
	EXPECT_EQ(SenpEditorCommandRoute::CloseReadonly, model.Route(command_ids::CloseActiveEditor));
	for (const auto command : { command_ids::SaveAll, command_ids::NewUntitledFile, command_ids::OpenFile,
		command_ids::CloseWindow, command_ids::Quit }) EXPECT_EQ(SenpEditorCommandRoute::Workbench, model.Route(command));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, CoreNotificationReentryIsRejectedWithAnExplicitTerminal)
{
	SenpReadonlyStatus reentrant = SenpReadonlyStatus::Succeeded;
	auto subscription = core.Subscribe([&](const EditorCoreChangeBatch&) { reentrant = model.Open(scope, L"nested", L"Nested").status; });
	const auto opened = model.Open(scope, L"outer", L"Outer");
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, opened.status);
	EXPECT_EQ(SenpReadonlyStatus::Conflict, reentrant);
	EXPECT_EQ(2U, core.Snapshot().group.inputs.size());
	subscription->Unsubscribe();
}

TEST_F(SenpReadonlyWorkbench, ReplacementControllerCannotReplayAnEarlierControllersOperations)
{
	std::string earlierId;
	{
		editor::SenpReadonlyWorkbench first(core);
		const auto opened = first.Open(scope, L"earlier", L"Earlier");
		ASSERT_EQ(SenpReadonlyStatus::Succeeded, opened.status); earlierId = opened.inputId;
		ASSERT_EQ(SenpReadonlyStatus::Succeeded, first.Shutdown());
	}
	editor::SenpReadonlyWorkbench second(core);
	const auto opened = second.Open(scope, L"later", L"Later");
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, opened.status); EXPECT_NE(earlierId, opened.inputId);
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, second.Show(opened.inputId));
	EXPECT_EQ(opened.inputId, *core.Snapshot().group.activeInputId);
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, second.Shutdown());
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeSwitchRetainsSelectionScrollAndExecutableUndo)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	std::wstring text;
	for (int i = 0; i < 80; ++i) text += L"Editable retained line " + std::to_wstring(i) + L"\r\n";
	::SetWindowTextW(legacy, text.c_str());
	::SendMessageW(legacy, EM_SETSEL, 0, 0);
	::SendMessageW(legacy, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"Unsaved "));
	::SendMessageW(legacy, EM_SETMODIFY, TRUE, 0);
	::SendMessageW(legacy, EM_SETSEL, 20, 35);
	::SendMessageW(legacy, EM_LINESCROLL, 0, 30);
	ASSERT_TRUE(::SendMessageW(legacy, EM_CANUNDO, 0, 0));
	const auto before = Text(legacy);
	const auto firstLine = ::SendMessageW(legacy, EM_GETFIRSTVISIBLELINE, 0, 0);
	const auto selection = ::SendMessageW(legacy, EM_GETSEL, 0, 0);
	ASSERT_GT(firstLine, 0);
	for (int i = 0; i < 12; ++i) {
		ASSERT_EQ(SenpSurfaceProjection::Applied, native->Show(detailId));
		EXPECT_FALSE(::IsWindowVisible(legacy)); EXPECT_TRUE(::IsWindowVisible(detail)); EXPECT_EQ(detail, ::GetFocus());
		ASSERT_EQ(SenpSurfaceProjection::Applied, native->Show("legacy"));
		EXPECT_TRUE(::IsWindowVisible(legacy)); EXPECT_FALSE(::IsWindowVisible(detail)); EXPECT_EQ(legacy, ::GetFocus());
		EXPECT_EQ(before, Text(legacy)); EXPECT_EQ(firstLine, ::SendMessageW(legacy, EM_GETFIRSTVISIBLELINE, 0, 0));
		EXPECT_EQ(selection, ::SendMessageW(legacy, EM_GETSEL, 0, 0));
		EXPECT_TRUE(::SendMessageW(legacy, EM_GETMODIFY, 0, 0)); EXPECT_TRUE(::SendMessageW(legacy, EM_CANUNDO, 0, 0));
	}
	EXPECT_TRUE(::SendMessageW(legacy, WM_UNDO, 0, 0)); EXPECT_EQ(text, Text(legacy));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeMissingSurfaceDoesNotChangeTheActiveInput)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	const auto missing = model.Open(scope, L"not-prepared", L"Not prepared");
	const auto before = core.Snapshot();
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Show(missing.inputId));
	EXPECT_EQ(before.revision, core.Snapshot().revision); EXPECT_EQ(before.group.activeInputId, core.Snapshot().group.activeInputId);
	EXPECT_FALSE(native->Bind(missing.inputId, legacy));
	EXPECT_FALSE(native->Bind("not-in-core", detail));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Show(missing.inputId));
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Apply());
	EXPECT_FALSE(::IsWindowVisible(legacy)); EXPECT_FALSE(::IsWindowVisible(detail));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeDestroyedSurfaceCannotRevealTheRetainedLegacyDocument)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	ASSERT_TRUE(::DestroyWindow(detail));
	const auto before = core.Snapshot();
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Show(detailId));
	EXPECT_EQ(before.revision, core.Snapshot().revision);
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Show(detailId));
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Apply());
	EXPECT_FALSE(::IsWindowVisible(legacy));
	EXPECT_TRUE(native->Unbind(detailId));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, model.Close(detailId));
	EXPECT_EQ(SenpSurfaceProjection::Applied, native->Apply(true));
	EXPECT_TRUE(::IsWindowVisible(legacy)); ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeClosePrunesProjectionAndReturnsFocusWithoutDestroyingTheEditor)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	ASSERT_EQ(SenpSurfaceProjection::Applied, native->Show(detailId));
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Close(detailId));
	EXPECT_EQ(SenpSurfaceProjection::Applied, native->Apply(true));
	EXPECT_TRUE(::IsWindow(legacy)); EXPECT_TRUE(::IsWindow(detail));
	EXPECT_TRUE(::IsWindowVisible(legacy)); EXPECT_FALSE(::IsWindowVisible(detail)); EXPECT_EQ(legacy, ::GetFocus());
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Show(detailId));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativePanelVisibilityAndResizeKeepTheSelectedInput)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	ASSERT_EQ(SenpSurfaceProjection::Applied, native->Show(detailId));
	native->SetVisible(false);
	EXPECT_FALSE(::IsWindowVisible(detail)); EXPECT_FALSE(::IsWindowVisible(legacy));
	EXPECT_EQ(detailId, *core.Snapshot().group.activeInputId);
	ASSERT_EQ(SenpSurfaceProjection::Applied, native->Layout({ 12, 24, 420, 260 }));
	native->SetVisible(true);
	EXPECT_TRUE(::IsWindowVisible(detail)); EXPECT_FALSE(::IsWindowVisible(legacy));
	RECT rect{}; ASSERT_TRUE(::GetWindowRect(detail, &rect));
	::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
	EXPECT_EQ(12, rect.left); EXPECT_EQ(24, rect.top); EXPECT_EQ(420, rect.right); EXPECT_EQ(260, rect.bottom);
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Layout({ 100, 0, 0, 20 }));
	EXPECT_EQ(detailId, *core.Snapshot().group.activeInputId);
}

TEST_F(SenpReadonlyWorkbench, NativeTeardownRetainsBorrowedWindowsAndCannotReselect)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	native->Close(); native->Close();
	EXPECT_EQ(SenpSurfaceProjection::Closed, native->Show(detailId));
	EXPECT_EQ(SenpSurfaceProjection::Closed, native->Apply());
	EXPECT_TRUE(::IsWindow(legacy)); EXPECT_TRUE(::IsWindow(detail));
	EXPECT_FALSE(::IsWindowVisible(legacy)); EXPECT_FALSE(::IsWindowVisible(detail));
	EXPECT_EQ("legacy", *core.Snapshot().group.activeInputId); ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeCloseDuringProjectionFinishesAfterTheCallbackReturns)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	ASSERT_TRUE(::SetWindowSubclass(detail, Hook, 1, reinterpret_cast<DWORD_PTR>(this)));
	nativeHook = 1;
	EXPECT_EQ(SenpSurfaceProjection::Closed, native->Show(detailId));
	EXPECT_FALSE(::IsWindowVisible(legacy)); EXPECT_FALSE(::IsWindowVisible(detail));
	EXPECT_EQ(SenpSurfaceProjection::Closed, native->Apply());
	EXPECT_EQ(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
	EXPECT_EQ(nullptr, ::GetPropW(legacy, L"Sakura.Senp.EditorSurfaceOwner"));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, NativeUnbindDuringProjectionKeepsOwnershipUntilTheExplicitRetry)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	ASSERT_TRUE(::SetWindowSubclass(detail, Hook, 1, reinterpret_cast<DWORD_PTR>(this)));
	nativeHook = 2;
	EXPECT_EQ(SenpSurfaceProjection::Applied, native->Show(detailId));
	EXPECT_FALSE(unbindAccepted);
	EXPECT_NE(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
	EXPECT_TRUE(native->Unbind(detailId));
	EXPECT_EQ(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
	EXPECT_FALSE(::IsWindowVisible(detail)); EXPECT_TRUE(::IsWindow(detail));
	EXPECT_EQ(SenpSurfaceProjection::Unavailable, native->Apply());
}

TEST_F(SenpReadonlyWorkbench, NativeSurfacesHaveOneOwnerAndUnchangedSnapshotsDoNotRepaint)
{
	CreateNative(); ASSERT_NE(nullptr, native);
	SenpEditorSurfaceSwitcher competitor(model, parent);
	EXPECT_FALSE(competitor.Bind("legacy", legacy));
	EXPECT_FALSE(competitor.Bind(detailId, detail));
	ASSERT_TRUE(::SetWindowSubclass(detail, Hook, 1, reinterpret_cast<DWORD_PTR>(this)));
	ASSERT_EQ(SenpSurfaceProjection::Applied, native->Show(detailId));
	positionMessages = 0; paintMessages = 0;
	for (int i = 0; i < 20; ++i) EXPECT_EQ(SenpSurfaceProjection::Applied, native->Apply());
	EXPECT_EQ(0, positionMessages); EXPECT_EQ(0, paintMessages);
	EXPECT_TRUE(native->Unbind("legacy"));
	EXPECT_TRUE(competitor.Bind("legacy", legacy));
	competitor.Close();
}

TEST_F(SenpReadonlyWorkbench, RealCEditDocTextAndUndoBufferSurviveReadonlyLifetime)
{
	auto document = std::make_unique<CEditDoc>(nullptr);
	CDocEditAgent editing(&document->m_cDocLineMgr);
	editing.AddLineStrX(L"unsaved source", 14);
	document->m_cDocEditor.SetModified(true, false);
	auto operation = std::make_unique<CInsertOpe>();
	operation->m_ptCaretPos_PHY_Before = CLogicPoint(CLogicInt(0), CLogicInt(0));
	operation->m_ptCaretPos_PHY_After = CLogicPoint(CLogicInt(14), CLogicInt(0));
	auto block = std::make_unique<COpeBlk>();
	ASSERT_TRUE(block->AppendOpe(operation.get())); (void)operation.release();
	auto* const originalBlock = block.get();
	ASSERT_TRUE(document->m_cDocEditor.m_cOpeBuf.AppendOpeBlk(block.get())); (void)block.release();
	const auto pointer = document->m_cDocEditor.m_cOpeBuf.GetCurrentPointer();
	const auto detailInput = model.Open(scope, L"body", L"Body");
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Show(detailInput.inputId));
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Show("legacy"));
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, model.Close(detailInput.inputId));
	EXPECT_TRUE(document->m_cDocEditor.IsModified());
	ASSERT_NE(nullptr, document->m_cDocLineMgr.GetDocLineTop());
	EXPECT_EQ(L"unsaved source", std::wstring(document->m_cDocLineMgr.GetDocLineTop()->GetPtr(), 14));
	EXPECT_EQ(pointer, document->m_cDocEditor.m_cOpeBuf.GetCurrentPointer());
	EXPECT_TRUE(document->m_cDocEditor.m_cOpeBuf.IsEnableUndo());
	bool modified{};
	EXPECT_EQ(originalBlock, document->m_cDocEditor.m_cOpeBuf.DoUndo(&modified));
	EXPECT_TRUE(document->m_cDocEditor.m_cOpeBuf.IsEnableRedo());
	EXPECT_EQ(originalBlock, document->m_cDocEditor.m_cOpeBuf.DoRedo(&modified));
	ExpectLegacy();
}

TEST_F(SenpReadonlyWorkbench, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1')
		GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet ReadonlyEditors";
	CreateNative(); ASSERT_NE(nullptr, native);
	::SetWindowTextW(parent, L"SENP native readonly Editor verification");
	::SetWindowTextW(legacy, L"Unsaved source document\r\n\r\nThis editor and its undo buffer stay alive.\r\nSwitch to the readonly detail and return.\r\nSelection and scroll belong to each retained surface.");
	::SetWindowTextW(detail, L"Issue 7: retain the source document\r\n\r\nReadonly editor input\r\nOwner: sample.details\r\n\r\nThe content renderer is an independent capability.\r\nThe input never replaces CEditDoc text.");
	ASSERT_TRUE(::SetWindowSubclass(parent, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 180000;
	while (!probeDone && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG message{};
		for (int i = 0; i < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
			::TranslateMessage(&message); ::DispatchMessageW(&message);
		}
	}
	::RemoveWindowSubclass(parent, ProbeProcedure, 296); EXPECT_TRUE(probeDone);
}

} // namespace
} // namespace workbench::editor::tests
