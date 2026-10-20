{
  description = "vgd3d12: a D3D12 user-mode driver for Windows guests on QEMU + Venus";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      mingw = pkgs.pkgsCross.mingwW64;
      mcfg = mingw.windows.mcfgthreads;
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake ninja pkg-config python3 gdb
          vulkan-loader vulkan-tools vulkan-validation-layers spirv-tools
          unzip osslsigncode
          mingw.buildPackages.gcc
          mingw.buildPackages.binutils
        ];

        # Read by cmake/mingw-w64.cmake: GCC from nixpkgs uses the mcfgthread model,
        # whose headers and static library live outside the wrapper's default paths.
        VGD_MINGW_MCFG_INCLUDE = "${mcfg.dev}/include";
        VGD_MINGW_MCFG_LIB = "${mcfg}/lib";

        # The Vulkan loader finds llvmpipe/RADV through the system driver path.
        LD_LIBRARY_PATH = "${pkgs.vulkan-loader}/lib";
      };
    };
}
