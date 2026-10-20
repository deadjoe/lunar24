from pathlib import Path
import subprocess,json,wave,math,array,concurrent.futures
root=Path(__file__).resolve().parents[1]; exe=root.parent/'build/lunar24_render'; out=root/'assets/audio'
shared=['--seconds','12','--sr','48000','--set','effector.master=0.3','--set','effector.blend=0','--set','vcf.l_freq=0.9','--set','vcf.r_freq=0.9']
cases=[
('01-classic','经典 Drone：第 4 秒改变 TUNE，第 8 秒改变 VOLT',['--drone','1','--at','4:drone_1.tune_2=0.6','--at','8:drone_1.volt=0.6']),
('02-fm-am','Drone 3：0–4 秒持续音，4–8 秒 FM，8–12 秒 AM',['--drone','3','--set','drone_3.noise=0','--set','drone_3.att=0.1','--set','drone_3.mod=0.35','--set','drone_3.rate=0.45','--at','4:drone_3.fm=1','--at','8:drone_3.fm=0','--at','8:drone_3.am=1']),
('03-noise','Drone 3：纯白噪声，第 6 秒降低两侧滤波截止频率',['--drone','3','--set','drone_3.pitch=0','--set','drone_3.noise=0.4','--set','drone_3.att=0.1','--at','6:vcf.l_freq=0.4','--at','6:vcf.r_freq=0.4']),
('04-sample-hold','Drone 3：S&H 接回 cv，第 6 秒提高 divider',['--drone','3','--set','drone_3.noise=0','--set','drone_3.att=0.1','--set','drone_3.rate=0.5','--set','drone_3.divider=0','--cable','drone_3.sh_out=drone_3.cv_in','--at','6:drone_3.divider=0.3']),
('05-waveforms','VCO A：每 3 秒更换正弦、三角、锯齿、方波',['--note','0:0:11','--set','mixer.ch6_vol=0','--set','vco_a.sub_sel=0','--set','vco_a.morph=0.261','--at','3:vco_a.morph=0.344','--at','6:vco_a.morph=0.45','--at','9:vco_a.morph=0.552']),
('06-vco-fm','只听 VCO B：第 4、8 秒逐渐增加 A 对 B 的调频',['--note','0:0:11','--set','mixer.ch5_vol=0','--set','vco_b.cv_amt=0','--at','4:vco_b.cv_amt=0.2','--at','8:vco_b.cv_amt=0.5']),
('07-shimmer','同一短音：第 0 秒低效果量，第 6 秒提高 BLEND',['--note','0:0:1','--note','6:0:1','--program-l','cathedral.1','--program-r','cathedral.1','--at','5:effector.blend=0.65']),
('08-generator','没有按键的 GENERATOR / FM tone：第 6 秒增加 Z',['--program-l','generator.1','--program-r','generator.1','--set','effector.blend=0.5','--set','effector.x=0.4','--set','effector.y=0.3','--set','effector.z=0.1','--at','6:effector.z=0.4'])]
def run(c):
 name,label,args=c;cmd=[str(exe),*shared,*args,'--out',str(out/(name+'.wav'))];r=subprocess.run(cmd,capture_output=True,text=True);(root/'.build'/f'{name}.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(name+': '+r.stderr)
 with wave.open(str(out/(name+'.wav'))) as w:
  a=array.array('h',w.readframes(w.getnframes()));peak=max(abs(x) for x in a)/32768;rms=math.sqrt(sum((x/32768)**2 for x in a)/len(a));stats={'seconds':len(a)/2/48000,'sample_rate':48000,'bits':16,'channels':2,'peak':peak,'rms':rms,'clipped_samples':sum(abs(x)>=32767 for x in a)}
 if rms<=1e-6:raise RuntimeError(name+' silent')
 rel=[str(Path(x).relative_to(root.parent)) if x.startswith(str(root.parent)) else x for x in cmd]  # repo-relative, no local paths
 return dict(id=name,title=label,file=name+'.wav',command=rel,stats=stats)
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool: results=list(pool.map(run,cases))
(out/'manifest.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
print(json.dumps([{k:r[k] for k in ['id','stats']} for r in results],ensure_ascii=False))
