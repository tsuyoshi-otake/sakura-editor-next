/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <tuple>

#include "env/CommonSetting.h"

TEST(WorkbenchSettings, MigratesLegacyOutlineExactlyOnce)
{
	CommonSetting_OutLine outline{};
	outline.m_bOutlineDockDisp = TRUE;
	outline.m_cxOutlineDockRight = 384;

	CommonSetting_Workbench workbench{};
	workbench.m_bRightPanelVisible = FALSE;
	workbench.m_nRightPanelExtent96 = 260;
	workbench.m_bOutlineMigrationComplete = FALSE;

	EXPECT_TRUE(MigrateOutlineToWorkbench(outline, workbench));
	EXPECT_TRUE(workbench.m_bRightPanelVisible);
	EXPECT_EQ(384, workbench.m_nRightPanelExtent96);
	EXPECT_TRUE(workbench.m_bOutlineMigrationComplete);

	outline.m_bOutlineDockDisp = FALSE;
	outline.m_cxOutlineDockRight = 640;
	EXPECT_FALSE(MigrateOutlineToWorkbench(outline, workbench));
	EXPECT_TRUE(workbench.m_bRightPanelVisible);
	EXPECT_EQ(384, workbench.m_nRightPanelExtent96);
}

TEST(WorkbenchSettings, UsesLegacyFloatingOutlineWidthWhenUndocked)
{
	CommonSetting_OutLine outline{};
	outline.m_bOutlineDockDisp = TRUE;
	outline.m_cxOutlineDockRight = 0;
	outline.m_widthOutlineWindow = 312;

	CommonSetting_Workbench workbench{};
	workbench.m_nRightPanelExtent96 = 260;
	workbench.m_bOutlineMigrationComplete = FALSE;

	EXPECT_TRUE(MigrateOutlineToWorkbench(outline, workbench));
	EXPECT_EQ(312, workbench.m_nRightPanelExtent96);
}

TEST(WorkbenchSettings, PreservesLegacyPhysicalWidthAcrossDpiMigration)
{
	for (const auto [sourceDpi, legacyPixels, expectedExtent96] : {
		std::tuple<unsigned int, int, int>{96, 384, 384},
		std::tuple<unsigned int, int, int>{144, 384, 256},
		std::tuple<unsigned int, int, int>{192, 384, 192},
	}) {
		CommonSetting_OutLine outline{};
		outline.m_bOutlineDockDisp = TRUE;
		outline.m_cxOutlineDockRight = legacyPixels;

		CommonSetting_Workbench workbench{};
		workbench.m_nRightPanelExtent96 = 260;
		workbench.m_bOutlineMigrationComplete = FALSE;

		ASSERT_TRUE(MigrateOutlineToWorkbench(outline, workbench, sourceDpi));
		EXPECT_EQ(expectedExtent96, workbench.m_nRightPanelExtent96) << "source DPI " << sourceDpi;
		EXPECT_EQ(legacyPixels, static_cast<int>(workbench.m_nRightPanelExtent96 * sourceDpi / 96))
			<< "source DPI " << sourceDpi;
	}
}

TEST(WorkbenchSettings, Uses96DpiWhenTheMigrationSourceDpiIsUnavailable)
{
	CommonSetting_OutLine outline{};
	outline.m_bOutlineDockDisp = TRUE;
	outline.m_cxOutlineDockRight = 384;

	CommonSetting_Workbench workbench{};
	workbench.m_nRightPanelExtent96 = 260;
	workbench.m_bOutlineMigrationComplete = FALSE;

	ASSERT_TRUE(MigrateOutlineToWorkbench(outline, workbench, 0));
	EXPECT_EQ(384, workbench.m_nRightPanelExtent96);
}
