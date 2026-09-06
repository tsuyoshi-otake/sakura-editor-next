from pathlib import Path
import csv,json,statistics,math
p=Path('C:/Users/developer/tmp/sakura-audit-safety/.codex/goal-loop/audit-safety/release-performance')
groups={}
for f in sorted(p.glob('pair-*.csv')):
    pair=int(f.stem.split('-')[1]);variant=f.stem.split('-')[2]
    rows=list(csv.DictReader(f.open()))
    for kind,count in sorted(set((x['kind'],int(x['readers'])) for x in rows)):
        selected=[x for x in rows if x['kind']==kind and int(x['readers'])==count]
        groups.setdefault((kind,count),{}).setdefault(variant,[]).append({
            'pair':pair,'scan_us':statistics.median(float(x['scan_us']) for x in selected),
            'setup_us':statistics.median(float(x['setup_us']) for x in selected),
            'converters':sorted(set(int(x['converters']) for x in selected)),
            'max_preview':max(int(x['max_preview']) for x in selected)})
result=[]
for (kind,count),variants in groups.items():
    summary={'kind':kind,'readers':count,'samples':variants}
    for variant,rows in variants.items():
        values=sorted(x['scan_us'] for x in rows)
        summary[variant]={'median_scan_us':statistics.median(values),'p95_scan_us':values[math.ceil(len(values)*.95)-1],
                          'median_setup_us':statistics.median(x['setup_us'] for x in rows)}
    for metric in ['median_scan_us','p95_scan_us']:
        summary[metric+'_regression_percent']=(summary['fixed'][metric]/summary['borrowed'][metric]-1)*100
    summary['pass']=summary['median_scan_us_regression_percent']<=15 and summary['p95_scan_us_regression_percent']<=30 and summary['fixed']['median_setup_us']<10000
    result.append(summary)
(p/'summary.json').write_text(json.dumps(result,indent=2))
for x in result: print({k:v for k,v in x.items() if k!='samples'})
