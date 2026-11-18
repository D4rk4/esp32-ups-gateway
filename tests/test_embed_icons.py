"""The inline status icons come from the vendored KDE Breeze files, through one tool."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import runpy
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest import mock
from xml.etree import ElementTree

ROOT = Path(os.path.abspath(__file__)).parents[1]
spec = importlib.util.spec_from_file_location("embed_icons", ROOT / "tools" / "embed_icons.py")
embed = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = embed
spec.loader.exec_module(embed)
fixture_spec = importlib.util.spec_from_file_location("make_ui_fixture", ROOT / "tools" / "make_ui_fixture.py")
make_ui_fixture = importlib.util.module_from_spec(fixture_spec)
fixture_spec.loader.exec_module(make_ui_fixture)

SVG = "http://www.w3.org/2000/svg"
BREEZE_COMMIT = "8cb887ed16c13fe5fb9b6609f04e8157a083e046"
# Git blob ids of the upstream files at BREEZE_COMMIT (icons/emblems/8/<name>, COPYING-ICONS).
UPSTREAM_BLOBS = {"emblem-checked.svg": "b1d2ad129cb347fd4d97b692ff315a7511821fae",
                  "emblem-warning.svg": "f4f4cf4feaeac420bd5e69427fd295345d3f07f8",
                  "emblem-error.svg": "c198b420bed935c76191e7101e90b95450054f69",
                  "emblem-information.svg": "751c61471829c1a5c54776c21a6995c9d33a09ba",
                  "COPYING-ICONS": "63d73657ce416e625bbaedf549fb2d9981616640"}
# sha256 of the unmodified texts from https://www.gnu.org/licenses/{lgpl,gpl}-3.0.txt.
LICENCE_SHA256 = {"COPYING.LESSER": "e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118",
                  "COPYING.GPL-3": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}
CONSEQUENCE = (
    "The project's own source is GPL-2.0-or-later; the embedded Breeze icons are LGPL-3.0-or-later, "
    "which cannot be combined under GNU GPL version 2 alone, so firmware images and release packages "
    "are distributed under GPL-3.0-or-later through the \"or later\" option of the project licence; "
    "the licence texts are `third_party/breeze-icons/COPYING-ICONS`, "
    "`third_party/breeze-icons/COPYING.LESSER` and `third_party/breeze-icons/COPYING.GPL-3`.")
COLOURS = {"good": "#27ae60", "warning": "#f67400", "critical": "#da4453", "info": "#3daee9"}
BREEZE_SOURCE = f"""<svg xmlns="{SVG}" viewBox="0 0 8 8">
  <defs id="defs3051">
    <style type="text/css" id="current-color-scheme">
      .ColorScheme-PositiveText {{
        color:#27ae60;
      }}
      </style>
  </defs>
  <g
     transform="translate(-7,-1037.3622)">
    <path
       style="fill:currentColor;fill-opacity:1;stroke:none"
       class="ColorScheme-PositiveText"
       d="M 4 0
          L 8 8 z" />
    <path style="fill:#ffffff" d="M 1 1 z" transform="translate(7,1037.3622)" id="rect4241" />
  </g>
</svg>
"""


def blob_id(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


class SymbolTests(unittest.TestCase):
    def source(self, body="", head=""):
        return f'<svg xmlns="{SVG}" viewBox="0 0 8 8">{head}{body}</svg>'

    def test_symbol_carries_the_literal_colour_and_nothing_that_depends_on_the_page(self):
        self.assertEqual(
            embed.symbol("good", BREEZE_SOURCE),
            '<symbol id="icon-good" viewBox="0 0 8 8">'
            '<g transform="translate(-7,-1037.3622)">'
            '<path style="fill:#27ae60;fill-opacity:1;stroke:none" d="M 4 0 L 8 8 z"/>'
            '<path style="fill:#ffffff" d="M 1 1 z" transform="translate(7,1037.3622)"/>'
            '</g></symbol>')

    def test_viewbox_comes_from_the_source(self):
        self.assertIn('viewBox="0 0 30 12"',
                      embed.symbol("good", BREEZE_SOURCE.replace('0 0 8 8', '0 0 30 12')))

    def test_style_class_defs_ids_and_namespaces_are_gone(self):
        markup = embed.symbol("good", BREEZE_SOURCE)
        for text in ("<style", "class", "currentColor", "<defs", "defs3051", "rect4241",
                     "current-color-scheme", "xmlns", "data:image/", "url("):
            self.assertNotIn(text, markup)
        self.assertEqual(re.findall(r' id="([^"]*)"', markup), ["icon-good"])

    def test_every_rule_of_the_style_element_is_applied_to_its_class(self):
        head = ('<defs><style>.A { color:#112233; } .B{color:#445566}</style></defs>')
        for classes, colour in (("A", "#112233"), ("B", "#445566")):
            with self.subTest(classes=classes):
                markup = embed.symbol(
                    "info", self.source(f'<path class="{classes}" fill="currentColor" d="M0 0"/>', head))
                self.assertIn(f'fill="{colour}"', markup)

    def test_whitespace_in_attribute_values_is_collapsed(self):
        self.assertIn('d="M 4 0 L 8 8 z"', embed.symbol("good", BREEZE_SOURCE))

    def test_element_without_children_is_self_closing_and_quotes_are_escaped(self):
        self.assertEqual(
            embed.symbol("good", self.source('<path d="a&quot;b&amp;c"/>')),
            '<symbol id="icon-good" viewBox="0 0 8 8"><path d="a&quot;b&amp;c"/></symbol>')

    def test_two_icons_never_share_an_id(self):
        ids = lambda role: set(re.findall(r' id="([^"]+)"', embed.symbol(role, BREEZE_SOURCE)))
        self.assertEqual(ids("good") & ids("info"), set())

    def test_source_must_be_an_svg_with_a_viewbox(self):
        for source in (f'<html xmlns="{SVG}"/>', f'<svg xmlns="{SVG}" width="8" height="8"/>'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                embed.symbol("good", source)

    def test_an_svg_without_a_viewbox_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "viewBox"):
            embed.symbol("good", f'<svg xmlns="{SVG}"><path d="M0 0"/></svg>')

    def test_active_external_or_unneeded_content_is_rejected(self):
        for body in ("<script>alert(1)</script>", '<image href="data:image/png;base64,AA=="/>',
                     "<foreignObject/>", "<linearGradient/>", '<use href="#a"/>',
                     "<defs><linearGradient/></defs>", "<style>.a{}</style>",
                     # a foreign namespace whose URI is as long as the SVG one is still foreign
                     '<path xmlns="http://www.w3.org/2000/svx" d="M0 0"/>',
                     '<g xmlns="http://www.w3.org/2000/svx"><path d="M0 0"/></g>'):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "unsupported"):
                embed.symbol("good", self.source(body))

    def test_style_rules_other_than_a_class_colour_are_rejected(self):
        for css in (".a{fill:red}", ".a{color:red}", "path{color:#112233}", ".a{color:#112233;fill:red}",
                    ".a{color:#112233}@import url(x);"):
            with self.subTest(css=css), self.assertRaisesRegex(ValueError, "unsupported"):
                embed.symbol("good", self.source("", f"<defs><style>{css}</style></defs>"))

    def test_several_classes_on_one_element_are_rejected_rather_than_guessed(self):
        head = "<defs><style>.A{color:#112233}.B{color:#445566}</style></defs>"
        with self.assertRaisesRegex(ValueError, "unsupported"):
            embed.symbol("good", self.source('<path class="A B" fill="currentColor" d="M0 0"/>', head))

    def test_class_without_a_colour_rule_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "nowhere"):
            embed.symbol("good", self.source('<path class="nowhere" d="M0 0"/>'))

    def test_references_and_namespaced_attributes_are_rejected(self):
        for element in ('<path fill="url(#g)"/>', '<path style="fill:url(#g)"/>',
                        '<path xmlns:i="urn:x" i:label="x"/>'):
            with self.subTest(element=element), self.assertRaisesRegex(ValueError, "unsupported"):
                embed.symbol("good", self.source(element))

    def test_event_handler_attributes_are_rejected_on_the_root_and_on_children(self):
        for element in (f'<svg xmlns="{SVG}" viewBox="0 0 8 8" onload="alert(1)"/>',
                        self.source('<path onclick="alert(1)" d="M0 0"/>'),
                        self.source('<g onmouseover="alert(1)"><path d="M0 0"/></g>')):
            with self.subTest(element=element), self.assertRaisesRegex(ValueError, "unsupported attribute"):
                embed.symbol("good", element)

    def test_links_and_other_attributes_outside_the_allow_list_are_rejected(self):
        for attribute in ('href="https://example.com/x"', 'xlink:href="#a" xmlns:xlink="urn:x"',
                          'src="x"', 'filter="none"', 'clip-path="none"', 'style-x="a"'):
            with self.subTest(attribute=attribute), self.assertRaisesRegex(ValueError, "unsupported attribute"):
                embed.symbol("good", self.source(f'<path {attribute} d="M0 0"/>'))

    def test_style_properties_outside_the_allow_list_are_rejected(self):
        for style in ("position:fixed", "fill:#fff;position:fixed", "behavior:x", "fill", ":red",
                      "fill:#fff;;;color:red", "pointer-events:none"):
            with self.subTest(style=style), self.assertRaisesRegex(ValueError, "unsupported style"):
                embed.symbol("good", self.source(f'<path style="{style}" d="M0 0"/>'))

    def test_style_and_paint_values_cannot_hide_a_reference(self):
        for value in ("URL(#g)", "uRl( #g )", "url(#g) red", "\\75rl(#g)", "'x'"):
            for markup in (f'<path fill="{value}" d="M0 0"/>', f'<path style="fill:{value}" d="M0 0"/>'):
                with self.subTest(markup=markup), self.assertRaisesRegex(ValueError, "unsupported"):
                    embed.symbol("good", self.source(markup))

    def test_paint_values_are_ascii_only(self):
        for markup in ('<path fill="ред" d="M0 0"/>', '<path style="fill:ред" d="M0 0"/>'):
            with self.subTest(markup=markup), self.assertRaisesRegex(ValueError, "unsupported style"):
                embed.symbol("good", self.source(markup))

    def test_a_paint_attribute_cannot_smuggle_a_second_declaration(self):
        with self.assertRaisesRegex(ValueError, "unsupported style"):
            embed.symbol("good", self.source('<path fill="red;position:fixed" d="M0 0"/>'))

    def test_url_is_rejected_in_any_value_whatever_its_case(self):
        for attribute in ('d="URL(#g)"', 'transform="Url(x)"', 'opacity="uRL(#g)"'):
            with self.subTest(attribute=attribute), self.assertRaisesRegex(ValueError, "unsupported"):
                embed.symbol("good", self.source(f"<path {attribute}/>"))

    def test_current_colour_that_no_class_resolves_is_rejected(self):
        head = "<defs><style>.A{color:#112233}</style></defs>"
        for body in ('<g class="A"><path style="fill:currentColor" d="M0 0"/></g>',
                     '<path fill="currentColor" d="M0 0"/>', '<path stroke="CURRENTCOLOR" d="M0 0"/>',
                     '<path style="stroke:currentcolor" d="M0 0"/>'):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "unsupported.*currentColor"):
                embed.symbol("good", self.source(body, head))

    def test_a_doctype_is_rejected_before_it_can_define_or_expand_entities(self):
        bomb = f'''<!DOCTYPE svg [<!ENTITY a "AAAA">]><svg xmlns="{SVG}" viewBox="0 0 8 8">
        <path d="&a;"/></svg>'''
        for source in (bomb, bomb.replace("DOCTYPE", "doctype"), f'<!DOCTYPE svg>{self.source("")}'):
            with self.subTest(source=source[:30]), self.assertRaisesRegex(ValueError, "unsupported.*DOCTYPE"):
                embed.symbol("good", source)


class VendoredIconTests(unittest.TestCase):
    DIRECTORY = ROOT / "third_party" / "breeze-icons"

    def test_roles_map_to_the_breeze_emblems_in_page_order(self):
        self.assertEqual(embed.ROLES, {"good": "emblem-checked", "warning": "emblem-warning",
                                       "critical": "emblem-error", "info": "emblem-information"})
        self.assertEqual(embed.VENDOR, "third_party/breeze-icons")
        self.assertEqual(embed.FALLBACK, "info")

    def test_files_are_byte_identical_to_the_upstream_blobs(self):
        for name, blob in UPSTREAM_BLOBS.items():
            with self.subTest(name=name):
                self.assertEqual(blob_id((self.DIRECTORY / name).read_bytes()), blob)

    def test_licence_texts_and_provenance_ship_with_the_files(self):
        lesser = (self.DIRECTORY / "COPYING.LESSER").read_text(encoding="utf-8")
        self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", lesser)
        self.assertIn("Version 3, 29 June 2007", lesser)
        notice = (self.DIRECTORY / "COPYING-ICONS").read_text(encoding="utf-8")
        self.assertIn("Copyright (C) 2014 Uri Herrera", notice)
        self.assertIn("version 3 of the License, or (at your option) any later version", notice)
        readme = (self.DIRECTORY / "README.md").read_text(encoding="utf-8")
        for text in (BREEZE_COMMIT, "https://github.com/KDE/breeze-icons", *UPSTREAM_BLOBS,
                     *(f"icons/emblems/8/{name}.svg" for name in embed.ROLES.values())):
            self.assertIn(text, readme)

    def test_gpl_and_lgpl_texts_are_the_unmodified_gnu_files(self):
        for name, digest in LICENCE_SHA256.items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((self.DIRECTORY / name).read_bytes()).hexdigest(), digest)
        self.assertIn("Version 3, 29 June 2007",
                      (self.DIRECTORY / "COPYING.GPL-3").read_text(encoding="utf-8"))

    def test_docs_name_every_licence_file_and_state_the_same_consequence(self):
        flat = lambda path: " ".join(path.read_text(encoding="utf-8").split())
        for path in (ROOT / "README.md", ROOT / "THIRD_PARTY.md"):
            with self.subTest(path=path.name):
                self.assertIn(CONSEQUENCE, flat(path))
        for path in (ROOT / "THIRD_PARTY.md", self.DIRECTORY / "README.md"):
            with self.subTest(path=path.name):
                self.assertIn("COPYING.GPL-3", flat(path))

    def test_no_other_vendor_directory_remains(self):
        self.assertEqual([path.name for path in (ROOT / "third_party").iterdir()], ["breeze-icons"])

    def test_every_role_becomes_a_self_contained_symbol_with_its_literal_colour(self):
        sources = embed.load_sources(ROOT)
        self.assertEqual(list(sources), ["good", "warning", "critical", "info"])
        for role, text in sources.items():
            with self.subTest(role=role):
                markup = embed.symbol(role, text)
                self.assertTrue(markup.startswith(f'<symbol id="icon-{role}" viewBox="0 0 8 8">'))
                self.assertIn(f"fill:{COLOURS[role]};", markup)
                for forbidden in ("<style", "class", "currentColor", "url(", "href", "<defs",
                                  "data:image/", "xmlns"):
                    self.assertNotIn(forbidden, markup)
                self.assertEqual(re.findall(r' id="([^"]*)"', markup), [f"icon-{role}"])
                ElementTree.fromstring(markup.replace("<symbol", f'<symbol xmlns="{SVG}"', 1))


class RenderTests(unittest.TestCase):
    PAGE = ("<body><!--icons:begin-->old<!--icons:end--><div id=\"main\"></div>"
            "<script>/*icons:begin*/function icon(){}/*icons:end*/</script></body>")
    SOURCES = {role: f'<svg viewBox="0 0 8 8" xmlns="{SVG}"><path d="M0 0"/></svg>'
               for role in embed.ROLES}

    def test_sprite_stays_in_the_render_tree_and_is_hidden_from_assistive_tech(self):
        page = embed.render(self.PAGE, self.SOURCES)
        sprite = re.search(r"<svg[^>]*>", page).group(0)
        self.assertIn('width="0"', sprite)
        self.assertIn('height="0"', sprite)
        self.assertIn("position:absolute", sprite)
        self.assertIn('aria-hidden="true"', sprite)
        self.assertNotIn("display:none", page)
        self.assertEqual(re.findall(r'<symbol id="([^"]+)"', page),
                         ["icon-good", "icon-warning", "icon-critical", "icon-info"])

    def test_modification_notice_sits_inside_the_generated_region(self):
        notice = "<!-- Icons: KDE Breeze, LGPL-3.0-or-later, class colours inlined; see THIRD_PARTY.md -->"
        page = embed.render(self.PAGE, self.SOURCES)
        region = page.split(embed.HTML_REGION[0])[1].split(embed.HTML_REGION[1])[0]
        self.assertTrue(region.startswith(notice), region[:120])
        self.assertEqual(page.count(notice), 1)

    def test_icon_function_points_at_the_symbols_and_falls_back_to_info(self):
        page = embed.render(self.PAGE, self.SOURCES)
        script = re.search(r"<script>(.*)</script>", page, re.S).group(1)
        self.assertIn('["good","warning","critical","info"]', script)
        self.assertIn('"info"', script)
        self.assertIn('<use href="#icon-', script)
        self.assertEqual(script.count("<symbol"), 0, "markup is defined once, in the sprite")

    def test_render_is_idempotent_and_replaces_stale_content(self):
        once = embed.render(self.PAGE, self.SOURCES)
        self.assertNotIn("old", once)
        self.assertNotIn("function icon(){}", once)
        self.assertEqual(embed.render(once, self.SOURCES), once)

    def test_each_region_must_appear_exactly_once_in_order(self):
        for page in ("<body></body>",
                     self.PAGE.replace("<!--icons:end-->", ""),
                     self.PAGE + "<!--icons:begin--><!--icons:end-->",
                     self.PAGE + "<!--icons:begin-->",
                     self.PAGE + "<!--icons:end-->",
                     self.PAGE.replace("<!--icons:begin-->", ""),
                     self.PAGE.replace("<!--icons:begin-->old<!--icons:end-->",
                                       "<!--icons:end-->old<!--icons:begin-->")):
            with self.subTest(page=page), self.assertRaisesRegex(ValueError, "exactly one"):
                embed.render(page, self.SOURCES)

    def test_backslashes_in_icon_markup_survive_substitution(self):
        sources = dict(self.SOURCES)
        sources["good"] = f'<svg viewBox="0 0 8 8" xmlns="{SVG}"><path d="a\\1b"/></svg>'
        self.assertIn('d="a\\1b"', embed.render(self.PAGE, sources))


class MainTests(unittest.TestCase):
    def make_root(self, directory):
        root = Path(directory)
        (root / "web").mkdir()
        (root / "web/index.html").write_bytes(RenderTests.PAGE.encode())
        vendor = root / embed.VENDOR
        vendor.mkdir(parents=True)
        for file in embed.ROLES.values():
            (vendor / f"{file}.svg").write_text(RenderTests.SOURCES["good"])
        return root

    def test_running_twice_leaves_the_page_byte_identical(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.make_root(directory)
            page = root / "web/index.html"
            output = StringIO()
            with redirect_stdout(output):
                self.assertEqual(embed.main(root), 0)
            self.assertIn("regenerated", output.getvalue())
            first = page.read_bytes()
            self.assertIn(b"<symbol", first)
            before = page.stat().st_mtime_ns
            output = StringIO()
            with redirect_stdout(output):
                self.assertEqual(embed.main(root), 0)
            self.assertIn("already up to date", output.getvalue())
            self.assertEqual(page.read_bytes(), first)
            self.assertEqual(page.stat().st_mtime_ns, before, "an up-to-date page is not rewritten")


class SyncTests(unittest.TestCase):
    def test_production_page_matches_what_the_tool_generates_from_the_vendored_files(self):
        page = (ROOT / "web/index.html").read_bytes().decode("utf-8")
        self.assertEqual(
            embed.render(page, embed.load_sources(ROOT)), page,
            "web/index.html is stale: run python tools/embed_icons.py")

    def test_script_entry_point_leaves_an_up_to_date_page_untouched(self):
        with mock.patch.object(Path, "write_bytes", side_effect=AssertionError("page rewritten")), \
             redirect_stdout(StringIO()), self.assertRaises(SystemExit) as raised:
            runpy.run_path(str(ROOT / "tools" / "embed_icons.py"), run_name="__main__")
        self.assertEqual(raised.exception.code, 0)

    def test_committed_fixture_is_what_make_ui_fixture_builds_from_the_production_page(self):
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            built = Path(directory) / "ui_fixture.html"
            make_ui_fixture.make(built)
            self.assertEqual(built.read_bytes(), (ROOT / "tests/ui_fixture.html").read_bytes(),
                             "tests/ui_fixture.html is stale: run python tools/make_ui_fixture.py")

    def test_notice_is_in_the_production_page_and_in_the_fixture(self):
        for name in ("web/index.html", "tests/ui_fixture.html"):
            with self.subTest(name=name):
                text = (ROOT / name).read_text(encoding="utf-8")
                region = text.split(embed.HTML_REGION[0])[1].split(embed.HTML_REGION[1])[0]
                self.assertRegex(region, r"^<!-- Icons: KDE Breeze, LGPL-3\.0-or-later,.*THIRD_PARTY\.md -->")

    def test_no_data_image_uri_anywhere_in_the_ui(self):
        for name in ("web/index.html", "tests/ui_fixture.html"):
            with self.subTest(name=name):
                self.assertNotIn("data:image/", (ROOT / name).read_text(encoding="utf-8"))

    def test_sprite_in_the_page_and_in_the_fixture_has_literal_colours_only(self):
        for name in ("web/index.html", "tests/ui_fixture.html"):
            with self.subTest(name=name):
                text = (ROOT / name).read_text(encoding="utf-8")
                sprite = text.split(embed.HTML_REGION[0])[1].split(embed.HTML_REGION[1])[0]
                self.assertEqual(re.findall(r'<symbol id="([^"]+)"', sprite),
                                 ["icon-good", "icon-warning", "icon-critical", "icon-info"])
                for colour in COLOURS.values():
                    self.assertEqual(sprite.count(f"fill:{colour};"), 1)
                for forbidden in ("<style", "class=", "currentColor", "url(", "<defs", "<linearGradient"):
                    self.assertNotIn(forbidden, sprite)


if __name__ == "__main__":
    unittest.main()
