# nix-dynld

nix-dynld is a meta-loader for ELF binaries on NixOS. Its goal is to let one binary work on both conventional Linux systems and on NixOS, if the binary uses the conventional loader path `/lib64/ld-linux-x86-64.so.2`.

nix-dynld is inspired by and based on the [Madness](https://antithesis.com/blog/madness/) [project](https://github.com/antithesishq/madness) originally developed by Antithesis Operations LLC.

## Motivation (Why this exists)

On most Linux systems, dynamically linked executables use a conventional ELF interpreter path such as `/lib64/ld-linux-x86-64.so.2`. But NixOS is different. Binaries built on NixOS usually point at a specific loader inside the Nix store. That makes the binary non-portable unless you patch the interpreter path for non-NixOS, but once you do that the same binary no longer runs on plain NixOS without extra help.

nix-dynld provides that extra help: a loader at the conventional path which selects a real loader from the Nix store, primarily by inspecting the target binary's RPATH or RUNPATH.

## Installation

### Flake usage

This flake exports the module as:

- `nixosModules.default`
- `nixosModules.dynld`

Example:

```nix
{
  inputs.nix-dynld.url = "path:./nix-dynld";

  outputs = { self, nixpkgs, nix-dynld, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        nix-dynld.nixosModules.dynld
        { dynld.enable = true; }
      ];
    };
  };
}
```

### Plain module import

Add this project's module directory to your NixOS imports:

```nix
{
  imports = [
    ./nix-dynld/modules
  ];

  dynld.enable = true;
}
```

## Behavior

nix-dynld is a two-stage loader. Stage 1 is a freestanding loader entrypoint placed at the conventional loader path. Stage 2 resolves the real loader from the target executable and then `execve`s it.

The main resolution path is:

1. Resolve the target executable path.
2. Read its ELF dynamic metadata directly.
3. Prefer `DT_RUNPATH`, otherwise `DT_RPATH`.
4. Treat that raw colon-separated string as a synthetic search path and look for `ld-linux-x86-64.so.2`.

Notes:

- Neither `$ORIGIN` expansion nor path normalization is performed in that path.
- Setting environment variable `DYNLD_ALLOW_LDD=1` enables the fallback path for binaries that do not provide a useful Nix-style RPATH/RUNPATH. This is impure and should be treated as a compatibility escape hatch.
- `LD_PRELOAD` is preserved across the stage1/stage2 handoff.
- `DYNLD_EXECUTABLE_NAME` is set for the final exec so the resolved executable path is available downstream.

## Building binaries for use with nix-dynld

If you build on non-NixOS, many binaries will already work on NixOS once nix-dynld is enabled, provided their shared-library dependencies are available.

If you build on NixOS and want the same binary to run on conventional Linux too, you usually need to do at least two things:

- Patch the ELF interpreter to `/lib64/ld-linux-x86-64.so.2`, for example with `patchelf --set-interpreter /lib64/ld-linux-x86-64.so.2`.
- Control your glibc baseline so you do not emit symbol-version requirements newer than the target systems provide.

The closer you stay to "only libc" or a mostly static dependency surface, the easier this is.

## Compared to nix-ld

`nix-ld` and `nix-dynld` solve the same high-level problem, but they make
different tradeoffs.

- `nix-ld` is explicit: you provide `NIX_LD` and usually `NIX_LD_LIBRARY_PATH`, and it forwards into that chosen loader. `nix-dynld` is heuristic: it tries to infer the right loader from the target executable's RPATH or RUNPATH and only falls back to the opt-in ambient path mode when asked.
- `nix-ld` documents and exposes a larger public env-var surface: `NIX_LD`, `NIX_LD_LIBRARY_PATH`, platform-specific variants, and `NIX_LD_LOG`. `nix-dynld` exposes a much smaller intended surface; the only public switch is `DYNLD_ALLOW_LDD=1`.
- `nix-ld` explicitly preserves `/proc/self/exe` for the launched process. `nix-dynld` does not currently has that property.

In practice:

- Use `nix-ld` when you want explicit control over loader and library paths.
- Use `nix-dynld` when you want a loader that can often infer the right Nix store loader automatically from the binary itself.

## NixOS 24.05 stub-ld

NixOS 24.05 introduced the built-in stub LD behavior that prints a clearer error when a foreign dynamically linked binary is launched. nix-dynld disables that stub behavior when enabled and replaces it with the real meta-loader.

## Attribution and license

This project is derived from Madness by Antithesis Operations LLC:
https://github.com/antithesishq/madness/

nix-dynld is licensed under the BSD 3-Clause License (see `LICENSE`). 
Copyright (c) 2024, Antithesis. 
Copyright (c) 2026, [Hex](https://0x484558.dev/) @ [aleph0 s.r.o.](https://aleph0.ai/)
