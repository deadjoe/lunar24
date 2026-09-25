#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# task #121 / GH#19 S6 PR-B: stage a SHADOW INCLUDE ROOT holding mutated product headers, for the two
# control arms this slice's gate needs. One tool, two mutations, selected by --mutation.
#
# WHY A GENERATED SHADOW ROOT AND NOT A COMMITTED COPY, AND NOT A `#ifdef`. The S6 precedent
# (tools/stage_gh19_s6_shadow.py) states the reasoning and it applies unchanged: a committed copy of a
# product header is a second encoding of the same code, free to drift from the one that ships; a
# `#ifdef` inside the header would put a build-time configuration branch into the shipped DSP, which is
# the "no hidden switches" hard edge S0 carries. The shadow root is placed BEFORE the product include
# dirs on one compile line, so exactly one header is overridden and every other header still resolves
# from core/include. Same compiler, same flags, same sources -- only the include order differs, which is
# what makes the arm a SAME-BUILD comparison rather than a cross-build one.
#
# THE ANCHOR MUST OCCUR EXACTLY ONCE. Verified before anything is written, and the run exits non-zero
# (so the build fails) when it does not: a stale anchor would stage a PRISTINE header under the control
# arm's name, the control would then measure the product against itself, and the negative control would
# pass while demonstrating nothing. That is the single failure mode this tool exists to make impossible.
#
# THE TWO MUTATIONS, AND WHAT EACH IS FOR
#
#   improvement-bypass  core/include/lunar24/core/pulse_blep_kernel.h, `polyblepPulseCorrection()`
#                       returns 0 immediately. The two-edge correction is bypassed entirely, so the
#                       product emits the naive pulse. This is the S3-authorized mutation
#                       (run_gh19_s3_pulse_pipeline_mutant.py's BYPASS_CORRECTION, same anchor, same
#                       replacement) and it is the neutralised arm of the IMPROVEMENT criterion: with
#                       the correction gone, the improvement columns must come in short and the gate
#                       must say so BY NAME (`PRB-FAIL-IMPROVEMENT`). Reusing S3's exact anchor rather
#                       than writing a second one is deliberate -- the claim being controlled is the
#                       same claim, so a second encoding of it could only drift.
#
#   degen-dispatch      core/include/lunar24/core/vco.h, `effectiveDuty()` loses the `pwDepth_` factor
#                       from its modulation term. See the LONG NOTE below: this mutation is NOT a valid
#                       control for the identity criterion, and the reason is structural.
#
# *** LONG NOTE: WHY `degen-dispatch` CANNOT FALSIFY THE IDENTITY, AND WHAT THAT MEANS ***
# vco.h's `effectiveDuty()` is `duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts`, and vco.h:179-180
# forces a non-finite depth or CV to 0.0. At `pwDepth_ == 0` the modulation term is therefore
# `0.0 * finite == 0.0` EXACTLY in IEEE-754 on every sample, so a fully live cable leaves the duty
# untouched: that is the degeneracy the identity criterion rests on, and it is an arithmetic fact, not
# a wiring convention.
# The consequence is that the two mutations the criteria file's section 5 names cannot produce
# `PRB-FAIL-DEGEN-IDENTITY`:
#   * removing the depth factor (this mutation) makes the cable's value REACH the duty, so the
#     constant-duty cell stops being constant -- it destroys the criterion's own ANTECEDENT rather than
#     falsifying its consequent. A gate that reported a failure here would be reporting "the premise
#     did not hold" as "the identity was broken", and those are different facts about different things.
#     What SHOULD happen, and what the driver does, is a refusal: there is no constant-duty dynamic
#     cell on this arm to judge, so no judgement is issued.
#   * perturbing `kPwmCvFullScaleVolts` (the criteria file's second suggestion) multiplies the CV by
#     zero at depth 0 and therefore has NO effect at all: the identity stays green, so it falsifies
#     nothing either.
# More generally, at depth 0 the CV value is irrelevant by construction, so NO mutation on the CV side
# or the depth side can break the identity while leaving the duty constant. The identity's falsifiability
# therefore has to come from the INSTRUMENT rather than from a product mutation -- a one-ulp tamper of
# the constructed cell's render, which is the S6 `gh19_s6_saw_baseline_report_pin` precedent ("one ulp in
# one of arm B's windows must fire EQUALITY-SINE"). This mutation is kept because the antecedent check it
# exercises is worth demonstrating (a gate that cannot notice its premise has evaporated is the
# silent-skip failure), but it is reported as PREMISE-LIVE, never as "the identity was falsified".
# This is raised with the implementation director; the criteria file's section 5 negative-control
# clause is the director's to rule on, not this tool's to reinterpret silently.
import argparse
import hashlib
import os
import sys

VCO_H = "core/include/lunar24/core/vco.h"
KERNEL_H = "core/include/lunar24/core/pulse_blep_kernel.h"

# --- improvement-bypass: verbatim the anchor and replacement S3's mutant runner uses. -------------
BYPASS_ANCHOR = (
    "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
    "  if (!(dt > 0.0)) return 0.0;"
)
BYPASS_REPLACEMENT = (
    "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
    "  // GH#19 S6 PR-B CONTROL ARM -- generated by tools/stage_gh19_prb_shadow.py into a shadow\n"
    "  // include root. The shipped header is NOT touched: this file exists only so that the\n"
    "  // neutralised arm of the improvement criterion differs from the candidate arm in the\n"
    "  // contribution of the two-edge correction and in nothing else.\n"
    "  return 0.0;  // CONTROL: the two-edge correction bypassed entirely\n"
    "  if (!(dt > 0.0)) return 0.0;"
)

# --- degen-dispatch: drop the depth factor. The residual line is kept inside the replacement so the
# two arms differ in exactly one respect -- whether depth scales the modulation -- and not in a
# secondary "is this expression evaluated at all" respect a reader would have to account for.
DEGEN_ANCHOR = "    const double d = duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts;\n"
DEGEN_REPLACEMENT = (
    "    // GH#19 S6 PR-B CONTROL ARM -- see tools/stage_gh19_prb_shadow.py. NOT a valid control for\n"
    "    // the identity criterion: it destroys the constant-duty premise instead of falsifying the\n"
    "    // identity. Retained to demonstrate that the premise check is live.\n"
    "    const double d = duty_ + pwCv_ / kPwmCvFullScaleVolts;\n"
)

MUTATIONS = {
    "improvement-bypass": (KERNEL_H, BYPASS_ANCHOR, BYPASS_REPLACEMENT),
    "degen-dispatch": (VCO_H, DEGEN_ANCHOR, DEGEN_REPLACEMENT),
}


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def stage(repo_root, shadow_root, mutation, quiet=False):
    """Write the mutated header under `shadow_root` (repo-relative layout preserved). Returns
    (path, tree_sha, mutated_sha) or None, printing an INVALID line naming the reason."""
    if mutation not in MUTATIONS:
        print("INVALID: unknown mutation %r (known: %s)" % (mutation, ",".join(sorted(MUTATIONS))))
        return None
    rel, anchor, replacement = MUTATIONS[mutation]
    src = os.path.join(repo_root, rel)
    if not os.path.exists(src):
        print("INVALID: %s does not exist in %s" % (rel, repo_root))
        return None
    # encoding= is not optional. These headers are UTF-8 and carry non-ASCII characters (em dashes in
    # the prose comments), and Python's text-mode default is the LOCALE codec: UTF-8 on Linux/macOS but
    # cp1252 on Windows, where this tool runs as a build input and raises UnicodeDecodeError on the
    # first one. That surfaces as an MSB8066 custom-build error, i.e. as a broken WINDOWS BUILD rather
    # than as a bad read, which is a long way from the real cause.
    with open(src, encoding="utf-8") as fh:
        text = fh.read()
    n = text.count(anchor)
    if n != 1:
        print("INVALID: the %s anchor occurs %d times in %s (expected exactly 1):\n%r"
              % (mutation, n, rel, anchor))
        return None
    staged = text.replace(anchor, replacement)
    tree_sha, mut_sha = sha256_text(text), sha256_text(staged)
    if tree_sha == mut_sha:
        # Unreachable while the anchor is non-empty, but stated rather than assumed: an edit that does
        # not change the header would make the control arm the product arm, and the control would then
        # compare the product against itself and pass while measuring nothing.
        print("INVALID: the %s mutation leaves %s byte-identical to the tree's copy" % (mutation, rel))
        return None
    dst = os.path.join(shadow_root, rel[len("core/include/"):])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    # newline="\n" for the same reason the read pins its codec: text-mode writing translates "\n" to
    # os.linesep, so on Windows the staged header would come out CRLF while the digest printed below is
    # taken from the untranslated string. Pinning it makes the staged bytes a function of the input
    # alone, so one digest describes one file on every platform.
    with open(dst, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(staged)
    if not quiet:
        print("PRB-SHADOW mutation=%s path=%s anchor_hits=1" % (mutation, dst))
        print("PRB-SHADOW tree_sha256=%s" % tree_sha)
        print("PRB-SHADOW mutated_sha256=%s differs=yes" % mut_sha)
    return dst, tree_sha, mut_sha


def main(argv):
    ap = argparse.ArgumentParser(description="stage a GH#19 S6 PR-B control-arm shadow include root")
    ap.add_argument("--repo-root", default=".", help="repo root holding core/include")
    ap.add_argument("--out", required=True, help="shadow include root to write into")
    ap.add_argument("--mutation", required=True, choices=sorted(MUTATIONS),
                    help="which product behaviour the control arm neutralises")
    args = ap.parse_args(argv)
    if stage(args.repo_root, args.out, args.mutation) is None:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
