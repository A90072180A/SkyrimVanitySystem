"""Read active MO2 assets, calculate reviewed height candidates, merge user pairs."""
from __future__ import annotations
from collections import Counter
from functools import lru_cache
import hashlib
import json
import math
import os
from pathlib import Path
import re
import time
import xml.etree.ElementTree as ET
from .formats import (FormatError, Resources, active_plugins, load_records, bounded_read,
                      parse_nif, parse_tri, relative_model, file_hash, sha256)
from . import geometry as geo
from vha_fileio import absolute_path, readable_file, ensure_directory, unlink_missing_ok

VERSION='0.17.0-offline2'
DEFAULT_ANCHOR='[AFxII] Converse AS.esp|0000080A'
STOCK_WORDS=('stocking','pantyhose','tights','bodystocking','hosiery','丝袜','连裤袜')

def atomic_json(path:Path,value,limit=64*1024*1024):
    payload=(json.dumps(value,ensure_ascii=False,allow_nan=False,indent=2)+'\n').encode('utf-8')
    if len(payload)>limit:raise ValueError('output exceeds size limit')
    ensure_directory(path.parent)
    tmp=path.with_name(path.name+f'.{os.getpid()}.tmp')
    try:
        with tmp.open('wb') as f:f.write(payload);f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:
        unlink_missing_ok(tmp)

def read_json(path):return json.loads(bounded_read(path).decode('utf-8-sig'),parse_constant=lambda x:(_ for _ in ()).throw(ValueError('nonfinite JSON')))

def transforms_equal(a,b):
    def meaningful(parents):
        identity={'rotation':(1.,0.,0.,0.,1.,0.,0.,0.,1.),'translation':(0.,0.,0.),'scale':1.}
        return [p for p in parents if p!=identity]
    return (a.get('local')==b.get('local') and a.get('skin')==b.get('skin')
            and meaningful(a.get('parentTransforms',[]))==meaningful(b.get('parentTransforms',[])))

def add_morph(points,offsets,value):
    if not math.isfinite(value) or abs(value)>10:raise FormatError('preset-morph-out-of-range')
    out=list(points)
    for i,d in offsets.items():
        if i>=len(out):raise FormatError('morph-index-out-of-bounds')
        out[i]=geo.add(out[i],geo.mul(d,value))
    return out

def dense(offsets,count):
    values=[(0.,0.,0.)]*count
    for i,d in offsets.items():
        if i>=count:raise FormatError('morph-index-out-of-bounds')
        values[i]=d
    return values

def preset_values(path:Path|None,name:str|None,weight:float):
    if path is None:return {},None
    payload=bounded_read(path,8*1024*1024)
    if b'<!DOCTYPE' in payload.upper() or b'<!ENTITY' in payload.upper():raise FormatError('XML-entities-not-allowed')
    root=ET.fromstring(payload)
    presets=[p for p in root.iter('Preset') if name is None or p.get('name')==name]
    if len(presets)!=1:raise FormatError('select one exact preset name from the XML')
    values={};seen=set()
    for node in presets[0].iter('SetSlider'):
        key,size=node.get('name',''),node.get('size','')
        if not key or size not in ('small','big'):continue
        if (key,size) in seen:raise FormatError('duplicate-preset-slider')
        seen.add((key,size));v=float(node.get('value','nan'))/100.
        if not math.isfinite(v) or abs(v)>10:raise FormatError('invalid-preset-slider')
        values.setdefault(key,{})[size]=v
    # Missing values in a preset are zero. No other presets/defaults are guessed.
    result={k:(1-weight/100)*v.get('small',0)+weight/100*v.get('big',0) for k,v in values.items()}
    for k in ('NoHeel','Heel','HiHeelz_CBBE','HiHeelz_CBBE_to_UBE'):
        if abs(result.get(k,0))>1e-8:raise FormatError('height sliders in the body preset must be zero; do not bake a footwear posture into all bodies')
    return {k:v for k,v in result.items() if v}, {'kind':'preset','path':str(path),'sha256':sha256(payload),'name':presets[0].get('name','')}

class Scanner:
    def __init__(self,data:Path,profile:Path,output:Path,weight=0.,preset:Path|None=None,
                 preset_name=None,archive_list:Path|None=None,race:str|None=None,progress=lambda s:None):
        self.data,self.profile,self.output=absolute_path(data),absolute_path(profile),absolute_path(output)
        if not math.isfinite(weight) or not 0<=weight<=100:raise ValueError('weight must be 0..100')
        self.weight=weight;self.progress=progress;self.race=race
        self.morphs,self.preset_source=preset_values(preset,preset_name,weight)
        self.plugins=active_plugins(self.data,self.profile)
        # Known plugin-associated archives, plus an explicit ordered list for INI archives.
        archives=[]
        if archive_list:
            for line in bounded_read(archive_list,1024*1024).decode('utf-8-sig').splitlines():
                n=line.strip()
                if not n or n.startswith('#'):continue
                if Path(n).name!=n or '/' in n or '\\' in n or not n.lower().endswith('.bsa'):raise FormatError('archive list requires filenames in Data')
                archives.append(self.data/n)
        for plugin in self.plugins:
            for name in (Path(plugin).with_suffix('.bsa').name,Path(plugin).stem+' - Textures.bsa'):
                if readable_file(self.data/name) and self.data/name not in archives:archives.append(self.data/name)
        self.resources=Resources(self.data,archives)
        self.records,self.plugin_sources=load_records(self.data,self.plugins,progress)
        self.profile_sources=[{'kind':'profile','path':str(self.profile/n),'sha256':file_hash(self.profile/n)}
                              for n in ('plugins.txt','loadorder.txt') if readable_file(self.profile/n)]
        if archive_list:self.profile_sources.append({'kind':'profile','path':str(absolute_path(archive_list)),'sha256':file_hash(archive_list)})
        self.rows=[];self.private={};self.candidates=[]
        self.created=time.time();self.used_sources={}

    def resource(self,path):
        data,src=self.resources.read(path)
        self.used_sources[(src['kind'],src['path'],src['resource'])]=src
        return data,src

    @lru_cache(maxsize=6)
    def shapes(self,model,weight_slider):
        payload,src=self.resource(model);high=parse_nif(payload);sources=[src]
        if not high:raise FormatError('no-BSTriShape-in-NIF')
        if weight_slider:
            if not re.search(r'_1\.nif$',model,re.I):raise FormatError('weight-slider-model-must-end-in-_1.nif')
            lowmodel=re.sub(r'_1\.nif$','_0.nif',model,flags=re.I)
            payload,src=self.resource(lowmodel);low=parse_nif(payload);sources.append(src)
            for h in high:
                lo=[s for s in low if s['name']==h['name']]
                if h['status']!='complete':continue
                if len(lo)!=1 or lo[0]['status']!='complete' or lo[0]['triangles']!=h['triangles'] or len(lo[0]['points'])!=len(h['points']) or lo[0]['bodyTris']!=h['bodyTris'] or not transforms_equal(lo[0],h):
                    h['status']='low-high-source-mismatch';continue
                h['points']=[geo.add(geo.mul(a,1-self.weight/100),geo.mul(b,self.weight/100)) for a,b in zip(lo[0]['points'],h['points'])]
        for shape in high:
            shape['morphs']={};shape['nativeMorphNames']=[]
            if shape['status']!='complete':continue
            if len(set(shape['bodyTris']))!=1:
                shape['capability']='missing-or-ambiguous-BODYTRI';continue
            try:
                b,src=self.resource(shape['bodyTris'][0]);sources.append(src)
                allm,inventory=parse_tri(b,{'NoHeel','Heel','HiHeelz_CBBE','HiHeelz_CBBE_to_UBE',*self.morphs})
                shape['nativeMorphNames']=inventory.get(shape['name'],[])
                shape['morphs']=allm.get(shape['name'],{})
                for n,o in shape['morphs'].items():
                    if any(i>=len(shape['points']) for i in o):raise FormatError('morph-index-out-of-bounds')
                for name,value in self.morphs.items():
                    if name in shape['morphs']:shape['points']=add_morph(shape['points'],shape['morphs'][name],value)
                shape['capability']='parsed'
            except (OSError,ValueError,struct_error) as e:
                shape['capability']='TRI-error: '+str(e)
        return high,sources

    def scan(self):
        armors=[r for r in self.records.values() if r['type']=='ARMO' and not r['deleted']]
        def relevant_armor(r):
            if r['slots']&((1<<7)|(1<<8)|(1<<18)|(1<<23)) or any(s in (r['name']+' '+r['editorID']).casefold() for s in STOCK_WORDS):
                return True
            # Localized names and full-body stockings are not reliably labelled.
            # Inspect all active UBE female armor donors, not only guessed names
            # or original stocking slots. Presence of a real NoHeel delta is
            # still required; an unknown mesh is never manufactured as a sock.
            for key in r['addons']:
                a=self.records.get(key.casefold())
                if not a or a['type']!='ARMA' or a['deleted']:continue
                if a['slots']&(1<<7):return True
                try:
                    if a['femaleModel'] and relative_model(a['femaleModel']).casefold().startswith('!ube\\'):return True
                except ValueError:pass
            return False
        relevant=[r for r in armors if relevant_armor(r)]
        for number,armor in enumerate(relevant):
            self.progress(f'模型 {number+1}/{len(relevant)}：{armor["name"]}')
            if not armor['addons']:
                self.rows.append({'key':armor['id'],'armor':armor['id'],'name':armor['name'],'kind':'unknown','status':'missing-armature-or-template'});continue
            for addonid in dict.fromkeys(armor['addons']):
                addon=self.records.get(addonid.casefold())
                if not addon or addon['type']!='ARMA' or addon['deleted']:
                    self.rows.append({'key':armor['id']+'::'+addonid,'armor':armor['id'],'addon':addonid,'name':armor['name'],'kind':'unknown','status':'missing-winning-ARMA'});continue
                model=addon['femaleModel']
                if not model:continue
                try:model=relative_model(model)
                except ValueError as e:model='';status=str(e)
                else:status='not-measured'
                is_ube=model.casefold().startswith('!ube\\')
                # Include unsupported alternatives in inventory instead of silently equating them with UBE.
                key=armor['id']+'::'+addonid
                row={'key':key,'armor':armor['id'],'addon':addonid,'name':armor['name'],'editorID':armor['editorID'],
                     'model':model,'kind':'footwear' if addon['slots']&(1<<7) else 'stocking-candidate',
                     'slots':addon['slots'],'UBE':is_ube,'weightSlider':addon['femaleWeightSlider'],
                     'race':addon['race'],'additionalRaces':addon['races'],'status':status,'shapes':[]}
                self.rows.append(row)
                if not model:continue
                if self.race and self.race.casefold() not in [x.casefold() for x in [addon['race'],*addon['races']]]:
                    row['status']='not-selected-race';continue
                if not is_ube:row['status']='non-UBE-alternative-not-fitted';continue
                try:
                    shapes,sources=self.shapes(model,addon['femaleWeightSlider']);row['sources']=sources
                    row['shapes']=[{'name':s['name'],'status':s['status'],'vertices':len(s.get('points',[])),
                                    'triangles':len(s.get('triangles',[])),'bodyTris':s['bodyTris'],
                                    'capability':s.get('capability'),'heightMorphs':{n:len(v) for n,v in s.get('morphs',{}).items() if n in ('NoHeel','Heel')}} for s in shapes]
                    if row['kind']=='footwear':
                        selected=[s for s in shapes if s['status']=='complete' and s['name'].casefold()=='feet']
                    else:
                        selected=[s for s in shapes if s['status']=='complete' and any(geo.norm2(d)>0 for d in s['morphs'].get('NoHeel',{}).values())]
                    if len(selected)!=1:
                        row['status']='no-usable-Feet' if row['kind']=='footwear' and not selected else 'NoHeel-unavailable' if not selected else 'multiple-eligible-shapes-needs-review'
                        continue
                    shape=selected[0];row['geometry']=shape['name'];row['status']='measurable'
                    if row['kind']!='footwear':row['kind']='stocking'
                    self.private[key]=(model,addon['femaleWeightSlider'],shape['name'])
                except (OSError,ValueError,struct_error) as e:row['status']='source-error: '+str(e)
        self.classify_foot_poses()
        report={'schema':1,'generatorVersion':VERSION,'createdUnix':self.created,'data':str(self.data),'profile':str(self.profile),
                'plugins':self.plugins,'archives':[str(a.path) for a in self.resources.archives],
                'context':self.context(),'coverage':'Active ARMO/ARMA inventory; UBE female SSE skinned single-partition geometry only. Missing/ambiguous assets explicitly skipped. INI-loaded archives require --archive-list.',
                'counts':dict(Counter(r['status'] for r in self.rows)),'entries':self.rows}
        atomic_json(self.output/'offline-catalog.json',report)
        return report

    def classify_foot_poses(self):
        """Distinguish flat-like/raised foot pose, not physical shoe-sole height."""
        try:
            shapes,_=self.shapes('!UBE\\Feet\\femalefeet_tangent_1.nif',True)
            refs=[s for s in shapes if s['name']=='Feet' and s['status']=='complete']
            if len(refs)!=1:return
            ref=refs[0];a=dense(ref['morphs'].get('HiHeelz_CBBE',{}),len(ref['points']));b=dense(ref['morphs'].get('HiHeelz_CBBE_to_UBE',{}),len(ref['points']))
            if not any(geo.norm2(v)>0 for v in a):return
            for row in self.rows:
                if row['kind']!='footwear' or row['status']!='measurable':continue
                self.progress('脚姿分类：'+row['name'])
                try:
                    shape,_=self.get_shape(row)
                    if not transforms_equal(ref,shape):raise FormatError('transform-mismatch')
                    target,common,_=geo.align(ref['points'],ref['triangles'],shape['points'],shape['triangles'])
                    ids=sorted({i for t in common for i in t})
                    aa=sum(geo.norm2(a[i]) for i in ids);bb=sum(geo.norm2(b[i]) for i in ids);ab=sum(geo.dot(a[i],b[i]) for i in ids)
                    ay=sum(geo.dot(a[i],geo.sub(target[i],ref['points'][i])) for i in ids)
                    by=sum(geo.dot(b[i],geo.sub(target[i],ref['points'][i])) for i in ids)
                    determinant=aa*bb-ab*ab
                    if determinant<=1e-12*max(aa*bb,1e-30):raise FormatError('degenerate-native-basis')
                    first,second=(ay*bb-by*ab)/determinant,(by*aa-ay*ab)/determinant
                    error=sum(geo.norm2(geo.sub(geo.sub(target[i],ref['points'][i]),geo.add(geo.mul(a[i],first),geo.mul(b[i],second)))) for i in ids)
                    normalized=math.sqrt(error/aa)
                    kind='flat-like-foot-pose' if abs(first)<.1 and abs(second)<.1 else 'raised-foot-pose' if first>.1 else 'other-foot-pose'
                    if normalized>.15:kind='unreliable-native-fit'
                    row['footPose']={'kind':kind,'HiHeelz_CBBE':first,'HiHeelz_CBBE_to_UBE':second,'normalizedResidual':normalized,
                                     'semantics':'native foot coefficients; not centimetres or physical heel/sole height'}
                except (ValueError,OSError,struct_error) as e:row['footPose']={'kind':'unresolved','reason':str(e)}
        except (ValueError,OSError,struct_error):
            # Classification is optional; paired height mapping can still use
            # the explicitly chosen flat reference shoe without this foot asset.
            return

    def context(self):
        return {'weight':self.weight,'bodyMorphs':self.morphs,'preset':self.preset_source,'raceFilter':self.race,
                'semantics':'Installed NIF baseline plus optional explicit preset; not live OBody or animation state. Do not apply a preset again to already preset-built meshes.'}

    def get_shape(self,row):
        model,weighted,name=self.private[row['key']]
        shapes,sources=self.shapes(model,weighted)
        selected=[s for s in shapes if s['name']==name and s['status']=='complete']
        if len(selected)!=1:raise FormatError('source-shape-no-longer-unique')
        return selected[0],sources

    def recommend(self,anchor_id=DEFAULT_ANCHOR,stocking_keys=None,heel_max=2.,max_residual=.15,max_pairs=20000):
        if not 1<=heel_max<=10 or not 0<=max_residual<=1:raise ValueError('invalid limits')
        shoes=[r for r in self.rows if r['kind']=='footwear' and r['status']=='measurable']
        socks=[r for r in self.rows if r['kind']=='stocking' and r['status']=='measurable' and (not stocking_keys or r['key'] in stocking_keys)]
        anchors=[r for r in shoes if r['armor'].casefold()==anchor_id.casefold() or r['key'].casefold()==anchor_id.casefold()]
        if len(anchors)!=1:raise FormatError('flat reference shoe not uniquely found; select its exact armor::addon key')
        if len(shoes)*len(socks)>max_pairs:raise FormatError(f'{len(socks)} × {len(shoes)} exceeds {max_pairs} pair budget; select fewer stockings')
        if not shoes or not socks:raise FormatError('no measurable stockings/shoes')
        anchor_row=anchors[0];anchor,anchor_sources=self.get_shape(anchor_row)
        candidates=[];number=0
        for sock in socks:
            stock,stock_sources=self.get_shape(sock);n=len(stock['points'])
            noheel=dense(stock['morphs'].get('NoHeel',{}),n);heel=dense(stock['morphs'].get('Heel',{}),n)
            flat=[geo.add(p,d) for p,d in zip(stock['points'],noheel)]
            map_cache={}
            for shoe in shoes:
                number+=1;self.progress(f'高度配对 {number}/{len(socks)*len(shoes)}：{sock["name"]} + {shoe["name"]}')
                row={'index':len(candidates),'stocking':sock['armor'],'stockingAddon':sock['addon'],'stockingModel':sock['model'],
                     'footwear':shoe['armor'],'footwearAddon':shoe['addon'],'footwearModel':shoe['model'],
                     'stockingName':sock['name'],'footwearName':shoe['name'],'NoHeel':None,'Heel':None,
                     'status':'not-run','requiresVisualReview':True,'reference':anchor_row['key']}
                try:
                    target,target_sources=self.get_shape(shoe)
                    if not transforms_equal(anchor,target) or not transforms_equal(stock,anchor):raise FormatError('local-or-skin-transform-mismatch')
                    aligned,common,mapping=geo.align(anchor['points'],anchor['triangles'],target['points'],target['triangles'])
                    # Cache once per common surface; no all-shoes/all-stockings arrays in RAM.
                    token=hashlib.sha256(repr(common).encode()).hexdigest()
                    if token not in map_cache:map_cache[token]=geo.build_map(anchor['points'],common,flat,noheel,heel)
                    mapping_data=map_cache[token]
                    result=geo.fit(mapping_data,anchor['points'],aligned,noheel,heel,heel_max)
                    row.update(result);row['topology']=mapping
                    duplicate_socks=sum(r['armor']==sock['armor'] for r in socks)>1
                    duplicate_shoes=sum(r['armor']==shoe['armor'] for r in shoes)>1
                    row['status']='ambiguous-addon-pair' if duplicate_socks or duplicate_shoes else 'range-saturated' if result['saturated'] else 'review-residual' if result['normalizedResidual']>max_residual else 'within-mathematical-limits'
                    row['sources']=list({(x['kind'],x['path'],x.get('resource','')):x for x in [*anchor_sources,*stock_sources,*target_sources]}.values())
                except (OSError,ValueError,struct_error) as e:row['status']=str(e)
                candidates.append(row)
        report={'schema':1,'generatorVersion':VERSION,'createdUnix':time.time(),'context':self.context(),
                'heelMax':heel_max,'NoHeelMaximum':1.,'maxResidual':max_residual,
                'reference':anchor_row,'referenceAssumption':'Selected shoe is a user-confirmed flat reference; stocking NoHeel=1 is assumed flat. Neither is established by a filename alone.',
                'applySemantics':'Selected values become deliberate fixed manual pairs, not live-body-validated automatic profiles. Existing manual/ignored pairs are preserved.',
                'inputSources':self.plugin_sources+self.profile_sources+([self.preset_source] if self.preset_source else []),
                'archivePaths':[str(a.path) for a in self.resources.archives], 'data':str(self.data),
                'counts':dict(Counter(r['status'] for r in candidates)),'entries':candidates}
        atomic_json(self.output/'offline-candidates.json',report);self.candidates=candidates
        return report

# struct.error is separate from ValueError on CPython.
import struct
struct_error=struct.error

def apply_report(report_path:Path,plugins_dir:Path,indexes:list[int],allow_review=False,user_file:Path|None=None):
    """Explicit batch application; keep pre-existing instructions and all assets."""
    import height_editor
    report=read_json(report_path)
    if report.get('schema')!=1 or report.get('generatorVersion')!=VERSION:raise ValueError('unsupported offline report')
    if not indexes:raise ValueError('no candidates selected')
    if len(set(indexes))!=len(indexes):raise ValueError('duplicate selected indexes')
    editor=height_editor.Editor(plugins_dir,user_file)
    resources=Resources(Path(report['data']),[Path(x) for x in report.get('archivePaths',[])])
    # A different active load order can change every winning ARMA: check it first.
    for src in report['inputSources']:
        if file_hash(Path(src['path']))!=src['sha256']:raise ValueError('input changed; rescan: '+src['path'])
    protected={(r['stocking'].casefold(),r['footwear'].casefold()) for r in editor.user.get('pairs',[])}
    ignored={r['armor'].casefold() for r in editor.user.get('items',[]) if r['kind']=='ignore'}
    protected.update((r['stocking'].casefold(),r['footwear'].casefold()) for r in editor.base.get('signedHeightOverrides',[]))
    legacy_shoes={k.casefold() for k in editor.base.get('heels',{})}
    added=[];preserved=[];checked=set()
    for i in indexes:
        if not isinstance(i,int) or not 0<=i<len(report['entries']):raise ValueError('invalid candidate index')
        row=report['entries'][i]
        if row['status'] not in (('within-mathematical-limits','manual-adjusted','review-residual') if allow_review else ('within-mathematical-limits','manual-adjusted')):
            raise ValueError(f'candidate {i} is not applicable: {row["status"]}')
        key=(row['stocking'].casefold(),row['footwear'].casefold())
        if key in protected or key[0] in ignored or key[1] in ignored or key[1] in legacy_shoes:
            preserved.append(i);continue
        height_editor.controls(float(row['NoHeel']),float(row['Heel']),editor.maximum)
        for src in row['sources']:
            token=(src['resource'],src['sha256'])
            if token in checked:continue
            _,now=resources.read(src['resource'])
            if now['sha256']!=src['sha256'] or now['kind']!=src['kind'] or os.path.normcase(str(absolute_path(Path(now['path']))))!=os.path.normcase(str(absolute_path(Path(src['path'])))):
                raise ValueError('resource/provider changed; rescan: '+src['resource'])
            checked.add(token)
        note=f"Reviewed offline baseline suggestion ({VERSION}), weight={report['context']['weight']:g}, residual={row['normalizedResidual']:.6f}. Revisit after mesh/body changes; not live OBody calibration."
        editor.set_pair(row['stocking'],row['footwear'],row['NoHeel'],row['Heel'],note)
        # Preserve user classifications; register only a previously unclassified donor.
        if not any(r['armor'].casefold()==key[0] for r in editor.user.get('items',[])):
            editor.mark(row['stocking'],'stocking','Offline scan: real NoHeel capability verified',row['stockingAddon'])
        protected.add(key);added.append(i)
    if len(editor.user.get('pairs',[]))>2048 or len(editor.user.get('items',[]))>2048:raise ValueError('user configuration entry limit')
    if len(json.dumps(editor.user,ensure_ascii=False).encode())>900000:raise ValueError('configuration too large; reduce selected batch')
    # Validate the entire overlay before saving with the existing editor's
    # concurrent-write and live-path guards. No partial batch is written.
    validate_user(editor.user,editor.maximum)
    if added:editor.save()
    result={'schema':1,'createdUnix':time.time(),'sourceReportSHA256':file_hash(report_path),'userFile':str(editor.path),
            'appliedIndexes':added,'preservedExistingIndexes':preserved,'semantics':'manual configuration saved; no assertion about game submission or visual fit'}
    atomic_json(report_path.parent/'offline-apply-receipt.json',result)
    return result


def validate_user(user,maximum=2.):
    """Fail rather than write an overlay that the native validator will reject."""
    import height_editor
    if not isinstance(user,dict) or set(user)-{'schema','settings','items','pairs'} or user.get('schema',1)!=1:
        raise ValueError('invalid user overlay schema/fields')
    settings=user.get('settings',{})
    booleans={'applyMorph','automaticHeight','autoDetectStockings','barefootFlatFeet','allowHeightEndpointApproximation','exportFootGeometry','exportStockingCalibration','writeRuntimeState','enableComponentSubset'}
    if not isinstance(settings,dict) or set(settings)-booleans-{'heelMax','heightMaxNormalizedResidual'}:raise ValueError('unknown user setting')
    for k,v in settings.items():
        if k in booleans and not isinstance(v,bool):raise ValueError('setting must be boolean: '+k)
        if k not in booleans and (isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v)):raise ValueError('setting must be finite: '+k)
    if 'heightMaxNormalizedResidual' in settings and not 0<=settings['heightMaxNormalizedResidual']<=1:raise ValueError('invalid residual threshold')
    height_editor.controls(0,0,maximum)
    pairs=user.get('pairs',[]);items=user.get('items',[])
    if not isinstance(pairs,list) or not isinstance(items,list) or max(len(pairs),len(items))>2048:raise ValueError('overlay entry limit')
    seen=set()
    for row in pairs:
        if not isinstance(row,dict) or set(row)-{'stocking','footwear','mode','NoHeel','Heel','note','approval'}:raise ValueError('unknown pair field')
        a,b=row.get('stocking',''),row.get('footwear','')
        if not isinstance(a,str) or not isinstance(b,str) or not height_editor.ID.fullmatch(a) or (b!='<barefoot>' and not height_editor.ID.fullmatch(b)):raise ValueError('invalid pair IDs')
        pair=a.casefold(),b.casefold()
        if pair in seen:raise ValueError('duplicate pair')
        seen.add(pair);mode=row.get('mode','manual')
        if mode not in ('manual','ignore'):raise ValueError('invalid pair mode')
        if mode=='manual':
            for name in ('NoHeel','Heel'):
                v=row.get(name)
                if isinstance(v,bool) or not isinstance(v,(int,float)):raise ValueError('invalid control value')
            height_editor.controls(row['NoHeel'],row['Heel'],maximum)
        if 'note' in row and (not isinstance(row['note'],str) or len(row['note'])>2000):raise ValueError('invalid pair note')
        if 'approval' in row:
            fields=set(height_editor.APPROVAL)|{'stockingAddon','footwearAddon'}
            if not isinstance(row['approval'],dict) or set(row['approval'])!=fields or not all(isinstance(v,str) and v for v in row['approval'].values()):raise ValueError('invalid existing approval')
    seen=set()
    for row in items:
        if not isinstance(row,dict) or set(row)-{'armor','kind','note','addons'}:raise ValueError('invalid item fields')
        a=row.get('armor','')
        if not isinstance(a,str) or not height_editor.ID.fullmatch(a) or a.casefold() in seen:raise ValueError('invalid/duplicate item')
        seen.add(a.casefold())
        if row.get('kind') not in ('stocking','footwear','ignore'):raise ValueError('invalid item kind')
        if 'note' in row and not isinstance(row['note'],str):raise ValueError('invalid item note')
        if 'addons' in row and (not isinstance(row['addons'],list) or len(row['addons'])>32 or not all(isinstance(a,str) and height_editor.ID.fullmatch(a) for a in row['addons'])):raise ValueError('invalid addon list')
