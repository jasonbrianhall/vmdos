/* dosfork.S -- the two halves of fork() for DOS (see dosproc.c).

   int dos_fork_save (struct dos_regs *r)
	Saves the callee-saved registers, the stack pointer and the return
	address in *r and returns 0, like setjmp.  It returns a second time,
	with the value given to dos_resume, when the child ends.

   void dos_resume (struct dos_regs *r, int value, int sel, void *stack)
	Copies the parent's memory back from the snapshot at sel:0 and
	returns from dos_fork_save (r) with value.  The snapshot starts with a
	table: a count n, then n (offset, length) pairs, then the n pieces
	of memory one after another.  The copy runs on `stack', a place the
	snapshot doesn't cover, and uses no stack itself, since it overwrites
	the memory the child's stack was in. */

	.text
	.p2align 4
	.globl	_dos_fork_save
_dos_fork_save:
	movl	4(%esp), %eax
	movl	%ebx, 0(%eax)
	movl	%esi, 4(%eax)
	movl	%edi, 8(%eax)
	movl	%ebp, 12(%eax)
	leal	4(%esp), %ecx		/* the caller's esp after we return */
	movl	%ecx, 16(%eax)
	movl	(%esp), %ecx		/* where we return to */
	movl	%ecx, 20(%eax)
	xorl	%eax, %eax
	ret

	.p2align 4
	.globl	_dos_resume
_dos_resume:
	movl	4(%esp), %edx		/* r */
	movl	8(%esp), %ebx		/* value */
	movl	12(%esp), %eax		/* sel */
	movl	16(%esp), %esp		/* a stack the copy won't touch */
	cld
	movw	%ax, %ds		/* source: the snapshot; es stays ours */
	movl	0, %ebp			/* n */
	movl	$4, %eax		/* table cursor */
	leal	4(,%ebp,8), %esi	/* data cursor */
1:	testl	%ebp, %ebp
	jz	2f
	movl	(%eax), %edi
	movl	4(%eax), %ecx
	shrl	$2, %ecx
	rep
	movsl
	addl	$8, %eax
	decl	%ebp
	jmp	1b
2:	movw	%es, %ax
	movw	%ax, %ds
	movl	%ebx, %eax		/* dos_fork_save's second return value */
	movl	0(%edx), %ebx
	movl	4(%edx), %esi
	movl	8(%edx), %edi
	movl	12(%edx), %ebp
	movl	16(%edx), %esp
	jmp	*20(%edx)
