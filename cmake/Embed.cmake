# forge_embed_text(<target> <file> <symbol>)
# Builds a text file into <target> as `extern const unsigned char <symbol>[]` (with a
# terminating zero) and `extern const unsigned long long <symbol>_size`, so
# data the engine always needs (the standard node library) cannot go missing.
# Rebuilt whenever the file changes.
function(forge_embed_text target file symbol)
  get_filename_component(abs ${file} ABSOLUTE)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/embed_${symbol}.cpp)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${abs} -DOUT=${out} -DSYMBOL=${symbol} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${abs} ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding ${file}"
    VERBATIM)
  target_sources(${target} PRIVATE ${out})
endfunction()
