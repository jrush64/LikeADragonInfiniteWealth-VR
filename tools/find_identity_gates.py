"""Scan dumped code for the identity-keyed skip idiom that V19 opened.

The V19 gate was:
    48 8B 05 <rel32>   mov rax, [rip+disp]      ; -> exe+0x48EB3B8 current group ptr
    83 38 02           cmp dword ptr [rax], 2   ; leading identity == 2 (eye 1)
    74 xx / 0F 84 ..   je  skip

This finds every `cmp dword ptr [reg], 2` (or cmp against a group-identity load) followed by a
conditional jump, in the dumped ranges, and reports the ones whose compared pointer traces back
to a rip-relative load of the current-group global.
"""

import argparse
import re
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

CURRENT_GROUP_RVA = 0x48EB3B8
JCC = {
    "je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle",
}


def scan(path: Path, base: int, group_rva: int):
    data = path.read_bytes()
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    instructions = list(decoder.disasm(data, base))

    # Track rip-relative loads that resolve to the current-group global.
    group_regs = {}
    findings = []
    for index, ins in enumerate(instructions):
        text = f"{ins.mnemonic} {ins.op_str}"

        # mov <reg>, [rip + disp]
        match = re.match(r"mov (\w+), qword ptr \[rip \+ (0x[0-9a-f]+)\]", text)
        if match:
            target = ins.address + ins.size + int(match.group(2), 16)
            if target == group_rva:
                group_regs[match.group(1)] = ins.address
            else:
                group_regs.pop(match.group(1), None)
            continue

        # cmp dword ptr [<reg>], <imm>
        match = re.match(r"cmp dword ptr \[(\w+)\], (\d+)", text)
        if match and index + 1 < len(instructions):
            nxt = instructions[index + 1]
            if nxt.mnemonic in JCC:
                reg, imm = match.group(1), int(match.group(2))
                origin = group_regs.get(reg)
                findings.append({
                    "cmp_at": ins.address,
                    "cmp": text,
                    "jcc_at": nxt.address,
                    "jcc": f"{nxt.mnemonic} {nxt.op_str}",
                    "fall": nxt.address + nxt.size,
                    "imm": imm,
                    "group_load": origin,
                })
    return findings


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--group-rva", type=lambda v: int(v, 0), default=CURRENT_GROUP_RVA)
    parser.add_argument("--imm", type=int, default=None,
                        help="only report comparisons against this immediate")
    args = parser.parse_args()

    for path in args.inputs:
        match = re.search(r"rva_([0-9A-Fa-f]+)", path.name)
        if not match:
            continue
        base = int(match.group(1), 16)
        for finding in scan(path, base, args.group_rva):
            if args.imm is not None and finding["imm"] != args.imm:
                continue
            confirmed = finding["group_load"] is not None
            tag = "GROUP-IDENTITY GATE" if confirmed else "candidate"
            origin = (f" load@0x{finding['group_load']:X}" if confirmed else "")
            print(f"[{tag}] {path.name}{origin}")
            print(f"    0x{finding['cmp_at']:X}  {finding['cmp']}")
            print(f"    0x{finding['jcc_at']:X}  {finding['jcc']}"
                  f"   (fall-through 0x{finding['fall']:X})")


if __name__ == "__main__":
    main()
