| Interrupt-time entry points for C-Desk-Vint's engine.
|
| The Time Manager calls a task with A1 pointing at its TMTask record; the
| Deferred Task Manager calls one with A1 holding its dtParam. Neither sets up
| A5, so each records the application's A5 where the glue can find it: after
| the TMTask fields, and as the deferred task's dtParam. The C routines get a
| normal C call with A5 in place.

	.text

| A1 -> cdv_tm, whose a5 field sits 22 bytes in (after the extended TMTask).
	.globl	cdv_tm_glue
	.align	2
cdv_tm_glue:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	22(%a1),%a5
	jsr	cdv_tm_fire
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| A1 = our A5.
	.globl	cdv_dt_glue
	.align	2
cdv_dt_glue:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	%a1,%a5
	jsr	cdv_dt_fire
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
