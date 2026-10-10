{
  description = "Minimal autonomous Artix-7 UART clock-configuration reproducer";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  outputs = { self, nixpkgs }: let
    pkgs = import nixpkgs { system = "x86_64-linux"; };
  in {
    devShells.x86_64-linux.default = pkgs.mkShell {
      packages = with pkgs; [ gcc cmake gnumake pkg-config boost eigen zlib
        git yosys iverilog python313 ];
    };
  };
}
