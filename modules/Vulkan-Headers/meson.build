prog_git = find_program('git')
if is_unixy or host_machine.system() == 'mingw'
prog_sh = find_program('sh')
script_clone_hdrs = files('clone_headers.sh')
else
prog_sh = files('clone_headers.bat')
script_clone_hdrs = ''
endif

vk_api_xml = custom_target(
  'vk_api_xml',
  input : [],
  output : ['vk.xml'], # because output can't have segments, link vk.xml to this subdir dir in build prefix
  command : [
    prog_sh, script_clone_hdrs, prog_git, meson.current_build_dir()
  ],
)
