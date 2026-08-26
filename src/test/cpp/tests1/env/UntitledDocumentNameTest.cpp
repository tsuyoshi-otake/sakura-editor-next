/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <string>

#include "env/CAppNodeManager.h"
#include "env/CFileNameManager.h"
#include "env/CShareData.h"
#include "env/DLLSHAREDATA.h"

#include "ShareDataTestSuite.hpp"

namespace {

//! The untitled editor's own name, without a number.
std::wstring BaseName()
{
	return GetUntitledDocumentName(0);
}

TEST(UntitledDocumentName, NumbersTheEditorTheWayVSCodeDoes)
{
	//	VS Code builds `untitled:Untitled-<n>` and shows the URI's basename, so the
	//	number is part of the editor's name rather than a suffix a caption appends.
	EXPECT_EQ(BaseName() + L"-1", GetUntitledDocumentName(1));
	EXPECT_EQ(BaseName() + L"-2", GetUntitledDocumentName(2));
	EXPECT_EQ(BaseName() + L"-12", GetUntitledDocumentName(12));
}

TEST(UntitledDocumentName, AnUnnumberedEditorKeepsTheBareName)
{
	//	Zero is what EditNode::GetSafeId returns for "no node", and a node that has
	//	not been given a number yet also holds zero. Neither may print `Untitled-0`.
	EXPECT_EQ(BaseName(), GetUntitledDocumentName(0));
	EXPECT_EQ(BaseName(), GetUntitledDocumentName(-1));
}

//! Owns the shared data once, and restores the node array around every test so
//! one test's windows never leak into the next.
class UntitledNumberAllocation : public ::testing::Test
{
protected:
	static void SetUpTestSuite() { env::ShareDataTestSuite::SetUpShareData(); }
	static void TearDownTestSuite() { env::ShareDataTestSuite::TearDownShareData(); }

	void SetUp() override
	{
		nodes = &GetDllShareData().m_sNodes;
		saved = *nodes;
		nodes->m_nEditArrNum = 0;
	}

	void TearDown() override { *nodes = saved; }

	//! Adds an untitled editor holding @a nNumber.
	EditNode& AddUntitled(int nNumber)
	{
		EditNode& node = nodes->m_pEditArr[nodes->m_nEditArrNum++];
		node = EditNode{};
		node.m_nId = nNumber;
		node.m_szFilePath[0] = 0;
		return node;
	}

	//! Adds a saved editor, which owns a path and therefore no untitled number.
	EditNode& AddSaved(int nNumber)
	{
		EditNode& node = AddUntitled(nNumber);
		wcscpy_s(node.m_szFilePath, L"C:/work/saved.txt");
		return node;
	}

	SShare_Nodes* nodes = nullptr;
	SShare_Nodes saved{};
};

TEST_F(UntitledNumberAllocation, AnOpenUntitledEditorOwnsItsNumber)
{
	AddUntitled(1);

	EXPECT_FALSE(CAppNodeManager::IsUntitledNumberFree(1, nullptr));
	EXPECT_TRUE(CAppNodeManager::IsUntitledNumberFree(2, nullptr));
}

TEST_F(UntitledNumberAllocation, ClosingTheLowestEditorFreesItsNumberAgain)
{
	//	VS Code hands out the lowest free number, so closing Untitled-1 while
	//	Untitled-2 stays open makes the next new file Untitled-1, not Untitled-3.
	AddUntitled(2);

	EXPECT_TRUE(CAppNodeManager::IsUntitledNumberFree(1, nullptr));
	EXPECT_FALSE(CAppNodeManager::IsUntitledNumberFree(2, nullptr));
	EXPECT_TRUE(CAppNodeManager::IsUntitledNumberFree(3, nullptr));
}

TEST_F(UntitledNumberAllocation, ASavedEditorNoLongerHoldsItsNumber)
{
	//	Once a document has a path it is named by that path, so the number it used
	//	to carry goes back into the pool.
	AddSaved(1);

	EXPECT_TRUE(CAppNodeManager::IsUntitledNumberFree(1, nullptr));
}

TEST_F(UntitledNumberAllocation, AnEditorDoesNotBlockTheNumberItAlreadyHolds)
{
	//	The excluded node is the one asking. Without the exclusion an open editor
	//	would find its own number taken and rename itself on every query.
	EditNode& self = AddUntitled(1);

	EXPECT_TRUE(CAppNodeManager::IsUntitledNumberFree(1, &self));
	EXPECT_FALSE(CAppNodeManager::IsUntitledNumberFree(1, nullptr));
}

TEST_F(UntitledNumberAllocation, TheFirstGapIsTheNumberTheNextEditorTakes)
{
	AddUntitled(1);
	AddUntitled(3);
	AddSaved(2);

	int nNumber = 1;
	while (!CAppNodeManager::IsUntitledNumberFree(nNumber, nullptr)) {
		nNumber++;
	}
	EXPECT_EQ(2, nNumber);
}

} // namespace
