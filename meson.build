project('dvr', 'c', 'cpp',
  version : '0.1.0',
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
    'b_vscrt=static_from_buildtype',
  ],
)

project_name = meson.project_name()
static_link_libs = true
exe_name = project_name

add_project_arguments('-DPROJECT_NAME="' + project_name + '"', language : ['c', 'cpp'])
add_project_arguments('-DPROJECT_VERSION="' + meson.project_version() + '"', language : ['c', 'cpp'])
git_commit = run_command('git', 'rev-parse', 'HEAD', check : false)
add_project_arguments('-DPROJECT_COMMIT_HASH="' + git_commit.stdout().strip() + '"', language : ['c', 'cpp'])

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

if cc.has_argument('-std=c23')
  add_project_arguments('-std=c23', language : 'c')
else
  add_project_arguments('-std=c2x', language : 'c')
endif

# set the C standard to C23 or C2x if C23 is not available

# platform specific settings
platform_deps = []
if host_machine.system() == 'windows'
  add_project_link_arguments('/SUBSYSTEM:WINDOWS', language : ['c', 'cpp'])
  add_project_arguments('-D_CRT_SECURE_NO_WARNINGS', language : ['c', 'cpp'])
  add_project_arguments('-DWIN32_LEAN_AND_MEAN', language : ['c', 'cpp'])
  add_project_arguments('-DJPH_PROFILE_ENABLED', language : ['c', 'cpp'])
  add_project_arguments('-DJPH_DEBUG_RENDERER', language : ['c', 'cpp'])
  if get_option('buildtype') != 'release'
    add_project_arguments('-DJPH_FLOATING_POINT_EXCEPTIONS_ENABLED', language : ['c', 'cpp'])
  endif
elif host_machine.system() == 'linux'
  add_project_arguments('-D_POSIX_C_SOURCE=200809L', language : 'c')

  platform_deps += cc.find_library('m', required : true)
  platform_deps += cc.find_library('pthread', required : true)
  platform_deps += cc.find_library('Xi', required : true)
  platform_deps += cc.find_library('Xcursor', required : true)
  platform_deps += dependency('X11', required : true)
  platform_deps += dependency('GL', required : true)
  platform_deps += dependency('dl', required : true)
elif host_machine.system() == 'emscripten-webgl' or host_machine.system() == 'emscripten-webgpu'
  exe_name += '.html'
  add_project_link_arguments(
    '--shell-file',
    join_paths(
      meson.current_source_dir(), 'web', 'shell.html'
    ),
    language : 'c'
  )
endif

# compiler specific settings
if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
  if get_option('buildtype') != 'release'
    add_project_arguments('-g', language : 'c')
  endif
  add_project_arguments('-Wconversion', '-Werror', language : ['c'])
endif

if cxx.get_id() == 'clang++' or cxx.get_id() == 'g++'
  # add_project_link_arguments('-fno-rtti', '-fno-exceptions', language : 'cpp')
endif

# buildtype specific settings
buildtype_files = []
buildtype_deps = []
if get_option('buildtype') == 'release'
  add_project_arguments('-DRELEASE', language : 'c')
else
  buildtype_files = [
    'src/debug.c',
  ]
  buildtype_deps = [
    dependency('cimgui', required : true, static : static_link_libs),
  ]
endif

# shaders
shaders = [
  'default_vs',
  'default_fs',
]

shader_targets = []

shader_dir = join_paths(meson.current_source_dir(), 'shaders')

glslc = find_program('glslc', required : true)

foreach shader : shaders
  shader_file = join_paths(shader_dir, shader + '.glsl')
  shader_spirv = shader + '.spv'
  shader_targets += custom_target(
    shader_spirv,
    command : [glslc, shader_file, '-o', shader_spirv],
    output : shader_spirv,
    input : shader_file,
  )
endforeach

cmake = import('cmake')
cmake_opt = cmake.subproject_options()

# jolt
# the jolt subproject is a cmake project, so we need to use the cmake module
# the jolt CMakeLists.txt file is in the subdirectory of the jolt subproject,
# in Build/CMakeLists.txt. So we need to set the subproject directory to
# Build, and the CMakeLists.txt file to CMakeLists.txt

cmake = import('cmake')
cmake_opt = cmake.subproject_options()
# supress warnings from jolt
cmake_opt.set_override_option('warning_level', '0')

full_c_src = []
full_cpp_src = []

if host_machine.system() == 'windows'
  full_c_src = run_command('powershell', '-Command', 'Get-ChildItem -Path src\\*.c -Recurse | Resolve-Path -Relative', check: true).stdout().strip().split('\n')
  full_cpp_src = run_command('powershell', '-Command', 'Get-ChildItem -Path src\\*.cpp -Recurse | Resolve-Path -Relative', check: true).stdout().strip().split('\n')

  exclude_files = [
    '.\\src\\debug.c',
  ]
else
  globber = run_command('sh', '-c', 'find src -name "*.c" | sort', check: true)
  full_c_src = globber.stdout().strip().split('\n')

  globber = run_command('sh', '-c', 'find src -name "*.cpp" | sort', check: true)
  full_cpp_src = globber.stdout().strip().split('\n')

  exclude_files = [
    'src/debug.c',
  ]
endif

if full_c_src.length() == 1 and full_c_src[0] == ''
  full_c_src = []
endif
if full_cpp_src.length() == 1 and full_cpp_src[0] == ''
  full_cpp_src = []
endif

basic_src = []

foreach file : full_c_src
  if not exclude_files.contains(file)
    basic_src += file
  endif
endforeach

foreach file : full_cpp_src
  if not exclude_files.contains(file)
    basic_src += file
  endif
endforeach

# final executable
src = [
  basic_src,
  buildtype_files,
  shader_targets,
]

inc = include_directories('include')
sys_inc = include_directories('libs', is_system : true)

assimp_subproject = cmake.subproject('assimp', options : cmake_opt)
assimp = assimp_subproject.dependency('assimp')

vulkan_deps = [
  dependency('vulkan', required : true),
]

deps = [
  subproject('cglm', default_options : ['default_library=static']).get_variable('cglm_dep'),
  dependency('glfw3', required : true, static : static_link_libs),
  assimp,
  vulkan_deps,
  platform_deps,
  buildtype_deps,
]

executable(
  exe_name,
  src,
  include_directories : [inc, sys_inc],
  dependencies : deps,
  install : true
)
