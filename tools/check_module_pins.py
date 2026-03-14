#!/usr/bin/env python3
"""Validate module GPIO assignments, print inverse pin maps, and generate pinout HTML."""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import pathlib
import re
import sys


ASSIGNMENT_RE = re.compile(r"\{\s*(SCP_GPIO_SIGNAL_[A-Z0-9_]+)\s*,\s*([0-9]+)\s*\}")
ARRAY_START_RE = re.compile(r"\bg_gpio_assignments\b\s*\[\]\s*=\s*\{")
SIGNAL_PREFIX = "SCP_GPIO_SIGNAL_"

PHYSICAL_TO_LABEL = {
    1: "GP0",
    2: "GP1",
    3: "GND",
    4: "GP2",
    5: "GP3",
    6: "GP4",
    7: "GP5",
    8: "GND",
    9: "GP6",
    10: "GP7",
    11: "GP8",
    12: "GP9",
    13: "GND",
    14: "GP10",
    15: "GP11",
    16: "GP12",
    17: "GP13",
    18: "GND",
    19: "GP14",
    20: "GP15",
    21: "GP16",
    22: "GP17",
    23: "GND",
    24: "GP18",
    25: "GP19",
    26: "GP20",
    27: "GP21",
    28: "GND",
    29: "GP22",
    30: "RUN",
    31: "GP26 / ADC0",
    32: "GP27 / ADC1",
    33: "AGND",
    34: "GP28 / ADC2",
    35: "ADC_VREF",
    36: "3V3(OUT)",
    37: "3V3_EN",
    38: "GND",
    39: "VSYS",
    40: "VBUS",
}

GPIO_TO_PHYSICAL_PIN = {
    0: 1,
    1: 2,
    2: 4,
    3: 5,
    4: 6,
    5: 7,
    6: 9,
    7: 10,
    8: 11,
    9: 12,
    10: 14,
    11: 15,
    12: 16,
    13: 17,
    14: 19,
    15: 20,
    16: 21,
    17: 22,
    18: 24,
    19: 25,
    20: 26,
    21: 27,
    22: 29,
    26: 31,
    27: 32,
    28: 34,
}

NON_HEADER_GPIO_NOTES = {
    23: "GPIO23 (not broken out on Pico header)",
    24: "GPIO24 (not broken out on Pico header)",
    25: "GPIO25 (on-board LED)",
    29: "GPIO29 / ADC3 (internal VSYS sense, not header pin)",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check per-module GPIO collisions and print used/available GPIOs."
    )
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--gpio-min", type=int, default=0, help="Lowest GPIO number.")
    parser.add_argument("--gpio-max", type=int, default=29, help="Highest GPIO number.")
    parser.add_argument("--quiet", action="store_true", help="Only print errors.")
    parser.add_argument("--module", help="Only check one module by directory name.")
    parser.add_argument(
        "--generate-html",
        action="store_true",
        help="Generate a pinout HTML file in docs/ for each checked module.",
    )
    parser.add_argument(
        "--docs-dir",
        default="docs",
        help="Directory for generated HTML output (relative to --root if not absolute).",
    )
    return parser.parse_args()


def parse_module_main(path: pathlib.Path) -> list[tuple[str, int]]:
    assignments: list[tuple[str, int]] = []
    in_assignments = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if not in_assignments and ARRAY_START_RE.search(line):
            in_assignments = True
            continue
        if in_assignments and "};" in line:
            break
        if not in_assignments:
            continue
        match = ASSIGNMENT_RE.search(line)
        if match:
            assignments.append((match.group(1), int(match.group(2))))
    return assignments


def normalize_signal_name(signal_macro: str) -> str:
    if signal_macro.startswith(SIGNAL_PREFIX):
        return signal_macro[len(SIGNAL_PREFIX):]
    return signal_macro


def gpio_for_base_label(base_label: str) -> int | None:
    if not base_label.startswith("GP"):
        return None
    head = base_label.split("/", 1)[0].strip()
    try:
        return int(head.removeprefix("GP"))
    except ValueError:
        return None


def build_pin_rows(assignments: list[tuple[str, int]]) -> list[dict[str, object]]:
    gpio_to_signals: dict[int, list[str]] = {}
    for signal_macro, gpio in assignments:
        gpio_to_signals.setdefault(gpio, []).append(normalize_signal_name(signal_macro))

    rows: list[dict[str, object]] = []
    for physical in range(1, 41):
        base = PHYSICAL_TO_LABEL[physical]
        gpio = gpio_for_base_label(base)
        rows.append(
            {
                "physical": physical,
                "base": base,
                "gpio": gpio,
                "assigned": gpio_to_signals.get(gpio, []) if gpio is not None else [],
            }
        )
    return rows


def _build_assignments_list_html(assignments: list[tuple[str, int]]) -> str:
    if not assignments:
        return "<li>No GPIO assignments found.</li>"
    items = []
    for signal_macro, gpio in sorted(assignments, key=lambda item: item[1]):
        signal = html.escape(normalize_signal_name(signal_macro))
        items.append(f"<li><strong>{signal}</strong> - GPIO{gpio}</li>")
    return "\n".join(items)


def _build_non_header_list_html(assignments: list[tuple[str, int]]) -> str:
    gpio_to_signals: dict[int, list[str]] = {}
    for signal_macro, gpio in assignments:
        gpio_to_signals.setdefault(gpio, []).append(normalize_signal_name(signal_macro))

    items = []
    for gpio in sorted(gpio_to_signals):
        if gpio in GPIO_TO_PHYSICAL_PIN:
            continue
        signals = ", ".join(gpio_to_signals[gpio])
        note = NON_HEADER_GPIO_NOTES.get(gpio, f"GPIO{gpio} (not on Pico header pin)")
        items.append(
            f"<li><strong>{html.escape(signals)}</strong> - {html.escape(note)}</li>"
        )
    return "\n".join(items) if items else "<li>None</li>"


def generate_pinout_html(module_name: str, assignments: list[tuple[str, int]], out_path: pathlib.Path) -> None:
    rows = build_pin_rows(assignments)
    left_rows = [row for row in rows if row["physical"] <= 20]
    right_rows = sorted(
        [row for row in rows if row["physical"] >= 21],
        key=lambda row: row["physical"],
        reverse=True,
    )

    assigned_marker_pins = sorted(
        GPIO_TO_PHYSICAL_PIN[gpio]
        for _, gpio in assignments
        if gpio in GPIO_TO_PHYSICAL_PIN
    )

    assignments_list_html = _build_assignments_list_html(assignments)
    non_header_list_html = _build_non_header_list_html(assignments)

    html_text = f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"UTF-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">
  <title>{html.escape(module_name)} Pico Pinout</title>
  <style>
    :root {{
      --bg: #f4f0e8;
      --ink: #162025;
      --muted: #5f6a73;
      --line: #d3c7ae;
      --accent: #ff6a3d;
      --accent-soft: rgba(255,106,61,0.12);
      --ok-soft: rgba(22,122,84,0.14);
      --shadow: 0 18px 40px rgba(22, 32, 37, 0.12);
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: ui-sans-serif, system-ui, sans-serif;
      color: var(--ink);
      background:
        radial-gradient(circle at 15% 20%, rgba(255, 181, 105, 0.18), transparent 45%),
        radial-gradient(circle at 90% 10%, rgba(60, 120, 180, 0.1), transparent 35%),
        var(--bg);
    }}
    .page {{ max-width: 1220px; margin: 0 auto; padding: 24px 16px 40px; }}
    .header h1 {{ margin: 0 0 6px; font-size: clamp(1.1rem, 2vw, 1.6rem); }}
    .header p {{ margin: 0; color: var(--muted); }}
    code {{ background: rgba(255,255,255,.7); padding: 1px 4px; border-radius: 4px; }}
    .meta {{ margin-top: 10px; display: flex; gap: 8px; flex-wrap: wrap; }}
    .badge {{ display: inline-flex; align-items: center; height: 28px; padding: 0 10px; border-radius: 999px; border: 1px solid rgba(90,100,110,.15); background: rgba(255,255,255,.72); font-size: 12px; }}
    .badge strong {{ margin-left: 6px; }}
    .shell {{ margin-top: 14px; background: rgba(255,250,240,.72); border: 1px solid rgba(120,105,80,.15); border-radius: 18px; box-shadow: var(--shadow); padding: 12px; overflow-x: auto; }}
    .stage {{ min-width: 940px; display: grid; grid-template-columns: 1fr 320px 1fr; gap: 18px; align-items: start; padding: 16px; border-radius: 12px; }}
    .labels {{ position: relative; height: 808px; }}
    .pin-label {{ position: absolute; left: 0; right: 0; height: 28px; display: grid; align-items: center; gap: 8px; font-size: 12px; line-height: 1; cursor: pointer; user-select: none; }}
    .labels.left .pin-label {{ grid-template-columns: 1fr 44px; text-align: right; }}
    .labels.right .pin-label {{ grid-template-columns: 44px 1fr; text-align: right; }}
    .pin-chip, .pin-text {{ display: flex; align-items: center; height: 28px; border-radius: 8px; transition: 140ms ease; }}
    .pin-chip {{ justify-content: center; border: 1px solid var(--line); background: rgba(255,255,255,.85); color: var(--muted); font-weight: 700; font-size: 11px; }}
    .pin-text {{ padding: 0 10px; border: 1px solid rgba(90,100,110,.12); background: rgba(255,255,255,.8); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; justify-content: flex-start; }}
    .labels.left .pin-text {{ justify-content: flex-end; text-align: right; }}
    .pin-label.assigned .pin-text {{ background: var(--ok-soft); border-color: rgba(22,122,84,.22); }}
    .pin-label.ground .pin-chip {{ background: #2a3238; border-color: #1a2024; color: #eef3f6; }}
    .pin-label.ground .pin-text {{ background: #3a444b; border-color: #2a3238; color: #f1f4f6; }}
    .pin-label.ground .pin-base {{ color: #f1f4f6; }}
    .pin-label.ground .pin-sep {{ color: #c8d0d6; }}
    .pin-label.power .pin-chip {{ background: #5a2f2f; border-color: #442222; color: #f7eaea; }}
    .pin-label.power .pin-text {{ background: #6b3a3a; border-color: #4f2a2a; color: #f8eeee; }}
    .pin-label.power .pin-base {{ color: #f8eeee; }}
    .pin-label.power .pin-sep {{ color: #f0d0d0; }}
    .pin-label.active .pin-chip {{ background: color-mix(in srgb, var(--accent) 14%, white); border-color: color-mix(in srgb, var(--accent) 35%, var(--line)); color: #9a330e; }}
    .pin-label.active .pin-text {{ background: color-mix(in srgb, var(--accent) 10%, white); border-color: color-mix(in srgb, var(--accent) 28%, rgba(90, 100, 110, 0.12)); }}
    .pin-base {{ color: #263138; }}
    .pin-assignment {{ color: #0f6a46; font-weight: 600; margin-left: 6px; }}
    .pin-sep {{ color: #7f8b94; margin: 0 4px; }}
    .board-wrap {{ position: relative; height: 808px; display: grid; place-items: center; }}
    .board-caption {{ position: absolute; left: 50%; top: -6px; transform: translateX(-50%); font-size: 12px; color: var(--muted); letter-spacing: .04em; text-transform: uppercase; }}
    .board-canvas {{ position: relative; width: 320px; height: 808px; }}
    .board-image, .board-overlay {{ position: absolute; inset: 0; width: 100%; height: 100%; display: block; }}
    .board-image {{ object-fit: contain; }}
    .pin-pad {{ fill: rgba(214,162,61,0.15); stroke: rgba(140,103,33,0.55); stroke-width: 0.45; transition: 140ms ease; }}
    .pin-pad.assigned {{ fill: rgba(22,122,84,0.22); stroke: rgba(22,122,84,0.7); stroke-width: 0.55; }}
    .pin-pad.ground {{ fill: rgba(33, 41, 47, 0.36); stroke: rgba(18, 22, 26, 0.9); stroke-width: 0.55; }}
    .pin-pad.power {{ fill: rgba(125, 55, 55, 0.34); stroke: rgba(74, 28, 28, 0.9); stroke-width: 0.55; }}
    .pin-pad.active {{ fill: rgba(255,106,61,.3); stroke: #963417; filter: drop-shadow(0 0 4px rgba(255,106,61,.45)); }}
    .lower {{ margin-top: 12px; display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }}
    .panel {{ background: rgba(255,255,255,.72); border: 1px solid rgba(90,100,110,.12); border-radius: 12px; padding: 10px 12px; }}
    .panel h2 {{ margin: 0 0 8px; font-size: 13px; text-transform: uppercase; letter-spacing: .04em; color: var(--muted); }}
    .panel ul {{ margin: 0; padding-left: 18px; }}
    .panel li {{ margin: 4px 0; font-size: 13px; }}
    .footnote {{ margin-top: 12px; font-size: 12px; color: var(--muted); }}
    @media (max-width: 900px) {{ .lower {{ grid-template-columns: 1fr; }} }}
  </style>
</head>
<body>
  <main class=\"page\">
    <section class=\"header\">
      <h1>Module Pinout: {html.escape(module_name)}</h1>
      <p>Generated from <code>g_gpio_assignments</code> in <code>modules/{html.escape(module_name)}/src/main.c</code>.</p>
      <div class=\"meta\">
        <span class=\"badge\">Assignments<strong>{len(assignments)}</strong></span>
        <span class=\"badge\">Output<strong>{html.escape(out_path.name)}</strong></span>
      </div>
    </section>

    <section class=\"shell\" aria-label=\"Pico pinout diagram\">
      <div class=\"stage\">
        <div class=\"labels left\" id=\"left-labels\"></div>
        <div class=\"board-wrap\">
          <div class=\"board-canvas\">
            <img class=\"board-image\" src=\"./raspberry-pi-pico.svg\" alt=\"Raspberry Pi Pico board\">
            <svg class=\"board-overlay\" viewBox=\"0 0 59.529 150.239\" role=\"img\" aria-label=\"Pico board with assigned pin highlights\">
              <g id=\"pads\"></g>
            </svg>
          </div>
        </div>
        <div class=\"labels right\" id=\"right-labels\"></div>
      </div>
    </section>

    <section class=\"lower\">
      <div class=\"panel\">
        <h2>GPIO Assignments</h2>
        <ul>
          {assignments_list_html}
        </ul>
      </div>
      <div class=\"panel\">
        <h2>Assigned Non-Header GPIOs</h2>
        <ul>
          {non_header_list_html}
        </ul>
      </div>
    </section>
  </main>

  <script>
    const LEFT_ROWS = {json.dumps(left_rows)};
    const RIGHT_ROWS = {json.dumps(right_rows)};
    const ASSIGNED_MARKER_PINS = new Set({json.dumps(assigned_marker_pins)});
    const DIM_RED_PINS = new Set([30, 35, 36, 37, 39, 40]);
    const GEOM = {{
      imageHeightPx: 808,
      viewBoxHeight: 150.239,
      rowStartY: 9.546,
      rowStep: 7.2,
      leftMarkerX: 1.2,
      rightMarkerX: 58.3,
      markerRadius: 1.7
    }};

    const padGroup = document.getElementById("pads");
    const leftLabels = document.getElementById("left-labels");
    const rightLabels = document.getElementById("right-labels");
    const labelEls = new Map();
    const padEls = new Map();

    function rowY(index) {{
      const viewBoxY = GEOM.rowStartY + index * GEOM.rowStep;
      return viewBoxY * (GEOM.imageHeightPx / GEOM.viewBoxHeight);
    }}

    function attachHover(el, physical) {{
      el.addEventListener("mouseenter", () => setActive(physical));
      el.addEventListener("mouseleave", clearActive);
      el.addEventListener("focus", () => setActive(physical));
      el.addEventListener("blur", clearActive);
      el.tabIndex = 0;
    }}

    function makeText(row) {{
      const base = document.createElement("span");
      base.className = "pin-base";
      base.textContent = row.base;

      const frag = document.createDocumentFragment();
      frag.appendChild(base);

      if (row.assigned.length) {{
        const sep = document.createElement("span");
        sep.className = "pin-sep";
        sep.textContent = "->";
        frag.appendChild(sep);

        const assignment = document.createElement("span");
        assignment.className = "pin-assignment";
        assignment.textContent = row.assigned.join(", ");
        frag.appendChild(assignment);
      }}
      return frag;
    }}

    function makeLabel(row, side, index) {{
      const wrap = document.createElement("div");
      wrap.className = "pin-label";
      if (row.assigned.length) wrap.classList.add("assigned");
      if (row.base.includes("GND")) wrap.classList.add("ground");
      if (DIM_RED_PINS.has(row.physical)) wrap.classList.add("power");
      wrap.dataset.physical = String(row.physical);
      wrap.style.top = `${{rowY(index) - 14}}px`;

      const chip = document.createElement("div");
      chip.className = "pin-chip";
      chip.textContent = String(row.physical);

      const text = document.createElement("div");
      text.className = "pin-text";
      text.appendChild(makeText(row));

      if (side === "left") {{
        wrap.append(text, chip);
      }} else {{
        wrap.append(chip, text);
      }}

      attachHover(wrap, row.physical);
      labelEls.set(row.physical, wrap);
      return wrap;
    }}

    function createPad(row, x, y) {{
      const ns = "http://www.w3.org/2000/svg";
      const pad = document.createElementNS(ns, "circle");
      pad.setAttribute("cx", String(x));
      pad.setAttribute("cy", String(y));
      pad.setAttribute("r", String(GEOM.markerRadius));
      pad.setAttribute("class", "pin-pad");
      if (ASSIGNED_MARKER_PINS.has(row.physical)) pad.classList.add("assigned");
      if (row.base.includes("GND")) pad.classList.add("ground");
      if (DIM_RED_PINS.has(row.physical)) pad.classList.add("power");
      pad.dataset.physical = String(row.physical);
      pad.style.cursor = "pointer";
      attachHover(pad, row.physical);
      padEls.set(row.physical, pad);
      padGroup.append(pad);
    }}

    function makePads() {{
      LEFT_ROWS.forEach((row, i) => createPad(row, GEOM.leftMarkerX, GEOM.rowStartY + i * GEOM.rowStep));
      RIGHT_ROWS.forEach((row, i) => createPad(row, GEOM.rightMarkerX, GEOM.rowStartY + i * GEOM.rowStep));
    }}

    function setActive(physical) {{
      clearActive();
      labelEls.get(physical)?.classList.add("active");
      padEls.get(physical)?.classList.add("active");
    }}

    function clearActive() {{
      document.querySelectorAll(".pin-label.active, .pin-pad.active").forEach(el => el.classList.remove("active"));
    }}

    LEFT_ROWS.forEach((row, i) => leftLabels.appendChild(makeLabel(row, "left", i)));
    RIGHT_ROWS.forEach((row, i) => rightLabels.appendChild(makeLabel(row, "right", i)));
    makePads();
  </script>
</body>
</html>
"""
    out_path.write_text(html_text, encoding="utf-8")


def main() -> int:
    args = parse_args()
    root = pathlib.Path(args.root).resolve()
    docs_dir = pathlib.Path(args.docs_dir)
    if not docs_dir.is_absolute():
        docs_dir = root / docs_dir

    mains = sorted(root.glob("modules/*/src/main.c"))
    if args.module:
        mains = [path for path in mains if path.parts[-3] == args.module]

    if not mains:
        if args.module:
            print(f"Module '{args.module}' not found under modules/*/src/main.c", file=sys.stderr)
        else:
            print("No module main.c files found under modules/*/src.", file=sys.stderr)
        return 1

    has_error = False
    for main_path in mains:
        module_name = main_path.parts[-3]
        assignments = parse_module_main(main_path)

        pins_to_signals: dict[int, list[str]] = {}
        for signal, pin in assignments:
            pins_to_signals.setdefault(pin, []).append(signal)
            if pin < args.gpio_min or pin > args.gpio_max:
                has_error = True
                print(
                    f"[ERROR] {module_name}: {signal} uses GPIO {pin}, "
                    f"outside allowed range {args.gpio_min}-{args.gpio_max}."
                )

        for pin, signals in sorted(pins_to_signals.items()):
            if len(signals) > 1:
                has_error = True
                print(f"[ERROR] {module_name}: GPIO {pin} collision: {', '.join(signals)}")

        if not args.quiet:
            used = sorted(pins_to_signals.keys())
            available = [
                pin
                for pin in range(args.gpio_min, args.gpio_max + 1)
                if pin not in pins_to_signals
            ]

            print(f"[INFO] {module_name}")
            if used:
                for pin in used:
                    print(f"  GPIO {pin}: {', '.join(pins_to_signals[pin])}")
            else:
                print("  No GPIO assignments found.")
            print(f"  Available GPIOs: {', '.join(str(pin) for pin in available)}")

        if args.generate_html:
            docs_dir.mkdir(parents=True, exist_ok=True)
            out_path = docs_dir / f"{module_name}_generated_pinout.html"
            generate_pinout_html(module_name, assignments, out_path)
            if not args.quiet:
                try:
                    shown = out_path.relative_to(root)
                except ValueError:
                    shown = out_path
                print(f"  Generated HTML: {shown}")

    return 1 if has_error else 0


if __name__ == "__main__":
    sys.exit(main())
