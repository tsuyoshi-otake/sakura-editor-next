/*! @file */
/*
	Copyright (C) 2021-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <algorithm>
#include "util/zoom.h"
#include "view/MiniMapOverview.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/WorkbenchZoom.h"

using namespace std::string_literals;

TEST( WorkbenchZoom, StepsClampsResetsAndScalesDpi )
{
	EXPECT_EQ(110, workbench::AdjustZoomPercent(100, 1));
	EXPECT_EQ(90, workbench::AdjustZoomPercent(100, -1));
	EXPECT_EQ(100, workbench::AdjustZoomPercent(170, 0));
	EXPECT_EQ(200, workbench::AdjustZoomPercent(200, 1));
	EXPECT_EQ(70, workbench::AdjustZoomPercent(70, -1));
	EXPECT_EQ(144U, workbench::ScaleDpi(120, 120));
}

TEST( GitScmModel, ParsesBranchAheadBehindAndChanges )
{
	const std::string status = "# branch.head feature/test\0# branch.upstream origin/feature/test\0"
		"# branch.ab +2 -3\0? new.txt\0"
		"1 M. N... 100644 100644 100644 abcdef abcdef src/file.cpp\0"s;
	const auto state = workbench::scm::ParsePorcelainV2(status);
	EXPECT_TRUE(state.repository);
	EXPECT_EQ(L"feature/test", state.branch);
	EXPECT_EQ(2, state.ahead);
	EXPECT_EQ(3, state.behind);
	ASSERT_EQ(2U, state.changes.size());
	EXPECT_EQ(L"new.txt", state.changes[0].path);
	EXPECT_EQ(L"src/file.cpp", state.changes[1].path);
	EXPECT_NE(std::wstring::npos, workbench::scm::FormatStatusLine(state).find(L"2 changes"));
}

TEST( MiniMapOverview, MapsEntireDocumentAndViewportConsistently )
{
	EXPECT_EQ(0, minimap::LineToPixel(0, 1000, 200));
	EXPECT_EQ(100, minimap::LineToPixel(500, 1000, 200));
	EXPECT_EQ(200, minimap::LineToPixel(1000, 1000, 200));
	EXPECT_EQ(0, minimap::PixelToLine(-1, 1000, 200));
	EXPECT_EQ(500, minimap::PixelToLine(100, 1000, 200));
	EXPECT_EQ(995, minimap::PixelToLine(199, 1000, 200));

	const auto viewport = minimap::ViewportToPixels(500, 550, 1000, 200);
	EXPECT_EQ(100, viewport.top);
	EXPECT_EQ(110, viewport.bottom);
	const auto onePixelViewport = minimap::ViewportToPixels(4, 5, 100000, 200);
	EXPECT_EQ(onePixelViewport.top + 1, onePixelViewport.bottom);
}

/*!
	@brief 設定値の正当性判定
*/
bool ZoomSettingIsValid( const ZoomSetting& zoomSetting )
{
	return (zoomSetting.m_nValueMin <= zoomSetting.m_nValueMax)
		&& (0.0 <= zoomSetting.m_nValueUnit)
		&& std::is_sorted( zoomSetting.m_vZoomFactors.begin(), zoomSetting.m_vZoomFactors.end() );
}

/*!
 * @brief ZoomSetting構造体のテスト
 */
TEST( zoom, ZoomSetting )
{
	// 不正な引数 - テーブルが昇順になっていない
	EXPECT_EQ( false, ZoomSettingIsValid( ZoomSetting( {0.0, 2.0, 1.0}, 0.0, 0.0, 1.0 ) ) );

	// 不正な引数 - 最大最小があべこべ
	EXPECT_EQ( false, ZoomSettingIsValid( ZoomSetting( {1.0}, 1.0, 0.0, 1.0 ) ) );

	// 不正な引数 - 解像度が負数
	EXPECT_EQ( false, ZoomSettingIsValid( ZoomSetting( {1.0}, 0.0, 0.0, -1.0 ) ) );

	// 正しい引数 - テーブルに同一値が含まれる
	EXPECT_EQ( true, ZoomSettingIsValid( ZoomSetting( {0.0, 1.0, 1.0, 2.0}, 0.0, 0.0, 1.0 ) ) );

	// 正しい引数 - 解像度が0
	EXPECT_EQ( true, ZoomSettingIsValid( ZoomSetting( {0.0}, 0.0, 0.0, 0.0 ) ) );
}

/*!
 * @brief GetZoomedValue関数のテスト - 引数チェック
 */
TEST( zoom, GetZoomedValue_CheckArguments )
{
	// ズーム設定が不正
	EXPECT_EQ( false, GetZoomedValue( ZoomSetting( {1.0}, 1.0, 0.0, 0.0 ), 100.0, 1.0, 1, nullptr, nullptr ) );

	// ズームステップが0
	EXPECT_EQ( false, GetZoomedValue( ZoomSetting( {0.5, 1.0, 1.5}, 0.0, 200.0, 0.0 ), 100.0, 1.0, 0, nullptr, nullptr ) );

	// 出力引数はnullptrを許容
	EXPECT_EQ( true, GetZoomedValue( ZoomSetting( {0.5, 1.0, 1.5}, 0.0, 200.0, 0.0 ), 100.0, 1.0, 1, nullptr, nullptr ) );
}

/*!
 * @brief GetZoomedValue関数のテスト - 基本的な拡大/縮小
 */
TEST( zoom, GetZoomedValue_ZoomUpDown )
{
	const ZoomSetting setting_50_150_10( {0.1, 0.9, 0.95, 1.0, 1.05, 1.1, 2.0}, 50.0, 150.0, 10.0 );
	const ZoomSetting setting_5_300_10( {0.1, 0.9, 0.95, 1.0, 1.05, 1.1, 2.0}, 5.0, 300.0, 10.0 );
	const ZoomSetting setting_5_300_0( {0.1, 0.9, 0.95, 1.0, 1.05, 1.1, 2.0}, 5.0, 300.0, 0.0 );
	double nValue = 0.0;
	double nZoom = 0.0;

	// 拡大 - 変化が最小単位以上となるまで拡大(2ステップ分)
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, 1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 110.0, nValue );
	EXPECT_DOUBLE_EQ( 1.1, nZoom );

	// 拡大 - 指定通り
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, 2, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 110.0, nValue );
	EXPECT_DOUBLE_EQ( 1.1, nZoom );

	// 拡大 - 最大値による丸め込み
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, 3, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 150.0, nValue );
	EXPECT_DOUBLE_EQ( 1.5, nZoom );

	// 拡大 - ズームテーブル末尾の倍率を適用
	EXPECT_EQ( true, GetZoomedValue( setting_5_300_10, 100.0, 1.0, 4, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 200.0, nValue );
	EXPECT_DOUBLE_EQ( 2.0, nZoom );

	// 縮小 - 変化が最小単位以上となるまで縮小(2ステップ分)
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, -1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 90.0, nValue );
	EXPECT_DOUBLE_EQ( 0.9, nZoom );

	// 縮小 - 指定通り
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, -2, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 90.0, nValue );
	EXPECT_DOUBLE_EQ( 0.9, nZoom );

	// 縮小 - 最小値による丸め込み
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 1.0, -3, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 50.0, nValue );
	EXPECT_DOUBLE_EQ( 0.5, nZoom );

	// 縮小 - ズームテーブル先頭の倍率を適用
	EXPECT_EQ( true, GetZoomedValue( setting_5_300_10, 100.0, 1.0, -4, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 10.0, nValue );
	EXPECT_DOUBLE_EQ( 0.1, nZoom );

	// 拡大 - 最小単位なし
	EXPECT_EQ( true, GetZoomedValue( setting_5_300_0, 1.1, 1.0, 1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( nValue, 1.155 );
	EXPECT_DOUBLE_EQ( nZoom, 1.05 );
}

/*!
 * @brief GetZoomedValue関数のテスト - 基準値が上限/下限の範囲外
 */
TEST( zoom, GetZoomedValue_OutOfRange )
{
	const ZoomSetting setting_50_150_10( {0.1, 0.9, 0.95, 1.0, 1.05, 1.1, 2.0}, 50.0, 150.0, 10.0 );
	double nValue = 0.0;
	double nZoom = 0.0;

	// 上限値を超える値から拡大 -> ズーム不可
	EXPECT_EQ( false, GetZoomedValue( setting_50_150_10, 200.0, 1.0, 1, &nValue, &nZoom ) );

	// 上限値を超える値から縮小 -> 上限値を上回る状態で縮小
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 200.0, 1.0, -1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 190.0, nValue );
	EXPECT_DOUBLE_EQ( 0.95, nZoom );

	// 下限値を超える値から縮小 -> ズーム不可
	EXPECT_EQ( false, GetZoomedValue( setting_50_150_10, 10.0, 1.0, -1, &nValue, &nZoom ) );

	// 下限値を超える値から拡大 -> 下限値を下回る状態で拡大(変化が最小単位以上となるまで拡大(3ステップ分))
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 10.0, 1.0, 1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 20.0, nValue );
	EXPECT_DOUBLE_EQ( 2.0, nZoom );
}

/*!
 * @brief GetZoomedValue関数のテスト - 開始ズーム値がズームテーブルの範囲外
 */
TEST( zoom, GetZoomedValue_OutOfZoomTable )
{
	const ZoomSetting setting_50_150_10( {0.1, 0.9, 0.95, 1.0, 1.05, 1.1, 2.0}, 50.0, 150.0, 10.0 );
	double nValue = 0.0;
	double nZoom = 0.0;

	// ズームテーブル上限を上回る倍率から拡大 -> ズーム不可
	EXPECT_EQ( false, GetZoomedValue( setting_50_150_10, 100.0, 2.5, 1, &nValue, &nZoom ) );

	// ズームテーブル上限を上回る倍率から縮小 -> ズームテーブル末尾の倍率を適用
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 2.5, -1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 200.0, nValue );
	EXPECT_DOUBLE_EQ( 2.0, nZoom );

	// ズームテーブル下限を下回る倍率から縮小 -> ズーム不可
	EXPECT_EQ( false, GetZoomedValue( setting_50_150_10, 100.0, 0.08, -1, &nValue, &nZoom ) );

	// ズームテーブル下限を下回る倍率から拡大 -> ズームテーブル先頭の倍率を適用
	EXPECT_EQ( true, GetZoomedValue( setting_50_150_10, 100.0, 0.08, 1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 10.0, nValue );
	EXPECT_DOUBLE_EQ( 0.1, nZoom );
}

/*!
 * @brief GetZoomedValue関数のテスト - 縮小時の固有動作を狙ったテスト
 */
TEST( zoom, GetZoomedValue_FindingOneMoreChange )
{
	const ZoomSetting setting_0_100_10( {0.97, 0.98, 0.99, 1.0}, 0.0, 100.0, 10.0 );
	double nValue = 0.0;
	double nZoom = 0.0;
	// 探している最中にテーブル先頭まで到達
	EXPECT_EQ( true, GetZoomedValue( setting_0_100_10, 100.0, 1.0, -1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 90.0, nValue );
	EXPECT_DOUBLE_EQ( 0.97, nZoom );

	const ZoomSetting setting_95_100_10( {0.97, 0.98, 0.99, 1.0}, 95.0, 100.0, 10.0 );
	// 下限値に当たる
	EXPECT_EQ( true, GetZoomedValue( setting_95_100_10, 100.0, 1.0, -1, &nValue, &nZoom ) );
	EXPECT_DOUBLE_EQ( 95.0, nValue );
	EXPECT_DOUBLE_EQ( 0.95, nZoom );
}
