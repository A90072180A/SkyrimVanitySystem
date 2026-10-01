"""Build a data-free MO2 tools overlay from the exact checked-out source."""
from pathlib import Path
import argparse
import hashlib
import json
import zipfile
from offline.foot_reference import TOOL_VERSION, RUNTIME_VERSION


def package(output, commit):
    root=Path(__file__).resolve().parent
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True)
    files={}
    for p in sorted(root.rglob('*')):
        if p.is_file() and '__pycache__' not in p.parts and p.suffix.lower() in ('.py','.cpp'):
            files['tools/'+p.relative_to(root).as_posix()]=p.read_bytes()
    files['docs/VanityUBEHeelAdapter/TOOLS-'+TOOL_VERSION+'.zh-CN.md']=(root.parent/('TOOLS-'+TOOL_VERSION+'.zh-CN.md')).read_bytes()
    manifest={'toolVersion':TOOL_VERSION,'runtimeVersion':RUNTIME_VERSION,'sourceCommit':commit,
              'includesRuntimeDLL':False,'changesModelsOrPersonalConfiguration':False,
              'files':{n:hashlib.sha256(b).hexdigest() for n,b in files.items()}}
    files['VHA-TOOLS-BUILD.json']=(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n').encode()
    with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name,data in sorted(files.items()):z.writestr(name,data)
    with zipfile.ZipFile(output) as z:
        if z.testzip():raise ValueError('CRC failure')
        for name,digest in manifest['files'].items():
            if hashlib.sha256(z.read(name)).hexdigest()!=digest:raise ValueError('manifest mismatch')
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True);p.add_argument('--commit',required=True)
    args=p.parse_args();package(args.output,args.commit)
