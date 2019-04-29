.global zpp_elf_binary
.hidden zpp_elf_binary

.global zpp_elf_binary_size
.hidden zpp_elf_binary_size

	.align 8
zpp_elf_binary:
	.incbin ZPP_ELF_BINARY_PATH
zpp_elf_binary_end:
	.align 8
zpp_elf_binary_size:
	.quad zpp_elf_binary_end-zpp_elf_binary
