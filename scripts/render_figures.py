#!/usr/bin/env python3
"""Publication-style vector figures from TurnServe source and recorded traces.

Visual references: ggraph (relations), patchwork (panel composition),
ggforce facet_zoom (overview + detail). Implemented with Matplotlib.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'turnserve-matplotlib'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Circle, FancyArrowPatch, FancyBboxPatch, Rectangle, RegularPolygon
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'docs/assets'
DATA = ROOT / 'results/smoke-cpu'
INK = '#183249'
MUTED = '#637386'
GRID = '#dfe6ee'
BLUE = '#315fa8'
TEAL = '#168578'
ORANGE = '#bd613b'
VIOLET = '#8071ab'
COLORS = {1:BLUE, 2:TEAL, 3:ORANGE, 4:VIOLET}
LABELS = {1:'Document', 2:'Short question', 3:'Original turn', 4:'Replacement'}
plt.rcParams.update({
    'font.family':'sans-serif', 'font.sans-serif':['DejaVu Sans'],
    'font.size':12, 'text.color':INK, 'axes.labelcolor':MUTED,
    'axes.edgecolor':GRID, 'axes.linewidth':1, 'axes.spines.top':False,
    'axes.spines.right':False, 'xtick.color':MUTED, 'ytick.color':MUTED,
    'xtick.labelsize':10, 'ytick.labelsize':11,
    'legend.frameon':False, 'svg.fonttype':'none', 'svg.hashsalt':'turnserve-figures-v2',
    'pdf.fonttype':42, 'figure.facecolor':'white', 'axes.facecolor':'white',
})

def events(policy='interleave'):
    return [json.loads(line) for line in (DATA/f'{policy}.jsonl').read_text().splitlines()]

def by_type(es, typ, rid):
    return [e for e in es if e['type']==typ and e['request']==rid]

def header(fig, number, title, subtitle):
    fig.text(.035,.945,f'FIG. {number}   /   TURNSERVE',fontsize=10,color=MUTED,weight='normal')
    fig.text(.035,.888,title,fontsize=22,weight='normal')
    fig.text(.035,.839,subtitle,fontsize=11,color=MUTED)

def save(fig, name, description, export):
    svg = ASSETS/f'{name}.svg'
    fig.savefig(svg,metadata={'Date':None,'Description':description})
    svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
    # Local high-resolution previews are regenerated, rather than bloating Git.
    fig.savefig(export/f'{name}.png',dpi=300)
    fig.savefig(export/f'{name}.pdf',metadata={'CreationDate':None,'ModDate':None,'Subject':description})
    plt.close(fig)

def arrow(ax, a, b, color=INK, rad=0, style='-|>', lw=1.6, **kw):
    ax.add_patch(FancyArrowPatch(a,b,arrowstyle=style,mutation_scale=13,
                               connectionstyle=f'arc3,rad={rad}',color=color,lw=lw,**kw))

def label(ax,x,y,s,size=12,color=INK,ha='center',**kw):
    return ax.text(x,y,s,fontsize=size,color=color,ha=ha,va='center',**kw)

def architecture(export):
    fig=plt.figure(figsize=(14,7.6))
    header(fig,'01','Concurrent turns. One model. Explicit state boundaries.',
           'C++ runtime architecture  /  scheduling, inference and generation ownership')
    ax=fig.add_axes([.02,.13,.96,.66]);ax.set_xlim(0,100);ax.set_ylim(0,53);ax.axis('off')
    # A shared owner boundary groups the actual runtime responsibilities.
    ax.add_patch(FancyBboxPatch((20,6),79,42,boxstyle='round,pad=0.2,rounding_size=2',
                               fc='#f2f6fb',ec='#cad8e7',lw=1.2))
    label(ax,46.5,45,'SINGLE RUNTIME OWNER',10,MUTED)
    source_y=[38,28,18]
    for rid,y in zip([1,2,3],source_y):
        color=COLORS[rid]
        ax.add_patch(Circle((7,y),2.25,fc='white',ec=color,lw=1.8))
        label(ax,7,y,f'R{rid}',11,color)
        label(ax,7,y-4.6,['Long prompt','Short question','Interruptible turn'][rid-1],10,MUTED)
        arrow(ax,(9.7,y),(24,29),color,rad=[-.13,0,.14][rid-1],alpha=.65)
    # Queue as a stack; scheduler as the point where token budgets are assigned.
    for i,color in enumerate([BLUE,TEAL,ORANGE]):
        ax.add_patch(Rectangle((24.4,25+i*2.6),7.2,1.9,fc=color,ec='white',lw=.6,alpha=.85))
    label(ax,28,36,'Bounded queue',12)
    label(ax,28,19.5,'admit / reject',10,MUTED)
    arrow(ax,(32.4,29),(36,29),BLUE)
    ax.add_patch(Circle((43,29),6.3,fc='white',ec=BLUE,lw=2))
    label(ax,43,30,'Scheduler',12)
    label(ax,43,26.7,'token budget',9,MUTED)
    arrow(ax,(49.8,29),(54.3,29),BLUE)
    ax.add_patch(Circle((62,29),7.1,fc='white',ec=INK,lw=1.8))
    # Small sequence glyphs represent isolated KV states, not model architecture.
    for row,color in enumerate([BLUE,TEAL,VIOLET]):
        for col in range(4):
            ax.add_patch(Rectangle((58.3+col*1.85,30.6-row*1.6),1.4,1.05,fc=color,ec='none',alpha=.8))
    label(ax,62,26,'Shared model',10)
    label(ax,62,18.5,'llama.cpp / CPU',10,MUTED)
    arrow(ax,(60,21.3),(46,22),MUTED,rad=-.3,lw=1.1)
    label(ax,52,15.3,'next batch',9,MUTED)
    # Output lifecycle branches; line width has no quantitative meaning.
    arrow(ax,(69.5,29),(77.7,29),INK)
    ax.add_patch(RegularPolygon((81,29),numVertices=4,radius=3.7,orientation=0,fc='#f7f9fb',ec=INK,lw=1.5))
    label(ax,81,29,'g',15,INK,fontstyle='italic')
    label(ax,81,21.5,'Generation\ncheck',11,MUTED)
    arrow(ax,(83.5,31.5),(91,39),TEAL,rad=-.12)
    arrow(ax,(83.5,26.5),(91,19),ORANGE,rad=.12)
    ax.add_patch(Circle((93,40.5),2,fc='#e2f2ed',ec=TEAL,lw=1.6))
    label(ax,93,40.5,'✓',13,TEAL)
    label(ax,93,46,'Commit',12,TEAL)
    label(ax,93,35.5,'current turn',9,MUTED)
    ax.plot(93,17.7,marker='x',ms=12,mew=2,color=ORANGE)
    label(ax,93,12.6,'Cancel',12,ORANGE)
    label(ax,93,8.8,'no history write',9,MUTED)
    ax.plot([23,70],[6,6],color=GRID,lw=1)
    label(ax,46.5,3,'request ID  ·  session ID  ·  generation',10,MUTED)
    fig.text(.035,.067,'OWNERSHIP',fontsize=10,color=BLUE,weight='bold')
    fig.text(.15,.067,'One thread mutates model state; producers enqueue commands concurrently.',fontsize=11)
    fig.text(.035,.031,'Architecture schematic. Node size and edge width do not encode measured quantities.',fontsize=10,color=MUTED)
    save(fig,'architecture','Architecture schematic derived from the C++ runtime. Non-quantitative directed relationships.',export)

def scheduling(export):
    fig=plt.figure(figsize=(14,9.1))
    header(fig,'02','Scheduling becomes visible in the dispatch order.',
           'Three recorded policy runs  /  each symbol is one per-request dispatch event')
    policies=['fifo','round-robin','interleave']
    starts=[.625,.405,.185]
    titles=['A   FIFO','B   Round-robin','C   Interleave']
    for policy,bottom,title in zip(policies,starts,titles):
        ax=fig.add_axes([.16,bottom,.795,.14])
        ds=[e for e in events(policy) if e['type'] in ('prefill','decode')]
        for row in range(4):
            ax.axhspan(row-.37,row+.37,color='#f3f6f9',zorder=0)
        for typ,marker,color,size in [('prefill','s',BLUE,45),('decode','o',TEAL,23)]:
            idx=[i+1 for i,e in enumerate(ds) if e['type']==typ]
            rows=[e['request']-1 for e in ds if e['type']==typ]
            ax.scatter(idx,rows,marker=marker,c=color,s=size,edgecolors='white',linewidths=.5,zorder=3)
        ax.set_xlim(0,80);ax.set_ylim(3.6,-.6)
        ax.set_yticks(range(4),[LABELS[i] for i in range(1,5)])
        ax.set_xticks([1,10,20,30,40,50,60,70,77]);ax.tick_params(length=0,pad=7)
        ax.spines['left'].set_visible(False);ax.spines['bottom'].set_visible(False)
        ax.set_title(title,loc='left',fontsize=13,pad=12,weight='bold')
        ax.grid(axis='x',color=GRID,lw=.6,zorder=0)
    fig.text(.56,.139,'Dispatch event index  →   (order, not elapsed time or batch number)',ha='center',fontsize=11,color=MUTED)
    handles=[Line2D([],[],color=BLUE,marker='s',lw=0,markersize=7,label='Prefill chunk'),
             Line2D([],[],color=TEAL,marker='o',lw=0,markersize=6,label='Decode token')]
    fig.legend(handles=handles,loc='lower left',bbox_to_anchor=(.035,.067),ncol=2,fontsize=11)
    fig.text(.035,.035,'One smoke run per policy. Symbol size is categorical; replacement arrivals depend on generated tokens.',fontsize=10,color=MUTED)
    save(fig,'scheduling-map','Three facets show actual prefill/decode dispatch order. Not a latency comparison. Raw data in results/smoke-cpu.',export)

def recorded(export):
    es=events()
    fig=plt.figure(figsize=(14,7.1))
    header(fig,'03','From first token to completed turn.',
           'Measured output events  /  SmolLM2-135M-Instruct Q4_K_M · CPU, 4 threads · interleave')
    ax=fig.add_axes([.08,.24,.52,.49]);bx=fig.add_axes([.755,.24,.21,.49])
    for rid in range(1,5):
        ts=by_type(es,'token',rid);sub=by_type(es,'submitted',rid)[0]
        end=next(e for e in es if e['request']==rid and e['type'] in ('completed','cancelled'))
        x=[sub['ms']]+[e['ms'] for e in ts]+[end['ms']]
        y=[0]+[e['count'] for e in ts]+[len(ts)]
        ax.step(x,y,where='post',color=COLORS[rid],lw=2,label=LABELS[rid])
        ax.plot(end['ms'],len(ts),marker='x' if rid==3 else 'o',ms=7,color=COLORS[rid])
        first=ts[0]['ms']-sub['ms'];last=end['ms']-sub['ms'];row=4-rid
        bx.plot([first,last],[row,row],color=COLORS[rid],lw=2,alpha=.75)
        bx.scatter(first,row,s=45,facecolors='white',edgecolors=COLORS[rid],lw=1.7,zorder=3)
        bx.scatter(last,row,s=45,c=COLORS[rid],marker='x' if rid==3 else 'o',zorder=3)
    ax.set_xlim(0,550);ax.set_ylim(0,52);ax.set_yticks([0,10,20,30,40,50]);ax.set_xticks([0,100,200,300,400,500])
    ax.set_ylabel('Emitted output tokens');ax.set_xlabel('Time since runtime start (ms)',labelpad=10)
    ax.grid(axis='y',color=GRID,lw=.7);ax.set_title('A   Cumulative output',loc='left',fontsize=13,pad=16,weight='bold')
    bx.set_xlim(0,550);bx.set_ylim(-.5,3.5);bx.set_xticks([0,250,500]);bx.set_yticks([3,2,1,0],[LABELS[i] for i in range(1,5)])
    bx.tick_params(axis='y',length=0);bx.spines['left'].set_visible(False)
    bx.grid(axis='x',color=GRID,lw=.7);bx.set_xlabel('Since submission (ms)',labelpad=10)
    bx.set_title('B   Request lifetimes',loc='left',fontsize=13,pad=16,weight='bold')
    handles=[Line2D([],[],color=COLORS[i],lw=2,label=LABELS[i]) for i in range(1,5)]
    fig.legend(handles=handles,loc='lower left',bbox_to_anchor=(.055,.083),ncol=4,fontsize=10,columnspacing=1.4)
    fig.text(.755,.118,'○ first token   ● completed   × cancelled',fontsize=9,color=MUTED)
    fig.text(.035,.041,'Single recorded execution; no averaging, uncertainty intervals, or cross-policy performance claim.',fontsize=10,color=MUTED)
    save(fig,'recorded-run','Step curves of actual emitted token counts with request-relative first-token and terminal times. One interleave smoke run.',export)

def cancellation(export):
    es=events();old=3;new=4
    end_old=by_type(es,'cancelled',old)[0];submit=by_type(es,'submitted',new)[0];accept=by_type(es,'accepted',new)[0];commit=by_type(es,'committed',new)[0]
    fig=plt.figure(figsize=(14,8.1))
    header(fig,'04','A replacement changes ownership, not just the prompt.',
           'One observed interruption  /  overview, acknowledgement detail and committed outcomes')
    ax=fig.add_axes([.11,.56,.82,.19])
    for rid,row in [(old,1),(new,0)]:
        sub=by_type(es,'submitted',rid)[0]
        end=next(e for e in es if e['request']==rid and e['type'] in ('cancelled','completed'))
        color=ORANGE if rid==old else VIOLET
        ax.plot([sub['ms'],end['ms']],[row,row],lw=3,color=color,alpha=.35)
        for e in by_type(es,'token',rid): ax.plot(e['ms'],row,'|',color=color,ms=15,mew=2)
        ax.plot(end['ms'],row,'x' if rid==old else 'o',color=color,ms=9,mew=2)
    ax.axvspan(135.98,136.22,color=BLUE,alpha=.2,lw=1)
    ax.annotate('Detail below',xy=(136.15,.5),xytext=(205,.68),fontsize=10,color=BLUE,
                arrowprops={'arrowstyle':'-','color':BLUE,'lw':1})
    ax.set_yticks([1,0],['Generation 1','Generation 2']);ax.set_ylim(-.55,1.5);ax.set_xlim(0,450)
    ax.set_xlabel('Time since runtime start (ms)',labelpad=8);ax.tick_params(axis='y',length=0)
    ax.spines['left'].set_visible(False);ax.set_title('A   Full interaction',loc='left',fontsize=13,pad=12,weight='bold')
    # Zoom on explicit events. Separate rows avoid colliding labels a few microseconds apart.
    bx=fig.add_axes([.11,.21,.49,.19])
    times=[submit['ms'],end_old['ms'],accept['ms']]
    colors=[BLUE,ORANGE,VIOLET]
    for t,row,color in zip(times,[2,1,0],colors):
        bx.plot([136,t],[row,row],color=GRID,lw=2)
        bx.scatter(t,row,c=color,s=55,zorder=3)
        bx.text(t+.008,row,f'{t:.3f}',va='center',fontsize=10,color=color)
    bx.set_xlim(136,136.25);bx.set_ylim(-.6,2.6)
    bx.set_yticks([2,1,0],['Replace queued','Old cancelled','New accepted'])
    bx.set_xticks([136,136.1,136.2]);bx.ticklabel_format(axis='x',style='plain',useOffset=False)
    bx.set_xlabel('Time since runtime start (ms)',labelpad=9);bx.spines['left'].set_visible(False);bx.tick_params(axis='y',length=0)
    bx.set_title('B   Acknowledgement window (magnified)',loc='left',fontsize=13,pad=14,weight='bold')
    cx=fig.add_axes([.71,.20,.25,.22]);cx.axis('off')
    cx.text(0,1,'C   History commits',fontsize=13,weight='bold',transform=cx.transAxes)
    for row,rid,color in [(.6,old,ORANGE),(.22,new,TEAL)]:
        n=len(by_type(es,'committed',rid))
        cx.text(0,row,f'Generation {1 if rid==old else 2}',fontsize=12,color=MUTED,transform=cx.transAxes)
        cx.text(.86,row,str(n),fontsize=26,color=color,ha='right',transform=cx.transAxes)
    cx.text(0,-.1,f'New turn committed at {commit["ms"]:.3f} ms',fontsize=10,color=MUTED,transform=cx.transAxes)
    fig.text(.035,.071,'The old generation ends without a history commit; the replacement owns the next completed turn.',fontsize=11)
    fig.text(.035,.033,'Real trace; different time scales in A and B. The acknowledgement window is an observation, not an SLA.',fontsize=10,color=MUTED)
    save(fig,'cancellation-detail','Actual cancellation, replacement acceptance and history-commit events. Panel B magnifies the interval highlighted in panel A.',export)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export-dir',type=Path,default=ROOT/'runs/figure-exports')
    args=parser.parse_args();args.export_dir.mkdir(parents=True,exist_ok=True);ASSETS.mkdir(parents=True,exist_ok=True)
    for fn in [architecture,scheduling,recorded,cancellation]:fn(args.export_dir)
    provenance={
        'renderer':'Matplotlib','renderer_version':matplotlib.__version__,
        'inputs':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(DATA.glob('*.jsonl'))},
        'figures':{'architecture':'Non-quantitative source-derived schematic',
                   'scheduling-map':'Dispatch order by policy; one raw event per symbol',
                   'recorded-run':'Interleave output-token counts and request lifetimes',
                   'cancellation-detail':'Interleave replacement and commit events; magnified time axis'},
        'replication':'One functional smoke run per policy; no pooled statistics or uncertainty estimates',
        'visual_references':['https://ggraph.data-imaginist.com/','https://patchwork.data-imaginist.com/','https://ggforce.data-imaginist.com/reference/facet_zoom.html']}
    (ASSETS/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    print(f'Wrote four SVG figures to {ASSETS}; PDF/300-dpi PNG previews in {args.export_dir}')

if __name__=='__main__':main()
