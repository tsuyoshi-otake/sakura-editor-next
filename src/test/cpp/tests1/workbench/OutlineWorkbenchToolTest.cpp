/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "outline/CDlgFuncList.h"
#include "outline/CFuncInfo.h"
#include "outline/CFuncInfoArr.h"
#include "sakura_rc.h"
#include "workbench/outline/COutlineWorkbenchTool.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace workbench::outline {
namespace {

template <class T>
size_t AppendTemplateValue( std::vector<std::byte>& data, T value )
{
	const size_t offset = data.size();
	const auto* bytes = reinterpret_cast<const std::byte*>(&value);
	data.insert( data.end(), bytes, bytes + sizeof(value) );
	return offset;
}

void AppendTemplateString( std::vector<std::byte>& data, const wchar_t* value )
{
	do {
		AppendTemplateValue<WORD>( data, static_cast<WORD>(*value) );
	} while( *value++ != L'\0' );
}

void AlignTemplateDword( std::vector<std::byte>& data )
{
	while( (reinterpret_cast<std::uintptr_t>(data.data()) + data.size()) % 4u != 0u ){
		data.push_back(std::byte{});
	}
}

struct TemplateItemOffsets {
	size_t style;
	size_t exStyle;
};

TemplateItemOffsets AppendStandardTemplateItem(
	std::vector<std::byte>& data, WORD id, DWORD style, DWORD exStyle )
{
	AlignTemplateDword(data);
	const size_t styleOffset = AppendTemplateValue<DWORD>(data, style);
	const size_t exStyleOffset = AppendTemplateValue<DWORD>(data, exStyle);
	for( int i = 0; i < 4; ++i ) AppendTemplateValue<short>(data, 0);
	AppendTemplateValue<WORD>(data, id);
	AppendTemplateValue<WORD>(data, 0xffff);
	AppendTemplateValue<WORD>(data, 0x0080);
	AppendTemplateString(data, L"");
	AppendTemplateValue<WORD>(data, 0);
	return { styleOffset, exStyleOffset };
}

template <class T>
T ReadTemplateValue( const std::vector<std::byte>& data, size_t offset )
{
	T value{};
	std::memcpy( &value, data.data() + offset, sizeof(value) );
	return value;
}

TEST(OutlineWorkbenchTool, LifecycleHasExplicitTerminalState)
{
	auto state = OutlineToolLifecycle::Idle;
	EXPECT_EQ( OutlineToolLifecycle::Idle,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Activate) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Create );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	EXPECT_EQ( OutlineToolLifecycle::Inactive,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Create) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Activate );
	EXPECT_EQ( OutlineToolLifecycle::Active, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Deactivate );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Close );
	EXPECT_EQ( OutlineToolLifecycle::Closed, state );
	for( const auto event : { OutlineToolEvent::Create, OutlineToolEvent::Activate,
		OutlineToolEvent::Deactivate, OutlineToolEvent::Close } ) {
		EXPECT_EQ( OutlineToolLifecycle::Closed, AdvanceOutlineToolLifecycle(state, event) );
	}
}

TEST(OutlineWorkbenchTool, GeometryClampsInvalidBoundsAndDefaultsDpi)
{
	const auto layout = NormalizeOutlineToolLayout( RECT{ -20, -10, -40, -30 }, 0 );
	EXPECT_EQ( (OutlineToolLayout{ RECT{ 0, 0, 0, 0 }, 96 }), layout );
}

TEST(OutlineWorkbenchTool, GeometryPreservesHostCoordinatesAtPerMonitorDpi)
{
	const RECT expected{ 3, 31, 403, 731 };
	const auto layout = NormalizeOutlineToolLayout( expected, 192 );
	EXPECT_EQ( (OutlineToolLayout{ expected, 192 }), layout );
}

TEST(OutlineWorkbenchTool, CreatedInactiveDialogIsVisibleWithoutActivation)
{
	// A SHOW_RELOAD can create the child after the visible host's previous layout.
	// Visibility is independent from keyboard activation so editor focus is retained.
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Idle) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Inactive) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Active) );
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Closed) );
}

TEST(OutlineWorkbenchTool, ReloadRelayoutRequiresSuccessfulVisibleChildCreation)
{
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(false, true, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, false, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, true, false) );
	EXPECT_TRUE( ShouldRelayoutOutlineAfterReload(true, true, true) );
}

TEST(OutlineWorkbenchTool, NestedExplorerViewUsesSideBarBackgroundForEverySurface)
{
	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.panel = { 0x10, 0x20, 0x30 };
	palette.sideBar = { 0x40, 0x50, 0x60 };
	EXPECT_EQ(palette.sideBar.ToColorRef(), OutlineBackgroundColor(palette));
}

TEST(OutlineWorkbenchTool, SymbolKindsUseCanonicalCodiconImageSlots)
{
	EXPECT_EQ(10, CDlgFuncList::WorkbenchSymbolImageCount());
	EXPECT_EQ(0, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DEFINITION));
	EXPECT_EQ(1, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DECLARE));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION));
	EXPECT_EQ(3, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_CLASS));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_STRUCT));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_UNION));
	EXPECT_EQ(5, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ENUM));
	EXPECT_EQ(6, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_NAMESPACE));
	EXPECT_EQ(7, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_INTERFACE));
	EXPECT_EQ(8, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_GLOBAL));
	EXPECT_EQ(9, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ELEMENT_MAX));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION | FUNCINFO_NOCLIPTEXT));
}

TEST(OutlineWorkbenchTool, SymbolImageSlotsResolveToCodiconNames)
{
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(0));
	EXPECT_EQ(L"symbol-method", CDlgFuncList::WorkbenchSymbolCodiconName(1));
	EXPECT_EQ(L"symbol-function", CDlgFuncList::WorkbenchSymbolCodiconName(2));
	EXPECT_EQ(L"symbol-class", CDlgFuncList::WorkbenchSymbolCodiconName(3));
	EXPECT_EQ(L"symbol-structure", CDlgFuncList::WorkbenchSymbolCodiconName(4));
	EXPECT_EQ(L"symbol-enum", CDlgFuncList::WorkbenchSymbolCodiconName(5));
	EXPECT_EQ(L"symbol-namespace", CDlgFuncList::WorkbenchSymbolCodiconName(6));
	EXPECT_EQ(L"symbol-interface", CDlgFuncList::WorkbenchSymbolCodiconName(7));
	EXPECT_EQ(L"symbol-variable", CDlgFuncList::WorkbenchSymbolCodiconName(8));
	EXPECT_EQ(L"symbol-property", CDlgFuncList::WorkbenchSymbolCodiconName(9));
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(99));
}

TEST(OutlineWorkbenchTool, ChildGeometryExactlyFillsClientWithoutBorder)
{
	EXPECT_EQ((OutlineChildLayout{RECT{0, 0, 640, 360}, false}),
		MakeOutlineChildLayout(640, 360));
	EXPECT_EQ((OutlineChildLayout{RECT{0, 0, 0, 0}, false}),
		MakeOutlineChildLayout(-10, -20));
}

TEST(OutlineWorkbenchTool, TwentyCollapseReopenCyclesRetainCachedModelWithoutRefresh)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion document{1, 7};
	coordinator.AdoptCommitted(document);
	for( int cycle = 0; cycle < 20; ++cycle ){
		coordinator.SetVisible(false);
		EXPECT_TRUE(coordinator.Snapshot().hasCommittedModel);
		coordinator.SetVisible(true);
		EXPECT_EQ(OutlineRefreshRequestStatus::Cached, coordinator.Request(document).status);
	}

	const auto snapshot = coordinator.Snapshot();
	EXPECT_TRUE(snapshot.visible);
	EXPECT_TRUE(snapshot.hasCommittedModel);
	EXPECT_EQ(document, snapshot.committedVersion);
	EXPECT_EQ(0u, snapshot.startedCount);
	EXPECT_EQ(20u, snapshot.cachedCount);
}

TEST(OutlineWorkbenchTool, RefreshStartsOncePerDocumentVersionAndDeduplicatesInFlightWork)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion firstVersion{4, 10};
	const auto first = coordinator.Request(firstVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, first.status);
	const auto duplicate = coordinator.Request(firstVersion);
	EXPECT_EQ(OutlineRefreshRequestStatus::InFlight, duplicate.status);
	EXPECT_EQ(first.generation, duplicate.generation);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(first.generation, true, firstVersion));
	EXPECT_EQ(OutlineRefreshRequestStatus::Cached, coordinator.Request(firstVersion).status);

	constexpr OutlineDocumentVersion secondVersion{4, 11};
	const auto second = coordinator.Request(secondVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, second.status);
	EXPECT_EQ(OutlineRefreshRequestStatus::InFlight, coordinator.Request(secondVersion).status);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(second.generation, true, secondVersion));

	const auto snapshot = coordinator.Snapshot();
	EXPECT_EQ(2u, snapshot.startedCount);
	EXPECT_EQ(2u, snapshot.committedCount);
	EXPECT_EQ(2u, snapshot.deduplicatedCount);
}

TEST(OutlineWorkbenchTool, SupersededAndDocumentChangedGenerationsAreDiscarded)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion firstVersion{9, 1};
	constexpr OutlineDocumentVersion secondVersion{9, 2};
	const auto first = coordinator.Request(firstVersion);
	const auto second = coordinator.Request(secondVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, first.status);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, second.status);
	EXPECT_EQ(OutlineRefreshCompletion::Stale,
		coordinator.Complete(first.generation, true, firstVersion));
	EXPECT_TRUE(coordinator.Snapshot().refreshInFlight);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(second.generation, true, secondVersion));

	constexpr OutlineDocumentVersion thirdVersion{9, 3};
	constexpr OutlineDocumentVersion changedWhileParsing{9, 4};
	const auto third = coordinator.Request(thirdVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, third.status);
	EXPECT_EQ(OutlineRefreshCompletion::Stale,
		coordinator.Complete(third.generation, true, changedWhileParsing));
	const auto stale = coordinator.Snapshot();
	EXPECT_FALSE(stale.refreshInFlight);
	EXPECT_TRUE(stale.hiddenRefreshPending);
	EXPECT_EQ(changedWhileParsing, stale.pendingVersion);
	EXPECT_EQ(secondVersion, stale.committedVersion);
}

TEST(OutlineWorkbenchTool, HiddenVersionChangeDefersWorkUntilVisibleAndCloseIsTerminal)
{
	OutlineRefreshCoordinator coordinator;
	coordinator.AdoptCommitted({12, 1});
	coordinator.SetVisible(false);
	EXPECT_EQ(OutlineRefreshRequestStatus::HiddenPending,
		coordinator.Request({12, 2}).status);
	EXPECT_EQ(0u, coordinator.Snapshot().startedCount);
	coordinator.SetVisible(true);
	EXPECT_EQ(OutlineRefreshRequestStatus::Started, coordinator.Request({12, 2}).status);
	coordinator.Close();
	EXPECT_EQ(OutlineRefreshRequestStatus::Closed, coordinator.Request({12, 3}).status);
	EXPECT_EQ(OutlineRefreshCompletion::Closed, coordinator.Complete(1, true, {12, 3}));
}

TEST(OutlineWorkbenchTool, NativeOutlineControlsAreBornWithoutNonClientBorders)
{
	std::vector<std::byte> dialog;
	dialog.reserve(256);
	AppendTemplateValue<DWORD>(dialog, WS_POPUP);
	AppendTemplateValue<DWORD>(dialog, 0);
	AppendTemplateValue<WORD>(dialog, 2);
	for( int i = 0; i < 4; ++i ) AppendTemplateValue<short>(dialog, 0);
	AppendTemplateString(dialog, L"");
	AppendTemplateString(dialog, L"");
	AppendTemplateString(dialog, L"");

	constexpr DWORD edgeStyles = WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE;
	const auto tree = AppendStandardTemplateItem(
		dialog, IDC_TREE_FL, WS_CHILD | WS_BORDER | TVS_HASLINES | TVS_SHOWSELALWAYS, edgeStyles );
	const auto list = AppendStandardTemplateItem(
		dialog, IDC_LIST_FL, WS_CHILD | WS_BORDER | LVS_REPORT, edgeStyles );

	ASSERT_TRUE( NormalizeWorkbenchOutlineDialogTemplate(dialog.data(), dialog.size()) );
	const DWORD treeStyle = ReadTemplateValue<DWORD>(dialog, tree.style);
	EXPECT_EQ(0u, treeStyle & (WS_BORDER | TVS_HASLINES | TVS_SHOWSELALWAYS));
	EXPECT_EQ(static_cast<DWORD>(TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT),
		treeStyle & (TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT));
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, tree.exStyle) & edgeStyles);
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, list.style) & WS_BORDER);
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, list.exStyle) & edgeStyles);
}

TEST(OutlineWorkbenchTool, MalformedDialogTemplateFailsClosed)
{
	std::vector<std::byte> malformed(4);
	EXPECT_FALSE( NormalizeWorkbenchOutlineDialogTemplate(malformed.data(), malformed.size()) );
}

} // namespace
} // namespace workbench::outline
