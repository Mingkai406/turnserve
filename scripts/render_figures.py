#!/usr/bin/env python3
"""Render original vector diagrams. The timeline uses recorded events, never invented measurements."""
import html
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'docs/assets'
INK, MUTED, LINE, TEAL, ORANGE = '#172b3a', '#5c6c78', '#d7dee3', '#16786d', '#ba5934'

def start(width, height, title, desc):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
            f'<title id="title">{html.escape(title)}</title><desc id="desc">{html.escape(desc)}</desc>',
            '<style>text{font-family:Arial,Helvetica,sans-serif} .mono{font-family:Menlo,Consolas,monospace}</style>',
            f'<rect width="{width}" height="{height}" fill="#ffffff"/>']

def text(parts, x, y, value, size=15, fill=INK, extra=''):
    parts.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" {extra}>{html.escape(value)}</text>')

def line(parts, x1, y1, x2, y2, color=LINE, width=1, extra=''):
    parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{width}" {extra}/>')

def architecture():
    p = start(1100, 344, 'TurnServe execution model', 'Concurrent producers submit to one runtime owner. The scheduler allocates prefill and decode work to llama.cpp. Only completed current generations commit to conversation history.')
    text(p, 32, 34, 'TURNSERVE / EXECUTION MODEL', 12, MUTED, 'letter-spacing="1.8"')
    text(p, 32, 76, 'Share the model. Keep each conversation intact.', 27)
    line(p,32,99,1068,99)
    cols = [(32, '01', 'Admit', 'Bounded command queue', 'Submit · cancel · replace'),
            (309,'02','Schedule','Prefill chunks + decode','Single runtime owner'),
            (586,'03','Execute','llama.cpp / model weights','Independent sequence state'),
            (863,'04','Commit','Current generation only','Completed turns → history')]
    for x, number, heading, a, b in cols:
        text(p,x,132,number,12,TEAL,'class="mono"')
        text(p,x,163,heading,21)
        text(p,x,191,a,14)
        text(p,x,215,b,13,MUTED)
        line(p,x,236,min(x+203,1068),236,TEAL,3)
        if number != '04':
            line(p,x+216,178,x+252,178,MUTED,1.4)
            p.append(f'<path d="M{x+246} 174 L{x+252} 178 L{x+246} 182" fill="none" stroke="{MUTED}" stroke-width="1.4"/>')
    line(p,32,267,1068,267)
    text(p,32,296,'CANCEL BOUNDARY',11,ORANGE,'letter-spacing="1.2"')
    text(p,222,296,'An in-flight batch may finish. An acknowledged cancellation cannot commit.',15)
    text(p,32,326,'Architecture schematic · v0.1 · single machine, CPU reference backend',12,MUTED)
    p.append('</svg>')
    (ASSETS/'architecture.svg').write_text('\n'.join(p))

def timeline():
    source = ROOT/'results/smoke-cpu/interleave.jsonl'
    es = [json.loads(l) for l in source.read_text().splitlines()]
    ends = {e['request']:e for e in es if e['type'] in {'completed','cancelled','failed','rejected'}}
    starts = {e['request']:e for e in es if e['type']=='submitted'}
    end_ms = max(e['ms'] for e in ends.values())
    scale_max = (int(end_ms/100)+1)*100
    p = start(1100, 388, 'Recorded multi-session run', 'A real CPU inference smoke test. Gray lines show the lifetime of each request, teal marks show emitted tokens, and the orange cross marks acknowledged cancellation. This is not a performance comparison.')
    text(p,32,33,'TURNSERVE / RECORDED EXECUTION',12,MUTED,'letter-spacing="1.8"')
    text(p,32,70,'One model, three conversations, one changed question.',24)
    text(p,32,97,'SmolLM2-135M-Instruct · Q4_K_M · CPU, 4 threads · interleave policy · single run',13,MUTED)
    x0,x1=218,1035
    x=lambda ms:x0+(x1-x0)*ms/scale_max
    for tick in range(0,scale_max+1,100):
        line(p,x(tick),122,x(tick),291,LINE,1,'stroke-dasharray="2 4"')
        text(p,x(tick),313,str(tick),11,MUTED,'text-anchor="middle" class="mono"')
    labels={1:'Long document',2:'Short question',3:'Original question',4:'Replacement'}
    for n,rid in enumerate(sorted(starts)):
        y=145+n*44
        text(p,32,y+5,labels.get(rid,str(rid)),14)
        a,b=starts[rid],ends[rid]
        line(p,x(a['ms']),y,x(b['ms']),y,'#b6c1ca',3)
        tokens=[e for e in es if e['request']==rid and e['type']=='token']
        for e in tokens:
            line(p,x(e['ms']),y-7,x(e['ms']),y+7,TEAL,2)
        if b['type']=='cancelled':
            bx=x(b['ms'])
            line(p,bx-5,y-5,bx+5,y+5,ORANGE,2)
            line(p,bx-5,y+5,bx+5,y-5,ORANGE,2)
    text(p,1054,313,'ms',11,MUTED)
    line(p,32,342,60,342,'#b6c1ca',3);text(p,69,347,'Request lifetime',12,MUTED)
    line(p,249,335,249,349,TEAL,2);text(p,261,347,'Output token',12,MUTED)
    text(p,422,347,'×',18,ORANGE);text(p,441,347,'Cancellation acknowledged',12,MUTED)
    text(p,32,376,'Functional smoke test; timings are observations from this run, not a throughput or latency claim.',12,MUTED)
    p.append('</svg>')
    (ASSETS/'recorded-run.svg').write_text('\n'.join(p))

if __name__=='__main__':
    ASSETS.mkdir(parents=True,exist_ok=True)
    architecture()
    timeline()
