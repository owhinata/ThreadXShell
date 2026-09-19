#!/usr/bin/env python3
"""Assemble one sendable container, verify what was assembled, publish it (#107).

This is the chain send_verified_container.sh ran at SEND time, moved to BUILD
time -- and it keeps the property that made that script worth having.

[!] SHARED SINCE ISSUE #108, AND IT KNOWS NO BOARD.  Every board fact arrives as
an argument: which model gate runs (and with what profile), the target identity,
the reservation, the thread allowances, and the slot table -- which each board
EMITS from its own firmware headers in one schema ({"slots": [{"index",
"payload_max"}]}), so this file reads one shape and neither board restates its
geometry here.  What stays here is what is not a board's: the ORDER below.

[!] ASSEMBLE FIRST, THEN VERIFY WHAT WAS ASSEMBLED.  The obvious shape -- check
the parts, staple them together, publish -- is NOT equivalent.  Verifying
components leaves the packer free to read its inputs again, and nothing
downstream would notice.  So: pack once, re-parse the container, extract the
model FROM IT, run the model gate on those extracted bytes, run the DEVICE's own
validator over the whole container, and only then publish that unmodified file.

[!] AND NOTHING IS PUBLISHED UNTIL EVERY GATE HAS PASSED.  Everything happens at
a private path and the finished file is renamed into place.  If the packer wrote
straight to the output, a later verifier failing -- or the build being
interrupted between them -- would leave a complete-looking .nnc sitting at the
path the README tells an operator to send.  That is the whole guarantee.

[!] THE RECEIPT IS CRC-32/ISO-HDLC OVER THE WHOLE FILE, and the expression is
spelled out because it has to match the device exactly: `blob list` prints the
CRC the board accumulated over the payload as received, and svc/crc32.h says in
so many words that the number exists so the two can be compared.  Left as "a
CRC32" this becomes POSIX cksum or CRC-32C one day and the two numbers stop
matching for a reason nobody can see.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib


def die(msg):
    sys.exit("build_asset: " + msg)


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        die("%s rejected it; nothing was published" % what)
    return r.stdout


def extract_model(layout_path, container_path, out_path):
    """Pull the model section out of the ASSEMBLED container."""
    L = json.load(open(layout_path))
    blob = open(container_path, "rb").read()
    hm = L["hdr"]
    n = struct.unpack_from("<I", blob, hm["section_count"])[0]
    for i in range(n):
        at = hm["sections"] + i * L["section_size"]
        t = struct.unpack_from("<I", blob, at + L["section"]["type"])[0]
        o = struct.unpack_from("<I", blob, at + L["section"]["offset"])[0]
        ln = struct.unpack_from("<I", blob, at + L["section"]["length"])[0]
        if t == L["section_type"]["model"]:
            open(out_path, "wb").write(blob[o:o + ln])
            return
    die("no model section in the container that was just packed")


def stack_args(stacks_path):
    """The bounds the plugin gate derived, as --stack arguments.  The packer
    refuses to declare a stack for a slot nobody measured."""
    names = {"pl_entry": "entry", "pl_shapes_ok": "shapes_ok",
             "pl_decode": "decode", "pl_draw": "draw", "pl_report": "report",
             "pl_param_set": "param_set", "pl_param_get": "param_get"}
    b = json.load(open(stacks_path))
    out = []
    for k, v in b.items():
        if k in names:
            out += ["--stack", "%s=%d" % (names[k], v)]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--plugin-elf", required=True)
    ap.add_argument("--plugin-stacks", required=True)
    ap.add_argument("--packer", required=True)
    ap.add_argument("--layout", required=True)
    ap.add_argument("--model-verifier", required=True)
    ap.add_argument("--container-verifier", required=True)
    ap.add_argument("--verify-args", default="")
    ap.add_argument("--build-id", required=True)
    ap.add_argument("--target-id", required=True)
    ap.add_argument("--link-addr", required=True)
    ap.add_argument("--capacity", required=True)
    ap.add_argument("--policy-stack", action="append", default=[])
    ap.add_argument("--slot")
    ap.add_argument("--slot-table")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    # [!] FAIL CLOSED ON EVERY LINK.  A missing tool means nothing is produced.
    # Skipping a check and publishing anyway would make the gate advisory, and an
    # advisory gate over the artifact an operator is told to trust is the thing
    # this exists not to be.
    if not args.model_verifier:
        die("no host C++ compiler was found when this build was configured, so "
            "the model gate does not exist; refusing to build an asset")
    for path, what in ((args.model_verifier, "model verifier"),
                       (args.container_verifier, "container verifier"),
                       (args.plugin_elf, "plugin image"),
                       (args.plugin_stacks, "plugin stack bounds"),
                       (args.layout, "ABI layout"),
                       (args.model, "model")):
        if not os.path.exists(path):
            die("%s missing: %s" % (what, path))

    # The binutils the packer needs, passed through the environment because the
    # packer takes them as arguments and CMake already knows them.  Missing, this
    # dies with a sentence like every other failure here -- a raw KeyError would
    # still refuse to publish, but for a reason nobody reading it could act on.
    for _v in ("ASSET_NM", "ASSET_OBJCOPY"):
        if not os.environ.get(_v):
            die("%s is not set; the packer cannot read the plugin image without "
                "the cross binutils" % _v)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    # Staged in the OUTPUT directory so the publishing rename is same-filesystem
    # and therefore atomic.
    work = tempfile.mkdtemp(prefix=".%s.build." % args.name, dir=out_dir)
    container = os.path.join(work, "asset.nnc")
    extracted = os.path.join(work, "extracted.tflite")
    try:
        run([sys.executable, args.packer,
             "--layout", args.layout, "--elf", args.plugin_elf,
             "--model", args.model, "--name", args.name,
             "--build-id", args.build_id, "--out", container,
             "--nm", os.environ["ASSET_NM"], "--objcopy", os.environ["ASSET_OBJCOPY"],
             "--target-id", args.target_id] + stack_args(args.plugin_stacks),
            "the packer")

        # From here on, only the ASSEMBLED file is read.
        extract_model(args.layout, container, extracted)
        run([args.model_verifier, extracted] + args.verify_args.split(),
            "the model gate")
        policy = []
        for e in args.policy_stack:
            policy += ["--stack", e]
        run([args.container_verifier, container,
             "--target", args.target_id, "--link", args.link_addr,
             "--capacity", args.capacity] + policy,
            "the device's own validator")

        blob = open(container, "rb").read()
        crc = zlib.crc32(blob) & 0xFFFFFFFF          # == what `blob list` prints

        # [!] DOES IT FIT THE SLOT IT DECLARES?  The host cannot discover the
        # slot an operator will type, but the DECLARATION names one, and the
        # packed size is known here.  `blob write` erases the whole slot BEFORE
        # the YMODEM size header arrives, so a container that cannot fit costs
        # the previous blob and an endurance event to discover on the device.
        # Refusing here is free and happens before any of that.
        if args.slot and args.slot_table:
            table = json.load(open(args.slot_table))
            for sl in table["slots"]:
                if sl["index"] == int(args.slot):
                    if len(blob) > sl["payload_max"]:
                        die("%s is %d B and slot %s holds %d B.  The device "
                            "would erase that slot before finding out."
                            % (args.name, len(blob), args.slot, sl["payload_max"]))
                    break
            else:
                die("slot %s is not in the map" % args.slot)

        # Every gate passed: publish.  Read-only because the path is about to be
        # pasted into picocom, and an accidental in-place overwrite should not be
        # silent -- though nothing stops someone sending a different file.
        #
        # [!] THE CONTAINER GOES FIRST, THEN THE RECEIPT.  Published the other way
        # round, an interruption between the two leaves a NEW receipt beside an
        # OLD .nnc -- and the operator would compare a CRC that describes neither
        # the file at that path nor what the board stored.  The receipt printer
        # recomputes the CRC from the file anyway, so neither order can mislead;
        # this order just makes the window harmless rather than merely detected.
        os.chmod(container, 0o444)
        os.replace(container, args.out)
        receipt = {"name": args.name, "bytes": len(blob), "crc32": "%08X" % crc,
                   "path": os.path.abspath(args.out)}
        with open(os.path.join(work, "receipt.json"), "w") as fh:
            json.dump(receipt, fh, indent=2)
        os.replace(os.path.join(work, "receipt.json"), args.out + ".json")
    finally:
        for f in os.listdir(work) if os.path.isdir(work) else []:
            os.unlink(os.path.join(work, f))
        if os.path.isdir(work):
            os.rmdir(work)


if __name__ == "__main__":
    main()
