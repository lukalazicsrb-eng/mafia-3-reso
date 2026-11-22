"""Compare matching DXBC passes in two Mafia III reflection reports."""

import argparse
import json
from pathlib import Path
import re


def read_report(path):
    with Path(path).open(encoding="utf-8-sig") as stream:
        data = json.load(stream)
    if data.get("schema_version") != 1 or data.get("status") != "ok" or not isinstance(data.get("passes"), list):
        raise ValueError(f"Snimak nije uspešno analiziran: {path}")
    return data


def collect(data):
    result = {}
    for draw in data["passes"]:
        if draw.get("classification") != "reflection_reference":
            continue
        digest = draw.get("shader_sha256", "")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            continue
        stage = draw.get("stage", "")
        shader = f"{draw.get('reference_name') or 'Reflection'} {draw.get('shader_crc32', '')}"
        bindings = draw.get("reads", []) + draw.get("outputs", [])
        if draw.get("depth"):
            bindings.append(draw["depth"])
        for entry in bindings:
            values = [entry.get(field) for field in ("view_width", "view_height", "samples")]
            if not all(isinstance(v, int) and v > 0 for v in values):
                continue
            slot = (entry.get("kind", "?"), entry.get("slot", 0))
            signature = (*values, entry.get("format") or "?", entry.get("first_mip"), entry.get("num_mips"))
            key = (stage, digest.lower(), slot)
            item = result.setdefault(key, (shader, set()))
            item[1].add(signature)
    return result


def describe(sample):
    width, height, samples, fmt, mip, count = sample
    return f"{width}x{height}, {samples}x, {fmt}, mip {mip}+{count}"


def compare(base, ultra):
    first, second = collect(base), collect(ultra)
    common = first.keys() & second.keys()
    lines = [f"Prvi snimak: {base.get('capture', '?')}", f"Drugi snimak: {ultra.get('capture', '?')}", ""]
    if not common:
        lines.append("Nema istih identifikovanih shader SHA-256 vrednosti i slotova u oba snimka.")
    for stage, digest, slot in sorted(common):
        key = (stage, digest, slot)
        label, previous = first[key]
        _, current = second[key]
        if len(previous) != 1 or len(current) != 1:
            status = "više varijanti u snimku"
        else:
            status = "ista zabeležena veza" if previous == current else "zabeležena razlika"
        lines.append(f"{label} | {stage} {slot[0]}{slot[1]} | {status}")
        lines.append(f"  prvi: {'; '.join(sorted(map(describe, previous)))}")
        lines.append(f"  drugi: {'; '.join(sorted(map(describe, current)))}")
    lines.extend(("", f"Veze prisutne samo u jednom snimku: prvi {len(first.keys() - second.keys())}, drugi {len(second.keys() - first.keys())}.",
                  "Poređenje opisuje posmatrane slotove. Različiti shaderi, scene ili aktivni prolazi zahtevaju proveru originalnih .rdc snimaka."))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", help="Baseline JSON report")
    parser.add_argument("ultra", help="Ultra JSON report")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        text = compare(read_report(args.base), read_report(args.ultra))
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            print(text, end="")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
