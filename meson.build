project('NetBuild', 'cpp',
  version : '0.1',
  default_options : ['cpp_std=c++17'])

sdl3_dep = dependency('sdl3')
sdl3_net_dep = dependency('sdl3-net')
json_dep = dependency('nlohmann_json')

inc = include_directories('src/include')

common_src = ['src/protocol.cpp']

executable('worker',
  ['src/worker.cpp'] + common_src,
  dependencies : [sdl3_dep, sdl3_net_dep, json_dep],
  include_directories : inc)

executable('coordinator',
  ['src/coordinator.cpp'] + common_src,
  dependencies : [sdl3_dep, sdl3_net_dep, json_dep],
  include_directories : inc)
