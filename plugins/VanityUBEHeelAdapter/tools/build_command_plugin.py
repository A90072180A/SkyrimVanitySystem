#!/usr/bin/env python3
"""Generate an original ESL-flagged mailbox ESP. No borrowed game records.

Requires the companion 0.17.0+ DLL to do anything. Not an old-DLL hotfix.
One master (Skyrim.esm); one float GLOB VHA_Reload, local FormID 0x800.
Console: set VHA_Reload to 1 (reload) or to 2 (status).
"""
from pathlib import Path
import argparse
import struct

def sub(sig,data):return sig.encode('ascii')+struct.pack('<H',len(data))+data
def record(sig,data,flags=0,form=0):
    return struct.pack('<4sIIIHHHH',sig.encode('ascii'),len(data),flags,form,0,0,44,0)+data

def payload():
    header=sub('HEDR',struct.pack('<fII',1.7,1,0x801))+sub('CNAM',b'Vanity UBE Heel Adapter\0')
    header+=sub('SNAM',b'Console reload mailbox. Requires Adapter DLL 0.17.0 or later.\0')
    header+=sub('MAST',b'Skyrim.esm\0')+sub('DATA',bytes(8))
    glob=record('GLOB',sub('EDID',b'VHA_Reload\0')+sub('FNAM',b'f')+sub('FLTV',struct.pack('<f',0)),form=0x01000800)
    group=struct.pack('<4sI4siHHI',b'GRUP',24+len(glob),b'GLOB',0,0,0,0)+glob
    return record('TES4',header,flags=0x200)+group

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_bytes(payload());print(a.out)
if __name__=='__main__':main()
