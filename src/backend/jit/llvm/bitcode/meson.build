# Copyright (c) 2022-2026, PostgreSQL Global Development Group
#
# emit LLVM bitcode for JIT inlining

assert(llvm.found())

foreach bitcode_module : bitcode_modules
  bitcode_targets = []
  bitcode_obj = bitcode_module['target']
  bitcode_cflags_local = bitcode_cflags + bitcode_module.get('additional_flags', [])
  bitcode_name = bitcode_module.get('name', bitcode_obj.name())

  foreach srcfile : bitcode_module['srcfiles']
    if meson.version().version_compare('>=0.59')
      srcfilename = fs.parent(srcfile) / fs.name(srcfile)
    else
      srcfilename = '@0@'.format(srcfile)
    endif

    targetname = '@0@_@1@.bc'.format(
      bitcode_name,
      srcfilename.underscorify(),
    )
    bitcode_targets += custom_target(
      targetname,
      # Depend on the bitcode object file's extracted object. Referencing the
      # object file pulls in its dependencies (e.g. generated headers) without
      # slowing down the build.
      input: [srcfile, bitcode_obj.extract_objects(srcfile)],
      output: targetname,
      command: [llvm_irgen_command, llvm_irgen_dep_args, bitcode_cflags_local],
      depfile: targetname + '.d',
      install: true,
      install_dir: dir_bitcode,
    )
  endforeach

  index_name = '@0@.index.bc'.format(bitcode_name)
  bitcode_index = custom_target('@0@'.format(bitcode_name),
    output: index_name,
    input: bitcode_targets,
    command: [irlink, '--lto', llvm_lto, '--outdir', '@OUTDIR@', '--index', index_name, '@INPUT@'],
    install: true,
    install_dir: dir_bitcode,
  )
  backend_targets += bitcode_index
endforeach
