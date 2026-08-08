from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"


class ReleasePromotionWorkflowContractTests(unittest.TestCase):
    def test_legacy_tag_build_rebinds_source_sha_only_in_its_cmd_child(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8-sig")
        promotion_start = text.index("    - name: MSBuild release promotion\n")
        ordinary_start = text.index("    - name: MSBuild\n", promotion_start)
        promotion = text[promotion_start:ordinary_start]
        ordinary = text[ordinary_start:]

        self.assertIn(
            "if: ${{ !(matrix.platform == 'x64' && matrix.config == 'Debug') && inputs.release_promotion }}",
            promotion,
        )
        self.assertIn('set "GITHUB_SHA=${{ env.RELEASE_SOURCE_SHA }}"', promotion)
        self.assertIn("call build-sln.bat ${{ matrix.platform }} ${{ matrix.config }}", promotion)
        self.assertIn(
            "if: ${{ !(matrix.platform == 'x64' && matrix.config == 'Debug') && !inputs.release_promotion }}",
            ordinary,
        )


if __name__ == "__main__":
    unittest.main()
