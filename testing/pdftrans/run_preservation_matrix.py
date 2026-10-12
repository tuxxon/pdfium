#!/usr/bin/env python3
# Copyright 2026 The PDFium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Runs pdftrans_source_preservation_audit over a synthetic manifest and/or
real fixtures and writes one ledger JSON.

Real-fixture selection policy (independent of the engine):
  prose     mode-0, non-Type3, upright horizontal TEXT with letters and no
            digits -> selectable
  numeric   no letters (numbers, punctuation)          -> protected
  mixed     letters AND digits in the same TEXT         -> unsupported, kept
  rotated   any non-upright matrix (e.g. arXiv margin)  -> protected
  other     non-zero mode / Type3 / empty               -> unsupported, kept
For each page with prose, one page-level run removes all prose together, and
one single-object run removes each prose object alone. Every fixture stays in
the denominator; a rejected run is reported with its engine code.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys


def sha256(path):
  with open(path, 'rb') as f:
    return hashlib.sha256(f.read()).hexdigest()


def run(tool, args):
  proc = subprocess.run([tool] + args, capture_output=True, text=True)
  if proc.returncode != 0:
    return {'error': proc.stderr.strip(), 'passed': False, 'result': 'TOOL_ERROR'}
  return json.loads(proc.stdout)


def classify(obj):
  if obj['type'] != 'text':
    return None
  text = obj['text']
  a, b, c, d = obj['matrix'][:4]
  if obj['mode'] != 0 or obj['type3'] or not text.strip():
    return 'other'
  if b != 0 or c != 0 or a <= 0 or d <= 0:
    return 'rotated'
  letters = any(ch.isalpha() for ch in text)
  digits = any(ch.isdigit() for ch in text)
  if letters and digits:
    return 'mixed'
  if not letters:
    return 'numeric'
  return 'prose'


def summarize(report):
  keep = {'result', 'passed', 'outputSha256', 'error'}
  out = {k: v for k, v in report.items() if k in keep}
  target = [p for p in report.get('pages', []) if 'paintMaskPixels' in p]
  if target:
    t = target[0]
    out.update({k: t[k] for k in (
        'paintMaskPixels', 'changedInsidePaint', 'changedOutsidePaint',
        'diffVsDeactivatedOriginal', 'streams')})
  bad = [p['page'] for p in report.get('pages', [])
         if not (p['settingsEqual'] and p['objectsEqual'] and
                 p.get('changedPixels', 0) == 0)]
  if bad:
    out['pagesWithDifferences'] = bad
  return out


def synthetic(tool, manifest_path, out_dir):
  base = os.path.dirname(manifest_path)
  cases = json.load(open(manifest_path))['cases']
  rows = []
  for case in cases:
    pdf = os.path.join(base, case['file'])
    args = ['--input', pdf, '--page', str(case['page']),
            '--output', os.path.join(out_dir, case['id'] + '.out.pdf')]
    for text in case['select']:
      args += ['--select-text', text]
    report = run(tool, args)
    row = {'id': case['id'], 'sourceSha256': sha256(pdf),
           'expect': case['expect']}
    row.update(summarize(report))
    row['status'] = ('PASS' if report['passed'] and
                     report['result'] == case['expect'] else 'FAIL')
    rows.append(row)
  return rows


def fixture(tool, pdf, out_dir, singles):
  name = os.path.splitext(os.path.basename(pdf))[0]
  listing = run(tool, ['--list', '--input', pdf])
  doc = {'fixture': os.path.basename(pdf), 'sourceSha256': sha256(pdf),
         'pages': []}
  for page in listing['pages']:
    classes = {}
    for obj in page['objects']:
      kind = classify(obj)
      if kind:
        classes.setdefault(kind, []).append(obj['index'])
    entry = {'page': page['page'], 'settings': page['settings'],
             'textClasses': {k: len(v) for k, v in sorted(classes.items())}}
    prose = classes.get('prose', [])
    if not prose:
      entry['status'] = 'NO_SELECTABLE_TEXT'
      doc['pages'].append(entry)
      continue
    args = ['--input', pdf, '--page', str(page['page']), '--output',
            os.path.join(out_dir, '%s.p%d.all.pdf' % (name, page['page']))]
    for index in prose:
      args += ['--select-index', str(index)]
    report = run(tool, args)
    entry['pageLevel'] = summarize(report)
    entry['pageLevel']['selected'] = prose
    entry['status'] = ('REJECTED' if report['result'] != 'SUCCESS' else
                       'PASS' if report['passed'] else 'FAIL')
    if not report['passed']:
      entry['status'] = 'FAIL'
    if singles:
      tally = {}
      failures = []
      for index in prose:
        single = run(tool, ['--input', pdf, '--page', str(page['page']),
                            '--select-index', str(index)])
        key = single['result']
        tally[key] = tally.get(key, 0) + 1
        if not single['passed']:
          failures.append({'index': index, **summarize(single)})
      entry['singles'] = {'results': tally, 'failures': failures}
    doc['pages'].append(entry)
  statuses = {p['status'] for p in doc['pages']}
  doc['status'] = ('FAIL' if 'FAIL' in statuses else
                   'PASS' if statuses <= {'PASS', 'NO_SELECTABLE_TEXT'} and
                   'PASS' in statuses else 'PARTIAL' if 'PASS' in statuses
                   else 'UNSUPPORTED')
  return doc


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--tool', required=True)
  parser.add_argument('--manifest')
  parser.add_argument('--fixture', action='append', default=[])
  parser.add_argument('--singles', action='store_true')
  parser.add_argument('--out-dir', required=True)
  args = parser.parse_args()
  os.makedirs(args.out_dir, exist_ok=True)
  ledger = {'toolSha256': sha256(args.tool)}
  ok = True
  if args.manifest:
    rows = synthetic(args.tool, args.manifest, args.out_dir)
    ledger['synthetic'] = rows
    ok &= all(r['status'] == 'PASS' for r in rows)
  if args.fixture:
    docs = [fixture(args.tool, f, args.out_dir, args.singles)
            for f in args.fixture]
    ledger['fixtures'] = docs
    ok &= all(d['status'] != 'FAIL' for d in docs)
  ledger['passed'] = ok
  path = os.path.join(args.out_dir, 'ledger.json')
  with open(path, 'w') as f:
    json.dump(ledger, f, indent=1, sort_keys=True)
  print(path, 'PASSED' if ok else 'FAILED')
  return 0 if ok else 1


if __name__ == '__main__':
  sys.exit(main())
