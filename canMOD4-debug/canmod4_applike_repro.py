#!/usr/bin/env python3
# FAITHFUL "wb-mqtt-smartweb"-style master (NOT my DLC=0 spam):
#   * sniff the bus, capture a real REQUEST->RESPONSE pair (GET_PARAMETER_VALUE, DLC>=1);
#   * then poll EXACTLY like the app: send that request -> WAIT for the device's response ->
#     pace to poll_interval (500 ms) -> next request.
# Full decoded UART log. Detects the wedge (transmit gets no 'Z') and dumps the exchange around it.
# SmartWeb response id = request id with message_type REQUEST(0) -> RESPONSE(2)  == id | (2<<27).
import serial, time, sys
PORT='/dev/ttyS5'; BAUD=115200
POLL_MS=500
T0=time.time()
def ts(): return '%8.3f'%(time.time()-T0)
def hx(b): return ' '.join('%02x'%x for x in b) if b else '--'
def sb(x): return x-256 if x>=128 else x
MTN={0:'REQUEST',1:'MT1',2:'RESPONSE',3:'ERROR'}
def mt(idv): return (idv>>27)&3
def dec(idv): return f"id=0x{idv:08X} PT={idv&0xff} PID={(idv>>8)&0xff} FN={(idv>>16)&0xff} MT={mt(idv)}/{MTN.get(mt(idv),'?')}"
def TXLOG(d,n): print(f"[{ts()}] TX  {hx(d):<30} {n}", flush=True)
def RXLOG(r,n): print(f"[{ts()}] RX  {hx(r):<30} {n}", flush=True)

class C:
    def __init__(s):
        s.s=serial.Serial(PORT,BAUD,8,'N',1,timeout=0); s.buf=bytearray()
        s.acks=0; s.fval=None; s.frames=[]; s.quiet=True
        s.rx_cap=None; s.rx_n=0; s.rx_supp=0
    def w(s,d,n='',log=True):
        s.s.write(d); s.s.flush()
        if log and not s.quiet: TXLOG(d,n)
    def _one(s):
        b=s.buf
        if not b: return None
        c=b[0]
        if c==0x0d: del b[0:1]; return ('cr',None,b'\x0d')
        if c in (0x7a,0x5a):
            if len(b)<2: return False
            if b[1]==0x0d: s.acks+=1; t=bytes(b[0:2]); del b[0:2]; return ('ack',chr(c),t)
            t=bytes(b[0:1]); del b[0:1]; return ('byte',chr(c),t)
        if c==0x46:
            if len(b)<3: return False
            if b[2]==0x0d: s.fval=sb(b[1]); t=bytes(b[0:3]); del b[0:3]; return ('F',s.fval,t)
            t=bytes(b[0:1]); del b[0:1]; return ('byte','F?',t)
        if c==0x56:
            if len(b)<4: return False
            if b[3]==0x0d: t=bytes(b[0:4]); del b[0:4]; return ('V',t[1:3],t)
            t=bytes(b[0:1]); del b[0:1]; return ('byte','V?',t)
        if c in (0x74,0x72,0x54,0x52):
            eff=c in (0x54,0x52); rtr=c in (0x72,0x52); hdr=6 if eff else 4
            if len(b)<hdr: return False
            dlc=b[hdr-1]
            if dlc>8: t=bytes(b[0:1]); del b[0:1]; return ('byte','frm?',t)
            msg=hdr+(0 if rtr else dlc)
            if len(b)<msg+1: return False
            if b[msg]==0x0d:
                t=bytes(b[0:msg+1]); idv=int.from_bytes(t[1:hdr-1],'big')
                data=bytes(t[hdr:hdr+(0 if rtr else dlc)]); del b[0:msg+1]
                s.frames.append((time.time(),idv,dlc,data))
                return ('frame',(chr(c),idv,dlc,data,rtr),t)
            t=bytes(b[0:1]); del b[0:1]; return ('byte','frm?',t)
        t=bytes(b[0:1]); del b[0:1]; return ('byte','??',t)
    def _emit(s,k,v,raw):
        if s.quiet or k=='cr': return
        if s.rx_cap is not None and s.rx_n>=s.rx_cap:
            s.rx_supp+=1; return
        s.rx_n+=1
        if   k=='ack':   RXLOG(raw,f"ACK '{v}'  <- transmit accepted (Z)")
        elif k=='F':     RXLOG(raw,f"F status = {v}")
        elif k=='V':     RXLOG(raw,f"V id = {hx(v)}")
        elif k=='frame':
            ch,idv,dlc,data,rtr=v
            RXLOG(raw,f"FRAME '{ch}' {dec(idv)} DLC={dlc} data=[{hx(data)}]")
        elif k=='cr':    pass
        else:            RXLOG(raw,f"GARBAGE ({v})")
    def pump(s,dur=0.0):
        t0=time.time()
        while True:
            n=s.s.in_waiting
            if n: s.buf+=s.s.read(n)
            while True:
                r=s._one()
                if r is None or r is False: break
                s._emit(*r)
            if dur<=0:
                if s.s.in_waiting: continue
                break
            if time.time()-t0>=dur: break
            time.sleep(0.003)
    def cmd(s,d,wait=0.2): s.w(d,'',log=False); s.pump(wait)
    def reinit(s):
        for d in (b'RST\r',b'C\r',b'S\x02\r',b'O\r',b'm\x00\x00\r',b'M\x00\x00\r',b'm\x00\x00\x00\x00\r',b'M\x00\x00\x00\x00\r'):
            s.cmd(d,0.18)

def frame_to_cmd(idv,dlc,data):
    return b'T'+idv.to_bytes(4,'big')+bytes([dlc])+bytes(data)+b'\r'

def main():
    c=C(); c.reinit(); time.sleep(0.3)
    c.quiet=False   # log ALL traffic (health + sniff + poll + post-wedge), NOTHING suppressed
    print("LEGEND: TX=bytes WE send | RX=bytes the CHIP emits (forwarded CAN frame / Z=tx-ack / F=status /", flush=True)
    print("        GARBAGE=UART-desync byte) | '-> [script note]'=my annotation, NOT chip output.", flush=True)
    print("        RX frame with another program_id = another bus node; an RX frame byte-identical to our TX", flush=True)
    print("        = the OTHER master's copy of that broadcast, NOT a chip echo.\n", flush=True)
    # ---- Health gate: a healthy chip ACKs I_AM_HERE (DLC=1, safe). Deep-wedged chip won't. ----
    IAM=b'T\x10\x01\xCC\x0B\x01\x0E\r'
    hh=0
    for _ in range(3):
        a=c.acks; c.w(IAM,'health probe I_AM_HERE'); t=time.time()
        while time.time()-t<0.5:
            c.pump(0.0)
            if c.acks>a: hh+=1; break
            time.sleep(0.003)
        c.pump(0.1)
    print(f"chip health (I_AM_HERE x3 -> Z): {hh}/3", flush=True)
    if hh<2:
        print("chip NOT healthy at start (deep-wedged) — needs reboot/power-cycle. abort.", flush=True)
        sys.exit(2)
    # ---- Phase A: sniff, find a request that gets a response ----
    print("=== SNIFF 20s: capture REQUEST frames from the active bus master ===", flush=True)
    c.frames=[]; c.pump(20.0)
    allreq={}    # req_id -> [count, dlc, data, answered?]
    resp_ids=set(f[1] for f in c.frames if mt(f[1])==2)
    for tm,idv,dlc,data in c.frames:
        if mt(idv)==0:   # a REQUEST (message_type REQUEST)
            e=allreq.setdefault(idv,[0,dlc,data,False]); e[0]+=1; e[1]=dlc; e[2]=data
            if (idv|(2<<27)) in resp_ids: e[3]=True
    nreq=sum(1 for f in c.frames if mt(f[1])==0)
    print(f"  captured {len(c.frames)} frames ({nreq} requests, {len(resp_ids)} distinct responses); "
          f"{len(allreq)} request-types", flush=True)
    answered = {k:v for k,v in allreq.items() if v[3]} if allreq else {}
    if answered:
        req_id=max(answered, key=lambda k: answered[k][0]); cnt,dlc,data,ans=answered[req_id]
        print("  source: sniffed request whose response we SAW in-window (will get real responses)", flush=True)
    else:
        # known-good GET_PARAMETER_VALUE -> device 209 (responded every poll in an earlier run)
        req_id, dlc, data = 0x0001D116, 4, bytes([0x01,0x01,0x02,0x00])
        print("  source: no answered request sniffed -> known-good responder 0x0001D116 (GET_PARAMETER_VALUE->209)", flush=True)
    exp_id=req_id|(2<<27)
    cmd=frame_to_cmd(req_id,dlc,data)
    print(f"  CHOSEN request: {dec(req_id)} DLC={dlc} data=[{hx(data)}]")
    print(f"  expect response: {dec(exp_id)}")
    print(f"  UART command to send: {hx(cmd)}")

    # ---- Phase B: poll exactly like the app ----
    print(f"\n=== POLL like wb-mqtt-smartweb: request -> WAIT response -> {POLL_MS}ms -> next ===", flush=True)
    c.quiet=False
    MAXP=120; wedged=False; frames_at_start=len(c.frames)
    for i in range(1,MAXP+1):
        cyc=time.time()
        a0=c.acks; fn0=len(c.frames)
        c.w(cmd,f"REQUEST #{i}  {dec(req_id)} DLC={dlc} data=[{hx(data)}]")
        # 1) wait for chip 'Z' (wedge detector)
        tz=time.time(); got_z=False
        while time.time()-tz<0.2:
            c.pump(0.0)
            if c.acks>a0: got_z=True; break
            time.sleep(0.002)
        if not got_z:
            print(f"          *** NO 'Z' ACK === WEDGE on REQUEST #{i} ***", flush=True)
            wedged=True
        else:
            # 2) wait for the device RESPONSE (like the app's async handler), up to 250ms
            tr=time.time(); got_resp=False; lat=None
            while time.time()-tr<0.25:
                c.pump(0.0)
                for (tm,idv,dl,da) in c.frames[fn0:]:
                    if idv==exp_id: got_resp=True; lat=(time.time()-cyc)*1000; break
                if got_resp: break
                time.sleep(0.002)
            print(f"          -> [script note] Z-ack ok; expected SmartWeb response {('arrived +%.0fms'%lat) if got_resp else 'NOT seen within 250ms'}", flush=True)
        if wedged: break
        # pace to POLL_MS like the periodic task
        rest=POLL_MS/1000.0-(time.time()-cyc)
        if rest>0: c.pump(rest)

    if wedged:
        print("\n========== POST-WEDGE DIAGNOSIS (ALL traffic, NOTHING suppressed) ==========", flush=True)
        print("-- read every status / identity register --", flush=True)
        for d,desc in [(b'F\r','F = CAN status'),(b'V\r','V = firmware id'),
                       (b'N\r','N = serial num'),(b'v\r','v = sub-version')]:
            c.w(d,desc); c.pump(0.45)
        print("-- listen 4s: ALL RX after wedge (forwarded frames + garbage, uncut) --", flush=True)
        c.pump(4.0)
        IAM=b'T\x10\x01\xCC\x0B\x01\x0E\r'   # DLC=1, safe recovery probe
        def recover_probe(label):
            a=c.acks; c.w(IAM,f"resend I_AM_HERE DLC=1 [{label}]"); t=time.time()
            while time.time()-t<0.4:
                c.pump(0.0)
                if c.acks>a: print(f"          -> RECOVERED (got Z) [{label}]",flush=True); return True
                time.sleep(0.003)
            print(f"          -> still wedged (no Z) [{label}]",flush=True); return False
        print("\n========== RECOVERY ATTEMPTS (resend after each) ==========", flush=True)
        print("\n-- 1) plain retry, no recovery --", flush=True)
        ok=recover_probe("no recovery")
        if not ok:
            print("\n-- 2) RCY (chip busoff-recovery command) --", flush=True)
            c.w(b'RCY\r','RCY'); c.pump(0.45); ok=recover_probe("after RCY")
        if not ok:
            print("\n-- 3) close + open --", flush=True)
            c.w(b'C\r','C close'); c.pump(0.35); c.w(b'O\r','O open'); c.pump(0.35); ok=recover_probe("after C+O")
        if not ok:
            print("\n-- 4) full RST + reinit (what the driver's restart does) --", flush=True)
            c.quiet=True; c.reinit(); c.quiet=False; ok=recover_probe("after RST+reinit")
        if ok:
            print("\n-- confirm SUSTAINED recovery: 5x I_AM_HERE --", flush=True)
            s=0
            for _ in range(5):
                if recover_probe("sustain"): s+=1
                c.pump(0.1)
            print(f"          => sustained {s}/5", flush=True)
        if c.rx_supp:
            print(f"\n[post-wedge RX flood: +{c.rx_supp} more RX lines suppressed for readability]", flush=True)
    incoming=len(c.frames)-frames_at_start
    if not wedged:
        print(f"\n=== survived {MAXP} app-like polls ({MAXP*POLL_MS/1000:.0f}s) WITHOUT wedge; "
              f"{incoming} incoming frames during poll (bus {'ACTIVE' if incoming>MAXP*0.3 else 'QUIET'}) ===", flush=True)
    print(f"\nDONE. wedged={wedged} incoming_during_poll={incoming}", flush=True)
if __name__=='__main__': main()
