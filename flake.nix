{
  description = "Venera: Direct3D, Vulkan and OpenGL for Windows guests on QEMU + Venus";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      mingw = pkgs.pkgsCross.mingwW64;
      mcfg = mingw.windows.mcfgthreads;
      # vkd3d-proton generates its D3D12 headers from IDL with widl. nixpkgs ships widl only with Wine: expose just
      # that binary, so wine itself stays off PATH.
      widl = pkgs.runCommand "widl" { } ''
        mkdir -p $out/bin
        ln -s ${pkgs.wine64Packages.minimal}/bin/widl $out/bin/widl
      '';
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake ninja pkg-config
          unzip osslsigncode gcab
          # Mesa (icd/build.sh): meson and its Python code generators.
          meson bison flex
          (python3.withPackages (p: [ p.mako p.pyyaml p.packaging ]))
          # vkd3d-proton (third_party/vkd3d-proton): IDL headers and its internal GLSL shaders.
          widl glslang
          mingw.buildPackages.gcc
          mingw.buildPackages.binutils
        ];

        # Read by cmake/mingw-w64.cmake: GCC from nixpkgs uses the mcfgthread model,
        # whose headers and static library live outside the wrapper's default paths.
        VENERA_MINGW_MCFG_INCLUDE = "${mcfg.dev}/include";
        VENERA_MINGW_MCFG_LIB = "${mcfg}/lib";

        # Read by cmake/clang-cl-kmd.cmake. Not on PATH: unwrapped clang would shadow the host cc/cpp.
        VENERA_CLANG_CL = "${pkgs.llvmPackages.clang-unwrapped}/bin/clang-cl";
        VENERA_LLD_LINK = "${pkgs.lld}/bin/lld-link";
        VENERA_LLVM_RC = "${pkgs.llvmPackages.llvm}/bin/llvm-rc";
        VENERA_LLVM_LIB = "${pkgs.llvmPackages.llvm}/bin/llvm-lib";
      };
    };
}
