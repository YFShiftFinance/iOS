set(LLVM_ENABLE_PROJECTS
      clang
      clang-tools-extra
      lld
      lldb
    CACHE STRING "")

set(LLVM_EXTERNAL_PROJECTS
      cmark
      swift
    CACHE STRING "")

# NOTE(compnerd) always enable assertions, the toolchain will not provide enough
# context to resolve issues otherwise and may silently generate invalid output.
set(LLVM_ENABLE_ASSERTIONS YES CACHE BOOL "")

set(ENABLE_X86_RELAX_RELOCATIONS YES CACHE BOOL "")
set(LLVM_ENABLE_PYTHON YES CACHE BOOL "")
set(LLVM_TARGETS_TO_BUILD AArch64 ARM WebAssembly X86 CACHE STRING "")

# Disable certain targets to reduce the configure time or to avoid configuration
# differences (and in some cases weird build errors on a complete build).
set(LLVM_APPEND_VC_REV NO CACHE BOOL "")
set(LLVM_BUILD_LLVM_DYLIB NO CACHE BOOL "")
set(LLVM_BUILD_LLVM_C_DYLIB NO CACHE BOOL "")
set(LLVM_ENABLE_LIBEDIT NO CACHE BOOL "")
set(LLVM_ENABLE_LIBXML2 NO CACHE BOOL "")
set(LLVM_ENABLE_OCAMLDOC NO CACHE BOOL "")
set(LLVM_ENABLE_ZLIB NO CACHE BOOL "")
set(LLVM_INCLUDE_BENCHMARKS NO CACHE BOOL "")
set(LLVM_INCLUDE_DOCS NO CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES NO CACHE BOOL "")
set(LLVM_INCLUDE_GO_TESTS NO CACHE BOOL "")
set(LLVM_TOOL_GOLD_BUILD NO CACHE BOOL "")
set(LLVM_TOOL_LLVM_SHLIB_BUILD NO CACHE BOOL "")

# Avoid swig dependency for lldb
set(LLDB_ALLOW_STATIC_BINDINGS YES CACHE BOOL "")
set(LLDB_USE_STATIC_BINDINGS YES CACHE BOOL "")

# This requires perl which may not be available on Windows
set(SWIFT_INCLUDE_DOCS NO CACHE BOOL "")
set(SWIFT_BUILD_STATIC_STDLIB NO CACHE BOOL "")
set(SWIFT_BUILD_STATIC_SDK_OVERLAY NO CACHE BOOL "")

set(LLVM_INSTALL_BINUTILS_SYMLINKS YES CACHE BOOL "")
set(LLVM_INSTALL_TOOLCHAIN_ONLY YES CACHE BOOL "")
set(LLVM_TOOLCHAIN_TOOLS
      addr2line
      ar
      c++filt
      dsymutil
      dwp
      # lipo
      llvm-ar
      llvm-cov
      llvm-cvtres
      llvm-cxxfilt
      llvm-dlltool
      llvm-dwarfdump
      llvm-dwp
      llvm-lib
      llvm-lipo
      llvm-mt
      llvm-mt
      llvm-nm
      llvm-objcopy
      llvm-objdump
      llvm-pdbutil
      llvm-profdata
      llvm-ranlib
      llvm-rc
      llvm-readelf
      llvm-readobj
      llvm-size
      llvm-strings
      llvm-strip
      llvm-symbolizer
      llvm-undname
      nm
      objcopy
      objdump
      ranlib
      readelf
      size
      strings
    CACHE STRING "")

set(CLANG_TOOLS
      clang
      clangd
      clang-format
      clang-resource-headers
      clang-tidy
    CACHE STRING "")

set(LLD_TOOLS
      lld
    CACHE STRING "")

set(LLDB_TOOLS
      liblldb
      lldb
      lldb-argdumper
      lldb-python-scripts
      lldb-server
      lldb-vscode
      repl_swift
    CACHE STRING "")

set(SWIFT_INSTALL_COMPONENTS
      autolink-driver
      compiler
      clang-builtin-headers
      editor-integration
      tools
      sourcekit-inproc
      swift-remote-mirror
      swift-remote-mirror-headers
    CACHE STRING "")

set(LLVM_DISTRIBUTION_COMPONENTS
      IndexStore
      libclang
      libclang-headers
      LTO
      ${LLVM_TOOLCHAIN_TOOLS}
      ${CLANG_TOOLS}
      ${LLD_TOOLS}
      ${LLDB_TOOLS}
      ${SWIFT_INSTALL_COMPONENTS}
    CACHE STRING "")
