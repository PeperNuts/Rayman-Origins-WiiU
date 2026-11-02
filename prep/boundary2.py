import json,re,os,collections
d=json.load(open(os.path.expanduser('~/boundary.json')))
size={}
for l in open(os.path.expanduser('~/proj/config/SOJP41/symbols.txt')):
    m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+); // type:(\w+)(?: size:0x([0-9A-F]+))?',l)
    if m: size[m.group(1)]=(m.group(4),int(m.group(5) or '0',16),m.group(2),int(m.group(3),16))
folded=[];real=collections.defaultdict(list)
for g,v in d['groups'].items():
    for s,c,n in v['symbols_detail']:
        t,z,sec,addr=size.get(s,('?',0,'?',0))
        if t=='function' and z<=0x20 and n>=3: folded.append((s,g,z,n))
        else: real[g].append((s,c,n,t,z))
print("Folded trivial bodies shared with game code (must be kept):",len(folded))
for f in sorted(folded,key=lambda f:-f[3])[:12]: print("   %-55s %-20s size %#x  used by %d units"%f)
print()
tot=0
for g in sorted(real,key=lambda g:-len(real[g])):
    fn=[r for r in real[g] if r[3]=='function']; dt=[r for r in real[g] if r[3]!='function']
    tot+=len(real[g])
    print(f"{g:22} {len(fn):4} functions {len(dt):3} data")
print("TOTAL real boundary symbols:",tot)
json.dump({'folded':folded,'real':real},open(os.path.expanduser('~/boundary2.json'),'w'),indent=1)
