project('dvr', 'c', 'cpp',
  version : '0.1.0',
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
    'debug=true',
    'b_vscrt=static_from_buildtype',
  ],
)

project_name = meson.project_name()
static_link_libs = true

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
elif host_machine.system() == 'linux'
  add_project_arguments('-D_POSIX_C_SOURCE=200809L', language : 'c')

  platform_deps += cc.find_library('m', required : true)
endif

# compiler specific settings
if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
  add_project_arguments('-Wconversion', '-Werror', language : ['c'])
endif

# buildtype specific settings
buildtype_deps = []
if get_option('buildtype') == 'release'
  add_project_arguments('-DRELEASE', language : 'c')
endif

if get_option('imgui')
  buildtype_deps = [
    subproject('cimgui', default_options : ['default_library=static']).get_variable('cimgui_dep'),
  ]
  add_project_arguments('-DDVR_ENABLE_IMGUI', language : 'c')
endif

basic_src = [
  'src/dvr.c',
  'src/header_impl.c',
  'src/log.c',
  'src/utils.c',
]

# final library
src = [
  basic_src,
]

inc = include_directories('include')
sys_inc = include_directories('libs', is_system : true)

vulkan_deps = [
  dependency('vulkan', required : true, static : static_link_libs),
]

deps = [
  dependency('glfw3', required : true, static : static_link_libs),
  vulkan_deps,
  platform_deps,
  buildtype_deps,
]

dvr = library(
  project_name,
  src,
  include_directories : [inc, sys_inc],
  dependencies : deps,
)

dvr_dep = declare_dependency(link_with : dvr, include_directories : [inc, sys_inc], dependencies : deps)

# examples
subdir('examples')
