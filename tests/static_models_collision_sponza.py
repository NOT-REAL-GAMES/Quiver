#!/usr/bin/env python3
"""Optional CPU-only collision benchmark using the actual static Sponza asset."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

import numpy as np


def export_fixture(model,output,scale=1):
    text=model.read_text()
    if (not re.search(r'numJoints\s+1\b',text) or
            not re.search(r'"origin"\s+-1\s+\(\s*0\s+0\s+0\s*\)\s+\(\s*0\s+0\s+0\s*\)',text)):
        raise RuntimeError('this actual-asset fixture requires the identity-joint Sponza model')
    chunks=[]
    for mesh in re.findall(r'\bmesh\s*\{(.*?)\}',text,re.S):
        vertices=[(int(i),int(first),int(count)) for i,first,count in
            re.findall(r'\bvert\s+(\d+)\s+\([^)]*\)\s+(\d+)\s+(\d+)',mesh)]
        weights=[(int(i),int(joint),float(bias),float(x),float(y),float(z)) for i,joint,bias,x,y,z in
            re.findall(r'\bweight\s+(\d+)\s+(\d+)\s+(\S+)\s+\(\s*(\S+)\s+(\S+)\s+(\S+)\s*\)',mesh)]
        triangles=[tuple(map(int,q)) for q in re.findall(r'\btri\s+\d+\s+(\d+)\s+(\d+)\s+(\d+)',mesh)]
        if not vertices or not weights or not triangles:raise RuntimeError('actual mesh section is incomplete')
        positions=np.empty((len(vertices),3),dtype='<f4')
        for i,first,count in vertices:
            if count!=1 or weights[first][0]!=first or weights[first][1]!=0 or weights[first][2]!=1:
                raise RuntimeError('actual identity-joint weighting changed')
            positions[i]=weights[first][3:]
        chunks.append(positions[np.asarray(triangles,dtype=np.uint32)])
    triangles=np.concatenate(chunks)
    if triangles.shape!=(262266,3,3):raise RuntimeError('actual Sponza triangle count changed')
    triangles*=scale
    with output.open('wb') as file:
        file.write(struct.pack('<I',len(triangles)));file.write(triangles.astype('<f4',copy=False).tobytes())
    return len(triangles)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',type=Path,required=True);parser.add_argument('--test-binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--scale',type=float,default=1)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    if not 0<args.scale<=1024:parser.error('finite scale in(0,1024] required')
    fixture=args.output/'sponza-triangles.bin';count=export_fixture(args.model,fixture,args.scale)
    frozen=args.output/'collision-test';shutil.copy2(args.test_binary.resolve(),frozen)
    result=subprocess.run([str(frozen.resolve()),str(fixture.resolve())],capture_output=True,text=True,timeout=180)
    (args.output/'output.log').write_text(result.stdout+result.stderr)
    report={'complete':result.returncode==0,'model':str(args.model.resolve()),'model_sha256':hashlib.sha256(args.model.read_bytes()).hexdigest(),
        'triangles':count,'model_scale':args.scale,'test_binary_sha256':hashlib.sha256(frozen.read_bytes()).hexdigest(),
        'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'exit_code':result.returncode,
        'note':'Single-threaded CPU time for BVH build and10000 point/player-box queries. No renderer or GPU is initialized.',
        'output':result.stdout+result.stderr}
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(result.stdout,end='');result.check_returncode()


if __name__=='__main__':main()
