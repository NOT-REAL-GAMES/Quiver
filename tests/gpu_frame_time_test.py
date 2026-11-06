#!/usr/bin/env python3
"""Exercise the actual gpu_frame_time command's mean and median reporting."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def extract(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for index in range(brace, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if not depth:
            return source[start:index + 1]
    raise ValueError(f'unterminated function {signature}')


root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/gl_vidsdl.c').read_text()
functions = '\n'.join(extract(source, signature) for signature in
                      ('static int VID_CompareGPUFrameTimes', 'static void VID_GPUFrameTime_f', 'static void VID_GPUStageTime_f'))
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DOUBLE_BUFFERED 2
#define GPU_MEASURE_HISTORY_CAPACITY 65536u
#define q_min(a,b) ((a)<(b)?(a):(b))
#define countof(a) (sizeof(a)/sizeof((a)[0]))
static bool gpu_measure_active, gpu_measure_written[DOUBLE_BUFFERED];
static double gpu_measure_ms;
static unsigned int gpu_measure_samples;
static float gpu_measure_history[GPU_MEASURE_HISTORY_CAPACITY];
static bool gpu_stage_active, gpu_stage_written[DOUBLE_BUFFERED];
static double gpu_stage_ms[2];
static unsigned int gpu_stage_samples;
static float gpu_stage_history[2][GPU_MEASURE_HISTORY_CAPACITY];
static double gpu_receiver_ms[3];
static unsigned int gpu_receiver_samples;
static float gpu_receiver_history[3][GPU_MEASURE_HISTORY_CAPACITY];
static const char *command = "stop";
static char output[2048];
static int Cmd_Argc(void) { return 2; }
static const char *Cmd_Argv(int n) { return n == 1 ? command : "gpu_frame_time"; }
static void GL_SynchronizeEndRenderingTask(void) {}
static void Con_Printf(const char *format, ...) {
    size_t at = strlen(output); va_list args; va_start(args, format);
    vsnprintf(output + at, sizeof(output) - at, format, args); va_end(args);
}
'''+functions+r'''
int main(void) {
    gpu_measure_samples=4; gpu_measure_ms=10;
    gpu_measure_history[0]=4; gpu_measure_history[1]=1;
    gpu_measure_history[2]=3; gpu_measure_history[3]=2;
    VID_GPUFrameTime_f();
    assert(strstr(output, "4 samples, 2.5000 ms mean"));
    assert(strstr(output, "GPU frame median: 2.5000 ms (4/4 samples)"));
    assert(strstr(output, "GPU frame p95: 4.0000 ms (4/4 samples)"));
    output[0]=0; command="start"; VID_GPUFrameTime_f();
    assert(gpu_measure_active && !gpu_measure_samples && gpu_measure_ms == 0);
    command="stop"; output[0]=0; VID_GPUFrameTime_f();
    assert(strstr(output, "no completed samples"));
    output[0]=0; gpu_measure_samples=GPU_MEASURE_HISTORY_CAPACITY+4;
    for(unsigned i=0;i<GPU_MEASURE_HISTORY_CAPACITY;++i) gpu_measure_history[i]=(float)i;
    VID_GPUFrameTime_f();
    assert(strstr(output, "GPU frame median: 32767.5000 ms (65536/65540 samples)"));
    output[0]=0; gpu_stage_samples=4; gpu_stage_ms[0]=10; gpu_stage_ms[1]=4;
    for(unsigned i=0;i<4;++i) { gpu_stage_history[0][i]=(float)(4-i); gpu_stage_history[1][i]=1; }
    gpu_receiver_samples=4;
    for(unsigned j=0;j<3;++j) { gpu_receiver_ms[j]=10; for(unsigned i=0;i<4;++i)gpu_receiver_history[j][i]=(float)(i+1); }
    VID_GPUStageTime_f();
    assert(strstr(output, "GPU receiver_filter stage: 4 samples, 2.5000 ms mean, 2.5000 ms median, 4.0000 ms p95"));
    assert(strstr(output, "GPU world stage: 4 samples, 2.5000 ms mean, 2.5000 ms median"));
    assert(strstr(output, "GPU entities stage: 4 samples, 1.0000 ms mean, 1.0000 ms median"));
    output[0]=0; command="start"; VID_GPUStageTime_f();
    assert(gpu_stage_active && !gpu_stage_samples && gpu_stage_ms[0] == 0 && gpu_stage_ms[1] == 0 && !gpu_receiver_samples && gpu_receiver_ms[2] == 0);
    output[0]=0; command="stop"; VID_GPUStageTime_f();
    assert(strstr(output, "GPU stage time: no completed samples"));
    puts("PASS: GPU frame timer mean, median and truncation reporting");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-frame-time-test-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
