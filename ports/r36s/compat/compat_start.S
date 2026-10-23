/* Replacement for glibc's crt1.o on aarch64 (non-PIE).
 *
 * glibc 2.34+ crt1.o calls __libc_start_main@GLIBC_2.34, which makes the binary refuse to start on
 * older systems (ArkOS 2.0 and other images with glibc < 2.34). This start file calls the original
 * __libc_start_main@GLIBC_2.17 ABI instead, passing init/fini routines (see glibc_compat.c) exactly
 * like the old crt1.o did. glibc 2.34+ still exports that version for old binaries, so the same
 * executable runs on both old and new glibc. */
	.text
	.align	2
	.global	_start
	.type	_start, %function
_start:
	mov	x29, #0
	mov	x30, #0
	mov	x5, x0                      /* rtld_fini from ld.so */
	adrp	x0, main
	add	x0, x0, :lo12:main
	ldr	x1, [sp]                    /* argc */
	add	x2, sp, #8                  /* argv */
	mov	x6, sp                      /* stack_end */
	adrp	x3, compat_csu_init
	add	x3, x3, :lo12:compat_csu_init
	adrp	x4, compat_csu_fini
	add	x4, x4, :lo12:compat_csu_fini
	and	sp, x6, #~15
	bl	compat_old___libc_start_main
	bl	abort
	.size	_start, .-_start

	.symver	compat_old___libc_start_main, __libc_start_main@GLIBC_2.17

	.section .rodata
	.global	_IO_stdin_used
	.type	_IO_stdin_used, %object
	.align	2
_IO_stdin_used:
	.word	0x20001
	.size	_IO_stdin_used, .-_IO_stdin_used

	.section .note.GNU-stack,"",%progbits
