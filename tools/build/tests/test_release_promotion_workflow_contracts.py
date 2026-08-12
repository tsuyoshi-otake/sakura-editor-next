from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
RELEASE_PROMOTION_WORKFLOW = REPO_ROOT / ".github/workflows/release-promotion.yml"


class ReleasePromotionWorkflowContractTests(unittest.TestCase):
    def test_legacy_tag_build_rebinds_source_sha_only_in_its_cmd_child(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8-sig")
        promotion_start = text.index("    - name: MSBuild release promotion\n")
        ordinary_start = text.index("    - name: MSBuild\n", promotion_start)
        promotion = text[promotion_start:ordinary_start]
        ordinary = text[ordinary_start:]

        self.assertIn("if: ${{ inputs.release_promotion }}", promotion)
        self.assertIn('set "GITHUB_SHA=${{ env.RELEASE_SOURCE_SHA }}"', promotion)
        self.assertIn("call build-sln.bat ${{ matrix.platform }} ${{ matrix.config }}", promotion)
        self.assertIn("if: ${{ !inputs.release_promotion }}", ordinary)

    def test_distribution_smoke_uses_a_runner_compatible_with_the_installer(self) -> None:
        text = RELEASE_PROMOTION_WORKFLOW.read_text(encoding="utf-8-sig")
        smoke_start = text.index("  smoke:\n")
        publish_start = text.index("  publish:\n", smoke_start)
        smoke = text[smoke_start:publish_start]

        self.assertIn("    runs-on: windows-2025\n", smoke)
        self.assertNotIn("runs-on: windows-2022", smoke)
        self.assertIn("    - name: Verify smoke runner compatibility\n", smoke)
        self.assertIn("$minimum = [Version]::new(10, 0, 22000)", smoke)


if __name__ == "__main__":
    unittest.main()
