"""Assert that a GoogleTest XML report records execution of a whole suite."""

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml", type=Path, required=True, help="GoogleTest XML report")
    parser.add_argument("--suite", required=True, help="exact GoogleTest suite name")
    parser.add_argument("--min-tests", type=int, default=1, help="minimum executed count")
    args = parser.parse_args()
    if args.min_tests < 1:
        parser.error("--min-tests must be at least 1")

    try:
        root = ET.parse(args.xml).getroot()
    except OSError as error:
        print(f"cannot read GoogleTest XML {args.xml}: {error}", file=sys.stderr)
        return 2
    except (ET.ParseError, LookupError, ValueError) as error:
        print(f"malformed GoogleTest XML {args.xml}: {error}", file=sys.stderr)
        return 2

    suites = [suite for suite in root.iter("testsuite") if suite.get("name") == args.suite]
    label = f"gtest suite {args.suite}"
    if not suites:
        print(f"{label}: suite missing", file=sys.stderr)
        return 1

    executed = []
    failed = 0
    not_executed = 0
    for suite in suites:
        for case in suite.findall("testcase"):
            if case.find("failure") is not None or case.find("error") is not None:
                failed += 1
            elif case.get("status") == "run" and case.get("result") == "completed":
                executed.append(case.get("name", "(unnamed)"))
            else:
                not_executed += 1

    if failed or not_executed:
        print(
            f"{label}: {len(executed)} executed, {failed} failed, {not_executed} not executed",
            file=sys.stderr,
        )
        return 1
    if not executed:
        print(f"{label}: no tests executed", file=sys.stderr)
        return 1
    if len(executed) < args.min_tests:
        print(
            f"{label}: {len(executed)} executed, expected at least {args.min_tests}",
            file=sys.stderr,
        )
        return 1

    print(f"{label}: {len(executed)} executed, 0 failed")
    for name in executed:
        print(name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
