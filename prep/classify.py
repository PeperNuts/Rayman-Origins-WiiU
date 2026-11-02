import re, os, struct, json, sys
P=os.path.expanduser('~/proj'); B=os.path.expanduser('~/tools/binutils')
undef=[l.strip() for l in open(os.path.expanduser('~/port/undef.txt')) if l.strip()]
syms={}
for l in open(P+'/config/SOJP41/symbols.txt'):
    m=re.match(r'(\S+) = (\S+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?(?: scope:(\w+))?',l)
    if m: syms[m.group(1)]=dict(sec=m.group(2),addr=int(m.group(3),16),type=m.group(4),size=int(m.group(5) or '0',16))
dol=open(P+'/orig/SOJP41/sys/main.dol','rb').read()
offs=struct.unpack('>18I',dol[0:72]);ads=struct.unpack('>18I',dol[72:144]);szs=struct.unpack('>18I',dol[144:216])
def dolbytes(a,n):
    for o,ad,s in zip(offs,ads,szs):
        if s and ad<=a and a+n<=ad+s: return dol[o+(a-ad):o+(a-ad)+n]
    return None
# relocations of the original split objects (all units) -> address set per function
import subprocess
reloc_addrs=set()
splits=open(P+'/config/SOJP41/splits.txt').read()
# use the full linked main.elf from the matching build: relocations are kept there? use objs instead
for f in os.listdir(P+'/build/SOJP41/obj'):
    pass
# simpler: main.elf produced by mwld has no relocs; rebuild from per-object rela via readelf on all objs that own needed syms
owners={}
def objsyms(path):
    out=subprocess.run([B+'/powerpc-eabi-readelf','-sW',path],capture_output=True,text=True).stdout
    return out
need=set(u for u in undef if u in syms and syms[u]['type']=='function')
res={}
PRIV=re.compile(r'\b(mfmsr|mtmsr|rfi|sc|dcbi|mtsr|mfsr|tlbie|mtspr|mfspr|mfsrin|eciwx|ecowx)\b')
for u in undef:
    s=syms.get(u)
    if not s: res[u]=dict(kind='unknown'); continue
    if s['type']!='function':
        res[u]=dict(kind='data',sec=s['sec'],size=s['size'],addr=s['addr']); continue
    res[u]=dict(kind='func',size=s['size'],addr=s['addr'])
json.dump(res,open(os.path.expanduser('~/port/undef_info.json'),'w'),indent=1)
from collections import Counter
print(Counter(v['kind'] for v in res.values()))
print([k for k,v in res.items() if v['kind']=='unknown'])
