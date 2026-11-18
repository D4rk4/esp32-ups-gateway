#!/usr/bin/env python3
"""Inline the vendored KDE Breeze icons (third_party/breeze-icons) into web/index.html.

This tool is the only place that decides which icon serves which status and how the
source SVG is cleaned. It rewrites two marked regions of web/index.html: the sprite
near the start of <body> and the icon() function. Running it twice leaves the page
byte-identical. Edit the regions through this tool, never by hand:
tests/test_embed_icons.py fails when the page is stale.

A Breeze emblem colours its disc with `fill:currentColor` and a class whose colour a
<style> inside <defs> defines. A <style> pasted into the page would apply to the whole
document, so the colour is written into the markup and the class and <style> are dropped.
"""
from pathlib import Path
import json
import re
from xml.etree import ElementTree
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
VENDOR = "third_party/breeze-icons"
ROLES = {"good": "emblem-checked", "warning": "emblem-warning",
         "critical": "emblem-error", "info": "emblem-information"}
FALLBACK = "info"
SVG = "{http://www.w3.org/2000/svg}"
ALLOWED = {"g", "path"}
# Anything else in a source file is refused, never passed through: the page's CSP allows
# inline script, so an on* attribute or a link would run or load in the owner's browser.
PAINT = {"fill", "fill-opacity", "stroke", "stroke-width", "stroke-opacity", "opacity"}
ATTRIBUTES = PAINT | {"viewBox", "class", "id", "style", "d", "transform"}
PAINT_VALUE = r"[A-Za-z0-9_#.%-]+"  # a colour, number or keyword: ASCII only; no url(), quotes, escapes or separators
CLASS_COLOUR = r"\.([\w-]+)\s*\{\s*color\s*:\s*(#[0-9a-fA-F]{6})\s*;?\s*\}"
HTML_REGION = ("<!--icons:begin-->", "<!--icons:end-->")
SCRIPT_REGION = ("/*icons:begin*/", "/*icons:end*/")
# LGPL-3.0 section 4(a): the page carries a modified copy of the icons and says so.
NOTICE = "<!-- Icons: KDE Breeze, LGPL-3.0-or-later, class colours inlined; see THIRD_PARTY.md -->"


def _colours(root):
    """Class -> colour from the <style> elements; any other style content is refused."""
    colours = {}
    for style in root.iter(SVG + "style"):
        text = style.text or ""
        if not re.fullmatch(rf"\s*(?:{CLASS_COLOUR}\s*)+", text):
            raise ValueError("unsupported style: only '.class { color: #rrggbb }' rules are read")
        colours.update(re.findall(CLASS_COLOUR, text))
    return colours


def _paint(name, value):
    if name not in PAINT or not re.fullmatch(PAINT_VALUE, value):
        raise ValueError(f"unsupported style: {name}:{value}")
    return value


def _style(text):
    """prop:value pairs, each from the PAINT allow-list; rewritten without stray whitespace."""
    pairs = (part.partition(":") for part in text.split(";") if part.strip())
    return ";".join(f"{name.strip()}:{_paint(name.strip(), value.strip())}" for name, _, value in pairs)


def _value(key, value):
    """One attribute: an allow-listed name and a value that references nothing."""
    if key not in ATTRIBUTES:
        raise ValueError(f"unsupported attribute: {key}")
    value = " ".join(value.split())
    if "url(" in value.lower():
        raise ValueError(f"unsupported value: {key}={value}")
    if key == "style":
        return _style(value)
    return _paint(key, value) if key in PAINT else value


def _attributes(element, colours):
    """Allow-listed attributes only. A class is replaced by its colour wherever currentColor is used."""
    classes = element.get("class", "").split()
    if len(classes) > 1:  # which rule wins would depend on stylesheet order; not needed here
        raise ValueError("unsupported attribute: more than one class")
    missing = [name for name in classes if name not in colours]
    if missing:
        raise ValueError("class without a colour rule: " + ", ".join(missing))
    result = {}
    for key, value in element.attrib.items():
        if key in ("class", "id"):  # nothing references an id; the class is resolved below
            continue
        value = _value(key, value)
        if classes:
            value = re.sub("currentColor", colours[classes[0]], value, flags=re.I)
        result[key] = value
    return result


def _serialize(attributes):
    return "".join(f' {key}="{escape(value, {chr(34): "&quot;"})}"' for key, value in attributes.items())


def _kept(element):
    # <style> was read by _colours; a <defs> that held only styles is empty now.
    return [child for child in element if child.tag != SVG + "style"
            and not (child.tag == SVG + "defs" and all(c.tag == SVG + "style" for c in child))]


def _emit(element, colours):
    name = element.tag.removeprefix(SVG)
    if element.tag != SVG + name or name not in ALLOWED:  # a foreign namespace keeps its prefix
        raise ValueError(f"unsupported SVG element: {name}")
    attributes = _serialize(_attributes(element, colours))
    children = "".join(_emit(child, colours) for child in _kept(element))
    return f"<{name}{attributes}>{children}</{name}>" if children else f"<{name}{attributes}/>"


def symbol(role, svg):
    """One source SVG -> one <symbol>. Pure: no file or page access."""
    if "<!doctype" in svg.lower():  # an internal entity would be expanded silently
        raise ValueError("unsupported: DOCTYPE")
    root = ElementTree.fromstring(svg)  # prolog and comments never reach the tree
    if root.tag != SVG + "svg":
        raise ValueError("not an SVG document")
    colours = _colours(root)
    attributes = _attributes(root, colours)
    if "viewBox" not in attributes:
        raise ValueError("SVG needs a viewBox")
    inner = "".join(_emit(child, colours) for child in _kept(root))
    markup = "<symbol" + _serialize({"id": f"icon-{role}", **attributes}) + ">" + inner + "</symbol>"
    if "currentcolor" in markup.lower():  # no class resolved it: it would take the page's colour
        raise ValueError("unsupported: currentColor without a class colour")
    return markup


def sprite(sources):
    symbols = "".join(symbol(role, sources[role]) for role in ROLES)
    # Never display:none: some browsers skip painting inside a display:none subtree.
    return ('<svg width="0" height="0" style="position:absolute" aria-hidden="true" '
            'focusable="false">' + symbols + "</svg>")


def icon_function():
    return ("function icon(name){const id=" + json.dumps(list(ROLES), separators=(",", ":"))
            + ".includes(name)?name:" + json.dumps(FALLBACK) + ";return '<svg width=\"14\" height=\"14\" "
            "aria-hidden=\"true\" focusable=\"false\"><use href=\"#icon-'+id+'\"/></svg>';}")


def _splice(text, begin, end, content):
    head, _, rest = text.partition(begin)
    if text.count(begin) != 1 or text.count(end) != 1 or end not in rest:
        raise ValueError(f"web/index.html needs exactly one {begin} ... {end} region")
    return head + begin + content + end + rest.split(end)[1]


def render(html, sources):
    return _splice(_splice(html, *HTML_REGION, NOTICE + sprite(sources)), *SCRIPT_REGION, icon_function())


def load_sources(root):
    return {role: (root / VENDOR / f"{file}.svg").read_text(encoding="utf-8")
            for role, file in ROLES.items()}


def main(root=ROOT):
    page = root / "web/index.html"
    html = page.read_bytes().decode("utf-8")
    updated = render(html, load_sources(root))
    changed = updated != html
    if changed:
        page.write_bytes(updated.encode("utf-8"))
    print(f"{page}: {'regenerated' if changed else 'already up to date'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
