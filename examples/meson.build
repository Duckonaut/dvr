# shaders
shaders = [
  'default_vs',
  'default_fs',
  'rt_vs',
  'rt_fs',
  'particle_update_cs',
  'diffuse_cs',
]

shader_targets = []

glslc = find_program('glslc', required : true)

foreach shader : shaders
  shader_file = join_paths('shaders', shader + '.glsl')
  shader_spirv = shader + '.spv'
  shader_targets += custom_target(
    shader_spirv,
    command : [glslc, '@INPUT@', '-o', '@OUTPUT@'],
    output : shader_spirv,
    input : shader_file,
    build_by_default : true,
    install : false,
  )
endforeach

# assets
# just copy the assets to the build directory one by one
assets = [
  'viking_room.obj',
  'viking_room.png',
]

example_assets = []

foreach asset : assets
  asset_file = join_paths('assets', asset)
  asset_target = asset
  example_assets += custom_target(
    asset_target,
    command : ['cp', '@INPUT@', '@OUTPUT@'],
    output : asset,
    input : asset_file,
    build_by_default : true,
    install : false,
  )
endforeach

cmake = import('cmake')
cmake_opt = cmake.subproject_options()

cmake = import('cmake')
cmake_opt = cmake.subproject_options()
# supress warnings from subprojects
cmake_opt.set_override_option('warning_level', '0')

assimp_subproject = cmake.subproject('assimp', options : cmake_opt)
assimp = assimp_subproject.dependency('assimp')

cglm = subproject('cglm', default_options : ['default_library=static']).get_variable('cglm_dep')

examples = [
  'model',
  'compute',
]

example_deps = {
  'model' : [
    assimp,
    cglm,
  ],
  'compute' : [
    cglm,
  ],
}

example_targets = []

foreach example : examples
  example_sources = [
    join_paths(example, 'main.c'),
  ]

  example_target = executable(example, example_sources, dependencies : [ example_deps[example], dvr_dep ] )
  example_targets += example_target
endforeach
