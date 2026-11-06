#!/usr/bin/env python3
"""Exercise runtime backend switches in a disposable Quake installation.

A live Vulkan display and registered Quake PAK are required. Optionally inject
one hardware graphics-pipeline failure to check automatic software fallback.
"""
import argparse
import os
from pathlib import Path
import re
import tempfile

import bench_meshlets as bench

ROOT = Path(__file__).resolve().parents[1]
STATE = re.compile(r'Radiance: requested=1 active=1 backend=(software|ray-query)')
INJECT = r'''
#include <vulkan/vulkan.h>
#include <stdio.h>
static VkShaderModule hw_modules[256]; static unsigned hw_count; static int forced_failure;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *m) {
 static PFN_vkCreateShaderModule real; if (!real) real=dlsym(RTLD_NEXT,"vkCreateShaderModule");
 VkResult r=real(d,i,a,m); if (r==VK_SUCCESS && i->codeSize>=20) {
  size_t count=i->codeSize/4; for(size_t n=5;n<count;) { uint32_t head=i->pCode[n], len=head>>16;
   if (!len || n+len>count) break;
   if ((head&65535u)==17u && len>=2 && i->pCode[n+1]==4472u) {
    if(hw_count<256)hw_modules[hw_count++]=*m; break;
   } n+=len;
  }
 } return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule m,const VkAllocationCallbacks *a) {
 static PFN_vkDestroyShaderModule real; if (!real) real=dlsym(RTLD_NEXT,"vkDestroyShaderModule");
 for(unsigned i=0;i<hw_count;i++) if(hw_modules[i]==m) {hw_modules[i]=hw_modules[--hw_count];break;}
 real(d,m,a);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *p) {
 static PFN_vkCreateGraphicsPipelines real; if (!real) real=dlsym(RTLD_NEXT,"vkCreateGraphicsPipelines");
 if (!forced_failure) for(uint32_t j=0;j<n;j++) for(uint32_t k=0;k<i[j].stageCount;k++)
  if(i[j].pStages[k].stage==VK_SHADER_STAGE_FRAGMENT_BIT) for(unsigned z=0;z<hw_count;z++)
   if(i[j].pStages[k].module==hw_modules[z]) { forced_failure=1; for(uint32_t h=0;h<n;h++)p[h]=VK_NULL_HANDLE;
    fprintf(stderr,"INJECTED_HARDWARE_GRAPHICS_FAILURE\n"); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
 return real(d,c,n,i,a,p);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True, help='Quake directory or id1 directory containing pak0.pak')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--inject-hardware-failure', action='store_true')
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 640, 360, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-backend-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir(); (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = '\n'.join(line for line in bench.BOOT.splitlines()
                     if not line.startswith(('vid_fullscreen', 'vid_vsync', 'vid_fsaa')))
    boot = boot.replace('map start', 'r_rtshadows 0\nr_radiance_backend 1\nr_radiance 1\nmax_edicts 32000\nmap e1m1')
    (work / 'id1/boot.cfg').write_text(boot + '\n')
    if args.inject_hardware_failure:
        bench.SHIM += INJECT
    engine = bench.Engine(args, work)
    try:
        try:
            os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
        except (AttributeError, OSError):
            pass
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        def state():
            text = engine.cmd('radiance_stats')
            match = STATE.search(text)
            if not match:
                raise RuntimeError(f'radiance not active: {text}')
            return match.group(1)
        if state() != 'software':
            raise RuntimeError('forced software backend not active')
        engine.cmd('r_radiance_backend 2')
        selected = state()
        output = engine.text() + (work / 'output.log').read_text(errors='replace')
        if args.inject_hardware_failure and 'Using ray queries' in output:
            if selected != 'software' or output.count('INJECTED_HARDWARE_GRAPHICS_FAILURE') != 1:
                raise RuntimeError('hardware pipeline injection did not fall back exactly once')
        elif not args.inject_hardware_failure and 'Using ray queries' in output and selected != 'ray-query':
            raise RuntimeError('ray-query device did not use its hardware backend')
        elif selected != 'software' and selected != 'ray-query':
            raise RuntimeError(f'unexpected backend: {selected}')
        print(f'First hardware request selected {selected}', flush=True)
        engine.cmd('r_radiance_backend 1')
        if state() != 'software':
            raise RuntimeError('switch back to software failed')
        engine.cmd('r_radiance_backend 2')
        if state() != selected:
            raise RuntimeError('repeated hardware request changed backend')
        output = engine.text() + (work / 'output.log').read_text(errors='replace')
        if args.inject_hardware_failure and output.count('INJECTED_HARDWARE_GRAPHICS_FAILURE') > 1:
            raise RuntimeError('hardware graphics failure was retried')
        if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
            re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
            raise RuntimeError(f'validation unavailable or reported an error; inspect {work}')
        print('PASS: backend switches and fallback', flush=True)
    finally:
        engine.close()


if __name__ == '__main__':
    main()
