if host_os == 'windows'
  d3d12 = cc.find_library('d3d12', required: true)
  d3d12_dxgi = cc.find_library('dxgi', required: true)
  d3d12_dxguid = cc.find_library('dxguid', required: true)
  d3d12_mesa_headers = declare_dependency(include_directories: include_directories(
    '../../../../../thirdparty/mesa/src/microsoft/compiler',
    '../../../../../thirdparty/mesa/src/microsoft/spirv_to_dxil',
  ))
  specific_ss.add(d3d12, d3d12_dxgi, d3d12_dxguid, d3d12_mesa_headers,
    libglslang, spirv_reflect,
    files('blits.c', 'draw.c', 'renderer.c', 'reports.c', 'shaders.c', 'surfaces.c',
          'textures.c'))
endif
