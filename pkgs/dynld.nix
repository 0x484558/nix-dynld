# dynld is a meta-loader which, when installed at the default FHS location /lib64/ld-linux-x86-64.so.2 (see ./ld-link.nix)
# will allow programs built with a normal NixOS R(UN)PATH but a FHS program interpreter to run on NixOS, by searching the
# program's RPATH for a loader.

# It's implemented in two stages. dynld_stage1 is a C program written without libc (since libc's initialization crashes
# when invoked as a program loader for some reason) that simply `exec`s the stage2 loader passing along its command line.
# dynld_stage2_loader is also a freestanding C program: it parses the executable's ELF metadata itself, resolves the
# appropriate loader path using only syscalls, and then `exec`s the real loader.

# Note that the program to be executed is mapped by the kernel into the process along with the stage1 loader, even though
# it will never run there. In order for this to work reliably the stage1 loader must be build position-independent (-pie)

{runCommand, gcc}:
let
    stage2_loader_src = ./dynld_stage2_loader.c;
    stage2_loader = runCommand "dynld_stage2_loader" {} ''
        ${gcc}/bin/gcc -fPIC -pie -fno-stack-protector -O2 -Wall -Wextra -Werror \
            -nostdlib -nostartfiles -ffreestanding -Wl,--no-dynamic-linker \
            ${stage2_loader_src} -o $out
    '';
    stage1_loader_src = ./dynld_stage1_loader.c;
    # As of 08/19/2021, dynld does not build with the GCC 10 toolchain.
    dynld_loader = runCommand "dynld_stage1_loader" {} ''
        ${gcc}/bin/gcc -fPIC -pie -fno-stack-protector -O2 -nostdlib -nostartfiles -ffreestanding \
            -Wl,--no-dynamic-linker \
            -DSTAGE2_LOADER=\"${stage2_loader}\" \
            ${stage1_loader_src} -o $out
    '';
in {
    inherit dynld_loader;
    loader = dynld_loader;
}
