project('cimgui', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++11',
    'warning_level=0',
    'debug=true',
    'prefer_static=true',
  ],
)

cc = meson.get_compiler('c')

static_link_libs = false
add_project_arguments('-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=1', language : 'cpp')
imgui_backends = [
  'imgui/backends/imgui_impl_glfw.cpp',
  'imgui/backends/imgui_impl_vulkan.cpp',
]
backend_deps = []
if host_machine.system() == 'windows'
  add_project_arguments('-DIMGUI_IMPL_API=extern"C" __declspec(dllexport)', language : 'cpp')
  static_link_libs = true
else
  add_project_arguments('-DIMGUI_IMPL_API=', language : 'cpp')

  if host_machine.system() == 'darwin'
  else
  endif
endif
cimgui_deps = [
  backend_deps,
]

tables_src = []
fs = import('fs')
if fs.exists('imgui/imgui_tables.cpp')
  tables_src = ['imgui/imgui_tables.cpp']
endif

src = [
    'cimgui.cpp',
    'imgui/imgui.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_demo.cpp',
    'imgui/imgui_widgets.cpp',
    imgui_backends,
    tables_src
]

cimgui_inc = [
  include_directories('.'),
  include_directories('imgui'),
]

cimgui = library(meson.project_name(), src,
  include_directories: [cimgui_inc],
  dependencies: cimgui_deps,
  install: true,
)
cimgui_dep = declare_dependency(
  include_directories : [cimgui_inc],
  link_with: cimgui,
  dependencies : cimgui_deps
)

