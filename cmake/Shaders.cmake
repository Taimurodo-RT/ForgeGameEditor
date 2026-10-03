# forge_add_shaders(<target> <shader>...)
# Compiles GLSL shaders (stage from the extension: .vert / .frag) into
# generated headers <name>.<stage>.h, included as "shaders/<name>_<stage>.h",
# and makes <target> depend on them.
function(forge_add_shaders target)
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
  file(MAKE_DIRECTORY ${out_dir})
  set(headers)
  foreach(src ${ARGN})
    get_filename_component(abs ${src} ABSOLUTE)
    get_filename_component(base ${src} NAME_WE)
    get_filename_component(ext ${src} EXT)
    string(SUBSTRING ${ext} 1 -1 stage)
    set(name ${base}_${stage})
    set(header ${out_dir}/${name}.h)
    add_custom_command(
      OUTPUT ${header}
      COMMAND forge_shaderc ${stage} ${abs} ${header} ${name}
      DEPENDS ${abs} forge_shaderc
      COMMENT "Compiling shader ${base}.${stage}"
      VERBATIM)
    list(APPEND headers ${header})
  endforeach()
  target_sources(${target} PRIVATE ${headers})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
endfunction()
