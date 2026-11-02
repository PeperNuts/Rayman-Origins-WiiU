import re,os,json,collections,sys
sys.path.insert(0,os.path.expanduser('~/scripts'))
P=os.path.expanduser('~/proj'); A=P+'/build/SOJP41/asm'
exec(open(os.path.expanduser('~/scripts/boundary.py')).read().split('def objpath')[0].split("gof={u:group(u)")[0])
kept=[u for u in units if not group(u)]
pat={
 'priv: mfmsr/mtmsr/rfi':r'\b(mfmsr|mtmsr|rfi)\b',
 'priv: cache invalidate dcbi':r'\bdcbi\b',
 'spr: GQR (paired-single quant)':r'\b(mtspr|mfspr)\s+GQR\d|\bmtspr\s+(9(1[2-9]))\b',
 'spr: HID/WPAR/DMA/L2CR/BAT/SRR':r'\b(mtspr|mfspr)\s+(HID\d|WPAR|DMA[UL]|L2CR|[ID]BAT\d[UL]|SRR\d|DEC|SPRG\d)\b',
 'locked cache dcbz_l':r'\bdcbz_l\b',
 'paired singles (ok on Espresso)':r'\bps(q_|_)\w*',
 'hw regs 0xCC00/0xCD00/0xCD80':r'lis\s+r\d+,\s*0x(CC0|CD0|CD8)',
 'OS low memory 0x8000xxxx':r'lis\s+r\d+,\s*0x8000\b',
 'uncached mirror 0xC000':r'lis\s+r\d+,\s*0xC00[0-9A-F]\b',
}
hits={k:collections.Counter() for k in pat}
for u in kept:
    p=A+'/'+os.path.splitext(u)[0]+'.s'
    if not os.path.exists(p): continue
    s=open(p,errors='ignore').read()
    for k,rx in pat.items():
        n=len(re.findall(rx,s))
        if n: hits[k][u]=n
res={}
for k,c in hits.items():
    print(f"{k:34} {sum(c.values()):6} hits in {len(c):4} units   e.g. {', '.join(f'{u}({n})' for u,n in c.most_common(5))}")
    res[k]=dict(c)
json.dump(res,open(os.path.expanduser('~/scan.json'),'w'),indent=1)
