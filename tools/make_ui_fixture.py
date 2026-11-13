#!/usr/bin/env python3
"""Create an explicitly synthetic, offline fixture using the actual production HTML.

The fixture replaces fetch and XMLHttpRequest before the production application
script. Neither transport falls through to the network. It is excluded from
firmware embedding; open it locally for synthetic visual checks only.
"""
from pathlib import Path
import argparse

ROOT=Path(__file__).resolve().parent.parent

def make(output: Path):
    production=(ROOT/'web/index.html').read_text()
    fixture=(ROOT/'tests/ui_fixture_api.js').read_text()
    marker='<script id="gateway-script">'
    if production.count(marker)!=1:
        raise RuntimeError('Expected one production application script marker')
    csp='<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; script-src \'unsafe-inline\'; style-src \'unsafe-inline\'; img-src data:; connect-src \'none\'; object-src \'none\'; base-uri \'none\'">'
    production=production.replace('<meta charset="utf-8">','<meta charset="utf-8">'+csp)
    production=production.replace('<title>UPS Gateway | Network Management</title>','<title>TEST FIXTURE · UPS Gateway · Synthetic Data</title>')
    production=production.replace(marker,'<script id="synthetic-api-fixture">\n'+fixture+'\n</script>\n'+marker)
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(production)
    print(str(output.resolve()))
    print(f'{output.stat().st_size} bytes; production HTML embedded without replacing application logic')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'tests/ui_fixture.html')
    args=parser.parse_args()
    make(args.output)
