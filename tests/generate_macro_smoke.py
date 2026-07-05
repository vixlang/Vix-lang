#!/usr/bin/env python3
from pathlib import Path


NUMERIC_START = 129
NUMERIC_END = 400
MACRO_START = 1
MACRO_END = 400
UNITS = 92


def large_macro_expected(idx: int) -> int:
    mul = 2 + idx % 5
    add = 3 + idx % 11
    bias = 1 + idx % 7
    total = 0
    for j in range(UNITS):
        x = j + 1 + idx % 9
        if j % 4 == 0:
            total += mul * x + add
        elif j % 4 == 1:
            total += mul * (x + x) + add
        elif j % 4 == 2:
            total += (mul * x + add) + (mul * x + add)
        else:
            total += mul * (x + bias) + add
    return total % 251


def large_macro_source(idx: int) -> str:
    mul = 2 + idx % 5
    add = 3 + idx % 11
    bias = 1 + idx % 7
    lines: list[str] = [
        "macro $make_affine(name: ident)",
        "{",
        "    fn $name(x: i32): i32",
        "    {",
        f"        let base = x * {mul}",
        f"        let shifted = base + {add}",
        "        return shifted",
        "    }",
        "}",
        "",
        "macro $pack[items: expr*]",
        "{",
        "    [$(items),*]",
        "}",
        "",
        "macro $twice(x: expr)",
        "{",
        "    ($x + $x)",
        "}",
        "",
        "macro $bias(x: expr)",
        "{",
        f"    ($x + {bias})",
        "}",
        "",
    ]
    for j in range(UNITS):
        lines.extend(
            [
                f"$make_affine(gen_{idx}_{j})",
                "",
            ]
        )

    lines.extend(
        [
            "fn main(): i32",
            "{",
            "    let values = $pack[",
        ]
    )
    for j in range(UNITS):
        x = j + 1 + idx % 9
        if j % 4 == 0:
            expr = f"gen_{idx}_{j}({x})"
        elif j % 4 == 1:
            expr = f"gen_{idx}_{j}($twice({x}))"
        elif j % 4 == 2:
            expr = f"$twice(gen_{idx}_{j}({x}))"
        else:
            expr = f"gen_{idx}_{j}($bias({x}))"
        comma = "," if j < UNITS - 1 else ""
        lines.append(f"        {expr}{comma}")

    lines.extend(
        [
            "    ]",
            "    let mut total = 0",
            "    for (i in 0 .. values.length)",
            "    {",
            "        total = total + values[i]",
            "    }",
            "    return total % 251",
            "}",
        ]
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    root = Path(__file__).resolve().parent / "files"
    root.mkdir(parents=True, exist_ok=True)
    for idx in range(NUMERIC_START, NUMERIC_END + 1):
        (root / f"test{idx}.vix").write_text(large_macro_source(idx))
    for idx in range(MACRO_START, MACRO_END + 1):
        (root / f"macro_test{idx:03d}.vix").write_text(large_macro_source(idx))
    print(f"generated {NUMERIC_END - NUMERIC_START + 1} numbered large macro smoke tests in {root}")
    print(f"generated {MACRO_END - MACRO_START + 1} dedicated large macro smoke tests in {root}")
    print(f"line count per file: {len(large_macro_source(NUMERIC_START).splitlines())}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
