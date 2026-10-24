import numpy as np, struct, sys
def load_ref(prefix):
    cnt=struct.unpack('<%di'%1400, open(prefix+'.cnt','rb').read()[:1400*4])
    raw=np.frombuffer(open(prefix+'.pcm','rb').read(),dtype='<i2').reshape(-1,2).astype(np.float32)
    out=[];pos=0
    for c in cnt:
        ch=raw[pos:pos+c]; pos+=c
        out.append(ch[:(c//2)*2].reshape(-1,2,2).mean(axis=1) if c>800 else ch)
    return np.concatenate(out)
def load_ours(path): return np.frombuffer(open(path,'rb').read(),dtype='<i2').reshape(-1,2).astype(np.float32)
def env(x,w=3277): n=len(x)//w; return np.sqrt((x[:n*w].reshape(n,w)**2).mean(axis=1))
def find_lag(r,o):
    er,eo=env(r.mean(axis=1)),env(o.mean(axis=1)); best=(-2,0)
    for lag in range(-250,251):
        a=er[max(0,lag):]; b=eo[max(0,-lag):]; n=min(len(a),len(b))
        if n<100: continue
        a=a[:n]; b=b[:n]
        if a.std()==0 or b.std()==0: continue
        c=np.corrcoef(a,b)[0,1]
        if c>best[0]: best=(c,lag)
    return best
def fine(rm,om,off,t0,W=32768,pad=4000):
    ri=int(t0*32768); a=rm[ri:ri+W]; oc=ri-off
    if oc-pad<0 or oc+W+pad>len(om): return None
    b=om[oc-pad:oc+W+pad]; A=a-a.mean(); nA=np.linalg.norm(A)
    # correlação cruzada via FFT
    n=1<<int(np.ceil(np.log2(len(b)+W)))
    fa=np.fft.rfft(A,n); fb=np.fft.rfft(b-b.mean(),n)
    cc=np.fft.irfft(fb*np.conj(fa),n)[:2*pad+1]
    # normalização local
    cs=np.cumsum(np.concatenate([[0],(b-b.mean())**2])); 
    norm=np.sqrt(cs[W:W+2*pad+1]-cs[:2*pad+1])*nA+1e-9
    r=cc/norm; s=int(np.argmax(r)); seg=b[s:s+W]
    g=np.dot(a,seg)/(np.dot(a,a)+1e-9)
    return r[s],g,s-pad,np.sqrt((a**2).mean()),np.sqrt((seg**2).mean())
for name,rp,op in [('TUDO','/tmp/refall','/tmp/o_0x3F.pcm'),('só PCM (FIFO A+B)','/tmp/refm0x30','/tmp/o_0x30.pcm'),('só PSG (4 canais)','/tmp/refm0x0F','/tmp/o_0x0F.pcm')]:
    r=load_ref(rp); o=load_ours(op); rm=r.mean(axis=1); om=o.mean(axis=1)
    c,lag=find_lag(r,o); off=int(lag*3277)
    print(f'\n=== {name}: envelope corr={c:.3f}, deslocamento={lag/10:.1f}s | RMS ref={np.sqrt((rm**2).mean()):.0f}, nosso={np.sqrt((om**2).mean()):.0f}')
    print('  janela(s) | corr.sinal | ganho nosso/ref | RMS ref | RMS nosso | desl.fino')
    for t0 in [8,10,12,14,16,18]:
        f=fine(rm,om,off,t0)
        if f: print(f'  {t0:3d}-{t0+1:<3d}   |   {f[0]:6.3f}   |    {f[1]:6.3f}       | {f[3]:7.0f} | {f[4]:8.0f}  | {f[2]}')
