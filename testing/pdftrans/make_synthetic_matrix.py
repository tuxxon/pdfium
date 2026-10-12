#!/usr/bin/env python3
"""Deterministic generator of small hand-built PDFs plus manifest.json.

Usage: python3 -I make_synthetic_matrix.py --out-dir DIR
Writes DIR/<case_id>.pdf for every case and DIR/manifest.json, then re-parses
every xref table and asserts each offset points at "N 0 obj".
"""
import argparse
import json
import os
import re
import sys
import zlib

WIN = "/Encoding /WinAnsiEncoding"
PAINT = ("q 0.85 0.9 1 rg 10 170 280 20 re f 0 0 1 RG 1 w "
         "10 10 280 180 re S Q\n")


def b(x):
    return x if isinstance(x, bytes) else x.encode("latin-1")


class Doc:
    """Object 1 = Catalog, 2 = Pages, 3..5 = standard-14 fonts."""

    def __init__(self):
        self.objs = [None, None, None]  # index == object number
        for name in ("Helvetica", "Times-Roman", "Courier"):
            self.add("<< /Type /Font /Subtype /Type1 /BaseFont /%s %s >>"
                     % (name, WIN))
        self.kids = []
        self.pages_attrs = ""
        self.fonts_extra = ""
        self.xobjs = ""

    def add(self, body):
        self.objs.append(b(body))
        return len(self.objs) - 1

    def stream(self, data, extra=""):
        data = b(data)
        return self.add(b("<< /Length %d %s >>\nstream\n" % (len(data), extra))
                        + data + b"\nendstream")

    def resources(self):
        r = "<< /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R %s >>" % self.fonts_extra
        if self.xobjs:
            r += " /XObject << %s >>" % self.xobjs
        return r + " >>"

    def page(self, streams, attrs="", media=True, res=True):
        refs = [s if isinstance(s, int) else self.stream(s) for s in streams]
        c = ("%d 0 R" % refs[0]) if len(refs) == 1 else \
            "[" + " ".join("%d 0 R" % r for r in refs) + "]"
        p = "<< /Type /Page /Parent 2 0 R /Contents %s " % c
        if media:
            p += "/MediaBox [0 0 300 200] "
        if res:
            p += "/Resources %s " % self.resources()
        n = self.add(p + attrs + " >>")
        self.kids.append(n)
        return n

    def render(self):
        self.objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
        self.objs[2] = b("<< /Type /Pages /Kids [%s] /Count %d %s >>" % (
            " ".join("%d 0 R" % k for k in self.kids), len(self.kids),
            self.pages_attrs))
        out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
        offs = [0]
        for n in range(1, len(self.objs)):
            offs.append(len(out))
            out += b("%d 0 obj\n" % n) + self.objs[n] + b"\nendobj\n"
        xref = len(out)
        out += b("xref\n0 %d\n0000000000 65535 f \n" % len(self.objs))
        for o in offs[1:]:
            out += b("%010d 00000 n \n" % o)
        out += b("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
                 % (len(self.objs), xref))
        return bytes(out)


def check_pdf(path):
    """Re-parse the xref table of a written file; assert offsets are right."""
    data = open(path, "rb").read()
    assert data.startswith(b"%PDF-1.7\n%"), path
    assert data.endswith(b"%%EOF\n"), path
    m = re.search(rb"startxref\n(\d+)\n%%EOF\n$", data)
    assert m, "%s: no startxref" % path
    pos = int(m.group(1))
    assert data[pos:pos + 5] == b"xref\n", "%s: startxref not at xref" % path
    m = re.compile(rb"xref\n0 (\d+)\n").match(data, pos)
    assert m, path
    count, p = int(m.group(1)), m.end()
    lines = [data[p + 20 * i:p + 20 * i + 20] for i in range(count)]
    assert lines[0] == b"0000000000 65535 f \n", path
    for num in range(1, count):
        ln = lines[num]
        assert re.fullmatch(rb"\d{10} 00000 n \n", ln), (path, num, ln)
        off = int(ln[:10])
        assert data[off:].startswith(b("%d 0 obj\n" % num)), \
            "%s: xref offset for obj %d is wrong" % (path, num)
    tr = data[p + 20 * count:]
    assert tr.startswith(b"trailer\n<<"), path
    assert re.search(rb"/Size %d\b" % count, tr) and b"/Root 1 0 R" in tr, path
    return count


CASES = []


def case(cid):
    def deco(fn):
        CASES.append((cid, fn))
        return fn
    return deco


def R(sel, expect="SUCCESS", notes="", page=0, other=()):
    return dict(select=list(sel), expect=expect, notes=notes, page=page,
                otherPages=list(other))


BASIC = PAINT + ("BT /F1 14 Tf 40 150 Td (ERASE) Tj 0 -30 Td (KEEP) Tj ET\n")


@case("tj_basic")
def _(d):
    d.page([BASIC])
    return R(["ERASE"], notes="Plain Tj removal; KEEP placed by its own Td.")


@case("tj_kerning")
def _(d):
    d.page([PAINT + "BT /F1 14 Tf 40 150 Td [(ER) -120 (A) 80 (SE)] TJ "
            "0 -30 Td [(KE) -50 (EP)] TJ ET\n"])
    return R(["ERASE"], notes="TJ with kerning; PDFium text of the object is "
             "expected to be the concatenated strings 'ERASE' (kerning "
             "numbers not part of text) - verify.")


@case("tc_tw")
def _(d):
    d.page([PAINT + "BT /F1 12 Tf 14 TL 40 150 Td 1.5 Tc 4 Tw "
            "(ERASE TWO WORDS) Tj T* (KEEP WITH SPACES) Tj ET\n"])
    return R(["ERASE TWO WORDS"], notes="Tc/Tw set; T* resets after removal.")


@case("text_moves")
def _(d):
    d.page([PAINT + "BT /F1 12 Tf 14 TL 40 170 Td (ERASE) Tj 0 -20 Td (KEEP) Tj\n"
            "1 0 0 1 40 120 Tm (ERASE TWO) Tj 0 -20 TD (KEEP 2) Tj\n"
            "T* (ERASE THREE) Tj T* (KEEP 3) Tj ET\n"])
    return R(["ERASE", "ERASE TWO", "ERASE THREE"],
             notes="Removals each followed by Td / TD / T* / Tm resets; "
             "multiple objects removed together.")


@case("escapes")
def _(d):
    d.page([PAINT + "BT /F1 12 Tf 40 170 Td (ER\\(A\\)SE\\\\) Tj 0 -20 Td "
            "(KEEP \\(1\\)) Tj 0 -20 Td (ERASE\\053) Tj 0 -20 Td "
            "<45524153452032> Tj 0 -20 Td (KEEP \\\\ 2) Tj 0 -20 Td "
            "(\\105\\122ASE ONE) Tj ET\n"])
    return R(["ER(A)SE\\", "ERASE+", "ERASE 2", "ERASE ONE"],
             notes="Literal escapes \\( \\) \\\\, octal \\053 and \\105\\122, "
             "hex string. Selected strings are the decoded text (WinAnsi, "
             "ASCII) - verify PDFium output for 'ER(A)SE\\'.")


@case("multi_stream")
def _(d):
    d.page([PAINT, "BT /F1 14 Tf 40 150 Td (ERASE) Tj ET\n",
            "BT /F1 14 Tf 40 120 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="/Contents array of three streams.")


@case("shared_stream")
def _(d):
    s = d.stream(BASIC)
    d.page([s])
    d.page([s])
    return R(["ERASE"], other=[1],
             notes="Both pages reference the same content stream object; "
             "page 1 still contains ERASE and must stay identical.")


for _rot in (90, 180, 270):
    def _mk(rot):
        @case("rotate_%d" % rot)
        def _(d):
            d.page([BASIC], "/Rotate %d" % rot)
            return R(["ERASE"], notes="/Rotate %d on the page." % rot)
    _mk(_rot)


@case("cropbox_offset")
def _(d):
    d.page([BASIC], "/CropBox [20 10 280 190]")
    return R(["ERASE"], notes="CropBox offset inside MediaBox.")


@case("userunit_2")
def _(d):
    d.page([BASIC], "/UserUnit 2")
    return R(["ERASE"], notes="/UserUnit 2.")


@case("inherited_attrs")
def _(d):
    d.pages_attrs = "/MediaBox [0 0 300 200] /Rotate 90 /Resources %s" \
        % d.resources()
    d.page([BASIC], media=False, res=False)
    return R(["ERASE"], notes="MediaBox, Rotate, Resources inherited from "
             "the Pages node.")


def add_form(d, text, name="Fm1"):
    n = d.stream("0.9 0.9 0.6 rg 150 20 100 40 re f 0 g BT /F1 12 Tf "
                 "160 35 Td (%s) Tj ET\n" % text,
                 "/Type /XObject /Subtype /Form /BBox [0 0 300 200] "
                 "/Resources << /Font << /F1 3 0 R >> >>")
    d.xobjs += "/%s %d 0 R " % (name, n)
    return n


@case("form_retained")
def _(d):
    add_form(d, "KEEP FORM")
    d.page(["q 1 0 0 1 0 0 cm /Fm1 Do Q\n" + BASIC])
    return R(["ERASE"], notes="Form XObject (own text + rect) drawn before "
             "the BT block; its text is retained.")


@case("form_text_selected")
def _(d):
    add_form(d, "INSIDE FORM")
    d.page(["q 1 0 0 1 0 0 cm /Fm1 Do Q\n" + PAINT +
            "BT /F1 14 Tf 40 150 Td (KEEP) Tj ET\n"])
    return R(["INSIDE FORM"], "NOT_SELECTABLE",
             "String exists only inside a Form XObject.")


@case("do_after_taint")
def _(d):
    add_form(d, "KEEP FORM")
    d.page([PAINT + "BT /F1 14 Tf 40 100 Td (KEEP) Tj 0 50 Td (ERASE) Tj ET\n"
            "/Fm1 Do\n"])
    return R(["ERASE"], "kTextPositionDependency",
             "Do while cursor tainted (after ET, no reset).")


@case("clip_path")
def _(d):
    d.page(["q 10 10 200 100 re W n BT /F1 14 Tf 40 80 Td (ERASE) Tj "
            "0 -30 Td (KEEP) Tj ET Q\n"
            "q 150 120 100 60 re W n 0.2 0.6 0.2 rg 120 100 150 90 re f Q\n"
            "0 0 1 RG 10 10 280 180 re S\n"])
    return R(["ERASE"], notes="Text inside a rectangular clip, plus a "
             "clipped path.")


@case("clip_text_mode")
def _(d):
    d.page(["q BT /F1 24 Tf 40 130 Td 7 Tr (ERASE) Tj 0 Tr 0 -40 Td "
            "(KEEP) Tj ET 0.8 0.2 0.2 rg 0 100 300 60 re f Q\n"])
    return R(["ERASE"], "kUnsupportedText", "Selected text uses Tr 7.")


@case("cmyk")
def _(d):
    d.page(["0.1 0.8 0.2 0 k 10 170 280 20 re f 0 0 0 1 K 2 w "
            "10 10 280 180 re S 0.5 0 1 0 k 200 60 60 60 re f\n"
            "BT /F1 14 Tf 1 0 0 0.2 k 40 150 Td (ERASE) Tj 0 -30 Td "
            "0 1 1 0 k (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="CMYK fills/strokes for text and paths.")


@case("figure_image")
def _(d):
    img = bytes(v for y in range(4) for x in range(4)
                for v in ((x * 80) % 256, (y * 80) % 256, 128))
    n = d.stream(img, "/Type /XObject /Subtype /Image /Width 4 /Height 4 "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8")
    d.xobjs += "/Im1 %d 0 R " % n
    d.page(["q 100 0 0 60 40 120 cm /Im1 Do Q\n"
            "0 0 1 RG 3 w 30 140 m 260 140 l S 1 0 0 rg 60 120 80 40 re f\n"
            "0 g BT /F1 14 Tf 40 140 Td (ERASE) Tj 0 -40 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="Image + paths overlap the ERASE bbox; all "
             "painted before the text block.")


@case("numbers_protected")
def _(d):
    d.page([PAINT + "BT /F1 12 Tf 40 160 Td (ERASE) Tj 0 -24 Td (12.5) Tj "
            "0 -24 Td (\\(3\\)) Tj 0 -24 Td (2015) Tj 0 -24 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="Retained numeric texts '12.5', '(3)', '2015'.")


@case("vertical_side_text")
def _(d):
    d.page([PAINT + "BT /F2 10 Tf 0 1 -1 0 24 30 Tm "
            "(arXiv:1234.5678 [cs.CV] 1 Jan 2015) Tj ET\n"
            "BT /F1 14 Tf 60 150 Td (ERASE) Tj 0 -30 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="Retained margin stamp via rotated Tm.")


@case("dependency_same_line")
def _(d):
    d.page([PAINT + "BT /F1 14 Tf 40 150 Td (ERASE) Tj (KEEP) Tj ET\n"])
    return R(["ERASE"], "kTextPositionDependency",
             "Retained Tj directly after removed Tj.")


@case("q_restore_taint")
def _(d):
    d.page([PAINT + "BT /F1 14 Tf 40 150 Td q (ERASE) Tj Q (KEEP) Tj ET\n"])
    return R(["ERASE"], "kTextPositionDependency",
             "Q does not clear taint from inside q..Q.")


@case("cross_stream_operand")
def _(d):
    d.page(["BT /F1 14 Tf 40 100 Td (KEEP) Tj 0 50 Td (ERASE)",
            " Tj ET\n" + PAINT])
    return R(["ERASE"], "kCrossStreamOperator",
             "Operand in stream 1, Tj operator in stream 2.")


@case("quote_selected")
def _(d):
    d.page([PAINT + "BT /F1 14 Tf 16 TL 40 150 Td (KEEP) Tj (ERASE) ' ET\n"])
    return R(["ERASE"], "kUnsupportedText", "Selected text is a ' operation.")


@case("type3_selected")
def _(d):
    gp = d.stream("1000 0 0 0 1000 1000 d1 100 100 800 800 re f\n")
    cmap = d.stream("/CIDInit /ProcSet findresource begin 12 dict begin "
                    "begincmap /CMapName /T3 def 1 begincodespacerange "
                    "<00> <FF> endcodespacerange 1 beginbfchar <41> <0041> "
                    "endbfchar endcmap CMapName currentdict /CMap "
                    "defineresource pop end end\n")
    f = d.add("<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] "
              "/FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /A %d 0 R >> "
              "/Encoding << /Type /Encoding /Differences [65 /A] >> "
              "/FirstChar 65 /LastChar 65 /Widths [1000] /ToUnicode %d 0 R "
              "/Resources << >> >>" % (gp, cmap))
    d.fonts_extra += "/F4 %d 0 R" % f
    d.page([PAINT + "BT /F1 14 Tf 40 100 Td (KEEP) Tj /F4 20 Tf 0 50 Td "
            "(AAA) Tj ET\n"])
    return R(["AAA"], "kUnsupportedText",
             "Type3 font text selected; /ToUnicode maps it to 'AAA'.")


@case("overlap_glyphs")
def _(d):
    d.page([PAINT + "BT /F1 24 Tf 40 120 Td (ERASE) Tj 6 -3 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="KEEP glyphs overlap the ERASE glyph area.")


@case("flate_stream")
def _(d):
    d.page([d.stream(zlib.compress(b(BASIC), 9), "/Filter /FlateDecode")])
    return R(["ERASE"], notes="FlateDecode content stream.")


@case("multi_page_other_unchanged")
def _(d):
    t = "BT /F1 14 Tf 40 150 Td (%s) Tj ET\n"
    d.page([PAINT + t % "PAGE ZERO"])
    d.page([BASIC])
    d.page([PAINT + t % "PAGE TWO"])
    return R(["ERASE"], page=1, other=[0, 2],
             notes="Edit page 1 of 3; pages 0 and 2 must be identical.")


@case("inline_image")
def _(d):
    d.page(["q 60 0 0 40 40 130 cm BI /W 3 /H 2 /CS /G /BPC 8 ID (X) Tj\nEI Q\n"
            "BT /F1 14 Tf 40 100 Td (ERASE) Tj 0 -30 Td (KEEP) Tj ET\n"])
    return R(["ERASE"], notes="Inline image whose data bytes are '(X) Tj'.")


@case("mixed_numeric_prose")
def _(d):
    d.page([PAINT + "BT /F1 12 Tf 40 150 Td (Table 3 shows 42%) Tj "
            "0 -30 Td (KEEP) Tj ET\n"])
    return R(["Table 3 shows 42%"],
             notes="Engine-level SUCCESS; driver-level policy about mixed "
             "numeric/prose text is separate.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    out = ap.parse_args().out_dir
    os.makedirs(out, exist_ok=True)
    cases, paths = [], []
    for cid, fn in CASES:
        d = Doc()
        r = fn(d)
        path = os.path.join(out, cid + ".pdf")
        with open(path, "wb") as f:
            f.write(d.render())
        paths.append(path)
        cases.append(dict(r, id=cid, file=cid + ".pdf"))
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump({"cases": cases}, f, indent=2, sort_keys=True)
        f.write("\n")
    for p in paths:
        check_pdf(p)
    print("wrote %d cases to %s; xref self-check OK" % (len(cases), out))


if __name__ == "__main__":
    sys.exit(main())
