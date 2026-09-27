if host_os == 'windows'
  d3d11 = cc.find_library('d3d11', required: true)
  d3d11_dxgi = cc.find_library('dxgi', required: true)
  d3dcompiler = cc.find_library('d3dcompiler', required: true)
  specific_ss.add(d3d11, d3d11_dxgi, d3dcompiler,
                  files('blits.c', 'draw.c', 'presentation.c', 'renderer.c',
                        'reports.c', 'shaders.c',
                        'surfaces.c', 'textures.c'))
endif
