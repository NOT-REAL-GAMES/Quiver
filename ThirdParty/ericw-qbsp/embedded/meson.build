# Each legacy tool needs its own namespace and copy of common code. QBSP
# uses double vectors; VIS/LIGHT use float vectors.
maptool_root = meson.project_source_root() / 'ThirdParty/ericw-qbsp'
maptool_common = ['threads', 'cmdlib', 'log', 'qvec', 'mathlib', 'polylib']
maptool_qbsp = ['brush', 'bspfile', 'csg4', 'file', 'globals', 'map', 'merge', 'outside', 'parser', 'portals', 'qbsp', 'solidbsp', 'surfaces', 'tjunc', 'util', 'wad', 'winding', 'writebsp', 'exportobj']
maptool_vis = ['flow', 'soundpvs', 'state', 'vis']
maptool_light = ['entities', 'litfile', 'ltface', 'trace', 'light', 'phong', 'bounce', 'settings']
maptool_wrappers = []
foreach tool : ['qbsp', 'vis', 'light']
    sources = []
    foreach item : maptool_common
        sources += 'common/' + item
    endforeach
    if tool != 'qbsp'
        sources += ['common/bsputils', 'common/bspfile']
    endif
    if tool == 'light'
        sources += 'common/mesh'
        tool_sources = maptool_light
    elif tool == 'vis'
        tool_sources = maptool_vis
    else
        tool_sources = maptool_qbsp
    endif
    foreach item : tool_sources
        sources += tool + '/' + item
    endforeach
    foreach item : sources
        cfg = configuration_data()
        cfg.set('PREINCLUDE', (maptool_root / 'embedded/preinclude.hh').replace('\\', '/'))
        cfg.set('SOURCE', (maptool_root / (item + '.cc')).replace('\\', '/'))
        cfg.set('NAMESPACE', 'quiver_' + tool)
        cfg.set('DEFINES', tool == 'qbsp' ? '#define DOUBLEVEC_T' : '')
        maptool_wrappers += configure_file(input : 'wrapper.cc.in',
            output : tool + '_' + item.replace('/', '_') + '.cc', configuration : cfg)
    endforeach
endforeach
maptool_args = ['-DQUIVER_EMBEDDED_MAPTOOLS', '-DERICWTOOLS_VERSION=v0.18.1']
if host_machine.system() == 'windows'
    maptool_args += ['-DUSE_WIN32THREADS', '-DWIN32', '-D_CRT_SECURE_NO_WARNINGS']
    if meson.get_compiler('cpp').get_id() == 'gcc'
        maptool_args += '-D__USE_MINGW_ANSI_STDIO=1'
    endif
else
    maptool_args += ['-DUSE_PTHREADS', '-DLINUX']
endif
maptool_lib = static_library('quiver_embedded_maptools', [maptool_wrappers, 'bridge.cc'],
    include_directories : include_directories('../include'),
    cpp_args : maptool_args,
    dependencies : dependency('threads'),
    override_options : ['cpp_std=c++11', 'warning_level=0', 'werror=false'])
