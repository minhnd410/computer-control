#!/usr/bin/env python3
"""Check and request the OS permissions this tool needs.

The macOS case is the interesting one. A command-line tool started from a
terminal is attributed to *the terminal*, not to itself, so:

  - it never appears in System Settings > Privacy & Security > Accessibility,
  - the permission prompt can never fire, because AXIsProcessTrusted() already
    returns true for the inherited grant, and
  - that inherited grant sometimes covers the trust check without covering
    real inspection, which looks exactly like "this app has no UI".

This script reports which of those you are in. Read-only unless --request.
"""

import sys

from computer_control import Session


def main() -> int:
    request = "--request" in sys.argv

    with Session() as cc:
        result = cc.batch([{"action": "permissions", "request": request}])
        report = result["steps"][0]["result"]

        print(f"Executable: {report['executable']}")
        if report.get("bundle"):
            print(f"Bundle:     {report['bundle']}")
        print(f"Own TCC identity: {report['own_tcc_identity']}")

        owner = report.get("permissions_attributed_to")
        if owner:
            print(f"Attributed to:    {owner}")
        print()

        for p in report["permissions"]:
            state = p["state"]
            mark = "ok " if state in ("granted", "not_required") else "!! "
            print(f"  {mark}{p['permission']:18} {state}")
            if state not in ("granted", "not_required"):
                if p.get("detail"):
                    print(f"       {p['detail']}")
                if p.get("affects"):
                    print(f"       affects: {', '.join(p['affects'])}")
                if p.get("remedy"):
                    for line in p["remedy"].split("\n"):
                        print(f"       {line}")
                print()

        if report["all_granted"]:
            print("\nEverything needed is granted.")
            return 0

        if not request:
            print("\nRun with --request to prompt for what is missing.")
        else:
            print("\nGrants are read at launch, so restart the process to pick them up.")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
