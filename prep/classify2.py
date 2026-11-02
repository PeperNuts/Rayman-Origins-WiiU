import re, os, json, subprocess, sys
P=os.path.expanduser('~/proj'); B=os.path.expanduser('~/tools/binutils'); OBJ=P+'/build/SOJP41/obj'
info=json.load(open(os.path.expanduser('~/port/undef_info.json')))
want=set(info)
kept=set(os.path.splitext(os.path.basename(l.strip()))[0] for l in open(os.path.expanduser('~/port/objs.txt')))
PRIV=re.compile(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(mfmsr|mtmsr|rfi|sc|dcbi|mtsr|mfsr|tlbie|mfgqr|mtgqr|mfhid|mthid|mtdec|mfdec|mtsrr\d|mfsrr\d|mtsprg|mfsprg|mtwpar|mfwpar|mtl2cr|mtdmau|mtdmal|mfdmau|mfdmal|mfpvr)\b')
SPR=re.compile(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(mtspr|mfspr)\s+(\S+)')
ABS=re.compile(r'\blis\s+r\d+,\s*(-?\d+)')
found={}
for root,_,files in os.walk(OBJ):
    for fn in files:
        if not fn.endswith('.o'): continue
        path=os.path.join(root,fn); unit=os.path.splitext(fn)[0]
        if unit in kept: continue
        st=subprocess.run([B+'/powerpc-eabi-readelf','-sW',path],capture_output=True,text=True).stdout
        hits=[]
        for l in st.splitlines():
            p=l.split()
            if len(p)>=8 and p[7] in want and p[6] not in ('UND','ABS') and p[3] in ('FUNC','OBJECT','NOTYPE'):
                hits.append((p[7],int(p[1],16),int(p[2]),p[3],p[6]))
        if not hits: continue
        rl=subprocess.run([B+'/powerpc-eabi-readelf','-rW',path],capture_output=True,text=True).stdout
        relocs={}; cur=None
        for l in rl.splitlines():
            m=re.match(r"Relocation section '\.rela(\S+)'",l)
            if m: cur=m.group(1); relocs.setdefault(cur,[]); continue
            p=l.split()
            if cur and p and re.fullmatch(r'[0-9a-f]{8}',p[0]): relocs[cur].append(int(p[0],16))
        secnames={}
        for l in subprocess.run([B+'/powerpc-eabi-readelf','-SW',path],capture_output=True,text=True).stdout.splitlines():
            m=re.match(r'\s*\[\s*(\d+)\]\s+(\S+)',l)
            if m: secnames[m.group(1)]=m.group(2)
        dis=None
        for name,val,size,typ,ndx in hits:
            sec=secnames.get(ndx,'?')
            ent=dict(unit=fn,sec=sec,off=val,size=size,type=typ)
            if typ=='FUNC' or sec in ('.text','.init'):
                nrel=sum(1 for o in relocs.get(sec,[]) if val<=o<val+size)
                if dis is None:
                    dis=subprocess.run([B+'/powerpc-eabi-objdump','-d','-M','gekko','-j',sec,path],capture_output=True,text=True).stdout
                body=[]; inside=False
                for l in dis.splitlines():
                    m=re.match(r'\s*([0-9a-f]+):',l)
                    if m:
                        a=int(m.group(1),16)
                        if val<=a<val+size: body.append(l)
                priv=[l for l in body if PRIV.match(l)]
                spr=[SPR.match(l).group(2) for l in body if SPR.match(l)]
                badspr=[s for s in spr if s not in ('lr','ctr','xer','8','9','1')]
                lis=[int(ABS.search(l).group(1)) for l in body if ABS.search(l)]
                abs_hw=[v for v in lis if 0xC000<=(v&0xFFFF)<=0xCFFF or 0x8000<=(v&0xFFFF)<=0x8180 or 0x9000<=(v&0xFFFF)<=0x93FF or 0xCD00<=(v&0xFFFF)<=0xCDFF]
                ent.update(relocs=nrel,priv=len(priv)+len(badspr),abs=len(abs_hw),ninsn=len(body))
                ent['copyable']= nrel==0 and not priv and not badspr and not abs_hw and len(body)>0
            found.setdefault(name,ent)
for k,v in info.items():
    if k in found: v.update(found[k])
json.dump(info,open(os.path.expanduser('~/port/undef_info.json'),'w'),indent=1)
from collections import Counter
print('resolved owners:',sum(1 for k in info if k in found),'/',len(info))
print('copyable funcs:',sum(1 for v in info.values() if v.get('copyable')))
print('missing:',[k for k in info if k not in found])
