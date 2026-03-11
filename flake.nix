{
  description = "nix-dynld is a meta-loader for ELF binaries, adapted from Antithesis Operations LLC's Madness project.";

  outputs = _: let
    module = import ./modules;
  in {
    nixosModules.default = module;
    nixosModules.dynld = module;
  };
}
