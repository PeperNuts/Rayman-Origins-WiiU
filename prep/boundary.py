import struct, re, os, sys, collections, json
P=os.path.expanduser('~/proj'); OBJ=P+'/build/SOJP41/obj'
units=[l[:-1] for l in open(P+'/config/SOJP41/splits.txt').read().splitlines() if l and not l[0].isspace() and l.endswith(':') and l!='Sections:']
# --- classification of Wii-only (replace) units -> library group
groups=[
 ('OS / CPU', r'^(OS[A-Za-z]*|__start|PPCArch|time\.dolphin|flush_cache)\.c$'),
 ('GX graphics', r'^GX[A-Za-z]*\.c$'),
 ('VI video', r'^(vi|vi3in1|i2c)\.c$'),
 ('AX/DSP audio', r'^(AX[A-Za-z]*|dsp[a-z_]*|DSPCode|ai)\.c$'),
 ('DVD disc', r'^(dvd[A-Za-z_]*)\.c$'),
 ('NAND / FS / IPC / ES', r'^(nand|NAND[A-Za-z]*|ipc[A-Za-z]*|fs|esp|arc|sdk/memory)\.c$'),
 ('SC settings', r'^(scsystem|scapi|scapi_prdinfo)\.c$'),
 ('WPAD/KPAD Wiimote', r'^(WPAD[A-Za-z]*|KPAD|KMPLS|KZMpls[A-Za-z]*|WUD[A-Za-z]*|lint|wenc)\.c$'),
 ('Bluetooth stack', r'^(bt[a-z]_[a-z_0-9]*|bte_[a-z_]*|gki_[a-z]*|hci[a-z_0-9]*|hid[dh]_[a-z]*|l2c_[a-z_0-9]*|port_[a-z_0-9]*|rfc_[a-z_0-9]*|sdp_[a-z_0-9]*|gap_[a-z_0-9]*|bd|ptim|utl|uusb_ppc|usb|wbt_ext|bta_[a-z_]*|btu_[a-z_0-9]*|btm_[a-z]*)\.c$'),
 ('GC pad / SI / EXI', r'^(Pad|SI[A-Za-z]*|EXI[A-Za-z]*|exi2|euart)\.c$'),
 ('Debugger (TRK)', r'^(metrotrk/.*|DebuggerDriver|cc_gdev|CircleBuffer|main_TRK|mainloop|mem_TRK|dispatch|dolphin_trk[a-z_]*|notify|nubevent|nubinit|serpoll|string_TRK|support|targcont|mpc_7xx_603e|msg|msgbuf|msghndlr|mslsupp|targimpl|target_options|targsupp|uart_console_io_gcn)\.(c|s)$'),
 ('HOME Menu (HBM)', r'^HBM[A-Za-z]*\.cpp$'),
 ('NW4R layout/font', r'^(lyt_[A-Za-z]*|ut_[A-Za-z]*)\.cpp$'),
 ('Bink Wii I/O+audio', r'^(wiiax|wiirfile|binkfile|wii_rrthreads|wii_rratomics)\.(c|cpp)$'),
]
def group(u):
    for g,rx in groups:
        if re.match(rx,u): return g
    return None
gof={u:group(u) for u in units}
def objpath(u): return OBJ+'/'+os.path.splitext(u)[0]+'.o'
def parse(path):
    e=open(path,'rb').read()
    shoff,=struct.unpack('>I',e[0x20:0x24]); shnum,shstr=struct.unpack('>HH',e[0x30:0x34])
    sh=[struct.unpack('>10I',e[shoff+i*40:shoff+i*40+40]) for i in range(shnum)]
    nm=lambda tab,o: e[tab+o:e.index(b'\0',tab+o)].decode('latin1')
    shn=lambda s: nm(sh[shstr][4],s[0])
    syms=[];defs=set()
    for s in sh:
        if s[1]==2:
            stro=sh[s[6]][4]
            for i in range(s[4],s[4]+s[5],16):
                n,v,z,info,oth,ndx=struct.unpack('>IIIBBH',e[i:i+16])
                name=nm(stro,n) if n else ''
                syms.append((name,ndx,info>>4))
                if ndx!=0 and (info>>4) in (1,2) and name: defs.add(name)
    refs=collections.Counter()
    for s in sh:
        if s[1]==4:
            tsec=shn(sh[s[7]])
            for i in range(s[4],s[4]+s[5],12):
                off,info,add=struct.unpack('>IIi',e[i:i+12])
                name,ndx,bind=syms[info>>8]
                if ndx==0 and name: refs[name]+=1
    return defs,refs
D={};R={}
for u in units:
    p=objpath(u)
    if os.path.exists(p): D[u],R[u]=parse(p)
owner={}
for u,ds in D.items():
    for s in ds: owner.setdefault(s,u)
# boundary: refs from kept units to symbols defined in replaced units
by_group=collections.defaultdict(lambda: collections.defaultdict(lambda:[0,set()]))
callers=collections.defaultdict(set)
for u,rs in R.items():
    if gof[u]: continue
    for s,c in rs.items():
        o=owner.get(s)
        if o and gof[o]:
            ent=by_group[gof[o]][s]; ent[0]+=c; ent[1].add(u); callers[gof[o]].add(u)
out={'kept_units':sum(1 for u in units if not gof[u]),'replaced_units':collections.Counter(g for g in gof.values() if g),
     'groups':{g:{'symbols':len(v),'refs':sum(x[0] for x in v.values()),'caller_units':sorted(callers[g]),
       'symbols_detail':sorted(((s,x[0],len(x[1])) for s,x in v.items()),key=lambda t:-t[1])} for g,v in by_group.items()}}
json.dump(out,open(os.path.expanduser('~/boundary.json'),'w'),indent=1,default=list)
print('kept units',out['kept_units']); print('replaced units by group',dict(out['replaced_units']))
for g,v in sorted(out['groups'].items(),key=lambda kv:-kv[1]['symbols']):
    print(f"{g:22} {v['symbols']:4} functions/globals  {v['refs']:5} refs  from {len(v['caller_units']):3} units")
