/* alr-lane: guest-memory copies that survive a bad pointer, and the
 * SIGSEGV/SIGBUS entry that makes them survive (alr_fault.c).
 *
 * Every load or store that may touch a guest address sits between
 * tawc_safe_begin and tawc_safe_end.  A fault there is redirected by
 * tawc_fault_route to tawc_safe_fault, which returns -1: the caller turns
 * that into EFAULT, as the kernel's own copy would.
 *
 * long tawc_safe_strcpy(char *dst, const char *src, size_t cap)
 *   Copies bytes up to and including a NUL, at most cap.  Returns the
 *   length without the NUL, cap when no NUL came within cap, or -1.
 *   One byte at a time: it never reads past the NUL, so a string that
 *   ends just before an unmapped page reads exactly as the kernel would.
 *
 * long tawc_safe_memcpy(void *dst, const void *src, size_t n)
 *   Returns 0, or -1.  Either side may be the guest's.
 */

#ifdef TAWCROOT_ALR

	.text
	.p2align 4
	.global tawc_safe_begin
	.hidden tawc_safe_begin
tawc_safe_begin:

	.global tawc_safe_strcpy
	.hidden tawc_safe_strcpy
	.type   tawc_safe_strcpy, %function
tawc_safe_strcpy:
	mov     x3, #0
1:	cmp     x3, x2
	b.hs    2f
	ldrb    w4, [x1, x3]
	strb    w4, [x0, x3]
	cbz     w4, 3f
	add     x3, x3, #1
	b       1b
2:	mov     x0, x2
	ret
3:	mov     x0, x3
	ret
	.size tawc_safe_strcpy, .-tawc_safe_strcpy

	.global tawc_safe_memcpy
	.hidden tawc_safe_memcpy
	.type   tawc_safe_memcpy, %function
tawc_safe_memcpy:
1:	cmp     x2, #8
	b.lo    2f
	ldr     x4, [x1], #8
	str     x4, [x0], #8
	sub     x2, x2, #8
	b       1b
2:	cbz     x2, 3f
	ldrb    w4, [x1], #1
	strb    w4, [x0], #1
	sub     x2, x2, #1
	b       2b
3:	mov     x0, #0
	ret
	.size tawc_safe_memcpy, .-tawc_safe_memcpy

	.global tawc_safe_fault
	.hidden tawc_safe_fault
tawc_safe_fault:
	mov     x0, #-1
	ret

	.global tawc_safe_end
	.hidden tawc_safe_end
tawc_safe_end:

/* SIGSEGV/SIGBUS handler as installed in the kernel.  tawc_fault_route
 * returns 0 when it dealt with the signal, else the guest's handler, which
 * is entered with the kernel's three arguments, stack and return address
 * untouched -- as if the kernel had called it.  x16: a BTI "c" landing pad
 * accepts a BR through x16/x17. */
	.global tawc_fault_entry
	.hidden tawc_fault_entry
	.type   tawc_fault_entry, %function
tawc_fault_entry:
	stp     x29, x30, [sp, #-48]!
	mov     x29, sp
	stp     x0, x1, [sp, #16]
	str     x2, [sp, #32]
	bl      tawc_fault_route
	mov     x16, x0
	ldp     x0, x1, [sp, #16]
	ldr     x2, [sp, #32]
	ldp     x29, x30, [sp], #48
	cbz     x16, 1f
	br      x16
1:	ret
	.size tawc_fault_entry, .-tawc_fault_entry

#endif /* TAWCROOT_ALR */
