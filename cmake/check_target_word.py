#!/usr/bin/env python3
"""The plugin target word, checked against the FIRMWARE image (issue #108).

A board's word is checked at three places, each for what it can see:

  - svc/plugin_target.h: the firmware's predefined macros, by static assert --
    the only check of the CMSE bit (the base's security state, which no image
    records);
  - check_plugin_image.py: each PLUGIN image's .ARM.attributes;
  - this: the FIRMWARE image's .ARM.attributes, with the same derivation.

[!] WHY THE THIRD EXISTS.  The macros cannot tell some cores apart:
-mcpu=cortex-m85+nopacbti predefines exactly what a Cortex-M55 does, so the
static assert would pass a Grove word of "M55" on an M85 firmware -- and the
plugin gate would not notice either, because the plugins are built with their
own -mcpu and would still be M55 (the #108 adversarial review).  The linked
firmware's attributes DO record the core on v8.1-M ("cortex-m55" /
"cortex-m85"), so the firmware's own image answers what its macros cannot.

The CMSE bit is masked here as in the plugin gate; the static assert owns it.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_plugin_image  # noqa: E402 -- one derivation, not a second copy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--target-id", required=True, type=lambda v: int(v, 0))
    args = ap.parse_args()

    errors = []
    note = check_plugin_image.check_target(args.elf, args.target_id, errors)
    if errors:
        print("check_target_word: FAIL (the firmware image)", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    print(f"check_target_word: firmware {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
