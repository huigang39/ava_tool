"""Synchronize FocDriver's standalone safety C engine into ava_tool.

Safety JSON files stay in the user's firmware/configuration directory and are
selected in the application. This script never copies or modifies those files.
"""
import argparse
from pathlib import Path

CORE_FILES = ("ckit/include/safety.h", "ckit/src/safety.c")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("foc_driver", type=Path)
    parser.add_argument("--check", action="store_true", help="Check matching C engine files without writing")
    args = parser.parse_args()
    ava_root = Path(__file__).resolve().parents[1]
    try:
        sources = {relative: (args.foc_driver / relative).read_bytes() for relative in CORE_FILES}
        if args.check:
            for relative, data in sources.items():
                if data != (ava_root / relative).read_bytes():
                    raise ValueError("Bundled C engine differs: " + relative)
            print("Safety C engine is current")
        else:
            for relative, data in sources.items():
                target = ava_root / relative
                if not target.is_file() or target.read_bytes() != data:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
            print("Synchronized safety C engine")
    except (OSError, ValueError) as error:
        parser.exit(1, "Safety C engine synchronization failed: " + str(error) + "\n")


if __name__ == "__main__":
    main()
