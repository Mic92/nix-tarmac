project('nix-tarmac', 'cpp',
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'optimization=3',
    'debug=true',
    'warning_level=2',
    'unity=on',
    'unity_size=16',
  ])

add_project_arguments(
  '-fvisibility=hidden',
  '-fvisibility-inlines-hidden',
  '-D_GNU_SOURCE',
  '-D_GLIBCXX_ASSERTIONS',
  '-fstack-protector-strong',
  '-Wshadow',
  '-Wnon-virtual-dtor',
  language: 'cpp')

lmdb = dependency('lmdb')
blake3 = dependency('libblake3')
archive = dependency('libarchive')
ssl = dependency('openssl')

core_deps = [lmdb, blake3, archive, ssl]
core = static_library('core', 'src/pack_cas.cpp', 'src/tree.cpp',
  dependencies: core_deps)
core_dep = declare_dependency(link_with: core, include_directories: 'src',
  dependencies: core_deps)

executable('bench', 'bench/bench.cpp', dependencies: core_dep)

foreach t : ['model_test', 'corrupt_test', 'nar_test', 'crash_test', 'gc_test']
  test(t, executable(t, 'tests' / t + '.cpp', dependencies: core_dep),
    timeout: 300)
endforeach

if get_option('fuzzer').enabled()
  fuzz_args = ['-fsanitize=fuzzer']
  executable('fuzz_ingest', 'fuzz/fuzz_ingest.cc',
    cpp_args: fuzz_args,
    link_args: fuzz_args,
    dependencies: core_dep)
endif

nix_fetchers = dependency('nix-fetchers', required: get_option('plugin'))
nix_store = dependency('nix-store', required: get_option('plugin'))
nix_util = dependency('nix-util', required: get_option('plugin'))
if nix_fetchers.found() and nix_store.found() and nix_util.found()
  nix_ver = nix_fetchers.version().split('.')
  majmin = nix_ver[0] + '.' + nix_ver[1].split('pre')[0]
  compat_args = [
    '-DNIX_COMPAT_VERSION_MAJOR=' + nix_ver[0],
    '-DNIX_COMPAT_VERSION_MINOR=' + nix_ver[1].split('pre')[0],
  ]
  module_suffix = host_machine.system() == 'darwin' ? 'dylib' : 'so'

  shared_module('nix-tarmac', 'plugin/tarmac.cc',
    include_directories: 'src',
    link_with: core,
    cpp_args: compat_args,
    dependencies: [nix_fetchers, nix_store, nix_util] + core_deps,
    name_prefix: '',
    name_suffix: module_suffix,
    install: true,
    install_dir: get_option('libdir') / 'nix' / 'nix-tarmac-versions' / majmin)

  shared_module('nix-tarmac-loader', 'plugin/loader.cc',
    cpp_args: ['-DTARMAC_MODULE_SUFFIX="' + module_suffix + '"'],
    name_prefix: '',
    name_suffix: module_suffix,
    install: true,
    install_dir: get_option('libdir') / 'nix' / 'plugins')
endif
