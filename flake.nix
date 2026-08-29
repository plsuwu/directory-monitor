{
  inputs = {
    nixpkgs.url = "nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShell = pkgs.mkShell {
          # NOTE for myself in the future because I always get stuck on this every time I write C:
          #
          # Some headers aren't found by `clangd` as it bypasses `NIX_CFLAGS_COMPILE` (regardless of
          # `compile_commands.json`, which doesn't capture environment-injected variables). Setting 
          # `clang` & `clang-tools` in `devShell.packages` lets us share `NIX_CFLAGS_COMPILE` with 
          # the Neovim-initialized `clangd` (or something along these lines).
          packages = [
            pkgs.clang-tools
            pkgs.clang
          ];

          buildInputs = with pkgs; [
            clang
            bear
          ];
        };
      }
    );
}

