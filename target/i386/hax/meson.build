i386_system_ss.add(when: 'CONFIG_HAX', if_true: files(
  'hax-all.c',
  'hax-mem.c',
  'hax-accel-ops.c',
))
if host_os == 'windows'
  i386_system_ss.add(when: 'CONFIG_HAX', if_true: files('hax-windows.c'))
else
  i386_system_ss.add(when: 'CONFIG_HAX', if_true: files('hax-posix.c'))
endif
