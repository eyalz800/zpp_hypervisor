	.section .t0,"ax",@progbits
	addb %dl, (%rcx)
	.section .t1,"ax",@progbits
	addb %dl, 0x10(%rcx)
	.section .t2,"ax",@progbits
	addb %dl, -0x8(%rbp)
	.section .t3,"ax",@progbits
	addb %dl, 0x12345(%rcx)
	.section .t4,"ax",@progbits
	addb %dl, (%rax,%rsi,4)
	.section .t5,"ax",@progbits
	addb %dl, 0x10(%rax,%rsi,8)
	.section .t6,"ax",@progbits
	addb %dl, (,%rsi,2)
	.section .t7,"ax",@progbits
	addb %dl, 0x40(%rip)
	.section .t8,"ax",@progbits
	addb %dl, -0x100(%rip)
	.section .t9,"ax",@progbits
	addb %dl, 0x1234
	.section .t10,"ax",@progbits
	addb %dl, (%r8)
	.section .t11,"ax",@progbits
	addb %dl, (%r12)
	.section .t12,"ax",@progbits
	addb %dl, 0x8(%r13)
	.section .t13,"ax",@progbits
	addb %dl, (%r8,%r15,2)
	.section .t14,"ax",@progbits
	addb %dl, (%rax,%r12,4)
	.section .t15,"ax",@progbits
	addb %dl, 0x100(%rbp)
	.section .t16,"ax",@progbits
	addb %dl, (%rsp)
	.section .t17,"ax",@progbits
	addb %dl, 0x10(%rsp,%rsi,4)
	.section .t18,"ax",@progbits
	addb %dl, %gs:0x10(%rcx)
	.section .t19,"ax",@progbits
	addb %dl, %fs:(%rax,%rsi,8)
	.section .t20,"ax",@progbits
	addb %bl, (%rcx)
	.section .t21,"ax",@progbits
	addb %bl, 0x10(%rcx)
	.section .t22,"ax",@progbits
	addb %bl, -0x8(%rbp)
	.section .t23,"ax",@progbits
	addb %bl, 0x12345(%rcx)
	.section .t24,"ax",@progbits
	addb %bl, (%rax,%rsi,4)
	.section .t25,"ax",@progbits
	addb %bl, 0x10(%rax,%rsi,8)
	.section .t26,"ax",@progbits
	addb %bl, (,%rsi,2)
	.section .t27,"ax",@progbits
	addb %bl, 0x40(%rip)
	.section .t28,"ax",@progbits
	addb %bl, -0x100(%rip)
	.section .t29,"ax",@progbits
	addb %bl, 0x1234
	.section .t30,"ax",@progbits
	addb %bl, (%r8)
	.section .t31,"ax",@progbits
	addb %bl, (%r12)
	.section .t32,"ax",@progbits
	addb %bl, 0x8(%r13)
	.section .t33,"ax",@progbits
	addb %bl, (%r8,%r15,2)
	.section .t34,"ax",@progbits
	addb %bl, (%rax,%r12,4)
	.section .t35,"ax",@progbits
	addb %bl, 0x100(%rbp)
	.section .t36,"ax",@progbits
	addb %bl, (%rsp)
	.section .t37,"ax",@progbits
	addb %bl, 0x10(%rsp,%rsi,4)
	.section .t38,"ax",@progbits
	addb %bl, %gs:0x10(%rcx)
	.section .t39,"ax",@progbits
	addb %bl, %fs:(%rax,%rsi,8)
	.section .t40,"ax",@progbits
	addb %r9b, (%rcx)
	.section .t41,"ax",@progbits
	addb %r9b, 0x10(%rcx)
	.section .t42,"ax",@progbits
	addb %r9b, -0x8(%rbp)
	.section .t43,"ax",@progbits
	addb %r9b, 0x12345(%rcx)
	.section .t44,"ax",@progbits
	addb %r9b, (%rax,%rsi,4)
	.section .t45,"ax",@progbits
	addb %r9b, 0x10(%rax,%rsi,8)
	.section .t46,"ax",@progbits
	addb %r9b, (,%rsi,2)
	.section .t47,"ax",@progbits
	addb %r9b, 0x40(%rip)
	.section .t48,"ax",@progbits
	addb %r9b, -0x100(%rip)
	.section .t49,"ax",@progbits
	addb %r9b, 0x1234
	.section .t50,"ax",@progbits
	addb %r9b, (%r8)
	.section .t51,"ax",@progbits
	addb %r9b, (%r12)
	.section .t52,"ax",@progbits
	addb %r9b, 0x8(%r13)
	.section .t53,"ax",@progbits
	addb %r9b, (%r8,%r15,2)
	.section .t54,"ax",@progbits
	addb %r9b, (%rax,%r12,4)
	.section .t55,"ax",@progbits
	addb %r9b, 0x100(%rbp)
	.section .t56,"ax",@progbits
	addb %r9b, (%rsp)
	.section .t57,"ax",@progbits
	addb %r9b, 0x10(%rsp,%rsi,4)
	.section .t58,"ax",@progbits
	addb %r9b, %gs:0x10(%rcx)
	.section .t59,"ax",@progbits
	addb %r9b, %fs:(%rax,%rsi,8)
	.section .t60,"ax",@progbits
	addb %r13b, (%rcx)
	.section .t61,"ax",@progbits
	addb %r13b, 0x10(%rcx)
	.section .t62,"ax",@progbits
	addb %r13b, -0x8(%rbp)
	.section .t63,"ax",@progbits
	addb %r13b, 0x12345(%rcx)
	.section .t64,"ax",@progbits
	addb %r13b, (%rax,%rsi,4)
	.section .t65,"ax",@progbits
	addb %r13b, 0x10(%rax,%rsi,8)
	.section .t66,"ax",@progbits
	addb %r13b, (,%rsi,2)
	.section .t67,"ax",@progbits
	addb %r13b, 0x40(%rip)
	.section .t68,"ax",@progbits
	addb %r13b, -0x100(%rip)
	.section .t69,"ax",@progbits
	addb %r13b, 0x1234
	.section .t70,"ax",@progbits
	addb %r13b, (%r8)
	.section .t71,"ax",@progbits
	addb %r13b, (%r12)
	.section .t72,"ax",@progbits
	addb %r13b, 0x8(%r13)
	.section .t73,"ax",@progbits
	addb %r13b, (%r8,%r15,2)
	.section .t74,"ax",@progbits
	addb %r13b, (%rax,%r12,4)
	.section .t75,"ax",@progbits
	addb %r13b, 0x100(%rbp)
	.section .t76,"ax",@progbits
	addb %r13b, (%rsp)
	.section .t77,"ax",@progbits
	addb %r13b, 0x10(%rsp,%rsi,4)
	.section .t78,"ax",@progbits
	addb %r13b, %gs:0x10(%rcx)
	.section .t79,"ax",@progbits
	addb %r13b, %fs:(%rax,%rsi,8)
	.section .t80,"ax",@progbits
	addw %dx, (%rcx)
	.section .t81,"ax",@progbits
	addw %dx, 0x10(%rcx)
	.section .t82,"ax",@progbits
	addw %dx, -0x8(%rbp)
	.section .t83,"ax",@progbits
	addw %dx, 0x12345(%rcx)
	.section .t84,"ax",@progbits
	addw %dx, (%rax,%rsi,4)
	.section .t85,"ax",@progbits
	addw %dx, 0x10(%rax,%rsi,8)
	.section .t86,"ax",@progbits
	addw %dx, (,%rsi,2)
	.section .t87,"ax",@progbits
	addw %dx, 0x40(%rip)
	.section .t88,"ax",@progbits
	addw %dx, -0x100(%rip)
	.section .t89,"ax",@progbits
	addw %dx, 0x1234
	.section .t90,"ax",@progbits
	addw %dx, (%r8)
	.section .t91,"ax",@progbits
	addw %dx, (%r12)
	.section .t92,"ax",@progbits
	addw %dx, 0x8(%r13)
	.section .t93,"ax",@progbits
	addw %dx, (%r8,%r15,2)
	.section .t94,"ax",@progbits
	addw %dx, (%rax,%r12,4)
	.section .t95,"ax",@progbits
	addw %dx, 0x100(%rbp)
	.section .t96,"ax",@progbits
	addw %dx, (%rsp)
	.section .t97,"ax",@progbits
	addw %dx, 0x10(%rsp,%rsi,4)
	.section .t98,"ax",@progbits
	addw %dx, %gs:0x10(%rcx)
	.section .t99,"ax",@progbits
	addw %dx, %fs:(%rax,%rsi,8)
	.section .t100,"ax",@progbits
	addw %bx, (%rcx)
	.section .t101,"ax",@progbits
	addw %bx, 0x10(%rcx)
	.section .t102,"ax",@progbits
	addw %bx, -0x8(%rbp)
	.section .t103,"ax",@progbits
	addw %bx, 0x12345(%rcx)
	.section .t104,"ax",@progbits
	addw %bx, (%rax,%rsi,4)
	.section .t105,"ax",@progbits
	addw %bx, 0x10(%rax,%rsi,8)
	.section .t106,"ax",@progbits
	addw %bx, (,%rsi,2)
	.section .t107,"ax",@progbits
	addw %bx, 0x40(%rip)
	.section .t108,"ax",@progbits
	addw %bx, -0x100(%rip)
	.section .t109,"ax",@progbits
	addw %bx, 0x1234
	.section .t110,"ax",@progbits
	addw %bx, (%r8)
	.section .t111,"ax",@progbits
	addw %bx, (%r12)
	.section .t112,"ax",@progbits
	addw %bx, 0x8(%r13)
	.section .t113,"ax",@progbits
	addw %bx, (%r8,%r15,2)
	.section .t114,"ax",@progbits
	addw %bx, (%rax,%r12,4)
	.section .t115,"ax",@progbits
	addw %bx, 0x100(%rbp)
	.section .t116,"ax",@progbits
	addw %bx, (%rsp)
	.section .t117,"ax",@progbits
	addw %bx, 0x10(%rsp,%rsi,4)
	.section .t118,"ax",@progbits
	addw %bx, %gs:0x10(%rcx)
	.section .t119,"ax",@progbits
	addw %bx, %fs:(%rax,%rsi,8)
	.section .t120,"ax",@progbits
	addw %r9w, (%rcx)
	.section .t121,"ax",@progbits
	addw %r9w, 0x10(%rcx)
	.section .t122,"ax",@progbits
	addw %r9w, -0x8(%rbp)
	.section .t123,"ax",@progbits
	addw %r9w, 0x12345(%rcx)
	.section .t124,"ax",@progbits
	addw %r9w, (%rax,%rsi,4)
	.section .t125,"ax",@progbits
	addw %r9w, 0x10(%rax,%rsi,8)
	.section .t126,"ax",@progbits
	addw %r9w, (,%rsi,2)
	.section .t127,"ax",@progbits
	addw %r9w, 0x40(%rip)
	.section .t128,"ax",@progbits
	addw %r9w, -0x100(%rip)
	.section .t129,"ax",@progbits
	addw %r9w, 0x1234
	.section .t130,"ax",@progbits
	addw %r9w, (%r8)
	.section .t131,"ax",@progbits
	addw %r9w, (%r12)
	.section .t132,"ax",@progbits
	addw %r9w, 0x8(%r13)
	.section .t133,"ax",@progbits
	addw %r9w, (%r8,%r15,2)
	.section .t134,"ax",@progbits
	addw %r9w, (%rax,%r12,4)
	.section .t135,"ax",@progbits
	addw %r9w, 0x100(%rbp)
	.section .t136,"ax",@progbits
	addw %r9w, (%rsp)
	.section .t137,"ax",@progbits
	addw %r9w, 0x10(%rsp,%rsi,4)
	.section .t138,"ax",@progbits
	addw %r9w, %gs:0x10(%rcx)
	.section .t139,"ax",@progbits
	addw %r9w, %fs:(%rax,%rsi,8)
	.section .t140,"ax",@progbits
	addl %edx, (%rcx)
	.section .t141,"ax",@progbits
	addl %edx, 0x10(%rcx)
	.section .t142,"ax",@progbits
	addl %edx, -0x8(%rbp)
	.section .t143,"ax",@progbits
	addl %edx, 0x12345(%rcx)
	.section .t144,"ax",@progbits
	addl %edx, (%rax,%rsi,4)
	.section .t145,"ax",@progbits
	addl %edx, 0x10(%rax,%rsi,8)
	.section .t146,"ax",@progbits
	addl %edx, (,%rsi,2)
	.section .t147,"ax",@progbits
	addl %edx, 0x40(%rip)
	.section .t148,"ax",@progbits
	addl %edx, -0x100(%rip)
	.section .t149,"ax",@progbits
	addl %edx, 0x1234
	.section .t150,"ax",@progbits
	addl %edx, (%r8)
	.section .t151,"ax",@progbits
	addl %edx, (%r12)
	.section .t152,"ax",@progbits
	addl %edx, 0x8(%r13)
	.section .t153,"ax",@progbits
	addl %edx, (%r8,%r15,2)
	.section .t154,"ax",@progbits
	addl %edx, (%rax,%r12,4)
	.section .t155,"ax",@progbits
	addl %edx, 0x100(%rbp)
	.section .t156,"ax",@progbits
	addl %edx, (%rsp)
	.section .t157,"ax",@progbits
	addl %edx, 0x10(%rsp,%rsi,4)
	.section .t158,"ax",@progbits
	addl %edx, %gs:0x10(%rcx)
	.section .t159,"ax",@progbits
	addl %edx, %fs:(%rax,%rsi,8)
	.section .t160,"ax",@progbits
	addl %ebx, (%rcx)
	.section .t161,"ax",@progbits
	addl %ebx, 0x10(%rcx)
	.section .t162,"ax",@progbits
	addl %ebx, -0x8(%rbp)
	.section .t163,"ax",@progbits
	addl %ebx, 0x12345(%rcx)
	.section .t164,"ax",@progbits
	addl %ebx, (%rax,%rsi,4)
	.section .t165,"ax",@progbits
	addl %ebx, 0x10(%rax,%rsi,8)
	.section .t166,"ax",@progbits
	addl %ebx, (,%rsi,2)
	.section .t167,"ax",@progbits
	addl %ebx, 0x40(%rip)
	.section .t168,"ax",@progbits
	addl %ebx, -0x100(%rip)
	.section .t169,"ax",@progbits
	addl %ebx, 0x1234
	.section .t170,"ax",@progbits
	addl %ebx, (%r8)
	.section .t171,"ax",@progbits
	addl %ebx, (%r12)
	.section .t172,"ax",@progbits
	addl %ebx, 0x8(%r13)
	.section .t173,"ax",@progbits
	addl %ebx, (%r8,%r15,2)
	.section .t174,"ax",@progbits
	addl %ebx, (%rax,%r12,4)
	.section .t175,"ax",@progbits
	addl %ebx, 0x100(%rbp)
	.section .t176,"ax",@progbits
	addl %ebx, (%rsp)
	.section .t177,"ax",@progbits
	addl %ebx, 0x10(%rsp,%rsi,4)
	.section .t178,"ax",@progbits
	addl %ebx, %gs:0x10(%rcx)
	.section .t179,"ax",@progbits
	addl %ebx, %fs:(%rax,%rsi,8)
	.section .t180,"ax",@progbits
	addl %r9d, (%rcx)
	.section .t181,"ax",@progbits
	addl %r9d, 0x10(%rcx)
	.section .t182,"ax",@progbits
	addl %r9d, -0x8(%rbp)
	.section .t183,"ax",@progbits
	addl %r9d, 0x12345(%rcx)
	.section .t184,"ax",@progbits
	addl %r9d, (%rax,%rsi,4)
	.section .t185,"ax",@progbits
	addl %r9d, 0x10(%rax,%rsi,8)
	.section .t186,"ax",@progbits
	addl %r9d, (,%rsi,2)
	.section .t187,"ax",@progbits
	addl %r9d, 0x40(%rip)
	.section .t188,"ax",@progbits
	addl %r9d, -0x100(%rip)
	.section .t189,"ax",@progbits
	addl %r9d, 0x1234
	.section .t190,"ax",@progbits
	addl %r9d, (%r8)
	.section .t191,"ax",@progbits
	addl %r9d, (%r12)
	.section .t192,"ax",@progbits
	addl %r9d, 0x8(%r13)
	.section .t193,"ax",@progbits
	addl %r9d, (%r8,%r15,2)
	.section .t194,"ax",@progbits
	addl %r9d, (%rax,%r12,4)
	.section .t195,"ax",@progbits
	addl %r9d, 0x100(%rbp)
	.section .t196,"ax",@progbits
	addl %r9d, (%rsp)
	.section .t197,"ax",@progbits
	addl %r9d, 0x10(%rsp,%rsi,4)
	.section .t198,"ax",@progbits
	addl %r9d, %gs:0x10(%rcx)
	.section .t199,"ax",@progbits
	addl %r9d, %fs:(%rax,%rsi,8)
	.section .t200,"ax",@progbits
	addq %rdx, (%rcx)
	.section .t201,"ax",@progbits
	addq %rdx, 0x10(%rcx)
	.section .t202,"ax",@progbits
	addq %rdx, -0x8(%rbp)
	.section .t203,"ax",@progbits
	addq %rdx, 0x12345(%rcx)
	.section .t204,"ax",@progbits
	addq %rdx, (%rax,%rsi,4)
	.section .t205,"ax",@progbits
	addq %rdx, 0x10(%rax,%rsi,8)
	.section .t206,"ax",@progbits
	addq %rdx, (,%rsi,2)
	.section .t207,"ax",@progbits
	addq %rdx, 0x40(%rip)
	.section .t208,"ax",@progbits
	addq %rdx, -0x100(%rip)
	.section .t209,"ax",@progbits
	addq %rdx, 0x1234
	.section .t210,"ax",@progbits
	addq %rdx, (%r8)
	.section .t211,"ax",@progbits
	addq %rdx, (%r12)
	.section .t212,"ax",@progbits
	addq %rdx, 0x8(%r13)
	.section .t213,"ax",@progbits
	addq %rdx, (%r8,%r15,2)
	.section .t214,"ax",@progbits
	addq %rdx, (%rax,%r12,4)
	.section .t215,"ax",@progbits
	addq %rdx, 0x100(%rbp)
	.section .t216,"ax",@progbits
	addq %rdx, (%rsp)
	.section .t217,"ax",@progbits
	addq %rdx, 0x10(%rsp,%rsi,4)
	.section .t218,"ax",@progbits
	addq %rdx, %gs:0x10(%rcx)
	.section .t219,"ax",@progbits
	addq %rdx, %fs:(%rax,%rsi,8)
	.section .t220,"ax",@progbits
	addq %rbx, (%rcx)
	.section .t221,"ax",@progbits
	addq %rbx, 0x10(%rcx)
	.section .t222,"ax",@progbits
	addq %rbx, -0x8(%rbp)
	.section .t223,"ax",@progbits
	addq %rbx, 0x12345(%rcx)
	.section .t224,"ax",@progbits
	addq %rbx, (%rax,%rsi,4)
	.section .t225,"ax",@progbits
	addq %rbx, 0x10(%rax,%rsi,8)
	.section .t226,"ax",@progbits
	addq %rbx, (,%rsi,2)
	.section .t227,"ax",@progbits
	addq %rbx, 0x40(%rip)
	.section .t228,"ax",@progbits
	addq %rbx, -0x100(%rip)
	.section .t229,"ax",@progbits
	addq %rbx, 0x1234
	.section .t230,"ax",@progbits
	addq %rbx, (%r8)
	.section .t231,"ax",@progbits
	addq %rbx, (%r12)
	.section .t232,"ax",@progbits
	addq %rbx, 0x8(%r13)
	.section .t233,"ax",@progbits
	addq %rbx, (%r8,%r15,2)
	.section .t234,"ax",@progbits
	addq %rbx, (%rax,%r12,4)
	.section .t235,"ax",@progbits
	addq %rbx, 0x100(%rbp)
	.section .t236,"ax",@progbits
	addq %rbx, (%rsp)
	.section .t237,"ax",@progbits
	addq %rbx, 0x10(%rsp,%rsi,4)
	.section .t238,"ax",@progbits
	addq %rbx, %gs:0x10(%rcx)
	.section .t239,"ax",@progbits
	addq %rbx, %fs:(%rax,%rsi,8)
	.section .t240,"ax",@progbits
	addq %r9, (%rcx)
	.section .t241,"ax",@progbits
	addq %r9, 0x10(%rcx)
	.section .t242,"ax",@progbits
	addq %r9, -0x8(%rbp)
	.section .t243,"ax",@progbits
	addq %r9, 0x12345(%rcx)
	.section .t244,"ax",@progbits
	addq %r9, (%rax,%rsi,4)
	.section .t245,"ax",@progbits
	addq %r9, 0x10(%rax,%rsi,8)
	.section .t246,"ax",@progbits
	addq %r9, (,%rsi,2)
	.section .t247,"ax",@progbits
	addq %r9, 0x40(%rip)
	.section .t248,"ax",@progbits
	addq %r9, -0x100(%rip)
	.section .t249,"ax",@progbits
	addq %r9, 0x1234
	.section .t250,"ax",@progbits
	addq %r9, (%r8)
	.section .t251,"ax",@progbits
	addq %r9, (%r12)
	.section .t252,"ax",@progbits
	addq %r9, 0x8(%r13)
	.section .t253,"ax",@progbits
	addq %r9, (%r8,%r15,2)
	.section .t254,"ax",@progbits
	addq %r9, (%rax,%r12,4)
	.section .t255,"ax",@progbits
	addq %r9, 0x100(%rbp)
	.section .t256,"ax",@progbits
	addq %r9, (%rsp)
	.section .t257,"ax",@progbits
	addq %r9, 0x10(%rsp,%rsi,4)
	.section .t258,"ax",@progbits
	addq %r9, %gs:0x10(%rcx)
	.section .t259,"ax",@progbits
	addq %r9, %fs:(%rax,%rsi,8)
	.section .t260,"ax",@progbits
	addq %r12, (%rcx)
	.section .t261,"ax",@progbits
	addq %r12, 0x10(%rcx)
	.section .t262,"ax",@progbits
	addq %r12, -0x8(%rbp)
	.section .t263,"ax",@progbits
	addq %r12, 0x12345(%rcx)
	.section .t264,"ax",@progbits
	addq %r12, (%rax,%rsi,4)
	.section .t265,"ax",@progbits
	addq %r12, 0x10(%rax,%rsi,8)
	.section .t266,"ax",@progbits
	addq %r12, (,%rsi,2)
	.section .t267,"ax",@progbits
	addq %r12, 0x40(%rip)
	.section .t268,"ax",@progbits
	addq %r12, -0x100(%rip)
	.section .t269,"ax",@progbits
	addq %r12, 0x1234
	.section .t270,"ax",@progbits
	addq %r12, (%r8)
	.section .t271,"ax",@progbits
	addq %r12, (%r12)
	.section .t272,"ax",@progbits
	addq %r12, 0x8(%r13)
	.section .t273,"ax",@progbits
	addq %r12, (%r8,%r15,2)
	.section .t274,"ax",@progbits
	addq %r12, (%rax,%r12,4)
	.section .t275,"ax",@progbits
	addq %r12, 0x100(%rbp)
	.section .t276,"ax",@progbits
	addq %r12, (%rsp)
	.section .t277,"ax",@progbits
	addq %r12, 0x10(%rsp,%rsi,4)
	.section .t278,"ax",@progbits
	addq %r12, %gs:0x10(%rcx)
	.section .t279,"ax",@progbits
	addq %r12, %fs:(%rax,%rsi,8)
	.section .t280,"ax",@progbits
	orb %dl, (%rcx)
	.section .t281,"ax",@progbits
	orb %dl, 0x10(%rcx)
	.section .t282,"ax",@progbits
	orb %dl, -0x8(%rbp)
	.section .t283,"ax",@progbits
	orb %dl, 0x12345(%rcx)
	.section .t284,"ax",@progbits
	orb %dl, (%rax,%rsi,4)
	.section .t285,"ax",@progbits
	orb %dl, 0x10(%rax,%rsi,8)
	.section .t286,"ax",@progbits
	orb %dl, (,%rsi,2)
	.section .t287,"ax",@progbits
	orb %dl, 0x40(%rip)
	.section .t288,"ax",@progbits
	orb %dl, -0x100(%rip)
	.section .t289,"ax",@progbits
	orb %dl, 0x1234
	.section .t290,"ax",@progbits
	orb %dl, (%r8)
	.section .t291,"ax",@progbits
	orb %dl, (%r12)
	.section .t292,"ax",@progbits
	orb %dl, 0x8(%r13)
	.section .t293,"ax",@progbits
	orb %dl, (%r8,%r15,2)
	.section .t294,"ax",@progbits
	orb %dl, (%rax,%r12,4)
	.section .t295,"ax",@progbits
	orb %dl, 0x100(%rbp)
	.section .t296,"ax",@progbits
	orb %dl, (%rsp)
	.section .t297,"ax",@progbits
	orb %dl, 0x10(%rsp,%rsi,4)
	.section .t298,"ax",@progbits
	orb %dl, %gs:0x10(%rcx)
	.section .t299,"ax",@progbits
	orb %dl, %fs:(%rax,%rsi,8)
	.section .t300,"ax",@progbits
	orb %bl, (%rcx)
	.section .t301,"ax",@progbits
	orb %bl, 0x10(%rcx)
	.section .t302,"ax",@progbits
	orb %bl, -0x8(%rbp)
	.section .t303,"ax",@progbits
	orb %bl, 0x12345(%rcx)
	.section .t304,"ax",@progbits
	orb %bl, (%rax,%rsi,4)
	.section .t305,"ax",@progbits
	orb %bl, 0x10(%rax,%rsi,8)
	.section .t306,"ax",@progbits
	orb %bl, (,%rsi,2)
	.section .t307,"ax",@progbits
	orb %bl, 0x40(%rip)
	.section .t308,"ax",@progbits
	orb %bl, -0x100(%rip)
	.section .t309,"ax",@progbits
	orb %bl, 0x1234
	.section .t310,"ax",@progbits
	orb %bl, (%r8)
	.section .t311,"ax",@progbits
	orb %bl, (%r12)
	.section .t312,"ax",@progbits
	orb %bl, 0x8(%r13)
	.section .t313,"ax",@progbits
	orb %bl, (%r8,%r15,2)
	.section .t314,"ax",@progbits
	orb %bl, (%rax,%r12,4)
	.section .t315,"ax",@progbits
	orb %bl, 0x100(%rbp)
	.section .t316,"ax",@progbits
	orb %bl, (%rsp)
	.section .t317,"ax",@progbits
	orb %bl, 0x10(%rsp,%rsi,4)
	.section .t318,"ax",@progbits
	orb %bl, %gs:0x10(%rcx)
	.section .t319,"ax",@progbits
	orb %bl, %fs:(%rax,%rsi,8)
	.section .t320,"ax",@progbits
	orb %r9b, (%rcx)
	.section .t321,"ax",@progbits
	orb %r9b, 0x10(%rcx)
	.section .t322,"ax",@progbits
	orb %r9b, -0x8(%rbp)
	.section .t323,"ax",@progbits
	orb %r9b, 0x12345(%rcx)
	.section .t324,"ax",@progbits
	orb %r9b, (%rax,%rsi,4)
	.section .t325,"ax",@progbits
	orb %r9b, 0x10(%rax,%rsi,8)
	.section .t326,"ax",@progbits
	orb %r9b, (,%rsi,2)
	.section .t327,"ax",@progbits
	orb %r9b, 0x40(%rip)
	.section .t328,"ax",@progbits
	orb %r9b, -0x100(%rip)
	.section .t329,"ax",@progbits
	orb %r9b, 0x1234
	.section .t330,"ax",@progbits
	orb %r9b, (%r8)
	.section .t331,"ax",@progbits
	orb %r9b, (%r12)
	.section .t332,"ax",@progbits
	orb %r9b, 0x8(%r13)
	.section .t333,"ax",@progbits
	orb %r9b, (%r8,%r15,2)
	.section .t334,"ax",@progbits
	orb %r9b, (%rax,%r12,4)
	.section .t335,"ax",@progbits
	orb %r9b, 0x100(%rbp)
	.section .t336,"ax",@progbits
	orb %r9b, (%rsp)
	.section .t337,"ax",@progbits
	orb %r9b, 0x10(%rsp,%rsi,4)
	.section .t338,"ax",@progbits
	orb %r9b, %gs:0x10(%rcx)
	.section .t339,"ax",@progbits
	orb %r9b, %fs:(%rax,%rsi,8)
	.section .t340,"ax",@progbits
	orb %r13b, (%rcx)
	.section .t341,"ax",@progbits
	orb %r13b, 0x10(%rcx)
	.section .t342,"ax",@progbits
	orb %r13b, -0x8(%rbp)
	.section .t343,"ax",@progbits
	orb %r13b, 0x12345(%rcx)
	.section .t344,"ax",@progbits
	orb %r13b, (%rax,%rsi,4)
	.section .t345,"ax",@progbits
	orb %r13b, 0x10(%rax,%rsi,8)
	.section .t346,"ax",@progbits
	orb %r13b, (,%rsi,2)
	.section .t347,"ax",@progbits
	orb %r13b, 0x40(%rip)
	.section .t348,"ax",@progbits
	orb %r13b, -0x100(%rip)
	.section .t349,"ax",@progbits
	orb %r13b, 0x1234
	.section .t350,"ax",@progbits
	orb %r13b, (%r8)
	.section .t351,"ax",@progbits
	orb %r13b, (%r12)
	.section .t352,"ax",@progbits
	orb %r13b, 0x8(%r13)
	.section .t353,"ax",@progbits
	orb %r13b, (%r8,%r15,2)
	.section .t354,"ax",@progbits
	orb %r13b, (%rax,%r12,4)
	.section .t355,"ax",@progbits
	orb %r13b, 0x100(%rbp)
	.section .t356,"ax",@progbits
	orb %r13b, (%rsp)
	.section .t357,"ax",@progbits
	orb %r13b, 0x10(%rsp,%rsi,4)
	.section .t358,"ax",@progbits
	orb %r13b, %gs:0x10(%rcx)
	.section .t359,"ax",@progbits
	orb %r13b, %fs:(%rax,%rsi,8)
	.section .t360,"ax",@progbits
	orw %dx, (%rcx)
	.section .t361,"ax",@progbits
	orw %dx, 0x10(%rcx)
	.section .t362,"ax",@progbits
	orw %dx, -0x8(%rbp)
	.section .t363,"ax",@progbits
	orw %dx, 0x12345(%rcx)
	.section .t364,"ax",@progbits
	orw %dx, (%rax,%rsi,4)
	.section .t365,"ax",@progbits
	orw %dx, 0x10(%rax,%rsi,8)
	.section .t366,"ax",@progbits
	orw %dx, (,%rsi,2)
	.section .t367,"ax",@progbits
	orw %dx, 0x40(%rip)
	.section .t368,"ax",@progbits
	orw %dx, -0x100(%rip)
	.section .t369,"ax",@progbits
	orw %dx, 0x1234
	.section .t370,"ax",@progbits
	orw %dx, (%r8)
	.section .t371,"ax",@progbits
	orw %dx, (%r12)
	.section .t372,"ax",@progbits
	orw %dx, 0x8(%r13)
	.section .t373,"ax",@progbits
	orw %dx, (%r8,%r15,2)
	.section .t374,"ax",@progbits
	orw %dx, (%rax,%r12,4)
	.section .t375,"ax",@progbits
	orw %dx, 0x100(%rbp)
	.section .t376,"ax",@progbits
	orw %dx, (%rsp)
	.section .t377,"ax",@progbits
	orw %dx, 0x10(%rsp,%rsi,4)
	.section .t378,"ax",@progbits
	orw %dx, %gs:0x10(%rcx)
	.section .t379,"ax",@progbits
	orw %dx, %fs:(%rax,%rsi,8)
	.section .t380,"ax",@progbits
	orw %bx, (%rcx)
	.section .t381,"ax",@progbits
	orw %bx, 0x10(%rcx)
	.section .t382,"ax",@progbits
	orw %bx, -0x8(%rbp)
	.section .t383,"ax",@progbits
	orw %bx, 0x12345(%rcx)
	.section .t384,"ax",@progbits
	orw %bx, (%rax,%rsi,4)
	.section .t385,"ax",@progbits
	orw %bx, 0x10(%rax,%rsi,8)
	.section .t386,"ax",@progbits
	orw %bx, (,%rsi,2)
	.section .t387,"ax",@progbits
	orw %bx, 0x40(%rip)
	.section .t388,"ax",@progbits
	orw %bx, -0x100(%rip)
	.section .t389,"ax",@progbits
	orw %bx, 0x1234
	.section .t390,"ax",@progbits
	orw %bx, (%r8)
	.section .t391,"ax",@progbits
	orw %bx, (%r12)
	.section .t392,"ax",@progbits
	orw %bx, 0x8(%r13)
	.section .t393,"ax",@progbits
	orw %bx, (%r8,%r15,2)
	.section .t394,"ax",@progbits
	orw %bx, (%rax,%r12,4)
	.section .t395,"ax",@progbits
	orw %bx, 0x100(%rbp)
	.section .t396,"ax",@progbits
	orw %bx, (%rsp)
	.section .t397,"ax",@progbits
	orw %bx, 0x10(%rsp,%rsi,4)
	.section .t398,"ax",@progbits
	orw %bx, %gs:0x10(%rcx)
	.section .t399,"ax",@progbits
	orw %bx, %fs:(%rax,%rsi,8)
	.section .t400,"ax",@progbits
	orw %r9w, (%rcx)
	.section .t401,"ax",@progbits
	orw %r9w, 0x10(%rcx)
	.section .t402,"ax",@progbits
	orw %r9w, -0x8(%rbp)
	.section .t403,"ax",@progbits
	orw %r9w, 0x12345(%rcx)
	.section .t404,"ax",@progbits
	orw %r9w, (%rax,%rsi,4)
	.section .t405,"ax",@progbits
	orw %r9w, 0x10(%rax,%rsi,8)
	.section .t406,"ax",@progbits
	orw %r9w, (,%rsi,2)
	.section .t407,"ax",@progbits
	orw %r9w, 0x40(%rip)
	.section .t408,"ax",@progbits
	orw %r9w, -0x100(%rip)
	.section .t409,"ax",@progbits
	orw %r9w, 0x1234
	.section .t410,"ax",@progbits
	orw %r9w, (%r8)
	.section .t411,"ax",@progbits
	orw %r9w, (%r12)
	.section .t412,"ax",@progbits
	orw %r9w, 0x8(%r13)
	.section .t413,"ax",@progbits
	orw %r9w, (%r8,%r15,2)
	.section .t414,"ax",@progbits
	orw %r9w, (%rax,%r12,4)
	.section .t415,"ax",@progbits
	orw %r9w, 0x100(%rbp)
	.section .t416,"ax",@progbits
	orw %r9w, (%rsp)
	.section .t417,"ax",@progbits
	orw %r9w, 0x10(%rsp,%rsi,4)
	.section .t418,"ax",@progbits
	orw %r9w, %gs:0x10(%rcx)
	.section .t419,"ax",@progbits
	orw %r9w, %fs:(%rax,%rsi,8)
	.section .t420,"ax",@progbits
	orl %edx, (%rcx)
	.section .t421,"ax",@progbits
	orl %edx, 0x10(%rcx)
	.section .t422,"ax",@progbits
	orl %edx, -0x8(%rbp)
	.section .t423,"ax",@progbits
	orl %edx, 0x12345(%rcx)
	.section .t424,"ax",@progbits
	orl %edx, (%rax,%rsi,4)
	.section .t425,"ax",@progbits
	orl %edx, 0x10(%rax,%rsi,8)
	.section .t426,"ax",@progbits
	orl %edx, (,%rsi,2)
	.section .t427,"ax",@progbits
	orl %edx, 0x40(%rip)
	.section .t428,"ax",@progbits
	orl %edx, -0x100(%rip)
	.section .t429,"ax",@progbits
	orl %edx, 0x1234
	.section .t430,"ax",@progbits
	orl %edx, (%r8)
	.section .t431,"ax",@progbits
	orl %edx, (%r12)
	.section .t432,"ax",@progbits
	orl %edx, 0x8(%r13)
	.section .t433,"ax",@progbits
	orl %edx, (%r8,%r15,2)
	.section .t434,"ax",@progbits
	orl %edx, (%rax,%r12,4)
	.section .t435,"ax",@progbits
	orl %edx, 0x100(%rbp)
	.section .t436,"ax",@progbits
	orl %edx, (%rsp)
	.section .t437,"ax",@progbits
	orl %edx, 0x10(%rsp,%rsi,4)
	.section .t438,"ax",@progbits
	orl %edx, %gs:0x10(%rcx)
	.section .t439,"ax",@progbits
	orl %edx, %fs:(%rax,%rsi,8)
	.section .t440,"ax",@progbits
	orl %ebx, (%rcx)
	.section .t441,"ax",@progbits
	orl %ebx, 0x10(%rcx)
	.section .t442,"ax",@progbits
	orl %ebx, -0x8(%rbp)
	.section .t443,"ax",@progbits
	orl %ebx, 0x12345(%rcx)
	.section .t444,"ax",@progbits
	orl %ebx, (%rax,%rsi,4)
	.section .t445,"ax",@progbits
	orl %ebx, 0x10(%rax,%rsi,8)
	.section .t446,"ax",@progbits
	orl %ebx, (,%rsi,2)
	.section .t447,"ax",@progbits
	orl %ebx, 0x40(%rip)
	.section .t448,"ax",@progbits
	orl %ebx, -0x100(%rip)
	.section .t449,"ax",@progbits
	orl %ebx, 0x1234
	.section .t450,"ax",@progbits
	orl %ebx, (%r8)
	.section .t451,"ax",@progbits
	orl %ebx, (%r12)
	.section .t452,"ax",@progbits
	orl %ebx, 0x8(%r13)
	.section .t453,"ax",@progbits
	orl %ebx, (%r8,%r15,2)
	.section .t454,"ax",@progbits
	orl %ebx, (%rax,%r12,4)
	.section .t455,"ax",@progbits
	orl %ebx, 0x100(%rbp)
	.section .t456,"ax",@progbits
	orl %ebx, (%rsp)
	.section .t457,"ax",@progbits
	orl %ebx, 0x10(%rsp,%rsi,4)
	.section .t458,"ax",@progbits
	orl %ebx, %gs:0x10(%rcx)
	.section .t459,"ax",@progbits
	orl %ebx, %fs:(%rax,%rsi,8)
	.section .t460,"ax",@progbits
	orl %r9d, (%rcx)
	.section .t461,"ax",@progbits
	orl %r9d, 0x10(%rcx)
	.section .t462,"ax",@progbits
	orl %r9d, -0x8(%rbp)
	.section .t463,"ax",@progbits
	orl %r9d, 0x12345(%rcx)
	.section .t464,"ax",@progbits
	orl %r9d, (%rax,%rsi,4)
	.section .t465,"ax",@progbits
	orl %r9d, 0x10(%rax,%rsi,8)
	.section .t466,"ax",@progbits
	orl %r9d, (,%rsi,2)
	.section .t467,"ax",@progbits
	orl %r9d, 0x40(%rip)
	.section .t468,"ax",@progbits
	orl %r9d, -0x100(%rip)
	.section .t469,"ax",@progbits
	orl %r9d, 0x1234
	.section .t470,"ax",@progbits
	orl %r9d, (%r8)
	.section .t471,"ax",@progbits
	orl %r9d, (%r12)
	.section .t472,"ax",@progbits
	orl %r9d, 0x8(%r13)
	.section .t473,"ax",@progbits
	orl %r9d, (%r8,%r15,2)
	.section .t474,"ax",@progbits
	orl %r9d, (%rax,%r12,4)
	.section .t475,"ax",@progbits
	orl %r9d, 0x100(%rbp)
	.section .t476,"ax",@progbits
	orl %r9d, (%rsp)
	.section .t477,"ax",@progbits
	orl %r9d, 0x10(%rsp,%rsi,4)
	.section .t478,"ax",@progbits
	orl %r9d, %gs:0x10(%rcx)
	.section .t479,"ax",@progbits
	orl %r9d, %fs:(%rax,%rsi,8)
	.section .t480,"ax",@progbits
	orq %rdx, (%rcx)
	.section .t481,"ax",@progbits
	orq %rdx, 0x10(%rcx)
	.section .t482,"ax",@progbits
	orq %rdx, -0x8(%rbp)
	.section .t483,"ax",@progbits
	orq %rdx, 0x12345(%rcx)
	.section .t484,"ax",@progbits
	orq %rdx, (%rax,%rsi,4)
	.section .t485,"ax",@progbits
	orq %rdx, 0x10(%rax,%rsi,8)
	.section .t486,"ax",@progbits
	orq %rdx, (,%rsi,2)
	.section .t487,"ax",@progbits
	orq %rdx, 0x40(%rip)
	.section .t488,"ax",@progbits
	orq %rdx, -0x100(%rip)
	.section .t489,"ax",@progbits
	orq %rdx, 0x1234
	.section .t490,"ax",@progbits
	orq %rdx, (%r8)
	.section .t491,"ax",@progbits
	orq %rdx, (%r12)
	.section .t492,"ax",@progbits
	orq %rdx, 0x8(%r13)
	.section .t493,"ax",@progbits
	orq %rdx, (%r8,%r15,2)
	.section .t494,"ax",@progbits
	orq %rdx, (%rax,%r12,4)
	.section .t495,"ax",@progbits
	orq %rdx, 0x100(%rbp)
	.section .t496,"ax",@progbits
	orq %rdx, (%rsp)
	.section .t497,"ax",@progbits
	orq %rdx, 0x10(%rsp,%rsi,4)
	.section .t498,"ax",@progbits
	orq %rdx, %gs:0x10(%rcx)
	.section .t499,"ax",@progbits
	orq %rdx, %fs:(%rax,%rsi,8)
	.section .t500,"ax",@progbits
	orq %rbx, (%rcx)
	.section .t501,"ax",@progbits
	orq %rbx, 0x10(%rcx)
	.section .t502,"ax",@progbits
	orq %rbx, -0x8(%rbp)
	.section .t503,"ax",@progbits
	orq %rbx, 0x12345(%rcx)
	.section .t504,"ax",@progbits
	orq %rbx, (%rax,%rsi,4)
	.section .t505,"ax",@progbits
	orq %rbx, 0x10(%rax,%rsi,8)
	.section .t506,"ax",@progbits
	orq %rbx, (,%rsi,2)
	.section .t507,"ax",@progbits
	orq %rbx, 0x40(%rip)
	.section .t508,"ax",@progbits
	orq %rbx, -0x100(%rip)
	.section .t509,"ax",@progbits
	orq %rbx, 0x1234
	.section .t510,"ax",@progbits
	orq %rbx, (%r8)
	.section .t511,"ax",@progbits
	orq %rbx, (%r12)
	.section .t512,"ax",@progbits
	orq %rbx, 0x8(%r13)
	.section .t513,"ax",@progbits
	orq %rbx, (%r8,%r15,2)
	.section .t514,"ax",@progbits
	orq %rbx, (%rax,%r12,4)
	.section .t515,"ax",@progbits
	orq %rbx, 0x100(%rbp)
	.section .t516,"ax",@progbits
	orq %rbx, (%rsp)
	.section .t517,"ax",@progbits
	orq %rbx, 0x10(%rsp,%rsi,4)
	.section .t518,"ax",@progbits
	orq %rbx, %gs:0x10(%rcx)
	.section .t519,"ax",@progbits
	orq %rbx, %fs:(%rax,%rsi,8)
	.section .t520,"ax",@progbits
	orq %r9, (%rcx)
	.section .t521,"ax",@progbits
	orq %r9, 0x10(%rcx)
	.section .t522,"ax",@progbits
	orq %r9, -0x8(%rbp)
	.section .t523,"ax",@progbits
	orq %r9, 0x12345(%rcx)
	.section .t524,"ax",@progbits
	orq %r9, (%rax,%rsi,4)
	.section .t525,"ax",@progbits
	orq %r9, 0x10(%rax,%rsi,8)
	.section .t526,"ax",@progbits
	orq %r9, (,%rsi,2)
	.section .t527,"ax",@progbits
	orq %r9, 0x40(%rip)
	.section .t528,"ax",@progbits
	orq %r9, -0x100(%rip)
	.section .t529,"ax",@progbits
	orq %r9, 0x1234
	.section .t530,"ax",@progbits
	orq %r9, (%r8)
	.section .t531,"ax",@progbits
	orq %r9, (%r12)
	.section .t532,"ax",@progbits
	orq %r9, 0x8(%r13)
	.section .t533,"ax",@progbits
	orq %r9, (%r8,%r15,2)
	.section .t534,"ax",@progbits
	orq %r9, (%rax,%r12,4)
	.section .t535,"ax",@progbits
	orq %r9, 0x100(%rbp)
	.section .t536,"ax",@progbits
	orq %r9, (%rsp)
	.section .t537,"ax",@progbits
	orq %r9, 0x10(%rsp,%rsi,4)
	.section .t538,"ax",@progbits
	orq %r9, %gs:0x10(%rcx)
	.section .t539,"ax",@progbits
	orq %r9, %fs:(%rax,%rsi,8)
	.section .t540,"ax",@progbits
	orq %r12, (%rcx)
	.section .t541,"ax",@progbits
	orq %r12, 0x10(%rcx)
	.section .t542,"ax",@progbits
	orq %r12, -0x8(%rbp)
	.section .t543,"ax",@progbits
	orq %r12, 0x12345(%rcx)
	.section .t544,"ax",@progbits
	orq %r12, (%rax,%rsi,4)
	.section .t545,"ax",@progbits
	orq %r12, 0x10(%rax,%rsi,8)
	.section .t546,"ax",@progbits
	orq %r12, (,%rsi,2)
	.section .t547,"ax",@progbits
	orq %r12, 0x40(%rip)
	.section .t548,"ax",@progbits
	orq %r12, -0x100(%rip)
	.section .t549,"ax",@progbits
	orq %r12, 0x1234
	.section .t550,"ax",@progbits
	orq %r12, (%r8)
	.section .t551,"ax",@progbits
	orq %r12, (%r12)
	.section .t552,"ax",@progbits
	orq %r12, 0x8(%r13)
	.section .t553,"ax",@progbits
	orq %r12, (%r8,%r15,2)
	.section .t554,"ax",@progbits
	orq %r12, (%rax,%r12,4)
	.section .t555,"ax",@progbits
	orq %r12, 0x100(%rbp)
	.section .t556,"ax",@progbits
	orq %r12, (%rsp)
	.section .t557,"ax",@progbits
	orq %r12, 0x10(%rsp,%rsi,4)
	.section .t558,"ax",@progbits
	orq %r12, %gs:0x10(%rcx)
	.section .t559,"ax",@progbits
	orq %r12, %fs:(%rax,%rsi,8)
	.section .t560,"ax",@progbits
	andb %dl, (%rcx)
	.section .t561,"ax",@progbits
	andb %dl, 0x10(%rcx)
	.section .t562,"ax",@progbits
	andb %dl, -0x8(%rbp)
	.section .t563,"ax",@progbits
	andb %dl, 0x12345(%rcx)
	.section .t564,"ax",@progbits
	andb %dl, (%rax,%rsi,4)
	.section .t565,"ax",@progbits
	andb %dl, 0x10(%rax,%rsi,8)
	.section .t566,"ax",@progbits
	andb %dl, (,%rsi,2)
	.section .t567,"ax",@progbits
	andb %dl, 0x40(%rip)
	.section .t568,"ax",@progbits
	andb %dl, -0x100(%rip)
	.section .t569,"ax",@progbits
	andb %dl, 0x1234
	.section .t570,"ax",@progbits
	andb %dl, (%r8)
	.section .t571,"ax",@progbits
	andb %dl, (%r12)
	.section .t572,"ax",@progbits
	andb %dl, 0x8(%r13)
	.section .t573,"ax",@progbits
	andb %dl, (%r8,%r15,2)
	.section .t574,"ax",@progbits
	andb %dl, (%rax,%r12,4)
	.section .t575,"ax",@progbits
	andb %dl, 0x100(%rbp)
	.section .t576,"ax",@progbits
	andb %dl, (%rsp)
	.section .t577,"ax",@progbits
	andb %dl, 0x10(%rsp,%rsi,4)
	.section .t578,"ax",@progbits
	andb %dl, %gs:0x10(%rcx)
	.section .t579,"ax",@progbits
	andb %dl, %fs:(%rax,%rsi,8)
	.section .t580,"ax",@progbits
	andb %bl, (%rcx)
	.section .t581,"ax",@progbits
	andb %bl, 0x10(%rcx)
	.section .t582,"ax",@progbits
	andb %bl, -0x8(%rbp)
	.section .t583,"ax",@progbits
	andb %bl, 0x12345(%rcx)
	.section .t584,"ax",@progbits
	andb %bl, (%rax,%rsi,4)
	.section .t585,"ax",@progbits
	andb %bl, 0x10(%rax,%rsi,8)
	.section .t586,"ax",@progbits
	andb %bl, (,%rsi,2)
	.section .t587,"ax",@progbits
	andb %bl, 0x40(%rip)
	.section .t588,"ax",@progbits
	andb %bl, -0x100(%rip)
	.section .t589,"ax",@progbits
	andb %bl, 0x1234
	.section .t590,"ax",@progbits
	andb %bl, (%r8)
	.section .t591,"ax",@progbits
	andb %bl, (%r12)
	.section .t592,"ax",@progbits
	andb %bl, 0x8(%r13)
	.section .t593,"ax",@progbits
	andb %bl, (%r8,%r15,2)
	.section .t594,"ax",@progbits
	andb %bl, (%rax,%r12,4)
	.section .t595,"ax",@progbits
	andb %bl, 0x100(%rbp)
	.section .t596,"ax",@progbits
	andb %bl, (%rsp)
	.section .t597,"ax",@progbits
	andb %bl, 0x10(%rsp,%rsi,4)
	.section .t598,"ax",@progbits
	andb %bl, %gs:0x10(%rcx)
	.section .t599,"ax",@progbits
	andb %bl, %fs:(%rax,%rsi,8)
	.section .t600,"ax",@progbits
	andb %r9b, (%rcx)
	.section .t601,"ax",@progbits
	andb %r9b, 0x10(%rcx)
	.section .t602,"ax",@progbits
	andb %r9b, -0x8(%rbp)
	.section .t603,"ax",@progbits
	andb %r9b, 0x12345(%rcx)
	.section .t604,"ax",@progbits
	andb %r9b, (%rax,%rsi,4)
	.section .t605,"ax",@progbits
	andb %r9b, 0x10(%rax,%rsi,8)
	.section .t606,"ax",@progbits
	andb %r9b, (,%rsi,2)
	.section .t607,"ax",@progbits
	andb %r9b, 0x40(%rip)
	.section .t608,"ax",@progbits
	andb %r9b, -0x100(%rip)
	.section .t609,"ax",@progbits
	andb %r9b, 0x1234
	.section .t610,"ax",@progbits
	andb %r9b, (%r8)
	.section .t611,"ax",@progbits
	andb %r9b, (%r12)
	.section .t612,"ax",@progbits
	andb %r9b, 0x8(%r13)
	.section .t613,"ax",@progbits
	andb %r9b, (%r8,%r15,2)
	.section .t614,"ax",@progbits
	andb %r9b, (%rax,%r12,4)
	.section .t615,"ax",@progbits
	andb %r9b, 0x100(%rbp)
	.section .t616,"ax",@progbits
	andb %r9b, (%rsp)
	.section .t617,"ax",@progbits
	andb %r9b, 0x10(%rsp,%rsi,4)
	.section .t618,"ax",@progbits
	andb %r9b, %gs:0x10(%rcx)
	.section .t619,"ax",@progbits
	andb %r9b, %fs:(%rax,%rsi,8)
	.section .t620,"ax",@progbits
	andb %r13b, (%rcx)
	.section .t621,"ax",@progbits
	andb %r13b, 0x10(%rcx)
	.section .t622,"ax",@progbits
	andb %r13b, -0x8(%rbp)
	.section .t623,"ax",@progbits
	andb %r13b, 0x12345(%rcx)
	.section .t624,"ax",@progbits
	andb %r13b, (%rax,%rsi,4)
	.section .t625,"ax",@progbits
	andb %r13b, 0x10(%rax,%rsi,8)
	.section .t626,"ax",@progbits
	andb %r13b, (,%rsi,2)
	.section .t627,"ax",@progbits
	andb %r13b, 0x40(%rip)
	.section .t628,"ax",@progbits
	andb %r13b, -0x100(%rip)
	.section .t629,"ax",@progbits
	andb %r13b, 0x1234
	.section .t630,"ax",@progbits
	andb %r13b, (%r8)
	.section .t631,"ax",@progbits
	andb %r13b, (%r12)
	.section .t632,"ax",@progbits
	andb %r13b, 0x8(%r13)
	.section .t633,"ax",@progbits
	andb %r13b, (%r8,%r15,2)
	.section .t634,"ax",@progbits
	andb %r13b, (%rax,%r12,4)
	.section .t635,"ax",@progbits
	andb %r13b, 0x100(%rbp)
	.section .t636,"ax",@progbits
	andb %r13b, (%rsp)
	.section .t637,"ax",@progbits
	andb %r13b, 0x10(%rsp,%rsi,4)
	.section .t638,"ax",@progbits
	andb %r13b, %gs:0x10(%rcx)
	.section .t639,"ax",@progbits
	andb %r13b, %fs:(%rax,%rsi,8)
	.section .t640,"ax",@progbits
	andw %dx, (%rcx)
	.section .t641,"ax",@progbits
	andw %dx, 0x10(%rcx)
	.section .t642,"ax",@progbits
	andw %dx, -0x8(%rbp)
	.section .t643,"ax",@progbits
	andw %dx, 0x12345(%rcx)
	.section .t644,"ax",@progbits
	andw %dx, (%rax,%rsi,4)
	.section .t645,"ax",@progbits
	andw %dx, 0x10(%rax,%rsi,8)
	.section .t646,"ax",@progbits
	andw %dx, (,%rsi,2)
	.section .t647,"ax",@progbits
	andw %dx, 0x40(%rip)
	.section .t648,"ax",@progbits
	andw %dx, -0x100(%rip)
	.section .t649,"ax",@progbits
	andw %dx, 0x1234
	.section .t650,"ax",@progbits
	andw %dx, (%r8)
	.section .t651,"ax",@progbits
	andw %dx, (%r12)
	.section .t652,"ax",@progbits
	andw %dx, 0x8(%r13)
	.section .t653,"ax",@progbits
	andw %dx, (%r8,%r15,2)
	.section .t654,"ax",@progbits
	andw %dx, (%rax,%r12,4)
	.section .t655,"ax",@progbits
	andw %dx, 0x100(%rbp)
	.section .t656,"ax",@progbits
	andw %dx, (%rsp)
	.section .t657,"ax",@progbits
	andw %dx, 0x10(%rsp,%rsi,4)
	.section .t658,"ax",@progbits
	andw %dx, %gs:0x10(%rcx)
	.section .t659,"ax",@progbits
	andw %dx, %fs:(%rax,%rsi,8)
	.section .t660,"ax",@progbits
	andw %bx, (%rcx)
	.section .t661,"ax",@progbits
	andw %bx, 0x10(%rcx)
	.section .t662,"ax",@progbits
	andw %bx, -0x8(%rbp)
	.section .t663,"ax",@progbits
	andw %bx, 0x12345(%rcx)
	.section .t664,"ax",@progbits
	andw %bx, (%rax,%rsi,4)
	.section .t665,"ax",@progbits
	andw %bx, 0x10(%rax,%rsi,8)
	.section .t666,"ax",@progbits
	andw %bx, (,%rsi,2)
	.section .t667,"ax",@progbits
	andw %bx, 0x40(%rip)
	.section .t668,"ax",@progbits
	andw %bx, -0x100(%rip)
	.section .t669,"ax",@progbits
	andw %bx, 0x1234
	.section .t670,"ax",@progbits
	andw %bx, (%r8)
	.section .t671,"ax",@progbits
	andw %bx, (%r12)
	.section .t672,"ax",@progbits
	andw %bx, 0x8(%r13)
	.section .t673,"ax",@progbits
	andw %bx, (%r8,%r15,2)
	.section .t674,"ax",@progbits
	andw %bx, (%rax,%r12,4)
	.section .t675,"ax",@progbits
	andw %bx, 0x100(%rbp)
	.section .t676,"ax",@progbits
	andw %bx, (%rsp)
	.section .t677,"ax",@progbits
	andw %bx, 0x10(%rsp,%rsi,4)
	.section .t678,"ax",@progbits
	andw %bx, %gs:0x10(%rcx)
	.section .t679,"ax",@progbits
	andw %bx, %fs:(%rax,%rsi,8)
	.section .t680,"ax",@progbits
	andw %r9w, (%rcx)
	.section .t681,"ax",@progbits
	andw %r9w, 0x10(%rcx)
	.section .t682,"ax",@progbits
	andw %r9w, -0x8(%rbp)
	.section .t683,"ax",@progbits
	andw %r9w, 0x12345(%rcx)
	.section .t684,"ax",@progbits
	andw %r9w, (%rax,%rsi,4)
	.section .t685,"ax",@progbits
	andw %r9w, 0x10(%rax,%rsi,8)
	.section .t686,"ax",@progbits
	andw %r9w, (,%rsi,2)
	.section .t687,"ax",@progbits
	andw %r9w, 0x40(%rip)
	.section .t688,"ax",@progbits
	andw %r9w, -0x100(%rip)
	.section .t689,"ax",@progbits
	andw %r9w, 0x1234
	.section .t690,"ax",@progbits
	andw %r9w, (%r8)
	.section .t691,"ax",@progbits
	andw %r9w, (%r12)
	.section .t692,"ax",@progbits
	andw %r9w, 0x8(%r13)
	.section .t693,"ax",@progbits
	andw %r9w, (%r8,%r15,2)
	.section .t694,"ax",@progbits
	andw %r9w, (%rax,%r12,4)
	.section .t695,"ax",@progbits
	andw %r9w, 0x100(%rbp)
	.section .t696,"ax",@progbits
	andw %r9w, (%rsp)
	.section .t697,"ax",@progbits
	andw %r9w, 0x10(%rsp,%rsi,4)
	.section .t698,"ax",@progbits
	andw %r9w, %gs:0x10(%rcx)
	.section .t699,"ax",@progbits
	andw %r9w, %fs:(%rax,%rsi,8)
	.section .t700,"ax",@progbits
	andl %edx, (%rcx)
	.section .t701,"ax",@progbits
	andl %edx, 0x10(%rcx)
	.section .t702,"ax",@progbits
	andl %edx, -0x8(%rbp)
	.section .t703,"ax",@progbits
	andl %edx, 0x12345(%rcx)
	.section .t704,"ax",@progbits
	andl %edx, (%rax,%rsi,4)
	.section .t705,"ax",@progbits
	andl %edx, 0x10(%rax,%rsi,8)
	.section .t706,"ax",@progbits
	andl %edx, (,%rsi,2)
	.section .t707,"ax",@progbits
	andl %edx, 0x40(%rip)
	.section .t708,"ax",@progbits
	andl %edx, -0x100(%rip)
	.section .t709,"ax",@progbits
	andl %edx, 0x1234
	.section .t710,"ax",@progbits
	andl %edx, (%r8)
	.section .t711,"ax",@progbits
	andl %edx, (%r12)
	.section .t712,"ax",@progbits
	andl %edx, 0x8(%r13)
	.section .t713,"ax",@progbits
	andl %edx, (%r8,%r15,2)
	.section .t714,"ax",@progbits
	andl %edx, (%rax,%r12,4)
	.section .t715,"ax",@progbits
	andl %edx, 0x100(%rbp)
	.section .t716,"ax",@progbits
	andl %edx, (%rsp)
	.section .t717,"ax",@progbits
	andl %edx, 0x10(%rsp,%rsi,4)
	.section .t718,"ax",@progbits
	andl %edx, %gs:0x10(%rcx)
	.section .t719,"ax",@progbits
	andl %edx, %fs:(%rax,%rsi,8)
	.section .t720,"ax",@progbits
	andl %ebx, (%rcx)
	.section .t721,"ax",@progbits
	andl %ebx, 0x10(%rcx)
	.section .t722,"ax",@progbits
	andl %ebx, -0x8(%rbp)
	.section .t723,"ax",@progbits
	andl %ebx, 0x12345(%rcx)
	.section .t724,"ax",@progbits
	andl %ebx, (%rax,%rsi,4)
	.section .t725,"ax",@progbits
	andl %ebx, 0x10(%rax,%rsi,8)
	.section .t726,"ax",@progbits
	andl %ebx, (,%rsi,2)
	.section .t727,"ax",@progbits
	andl %ebx, 0x40(%rip)
	.section .t728,"ax",@progbits
	andl %ebx, -0x100(%rip)
	.section .t729,"ax",@progbits
	andl %ebx, 0x1234
	.section .t730,"ax",@progbits
	andl %ebx, (%r8)
	.section .t731,"ax",@progbits
	andl %ebx, (%r12)
	.section .t732,"ax",@progbits
	andl %ebx, 0x8(%r13)
	.section .t733,"ax",@progbits
	andl %ebx, (%r8,%r15,2)
	.section .t734,"ax",@progbits
	andl %ebx, (%rax,%r12,4)
	.section .t735,"ax",@progbits
	andl %ebx, 0x100(%rbp)
	.section .t736,"ax",@progbits
	andl %ebx, (%rsp)
	.section .t737,"ax",@progbits
	andl %ebx, 0x10(%rsp,%rsi,4)
	.section .t738,"ax",@progbits
	andl %ebx, %gs:0x10(%rcx)
	.section .t739,"ax",@progbits
	andl %ebx, %fs:(%rax,%rsi,8)
	.section .t740,"ax",@progbits
	andl %r9d, (%rcx)
	.section .t741,"ax",@progbits
	andl %r9d, 0x10(%rcx)
	.section .t742,"ax",@progbits
	andl %r9d, -0x8(%rbp)
	.section .t743,"ax",@progbits
	andl %r9d, 0x12345(%rcx)
	.section .t744,"ax",@progbits
	andl %r9d, (%rax,%rsi,4)
	.section .t745,"ax",@progbits
	andl %r9d, 0x10(%rax,%rsi,8)
	.section .t746,"ax",@progbits
	andl %r9d, (,%rsi,2)
	.section .t747,"ax",@progbits
	andl %r9d, 0x40(%rip)
	.section .t748,"ax",@progbits
	andl %r9d, -0x100(%rip)
	.section .t749,"ax",@progbits
	andl %r9d, 0x1234
	.section .t750,"ax",@progbits
	andl %r9d, (%r8)
	.section .t751,"ax",@progbits
	andl %r9d, (%r12)
	.section .t752,"ax",@progbits
	andl %r9d, 0x8(%r13)
	.section .t753,"ax",@progbits
	andl %r9d, (%r8,%r15,2)
	.section .t754,"ax",@progbits
	andl %r9d, (%rax,%r12,4)
	.section .t755,"ax",@progbits
	andl %r9d, 0x100(%rbp)
	.section .t756,"ax",@progbits
	andl %r9d, (%rsp)
	.section .t757,"ax",@progbits
	andl %r9d, 0x10(%rsp,%rsi,4)
	.section .t758,"ax",@progbits
	andl %r9d, %gs:0x10(%rcx)
	.section .t759,"ax",@progbits
	andl %r9d, %fs:(%rax,%rsi,8)
	.section .t760,"ax",@progbits
	andq %rdx, (%rcx)
	.section .t761,"ax",@progbits
	andq %rdx, 0x10(%rcx)
	.section .t762,"ax",@progbits
	andq %rdx, -0x8(%rbp)
	.section .t763,"ax",@progbits
	andq %rdx, 0x12345(%rcx)
	.section .t764,"ax",@progbits
	andq %rdx, (%rax,%rsi,4)
	.section .t765,"ax",@progbits
	andq %rdx, 0x10(%rax,%rsi,8)
	.section .t766,"ax",@progbits
	andq %rdx, (,%rsi,2)
	.section .t767,"ax",@progbits
	andq %rdx, 0x40(%rip)
	.section .t768,"ax",@progbits
	andq %rdx, -0x100(%rip)
	.section .t769,"ax",@progbits
	andq %rdx, 0x1234
	.section .t770,"ax",@progbits
	andq %rdx, (%r8)
	.section .t771,"ax",@progbits
	andq %rdx, (%r12)
	.section .t772,"ax",@progbits
	andq %rdx, 0x8(%r13)
	.section .t773,"ax",@progbits
	andq %rdx, (%r8,%r15,2)
	.section .t774,"ax",@progbits
	andq %rdx, (%rax,%r12,4)
	.section .t775,"ax",@progbits
	andq %rdx, 0x100(%rbp)
	.section .t776,"ax",@progbits
	andq %rdx, (%rsp)
	.section .t777,"ax",@progbits
	andq %rdx, 0x10(%rsp,%rsi,4)
	.section .t778,"ax",@progbits
	andq %rdx, %gs:0x10(%rcx)
	.section .t779,"ax",@progbits
	andq %rdx, %fs:(%rax,%rsi,8)
	.section .t780,"ax",@progbits
	andq %rbx, (%rcx)
	.section .t781,"ax",@progbits
	andq %rbx, 0x10(%rcx)
	.section .t782,"ax",@progbits
	andq %rbx, -0x8(%rbp)
	.section .t783,"ax",@progbits
	andq %rbx, 0x12345(%rcx)
	.section .t784,"ax",@progbits
	andq %rbx, (%rax,%rsi,4)
	.section .t785,"ax",@progbits
	andq %rbx, 0x10(%rax,%rsi,8)
	.section .t786,"ax",@progbits
	andq %rbx, (,%rsi,2)
	.section .t787,"ax",@progbits
	andq %rbx, 0x40(%rip)
	.section .t788,"ax",@progbits
	andq %rbx, -0x100(%rip)
	.section .t789,"ax",@progbits
	andq %rbx, 0x1234
	.section .t790,"ax",@progbits
	andq %rbx, (%r8)
	.section .t791,"ax",@progbits
	andq %rbx, (%r12)
	.section .t792,"ax",@progbits
	andq %rbx, 0x8(%r13)
	.section .t793,"ax",@progbits
	andq %rbx, (%r8,%r15,2)
	.section .t794,"ax",@progbits
	andq %rbx, (%rax,%r12,4)
	.section .t795,"ax",@progbits
	andq %rbx, 0x100(%rbp)
	.section .t796,"ax",@progbits
	andq %rbx, (%rsp)
	.section .t797,"ax",@progbits
	andq %rbx, 0x10(%rsp,%rsi,4)
	.section .t798,"ax",@progbits
	andq %rbx, %gs:0x10(%rcx)
	.section .t799,"ax",@progbits
	andq %rbx, %fs:(%rax,%rsi,8)
	.section .t800,"ax",@progbits
	andq %r9, (%rcx)
	.section .t801,"ax",@progbits
	andq %r9, 0x10(%rcx)
	.section .t802,"ax",@progbits
	andq %r9, -0x8(%rbp)
	.section .t803,"ax",@progbits
	andq %r9, 0x12345(%rcx)
	.section .t804,"ax",@progbits
	andq %r9, (%rax,%rsi,4)
	.section .t805,"ax",@progbits
	andq %r9, 0x10(%rax,%rsi,8)
	.section .t806,"ax",@progbits
	andq %r9, (,%rsi,2)
	.section .t807,"ax",@progbits
	andq %r9, 0x40(%rip)
	.section .t808,"ax",@progbits
	andq %r9, -0x100(%rip)
	.section .t809,"ax",@progbits
	andq %r9, 0x1234
	.section .t810,"ax",@progbits
	andq %r9, (%r8)
	.section .t811,"ax",@progbits
	andq %r9, (%r12)
	.section .t812,"ax",@progbits
	andq %r9, 0x8(%r13)
	.section .t813,"ax",@progbits
	andq %r9, (%r8,%r15,2)
	.section .t814,"ax",@progbits
	andq %r9, (%rax,%r12,4)
	.section .t815,"ax",@progbits
	andq %r9, 0x100(%rbp)
	.section .t816,"ax",@progbits
	andq %r9, (%rsp)
	.section .t817,"ax",@progbits
	andq %r9, 0x10(%rsp,%rsi,4)
	.section .t818,"ax",@progbits
	andq %r9, %gs:0x10(%rcx)
	.section .t819,"ax",@progbits
	andq %r9, %fs:(%rax,%rsi,8)
	.section .t820,"ax",@progbits
	andq %r12, (%rcx)
	.section .t821,"ax",@progbits
	andq %r12, 0x10(%rcx)
	.section .t822,"ax",@progbits
	andq %r12, -0x8(%rbp)
	.section .t823,"ax",@progbits
	andq %r12, 0x12345(%rcx)
	.section .t824,"ax",@progbits
	andq %r12, (%rax,%rsi,4)
	.section .t825,"ax",@progbits
	andq %r12, 0x10(%rax,%rsi,8)
	.section .t826,"ax",@progbits
	andq %r12, (,%rsi,2)
	.section .t827,"ax",@progbits
	andq %r12, 0x40(%rip)
	.section .t828,"ax",@progbits
	andq %r12, -0x100(%rip)
	.section .t829,"ax",@progbits
	andq %r12, 0x1234
	.section .t830,"ax",@progbits
	andq %r12, (%r8)
	.section .t831,"ax",@progbits
	andq %r12, (%r12)
	.section .t832,"ax",@progbits
	andq %r12, 0x8(%r13)
	.section .t833,"ax",@progbits
	andq %r12, (%r8,%r15,2)
	.section .t834,"ax",@progbits
	andq %r12, (%rax,%r12,4)
	.section .t835,"ax",@progbits
	andq %r12, 0x100(%rbp)
	.section .t836,"ax",@progbits
	andq %r12, (%rsp)
	.section .t837,"ax",@progbits
	andq %r12, 0x10(%rsp,%rsi,4)
	.section .t838,"ax",@progbits
	andq %r12, %gs:0x10(%rcx)
	.section .t839,"ax",@progbits
	andq %r12, %fs:(%rax,%rsi,8)
	.section .t840,"ax",@progbits
	subb %dl, (%rcx)
	.section .t841,"ax",@progbits
	subb %dl, 0x10(%rcx)
	.section .t842,"ax",@progbits
	subb %dl, -0x8(%rbp)
	.section .t843,"ax",@progbits
	subb %dl, 0x12345(%rcx)
	.section .t844,"ax",@progbits
	subb %dl, (%rax,%rsi,4)
	.section .t845,"ax",@progbits
	subb %dl, 0x10(%rax,%rsi,8)
	.section .t846,"ax",@progbits
	subb %dl, (,%rsi,2)
	.section .t847,"ax",@progbits
	subb %dl, 0x40(%rip)
	.section .t848,"ax",@progbits
	subb %dl, -0x100(%rip)
	.section .t849,"ax",@progbits
	subb %dl, 0x1234
	.section .t850,"ax",@progbits
	subb %dl, (%r8)
	.section .t851,"ax",@progbits
	subb %dl, (%r12)
	.section .t852,"ax",@progbits
	subb %dl, 0x8(%r13)
	.section .t853,"ax",@progbits
	subb %dl, (%r8,%r15,2)
	.section .t854,"ax",@progbits
	subb %dl, (%rax,%r12,4)
	.section .t855,"ax",@progbits
	subb %dl, 0x100(%rbp)
	.section .t856,"ax",@progbits
	subb %dl, (%rsp)
	.section .t857,"ax",@progbits
	subb %dl, 0x10(%rsp,%rsi,4)
	.section .t858,"ax",@progbits
	subb %dl, %gs:0x10(%rcx)
	.section .t859,"ax",@progbits
	subb %dl, %fs:(%rax,%rsi,8)
	.section .t860,"ax",@progbits
	subb %bl, (%rcx)
	.section .t861,"ax",@progbits
	subb %bl, 0x10(%rcx)
	.section .t862,"ax",@progbits
	subb %bl, -0x8(%rbp)
	.section .t863,"ax",@progbits
	subb %bl, 0x12345(%rcx)
	.section .t864,"ax",@progbits
	subb %bl, (%rax,%rsi,4)
	.section .t865,"ax",@progbits
	subb %bl, 0x10(%rax,%rsi,8)
	.section .t866,"ax",@progbits
	subb %bl, (,%rsi,2)
	.section .t867,"ax",@progbits
	subb %bl, 0x40(%rip)
	.section .t868,"ax",@progbits
	subb %bl, -0x100(%rip)
	.section .t869,"ax",@progbits
	subb %bl, 0x1234
	.section .t870,"ax",@progbits
	subb %bl, (%r8)
	.section .t871,"ax",@progbits
	subb %bl, (%r12)
	.section .t872,"ax",@progbits
	subb %bl, 0x8(%r13)
	.section .t873,"ax",@progbits
	subb %bl, (%r8,%r15,2)
	.section .t874,"ax",@progbits
	subb %bl, (%rax,%r12,4)
	.section .t875,"ax",@progbits
	subb %bl, 0x100(%rbp)
	.section .t876,"ax",@progbits
	subb %bl, (%rsp)
	.section .t877,"ax",@progbits
	subb %bl, 0x10(%rsp,%rsi,4)
	.section .t878,"ax",@progbits
	subb %bl, %gs:0x10(%rcx)
	.section .t879,"ax",@progbits
	subb %bl, %fs:(%rax,%rsi,8)
	.section .t880,"ax",@progbits
	subb %r9b, (%rcx)
	.section .t881,"ax",@progbits
	subb %r9b, 0x10(%rcx)
	.section .t882,"ax",@progbits
	subb %r9b, -0x8(%rbp)
	.section .t883,"ax",@progbits
	subb %r9b, 0x12345(%rcx)
	.section .t884,"ax",@progbits
	subb %r9b, (%rax,%rsi,4)
	.section .t885,"ax",@progbits
	subb %r9b, 0x10(%rax,%rsi,8)
	.section .t886,"ax",@progbits
	subb %r9b, (,%rsi,2)
	.section .t887,"ax",@progbits
	subb %r9b, 0x40(%rip)
	.section .t888,"ax",@progbits
	subb %r9b, -0x100(%rip)
	.section .t889,"ax",@progbits
	subb %r9b, 0x1234
	.section .t890,"ax",@progbits
	subb %r9b, (%r8)
	.section .t891,"ax",@progbits
	subb %r9b, (%r12)
	.section .t892,"ax",@progbits
	subb %r9b, 0x8(%r13)
	.section .t893,"ax",@progbits
	subb %r9b, (%r8,%r15,2)
	.section .t894,"ax",@progbits
	subb %r9b, (%rax,%r12,4)
	.section .t895,"ax",@progbits
	subb %r9b, 0x100(%rbp)
	.section .t896,"ax",@progbits
	subb %r9b, (%rsp)
	.section .t897,"ax",@progbits
	subb %r9b, 0x10(%rsp,%rsi,4)
	.section .t898,"ax",@progbits
	subb %r9b, %gs:0x10(%rcx)
	.section .t899,"ax",@progbits
	subb %r9b, %fs:(%rax,%rsi,8)
	.section .t900,"ax",@progbits
	subb %r13b, (%rcx)
	.section .t901,"ax",@progbits
	subb %r13b, 0x10(%rcx)
	.section .t902,"ax",@progbits
	subb %r13b, -0x8(%rbp)
	.section .t903,"ax",@progbits
	subb %r13b, 0x12345(%rcx)
	.section .t904,"ax",@progbits
	subb %r13b, (%rax,%rsi,4)
	.section .t905,"ax",@progbits
	subb %r13b, 0x10(%rax,%rsi,8)
	.section .t906,"ax",@progbits
	subb %r13b, (,%rsi,2)
	.section .t907,"ax",@progbits
	subb %r13b, 0x40(%rip)
	.section .t908,"ax",@progbits
	subb %r13b, -0x100(%rip)
	.section .t909,"ax",@progbits
	subb %r13b, 0x1234
	.section .t910,"ax",@progbits
	subb %r13b, (%r8)
	.section .t911,"ax",@progbits
	subb %r13b, (%r12)
	.section .t912,"ax",@progbits
	subb %r13b, 0x8(%r13)
	.section .t913,"ax",@progbits
	subb %r13b, (%r8,%r15,2)
	.section .t914,"ax",@progbits
	subb %r13b, (%rax,%r12,4)
	.section .t915,"ax",@progbits
	subb %r13b, 0x100(%rbp)
	.section .t916,"ax",@progbits
	subb %r13b, (%rsp)
	.section .t917,"ax",@progbits
	subb %r13b, 0x10(%rsp,%rsi,4)
	.section .t918,"ax",@progbits
	subb %r13b, %gs:0x10(%rcx)
	.section .t919,"ax",@progbits
	subb %r13b, %fs:(%rax,%rsi,8)
	.section .t920,"ax",@progbits
	subw %dx, (%rcx)
	.section .t921,"ax",@progbits
	subw %dx, 0x10(%rcx)
	.section .t922,"ax",@progbits
	subw %dx, -0x8(%rbp)
	.section .t923,"ax",@progbits
	subw %dx, 0x12345(%rcx)
	.section .t924,"ax",@progbits
	subw %dx, (%rax,%rsi,4)
	.section .t925,"ax",@progbits
	subw %dx, 0x10(%rax,%rsi,8)
	.section .t926,"ax",@progbits
	subw %dx, (,%rsi,2)
	.section .t927,"ax",@progbits
	subw %dx, 0x40(%rip)
	.section .t928,"ax",@progbits
	subw %dx, -0x100(%rip)
	.section .t929,"ax",@progbits
	subw %dx, 0x1234
	.section .t930,"ax",@progbits
	subw %dx, (%r8)
	.section .t931,"ax",@progbits
	subw %dx, (%r12)
	.section .t932,"ax",@progbits
	subw %dx, 0x8(%r13)
	.section .t933,"ax",@progbits
	subw %dx, (%r8,%r15,2)
	.section .t934,"ax",@progbits
	subw %dx, (%rax,%r12,4)
	.section .t935,"ax",@progbits
	subw %dx, 0x100(%rbp)
	.section .t936,"ax",@progbits
	subw %dx, (%rsp)
	.section .t937,"ax",@progbits
	subw %dx, 0x10(%rsp,%rsi,4)
	.section .t938,"ax",@progbits
	subw %dx, %gs:0x10(%rcx)
	.section .t939,"ax",@progbits
	subw %dx, %fs:(%rax,%rsi,8)
	.section .t940,"ax",@progbits
	subw %bx, (%rcx)
	.section .t941,"ax",@progbits
	subw %bx, 0x10(%rcx)
	.section .t942,"ax",@progbits
	subw %bx, -0x8(%rbp)
	.section .t943,"ax",@progbits
	subw %bx, 0x12345(%rcx)
	.section .t944,"ax",@progbits
	subw %bx, (%rax,%rsi,4)
	.section .t945,"ax",@progbits
	subw %bx, 0x10(%rax,%rsi,8)
	.section .t946,"ax",@progbits
	subw %bx, (,%rsi,2)
	.section .t947,"ax",@progbits
	subw %bx, 0x40(%rip)
	.section .t948,"ax",@progbits
	subw %bx, -0x100(%rip)
	.section .t949,"ax",@progbits
	subw %bx, 0x1234
	.section .t950,"ax",@progbits
	subw %bx, (%r8)
	.section .t951,"ax",@progbits
	subw %bx, (%r12)
	.section .t952,"ax",@progbits
	subw %bx, 0x8(%r13)
	.section .t953,"ax",@progbits
	subw %bx, (%r8,%r15,2)
	.section .t954,"ax",@progbits
	subw %bx, (%rax,%r12,4)
	.section .t955,"ax",@progbits
	subw %bx, 0x100(%rbp)
	.section .t956,"ax",@progbits
	subw %bx, (%rsp)
	.section .t957,"ax",@progbits
	subw %bx, 0x10(%rsp,%rsi,4)
	.section .t958,"ax",@progbits
	subw %bx, %gs:0x10(%rcx)
	.section .t959,"ax",@progbits
	subw %bx, %fs:(%rax,%rsi,8)
	.section .t960,"ax",@progbits
	subw %r9w, (%rcx)
	.section .t961,"ax",@progbits
	subw %r9w, 0x10(%rcx)
	.section .t962,"ax",@progbits
	subw %r9w, -0x8(%rbp)
	.section .t963,"ax",@progbits
	subw %r9w, 0x12345(%rcx)
	.section .t964,"ax",@progbits
	subw %r9w, (%rax,%rsi,4)
	.section .t965,"ax",@progbits
	subw %r9w, 0x10(%rax,%rsi,8)
	.section .t966,"ax",@progbits
	subw %r9w, (,%rsi,2)
	.section .t967,"ax",@progbits
	subw %r9w, 0x40(%rip)
	.section .t968,"ax",@progbits
	subw %r9w, -0x100(%rip)
	.section .t969,"ax",@progbits
	subw %r9w, 0x1234
	.section .t970,"ax",@progbits
	subw %r9w, (%r8)
	.section .t971,"ax",@progbits
	subw %r9w, (%r12)
	.section .t972,"ax",@progbits
	subw %r9w, 0x8(%r13)
	.section .t973,"ax",@progbits
	subw %r9w, (%r8,%r15,2)
	.section .t974,"ax",@progbits
	subw %r9w, (%rax,%r12,4)
	.section .t975,"ax",@progbits
	subw %r9w, 0x100(%rbp)
	.section .t976,"ax",@progbits
	subw %r9w, (%rsp)
	.section .t977,"ax",@progbits
	subw %r9w, 0x10(%rsp,%rsi,4)
	.section .t978,"ax",@progbits
	subw %r9w, %gs:0x10(%rcx)
	.section .t979,"ax",@progbits
	subw %r9w, %fs:(%rax,%rsi,8)
	.section .t980,"ax",@progbits
	subl %edx, (%rcx)
	.section .t981,"ax",@progbits
	subl %edx, 0x10(%rcx)
	.section .t982,"ax",@progbits
	subl %edx, -0x8(%rbp)
	.section .t983,"ax",@progbits
	subl %edx, 0x12345(%rcx)
	.section .t984,"ax",@progbits
	subl %edx, (%rax,%rsi,4)
	.section .t985,"ax",@progbits
	subl %edx, 0x10(%rax,%rsi,8)
	.section .t986,"ax",@progbits
	subl %edx, (,%rsi,2)
	.section .t987,"ax",@progbits
	subl %edx, 0x40(%rip)
	.section .t988,"ax",@progbits
	subl %edx, -0x100(%rip)
	.section .t989,"ax",@progbits
	subl %edx, 0x1234
	.section .t990,"ax",@progbits
	subl %edx, (%r8)
	.section .t991,"ax",@progbits
	subl %edx, (%r12)
	.section .t992,"ax",@progbits
	subl %edx, 0x8(%r13)
	.section .t993,"ax",@progbits
	subl %edx, (%r8,%r15,2)
	.section .t994,"ax",@progbits
	subl %edx, (%rax,%r12,4)
	.section .t995,"ax",@progbits
	subl %edx, 0x100(%rbp)
	.section .t996,"ax",@progbits
	subl %edx, (%rsp)
	.section .t997,"ax",@progbits
	subl %edx, 0x10(%rsp,%rsi,4)
	.section .t998,"ax",@progbits
	subl %edx, %gs:0x10(%rcx)
	.section .t999,"ax",@progbits
	subl %edx, %fs:(%rax,%rsi,8)
	.section .t1000,"ax",@progbits
	subl %ebx, (%rcx)
	.section .t1001,"ax",@progbits
	subl %ebx, 0x10(%rcx)
	.section .t1002,"ax",@progbits
	subl %ebx, -0x8(%rbp)
	.section .t1003,"ax",@progbits
	subl %ebx, 0x12345(%rcx)
	.section .t1004,"ax",@progbits
	subl %ebx, (%rax,%rsi,4)
	.section .t1005,"ax",@progbits
	subl %ebx, 0x10(%rax,%rsi,8)
	.section .t1006,"ax",@progbits
	subl %ebx, (,%rsi,2)
	.section .t1007,"ax",@progbits
	subl %ebx, 0x40(%rip)
	.section .t1008,"ax",@progbits
	subl %ebx, -0x100(%rip)
	.section .t1009,"ax",@progbits
	subl %ebx, 0x1234
	.section .t1010,"ax",@progbits
	subl %ebx, (%r8)
	.section .t1011,"ax",@progbits
	subl %ebx, (%r12)
	.section .t1012,"ax",@progbits
	subl %ebx, 0x8(%r13)
	.section .t1013,"ax",@progbits
	subl %ebx, (%r8,%r15,2)
	.section .t1014,"ax",@progbits
	subl %ebx, (%rax,%r12,4)
	.section .t1015,"ax",@progbits
	subl %ebx, 0x100(%rbp)
	.section .t1016,"ax",@progbits
	subl %ebx, (%rsp)
	.section .t1017,"ax",@progbits
	subl %ebx, 0x10(%rsp,%rsi,4)
	.section .t1018,"ax",@progbits
	subl %ebx, %gs:0x10(%rcx)
	.section .t1019,"ax",@progbits
	subl %ebx, %fs:(%rax,%rsi,8)
	.section .t1020,"ax",@progbits
	subl %r9d, (%rcx)
	.section .t1021,"ax",@progbits
	subl %r9d, 0x10(%rcx)
	.section .t1022,"ax",@progbits
	subl %r9d, -0x8(%rbp)
	.section .t1023,"ax",@progbits
	subl %r9d, 0x12345(%rcx)
	.section .t1024,"ax",@progbits
	subl %r9d, (%rax,%rsi,4)
	.section .t1025,"ax",@progbits
	subl %r9d, 0x10(%rax,%rsi,8)
	.section .t1026,"ax",@progbits
	subl %r9d, (,%rsi,2)
	.section .t1027,"ax",@progbits
	subl %r9d, 0x40(%rip)
	.section .t1028,"ax",@progbits
	subl %r9d, -0x100(%rip)
	.section .t1029,"ax",@progbits
	subl %r9d, 0x1234
	.section .t1030,"ax",@progbits
	subl %r9d, (%r8)
	.section .t1031,"ax",@progbits
	subl %r9d, (%r12)
	.section .t1032,"ax",@progbits
	subl %r9d, 0x8(%r13)
	.section .t1033,"ax",@progbits
	subl %r9d, (%r8,%r15,2)
	.section .t1034,"ax",@progbits
	subl %r9d, (%rax,%r12,4)
	.section .t1035,"ax",@progbits
	subl %r9d, 0x100(%rbp)
	.section .t1036,"ax",@progbits
	subl %r9d, (%rsp)
	.section .t1037,"ax",@progbits
	subl %r9d, 0x10(%rsp,%rsi,4)
	.section .t1038,"ax",@progbits
	subl %r9d, %gs:0x10(%rcx)
	.section .t1039,"ax",@progbits
	subl %r9d, %fs:(%rax,%rsi,8)
	.section .t1040,"ax",@progbits
	subq %rdx, (%rcx)
	.section .t1041,"ax",@progbits
	subq %rdx, 0x10(%rcx)
	.section .t1042,"ax",@progbits
	subq %rdx, -0x8(%rbp)
	.section .t1043,"ax",@progbits
	subq %rdx, 0x12345(%rcx)
	.section .t1044,"ax",@progbits
	subq %rdx, (%rax,%rsi,4)
	.section .t1045,"ax",@progbits
	subq %rdx, 0x10(%rax,%rsi,8)
	.section .t1046,"ax",@progbits
	subq %rdx, (,%rsi,2)
	.section .t1047,"ax",@progbits
	subq %rdx, 0x40(%rip)
	.section .t1048,"ax",@progbits
	subq %rdx, -0x100(%rip)
	.section .t1049,"ax",@progbits
	subq %rdx, 0x1234
	.section .t1050,"ax",@progbits
	subq %rdx, (%r8)
	.section .t1051,"ax",@progbits
	subq %rdx, (%r12)
	.section .t1052,"ax",@progbits
	subq %rdx, 0x8(%r13)
	.section .t1053,"ax",@progbits
	subq %rdx, (%r8,%r15,2)
	.section .t1054,"ax",@progbits
	subq %rdx, (%rax,%r12,4)
	.section .t1055,"ax",@progbits
	subq %rdx, 0x100(%rbp)
	.section .t1056,"ax",@progbits
	subq %rdx, (%rsp)
	.section .t1057,"ax",@progbits
	subq %rdx, 0x10(%rsp,%rsi,4)
	.section .t1058,"ax",@progbits
	subq %rdx, %gs:0x10(%rcx)
	.section .t1059,"ax",@progbits
	subq %rdx, %fs:(%rax,%rsi,8)
	.section .t1060,"ax",@progbits
	subq %rbx, (%rcx)
	.section .t1061,"ax",@progbits
	subq %rbx, 0x10(%rcx)
	.section .t1062,"ax",@progbits
	subq %rbx, -0x8(%rbp)
	.section .t1063,"ax",@progbits
	subq %rbx, 0x12345(%rcx)
	.section .t1064,"ax",@progbits
	subq %rbx, (%rax,%rsi,4)
	.section .t1065,"ax",@progbits
	subq %rbx, 0x10(%rax,%rsi,8)
	.section .t1066,"ax",@progbits
	subq %rbx, (,%rsi,2)
	.section .t1067,"ax",@progbits
	subq %rbx, 0x40(%rip)
	.section .t1068,"ax",@progbits
	subq %rbx, -0x100(%rip)
	.section .t1069,"ax",@progbits
	subq %rbx, 0x1234
	.section .t1070,"ax",@progbits
	subq %rbx, (%r8)
	.section .t1071,"ax",@progbits
	subq %rbx, (%r12)
	.section .t1072,"ax",@progbits
	subq %rbx, 0x8(%r13)
	.section .t1073,"ax",@progbits
	subq %rbx, (%r8,%r15,2)
	.section .t1074,"ax",@progbits
	subq %rbx, (%rax,%r12,4)
	.section .t1075,"ax",@progbits
	subq %rbx, 0x100(%rbp)
	.section .t1076,"ax",@progbits
	subq %rbx, (%rsp)
	.section .t1077,"ax",@progbits
	subq %rbx, 0x10(%rsp,%rsi,4)
	.section .t1078,"ax",@progbits
	subq %rbx, %gs:0x10(%rcx)
	.section .t1079,"ax",@progbits
	subq %rbx, %fs:(%rax,%rsi,8)
	.section .t1080,"ax",@progbits
	subq %r9, (%rcx)
	.section .t1081,"ax",@progbits
	subq %r9, 0x10(%rcx)
	.section .t1082,"ax",@progbits
	subq %r9, -0x8(%rbp)
	.section .t1083,"ax",@progbits
	subq %r9, 0x12345(%rcx)
	.section .t1084,"ax",@progbits
	subq %r9, (%rax,%rsi,4)
	.section .t1085,"ax",@progbits
	subq %r9, 0x10(%rax,%rsi,8)
	.section .t1086,"ax",@progbits
	subq %r9, (,%rsi,2)
	.section .t1087,"ax",@progbits
	subq %r9, 0x40(%rip)
	.section .t1088,"ax",@progbits
	subq %r9, -0x100(%rip)
	.section .t1089,"ax",@progbits
	subq %r9, 0x1234
	.section .t1090,"ax",@progbits
	subq %r9, (%r8)
	.section .t1091,"ax",@progbits
	subq %r9, (%r12)
	.section .t1092,"ax",@progbits
	subq %r9, 0x8(%r13)
	.section .t1093,"ax",@progbits
	subq %r9, (%r8,%r15,2)
	.section .t1094,"ax",@progbits
	subq %r9, (%rax,%r12,4)
	.section .t1095,"ax",@progbits
	subq %r9, 0x100(%rbp)
	.section .t1096,"ax",@progbits
	subq %r9, (%rsp)
	.section .t1097,"ax",@progbits
	subq %r9, 0x10(%rsp,%rsi,4)
	.section .t1098,"ax",@progbits
	subq %r9, %gs:0x10(%rcx)
	.section .t1099,"ax",@progbits
	subq %r9, %fs:(%rax,%rsi,8)
	.section .t1100,"ax",@progbits
	subq %r12, (%rcx)
	.section .t1101,"ax",@progbits
	subq %r12, 0x10(%rcx)
	.section .t1102,"ax",@progbits
	subq %r12, -0x8(%rbp)
	.section .t1103,"ax",@progbits
	subq %r12, 0x12345(%rcx)
	.section .t1104,"ax",@progbits
	subq %r12, (%rax,%rsi,4)
	.section .t1105,"ax",@progbits
	subq %r12, 0x10(%rax,%rsi,8)
	.section .t1106,"ax",@progbits
	subq %r12, (,%rsi,2)
	.section .t1107,"ax",@progbits
	subq %r12, 0x40(%rip)
	.section .t1108,"ax",@progbits
	subq %r12, -0x100(%rip)
	.section .t1109,"ax",@progbits
	subq %r12, 0x1234
	.section .t1110,"ax",@progbits
	subq %r12, (%r8)
	.section .t1111,"ax",@progbits
	subq %r12, (%r12)
	.section .t1112,"ax",@progbits
	subq %r12, 0x8(%r13)
	.section .t1113,"ax",@progbits
	subq %r12, (%r8,%r15,2)
	.section .t1114,"ax",@progbits
	subq %r12, (%rax,%r12,4)
	.section .t1115,"ax",@progbits
	subq %r12, 0x100(%rbp)
	.section .t1116,"ax",@progbits
	subq %r12, (%rsp)
	.section .t1117,"ax",@progbits
	subq %r12, 0x10(%rsp,%rsi,4)
	.section .t1118,"ax",@progbits
	subq %r12, %gs:0x10(%rcx)
	.section .t1119,"ax",@progbits
	subq %r12, %fs:(%rax,%rsi,8)
	.section .t1120,"ax",@progbits
	xorb %dl, (%rcx)
	.section .t1121,"ax",@progbits
	xorb %dl, 0x10(%rcx)
	.section .t1122,"ax",@progbits
	xorb %dl, -0x8(%rbp)
	.section .t1123,"ax",@progbits
	xorb %dl, 0x12345(%rcx)
	.section .t1124,"ax",@progbits
	xorb %dl, (%rax,%rsi,4)
	.section .t1125,"ax",@progbits
	xorb %dl, 0x10(%rax,%rsi,8)
	.section .t1126,"ax",@progbits
	xorb %dl, (,%rsi,2)
	.section .t1127,"ax",@progbits
	xorb %dl, 0x40(%rip)
	.section .t1128,"ax",@progbits
	xorb %dl, -0x100(%rip)
	.section .t1129,"ax",@progbits
	xorb %dl, 0x1234
	.section .t1130,"ax",@progbits
	xorb %dl, (%r8)
	.section .t1131,"ax",@progbits
	xorb %dl, (%r12)
	.section .t1132,"ax",@progbits
	xorb %dl, 0x8(%r13)
	.section .t1133,"ax",@progbits
	xorb %dl, (%r8,%r15,2)
	.section .t1134,"ax",@progbits
	xorb %dl, (%rax,%r12,4)
	.section .t1135,"ax",@progbits
	xorb %dl, 0x100(%rbp)
	.section .t1136,"ax",@progbits
	xorb %dl, (%rsp)
	.section .t1137,"ax",@progbits
	xorb %dl, 0x10(%rsp,%rsi,4)
	.section .t1138,"ax",@progbits
	xorb %dl, %gs:0x10(%rcx)
	.section .t1139,"ax",@progbits
	xorb %dl, %fs:(%rax,%rsi,8)
	.section .t1140,"ax",@progbits
	xorb %bl, (%rcx)
	.section .t1141,"ax",@progbits
	xorb %bl, 0x10(%rcx)
	.section .t1142,"ax",@progbits
	xorb %bl, -0x8(%rbp)
	.section .t1143,"ax",@progbits
	xorb %bl, 0x12345(%rcx)
	.section .t1144,"ax",@progbits
	xorb %bl, (%rax,%rsi,4)
	.section .t1145,"ax",@progbits
	xorb %bl, 0x10(%rax,%rsi,8)
	.section .t1146,"ax",@progbits
	xorb %bl, (,%rsi,2)
	.section .t1147,"ax",@progbits
	xorb %bl, 0x40(%rip)
	.section .t1148,"ax",@progbits
	xorb %bl, -0x100(%rip)
	.section .t1149,"ax",@progbits
	xorb %bl, 0x1234
	.section .t1150,"ax",@progbits
	xorb %bl, (%r8)
	.section .t1151,"ax",@progbits
	xorb %bl, (%r12)
	.section .t1152,"ax",@progbits
	xorb %bl, 0x8(%r13)
	.section .t1153,"ax",@progbits
	xorb %bl, (%r8,%r15,2)
	.section .t1154,"ax",@progbits
	xorb %bl, (%rax,%r12,4)
	.section .t1155,"ax",@progbits
	xorb %bl, 0x100(%rbp)
	.section .t1156,"ax",@progbits
	xorb %bl, (%rsp)
	.section .t1157,"ax",@progbits
	xorb %bl, 0x10(%rsp,%rsi,4)
	.section .t1158,"ax",@progbits
	xorb %bl, %gs:0x10(%rcx)
	.section .t1159,"ax",@progbits
	xorb %bl, %fs:(%rax,%rsi,8)
	.section .t1160,"ax",@progbits
	xorb %r9b, (%rcx)
	.section .t1161,"ax",@progbits
	xorb %r9b, 0x10(%rcx)
	.section .t1162,"ax",@progbits
	xorb %r9b, -0x8(%rbp)
	.section .t1163,"ax",@progbits
	xorb %r9b, 0x12345(%rcx)
	.section .t1164,"ax",@progbits
	xorb %r9b, (%rax,%rsi,4)
	.section .t1165,"ax",@progbits
	xorb %r9b, 0x10(%rax,%rsi,8)
	.section .t1166,"ax",@progbits
	xorb %r9b, (,%rsi,2)
	.section .t1167,"ax",@progbits
	xorb %r9b, 0x40(%rip)
	.section .t1168,"ax",@progbits
	xorb %r9b, -0x100(%rip)
	.section .t1169,"ax",@progbits
	xorb %r9b, 0x1234
	.section .t1170,"ax",@progbits
	xorb %r9b, (%r8)
	.section .t1171,"ax",@progbits
	xorb %r9b, (%r12)
	.section .t1172,"ax",@progbits
	xorb %r9b, 0x8(%r13)
	.section .t1173,"ax",@progbits
	xorb %r9b, (%r8,%r15,2)
	.section .t1174,"ax",@progbits
	xorb %r9b, (%rax,%r12,4)
	.section .t1175,"ax",@progbits
	xorb %r9b, 0x100(%rbp)
	.section .t1176,"ax",@progbits
	xorb %r9b, (%rsp)
	.section .t1177,"ax",@progbits
	xorb %r9b, 0x10(%rsp,%rsi,4)
	.section .t1178,"ax",@progbits
	xorb %r9b, %gs:0x10(%rcx)
	.section .t1179,"ax",@progbits
	xorb %r9b, %fs:(%rax,%rsi,8)
	.section .t1180,"ax",@progbits
	xorb %r13b, (%rcx)
	.section .t1181,"ax",@progbits
	xorb %r13b, 0x10(%rcx)
	.section .t1182,"ax",@progbits
	xorb %r13b, -0x8(%rbp)
	.section .t1183,"ax",@progbits
	xorb %r13b, 0x12345(%rcx)
	.section .t1184,"ax",@progbits
	xorb %r13b, (%rax,%rsi,4)
	.section .t1185,"ax",@progbits
	xorb %r13b, 0x10(%rax,%rsi,8)
	.section .t1186,"ax",@progbits
	xorb %r13b, (,%rsi,2)
	.section .t1187,"ax",@progbits
	xorb %r13b, 0x40(%rip)
	.section .t1188,"ax",@progbits
	xorb %r13b, -0x100(%rip)
	.section .t1189,"ax",@progbits
	xorb %r13b, 0x1234
	.section .t1190,"ax",@progbits
	xorb %r13b, (%r8)
	.section .t1191,"ax",@progbits
	xorb %r13b, (%r12)
	.section .t1192,"ax",@progbits
	xorb %r13b, 0x8(%r13)
	.section .t1193,"ax",@progbits
	xorb %r13b, (%r8,%r15,2)
	.section .t1194,"ax",@progbits
	xorb %r13b, (%rax,%r12,4)
	.section .t1195,"ax",@progbits
	xorb %r13b, 0x100(%rbp)
	.section .t1196,"ax",@progbits
	xorb %r13b, (%rsp)
	.section .t1197,"ax",@progbits
	xorb %r13b, 0x10(%rsp,%rsi,4)
	.section .t1198,"ax",@progbits
	xorb %r13b, %gs:0x10(%rcx)
	.section .t1199,"ax",@progbits
	xorb %r13b, %fs:(%rax,%rsi,8)
	.section .t1200,"ax",@progbits
	xorw %dx, (%rcx)
	.section .t1201,"ax",@progbits
	xorw %dx, 0x10(%rcx)
	.section .t1202,"ax",@progbits
	xorw %dx, -0x8(%rbp)
	.section .t1203,"ax",@progbits
	xorw %dx, 0x12345(%rcx)
	.section .t1204,"ax",@progbits
	xorw %dx, (%rax,%rsi,4)
	.section .t1205,"ax",@progbits
	xorw %dx, 0x10(%rax,%rsi,8)
	.section .t1206,"ax",@progbits
	xorw %dx, (,%rsi,2)
	.section .t1207,"ax",@progbits
	xorw %dx, 0x40(%rip)
	.section .t1208,"ax",@progbits
	xorw %dx, -0x100(%rip)
	.section .t1209,"ax",@progbits
	xorw %dx, 0x1234
	.section .t1210,"ax",@progbits
	xorw %dx, (%r8)
	.section .t1211,"ax",@progbits
	xorw %dx, (%r12)
	.section .t1212,"ax",@progbits
	xorw %dx, 0x8(%r13)
	.section .t1213,"ax",@progbits
	xorw %dx, (%r8,%r15,2)
	.section .t1214,"ax",@progbits
	xorw %dx, (%rax,%r12,4)
	.section .t1215,"ax",@progbits
	xorw %dx, 0x100(%rbp)
	.section .t1216,"ax",@progbits
	xorw %dx, (%rsp)
	.section .t1217,"ax",@progbits
	xorw %dx, 0x10(%rsp,%rsi,4)
	.section .t1218,"ax",@progbits
	xorw %dx, %gs:0x10(%rcx)
	.section .t1219,"ax",@progbits
	xorw %dx, %fs:(%rax,%rsi,8)
	.section .t1220,"ax",@progbits
	xorw %bx, (%rcx)
	.section .t1221,"ax",@progbits
	xorw %bx, 0x10(%rcx)
	.section .t1222,"ax",@progbits
	xorw %bx, -0x8(%rbp)
	.section .t1223,"ax",@progbits
	xorw %bx, 0x12345(%rcx)
	.section .t1224,"ax",@progbits
	xorw %bx, (%rax,%rsi,4)
	.section .t1225,"ax",@progbits
	xorw %bx, 0x10(%rax,%rsi,8)
	.section .t1226,"ax",@progbits
	xorw %bx, (,%rsi,2)
	.section .t1227,"ax",@progbits
	xorw %bx, 0x40(%rip)
	.section .t1228,"ax",@progbits
	xorw %bx, -0x100(%rip)
	.section .t1229,"ax",@progbits
	xorw %bx, 0x1234
	.section .t1230,"ax",@progbits
	xorw %bx, (%r8)
	.section .t1231,"ax",@progbits
	xorw %bx, (%r12)
	.section .t1232,"ax",@progbits
	xorw %bx, 0x8(%r13)
	.section .t1233,"ax",@progbits
	xorw %bx, (%r8,%r15,2)
	.section .t1234,"ax",@progbits
	xorw %bx, (%rax,%r12,4)
	.section .t1235,"ax",@progbits
	xorw %bx, 0x100(%rbp)
	.section .t1236,"ax",@progbits
	xorw %bx, (%rsp)
	.section .t1237,"ax",@progbits
	xorw %bx, 0x10(%rsp,%rsi,4)
	.section .t1238,"ax",@progbits
	xorw %bx, %gs:0x10(%rcx)
	.section .t1239,"ax",@progbits
	xorw %bx, %fs:(%rax,%rsi,8)
	.section .t1240,"ax",@progbits
	xorw %r9w, (%rcx)
	.section .t1241,"ax",@progbits
	xorw %r9w, 0x10(%rcx)
	.section .t1242,"ax",@progbits
	xorw %r9w, -0x8(%rbp)
	.section .t1243,"ax",@progbits
	xorw %r9w, 0x12345(%rcx)
	.section .t1244,"ax",@progbits
	xorw %r9w, (%rax,%rsi,4)
	.section .t1245,"ax",@progbits
	xorw %r9w, 0x10(%rax,%rsi,8)
	.section .t1246,"ax",@progbits
	xorw %r9w, (,%rsi,2)
	.section .t1247,"ax",@progbits
	xorw %r9w, 0x40(%rip)
	.section .t1248,"ax",@progbits
	xorw %r9w, -0x100(%rip)
	.section .t1249,"ax",@progbits
	xorw %r9w, 0x1234
	.section .t1250,"ax",@progbits
	xorw %r9w, (%r8)
	.section .t1251,"ax",@progbits
	xorw %r9w, (%r12)
	.section .t1252,"ax",@progbits
	xorw %r9w, 0x8(%r13)
	.section .t1253,"ax",@progbits
	xorw %r9w, (%r8,%r15,2)
	.section .t1254,"ax",@progbits
	xorw %r9w, (%rax,%r12,4)
	.section .t1255,"ax",@progbits
	xorw %r9w, 0x100(%rbp)
	.section .t1256,"ax",@progbits
	xorw %r9w, (%rsp)
	.section .t1257,"ax",@progbits
	xorw %r9w, 0x10(%rsp,%rsi,4)
	.section .t1258,"ax",@progbits
	xorw %r9w, %gs:0x10(%rcx)
	.section .t1259,"ax",@progbits
	xorw %r9w, %fs:(%rax,%rsi,8)
	.section .t1260,"ax",@progbits
	xorl %edx, (%rcx)
	.section .t1261,"ax",@progbits
	xorl %edx, 0x10(%rcx)
	.section .t1262,"ax",@progbits
	xorl %edx, -0x8(%rbp)
	.section .t1263,"ax",@progbits
	xorl %edx, 0x12345(%rcx)
	.section .t1264,"ax",@progbits
	xorl %edx, (%rax,%rsi,4)
	.section .t1265,"ax",@progbits
	xorl %edx, 0x10(%rax,%rsi,8)
	.section .t1266,"ax",@progbits
	xorl %edx, (,%rsi,2)
	.section .t1267,"ax",@progbits
	xorl %edx, 0x40(%rip)
	.section .t1268,"ax",@progbits
	xorl %edx, -0x100(%rip)
	.section .t1269,"ax",@progbits
	xorl %edx, 0x1234
	.section .t1270,"ax",@progbits
	xorl %edx, (%r8)
	.section .t1271,"ax",@progbits
	xorl %edx, (%r12)
	.section .t1272,"ax",@progbits
	xorl %edx, 0x8(%r13)
	.section .t1273,"ax",@progbits
	xorl %edx, (%r8,%r15,2)
	.section .t1274,"ax",@progbits
	xorl %edx, (%rax,%r12,4)
	.section .t1275,"ax",@progbits
	xorl %edx, 0x100(%rbp)
	.section .t1276,"ax",@progbits
	xorl %edx, (%rsp)
	.section .t1277,"ax",@progbits
	xorl %edx, 0x10(%rsp,%rsi,4)
	.section .t1278,"ax",@progbits
	xorl %edx, %gs:0x10(%rcx)
	.section .t1279,"ax",@progbits
	xorl %edx, %fs:(%rax,%rsi,8)
	.section .t1280,"ax",@progbits
	xorl %ebx, (%rcx)
	.section .t1281,"ax",@progbits
	xorl %ebx, 0x10(%rcx)
	.section .t1282,"ax",@progbits
	xorl %ebx, -0x8(%rbp)
	.section .t1283,"ax",@progbits
	xorl %ebx, 0x12345(%rcx)
	.section .t1284,"ax",@progbits
	xorl %ebx, (%rax,%rsi,4)
	.section .t1285,"ax",@progbits
	xorl %ebx, 0x10(%rax,%rsi,8)
	.section .t1286,"ax",@progbits
	xorl %ebx, (,%rsi,2)
	.section .t1287,"ax",@progbits
	xorl %ebx, 0x40(%rip)
	.section .t1288,"ax",@progbits
	xorl %ebx, -0x100(%rip)
	.section .t1289,"ax",@progbits
	xorl %ebx, 0x1234
	.section .t1290,"ax",@progbits
	xorl %ebx, (%r8)
	.section .t1291,"ax",@progbits
	xorl %ebx, (%r12)
	.section .t1292,"ax",@progbits
	xorl %ebx, 0x8(%r13)
	.section .t1293,"ax",@progbits
	xorl %ebx, (%r8,%r15,2)
	.section .t1294,"ax",@progbits
	xorl %ebx, (%rax,%r12,4)
	.section .t1295,"ax",@progbits
	xorl %ebx, 0x100(%rbp)
	.section .t1296,"ax",@progbits
	xorl %ebx, (%rsp)
	.section .t1297,"ax",@progbits
	xorl %ebx, 0x10(%rsp,%rsi,4)
	.section .t1298,"ax",@progbits
	xorl %ebx, %gs:0x10(%rcx)
	.section .t1299,"ax",@progbits
	xorl %ebx, %fs:(%rax,%rsi,8)
	.section .t1300,"ax",@progbits
	xorl %r9d, (%rcx)
	.section .t1301,"ax",@progbits
	xorl %r9d, 0x10(%rcx)
	.section .t1302,"ax",@progbits
	xorl %r9d, -0x8(%rbp)
	.section .t1303,"ax",@progbits
	xorl %r9d, 0x12345(%rcx)
	.section .t1304,"ax",@progbits
	xorl %r9d, (%rax,%rsi,4)
	.section .t1305,"ax",@progbits
	xorl %r9d, 0x10(%rax,%rsi,8)
	.section .t1306,"ax",@progbits
	xorl %r9d, (,%rsi,2)
	.section .t1307,"ax",@progbits
	xorl %r9d, 0x40(%rip)
	.section .t1308,"ax",@progbits
	xorl %r9d, -0x100(%rip)
	.section .t1309,"ax",@progbits
	xorl %r9d, 0x1234
	.section .t1310,"ax",@progbits
	xorl %r9d, (%r8)
	.section .t1311,"ax",@progbits
	xorl %r9d, (%r12)
	.section .t1312,"ax",@progbits
	xorl %r9d, 0x8(%r13)
	.section .t1313,"ax",@progbits
	xorl %r9d, (%r8,%r15,2)
	.section .t1314,"ax",@progbits
	xorl %r9d, (%rax,%r12,4)
	.section .t1315,"ax",@progbits
	xorl %r9d, 0x100(%rbp)
	.section .t1316,"ax",@progbits
	xorl %r9d, (%rsp)
	.section .t1317,"ax",@progbits
	xorl %r9d, 0x10(%rsp,%rsi,4)
	.section .t1318,"ax",@progbits
	xorl %r9d, %gs:0x10(%rcx)
	.section .t1319,"ax",@progbits
	xorl %r9d, %fs:(%rax,%rsi,8)
	.section .t1320,"ax",@progbits
	xorq %rdx, (%rcx)
	.section .t1321,"ax",@progbits
	xorq %rdx, 0x10(%rcx)
	.section .t1322,"ax",@progbits
	xorq %rdx, -0x8(%rbp)
	.section .t1323,"ax",@progbits
	xorq %rdx, 0x12345(%rcx)
	.section .t1324,"ax",@progbits
	xorq %rdx, (%rax,%rsi,4)
	.section .t1325,"ax",@progbits
	xorq %rdx, 0x10(%rax,%rsi,8)
	.section .t1326,"ax",@progbits
	xorq %rdx, (,%rsi,2)
	.section .t1327,"ax",@progbits
	xorq %rdx, 0x40(%rip)
	.section .t1328,"ax",@progbits
	xorq %rdx, -0x100(%rip)
	.section .t1329,"ax",@progbits
	xorq %rdx, 0x1234
	.section .t1330,"ax",@progbits
	xorq %rdx, (%r8)
	.section .t1331,"ax",@progbits
	xorq %rdx, (%r12)
	.section .t1332,"ax",@progbits
	xorq %rdx, 0x8(%r13)
	.section .t1333,"ax",@progbits
	xorq %rdx, (%r8,%r15,2)
	.section .t1334,"ax",@progbits
	xorq %rdx, (%rax,%r12,4)
	.section .t1335,"ax",@progbits
	xorq %rdx, 0x100(%rbp)
	.section .t1336,"ax",@progbits
	xorq %rdx, (%rsp)
	.section .t1337,"ax",@progbits
	xorq %rdx, 0x10(%rsp,%rsi,4)
	.section .t1338,"ax",@progbits
	xorq %rdx, %gs:0x10(%rcx)
	.section .t1339,"ax",@progbits
	xorq %rdx, %fs:(%rax,%rsi,8)
	.section .t1340,"ax",@progbits
	xorq %rbx, (%rcx)
	.section .t1341,"ax",@progbits
	xorq %rbx, 0x10(%rcx)
	.section .t1342,"ax",@progbits
	xorq %rbx, -0x8(%rbp)
	.section .t1343,"ax",@progbits
	xorq %rbx, 0x12345(%rcx)
	.section .t1344,"ax",@progbits
	xorq %rbx, (%rax,%rsi,4)
	.section .t1345,"ax",@progbits
	xorq %rbx, 0x10(%rax,%rsi,8)
	.section .t1346,"ax",@progbits
	xorq %rbx, (,%rsi,2)
	.section .t1347,"ax",@progbits
	xorq %rbx, 0x40(%rip)
	.section .t1348,"ax",@progbits
	xorq %rbx, -0x100(%rip)
	.section .t1349,"ax",@progbits
	xorq %rbx, 0x1234
	.section .t1350,"ax",@progbits
	xorq %rbx, (%r8)
	.section .t1351,"ax",@progbits
	xorq %rbx, (%r12)
	.section .t1352,"ax",@progbits
	xorq %rbx, 0x8(%r13)
	.section .t1353,"ax",@progbits
	xorq %rbx, (%r8,%r15,2)
	.section .t1354,"ax",@progbits
	xorq %rbx, (%rax,%r12,4)
	.section .t1355,"ax",@progbits
	xorq %rbx, 0x100(%rbp)
	.section .t1356,"ax",@progbits
	xorq %rbx, (%rsp)
	.section .t1357,"ax",@progbits
	xorq %rbx, 0x10(%rsp,%rsi,4)
	.section .t1358,"ax",@progbits
	xorq %rbx, %gs:0x10(%rcx)
	.section .t1359,"ax",@progbits
	xorq %rbx, %fs:(%rax,%rsi,8)
	.section .t1360,"ax",@progbits
	xorq %r9, (%rcx)
	.section .t1361,"ax",@progbits
	xorq %r9, 0x10(%rcx)
	.section .t1362,"ax",@progbits
	xorq %r9, -0x8(%rbp)
	.section .t1363,"ax",@progbits
	xorq %r9, 0x12345(%rcx)
	.section .t1364,"ax",@progbits
	xorq %r9, (%rax,%rsi,4)
	.section .t1365,"ax",@progbits
	xorq %r9, 0x10(%rax,%rsi,8)
	.section .t1366,"ax",@progbits
	xorq %r9, (,%rsi,2)
	.section .t1367,"ax",@progbits
	xorq %r9, 0x40(%rip)
	.section .t1368,"ax",@progbits
	xorq %r9, -0x100(%rip)
	.section .t1369,"ax",@progbits
	xorq %r9, 0x1234
	.section .t1370,"ax",@progbits
	xorq %r9, (%r8)
	.section .t1371,"ax",@progbits
	xorq %r9, (%r12)
	.section .t1372,"ax",@progbits
	xorq %r9, 0x8(%r13)
	.section .t1373,"ax",@progbits
	xorq %r9, (%r8,%r15,2)
	.section .t1374,"ax",@progbits
	xorq %r9, (%rax,%r12,4)
	.section .t1375,"ax",@progbits
	xorq %r9, 0x100(%rbp)
	.section .t1376,"ax",@progbits
	xorq %r9, (%rsp)
	.section .t1377,"ax",@progbits
	xorq %r9, 0x10(%rsp,%rsi,4)
	.section .t1378,"ax",@progbits
	xorq %r9, %gs:0x10(%rcx)
	.section .t1379,"ax",@progbits
	xorq %r9, %fs:(%rax,%rsi,8)
	.section .t1380,"ax",@progbits
	xorq %r12, (%rcx)
	.section .t1381,"ax",@progbits
	xorq %r12, 0x10(%rcx)
	.section .t1382,"ax",@progbits
	xorq %r12, -0x8(%rbp)
	.section .t1383,"ax",@progbits
	xorq %r12, 0x12345(%rcx)
	.section .t1384,"ax",@progbits
	xorq %r12, (%rax,%rsi,4)
	.section .t1385,"ax",@progbits
	xorq %r12, 0x10(%rax,%rsi,8)
	.section .t1386,"ax",@progbits
	xorq %r12, (,%rsi,2)
	.section .t1387,"ax",@progbits
	xorq %r12, 0x40(%rip)
	.section .t1388,"ax",@progbits
	xorq %r12, -0x100(%rip)
	.section .t1389,"ax",@progbits
	xorq %r12, 0x1234
	.section .t1390,"ax",@progbits
	xorq %r12, (%r8)
	.section .t1391,"ax",@progbits
	xorq %r12, (%r12)
	.section .t1392,"ax",@progbits
	xorq %r12, 0x8(%r13)
	.section .t1393,"ax",@progbits
	xorq %r12, (%r8,%r15,2)
	.section .t1394,"ax",@progbits
	xorq %r12, (%rax,%r12,4)
	.section .t1395,"ax",@progbits
	xorq %r12, 0x100(%rbp)
	.section .t1396,"ax",@progbits
	xorq %r12, (%rsp)
	.section .t1397,"ax",@progbits
	xorq %r12, 0x10(%rsp,%rsi,4)
	.section .t1398,"ax",@progbits
	xorq %r12, %gs:0x10(%rcx)
	.section .t1399,"ax",@progbits
	xorq %r12, %fs:(%rax,%rsi,8)
	.section .t1400,"ax",@progbits
	movb %dl, (%rcx)
	.section .t1401,"ax",@progbits
	movb %dl, 0x10(%rcx)
	.section .t1402,"ax",@progbits
	movb %dl, -0x8(%rbp)
	.section .t1403,"ax",@progbits
	movb %dl, 0x12345(%rcx)
	.section .t1404,"ax",@progbits
	movb %dl, (%rax,%rsi,4)
	.section .t1405,"ax",@progbits
	movb %dl, 0x10(%rax,%rsi,8)
	.section .t1406,"ax",@progbits
	movb %dl, (,%rsi,2)
	.section .t1407,"ax",@progbits
	movb %dl, 0x40(%rip)
	.section .t1408,"ax",@progbits
	movb %dl, -0x100(%rip)
	.section .t1409,"ax",@progbits
	movb %dl, 0x1234
	.section .t1410,"ax",@progbits
	movb %dl, (%r8)
	.section .t1411,"ax",@progbits
	movb %dl, (%r12)
	.section .t1412,"ax",@progbits
	movb %dl, 0x8(%r13)
	.section .t1413,"ax",@progbits
	movb %dl, (%r8,%r15,2)
	.section .t1414,"ax",@progbits
	movb %dl, (%rax,%r12,4)
	.section .t1415,"ax",@progbits
	movb %dl, 0x100(%rbp)
	.section .t1416,"ax",@progbits
	movb %dl, (%rsp)
	.section .t1417,"ax",@progbits
	movb %dl, 0x10(%rsp,%rsi,4)
	.section .t1418,"ax",@progbits
	movb %dl, %gs:0x10(%rcx)
	.section .t1419,"ax",@progbits
	movb %dl, %fs:(%rax,%rsi,8)
	.section .t1420,"ax",@progbits
	movb %bl, (%rcx)
	.section .t1421,"ax",@progbits
	movb %bl, 0x10(%rcx)
	.section .t1422,"ax",@progbits
	movb %bl, -0x8(%rbp)
	.section .t1423,"ax",@progbits
	movb %bl, 0x12345(%rcx)
	.section .t1424,"ax",@progbits
	movb %bl, (%rax,%rsi,4)
	.section .t1425,"ax",@progbits
	movb %bl, 0x10(%rax,%rsi,8)
	.section .t1426,"ax",@progbits
	movb %bl, (,%rsi,2)
	.section .t1427,"ax",@progbits
	movb %bl, 0x40(%rip)
	.section .t1428,"ax",@progbits
	movb %bl, -0x100(%rip)
	.section .t1429,"ax",@progbits
	movb %bl, 0x1234
	.section .t1430,"ax",@progbits
	movb %bl, (%r8)
	.section .t1431,"ax",@progbits
	movb %bl, (%r12)
	.section .t1432,"ax",@progbits
	movb %bl, 0x8(%r13)
	.section .t1433,"ax",@progbits
	movb %bl, (%r8,%r15,2)
	.section .t1434,"ax",@progbits
	movb %bl, (%rax,%r12,4)
	.section .t1435,"ax",@progbits
	movb %bl, 0x100(%rbp)
	.section .t1436,"ax",@progbits
	movb %bl, (%rsp)
	.section .t1437,"ax",@progbits
	movb %bl, 0x10(%rsp,%rsi,4)
	.section .t1438,"ax",@progbits
	movb %bl, %gs:0x10(%rcx)
	.section .t1439,"ax",@progbits
	movb %bl, %fs:(%rax,%rsi,8)
	.section .t1440,"ax",@progbits
	movb %r9b, (%rcx)
	.section .t1441,"ax",@progbits
	movb %r9b, 0x10(%rcx)
	.section .t1442,"ax",@progbits
	movb %r9b, -0x8(%rbp)
	.section .t1443,"ax",@progbits
	movb %r9b, 0x12345(%rcx)
	.section .t1444,"ax",@progbits
	movb %r9b, (%rax,%rsi,4)
	.section .t1445,"ax",@progbits
	movb %r9b, 0x10(%rax,%rsi,8)
	.section .t1446,"ax",@progbits
	movb %r9b, (,%rsi,2)
	.section .t1447,"ax",@progbits
	movb %r9b, 0x40(%rip)
	.section .t1448,"ax",@progbits
	movb %r9b, -0x100(%rip)
	.section .t1449,"ax",@progbits
	movb %r9b, 0x1234
	.section .t1450,"ax",@progbits
	movb %r9b, (%r8)
	.section .t1451,"ax",@progbits
	movb %r9b, (%r12)
	.section .t1452,"ax",@progbits
	movb %r9b, 0x8(%r13)
	.section .t1453,"ax",@progbits
	movb %r9b, (%r8,%r15,2)
	.section .t1454,"ax",@progbits
	movb %r9b, (%rax,%r12,4)
	.section .t1455,"ax",@progbits
	movb %r9b, 0x100(%rbp)
	.section .t1456,"ax",@progbits
	movb %r9b, (%rsp)
	.section .t1457,"ax",@progbits
	movb %r9b, 0x10(%rsp,%rsi,4)
	.section .t1458,"ax",@progbits
	movb %r9b, %gs:0x10(%rcx)
	.section .t1459,"ax",@progbits
	movb %r9b, %fs:(%rax,%rsi,8)
	.section .t1460,"ax",@progbits
	movb %r13b, (%rcx)
	.section .t1461,"ax",@progbits
	movb %r13b, 0x10(%rcx)
	.section .t1462,"ax",@progbits
	movb %r13b, -0x8(%rbp)
	.section .t1463,"ax",@progbits
	movb %r13b, 0x12345(%rcx)
	.section .t1464,"ax",@progbits
	movb %r13b, (%rax,%rsi,4)
	.section .t1465,"ax",@progbits
	movb %r13b, 0x10(%rax,%rsi,8)
	.section .t1466,"ax",@progbits
	movb %r13b, (,%rsi,2)
	.section .t1467,"ax",@progbits
	movb %r13b, 0x40(%rip)
	.section .t1468,"ax",@progbits
	movb %r13b, -0x100(%rip)
	.section .t1469,"ax",@progbits
	movb %r13b, 0x1234
	.section .t1470,"ax",@progbits
	movb %r13b, (%r8)
	.section .t1471,"ax",@progbits
	movb %r13b, (%r12)
	.section .t1472,"ax",@progbits
	movb %r13b, 0x8(%r13)
	.section .t1473,"ax",@progbits
	movb %r13b, (%r8,%r15,2)
	.section .t1474,"ax",@progbits
	movb %r13b, (%rax,%r12,4)
	.section .t1475,"ax",@progbits
	movb %r13b, 0x100(%rbp)
	.section .t1476,"ax",@progbits
	movb %r13b, (%rsp)
	.section .t1477,"ax",@progbits
	movb %r13b, 0x10(%rsp,%rsi,4)
	.section .t1478,"ax",@progbits
	movb %r13b, %gs:0x10(%rcx)
	.section .t1479,"ax",@progbits
	movb %r13b, %fs:(%rax,%rsi,8)
	.section .t1480,"ax",@progbits
	movw %dx, (%rcx)
	.section .t1481,"ax",@progbits
	movw %dx, 0x10(%rcx)
	.section .t1482,"ax",@progbits
	movw %dx, -0x8(%rbp)
	.section .t1483,"ax",@progbits
	movw %dx, 0x12345(%rcx)
	.section .t1484,"ax",@progbits
	movw %dx, (%rax,%rsi,4)
	.section .t1485,"ax",@progbits
	movw %dx, 0x10(%rax,%rsi,8)
	.section .t1486,"ax",@progbits
	movw %dx, (,%rsi,2)
	.section .t1487,"ax",@progbits
	movw %dx, 0x40(%rip)
	.section .t1488,"ax",@progbits
	movw %dx, -0x100(%rip)
	.section .t1489,"ax",@progbits
	movw %dx, 0x1234
	.section .t1490,"ax",@progbits
	movw %dx, (%r8)
	.section .t1491,"ax",@progbits
	movw %dx, (%r12)
	.section .t1492,"ax",@progbits
	movw %dx, 0x8(%r13)
	.section .t1493,"ax",@progbits
	movw %dx, (%r8,%r15,2)
	.section .t1494,"ax",@progbits
	movw %dx, (%rax,%r12,4)
	.section .t1495,"ax",@progbits
	movw %dx, 0x100(%rbp)
	.section .t1496,"ax",@progbits
	movw %dx, (%rsp)
	.section .t1497,"ax",@progbits
	movw %dx, 0x10(%rsp,%rsi,4)
	.section .t1498,"ax",@progbits
	movw %dx, %gs:0x10(%rcx)
	.section .t1499,"ax",@progbits
	movw %dx, %fs:(%rax,%rsi,8)
	.section .t1500,"ax",@progbits
	movw %bx, (%rcx)
	.section .t1501,"ax",@progbits
	movw %bx, 0x10(%rcx)
	.section .t1502,"ax",@progbits
	movw %bx, -0x8(%rbp)
	.section .t1503,"ax",@progbits
	movw %bx, 0x12345(%rcx)
	.section .t1504,"ax",@progbits
	movw %bx, (%rax,%rsi,4)
	.section .t1505,"ax",@progbits
	movw %bx, 0x10(%rax,%rsi,8)
	.section .t1506,"ax",@progbits
	movw %bx, (,%rsi,2)
	.section .t1507,"ax",@progbits
	movw %bx, 0x40(%rip)
	.section .t1508,"ax",@progbits
	movw %bx, -0x100(%rip)
	.section .t1509,"ax",@progbits
	movw %bx, 0x1234
	.section .t1510,"ax",@progbits
	movw %bx, (%r8)
	.section .t1511,"ax",@progbits
	movw %bx, (%r12)
	.section .t1512,"ax",@progbits
	movw %bx, 0x8(%r13)
	.section .t1513,"ax",@progbits
	movw %bx, (%r8,%r15,2)
	.section .t1514,"ax",@progbits
	movw %bx, (%rax,%r12,4)
	.section .t1515,"ax",@progbits
	movw %bx, 0x100(%rbp)
	.section .t1516,"ax",@progbits
	movw %bx, (%rsp)
	.section .t1517,"ax",@progbits
	movw %bx, 0x10(%rsp,%rsi,4)
	.section .t1518,"ax",@progbits
	movw %bx, %gs:0x10(%rcx)
	.section .t1519,"ax",@progbits
	movw %bx, %fs:(%rax,%rsi,8)
	.section .t1520,"ax",@progbits
	movw %r9w, (%rcx)
	.section .t1521,"ax",@progbits
	movw %r9w, 0x10(%rcx)
	.section .t1522,"ax",@progbits
	movw %r9w, -0x8(%rbp)
	.section .t1523,"ax",@progbits
	movw %r9w, 0x12345(%rcx)
	.section .t1524,"ax",@progbits
	movw %r9w, (%rax,%rsi,4)
	.section .t1525,"ax",@progbits
	movw %r9w, 0x10(%rax,%rsi,8)
	.section .t1526,"ax",@progbits
	movw %r9w, (,%rsi,2)
	.section .t1527,"ax",@progbits
	movw %r9w, 0x40(%rip)
	.section .t1528,"ax",@progbits
	movw %r9w, -0x100(%rip)
	.section .t1529,"ax",@progbits
	movw %r9w, 0x1234
	.section .t1530,"ax",@progbits
	movw %r9w, (%r8)
	.section .t1531,"ax",@progbits
	movw %r9w, (%r12)
	.section .t1532,"ax",@progbits
	movw %r9w, 0x8(%r13)
	.section .t1533,"ax",@progbits
	movw %r9w, (%r8,%r15,2)
	.section .t1534,"ax",@progbits
	movw %r9w, (%rax,%r12,4)
	.section .t1535,"ax",@progbits
	movw %r9w, 0x100(%rbp)
	.section .t1536,"ax",@progbits
	movw %r9w, (%rsp)
	.section .t1537,"ax",@progbits
	movw %r9w, 0x10(%rsp,%rsi,4)
	.section .t1538,"ax",@progbits
	movw %r9w, %gs:0x10(%rcx)
	.section .t1539,"ax",@progbits
	movw %r9w, %fs:(%rax,%rsi,8)
	.section .t1540,"ax",@progbits
	movl %edx, (%rcx)
	.section .t1541,"ax",@progbits
	movl %edx, 0x10(%rcx)
	.section .t1542,"ax",@progbits
	movl %edx, -0x8(%rbp)
	.section .t1543,"ax",@progbits
	movl %edx, 0x12345(%rcx)
	.section .t1544,"ax",@progbits
	movl %edx, (%rax,%rsi,4)
	.section .t1545,"ax",@progbits
	movl %edx, 0x10(%rax,%rsi,8)
	.section .t1546,"ax",@progbits
	movl %edx, (,%rsi,2)
	.section .t1547,"ax",@progbits
	movl %edx, 0x40(%rip)
	.section .t1548,"ax",@progbits
	movl %edx, -0x100(%rip)
	.section .t1549,"ax",@progbits
	movl %edx, 0x1234
	.section .t1550,"ax",@progbits
	movl %edx, (%r8)
	.section .t1551,"ax",@progbits
	movl %edx, (%r12)
	.section .t1552,"ax",@progbits
	movl %edx, 0x8(%r13)
	.section .t1553,"ax",@progbits
	movl %edx, (%r8,%r15,2)
	.section .t1554,"ax",@progbits
	movl %edx, (%rax,%r12,4)
	.section .t1555,"ax",@progbits
	movl %edx, 0x100(%rbp)
	.section .t1556,"ax",@progbits
	movl %edx, (%rsp)
	.section .t1557,"ax",@progbits
	movl %edx, 0x10(%rsp,%rsi,4)
	.section .t1558,"ax",@progbits
	movl %edx, %gs:0x10(%rcx)
	.section .t1559,"ax",@progbits
	movl %edx, %fs:(%rax,%rsi,8)
	.section .t1560,"ax",@progbits
	movl %ebx, (%rcx)
	.section .t1561,"ax",@progbits
	movl %ebx, 0x10(%rcx)
	.section .t1562,"ax",@progbits
	movl %ebx, -0x8(%rbp)
	.section .t1563,"ax",@progbits
	movl %ebx, 0x12345(%rcx)
	.section .t1564,"ax",@progbits
	movl %ebx, (%rax,%rsi,4)
	.section .t1565,"ax",@progbits
	movl %ebx, 0x10(%rax,%rsi,8)
	.section .t1566,"ax",@progbits
	movl %ebx, (,%rsi,2)
	.section .t1567,"ax",@progbits
	movl %ebx, 0x40(%rip)
	.section .t1568,"ax",@progbits
	movl %ebx, -0x100(%rip)
	.section .t1569,"ax",@progbits
	movl %ebx, 0x1234
	.section .t1570,"ax",@progbits
	movl %ebx, (%r8)
	.section .t1571,"ax",@progbits
	movl %ebx, (%r12)
	.section .t1572,"ax",@progbits
	movl %ebx, 0x8(%r13)
	.section .t1573,"ax",@progbits
	movl %ebx, (%r8,%r15,2)
	.section .t1574,"ax",@progbits
	movl %ebx, (%rax,%r12,4)
	.section .t1575,"ax",@progbits
	movl %ebx, 0x100(%rbp)
	.section .t1576,"ax",@progbits
	movl %ebx, (%rsp)
	.section .t1577,"ax",@progbits
	movl %ebx, 0x10(%rsp,%rsi,4)
	.section .t1578,"ax",@progbits
	movl %ebx, %gs:0x10(%rcx)
	.section .t1579,"ax",@progbits
	movl %ebx, %fs:(%rax,%rsi,8)
	.section .t1580,"ax",@progbits
	movl %r9d, (%rcx)
	.section .t1581,"ax",@progbits
	movl %r9d, 0x10(%rcx)
	.section .t1582,"ax",@progbits
	movl %r9d, -0x8(%rbp)
	.section .t1583,"ax",@progbits
	movl %r9d, 0x12345(%rcx)
	.section .t1584,"ax",@progbits
	movl %r9d, (%rax,%rsi,4)
	.section .t1585,"ax",@progbits
	movl %r9d, 0x10(%rax,%rsi,8)
	.section .t1586,"ax",@progbits
	movl %r9d, (,%rsi,2)
	.section .t1587,"ax",@progbits
	movl %r9d, 0x40(%rip)
	.section .t1588,"ax",@progbits
	movl %r9d, -0x100(%rip)
	.section .t1589,"ax",@progbits
	movl %r9d, 0x1234
	.section .t1590,"ax",@progbits
	movl %r9d, (%r8)
	.section .t1591,"ax",@progbits
	movl %r9d, (%r12)
	.section .t1592,"ax",@progbits
	movl %r9d, 0x8(%r13)
	.section .t1593,"ax",@progbits
	movl %r9d, (%r8,%r15,2)
	.section .t1594,"ax",@progbits
	movl %r9d, (%rax,%r12,4)
	.section .t1595,"ax",@progbits
	movl %r9d, 0x100(%rbp)
	.section .t1596,"ax",@progbits
	movl %r9d, (%rsp)
	.section .t1597,"ax",@progbits
	movl %r9d, 0x10(%rsp,%rsi,4)
	.section .t1598,"ax",@progbits
	movl %r9d, %gs:0x10(%rcx)
	.section .t1599,"ax",@progbits
	movl %r9d, %fs:(%rax,%rsi,8)
	.section .t1600,"ax",@progbits
	movq %rdx, (%rcx)
	.section .t1601,"ax",@progbits
	movq %rdx, 0x10(%rcx)
	.section .t1602,"ax",@progbits
	movq %rdx, -0x8(%rbp)
	.section .t1603,"ax",@progbits
	movq %rdx, 0x12345(%rcx)
	.section .t1604,"ax",@progbits
	movq %rdx, (%rax,%rsi,4)
	.section .t1605,"ax",@progbits
	movq %rdx, 0x10(%rax,%rsi,8)
	.section .t1606,"ax",@progbits
	movq %rdx, (,%rsi,2)
	.section .t1607,"ax",@progbits
	movq %rdx, 0x40(%rip)
	.section .t1608,"ax",@progbits
	movq %rdx, -0x100(%rip)
	.section .t1609,"ax",@progbits
	movq %rdx, 0x1234
	.section .t1610,"ax",@progbits
	movq %rdx, (%r8)
	.section .t1611,"ax",@progbits
	movq %rdx, (%r12)
	.section .t1612,"ax",@progbits
	movq %rdx, 0x8(%r13)
	.section .t1613,"ax",@progbits
	movq %rdx, (%r8,%r15,2)
	.section .t1614,"ax",@progbits
	movq %rdx, (%rax,%r12,4)
	.section .t1615,"ax",@progbits
	movq %rdx, 0x100(%rbp)
	.section .t1616,"ax",@progbits
	movq %rdx, (%rsp)
	.section .t1617,"ax",@progbits
	movq %rdx, 0x10(%rsp,%rsi,4)
	.section .t1618,"ax",@progbits
	movq %rdx, %gs:0x10(%rcx)
	.section .t1619,"ax",@progbits
	movq %rdx, %fs:(%rax,%rsi,8)
	.section .t1620,"ax",@progbits
	movq %rbx, (%rcx)
	.section .t1621,"ax",@progbits
	movq %rbx, 0x10(%rcx)
	.section .t1622,"ax",@progbits
	movq %rbx, -0x8(%rbp)
	.section .t1623,"ax",@progbits
	movq %rbx, 0x12345(%rcx)
	.section .t1624,"ax",@progbits
	movq %rbx, (%rax,%rsi,4)
	.section .t1625,"ax",@progbits
	movq %rbx, 0x10(%rax,%rsi,8)
	.section .t1626,"ax",@progbits
	movq %rbx, (,%rsi,2)
	.section .t1627,"ax",@progbits
	movq %rbx, 0x40(%rip)
	.section .t1628,"ax",@progbits
	movq %rbx, -0x100(%rip)
	.section .t1629,"ax",@progbits
	movq %rbx, 0x1234
	.section .t1630,"ax",@progbits
	movq %rbx, (%r8)
	.section .t1631,"ax",@progbits
	movq %rbx, (%r12)
	.section .t1632,"ax",@progbits
	movq %rbx, 0x8(%r13)
	.section .t1633,"ax",@progbits
	movq %rbx, (%r8,%r15,2)
	.section .t1634,"ax",@progbits
	movq %rbx, (%rax,%r12,4)
	.section .t1635,"ax",@progbits
	movq %rbx, 0x100(%rbp)
	.section .t1636,"ax",@progbits
	movq %rbx, (%rsp)
	.section .t1637,"ax",@progbits
	movq %rbx, 0x10(%rsp,%rsi,4)
	.section .t1638,"ax",@progbits
	movq %rbx, %gs:0x10(%rcx)
	.section .t1639,"ax",@progbits
	movq %rbx, %fs:(%rax,%rsi,8)
	.section .t1640,"ax",@progbits
	movq %r9, (%rcx)
	.section .t1641,"ax",@progbits
	movq %r9, 0x10(%rcx)
	.section .t1642,"ax",@progbits
	movq %r9, -0x8(%rbp)
	.section .t1643,"ax",@progbits
	movq %r9, 0x12345(%rcx)
	.section .t1644,"ax",@progbits
	movq %r9, (%rax,%rsi,4)
	.section .t1645,"ax",@progbits
	movq %r9, 0x10(%rax,%rsi,8)
	.section .t1646,"ax",@progbits
	movq %r9, (,%rsi,2)
	.section .t1647,"ax",@progbits
	movq %r9, 0x40(%rip)
	.section .t1648,"ax",@progbits
	movq %r9, -0x100(%rip)
	.section .t1649,"ax",@progbits
	movq %r9, 0x1234
	.section .t1650,"ax",@progbits
	movq %r9, (%r8)
	.section .t1651,"ax",@progbits
	movq %r9, (%r12)
	.section .t1652,"ax",@progbits
	movq %r9, 0x8(%r13)
	.section .t1653,"ax",@progbits
	movq %r9, (%r8,%r15,2)
	.section .t1654,"ax",@progbits
	movq %r9, (%rax,%r12,4)
	.section .t1655,"ax",@progbits
	movq %r9, 0x100(%rbp)
	.section .t1656,"ax",@progbits
	movq %r9, (%rsp)
	.section .t1657,"ax",@progbits
	movq %r9, 0x10(%rsp,%rsi,4)
	.section .t1658,"ax",@progbits
	movq %r9, %gs:0x10(%rcx)
	.section .t1659,"ax",@progbits
	movq %r9, %fs:(%rax,%rsi,8)
	.section .t1660,"ax",@progbits
	movq %r12, (%rcx)
	.section .t1661,"ax",@progbits
	movq %r12, 0x10(%rcx)
	.section .t1662,"ax",@progbits
	movq %r12, -0x8(%rbp)
	.section .t1663,"ax",@progbits
	movq %r12, 0x12345(%rcx)
	.section .t1664,"ax",@progbits
	movq %r12, (%rax,%rsi,4)
	.section .t1665,"ax",@progbits
	movq %r12, 0x10(%rax,%rsi,8)
	.section .t1666,"ax",@progbits
	movq %r12, (,%rsi,2)
	.section .t1667,"ax",@progbits
	movq %r12, 0x40(%rip)
	.section .t1668,"ax",@progbits
	movq %r12, -0x100(%rip)
	.section .t1669,"ax",@progbits
	movq %r12, 0x1234
	.section .t1670,"ax",@progbits
	movq %r12, (%r8)
	.section .t1671,"ax",@progbits
	movq %r12, (%r12)
	.section .t1672,"ax",@progbits
	movq %r12, 0x8(%r13)
	.section .t1673,"ax",@progbits
	movq %r12, (%r8,%r15,2)
	.section .t1674,"ax",@progbits
	movq %r12, (%rax,%r12,4)
	.section .t1675,"ax",@progbits
	movq %r12, 0x100(%rbp)
	.section .t1676,"ax",@progbits
	movq %r12, (%rsp)
	.section .t1677,"ax",@progbits
	movq %r12, 0x10(%rsp,%rsi,4)
	.section .t1678,"ax",@progbits
	movq %r12, %gs:0x10(%rcx)
	.section .t1679,"ax",@progbits
	movq %r12, %fs:(%rax,%rsi,8)
	.section .t1680,"ax",@progbits
	xchgb %dl, (%rcx)
	.section .t1681,"ax",@progbits
	xchgb %dl, 0x10(%rcx)
	.section .t1682,"ax",@progbits
	xchgb %dl, -0x8(%rbp)
	.section .t1683,"ax",@progbits
	xchgb %dl, 0x12345(%rcx)
	.section .t1684,"ax",@progbits
	xchgb %dl, (%rax,%rsi,4)
	.section .t1685,"ax",@progbits
	xchgb %dl, 0x10(%rax,%rsi,8)
	.section .t1686,"ax",@progbits
	xchgb %dl, (,%rsi,2)
	.section .t1687,"ax",@progbits
	xchgb %dl, 0x40(%rip)
	.section .t1688,"ax",@progbits
	xchgb %dl, -0x100(%rip)
	.section .t1689,"ax",@progbits
	xchgb %dl, 0x1234
	.section .t1690,"ax",@progbits
	xchgb %dl, (%r8)
	.section .t1691,"ax",@progbits
	xchgb %dl, (%r12)
	.section .t1692,"ax",@progbits
	xchgb %dl, 0x8(%r13)
	.section .t1693,"ax",@progbits
	xchgb %dl, (%r8,%r15,2)
	.section .t1694,"ax",@progbits
	xchgb %dl, (%rax,%r12,4)
	.section .t1695,"ax",@progbits
	xchgb %dl, 0x100(%rbp)
	.section .t1696,"ax",@progbits
	xchgb %dl, (%rsp)
	.section .t1697,"ax",@progbits
	xchgb %dl, 0x10(%rsp,%rsi,4)
	.section .t1698,"ax",@progbits
	xchgb %dl, %gs:0x10(%rcx)
	.section .t1699,"ax",@progbits
	xchgb %dl, %fs:(%rax,%rsi,8)
	.section .t1700,"ax",@progbits
	xchgb %bl, (%rcx)
	.section .t1701,"ax",@progbits
	xchgb %bl, 0x10(%rcx)
	.section .t1702,"ax",@progbits
	xchgb %bl, -0x8(%rbp)
	.section .t1703,"ax",@progbits
	xchgb %bl, 0x12345(%rcx)
	.section .t1704,"ax",@progbits
	xchgb %bl, (%rax,%rsi,4)
	.section .t1705,"ax",@progbits
	xchgb %bl, 0x10(%rax,%rsi,8)
	.section .t1706,"ax",@progbits
	xchgb %bl, (,%rsi,2)
	.section .t1707,"ax",@progbits
	xchgb %bl, 0x40(%rip)
	.section .t1708,"ax",@progbits
	xchgb %bl, -0x100(%rip)
	.section .t1709,"ax",@progbits
	xchgb %bl, 0x1234
	.section .t1710,"ax",@progbits
	xchgb %bl, (%r8)
	.section .t1711,"ax",@progbits
	xchgb %bl, (%r12)
	.section .t1712,"ax",@progbits
	xchgb %bl, 0x8(%r13)
	.section .t1713,"ax",@progbits
	xchgb %bl, (%r8,%r15,2)
	.section .t1714,"ax",@progbits
	xchgb %bl, (%rax,%r12,4)
	.section .t1715,"ax",@progbits
	xchgb %bl, 0x100(%rbp)
	.section .t1716,"ax",@progbits
	xchgb %bl, (%rsp)
	.section .t1717,"ax",@progbits
	xchgb %bl, 0x10(%rsp,%rsi,4)
	.section .t1718,"ax",@progbits
	xchgb %bl, %gs:0x10(%rcx)
	.section .t1719,"ax",@progbits
	xchgb %bl, %fs:(%rax,%rsi,8)
	.section .t1720,"ax",@progbits
	xchgb %r9b, (%rcx)
	.section .t1721,"ax",@progbits
	xchgb %r9b, 0x10(%rcx)
	.section .t1722,"ax",@progbits
	xchgb %r9b, -0x8(%rbp)
	.section .t1723,"ax",@progbits
	xchgb %r9b, 0x12345(%rcx)
	.section .t1724,"ax",@progbits
	xchgb %r9b, (%rax,%rsi,4)
	.section .t1725,"ax",@progbits
	xchgb %r9b, 0x10(%rax,%rsi,8)
	.section .t1726,"ax",@progbits
	xchgb %r9b, (,%rsi,2)
	.section .t1727,"ax",@progbits
	xchgb %r9b, 0x40(%rip)
	.section .t1728,"ax",@progbits
	xchgb %r9b, -0x100(%rip)
	.section .t1729,"ax",@progbits
	xchgb %r9b, 0x1234
	.section .t1730,"ax",@progbits
	xchgb %r9b, (%r8)
	.section .t1731,"ax",@progbits
	xchgb %r9b, (%r12)
	.section .t1732,"ax",@progbits
	xchgb %r9b, 0x8(%r13)
	.section .t1733,"ax",@progbits
	xchgb %r9b, (%r8,%r15,2)
	.section .t1734,"ax",@progbits
	xchgb %r9b, (%rax,%r12,4)
	.section .t1735,"ax",@progbits
	xchgb %r9b, 0x100(%rbp)
	.section .t1736,"ax",@progbits
	xchgb %r9b, (%rsp)
	.section .t1737,"ax",@progbits
	xchgb %r9b, 0x10(%rsp,%rsi,4)
	.section .t1738,"ax",@progbits
	xchgb %r9b, %gs:0x10(%rcx)
	.section .t1739,"ax",@progbits
	xchgb %r9b, %fs:(%rax,%rsi,8)
	.section .t1740,"ax",@progbits
	xchgb %r13b, (%rcx)
	.section .t1741,"ax",@progbits
	xchgb %r13b, 0x10(%rcx)
	.section .t1742,"ax",@progbits
	xchgb %r13b, -0x8(%rbp)
	.section .t1743,"ax",@progbits
	xchgb %r13b, 0x12345(%rcx)
	.section .t1744,"ax",@progbits
	xchgb %r13b, (%rax,%rsi,4)
	.section .t1745,"ax",@progbits
	xchgb %r13b, 0x10(%rax,%rsi,8)
	.section .t1746,"ax",@progbits
	xchgb %r13b, (,%rsi,2)
	.section .t1747,"ax",@progbits
	xchgb %r13b, 0x40(%rip)
	.section .t1748,"ax",@progbits
	xchgb %r13b, -0x100(%rip)
	.section .t1749,"ax",@progbits
	xchgb %r13b, 0x1234
	.section .t1750,"ax",@progbits
	xchgb %r13b, (%r8)
	.section .t1751,"ax",@progbits
	xchgb %r13b, (%r12)
	.section .t1752,"ax",@progbits
	xchgb %r13b, 0x8(%r13)
	.section .t1753,"ax",@progbits
	xchgb %r13b, (%r8,%r15,2)
	.section .t1754,"ax",@progbits
	xchgb %r13b, (%rax,%r12,4)
	.section .t1755,"ax",@progbits
	xchgb %r13b, 0x100(%rbp)
	.section .t1756,"ax",@progbits
	xchgb %r13b, (%rsp)
	.section .t1757,"ax",@progbits
	xchgb %r13b, 0x10(%rsp,%rsi,4)
	.section .t1758,"ax",@progbits
	xchgb %r13b, %gs:0x10(%rcx)
	.section .t1759,"ax",@progbits
	xchgb %r13b, %fs:(%rax,%rsi,8)
	.section .t1760,"ax",@progbits
	xchgw %dx, (%rcx)
	.section .t1761,"ax",@progbits
	xchgw %dx, 0x10(%rcx)
	.section .t1762,"ax",@progbits
	xchgw %dx, -0x8(%rbp)
	.section .t1763,"ax",@progbits
	xchgw %dx, 0x12345(%rcx)
	.section .t1764,"ax",@progbits
	xchgw %dx, (%rax,%rsi,4)
	.section .t1765,"ax",@progbits
	xchgw %dx, 0x10(%rax,%rsi,8)
	.section .t1766,"ax",@progbits
	xchgw %dx, (,%rsi,2)
	.section .t1767,"ax",@progbits
	xchgw %dx, 0x40(%rip)
	.section .t1768,"ax",@progbits
	xchgw %dx, -0x100(%rip)
	.section .t1769,"ax",@progbits
	xchgw %dx, 0x1234
	.section .t1770,"ax",@progbits
	xchgw %dx, (%r8)
	.section .t1771,"ax",@progbits
	xchgw %dx, (%r12)
	.section .t1772,"ax",@progbits
	xchgw %dx, 0x8(%r13)
	.section .t1773,"ax",@progbits
	xchgw %dx, (%r8,%r15,2)
	.section .t1774,"ax",@progbits
	xchgw %dx, (%rax,%r12,4)
	.section .t1775,"ax",@progbits
	xchgw %dx, 0x100(%rbp)
	.section .t1776,"ax",@progbits
	xchgw %dx, (%rsp)
	.section .t1777,"ax",@progbits
	xchgw %dx, 0x10(%rsp,%rsi,4)
	.section .t1778,"ax",@progbits
	xchgw %dx, %gs:0x10(%rcx)
	.section .t1779,"ax",@progbits
	xchgw %dx, %fs:(%rax,%rsi,8)
	.section .t1780,"ax",@progbits
	xchgw %bx, (%rcx)
	.section .t1781,"ax",@progbits
	xchgw %bx, 0x10(%rcx)
	.section .t1782,"ax",@progbits
	xchgw %bx, -0x8(%rbp)
	.section .t1783,"ax",@progbits
	xchgw %bx, 0x12345(%rcx)
	.section .t1784,"ax",@progbits
	xchgw %bx, (%rax,%rsi,4)
	.section .t1785,"ax",@progbits
	xchgw %bx, 0x10(%rax,%rsi,8)
	.section .t1786,"ax",@progbits
	xchgw %bx, (,%rsi,2)
	.section .t1787,"ax",@progbits
	xchgw %bx, 0x40(%rip)
	.section .t1788,"ax",@progbits
	xchgw %bx, -0x100(%rip)
	.section .t1789,"ax",@progbits
	xchgw %bx, 0x1234
	.section .t1790,"ax",@progbits
	xchgw %bx, (%r8)
	.section .t1791,"ax",@progbits
	xchgw %bx, (%r12)
	.section .t1792,"ax",@progbits
	xchgw %bx, 0x8(%r13)
	.section .t1793,"ax",@progbits
	xchgw %bx, (%r8,%r15,2)
	.section .t1794,"ax",@progbits
	xchgw %bx, (%rax,%r12,4)
	.section .t1795,"ax",@progbits
	xchgw %bx, 0x100(%rbp)
	.section .t1796,"ax",@progbits
	xchgw %bx, (%rsp)
	.section .t1797,"ax",@progbits
	xchgw %bx, 0x10(%rsp,%rsi,4)
	.section .t1798,"ax",@progbits
	xchgw %bx, %gs:0x10(%rcx)
	.section .t1799,"ax",@progbits
	xchgw %bx, %fs:(%rax,%rsi,8)
	.section .t1800,"ax",@progbits
	xchgw %r9w, (%rcx)
	.section .t1801,"ax",@progbits
	xchgw %r9w, 0x10(%rcx)
	.section .t1802,"ax",@progbits
	xchgw %r9w, -0x8(%rbp)
	.section .t1803,"ax",@progbits
	xchgw %r9w, 0x12345(%rcx)
	.section .t1804,"ax",@progbits
	xchgw %r9w, (%rax,%rsi,4)
	.section .t1805,"ax",@progbits
	xchgw %r9w, 0x10(%rax,%rsi,8)
	.section .t1806,"ax",@progbits
	xchgw %r9w, (,%rsi,2)
	.section .t1807,"ax",@progbits
	xchgw %r9w, 0x40(%rip)
	.section .t1808,"ax",@progbits
	xchgw %r9w, -0x100(%rip)
	.section .t1809,"ax",@progbits
	xchgw %r9w, 0x1234
	.section .t1810,"ax",@progbits
	xchgw %r9w, (%r8)
	.section .t1811,"ax",@progbits
	xchgw %r9w, (%r12)
	.section .t1812,"ax",@progbits
	xchgw %r9w, 0x8(%r13)
	.section .t1813,"ax",@progbits
	xchgw %r9w, (%r8,%r15,2)
	.section .t1814,"ax",@progbits
	xchgw %r9w, (%rax,%r12,4)
	.section .t1815,"ax",@progbits
	xchgw %r9w, 0x100(%rbp)
	.section .t1816,"ax",@progbits
	xchgw %r9w, (%rsp)
	.section .t1817,"ax",@progbits
	xchgw %r9w, 0x10(%rsp,%rsi,4)
	.section .t1818,"ax",@progbits
	xchgw %r9w, %gs:0x10(%rcx)
	.section .t1819,"ax",@progbits
	xchgw %r9w, %fs:(%rax,%rsi,8)
	.section .t1820,"ax",@progbits
	xchgl %edx, (%rcx)
	.section .t1821,"ax",@progbits
	xchgl %edx, 0x10(%rcx)
	.section .t1822,"ax",@progbits
	xchgl %edx, -0x8(%rbp)
	.section .t1823,"ax",@progbits
	xchgl %edx, 0x12345(%rcx)
	.section .t1824,"ax",@progbits
	xchgl %edx, (%rax,%rsi,4)
	.section .t1825,"ax",@progbits
	xchgl %edx, 0x10(%rax,%rsi,8)
	.section .t1826,"ax",@progbits
	xchgl %edx, (,%rsi,2)
	.section .t1827,"ax",@progbits
	xchgl %edx, 0x40(%rip)
	.section .t1828,"ax",@progbits
	xchgl %edx, -0x100(%rip)
	.section .t1829,"ax",@progbits
	xchgl %edx, 0x1234
	.section .t1830,"ax",@progbits
	xchgl %edx, (%r8)
	.section .t1831,"ax",@progbits
	xchgl %edx, (%r12)
	.section .t1832,"ax",@progbits
	xchgl %edx, 0x8(%r13)
	.section .t1833,"ax",@progbits
	xchgl %edx, (%r8,%r15,2)
	.section .t1834,"ax",@progbits
	xchgl %edx, (%rax,%r12,4)
	.section .t1835,"ax",@progbits
	xchgl %edx, 0x100(%rbp)
	.section .t1836,"ax",@progbits
	xchgl %edx, (%rsp)
	.section .t1837,"ax",@progbits
	xchgl %edx, 0x10(%rsp,%rsi,4)
	.section .t1838,"ax",@progbits
	xchgl %edx, %gs:0x10(%rcx)
	.section .t1839,"ax",@progbits
	xchgl %edx, %fs:(%rax,%rsi,8)
	.section .t1840,"ax",@progbits
	xchgl %ebx, (%rcx)
	.section .t1841,"ax",@progbits
	xchgl %ebx, 0x10(%rcx)
	.section .t1842,"ax",@progbits
	xchgl %ebx, -0x8(%rbp)
	.section .t1843,"ax",@progbits
	xchgl %ebx, 0x12345(%rcx)
	.section .t1844,"ax",@progbits
	xchgl %ebx, (%rax,%rsi,4)
	.section .t1845,"ax",@progbits
	xchgl %ebx, 0x10(%rax,%rsi,8)
	.section .t1846,"ax",@progbits
	xchgl %ebx, (,%rsi,2)
	.section .t1847,"ax",@progbits
	xchgl %ebx, 0x40(%rip)
	.section .t1848,"ax",@progbits
	xchgl %ebx, -0x100(%rip)
	.section .t1849,"ax",@progbits
	xchgl %ebx, 0x1234
	.section .t1850,"ax",@progbits
	xchgl %ebx, (%r8)
	.section .t1851,"ax",@progbits
	xchgl %ebx, (%r12)
	.section .t1852,"ax",@progbits
	xchgl %ebx, 0x8(%r13)
	.section .t1853,"ax",@progbits
	xchgl %ebx, (%r8,%r15,2)
	.section .t1854,"ax",@progbits
	xchgl %ebx, (%rax,%r12,4)
	.section .t1855,"ax",@progbits
	xchgl %ebx, 0x100(%rbp)
	.section .t1856,"ax",@progbits
	xchgl %ebx, (%rsp)
	.section .t1857,"ax",@progbits
	xchgl %ebx, 0x10(%rsp,%rsi,4)
	.section .t1858,"ax",@progbits
	xchgl %ebx, %gs:0x10(%rcx)
	.section .t1859,"ax",@progbits
	xchgl %ebx, %fs:(%rax,%rsi,8)
	.section .t1860,"ax",@progbits
	xchgl %r9d, (%rcx)
	.section .t1861,"ax",@progbits
	xchgl %r9d, 0x10(%rcx)
	.section .t1862,"ax",@progbits
	xchgl %r9d, -0x8(%rbp)
	.section .t1863,"ax",@progbits
	xchgl %r9d, 0x12345(%rcx)
	.section .t1864,"ax",@progbits
	xchgl %r9d, (%rax,%rsi,4)
	.section .t1865,"ax",@progbits
	xchgl %r9d, 0x10(%rax,%rsi,8)
	.section .t1866,"ax",@progbits
	xchgl %r9d, (,%rsi,2)
	.section .t1867,"ax",@progbits
	xchgl %r9d, 0x40(%rip)
	.section .t1868,"ax",@progbits
	xchgl %r9d, -0x100(%rip)
	.section .t1869,"ax",@progbits
	xchgl %r9d, 0x1234
	.section .t1870,"ax",@progbits
	xchgl %r9d, (%r8)
	.section .t1871,"ax",@progbits
	xchgl %r9d, (%r12)
	.section .t1872,"ax",@progbits
	xchgl %r9d, 0x8(%r13)
	.section .t1873,"ax",@progbits
	xchgl %r9d, (%r8,%r15,2)
	.section .t1874,"ax",@progbits
	xchgl %r9d, (%rax,%r12,4)
	.section .t1875,"ax",@progbits
	xchgl %r9d, 0x100(%rbp)
	.section .t1876,"ax",@progbits
	xchgl %r9d, (%rsp)
	.section .t1877,"ax",@progbits
	xchgl %r9d, 0x10(%rsp,%rsi,4)
	.section .t1878,"ax",@progbits
	xchgl %r9d, %gs:0x10(%rcx)
	.section .t1879,"ax",@progbits
	xchgl %r9d, %fs:(%rax,%rsi,8)
	.section .t1880,"ax",@progbits
	xchgq %rdx, (%rcx)
	.section .t1881,"ax",@progbits
	xchgq %rdx, 0x10(%rcx)
	.section .t1882,"ax",@progbits
	xchgq %rdx, -0x8(%rbp)
	.section .t1883,"ax",@progbits
	xchgq %rdx, 0x12345(%rcx)
	.section .t1884,"ax",@progbits
	xchgq %rdx, (%rax,%rsi,4)
	.section .t1885,"ax",@progbits
	xchgq %rdx, 0x10(%rax,%rsi,8)
	.section .t1886,"ax",@progbits
	xchgq %rdx, (,%rsi,2)
	.section .t1887,"ax",@progbits
	xchgq %rdx, 0x40(%rip)
	.section .t1888,"ax",@progbits
	xchgq %rdx, -0x100(%rip)
	.section .t1889,"ax",@progbits
	xchgq %rdx, 0x1234
	.section .t1890,"ax",@progbits
	xchgq %rdx, (%r8)
	.section .t1891,"ax",@progbits
	xchgq %rdx, (%r12)
	.section .t1892,"ax",@progbits
	xchgq %rdx, 0x8(%r13)
	.section .t1893,"ax",@progbits
	xchgq %rdx, (%r8,%r15,2)
	.section .t1894,"ax",@progbits
	xchgq %rdx, (%rax,%r12,4)
	.section .t1895,"ax",@progbits
	xchgq %rdx, 0x100(%rbp)
	.section .t1896,"ax",@progbits
	xchgq %rdx, (%rsp)
	.section .t1897,"ax",@progbits
	xchgq %rdx, 0x10(%rsp,%rsi,4)
	.section .t1898,"ax",@progbits
	xchgq %rdx, %gs:0x10(%rcx)
	.section .t1899,"ax",@progbits
	xchgq %rdx, %fs:(%rax,%rsi,8)
	.section .t1900,"ax",@progbits
	xchgq %rbx, (%rcx)
	.section .t1901,"ax",@progbits
	xchgq %rbx, 0x10(%rcx)
	.section .t1902,"ax",@progbits
	xchgq %rbx, -0x8(%rbp)
	.section .t1903,"ax",@progbits
	xchgq %rbx, 0x12345(%rcx)
	.section .t1904,"ax",@progbits
	xchgq %rbx, (%rax,%rsi,4)
	.section .t1905,"ax",@progbits
	xchgq %rbx, 0x10(%rax,%rsi,8)
	.section .t1906,"ax",@progbits
	xchgq %rbx, (,%rsi,2)
	.section .t1907,"ax",@progbits
	xchgq %rbx, 0x40(%rip)
	.section .t1908,"ax",@progbits
	xchgq %rbx, -0x100(%rip)
	.section .t1909,"ax",@progbits
	xchgq %rbx, 0x1234
	.section .t1910,"ax",@progbits
	xchgq %rbx, (%r8)
	.section .t1911,"ax",@progbits
	xchgq %rbx, (%r12)
	.section .t1912,"ax",@progbits
	xchgq %rbx, 0x8(%r13)
	.section .t1913,"ax",@progbits
	xchgq %rbx, (%r8,%r15,2)
	.section .t1914,"ax",@progbits
	xchgq %rbx, (%rax,%r12,4)
	.section .t1915,"ax",@progbits
	xchgq %rbx, 0x100(%rbp)
	.section .t1916,"ax",@progbits
	xchgq %rbx, (%rsp)
	.section .t1917,"ax",@progbits
	xchgq %rbx, 0x10(%rsp,%rsi,4)
	.section .t1918,"ax",@progbits
	xchgq %rbx, %gs:0x10(%rcx)
	.section .t1919,"ax",@progbits
	xchgq %rbx, %fs:(%rax,%rsi,8)
	.section .t1920,"ax",@progbits
	xchgq %r9, (%rcx)
	.section .t1921,"ax",@progbits
	xchgq %r9, 0x10(%rcx)
	.section .t1922,"ax",@progbits
	xchgq %r9, -0x8(%rbp)
	.section .t1923,"ax",@progbits
	xchgq %r9, 0x12345(%rcx)
	.section .t1924,"ax",@progbits
	xchgq %r9, (%rax,%rsi,4)
	.section .t1925,"ax",@progbits
	xchgq %r9, 0x10(%rax,%rsi,8)
	.section .t1926,"ax",@progbits
	xchgq %r9, (,%rsi,2)
	.section .t1927,"ax",@progbits
	xchgq %r9, 0x40(%rip)
	.section .t1928,"ax",@progbits
	xchgq %r9, -0x100(%rip)
	.section .t1929,"ax",@progbits
	xchgq %r9, 0x1234
	.section .t1930,"ax",@progbits
	xchgq %r9, (%r8)
	.section .t1931,"ax",@progbits
	xchgq %r9, (%r12)
	.section .t1932,"ax",@progbits
	xchgq %r9, 0x8(%r13)
	.section .t1933,"ax",@progbits
	xchgq %r9, (%r8,%r15,2)
	.section .t1934,"ax",@progbits
	xchgq %r9, (%rax,%r12,4)
	.section .t1935,"ax",@progbits
	xchgq %r9, 0x100(%rbp)
	.section .t1936,"ax",@progbits
	xchgq %r9, (%rsp)
	.section .t1937,"ax",@progbits
	xchgq %r9, 0x10(%rsp,%rsi,4)
	.section .t1938,"ax",@progbits
	xchgq %r9, %gs:0x10(%rcx)
	.section .t1939,"ax",@progbits
	xchgq %r9, %fs:(%rax,%rsi,8)
	.section .t1940,"ax",@progbits
	xchgq %r12, (%rcx)
	.section .t1941,"ax",@progbits
	xchgq %r12, 0x10(%rcx)
	.section .t1942,"ax",@progbits
	xchgq %r12, -0x8(%rbp)
	.section .t1943,"ax",@progbits
	xchgq %r12, 0x12345(%rcx)
	.section .t1944,"ax",@progbits
	xchgq %r12, (%rax,%rsi,4)
	.section .t1945,"ax",@progbits
	xchgq %r12, 0x10(%rax,%rsi,8)
	.section .t1946,"ax",@progbits
	xchgq %r12, (,%rsi,2)
	.section .t1947,"ax",@progbits
	xchgq %r12, 0x40(%rip)
	.section .t1948,"ax",@progbits
	xchgq %r12, -0x100(%rip)
	.section .t1949,"ax",@progbits
	xchgq %r12, 0x1234
	.section .t1950,"ax",@progbits
	xchgq %r12, (%r8)
	.section .t1951,"ax",@progbits
	xchgq %r12, (%r12)
	.section .t1952,"ax",@progbits
	xchgq %r12, 0x8(%r13)
	.section .t1953,"ax",@progbits
	xchgq %r12, (%r8,%r15,2)
	.section .t1954,"ax",@progbits
	xchgq %r12, (%rax,%r12,4)
	.section .t1955,"ax",@progbits
	xchgq %r12, 0x100(%rbp)
	.section .t1956,"ax",@progbits
	xchgq %r12, (%rsp)
	.section .t1957,"ax",@progbits
	xchgq %r12, 0x10(%rsp,%rsi,4)
	.section .t1958,"ax",@progbits
	xchgq %r12, %gs:0x10(%rcx)
	.section .t1959,"ax",@progbits
	xchgq %r12, %fs:(%rax,%rsi,8)
	.section .t1960,"ax",@progbits
	testb %dl, (%rcx)
	.section .t1961,"ax",@progbits
	testb %dl, 0x10(%rcx)
	.section .t1962,"ax",@progbits
	testb %dl, -0x8(%rbp)
	.section .t1963,"ax",@progbits
	testb %dl, 0x12345(%rcx)
	.section .t1964,"ax",@progbits
	testb %dl, (%rax,%rsi,4)
	.section .t1965,"ax",@progbits
	testb %dl, 0x10(%rax,%rsi,8)
	.section .t1966,"ax",@progbits
	testb %dl, (,%rsi,2)
	.section .t1967,"ax",@progbits
	testb %dl, 0x40(%rip)
	.section .t1968,"ax",@progbits
	testb %dl, -0x100(%rip)
	.section .t1969,"ax",@progbits
	testb %dl, 0x1234
	.section .t1970,"ax",@progbits
	testb %dl, (%r8)
	.section .t1971,"ax",@progbits
	testb %dl, (%r12)
	.section .t1972,"ax",@progbits
	testb %dl, 0x8(%r13)
	.section .t1973,"ax",@progbits
	testb %dl, (%r8,%r15,2)
	.section .t1974,"ax",@progbits
	testb %dl, (%rax,%r12,4)
	.section .t1975,"ax",@progbits
	testb %dl, 0x100(%rbp)
	.section .t1976,"ax",@progbits
	testb %dl, (%rsp)
	.section .t1977,"ax",@progbits
	testb %dl, 0x10(%rsp,%rsi,4)
	.section .t1978,"ax",@progbits
	testb %dl, %gs:0x10(%rcx)
	.section .t1979,"ax",@progbits
	testb %dl, %fs:(%rax,%rsi,8)
	.section .t1980,"ax",@progbits
	testb %bl, (%rcx)
	.section .t1981,"ax",@progbits
	testb %bl, 0x10(%rcx)
	.section .t1982,"ax",@progbits
	testb %bl, -0x8(%rbp)
	.section .t1983,"ax",@progbits
	testb %bl, 0x12345(%rcx)
	.section .t1984,"ax",@progbits
	testb %bl, (%rax,%rsi,4)
	.section .t1985,"ax",@progbits
	testb %bl, 0x10(%rax,%rsi,8)
	.section .t1986,"ax",@progbits
	testb %bl, (,%rsi,2)
	.section .t1987,"ax",@progbits
	testb %bl, 0x40(%rip)
	.section .t1988,"ax",@progbits
	testb %bl, -0x100(%rip)
	.section .t1989,"ax",@progbits
	testb %bl, 0x1234
	.section .t1990,"ax",@progbits
	testb %bl, (%r8)
	.section .t1991,"ax",@progbits
	testb %bl, (%r12)
	.section .t1992,"ax",@progbits
	testb %bl, 0x8(%r13)
	.section .t1993,"ax",@progbits
	testb %bl, (%r8,%r15,2)
	.section .t1994,"ax",@progbits
	testb %bl, (%rax,%r12,4)
	.section .t1995,"ax",@progbits
	testb %bl, 0x100(%rbp)
	.section .t1996,"ax",@progbits
	testb %bl, (%rsp)
	.section .t1997,"ax",@progbits
	testb %bl, 0x10(%rsp,%rsi,4)
	.section .t1998,"ax",@progbits
	testb %bl, %gs:0x10(%rcx)
	.section .t1999,"ax",@progbits
	testb %bl, %fs:(%rax,%rsi,8)
	.section .t2000,"ax",@progbits
	testb %r9b, (%rcx)
	.section .t2001,"ax",@progbits
	testb %r9b, 0x10(%rcx)
	.section .t2002,"ax",@progbits
	testb %r9b, -0x8(%rbp)
	.section .t2003,"ax",@progbits
	testb %r9b, 0x12345(%rcx)
	.section .t2004,"ax",@progbits
	testb %r9b, (%rax,%rsi,4)
	.section .t2005,"ax",@progbits
	testb %r9b, 0x10(%rax,%rsi,8)
	.section .t2006,"ax",@progbits
	testb %r9b, (,%rsi,2)
	.section .t2007,"ax",@progbits
	testb %r9b, 0x40(%rip)
	.section .t2008,"ax",@progbits
	testb %r9b, -0x100(%rip)
	.section .t2009,"ax",@progbits
	testb %r9b, 0x1234
	.section .t2010,"ax",@progbits
	testb %r9b, (%r8)
	.section .t2011,"ax",@progbits
	testb %r9b, (%r12)
	.section .t2012,"ax",@progbits
	testb %r9b, 0x8(%r13)
	.section .t2013,"ax",@progbits
	testb %r9b, (%r8,%r15,2)
	.section .t2014,"ax",@progbits
	testb %r9b, (%rax,%r12,4)
	.section .t2015,"ax",@progbits
	testb %r9b, 0x100(%rbp)
	.section .t2016,"ax",@progbits
	testb %r9b, (%rsp)
	.section .t2017,"ax",@progbits
	testb %r9b, 0x10(%rsp,%rsi,4)
	.section .t2018,"ax",@progbits
	testb %r9b, %gs:0x10(%rcx)
	.section .t2019,"ax",@progbits
	testb %r9b, %fs:(%rax,%rsi,8)
	.section .t2020,"ax",@progbits
	testb %r13b, (%rcx)
	.section .t2021,"ax",@progbits
	testb %r13b, 0x10(%rcx)
	.section .t2022,"ax",@progbits
	testb %r13b, -0x8(%rbp)
	.section .t2023,"ax",@progbits
	testb %r13b, 0x12345(%rcx)
	.section .t2024,"ax",@progbits
	testb %r13b, (%rax,%rsi,4)
	.section .t2025,"ax",@progbits
	testb %r13b, 0x10(%rax,%rsi,8)
	.section .t2026,"ax",@progbits
	testb %r13b, (,%rsi,2)
	.section .t2027,"ax",@progbits
	testb %r13b, 0x40(%rip)
	.section .t2028,"ax",@progbits
	testb %r13b, -0x100(%rip)
	.section .t2029,"ax",@progbits
	testb %r13b, 0x1234
	.section .t2030,"ax",@progbits
	testb %r13b, (%r8)
	.section .t2031,"ax",@progbits
	testb %r13b, (%r12)
	.section .t2032,"ax",@progbits
	testb %r13b, 0x8(%r13)
	.section .t2033,"ax",@progbits
	testb %r13b, (%r8,%r15,2)
	.section .t2034,"ax",@progbits
	testb %r13b, (%rax,%r12,4)
	.section .t2035,"ax",@progbits
	testb %r13b, 0x100(%rbp)
	.section .t2036,"ax",@progbits
	testb %r13b, (%rsp)
	.section .t2037,"ax",@progbits
	testb %r13b, 0x10(%rsp,%rsi,4)
	.section .t2038,"ax",@progbits
	testb %r13b, %gs:0x10(%rcx)
	.section .t2039,"ax",@progbits
	testb %r13b, %fs:(%rax,%rsi,8)
	.section .t2040,"ax",@progbits
	testw %dx, (%rcx)
	.section .t2041,"ax",@progbits
	testw %dx, 0x10(%rcx)
	.section .t2042,"ax",@progbits
	testw %dx, -0x8(%rbp)
	.section .t2043,"ax",@progbits
	testw %dx, 0x12345(%rcx)
	.section .t2044,"ax",@progbits
	testw %dx, (%rax,%rsi,4)
	.section .t2045,"ax",@progbits
	testw %dx, 0x10(%rax,%rsi,8)
	.section .t2046,"ax",@progbits
	testw %dx, (,%rsi,2)
	.section .t2047,"ax",@progbits
	testw %dx, 0x40(%rip)
	.section .t2048,"ax",@progbits
	testw %dx, -0x100(%rip)
	.section .t2049,"ax",@progbits
	testw %dx, 0x1234
	.section .t2050,"ax",@progbits
	testw %dx, (%r8)
	.section .t2051,"ax",@progbits
	testw %dx, (%r12)
	.section .t2052,"ax",@progbits
	testw %dx, 0x8(%r13)
	.section .t2053,"ax",@progbits
	testw %dx, (%r8,%r15,2)
	.section .t2054,"ax",@progbits
	testw %dx, (%rax,%r12,4)
	.section .t2055,"ax",@progbits
	testw %dx, 0x100(%rbp)
	.section .t2056,"ax",@progbits
	testw %dx, (%rsp)
	.section .t2057,"ax",@progbits
	testw %dx, 0x10(%rsp,%rsi,4)
	.section .t2058,"ax",@progbits
	testw %dx, %gs:0x10(%rcx)
	.section .t2059,"ax",@progbits
	testw %dx, %fs:(%rax,%rsi,8)
	.section .t2060,"ax",@progbits
	testw %bx, (%rcx)
	.section .t2061,"ax",@progbits
	testw %bx, 0x10(%rcx)
	.section .t2062,"ax",@progbits
	testw %bx, -0x8(%rbp)
	.section .t2063,"ax",@progbits
	testw %bx, 0x12345(%rcx)
	.section .t2064,"ax",@progbits
	testw %bx, (%rax,%rsi,4)
	.section .t2065,"ax",@progbits
	testw %bx, 0x10(%rax,%rsi,8)
	.section .t2066,"ax",@progbits
	testw %bx, (,%rsi,2)
	.section .t2067,"ax",@progbits
	testw %bx, 0x40(%rip)
	.section .t2068,"ax",@progbits
	testw %bx, -0x100(%rip)
	.section .t2069,"ax",@progbits
	testw %bx, 0x1234
	.section .t2070,"ax",@progbits
	testw %bx, (%r8)
	.section .t2071,"ax",@progbits
	testw %bx, (%r12)
	.section .t2072,"ax",@progbits
	testw %bx, 0x8(%r13)
	.section .t2073,"ax",@progbits
	testw %bx, (%r8,%r15,2)
	.section .t2074,"ax",@progbits
	testw %bx, (%rax,%r12,4)
	.section .t2075,"ax",@progbits
	testw %bx, 0x100(%rbp)
	.section .t2076,"ax",@progbits
	testw %bx, (%rsp)
	.section .t2077,"ax",@progbits
	testw %bx, 0x10(%rsp,%rsi,4)
	.section .t2078,"ax",@progbits
	testw %bx, %gs:0x10(%rcx)
	.section .t2079,"ax",@progbits
	testw %bx, %fs:(%rax,%rsi,8)
	.section .t2080,"ax",@progbits
	testw %r9w, (%rcx)
	.section .t2081,"ax",@progbits
	testw %r9w, 0x10(%rcx)
	.section .t2082,"ax",@progbits
	testw %r9w, -0x8(%rbp)
	.section .t2083,"ax",@progbits
	testw %r9w, 0x12345(%rcx)
	.section .t2084,"ax",@progbits
	testw %r9w, (%rax,%rsi,4)
	.section .t2085,"ax",@progbits
	testw %r9w, 0x10(%rax,%rsi,8)
	.section .t2086,"ax",@progbits
	testw %r9w, (,%rsi,2)
	.section .t2087,"ax",@progbits
	testw %r9w, 0x40(%rip)
	.section .t2088,"ax",@progbits
	testw %r9w, -0x100(%rip)
	.section .t2089,"ax",@progbits
	testw %r9w, 0x1234
	.section .t2090,"ax",@progbits
	testw %r9w, (%r8)
	.section .t2091,"ax",@progbits
	testw %r9w, (%r12)
	.section .t2092,"ax",@progbits
	testw %r9w, 0x8(%r13)
	.section .t2093,"ax",@progbits
	testw %r9w, (%r8,%r15,2)
	.section .t2094,"ax",@progbits
	testw %r9w, (%rax,%r12,4)
	.section .t2095,"ax",@progbits
	testw %r9w, 0x100(%rbp)
	.section .t2096,"ax",@progbits
	testw %r9w, (%rsp)
	.section .t2097,"ax",@progbits
	testw %r9w, 0x10(%rsp,%rsi,4)
	.section .t2098,"ax",@progbits
	testw %r9w, %gs:0x10(%rcx)
	.section .t2099,"ax",@progbits
	testw %r9w, %fs:(%rax,%rsi,8)
	.section .t2100,"ax",@progbits
	testl %edx, (%rcx)
	.section .t2101,"ax",@progbits
	testl %edx, 0x10(%rcx)
	.section .t2102,"ax",@progbits
	testl %edx, -0x8(%rbp)
	.section .t2103,"ax",@progbits
	testl %edx, 0x12345(%rcx)
	.section .t2104,"ax",@progbits
	testl %edx, (%rax,%rsi,4)
	.section .t2105,"ax",@progbits
	testl %edx, 0x10(%rax,%rsi,8)
	.section .t2106,"ax",@progbits
	testl %edx, (,%rsi,2)
	.section .t2107,"ax",@progbits
	testl %edx, 0x40(%rip)
	.section .t2108,"ax",@progbits
	testl %edx, -0x100(%rip)
	.section .t2109,"ax",@progbits
	testl %edx, 0x1234
	.section .t2110,"ax",@progbits
	testl %edx, (%r8)
	.section .t2111,"ax",@progbits
	testl %edx, (%r12)
	.section .t2112,"ax",@progbits
	testl %edx, 0x8(%r13)
	.section .t2113,"ax",@progbits
	testl %edx, (%r8,%r15,2)
	.section .t2114,"ax",@progbits
	testl %edx, (%rax,%r12,4)
	.section .t2115,"ax",@progbits
	testl %edx, 0x100(%rbp)
	.section .t2116,"ax",@progbits
	testl %edx, (%rsp)
	.section .t2117,"ax",@progbits
	testl %edx, 0x10(%rsp,%rsi,4)
	.section .t2118,"ax",@progbits
	testl %edx, %gs:0x10(%rcx)
	.section .t2119,"ax",@progbits
	testl %edx, %fs:(%rax,%rsi,8)
	.section .t2120,"ax",@progbits
	testl %ebx, (%rcx)
	.section .t2121,"ax",@progbits
	testl %ebx, 0x10(%rcx)
	.section .t2122,"ax",@progbits
	testl %ebx, -0x8(%rbp)
	.section .t2123,"ax",@progbits
	testl %ebx, 0x12345(%rcx)
	.section .t2124,"ax",@progbits
	testl %ebx, (%rax,%rsi,4)
	.section .t2125,"ax",@progbits
	testl %ebx, 0x10(%rax,%rsi,8)
	.section .t2126,"ax",@progbits
	testl %ebx, (,%rsi,2)
	.section .t2127,"ax",@progbits
	testl %ebx, 0x40(%rip)
	.section .t2128,"ax",@progbits
	testl %ebx, -0x100(%rip)
	.section .t2129,"ax",@progbits
	testl %ebx, 0x1234
	.section .t2130,"ax",@progbits
	testl %ebx, (%r8)
	.section .t2131,"ax",@progbits
	testl %ebx, (%r12)
	.section .t2132,"ax",@progbits
	testl %ebx, 0x8(%r13)
	.section .t2133,"ax",@progbits
	testl %ebx, (%r8,%r15,2)
	.section .t2134,"ax",@progbits
	testl %ebx, (%rax,%r12,4)
	.section .t2135,"ax",@progbits
	testl %ebx, 0x100(%rbp)
	.section .t2136,"ax",@progbits
	testl %ebx, (%rsp)
	.section .t2137,"ax",@progbits
	testl %ebx, 0x10(%rsp,%rsi,4)
	.section .t2138,"ax",@progbits
	testl %ebx, %gs:0x10(%rcx)
	.section .t2139,"ax",@progbits
	testl %ebx, %fs:(%rax,%rsi,8)
	.section .t2140,"ax",@progbits
	testl %r9d, (%rcx)
	.section .t2141,"ax",@progbits
	testl %r9d, 0x10(%rcx)
	.section .t2142,"ax",@progbits
	testl %r9d, -0x8(%rbp)
	.section .t2143,"ax",@progbits
	testl %r9d, 0x12345(%rcx)
	.section .t2144,"ax",@progbits
	testl %r9d, (%rax,%rsi,4)
	.section .t2145,"ax",@progbits
	testl %r9d, 0x10(%rax,%rsi,8)
	.section .t2146,"ax",@progbits
	testl %r9d, (,%rsi,2)
	.section .t2147,"ax",@progbits
	testl %r9d, 0x40(%rip)
	.section .t2148,"ax",@progbits
	testl %r9d, -0x100(%rip)
	.section .t2149,"ax",@progbits
	testl %r9d, 0x1234
	.section .t2150,"ax",@progbits
	testl %r9d, (%r8)
	.section .t2151,"ax",@progbits
	testl %r9d, (%r12)
	.section .t2152,"ax",@progbits
	testl %r9d, 0x8(%r13)
	.section .t2153,"ax",@progbits
	testl %r9d, (%r8,%r15,2)
	.section .t2154,"ax",@progbits
	testl %r9d, (%rax,%r12,4)
	.section .t2155,"ax",@progbits
	testl %r9d, 0x100(%rbp)
	.section .t2156,"ax",@progbits
	testl %r9d, (%rsp)
	.section .t2157,"ax",@progbits
	testl %r9d, 0x10(%rsp,%rsi,4)
	.section .t2158,"ax",@progbits
	testl %r9d, %gs:0x10(%rcx)
	.section .t2159,"ax",@progbits
	testl %r9d, %fs:(%rax,%rsi,8)
	.section .t2160,"ax",@progbits
	testq %rdx, (%rcx)
	.section .t2161,"ax",@progbits
	testq %rdx, 0x10(%rcx)
	.section .t2162,"ax",@progbits
	testq %rdx, -0x8(%rbp)
	.section .t2163,"ax",@progbits
	testq %rdx, 0x12345(%rcx)
	.section .t2164,"ax",@progbits
	testq %rdx, (%rax,%rsi,4)
	.section .t2165,"ax",@progbits
	testq %rdx, 0x10(%rax,%rsi,8)
	.section .t2166,"ax",@progbits
	testq %rdx, (,%rsi,2)
	.section .t2167,"ax",@progbits
	testq %rdx, 0x40(%rip)
	.section .t2168,"ax",@progbits
	testq %rdx, -0x100(%rip)
	.section .t2169,"ax",@progbits
	testq %rdx, 0x1234
	.section .t2170,"ax",@progbits
	testq %rdx, (%r8)
	.section .t2171,"ax",@progbits
	testq %rdx, (%r12)
	.section .t2172,"ax",@progbits
	testq %rdx, 0x8(%r13)
	.section .t2173,"ax",@progbits
	testq %rdx, (%r8,%r15,2)
	.section .t2174,"ax",@progbits
	testq %rdx, (%rax,%r12,4)
	.section .t2175,"ax",@progbits
	testq %rdx, 0x100(%rbp)
	.section .t2176,"ax",@progbits
	testq %rdx, (%rsp)
	.section .t2177,"ax",@progbits
	testq %rdx, 0x10(%rsp,%rsi,4)
	.section .t2178,"ax",@progbits
	testq %rdx, %gs:0x10(%rcx)
	.section .t2179,"ax",@progbits
	testq %rdx, %fs:(%rax,%rsi,8)
	.section .t2180,"ax",@progbits
	testq %rbx, (%rcx)
	.section .t2181,"ax",@progbits
	testq %rbx, 0x10(%rcx)
	.section .t2182,"ax",@progbits
	testq %rbx, -0x8(%rbp)
	.section .t2183,"ax",@progbits
	testq %rbx, 0x12345(%rcx)
	.section .t2184,"ax",@progbits
	testq %rbx, (%rax,%rsi,4)
	.section .t2185,"ax",@progbits
	testq %rbx, 0x10(%rax,%rsi,8)
	.section .t2186,"ax",@progbits
	testq %rbx, (,%rsi,2)
	.section .t2187,"ax",@progbits
	testq %rbx, 0x40(%rip)
	.section .t2188,"ax",@progbits
	testq %rbx, -0x100(%rip)
	.section .t2189,"ax",@progbits
	testq %rbx, 0x1234
	.section .t2190,"ax",@progbits
	testq %rbx, (%r8)
	.section .t2191,"ax",@progbits
	testq %rbx, (%r12)
	.section .t2192,"ax",@progbits
	testq %rbx, 0x8(%r13)
	.section .t2193,"ax",@progbits
	testq %rbx, (%r8,%r15,2)
	.section .t2194,"ax",@progbits
	testq %rbx, (%rax,%r12,4)
	.section .t2195,"ax",@progbits
	testq %rbx, 0x100(%rbp)
	.section .t2196,"ax",@progbits
	testq %rbx, (%rsp)
	.section .t2197,"ax",@progbits
	testq %rbx, 0x10(%rsp,%rsi,4)
	.section .t2198,"ax",@progbits
	testq %rbx, %gs:0x10(%rcx)
	.section .t2199,"ax",@progbits
	testq %rbx, %fs:(%rax,%rsi,8)
	.section .t2200,"ax",@progbits
	testq %r9, (%rcx)
	.section .t2201,"ax",@progbits
	testq %r9, 0x10(%rcx)
	.section .t2202,"ax",@progbits
	testq %r9, -0x8(%rbp)
	.section .t2203,"ax",@progbits
	testq %r9, 0x12345(%rcx)
	.section .t2204,"ax",@progbits
	testq %r9, (%rax,%rsi,4)
	.section .t2205,"ax",@progbits
	testq %r9, 0x10(%rax,%rsi,8)
	.section .t2206,"ax",@progbits
	testq %r9, (,%rsi,2)
	.section .t2207,"ax",@progbits
	testq %r9, 0x40(%rip)
	.section .t2208,"ax",@progbits
	testq %r9, -0x100(%rip)
	.section .t2209,"ax",@progbits
	testq %r9, 0x1234
	.section .t2210,"ax",@progbits
	testq %r9, (%r8)
	.section .t2211,"ax",@progbits
	testq %r9, (%r12)
	.section .t2212,"ax",@progbits
	testq %r9, 0x8(%r13)
	.section .t2213,"ax",@progbits
	testq %r9, (%r8,%r15,2)
	.section .t2214,"ax",@progbits
	testq %r9, (%rax,%r12,4)
	.section .t2215,"ax",@progbits
	testq %r9, 0x100(%rbp)
	.section .t2216,"ax",@progbits
	testq %r9, (%rsp)
	.section .t2217,"ax",@progbits
	testq %r9, 0x10(%rsp,%rsi,4)
	.section .t2218,"ax",@progbits
	testq %r9, %gs:0x10(%rcx)
	.section .t2219,"ax",@progbits
	testq %r9, %fs:(%rax,%rsi,8)
	.section .t2220,"ax",@progbits
	testq %r12, (%rcx)
	.section .t2221,"ax",@progbits
	testq %r12, 0x10(%rcx)
	.section .t2222,"ax",@progbits
	testq %r12, -0x8(%rbp)
	.section .t2223,"ax",@progbits
	testq %r12, 0x12345(%rcx)
	.section .t2224,"ax",@progbits
	testq %r12, (%rax,%rsi,4)
	.section .t2225,"ax",@progbits
	testq %r12, 0x10(%rax,%rsi,8)
	.section .t2226,"ax",@progbits
	testq %r12, (,%rsi,2)
	.section .t2227,"ax",@progbits
	testq %r12, 0x40(%rip)
	.section .t2228,"ax",@progbits
	testq %r12, -0x100(%rip)
	.section .t2229,"ax",@progbits
	testq %r12, 0x1234
	.section .t2230,"ax",@progbits
	testq %r12, (%r8)
	.section .t2231,"ax",@progbits
	testq %r12, (%r12)
	.section .t2232,"ax",@progbits
	testq %r12, 0x8(%r13)
	.section .t2233,"ax",@progbits
	testq %r12, (%r8,%r15,2)
	.section .t2234,"ax",@progbits
	testq %r12, (%rax,%r12,4)
	.section .t2235,"ax",@progbits
	testq %r12, 0x100(%rbp)
	.section .t2236,"ax",@progbits
	testq %r12, (%rsp)
	.section .t2237,"ax",@progbits
	testq %r12, 0x10(%rsp,%rsi,4)
	.section .t2238,"ax",@progbits
	testq %r12, %gs:0x10(%rcx)
	.section .t2239,"ax",@progbits
	testq %r12, %fs:(%rax,%rsi,8)
	.section .t2240,"ax",@progbits
	cmpb %dl, (%rcx)
	.section .t2241,"ax",@progbits
	cmpb %dl, 0x10(%rcx)
	.section .t2242,"ax",@progbits
	cmpb %dl, -0x8(%rbp)
	.section .t2243,"ax",@progbits
	cmpb %dl, 0x12345(%rcx)
	.section .t2244,"ax",@progbits
	cmpb %dl, (%rax,%rsi,4)
	.section .t2245,"ax",@progbits
	cmpb %dl, 0x10(%rax,%rsi,8)
	.section .t2246,"ax",@progbits
	cmpb %dl, (,%rsi,2)
	.section .t2247,"ax",@progbits
	cmpb %dl, 0x40(%rip)
	.section .t2248,"ax",@progbits
	cmpb %dl, -0x100(%rip)
	.section .t2249,"ax",@progbits
	cmpb %dl, 0x1234
	.section .t2250,"ax",@progbits
	cmpb %dl, (%r8)
	.section .t2251,"ax",@progbits
	cmpb %dl, (%r12)
	.section .t2252,"ax",@progbits
	cmpb %dl, 0x8(%r13)
	.section .t2253,"ax",@progbits
	cmpb %dl, (%r8,%r15,2)
	.section .t2254,"ax",@progbits
	cmpb %dl, (%rax,%r12,4)
	.section .t2255,"ax",@progbits
	cmpb %dl, 0x100(%rbp)
	.section .t2256,"ax",@progbits
	cmpb %dl, (%rsp)
	.section .t2257,"ax",@progbits
	cmpb %dl, 0x10(%rsp,%rsi,4)
	.section .t2258,"ax",@progbits
	cmpb %dl, %gs:0x10(%rcx)
	.section .t2259,"ax",@progbits
	cmpb %dl, %fs:(%rax,%rsi,8)
	.section .t2260,"ax",@progbits
	cmpb %bl, (%rcx)
	.section .t2261,"ax",@progbits
	cmpb %bl, 0x10(%rcx)
	.section .t2262,"ax",@progbits
	cmpb %bl, -0x8(%rbp)
	.section .t2263,"ax",@progbits
	cmpb %bl, 0x12345(%rcx)
	.section .t2264,"ax",@progbits
	cmpb %bl, (%rax,%rsi,4)
	.section .t2265,"ax",@progbits
	cmpb %bl, 0x10(%rax,%rsi,8)
	.section .t2266,"ax",@progbits
	cmpb %bl, (,%rsi,2)
	.section .t2267,"ax",@progbits
	cmpb %bl, 0x40(%rip)
	.section .t2268,"ax",@progbits
	cmpb %bl, -0x100(%rip)
	.section .t2269,"ax",@progbits
	cmpb %bl, 0x1234
	.section .t2270,"ax",@progbits
	cmpb %bl, (%r8)
	.section .t2271,"ax",@progbits
	cmpb %bl, (%r12)
	.section .t2272,"ax",@progbits
	cmpb %bl, 0x8(%r13)
	.section .t2273,"ax",@progbits
	cmpb %bl, (%r8,%r15,2)
	.section .t2274,"ax",@progbits
	cmpb %bl, (%rax,%r12,4)
	.section .t2275,"ax",@progbits
	cmpb %bl, 0x100(%rbp)
	.section .t2276,"ax",@progbits
	cmpb %bl, (%rsp)
	.section .t2277,"ax",@progbits
	cmpb %bl, 0x10(%rsp,%rsi,4)
	.section .t2278,"ax",@progbits
	cmpb %bl, %gs:0x10(%rcx)
	.section .t2279,"ax",@progbits
	cmpb %bl, %fs:(%rax,%rsi,8)
	.section .t2280,"ax",@progbits
	cmpb %r9b, (%rcx)
	.section .t2281,"ax",@progbits
	cmpb %r9b, 0x10(%rcx)
	.section .t2282,"ax",@progbits
	cmpb %r9b, -0x8(%rbp)
	.section .t2283,"ax",@progbits
	cmpb %r9b, 0x12345(%rcx)
	.section .t2284,"ax",@progbits
	cmpb %r9b, (%rax,%rsi,4)
	.section .t2285,"ax",@progbits
	cmpb %r9b, 0x10(%rax,%rsi,8)
	.section .t2286,"ax",@progbits
	cmpb %r9b, (,%rsi,2)
	.section .t2287,"ax",@progbits
	cmpb %r9b, 0x40(%rip)
	.section .t2288,"ax",@progbits
	cmpb %r9b, -0x100(%rip)
	.section .t2289,"ax",@progbits
	cmpb %r9b, 0x1234
	.section .t2290,"ax",@progbits
	cmpb %r9b, (%r8)
	.section .t2291,"ax",@progbits
	cmpb %r9b, (%r12)
	.section .t2292,"ax",@progbits
	cmpb %r9b, 0x8(%r13)
	.section .t2293,"ax",@progbits
	cmpb %r9b, (%r8,%r15,2)
	.section .t2294,"ax",@progbits
	cmpb %r9b, (%rax,%r12,4)
	.section .t2295,"ax",@progbits
	cmpb %r9b, 0x100(%rbp)
	.section .t2296,"ax",@progbits
	cmpb %r9b, (%rsp)
	.section .t2297,"ax",@progbits
	cmpb %r9b, 0x10(%rsp,%rsi,4)
	.section .t2298,"ax",@progbits
	cmpb %r9b, %gs:0x10(%rcx)
	.section .t2299,"ax",@progbits
	cmpb %r9b, %fs:(%rax,%rsi,8)
	.section .t2300,"ax",@progbits
	cmpb %r13b, (%rcx)
	.section .t2301,"ax",@progbits
	cmpb %r13b, 0x10(%rcx)
	.section .t2302,"ax",@progbits
	cmpb %r13b, -0x8(%rbp)
	.section .t2303,"ax",@progbits
	cmpb %r13b, 0x12345(%rcx)
	.section .t2304,"ax",@progbits
	cmpb %r13b, (%rax,%rsi,4)
	.section .t2305,"ax",@progbits
	cmpb %r13b, 0x10(%rax,%rsi,8)
	.section .t2306,"ax",@progbits
	cmpb %r13b, (,%rsi,2)
	.section .t2307,"ax",@progbits
	cmpb %r13b, 0x40(%rip)
	.section .t2308,"ax",@progbits
	cmpb %r13b, -0x100(%rip)
	.section .t2309,"ax",@progbits
	cmpb %r13b, 0x1234
	.section .t2310,"ax",@progbits
	cmpb %r13b, (%r8)
	.section .t2311,"ax",@progbits
	cmpb %r13b, (%r12)
	.section .t2312,"ax",@progbits
	cmpb %r13b, 0x8(%r13)
	.section .t2313,"ax",@progbits
	cmpb %r13b, (%r8,%r15,2)
	.section .t2314,"ax",@progbits
	cmpb %r13b, (%rax,%r12,4)
	.section .t2315,"ax",@progbits
	cmpb %r13b, 0x100(%rbp)
	.section .t2316,"ax",@progbits
	cmpb %r13b, (%rsp)
	.section .t2317,"ax",@progbits
	cmpb %r13b, 0x10(%rsp,%rsi,4)
	.section .t2318,"ax",@progbits
	cmpb %r13b, %gs:0x10(%rcx)
	.section .t2319,"ax",@progbits
	cmpb %r13b, %fs:(%rax,%rsi,8)
	.section .t2320,"ax",@progbits
	cmpw %dx, (%rcx)
	.section .t2321,"ax",@progbits
	cmpw %dx, 0x10(%rcx)
	.section .t2322,"ax",@progbits
	cmpw %dx, -0x8(%rbp)
	.section .t2323,"ax",@progbits
	cmpw %dx, 0x12345(%rcx)
	.section .t2324,"ax",@progbits
	cmpw %dx, (%rax,%rsi,4)
	.section .t2325,"ax",@progbits
	cmpw %dx, 0x10(%rax,%rsi,8)
	.section .t2326,"ax",@progbits
	cmpw %dx, (,%rsi,2)
	.section .t2327,"ax",@progbits
	cmpw %dx, 0x40(%rip)
	.section .t2328,"ax",@progbits
	cmpw %dx, -0x100(%rip)
	.section .t2329,"ax",@progbits
	cmpw %dx, 0x1234
	.section .t2330,"ax",@progbits
	cmpw %dx, (%r8)
	.section .t2331,"ax",@progbits
	cmpw %dx, (%r12)
	.section .t2332,"ax",@progbits
	cmpw %dx, 0x8(%r13)
	.section .t2333,"ax",@progbits
	cmpw %dx, (%r8,%r15,2)
	.section .t2334,"ax",@progbits
	cmpw %dx, (%rax,%r12,4)
	.section .t2335,"ax",@progbits
	cmpw %dx, 0x100(%rbp)
	.section .t2336,"ax",@progbits
	cmpw %dx, (%rsp)
	.section .t2337,"ax",@progbits
	cmpw %dx, 0x10(%rsp,%rsi,4)
	.section .t2338,"ax",@progbits
	cmpw %dx, %gs:0x10(%rcx)
	.section .t2339,"ax",@progbits
	cmpw %dx, %fs:(%rax,%rsi,8)
	.section .t2340,"ax",@progbits
	cmpw %bx, (%rcx)
	.section .t2341,"ax",@progbits
	cmpw %bx, 0x10(%rcx)
	.section .t2342,"ax",@progbits
	cmpw %bx, -0x8(%rbp)
	.section .t2343,"ax",@progbits
	cmpw %bx, 0x12345(%rcx)
	.section .t2344,"ax",@progbits
	cmpw %bx, (%rax,%rsi,4)
	.section .t2345,"ax",@progbits
	cmpw %bx, 0x10(%rax,%rsi,8)
	.section .t2346,"ax",@progbits
	cmpw %bx, (,%rsi,2)
	.section .t2347,"ax",@progbits
	cmpw %bx, 0x40(%rip)
	.section .t2348,"ax",@progbits
	cmpw %bx, -0x100(%rip)
	.section .t2349,"ax",@progbits
	cmpw %bx, 0x1234
	.section .t2350,"ax",@progbits
	cmpw %bx, (%r8)
	.section .t2351,"ax",@progbits
	cmpw %bx, (%r12)
	.section .t2352,"ax",@progbits
	cmpw %bx, 0x8(%r13)
	.section .t2353,"ax",@progbits
	cmpw %bx, (%r8,%r15,2)
	.section .t2354,"ax",@progbits
	cmpw %bx, (%rax,%r12,4)
	.section .t2355,"ax",@progbits
	cmpw %bx, 0x100(%rbp)
	.section .t2356,"ax",@progbits
	cmpw %bx, (%rsp)
	.section .t2357,"ax",@progbits
	cmpw %bx, 0x10(%rsp,%rsi,4)
	.section .t2358,"ax",@progbits
	cmpw %bx, %gs:0x10(%rcx)
	.section .t2359,"ax",@progbits
	cmpw %bx, %fs:(%rax,%rsi,8)
	.section .t2360,"ax",@progbits
	cmpw %r9w, (%rcx)
	.section .t2361,"ax",@progbits
	cmpw %r9w, 0x10(%rcx)
	.section .t2362,"ax",@progbits
	cmpw %r9w, -0x8(%rbp)
	.section .t2363,"ax",@progbits
	cmpw %r9w, 0x12345(%rcx)
	.section .t2364,"ax",@progbits
	cmpw %r9w, (%rax,%rsi,4)
	.section .t2365,"ax",@progbits
	cmpw %r9w, 0x10(%rax,%rsi,8)
	.section .t2366,"ax",@progbits
	cmpw %r9w, (,%rsi,2)
	.section .t2367,"ax",@progbits
	cmpw %r9w, 0x40(%rip)
	.section .t2368,"ax",@progbits
	cmpw %r9w, -0x100(%rip)
	.section .t2369,"ax",@progbits
	cmpw %r9w, 0x1234
	.section .t2370,"ax",@progbits
	cmpw %r9w, (%r8)
	.section .t2371,"ax",@progbits
	cmpw %r9w, (%r12)
	.section .t2372,"ax",@progbits
	cmpw %r9w, 0x8(%r13)
	.section .t2373,"ax",@progbits
	cmpw %r9w, (%r8,%r15,2)
	.section .t2374,"ax",@progbits
	cmpw %r9w, (%rax,%r12,4)
	.section .t2375,"ax",@progbits
	cmpw %r9w, 0x100(%rbp)
	.section .t2376,"ax",@progbits
	cmpw %r9w, (%rsp)
	.section .t2377,"ax",@progbits
	cmpw %r9w, 0x10(%rsp,%rsi,4)
	.section .t2378,"ax",@progbits
	cmpw %r9w, %gs:0x10(%rcx)
	.section .t2379,"ax",@progbits
	cmpw %r9w, %fs:(%rax,%rsi,8)
	.section .t2380,"ax",@progbits
	cmpl %edx, (%rcx)
	.section .t2381,"ax",@progbits
	cmpl %edx, 0x10(%rcx)
	.section .t2382,"ax",@progbits
	cmpl %edx, -0x8(%rbp)
	.section .t2383,"ax",@progbits
	cmpl %edx, 0x12345(%rcx)
	.section .t2384,"ax",@progbits
	cmpl %edx, (%rax,%rsi,4)
	.section .t2385,"ax",@progbits
	cmpl %edx, 0x10(%rax,%rsi,8)
	.section .t2386,"ax",@progbits
	cmpl %edx, (,%rsi,2)
	.section .t2387,"ax",@progbits
	cmpl %edx, 0x40(%rip)
	.section .t2388,"ax",@progbits
	cmpl %edx, -0x100(%rip)
	.section .t2389,"ax",@progbits
	cmpl %edx, 0x1234
	.section .t2390,"ax",@progbits
	cmpl %edx, (%r8)
	.section .t2391,"ax",@progbits
	cmpl %edx, (%r12)
	.section .t2392,"ax",@progbits
	cmpl %edx, 0x8(%r13)
	.section .t2393,"ax",@progbits
	cmpl %edx, (%r8,%r15,2)
	.section .t2394,"ax",@progbits
	cmpl %edx, (%rax,%r12,4)
	.section .t2395,"ax",@progbits
	cmpl %edx, 0x100(%rbp)
	.section .t2396,"ax",@progbits
	cmpl %edx, (%rsp)
	.section .t2397,"ax",@progbits
	cmpl %edx, 0x10(%rsp,%rsi,4)
	.section .t2398,"ax",@progbits
	cmpl %edx, %gs:0x10(%rcx)
	.section .t2399,"ax",@progbits
	cmpl %edx, %fs:(%rax,%rsi,8)
	.section .t2400,"ax",@progbits
	cmpl %ebx, (%rcx)
	.section .t2401,"ax",@progbits
	cmpl %ebx, 0x10(%rcx)
	.section .t2402,"ax",@progbits
	cmpl %ebx, -0x8(%rbp)
	.section .t2403,"ax",@progbits
	cmpl %ebx, 0x12345(%rcx)
	.section .t2404,"ax",@progbits
	cmpl %ebx, (%rax,%rsi,4)
	.section .t2405,"ax",@progbits
	cmpl %ebx, 0x10(%rax,%rsi,8)
	.section .t2406,"ax",@progbits
	cmpl %ebx, (,%rsi,2)
	.section .t2407,"ax",@progbits
	cmpl %ebx, 0x40(%rip)
	.section .t2408,"ax",@progbits
	cmpl %ebx, -0x100(%rip)
	.section .t2409,"ax",@progbits
	cmpl %ebx, 0x1234
	.section .t2410,"ax",@progbits
	cmpl %ebx, (%r8)
	.section .t2411,"ax",@progbits
	cmpl %ebx, (%r12)
	.section .t2412,"ax",@progbits
	cmpl %ebx, 0x8(%r13)
	.section .t2413,"ax",@progbits
	cmpl %ebx, (%r8,%r15,2)
	.section .t2414,"ax",@progbits
	cmpl %ebx, (%rax,%r12,4)
	.section .t2415,"ax",@progbits
	cmpl %ebx, 0x100(%rbp)
	.section .t2416,"ax",@progbits
	cmpl %ebx, (%rsp)
	.section .t2417,"ax",@progbits
	cmpl %ebx, 0x10(%rsp,%rsi,4)
	.section .t2418,"ax",@progbits
	cmpl %ebx, %gs:0x10(%rcx)
	.section .t2419,"ax",@progbits
	cmpl %ebx, %fs:(%rax,%rsi,8)
	.section .t2420,"ax",@progbits
	cmpl %r9d, (%rcx)
	.section .t2421,"ax",@progbits
	cmpl %r9d, 0x10(%rcx)
	.section .t2422,"ax",@progbits
	cmpl %r9d, -0x8(%rbp)
	.section .t2423,"ax",@progbits
	cmpl %r9d, 0x12345(%rcx)
	.section .t2424,"ax",@progbits
	cmpl %r9d, (%rax,%rsi,4)
	.section .t2425,"ax",@progbits
	cmpl %r9d, 0x10(%rax,%rsi,8)
	.section .t2426,"ax",@progbits
	cmpl %r9d, (,%rsi,2)
	.section .t2427,"ax",@progbits
	cmpl %r9d, 0x40(%rip)
	.section .t2428,"ax",@progbits
	cmpl %r9d, -0x100(%rip)
	.section .t2429,"ax",@progbits
	cmpl %r9d, 0x1234
	.section .t2430,"ax",@progbits
	cmpl %r9d, (%r8)
	.section .t2431,"ax",@progbits
	cmpl %r9d, (%r12)
	.section .t2432,"ax",@progbits
	cmpl %r9d, 0x8(%r13)
	.section .t2433,"ax",@progbits
	cmpl %r9d, (%r8,%r15,2)
	.section .t2434,"ax",@progbits
	cmpl %r9d, (%rax,%r12,4)
	.section .t2435,"ax",@progbits
	cmpl %r9d, 0x100(%rbp)
	.section .t2436,"ax",@progbits
	cmpl %r9d, (%rsp)
	.section .t2437,"ax",@progbits
	cmpl %r9d, 0x10(%rsp,%rsi,4)
	.section .t2438,"ax",@progbits
	cmpl %r9d, %gs:0x10(%rcx)
	.section .t2439,"ax",@progbits
	cmpl %r9d, %fs:(%rax,%rsi,8)
	.section .t2440,"ax",@progbits
	cmpq %rdx, (%rcx)
	.section .t2441,"ax",@progbits
	cmpq %rdx, 0x10(%rcx)
	.section .t2442,"ax",@progbits
	cmpq %rdx, -0x8(%rbp)
	.section .t2443,"ax",@progbits
	cmpq %rdx, 0x12345(%rcx)
	.section .t2444,"ax",@progbits
	cmpq %rdx, (%rax,%rsi,4)
	.section .t2445,"ax",@progbits
	cmpq %rdx, 0x10(%rax,%rsi,8)
	.section .t2446,"ax",@progbits
	cmpq %rdx, (,%rsi,2)
	.section .t2447,"ax",@progbits
	cmpq %rdx, 0x40(%rip)
	.section .t2448,"ax",@progbits
	cmpq %rdx, -0x100(%rip)
	.section .t2449,"ax",@progbits
	cmpq %rdx, 0x1234
	.section .t2450,"ax",@progbits
	cmpq %rdx, (%r8)
	.section .t2451,"ax",@progbits
	cmpq %rdx, (%r12)
	.section .t2452,"ax",@progbits
	cmpq %rdx, 0x8(%r13)
	.section .t2453,"ax",@progbits
	cmpq %rdx, (%r8,%r15,2)
	.section .t2454,"ax",@progbits
	cmpq %rdx, (%rax,%r12,4)
	.section .t2455,"ax",@progbits
	cmpq %rdx, 0x100(%rbp)
	.section .t2456,"ax",@progbits
	cmpq %rdx, (%rsp)
	.section .t2457,"ax",@progbits
	cmpq %rdx, 0x10(%rsp,%rsi,4)
	.section .t2458,"ax",@progbits
	cmpq %rdx, %gs:0x10(%rcx)
	.section .t2459,"ax",@progbits
	cmpq %rdx, %fs:(%rax,%rsi,8)
	.section .t2460,"ax",@progbits
	cmpq %rbx, (%rcx)
	.section .t2461,"ax",@progbits
	cmpq %rbx, 0x10(%rcx)
	.section .t2462,"ax",@progbits
	cmpq %rbx, -0x8(%rbp)
	.section .t2463,"ax",@progbits
	cmpq %rbx, 0x12345(%rcx)
	.section .t2464,"ax",@progbits
	cmpq %rbx, (%rax,%rsi,4)
	.section .t2465,"ax",@progbits
	cmpq %rbx, 0x10(%rax,%rsi,8)
	.section .t2466,"ax",@progbits
	cmpq %rbx, (,%rsi,2)
	.section .t2467,"ax",@progbits
	cmpq %rbx, 0x40(%rip)
	.section .t2468,"ax",@progbits
	cmpq %rbx, -0x100(%rip)
	.section .t2469,"ax",@progbits
	cmpq %rbx, 0x1234
	.section .t2470,"ax",@progbits
	cmpq %rbx, (%r8)
	.section .t2471,"ax",@progbits
	cmpq %rbx, (%r12)
	.section .t2472,"ax",@progbits
	cmpq %rbx, 0x8(%r13)
	.section .t2473,"ax",@progbits
	cmpq %rbx, (%r8,%r15,2)
	.section .t2474,"ax",@progbits
	cmpq %rbx, (%rax,%r12,4)
	.section .t2475,"ax",@progbits
	cmpq %rbx, 0x100(%rbp)
	.section .t2476,"ax",@progbits
	cmpq %rbx, (%rsp)
	.section .t2477,"ax",@progbits
	cmpq %rbx, 0x10(%rsp,%rsi,4)
	.section .t2478,"ax",@progbits
	cmpq %rbx, %gs:0x10(%rcx)
	.section .t2479,"ax",@progbits
	cmpq %rbx, %fs:(%rax,%rsi,8)
	.section .t2480,"ax",@progbits
	cmpq %r9, (%rcx)
	.section .t2481,"ax",@progbits
	cmpq %r9, 0x10(%rcx)
	.section .t2482,"ax",@progbits
	cmpq %r9, -0x8(%rbp)
	.section .t2483,"ax",@progbits
	cmpq %r9, 0x12345(%rcx)
	.section .t2484,"ax",@progbits
	cmpq %r9, (%rax,%rsi,4)
	.section .t2485,"ax",@progbits
	cmpq %r9, 0x10(%rax,%rsi,8)
	.section .t2486,"ax",@progbits
	cmpq %r9, (,%rsi,2)
	.section .t2487,"ax",@progbits
	cmpq %r9, 0x40(%rip)
	.section .t2488,"ax",@progbits
	cmpq %r9, -0x100(%rip)
	.section .t2489,"ax",@progbits
	cmpq %r9, 0x1234
	.section .t2490,"ax",@progbits
	cmpq %r9, (%r8)
	.section .t2491,"ax",@progbits
	cmpq %r9, (%r12)
	.section .t2492,"ax",@progbits
	cmpq %r9, 0x8(%r13)
	.section .t2493,"ax",@progbits
	cmpq %r9, (%r8,%r15,2)
	.section .t2494,"ax",@progbits
	cmpq %r9, (%rax,%r12,4)
	.section .t2495,"ax",@progbits
	cmpq %r9, 0x100(%rbp)
	.section .t2496,"ax",@progbits
	cmpq %r9, (%rsp)
	.section .t2497,"ax",@progbits
	cmpq %r9, 0x10(%rsp,%rsi,4)
	.section .t2498,"ax",@progbits
	cmpq %r9, %gs:0x10(%rcx)
	.section .t2499,"ax",@progbits
	cmpq %r9, %fs:(%rax,%rsi,8)
	.section .t2500,"ax",@progbits
	cmpq %r12, (%rcx)
	.section .t2501,"ax",@progbits
	cmpq %r12, 0x10(%rcx)
	.section .t2502,"ax",@progbits
	cmpq %r12, -0x8(%rbp)
	.section .t2503,"ax",@progbits
	cmpq %r12, 0x12345(%rcx)
	.section .t2504,"ax",@progbits
	cmpq %r12, (%rax,%rsi,4)
	.section .t2505,"ax",@progbits
	cmpq %r12, 0x10(%rax,%rsi,8)
	.section .t2506,"ax",@progbits
	cmpq %r12, (,%rsi,2)
	.section .t2507,"ax",@progbits
	cmpq %r12, 0x40(%rip)
	.section .t2508,"ax",@progbits
	cmpq %r12, -0x100(%rip)
	.section .t2509,"ax",@progbits
	cmpq %r12, 0x1234
	.section .t2510,"ax",@progbits
	cmpq %r12, (%r8)
	.section .t2511,"ax",@progbits
	cmpq %r12, (%r12)
	.section .t2512,"ax",@progbits
	cmpq %r12, 0x8(%r13)
	.section .t2513,"ax",@progbits
	cmpq %r12, (%r8,%r15,2)
	.section .t2514,"ax",@progbits
	cmpq %r12, (%rax,%r12,4)
	.section .t2515,"ax",@progbits
	cmpq %r12, 0x100(%rbp)
	.section .t2516,"ax",@progbits
	cmpq %r12, (%rsp)
	.section .t2517,"ax",@progbits
	cmpq %r12, 0x10(%rsp,%rsi,4)
	.section .t2518,"ax",@progbits
	cmpq %r12, %gs:0x10(%rcx)
	.section .t2519,"ax",@progbits
	cmpq %r12, %fs:(%rax,%rsi,8)
	.section .t2520,"ax",@progbits
	xaddb %dl, (%rcx)
	.section .t2521,"ax",@progbits
	xaddb %dl, 0x10(%rcx)
	.section .t2522,"ax",@progbits
	xaddb %dl, -0x8(%rbp)
	.section .t2523,"ax",@progbits
	xaddb %dl, 0x12345(%rcx)
	.section .t2524,"ax",@progbits
	xaddb %dl, (%rax,%rsi,4)
	.section .t2525,"ax",@progbits
	xaddb %dl, 0x10(%rax,%rsi,8)
	.section .t2526,"ax",@progbits
	xaddb %dl, (,%rsi,2)
	.section .t2527,"ax",@progbits
	xaddb %dl, 0x40(%rip)
	.section .t2528,"ax",@progbits
	xaddb %dl, -0x100(%rip)
	.section .t2529,"ax",@progbits
	xaddb %dl, 0x1234
	.section .t2530,"ax",@progbits
	xaddb %dl, (%r8)
	.section .t2531,"ax",@progbits
	xaddb %dl, (%r12)
	.section .t2532,"ax",@progbits
	xaddb %dl, 0x8(%r13)
	.section .t2533,"ax",@progbits
	xaddb %dl, (%r8,%r15,2)
	.section .t2534,"ax",@progbits
	xaddb %dl, (%rax,%r12,4)
	.section .t2535,"ax",@progbits
	xaddb %dl, 0x100(%rbp)
	.section .t2536,"ax",@progbits
	xaddb %dl, (%rsp)
	.section .t2537,"ax",@progbits
	xaddb %dl, 0x10(%rsp,%rsi,4)
	.section .t2538,"ax",@progbits
	xaddb %dl, %gs:0x10(%rcx)
	.section .t2539,"ax",@progbits
	xaddb %dl, %fs:(%rax,%rsi,8)
	.section .t2540,"ax",@progbits
	xaddb %bl, (%rcx)
	.section .t2541,"ax",@progbits
	xaddb %bl, 0x10(%rcx)
	.section .t2542,"ax",@progbits
	xaddb %bl, -0x8(%rbp)
	.section .t2543,"ax",@progbits
	xaddb %bl, 0x12345(%rcx)
	.section .t2544,"ax",@progbits
	xaddb %bl, (%rax,%rsi,4)
	.section .t2545,"ax",@progbits
	xaddb %bl, 0x10(%rax,%rsi,8)
	.section .t2546,"ax",@progbits
	xaddb %bl, (,%rsi,2)
	.section .t2547,"ax",@progbits
	xaddb %bl, 0x40(%rip)
	.section .t2548,"ax",@progbits
	xaddb %bl, -0x100(%rip)
	.section .t2549,"ax",@progbits
	xaddb %bl, 0x1234
	.section .t2550,"ax",@progbits
	xaddb %bl, (%r8)
	.section .t2551,"ax",@progbits
	xaddb %bl, (%r12)
	.section .t2552,"ax",@progbits
	xaddb %bl, 0x8(%r13)
	.section .t2553,"ax",@progbits
	xaddb %bl, (%r8,%r15,2)
	.section .t2554,"ax",@progbits
	xaddb %bl, (%rax,%r12,4)
	.section .t2555,"ax",@progbits
	xaddb %bl, 0x100(%rbp)
	.section .t2556,"ax",@progbits
	xaddb %bl, (%rsp)
	.section .t2557,"ax",@progbits
	xaddb %bl, 0x10(%rsp,%rsi,4)
	.section .t2558,"ax",@progbits
	xaddb %bl, %gs:0x10(%rcx)
	.section .t2559,"ax",@progbits
	xaddb %bl, %fs:(%rax,%rsi,8)
	.section .t2560,"ax",@progbits
	xaddb %r9b, (%rcx)
	.section .t2561,"ax",@progbits
	xaddb %r9b, 0x10(%rcx)
	.section .t2562,"ax",@progbits
	xaddb %r9b, -0x8(%rbp)
	.section .t2563,"ax",@progbits
	xaddb %r9b, 0x12345(%rcx)
	.section .t2564,"ax",@progbits
	xaddb %r9b, (%rax,%rsi,4)
	.section .t2565,"ax",@progbits
	xaddb %r9b, 0x10(%rax,%rsi,8)
	.section .t2566,"ax",@progbits
	xaddb %r9b, (,%rsi,2)
	.section .t2567,"ax",@progbits
	xaddb %r9b, 0x40(%rip)
	.section .t2568,"ax",@progbits
	xaddb %r9b, -0x100(%rip)
	.section .t2569,"ax",@progbits
	xaddb %r9b, 0x1234
	.section .t2570,"ax",@progbits
	xaddb %r9b, (%r8)
	.section .t2571,"ax",@progbits
	xaddb %r9b, (%r12)
	.section .t2572,"ax",@progbits
	xaddb %r9b, 0x8(%r13)
	.section .t2573,"ax",@progbits
	xaddb %r9b, (%r8,%r15,2)
	.section .t2574,"ax",@progbits
	xaddb %r9b, (%rax,%r12,4)
	.section .t2575,"ax",@progbits
	xaddb %r9b, 0x100(%rbp)
	.section .t2576,"ax",@progbits
	xaddb %r9b, (%rsp)
	.section .t2577,"ax",@progbits
	xaddb %r9b, 0x10(%rsp,%rsi,4)
	.section .t2578,"ax",@progbits
	xaddb %r9b, %gs:0x10(%rcx)
	.section .t2579,"ax",@progbits
	xaddb %r9b, %fs:(%rax,%rsi,8)
	.section .t2580,"ax",@progbits
	xaddb %r13b, (%rcx)
	.section .t2581,"ax",@progbits
	xaddb %r13b, 0x10(%rcx)
	.section .t2582,"ax",@progbits
	xaddb %r13b, -0x8(%rbp)
	.section .t2583,"ax",@progbits
	xaddb %r13b, 0x12345(%rcx)
	.section .t2584,"ax",@progbits
	xaddb %r13b, (%rax,%rsi,4)
	.section .t2585,"ax",@progbits
	xaddb %r13b, 0x10(%rax,%rsi,8)
	.section .t2586,"ax",@progbits
	xaddb %r13b, (,%rsi,2)
	.section .t2587,"ax",@progbits
	xaddb %r13b, 0x40(%rip)
	.section .t2588,"ax",@progbits
	xaddb %r13b, -0x100(%rip)
	.section .t2589,"ax",@progbits
	xaddb %r13b, 0x1234
	.section .t2590,"ax",@progbits
	xaddb %r13b, (%r8)
	.section .t2591,"ax",@progbits
	xaddb %r13b, (%r12)
	.section .t2592,"ax",@progbits
	xaddb %r13b, 0x8(%r13)
	.section .t2593,"ax",@progbits
	xaddb %r13b, (%r8,%r15,2)
	.section .t2594,"ax",@progbits
	xaddb %r13b, (%rax,%r12,4)
	.section .t2595,"ax",@progbits
	xaddb %r13b, 0x100(%rbp)
	.section .t2596,"ax",@progbits
	xaddb %r13b, (%rsp)
	.section .t2597,"ax",@progbits
	xaddb %r13b, 0x10(%rsp,%rsi,4)
	.section .t2598,"ax",@progbits
	xaddb %r13b, %gs:0x10(%rcx)
	.section .t2599,"ax",@progbits
	xaddb %r13b, %fs:(%rax,%rsi,8)
	.section .t2600,"ax",@progbits
	xaddw %dx, (%rcx)
	.section .t2601,"ax",@progbits
	xaddw %dx, 0x10(%rcx)
	.section .t2602,"ax",@progbits
	xaddw %dx, -0x8(%rbp)
	.section .t2603,"ax",@progbits
	xaddw %dx, 0x12345(%rcx)
	.section .t2604,"ax",@progbits
	xaddw %dx, (%rax,%rsi,4)
	.section .t2605,"ax",@progbits
	xaddw %dx, 0x10(%rax,%rsi,8)
	.section .t2606,"ax",@progbits
	xaddw %dx, (,%rsi,2)
	.section .t2607,"ax",@progbits
	xaddw %dx, 0x40(%rip)
	.section .t2608,"ax",@progbits
	xaddw %dx, -0x100(%rip)
	.section .t2609,"ax",@progbits
	xaddw %dx, 0x1234
	.section .t2610,"ax",@progbits
	xaddw %dx, (%r8)
	.section .t2611,"ax",@progbits
	xaddw %dx, (%r12)
	.section .t2612,"ax",@progbits
	xaddw %dx, 0x8(%r13)
	.section .t2613,"ax",@progbits
	xaddw %dx, (%r8,%r15,2)
	.section .t2614,"ax",@progbits
	xaddw %dx, (%rax,%r12,4)
	.section .t2615,"ax",@progbits
	xaddw %dx, 0x100(%rbp)
	.section .t2616,"ax",@progbits
	xaddw %dx, (%rsp)
	.section .t2617,"ax",@progbits
	xaddw %dx, 0x10(%rsp,%rsi,4)
	.section .t2618,"ax",@progbits
	xaddw %dx, %gs:0x10(%rcx)
	.section .t2619,"ax",@progbits
	xaddw %dx, %fs:(%rax,%rsi,8)
	.section .t2620,"ax",@progbits
	xaddw %bx, (%rcx)
	.section .t2621,"ax",@progbits
	xaddw %bx, 0x10(%rcx)
	.section .t2622,"ax",@progbits
	xaddw %bx, -0x8(%rbp)
	.section .t2623,"ax",@progbits
	xaddw %bx, 0x12345(%rcx)
	.section .t2624,"ax",@progbits
	xaddw %bx, (%rax,%rsi,4)
	.section .t2625,"ax",@progbits
	xaddw %bx, 0x10(%rax,%rsi,8)
	.section .t2626,"ax",@progbits
	xaddw %bx, (,%rsi,2)
	.section .t2627,"ax",@progbits
	xaddw %bx, 0x40(%rip)
	.section .t2628,"ax",@progbits
	xaddw %bx, -0x100(%rip)
	.section .t2629,"ax",@progbits
	xaddw %bx, 0x1234
	.section .t2630,"ax",@progbits
	xaddw %bx, (%r8)
	.section .t2631,"ax",@progbits
	xaddw %bx, (%r12)
	.section .t2632,"ax",@progbits
	xaddw %bx, 0x8(%r13)
	.section .t2633,"ax",@progbits
	xaddw %bx, (%r8,%r15,2)
	.section .t2634,"ax",@progbits
	xaddw %bx, (%rax,%r12,4)
	.section .t2635,"ax",@progbits
	xaddw %bx, 0x100(%rbp)
	.section .t2636,"ax",@progbits
	xaddw %bx, (%rsp)
	.section .t2637,"ax",@progbits
	xaddw %bx, 0x10(%rsp,%rsi,4)
	.section .t2638,"ax",@progbits
	xaddw %bx, %gs:0x10(%rcx)
	.section .t2639,"ax",@progbits
	xaddw %bx, %fs:(%rax,%rsi,8)
	.section .t2640,"ax",@progbits
	xaddw %r9w, (%rcx)
	.section .t2641,"ax",@progbits
	xaddw %r9w, 0x10(%rcx)
	.section .t2642,"ax",@progbits
	xaddw %r9w, -0x8(%rbp)
	.section .t2643,"ax",@progbits
	xaddw %r9w, 0x12345(%rcx)
	.section .t2644,"ax",@progbits
	xaddw %r9w, (%rax,%rsi,4)
	.section .t2645,"ax",@progbits
	xaddw %r9w, 0x10(%rax,%rsi,8)
	.section .t2646,"ax",@progbits
	xaddw %r9w, (,%rsi,2)
	.section .t2647,"ax",@progbits
	xaddw %r9w, 0x40(%rip)
	.section .t2648,"ax",@progbits
	xaddw %r9w, -0x100(%rip)
	.section .t2649,"ax",@progbits
	xaddw %r9w, 0x1234
	.section .t2650,"ax",@progbits
	xaddw %r9w, (%r8)
	.section .t2651,"ax",@progbits
	xaddw %r9w, (%r12)
	.section .t2652,"ax",@progbits
	xaddw %r9w, 0x8(%r13)
	.section .t2653,"ax",@progbits
	xaddw %r9w, (%r8,%r15,2)
	.section .t2654,"ax",@progbits
	xaddw %r9w, (%rax,%r12,4)
	.section .t2655,"ax",@progbits
	xaddw %r9w, 0x100(%rbp)
	.section .t2656,"ax",@progbits
	xaddw %r9w, (%rsp)
	.section .t2657,"ax",@progbits
	xaddw %r9w, 0x10(%rsp,%rsi,4)
	.section .t2658,"ax",@progbits
	xaddw %r9w, %gs:0x10(%rcx)
	.section .t2659,"ax",@progbits
	xaddw %r9w, %fs:(%rax,%rsi,8)
	.section .t2660,"ax",@progbits
	xaddl %edx, (%rcx)
	.section .t2661,"ax",@progbits
	xaddl %edx, 0x10(%rcx)
	.section .t2662,"ax",@progbits
	xaddl %edx, -0x8(%rbp)
	.section .t2663,"ax",@progbits
	xaddl %edx, 0x12345(%rcx)
	.section .t2664,"ax",@progbits
	xaddl %edx, (%rax,%rsi,4)
	.section .t2665,"ax",@progbits
	xaddl %edx, 0x10(%rax,%rsi,8)
	.section .t2666,"ax",@progbits
	xaddl %edx, (,%rsi,2)
	.section .t2667,"ax",@progbits
	xaddl %edx, 0x40(%rip)
	.section .t2668,"ax",@progbits
	xaddl %edx, -0x100(%rip)
	.section .t2669,"ax",@progbits
	xaddl %edx, 0x1234
	.section .t2670,"ax",@progbits
	xaddl %edx, (%r8)
	.section .t2671,"ax",@progbits
	xaddl %edx, (%r12)
	.section .t2672,"ax",@progbits
	xaddl %edx, 0x8(%r13)
	.section .t2673,"ax",@progbits
	xaddl %edx, (%r8,%r15,2)
	.section .t2674,"ax",@progbits
	xaddl %edx, (%rax,%r12,4)
	.section .t2675,"ax",@progbits
	xaddl %edx, 0x100(%rbp)
	.section .t2676,"ax",@progbits
	xaddl %edx, (%rsp)
	.section .t2677,"ax",@progbits
	xaddl %edx, 0x10(%rsp,%rsi,4)
	.section .t2678,"ax",@progbits
	xaddl %edx, %gs:0x10(%rcx)
	.section .t2679,"ax",@progbits
	xaddl %edx, %fs:(%rax,%rsi,8)
	.section .t2680,"ax",@progbits
	xaddl %ebx, (%rcx)
	.section .t2681,"ax",@progbits
	xaddl %ebx, 0x10(%rcx)
	.section .t2682,"ax",@progbits
	xaddl %ebx, -0x8(%rbp)
	.section .t2683,"ax",@progbits
	xaddl %ebx, 0x12345(%rcx)
	.section .t2684,"ax",@progbits
	xaddl %ebx, (%rax,%rsi,4)
	.section .t2685,"ax",@progbits
	xaddl %ebx, 0x10(%rax,%rsi,8)
	.section .t2686,"ax",@progbits
	xaddl %ebx, (,%rsi,2)
	.section .t2687,"ax",@progbits
	xaddl %ebx, 0x40(%rip)
	.section .t2688,"ax",@progbits
	xaddl %ebx, -0x100(%rip)
	.section .t2689,"ax",@progbits
	xaddl %ebx, 0x1234
	.section .t2690,"ax",@progbits
	xaddl %ebx, (%r8)
	.section .t2691,"ax",@progbits
	xaddl %ebx, (%r12)
	.section .t2692,"ax",@progbits
	xaddl %ebx, 0x8(%r13)
	.section .t2693,"ax",@progbits
	xaddl %ebx, (%r8,%r15,2)
	.section .t2694,"ax",@progbits
	xaddl %ebx, (%rax,%r12,4)
	.section .t2695,"ax",@progbits
	xaddl %ebx, 0x100(%rbp)
	.section .t2696,"ax",@progbits
	xaddl %ebx, (%rsp)
	.section .t2697,"ax",@progbits
	xaddl %ebx, 0x10(%rsp,%rsi,4)
	.section .t2698,"ax",@progbits
	xaddl %ebx, %gs:0x10(%rcx)
	.section .t2699,"ax",@progbits
	xaddl %ebx, %fs:(%rax,%rsi,8)
	.section .t2700,"ax",@progbits
	xaddl %r9d, (%rcx)
	.section .t2701,"ax",@progbits
	xaddl %r9d, 0x10(%rcx)
	.section .t2702,"ax",@progbits
	xaddl %r9d, -0x8(%rbp)
	.section .t2703,"ax",@progbits
	xaddl %r9d, 0x12345(%rcx)
	.section .t2704,"ax",@progbits
	xaddl %r9d, (%rax,%rsi,4)
	.section .t2705,"ax",@progbits
	xaddl %r9d, 0x10(%rax,%rsi,8)
	.section .t2706,"ax",@progbits
	xaddl %r9d, (,%rsi,2)
	.section .t2707,"ax",@progbits
	xaddl %r9d, 0x40(%rip)
	.section .t2708,"ax",@progbits
	xaddl %r9d, -0x100(%rip)
	.section .t2709,"ax",@progbits
	xaddl %r9d, 0x1234
	.section .t2710,"ax",@progbits
	xaddl %r9d, (%r8)
	.section .t2711,"ax",@progbits
	xaddl %r9d, (%r12)
	.section .t2712,"ax",@progbits
	xaddl %r9d, 0x8(%r13)
	.section .t2713,"ax",@progbits
	xaddl %r9d, (%r8,%r15,2)
	.section .t2714,"ax",@progbits
	xaddl %r9d, (%rax,%r12,4)
	.section .t2715,"ax",@progbits
	xaddl %r9d, 0x100(%rbp)
	.section .t2716,"ax",@progbits
	xaddl %r9d, (%rsp)
	.section .t2717,"ax",@progbits
	xaddl %r9d, 0x10(%rsp,%rsi,4)
	.section .t2718,"ax",@progbits
	xaddl %r9d, %gs:0x10(%rcx)
	.section .t2719,"ax",@progbits
	xaddl %r9d, %fs:(%rax,%rsi,8)
	.section .t2720,"ax",@progbits
	xaddq %rdx, (%rcx)
	.section .t2721,"ax",@progbits
	xaddq %rdx, 0x10(%rcx)
	.section .t2722,"ax",@progbits
	xaddq %rdx, -0x8(%rbp)
	.section .t2723,"ax",@progbits
	xaddq %rdx, 0x12345(%rcx)
	.section .t2724,"ax",@progbits
	xaddq %rdx, (%rax,%rsi,4)
	.section .t2725,"ax",@progbits
	xaddq %rdx, 0x10(%rax,%rsi,8)
	.section .t2726,"ax",@progbits
	xaddq %rdx, (,%rsi,2)
	.section .t2727,"ax",@progbits
	xaddq %rdx, 0x40(%rip)
	.section .t2728,"ax",@progbits
	xaddq %rdx, -0x100(%rip)
	.section .t2729,"ax",@progbits
	xaddq %rdx, 0x1234
	.section .t2730,"ax",@progbits
	xaddq %rdx, (%r8)
	.section .t2731,"ax",@progbits
	xaddq %rdx, (%r12)
	.section .t2732,"ax",@progbits
	xaddq %rdx, 0x8(%r13)
	.section .t2733,"ax",@progbits
	xaddq %rdx, (%r8,%r15,2)
	.section .t2734,"ax",@progbits
	xaddq %rdx, (%rax,%r12,4)
	.section .t2735,"ax",@progbits
	xaddq %rdx, 0x100(%rbp)
	.section .t2736,"ax",@progbits
	xaddq %rdx, (%rsp)
	.section .t2737,"ax",@progbits
	xaddq %rdx, 0x10(%rsp,%rsi,4)
	.section .t2738,"ax",@progbits
	xaddq %rdx, %gs:0x10(%rcx)
	.section .t2739,"ax",@progbits
	xaddq %rdx, %fs:(%rax,%rsi,8)
	.section .t2740,"ax",@progbits
	xaddq %rbx, (%rcx)
	.section .t2741,"ax",@progbits
	xaddq %rbx, 0x10(%rcx)
	.section .t2742,"ax",@progbits
	xaddq %rbx, -0x8(%rbp)
	.section .t2743,"ax",@progbits
	xaddq %rbx, 0x12345(%rcx)
	.section .t2744,"ax",@progbits
	xaddq %rbx, (%rax,%rsi,4)
	.section .t2745,"ax",@progbits
	xaddq %rbx, 0x10(%rax,%rsi,8)
	.section .t2746,"ax",@progbits
	xaddq %rbx, (,%rsi,2)
	.section .t2747,"ax",@progbits
	xaddq %rbx, 0x40(%rip)
	.section .t2748,"ax",@progbits
	xaddq %rbx, -0x100(%rip)
	.section .t2749,"ax",@progbits
	xaddq %rbx, 0x1234
	.section .t2750,"ax",@progbits
	xaddq %rbx, (%r8)
	.section .t2751,"ax",@progbits
	xaddq %rbx, (%r12)
	.section .t2752,"ax",@progbits
	xaddq %rbx, 0x8(%r13)
	.section .t2753,"ax",@progbits
	xaddq %rbx, (%r8,%r15,2)
	.section .t2754,"ax",@progbits
	xaddq %rbx, (%rax,%r12,4)
	.section .t2755,"ax",@progbits
	xaddq %rbx, 0x100(%rbp)
	.section .t2756,"ax",@progbits
	xaddq %rbx, (%rsp)
	.section .t2757,"ax",@progbits
	xaddq %rbx, 0x10(%rsp,%rsi,4)
	.section .t2758,"ax",@progbits
	xaddq %rbx, %gs:0x10(%rcx)
	.section .t2759,"ax",@progbits
	xaddq %rbx, %fs:(%rax,%rsi,8)
	.section .t2760,"ax",@progbits
	xaddq %r9, (%rcx)
	.section .t2761,"ax",@progbits
	xaddq %r9, 0x10(%rcx)
	.section .t2762,"ax",@progbits
	xaddq %r9, -0x8(%rbp)
	.section .t2763,"ax",@progbits
	xaddq %r9, 0x12345(%rcx)
	.section .t2764,"ax",@progbits
	xaddq %r9, (%rax,%rsi,4)
	.section .t2765,"ax",@progbits
	xaddq %r9, 0x10(%rax,%rsi,8)
	.section .t2766,"ax",@progbits
	xaddq %r9, (,%rsi,2)
	.section .t2767,"ax",@progbits
	xaddq %r9, 0x40(%rip)
	.section .t2768,"ax",@progbits
	xaddq %r9, -0x100(%rip)
	.section .t2769,"ax",@progbits
	xaddq %r9, 0x1234
	.section .t2770,"ax",@progbits
	xaddq %r9, (%r8)
	.section .t2771,"ax",@progbits
	xaddq %r9, (%r12)
	.section .t2772,"ax",@progbits
	xaddq %r9, 0x8(%r13)
	.section .t2773,"ax",@progbits
	xaddq %r9, (%r8,%r15,2)
	.section .t2774,"ax",@progbits
	xaddq %r9, (%rax,%r12,4)
	.section .t2775,"ax",@progbits
	xaddq %r9, 0x100(%rbp)
	.section .t2776,"ax",@progbits
	xaddq %r9, (%rsp)
	.section .t2777,"ax",@progbits
	xaddq %r9, 0x10(%rsp,%rsi,4)
	.section .t2778,"ax",@progbits
	xaddq %r9, %gs:0x10(%rcx)
	.section .t2779,"ax",@progbits
	xaddq %r9, %fs:(%rax,%rsi,8)
	.section .t2780,"ax",@progbits
	xaddq %r12, (%rcx)
	.section .t2781,"ax",@progbits
	xaddq %r12, 0x10(%rcx)
	.section .t2782,"ax",@progbits
	xaddq %r12, -0x8(%rbp)
	.section .t2783,"ax",@progbits
	xaddq %r12, 0x12345(%rcx)
	.section .t2784,"ax",@progbits
	xaddq %r12, (%rax,%rsi,4)
	.section .t2785,"ax",@progbits
	xaddq %r12, 0x10(%rax,%rsi,8)
	.section .t2786,"ax",@progbits
	xaddq %r12, (,%rsi,2)
	.section .t2787,"ax",@progbits
	xaddq %r12, 0x40(%rip)
	.section .t2788,"ax",@progbits
	xaddq %r12, -0x100(%rip)
	.section .t2789,"ax",@progbits
	xaddq %r12, 0x1234
	.section .t2790,"ax",@progbits
	xaddq %r12, (%r8)
	.section .t2791,"ax",@progbits
	xaddq %r12, (%r12)
	.section .t2792,"ax",@progbits
	xaddq %r12, 0x8(%r13)
	.section .t2793,"ax",@progbits
	xaddq %r12, (%r8,%r15,2)
	.section .t2794,"ax",@progbits
	xaddq %r12, (%rax,%r12,4)
	.section .t2795,"ax",@progbits
	xaddq %r12, 0x100(%rbp)
	.section .t2796,"ax",@progbits
	xaddq %r12, (%rsp)
	.section .t2797,"ax",@progbits
	xaddq %r12, 0x10(%rsp,%rsi,4)
	.section .t2798,"ax",@progbits
	xaddq %r12, %gs:0x10(%rcx)
	.section .t2799,"ax",@progbits
	xaddq %r12, %fs:(%rax,%rsi,8)
	.section .t2800,"ax",@progbits
	cmpxchgb %dl, (%rcx)
	.section .t2801,"ax",@progbits
	cmpxchgb %dl, 0x10(%rcx)
	.section .t2802,"ax",@progbits
	cmpxchgb %dl, -0x8(%rbp)
	.section .t2803,"ax",@progbits
	cmpxchgb %dl, 0x12345(%rcx)
	.section .t2804,"ax",@progbits
	cmpxchgb %dl, (%rax,%rsi,4)
	.section .t2805,"ax",@progbits
	cmpxchgb %dl, 0x10(%rax,%rsi,8)
	.section .t2806,"ax",@progbits
	cmpxchgb %dl, (,%rsi,2)
	.section .t2807,"ax",@progbits
	cmpxchgb %dl, 0x40(%rip)
	.section .t2808,"ax",@progbits
	cmpxchgb %dl, -0x100(%rip)
	.section .t2809,"ax",@progbits
	cmpxchgb %dl, 0x1234
	.section .t2810,"ax",@progbits
	cmpxchgb %dl, (%r8)
	.section .t2811,"ax",@progbits
	cmpxchgb %dl, (%r12)
	.section .t2812,"ax",@progbits
	cmpxchgb %dl, 0x8(%r13)
	.section .t2813,"ax",@progbits
	cmpxchgb %dl, (%r8,%r15,2)
	.section .t2814,"ax",@progbits
	cmpxchgb %dl, (%rax,%r12,4)
	.section .t2815,"ax",@progbits
	cmpxchgb %dl, 0x100(%rbp)
	.section .t2816,"ax",@progbits
	cmpxchgb %dl, (%rsp)
	.section .t2817,"ax",@progbits
	cmpxchgb %dl, 0x10(%rsp,%rsi,4)
	.section .t2818,"ax",@progbits
	cmpxchgb %dl, %gs:0x10(%rcx)
	.section .t2819,"ax",@progbits
	cmpxchgb %dl, %fs:(%rax,%rsi,8)
	.section .t2820,"ax",@progbits
	cmpxchgb %bl, (%rcx)
	.section .t2821,"ax",@progbits
	cmpxchgb %bl, 0x10(%rcx)
	.section .t2822,"ax",@progbits
	cmpxchgb %bl, -0x8(%rbp)
	.section .t2823,"ax",@progbits
	cmpxchgb %bl, 0x12345(%rcx)
	.section .t2824,"ax",@progbits
	cmpxchgb %bl, (%rax,%rsi,4)
	.section .t2825,"ax",@progbits
	cmpxchgb %bl, 0x10(%rax,%rsi,8)
	.section .t2826,"ax",@progbits
	cmpxchgb %bl, (,%rsi,2)
	.section .t2827,"ax",@progbits
	cmpxchgb %bl, 0x40(%rip)
	.section .t2828,"ax",@progbits
	cmpxchgb %bl, -0x100(%rip)
	.section .t2829,"ax",@progbits
	cmpxchgb %bl, 0x1234
	.section .t2830,"ax",@progbits
	cmpxchgb %bl, (%r8)
	.section .t2831,"ax",@progbits
	cmpxchgb %bl, (%r12)
	.section .t2832,"ax",@progbits
	cmpxchgb %bl, 0x8(%r13)
	.section .t2833,"ax",@progbits
	cmpxchgb %bl, (%r8,%r15,2)
	.section .t2834,"ax",@progbits
	cmpxchgb %bl, (%rax,%r12,4)
	.section .t2835,"ax",@progbits
	cmpxchgb %bl, 0x100(%rbp)
	.section .t2836,"ax",@progbits
	cmpxchgb %bl, (%rsp)
	.section .t2837,"ax",@progbits
	cmpxchgb %bl, 0x10(%rsp,%rsi,4)
	.section .t2838,"ax",@progbits
	cmpxchgb %bl, %gs:0x10(%rcx)
	.section .t2839,"ax",@progbits
	cmpxchgb %bl, %fs:(%rax,%rsi,8)
	.section .t2840,"ax",@progbits
	cmpxchgb %r9b, (%rcx)
	.section .t2841,"ax",@progbits
	cmpxchgb %r9b, 0x10(%rcx)
	.section .t2842,"ax",@progbits
	cmpxchgb %r9b, -0x8(%rbp)
	.section .t2843,"ax",@progbits
	cmpxchgb %r9b, 0x12345(%rcx)
	.section .t2844,"ax",@progbits
	cmpxchgb %r9b, (%rax,%rsi,4)
	.section .t2845,"ax",@progbits
	cmpxchgb %r9b, 0x10(%rax,%rsi,8)
	.section .t2846,"ax",@progbits
	cmpxchgb %r9b, (,%rsi,2)
	.section .t2847,"ax",@progbits
	cmpxchgb %r9b, 0x40(%rip)
	.section .t2848,"ax",@progbits
	cmpxchgb %r9b, -0x100(%rip)
	.section .t2849,"ax",@progbits
	cmpxchgb %r9b, 0x1234
	.section .t2850,"ax",@progbits
	cmpxchgb %r9b, (%r8)
	.section .t2851,"ax",@progbits
	cmpxchgb %r9b, (%r12)
	.section .t2852,"ax",@progbits
	cmpxchgb %r9b, 0x8(%r13)
	.section .t2853,"ax",@progbits
	cmpxchgb %r9b, (%r8,%r15,2)
	.section .t2854,"ax",@progbits
	cmpxchgb %r9b, (%rax,%r12,4)
	.section .t2855,"ax",@progbits
	cmpxchgb %r9b, 0x100(%rbp)
	.section .t2856,"ax",@progbits
	cmpxchgb %r9b, (%rsp)
	.section .t2857,"ax",@progbits
	cmpxchgb %r9b, 0x10(%rsp,%rsi,4)
	.section .t2858,"ax",@progbits
	cmpxchgb %r9b, %gs:0x10(%rcx)
	.section .t2859,"ax",@progbits
	cmpxchgb %r9b, %fs:(%rax,%rsi,8)
	.section .t2860,"ax",@progbits
	cmpxchgb %r13b, (%rcx)
	.section .t2861,"ax",@progbits
	cmpxchgb %r13b, 0x10(%rcx)
	.section .t2862,"ax",@progbits
	cmpxchgb %r13b, -0x8(%rbp)
	.section .t2863,"ax",@progbits
	cmpxchgb %r13b, 0x12345(%rcx)
	.section .t2864,"ax",@progbits
	cmpxchgb %r13b, (%rax,%rsi,4)
	.section .t2865,"ax",@progbits
	cmpxchgb %r13b, 0x10(%rax,%rsi,8)
	.section .t2866,"ax",@progbits
	cmpxchgb %r13b, (,%rsi,2)
	.section .t2867,"ax",@progbits
	cmpxchgb %r13b, 0x40(%rip)
	.section .t2868,"ax",@progbits
	cmpxchgb %r13b, -0x100(%rip)
	.section .t2869,"ax",@progbits
	cmpxchgb %r13b, 0x1234
	.section .t2870,"ax",@progbits
	cmpxchgb %r13b, (%r8)
	.section .t2871,"ax",@progbits
	cmpxchgb %r13b, (%r12)
	.section .t2872,"ax",@progbits
	cmpxchgb %r13b, 0x8(%r13)
	.section .t2873,"ax",@progbits
	cmpxchgb %r13b, (%r8,%r15,2)
	.section .t2874,"ax",@progbits
	cmpxchgb %r13b, (%rax,%r12,4)
	.section .t2875,"ax",@progbits
	cmpxchgb %r13b, 0x100(%rbp)
	.section .t2876,"ax",@progbits
	cmpxchgb %r13b, (%rsp)
	.section .t2877,"ax",@progbits
	cmpxchgb %r13b, 0x10(%rsp,%rsi,4)
	.section .t2878,"ax",@progbits
	cmpxchgb %r13b, %gs:0x10(%rcx)
	.section .t2879,"ax",@progbits
	cmpxchgb %r13b, %fs:(%rax,%rsi,8)
	.section .t2880,"ax",@progbits
	cmpxchgw %dx, (%rcx)
	.section .t2881,"ax",@progbits
	cmpxchgw %dx, 0x10(%rcx)
	.section .t2882,"ax",@progbits
	cmpxchgw %dx, -0x8(%rbp)
	.section .t2883,"ax",@progbits
	cmpxchgw %dx, 0x12345(%rcx)
	.section .t2884,"ax",@progbits
	cmpxchgw %dx, (%rax,%rsi,4)
	.section .t2885,"ax",@progbits
	cmpxchgw %dx, 0x10(%rax,%rsi,8)
	.section .t2886,"ax",@progbits
	cmpxchgw %dx, (,%rsi,2)
	.section .t2887,"ax",@progbits
	cmpxchgw %dx, 0x40(%rip)
	.section .t2888,"ax",@progbits
	cmpxchgw %dx, -0x100(%rip)
	.section .t2889,"ax",@progbits
	cmpxchgw %dx, 0x1234
	.section .t2890,"ax",@progbits
	cmpxchgw %dx, (%r8)
	.section .t2891,"ax",@progbits
	cmpxchgw %dx, (%r12)
	.section .t2892,"ax",@progbits
	cmpxchgw %dx, 0x8(%r13)
	.section .t2893,"ax",@progbits
	cmpxchgw %dx, (%r8,%r15,2)
	.section .t2894,"ax",@progbits
	cmpxchgw %dx, (%rax,%r12,4)
	.section .t2895,"ax",@progbits
	cmpxchgw %dx, 0x100(%rbp)
	.section .t2896,"ax",@progbits
	cmpxchgw %dx, (%rsp)
	.section .t2897,"ax",@progbits
	cmpxchgw %dx, 0x10(%rsp,%rsi,4)
	.section .t2898,"ax",@progbits
	cmpxchgw %dx, %gs:0x10(%rcx)
	.section .t2899,"ax",@progbits
	cmpxchgw %dx, %fs:(%rax,%rsi,8)
	.section .t2900,"ax",@progbits
	cmpxchgw %bx, (%rcx)
	.section .t2901,"ax",@progbits
	cmpxchgw %bx, 0x10(%rcx)
	.section .t2902,"ax",@progbits
	cmpxchgw %bx, -0x8(%rbp)
	.section .t2903,"ax",@progbits
	cmpxchgw %bx, 0x12345(%rcx)
	.section .t2904,"ax",@progbits
	cmpxchgw %bx, (%rax,%rsi,4)
	.section .t2905,"ax",@progbits
	cmpxchgw %bx, 0x10(%rax,%rsi,8)
	.section .t2906,"ax",@progbits
	cmpxchgw %bx, (,%rsi,2)
	.section .t2907,"ax",@progbits
	cmpxchgw %bx, 0x40(%rip)
	.section .t2908,"ax",@progbits
	cmpxchgw %bx, -0x100(%rip)
	.section .t2909,"ax",@progbits
	cmpxchgw %bx, 0x1234
	.section .t2910,"ax",@progbits
	cmpxchgw %bx, (%r8)
	.section .t2911,"ax",@progbits
	cmpxchgw %bx, (%r12)
	.section .t2912,"ax",@progbits
	cmpxchgw %bx, 0x8(%r13)
	.section .t2913,"ax",@progbits
	cmpxchgw %bx, (%r8,%r15,2)
	.section .t2914,"ax",@progbits
	cmpxchgw %bx, (%rax,%r12,4)
	.section .t2915,"ax",@progbits
	cmpxchgw %bx, 0x100(%rbp)
	.section .t2916,"ax",@progbits
	cmpxchgw %bx, (%rsp)
	.section .t2917,"ax",@progbits
	cmpxchgw %bx, 0x10(%rsp,%rsi,4)
	.section .t2918,"ax",@progbits
	cmpxchgw %bx, %gs:0x10(%rcx)
	.section .t2919,"ax",@progbits
	cmpxchgw %bx, %fs:(%rax,%rsi,8)
	.section .t2920,"ax",@progbits
	cmpxchgw %r9w, (%rcx)
	.section .t2921,"ax",@progbits
	cmpxchgw %r9w, 0x10(%rcx)
	.section .t2922,"ax",@progbits
	cmpxchgw %r9w, -0x8(%rbp)
	.section .t2923,"ax",@progbits
	cmpxchgw %r9w, 0x12345(%rcx)
	.section .t2924,"ax",@progbits
	cmpxchgw %r9w, (%rax,%rsi,4)
	.section .t2925,"ax",@progbits
	cmpxchgw %r9w, 0x10(%rax,%rsi,8)
	.section .t2926,"ax",@progbits
	cmpxchgw %r9w, (,%rsi,2)
	.section .t2927,"ax",@progbits
	cmpxchgw %r9w, 0x40(%rip)
	.section .t2928,"ax",@progbits
	cmpxchgw %r9w, -0x100(%rip)
	.section .t2929,"ax",@progbits
	cmpxchgw %r9w, 0x1234
	.section .t2930,"ax",@progbits
	cmpxchgw %r9w, (%r8)
	.section .t2931,"ax",@progbits
	cmpxchgw %r9w, (%r12)
	.section .t2932,"ax",@progbits
	cmpxchgw %r9w, 0x8(%r13)
	.section .t2933,"ax",@progbits
	cmpxchgw %r9w, (%r8,%r15,2)
	.section .t2934,"ax",@progbits
	cmpxchgw %r9w, (%rax,%r12,4)
	.section .t2935,"ax",@progbits
	cmpxchgw %r9w, 0x100(%rbp)
	.section .t2936,"ax",@progbits
	cmpxchgw %r9w, (%rsp)
	.section .t2937,"ax",@progbits
	cmpxchgw %r9w, 0x10(%rsp,%rsi,4)
	.section .t2938,"ax",@progbits
	cmpxchgw %r9w, %gs:0x10(%rcx)
	.section .t2939,"ax",@progbits
	cmpxchgw %r9w, %fs:(%rax,%rsi,8)
	.section .t2940,"ax",@progbits
	cmpxchgl %edx, (%rcx)
	.section .t2941,"ax",@progbits
	cmpxchgl %edx, 0x10(%rcx)
	.section .t2942,"ax",@progbits
	cmpxchgl %edx, -0x8(%rbp)
	.section .t2943,"ax",@progbits
	cmpxchgl %edx, 0x12345(%rcx)
	.section .t2944,"ax",@progbits
	cmpxchgl %edx, (%rax,%rsi,4)
	.section .t2945,"ax",@progbits
	cmpxchgl %edx, 0x10(%rax,%rsi,8)
	.section .t2946,"ax",@progbits
	cmpxchgl %edx, (,%rsi,2)
	.section .t2947,"ax",@progbits
	cmpxchgl %edx, 0x40(%rip)
	.section .t2948,"ax",@progbits
	cmpxchgl %edx, -0x100(%rip)
	.section .t2949,"ax",@progbits
	cmpxchgl %edx, 0x1234
	.section .t2950,"ax",@progbits
	cmpxchgl %edx, (%r8)
	.section .t2951,"ax",@progbits
	cmpxchgl %edx, (%r12)
	.section .t2952,"ax",@progbits
	cmpxchgl %edx, 0x8(%r13)
	.section .t2953,"ax",@progbits
	cmpxchgl %edx, (%r8,%r15,2)
	.section .t2954,"ax",@progbits
	cmpxchgl %edx, (%rax,%r12,4)
	.section .t2955,"ax",@progbits
	cmpxchgl %edx, 0x100(%rbp)
	.section .t2956,"ax",@progbits
	cmpxchgl %edx, (%rsp)
	.section .t2957,"ax",@progbits
	cmpxchgl %edx, 0x10(%rsp,%rsi,4)
	.section .t2958,"ax",@progbits
	cmpxchgl %edx, %gs:0x10(%rcx)
	.section .t2959,"ax",@progbits
	cmpxchgl %edx, %fs:(%rax,%rsi,8)
	.section .t2960,"ax",@progbits
	cmpxchgl %ebx, (%rcx)
	.section .t2961,"ax",@progbits
	cmpxchgl %ebx, 0x10(%rcx)
	.section .t2962,"ax",@progbits
	cmpxchgl %ebx, -0x8(%rbp)
	.section .t2963,"ax",@progbits
	cmpxchgl %ebx, 0x12345(%rcx)
	.section .t2964,"ax",@progbits
	cmpxchgl %ebx, (%rax,%rsi,4)
	.section .t2965,"ax",@progbits
	cmpxchgl %ebx, 0x10(%rax,%rsi,8)
	.section .t2966,"ax",@progbits
	cmpxchgl %ebx, (,%rsi,2)
	.section .t2967,"ax",@progbits
	cmpxchgl %ebx, 0x40(%rip)
	.section .t2968,"ax",@progbits
	cmpxchgl %ebx, -0x100(%rip)
	.section .t2969,"ax",@progbits
	cmpxchgl %ebx, 0x1234
	.section .t2970,"ax",@progbits
	cmpxchgl %ebx, (%r8)
	.section .t2971,"ax",@progbits
	cmpxchgl %ebx, (%r12)
	.section .t2972,"ax",@progbits
	cmpxchgl %ebx, 0x8(%r13)
	.section .t2973,"ax",@progbits
	cmpxchgl %ebx, (%r8,%r15,2)
	.section .t2974,"ax",@progbits
	cmpxchgl %ebx, (%rax,%r12,4)
	.section .t2975,"ax",@progbits
	cmpxchgl %ebx, 0x100(%rbp)
	.section .t2976,"ax",@progbits
	cmpxchgl %ebx, (%rsp)
	.section .t2977,"ax",@progbits
	cmpxchgl %ebx, 0x10(%rsp,%rsi,4)
	.section .t2978,"ax",@progbits
	cmpxchgl %ebx, %gs:0x10(%rcx)
	.section .t2979,"ax",@progbits
	cmpxchgl %ebx, %fs:(%rax,%rsi,8)
	.section .t2980,"ax",@progbits
	cmpxchgl %r9d, (%rcx)
	.section .t2981,"ax",@progbits
	cmpxchgl %r9d, 0x10(%rcx)
	.section .t2982,"ax",@progbits
	cmpxchgl %r9d, -0x8(%rbp)
	.section .t2983,"ax",@progbits
	cmpxchgl %r9d, 0x12345(%rcx)
	.section .t2984,"ax",@progbits
	cmpxchgl %r9d, (%rax,%rsi,4)
	.section .t2985,"ax",@progbits
	cmpxchgl %r9d, 0x10(%rax,%rsi,8)
	.section .t2986,"ax",@progbits
	cmpxchgl %r9d, (,%rsi,2)
	.section .t2987,"ax",@progbits
	cmpxchgl %r9d, 0x40(%rip)
	.section .t2988,"ax",@progbits
	cmpxchgl %r9d, -0x100(%rip)
	.section .t2989,"ax",@progbits
	cmpxchgl %r9d, 0x1234
	.section .t2990,"ax",@progbits
	cmpxchgl %r9d, (%r8)
	.section .t2991,"ax",@progbits
	cmpxchgl %r9d, (%r12)
	.section .t2992,"ax",@progbits
	cmpxchgl %r9d, 0x8(%r13)
	.section .t2993,"ax",@progbits
	cmpxchgl %r9d, (%r8,%r15,2)
	.section .t2994,"ax",@progbits
	cmpxchgl %r9d, (%rax,%r12,4)
	.section .t2995,"ax",@progbits
	cmpxchgl %r9d, 0x100(%rbp)
	.section .t2996,"ax",@progbits
	cmpxchgl %r9d, (%rsp)
	.section .t2997,"ax",@progbits
	cmpxchgl %r9d, 0x10(%rsp,%rsi,4)
	.section .t2998,"ax",@progbits
	cmpxchgl %r9d, %gs:0x10(%rcx)
	.section .t2999,"ax",@progbits
	cmpxchgl %r9d, %fs:(%rax,%rsi,8)
	.section .t3000,"ax",@progbits
	cmpxchgq %rdx, (%rcx)
	.section .t3001,"ax",@progbits
	cmpxchgq %rdx, 0x10(%rcx)
	.section .t3002,"ax",@progbits
	cmpxchgq %rdx, -0x8(%rbp)
	.section .t3003,"ax",@progbits
	cmpxchgq %rdx, 0x12345(%rcx)
	.section .t3004,"ax",@progbits
	cmpxchgq %rdx, (%rax,%rsi,4)
	.section .t3005,"ax",@progbits
	cmpxchgq %rdx, 0x10(%rax,%rsi,8)
	.section .t3006,"ax",@progbits
	cmpxchgq %rdx, (,%rsi,2)
	.section .t3007,"ax",@progbits
	cmpxchgq %rdx, 0x40(%rip)
	.section .t3008,"ax",@progbits
	cmpxchgq %rdx, -0x100(%rip)
	.section .t3009,"ax",@progbits
	cmpxchgq %rdx, 0x1234
	.section .t3010,"ax",@progbits
	cmpxchgq %rdx, (%r8)
	.section .t3011,"ax",@progbits
	cmpxchgq %rdx, (%r12)
	.section .t3012,"ax",@progbits
	cmpxchgq %rdx, 0x8(%r13)
	.section .t3013,"ax",@progbits
	cmpxchgq %rdx, (%r8,%r15,2)
	.section .t3014,"ax",@progbits
	cmpxchgq %rdx, (%rax,%r12,4)
	.section .t3015,"ax",@progbits
	cmpxchgq %rdx, 0x100(%rbp)
	.section .t3016,"ax",@progbits
	cmpxchgq %rdx, (%rsp)
	.section .t3017,"ax",@progbits
	cmpxchgq %rdx, 0x10(%rsp,%rsi,4)
	.section .t3018,"ax",@progbits
	cmpxchgq %rdx, %gs:0x10(%rcx)
	.section .t3019,"ax",@progbits
	cmpxchgq %rdx, %fs:(%rax,%rsi,8)
	.section .t3020,"ax",@progbits
	cmpxchgq %rbx, (%rcx)
	.section .t3021,"ax",@progbits
	cmpxchgq %rbx, 0x10(%rcx)
	.section .t3022,"ax",@progbits
	cmpxchgq %rbx, -0x8(%rbp)
	.section .t3023,"ax",@progbits
	cmpxchgq %rbx, 0x12345(%rcx)
	.section .t3024,"ax",@progbits
	cmpxchgq %rbx, (%rax,%rsi,4)
	.section .t3025,"ax",@progbits
	cmpxchgq %rbx, 0x10(%rax,%rsi,8)
	.section .t3026,"ax",@progbits
	cmpxchgq %rbx, (,%rsi,2)
	.section .t3027,"ax",@progbits
	cmpxchgq %rbx, 0x40(%rip)
	.section .t3028,"ax",@progbits
	cmpxchgq %rbx, -0x100(%rip)
	.section .t3029,"ax",@progbits
	cmpxchgq %rbx, 0x1234
	.section .t3030,"ax",@progbits
	cmpxchgq %rbx, (%r8)
	.section .t3031,"ax",@progbits
	cmpxchgq %rbx, (%r12)
	.section .t3032,"ax",@progbits
	cmpxchgq %rbx, 0x8(%r13)
	.section .t3033,"ax",@progbits
	cmpxchgq %rbx, (%r8,%r15,2)
	.section .t3034,"ax",@progbits
	cmpxchgq %rbx, (%rax,%r12,4)
	.section .t3035,"ax",@progbits
	cmpxchgq %rbx, 0x100(%rbp)
	.section .t3036,"ax",@progbits
	cmpxchgq %rbx, (%rsp)
	.section .t3037,"ax",@progbits
	cmpxchgq %rbx, 0x10(%rsp,%rsi,4)
	.section .t3038,"ax",@progbits
	cmpxchgq %rbx, %gs:0x10(%rcx)
	.section .t3039,"ax",@progbits
	cmpxchgq %rbx, %fs:(%rax,%rsi,8)
	.section .t3040,"ax",@progbits
	cmpxchgq %r9, (%rcx)
	.section .t3041,"ax",@progbits
	cmpxchgq %r9, 0x10(%rcx)
	.section .t3042,"ax",@progbits
	cmpxchgq %r9, -0x8(%rbp)
	.section .t3043,"ax",@progbits
	cmpxchgq %r9, 0x12345(%rcx)
	.section .t3044,"ax",@progbits
	cmpxchgq %r9, (%rax,%rsi,4)
	.section .t3045,"ax",@progbits
	cmpxchgq %r9, 0x10(%rax,%rsi,8)
	.section .t3046,"ax",@progbits
	cmpxchgq %r9, (,%rsi,2)
	.section .t3047,"ax",@progbits
	cmpxchgq %r9, 0x40(%rip)
	.section .t3048,"ax",@progbits
	cmpxchgq %r9, -0x100(%rip)
	.section .t3049,"ax",@progbits
	cmpxchgq %r9, 0x1234
	.section .t3050,"ax",@progbits
	cmpxchgq %r9, (%r8)
	.section .t3051,"ax",@progbits
	cmpxchgq %r9, (%r12)
	.section .t3052,"ax",@progbits
	cmpxchgq %r9, 0x8(%r13)
	.section .t3053,"ax",@progbits
	cmpxchgq %r9, (%r8,%r15,2)
	.section .t3054,"ax",@progbits
	cmpxchgq %r9, (%rax,%r12,4)
	.section .t3055,"ax",@progbits
	cmpxchgq %r9, 0x100(%rbp)
	.section .t3056,"ax",@progbits
	cmpxchgq %r9, (%rsp)
	.section .t3057,"ax",@progbits
	cmpxchgq %r9, 0x10(%rsp,%rsi,4)
	.section .t3058,"ax",@progbits
	cmpxchgq %r9, %gs:0x10(%rcx)
	.section .t3059,"ax",@progbits
	cmpxchgq %r9, %fs:(%rax,%rsi,8)
	.section .t3060,"ax",@progbits
	cmpxchgq %r12, (%rcx)
	.section .t3061,"ax",@progbits
	cmpxchgq %r12, 0x10(%rcx)
	.section .t3062,"ax",@progbits
	cmpxchgq %r12, -0x8(%rbp)
	.section .t3063,"ax",@progbits
	cmpxchgq %r12, 0x12345(%rcx)
	.section .t3064,"ax",@progbits
	cmpxchgq %r12, (%rax,%rsi,4)
	.section .t3065,"ax",@progbits
	cmpxchgq %r12, 0x10(%rax,%rsi,8)
	.section .t3066,"ax",@progbits
	cmpxchgq %r12, (,%rsi,2)
	.section .t3067,"ax",@progbits
	cmpxchgq %r12, 0x40(%rip)
	.section .t3068,"ax",@progbits
	cmpxchgq %r12, -0x100(%rip)
	.section .t3069,"ax",@progbits
	cmpxchgq %r12, 0x1234
	.section .t3070,"ax",@progbits
	cmpxchgq %r12, (%r8)
	.section .t3071,"ax",@progbits
	cmpxchgq %r12, (%r12)
	.section .t3072,"ax",@progbits
	cmpxchgq %r12, 0x8(%r13)
	.section .t3073,"ax",@progbits
	cmpxchgq %r12, (%r8,%r15,2)
	.section .t3074,"ax",@progbits
	cmpxchgq %r12, (%rax,%r12,4)
	.section .t3075,"ax",@progbits
	cmpxchgq %r12, 0x100(%rbp)
	.section .t3076,"ax",@progbits
	cmpxchgq %r12, (%rsp)
	.section .t3077,"ax",@progbits
	cmpxchgq %r12, 0x10(%rsp,%rsi,4)
	.section .t3078,"ax",@progbits
	cmpxchgq %r12, %gs:0x10(%rcx)
	.section .t3079,"ax",@progbits
	cmpxchgq %r12, %fs:(%rax,%rsi,8)
	.section .t3080,"ax",@progbits
	movb (%rcx), %dl
	.section .t3081,"ax",@progbits
	movb 0x10(%rcx), %dl
	.section .t3082,"ax",@progbits
	movb -0x8(%rbp), %dl
	.section .t3083,"ax",@progbits
	movb 0x12345(%rcx), %dl
	.section .t3084,"ax",@progbits
	movb (%rax,%rsi,4), %dl
	.section .t3085,"ax",@progbits
	movb 0x10(%rax,%rsi,8), %dl
	.section .t3086,"ax",@progbits
	movb (,%rsi,2), %dl
	.section .t3087,"ax",@progbits
	movb 0x40(%rip), %dl
	.section .t3088,"ax",@progbits
	movb -0x100(%rip), %dl
	.section .t3089,"ax",@progbits
	movb 0x1234, %dl
	.section .t3090,"ax",@progbits
	movb (%r8), %dl
	.section .t3091,"ax",@progbits
	movb (%r12), %dl
	.section .t3092,"ax",@progbits
	movb 0x8(%r13), %dl
	.section .t3093,"ax",@progbits
	movb (%r8,%r15,2), %dl
	.section .t3094,"ax",@progbits
	movb (%rax,%r12,4), %dl
	.section .t3095,"ax",@progbits
	movb 0x100(%rbp), %dl
	.section .t3096,"ax",@progbits
	movb (%rsp), %dl
	.section .t3097,"ax",@progbits
	movb 0x10(%rsp,%rsi,4), %dl
	.section .t3098,"ax",@progbits
	movb %gs:0x10(%rcx), %dl
	.section .t3099,"ax",@progbits
	movb %fs:(%rax,%rsi,8), %dl
	.section .t3100,"ax",@progbits
	movb (%rcx), %bl
	.section .t3101,"ax",@progbits
	movb 0x10(%rcx), %bl
	.section .t3102,"ax",@progbits
	movb -0x8(%rbp), %bl
	.section .t3103,"ax",@progbits
	movb 0x12345(%rcx), %bl
	.section .t3104,"ax",@progbits
	movb (%rax,%rsi,4), %bl
	.section .t3105,"ax",@progbits
	movb 0x10(%rax,%rsi,8), %bl
	.section .t3106,"ax",@progbits
	movb (,%rsi,2), %bl
	.section .t3107,"ax",@progbits
	movb 0x40(%rip), %bl
	.section .t3108,"ax",@progbits
	movb -0x100(%rip), %bl
	.section .t3109,"ax",@progbits
	movb 0x1234, %bl
	.section .t3110,"ax",@progbits
	movb (%r8), %bl
	.section .t3111,"ax",@progbits
	movb (%r12), %bl
	.section .t3112,"ax",@progbits
	movb 0x8(%r13), %bl
	.section .t3113,"ax",@progbits
	movb (%r8,%r15,2), %bl
	.section .t3114,"ax",@progbits
	movb (%rax,%r12,4), %bl
	.section .t3115,"ax",@progbits
	movb 0x100(%rbp), %bl
	.section .t3116,"ax",@progbits
	movb (%rsp), %bl
	.section .t3117,"ax",@progbits
	movb 0x10(%rsp,%rsi,4), %bl
	.section .t3118,"ax",@progbits
	movb %gs:0x10(%rcx), %bl
	.section .t3119,"ax",@progbits
	movb %fs:(%rax,%rsi,8), %bl
	.section .t3120,"ax",@progbits
	movb (%rcx), %r9b
	.section .t3121,"ax",@progbits
	movb 0x10(%rcx), %r9b
	.section .t3122,"ax",@progbits
	movb -0x8(%rbp), %r9b
	.section .t3123,"ax",@progbits
	movb 0x12345(%rcx), %r9b
	.section .t3124,"ax",@progbits
	movb (%rax,%rsi,4), %r9b
	.section .t3125,"ax",@progbits
	movb 0x10(%rax,%rsi,8), %r9b
	.section .t3126,"ax",@progbits
	movb (,%rsi,2), %r9b
	.section .t3127,"ax",@progbits
	movb 0x40(%rip), %r9b
	.section .t3128,"ax",@progbits
	movb -0x100(%rip), %r9b
	.section .t3129,"ax",@progbits
	movb 0x1234, %r9b
	.section .t3130,"ax",@progbits
	movb (%r8), %r9b
	.section .t3131,"ax",@progbits
	movb (%r12), %r9b
	.section .t3132,"ax",@progbits
	movb 0x8(%r13), %r9b
	.section .t3133,"ax",@progbits
	movb (%r8,%r15,2), %r9b
	.section .t3134,"ax",@progbits
	movb (%rax,%r12,4), %r9b
	.section .t3135,"ax",@progbits
	movb 0x100(%rbp), %r9b
	.section .t3136,"ax",@progbits
	movb (%rsp), %r9b
	.section .t3137,"ax",@progbits
	movb 0x10(%rsp,%rsi,4), %r9b
	.section .t3138,"ax",@progbits
	movb %gs:0x10(%rcx), %r9b
	.section .t3139,"ax",@progbits
	movb %fs:(%rax,%rsi,8), %r9b
	.section .t3140,"ax",@progbits
	movb (%rcx), %r13b
	.section .t3141,"ax",@progbits
	movb 0x10(%rcx), %r13b
	.section .t3142,"ax",@progbits
	movb -0x8(%rbp), %r13b
	.section .t3143,"ax",@progbits
	movb 0x12345(%rcx), %r13b
	.section .t3144,"ax",@progbits
	movb (%rax,%rsi,4), %r13b
	.section .t3145,"ax",@progbits
	movb 0x10(%rax,%rsi,8), %r13b
	.section .t3146,"ax",@progbits
	movb (,%rsi,2), %r13b
	.section .t3147,"ax",@progbits
	movb 0x40(%rip), %r13b
	.section .t3148,"ax",@progbits
	movb -0x100(%rip), %r13b
	.section .t3149,"ax",@progbits
	movb 0x1234, %r13b
	.section .t3150,"ax",@progbits
	movb (%r8), %r13b
	.section .t3151,"ax",@progbits
	movb (%r12), %r13b
	.section .t3152,"ax",@progbits
	movb 0x8(%r13), %r13b
	.section .t3153,"ax",@progbits
	movb (%r8,%r15,2), %r13b
	.section .t3154,"ax",@progbits
	movb (%rax,%r12,4), %r13b
	.section .t3155,"ax",@progbits
	movb 0x100(%rbp), %r13b
	.section .t3156,"ax",@progbits
	movb (%rsp), %r13b
	.section .t3157,"ax",@progbits
	movb 0x10(%rsp,%rsi,4), %r13b
	.section .t3158,"ax",@progbits
	movb %gs:0x10(%rcx), %r13b
	.section .t3159,"ax",@progbits
	movb %fs:(%rax,%rsi,8), %r13b
	.section .t3160,"ax",@progbits
	movw (%rcx), %dx
	.section .t3161,"ax",@progbits
	movw 0x10(%rcx), %dx
	.section .t3162,"ax",@progbits
	movw -0x8(%rbp), %dx
	.section .t3163,"ax",@progbits
	movw 0x12345(%rcx), %dx
	.section .t3164,"ax",@progbits
	movw (%rax,%rsi,4), %dx
	.section .t3165,"ax",@progbits
	movw 0x10(%rax,%rsi,8), %dx
	.section .t3166,"ax",@progbits
	movw (,%rsi,2), %dx
	.section .t3167,"ax",@progbits
	movw 0x40(%rip), %dx
	.section .t3168,"ax",@progbits
	movw -0x100(%rip), %dx
	.section .t3169,"ax",@progbits
	movw 0x1234, %dx
	.section .t3170,"ax",@progbits
	movw (%r8), %dx
	.section .t3171,"ax",@progbits
	movw (%r12), %dx
	.section .t3172,"ax",@progbits
	movw 0x8(%r13), %dx
	.section .t3173,"ax",@progbits
	movw (%r8,%r15,2), %dx
	.section .t3174,"ax",@progbits
	movw (%rax,%r12,4), %dx
	.section .t3175,"ax",@progbits
	movw 0x100(%rbp), %dx
	.section .t3176,"ax",@progbits
	movw (%rsp), %dx
	.section .t3177,"ax",@progbits
	movw 0x10(%rsp,%rsi,4), %dx
	.section .t3178,"ax",@progbits
	movw %gs:0x10(%rcx), %dx
	.section .t3179,"ax",@progbits
	movw %fs:(%rax,%rsi,8), %dx
	.section .t3180,"ax",@progbits
	movw (%rcx), %bx
	.section .t3181,"ax",@progbits
	movw 0x10(%rcx), %bx
	.section .t3182,"ax",@progbits
	movw -0x8(%rbp), %bx
	.section .t3183,"ax",@progbits
	movw 0x12345(%rcx), %bx
	.section .t3184,"ax",@progbits
	movw (%rax,%rsi,4), %bx
	.section .t3185,"ax",@progbits
	movw 0x10(%rax,%rsi,8), %bx
	.section .t3186,"ax",@progbits
	movw (,%rsi,2), %bx
	.section .t3187,"ax",@progbits
	movw 0x40(%rip), %bx
	.section .t3188,"ax",@progbits
	movw -0x100(%rip), %bx
	.section .t3189,"ax",@progbits
	movw 0x1234, %bx
	.section .t3190,"ax",@progbits
	movw (%r8), %bx
	.section .t3191,"ax",@progbits
	movw (%r12), %bx
	.section .t3192,"ax",@progbits
	movw 0x8(%r13), %bx
	.section .t3193,"ax",@progbits
	movw (%r8,%r15,2), %bx
	.section .t3194,"ax",@progbits
	movw (%rax,%r12,4), %bx
	.section .t3195,"ax",@progbits
	movw 0x100(%rbp), %bx
	.section .t3196,"ax",@progbits
	movw (%rsp), %bx
	.section .t3197,"ax",@progbits
	movw 0x10(%rsp,%rsi,4), %bx
	.section .t3198,"ax",@progbits
	movw %gs:0x10(%rcx), %bx
	.section .t3199,"ax",@progbits
	movw %fs:(%rax,%rsi,8), %bx
	.section .t3200,"ax",@progbits
	movw (%rcx), %r9w
	.section .t3201,"ax",@progbits
	movw 0x10(%rcx), %r9w
	.section .t3202,"ax",@progbits
	movw -0x8(%rbp), %r9w
	.section .t3203,"ax",@progbits
	movw 0x12345(%rcx), %r9w
	.section .t3204,"ax",@progbits
	movw (%rax,%rsi,4), %r9w
	.section .t3205,"ax",@progbits
	movw 0x10(%rax,%rsi,8), %r9w
	.section .t3206,"ax",@progbits
	movw (,%rsi,2), %r9w
	.section .t3207,"ax",@progbits
	movw 0x40(%rip), %r9w
	.section .t3208,"ax",@progbits
	movw -0x100(%rip), %r9w
	.section .t3209,"ax",@progbits
	movw 0x1234, %r9w
	.section .t3210,"ax",@progbits
	movw (%r8), %r9w
	.section .t3211,"ax",@progbits
	movw (%r12), %r9w
	.section .t3212,"ax",@progbits
	movw 0x8(%r13), %r9w
	.section .t3213,"ax",@progbits
	movw (%r8,%r15,2), %r9w
	.section .t3214,"ax",@progbits
	movw (%rax,%r12,4), %r9w
	.section .t3215,"ax",@progbits
	movw 0x100(%rbp), %r9w
	.section .t3216,"ax",@progbits
	movw (%rsp), %r9w
	.section .t3217,"ax",@progbits
	movw 0x10(%rsp,%rsi,4), %r9w
	.section .t3218,"ax",@progbits
	movw %gs:0x10(%rcx), %r9w
	.section .t3219,"ax",@progbits
	movw %fs:(%rax,%rsi,8), %r9w
	.section .t3220,"ax",@progbits
	movl (%rcx), %edx
	.section .t3221,"ax",@progbits
	movl 0x10(%rcx), %edx
	.section .t3222,"ax",@progbits
	movl -0x8(%rbp), %edx
	.section .t3223,"ax",@progbits
	movl 0x12345(%rcx), %edx
	.section .t3224,"ax",@progbits
	movl (%rax,%rsi,4), %edx
	.section .t3225,"ax",@progbits
	movl 0x10(%rax,%rsi,8), %edx
	.section .t3226,"ax",@progbits
	movl (,%rsi,2), %edx
	.section .t3227,"ax",@progbits
	movl 0x40(%rip), %edx
	.section .t3228,"ax",@progbits
	movl -0x100(%rip), %edx
	.section .t3229,"ax",@progbits
	movl 0x1234, %edx
	.section .t3230,"ax",@progbits
	movl (%r8), %edx
	.section .t3231,"ax",@progbits
	movl (%r12), %edx
	.section .t3232,"ax",@progbits
	movl 0x8(%r13), %edx
	.section .t3233,"ax",@progbits
	movl (%r8,%r15,2), %edx
	.section .t3234,"ax",@progbits
	movl (%rax,%r12,4), %edx
	.section .t3235,"ax",@progbits
	movl 0x100(%rbp), %edx
	.section .t3236,"ax",@progbits
	movl (%rsp), %edx
	.section .t3237,"ax",@progbits
	movl 0x10(%rsp,%rsi,4), %edx
	.section .t3238,"ax",@progbits
	movl %gs:0x10(%rcx), %edx
	.section .t3239,"ax",@progbits
	movl %fs:(%rax,%rsi,8), %edx
	.section .t3240,"ax",@progbits
	movl (%rcx), %ebx
	.section .t3241,"ax",@progbits
	movl 0x10(%rcx), %ebx
	.section .t3242,"ax",@progbits
	movl -0x8(%rbp), %ebx
	.section .t3243,"ax",@progbits
	movl 0x12345(%rcx), %ebx
	.section .t3244,"ax",@progbits
	movl (%rax,%rsi,4), %ebx
	.section .t3245,"ax",@progbits
	movl 0x10(%rax,%rsi,8), %ebx
	.section .t3246,"ax",@progbits
	movl (,%rsi,2), %ebx
	.section .t3247,"ax",@progbits
	movl 0x40(%rip), %ebx
	.section .t3248,"ax",@progbits
	movl -0x100(%rip), %ebx
	.section .t3249,"ax",@progbits
	movl 0x1234, %ebx
	.section .t3250,"ax",@progbits
	movl (%r8), %ebx
	.section .t3251,"ax",@progbits
	movl (%r12), %ebx
	.section .t3252,"ax",@progbits
	movl 0x8(%r13), %ebx
	.section .t3253,"ax",@progbits
	movl (%r8,%r15,2), %ebx
	.section .t3254,"ax",@progbits
	movl (%rax,%r12,4), %ebx
	.section .t3255,"ax",@progbits
	movl 0x100(%rbp), %ebx
	.section .t3256,"ax",@progbits
	movl (%rsp), %ebx
	.section .t3257,"ax",@progbits
	movl 0x10(%rsp,%rsi,4), %ebx
	.section .t3258,"ax",@progbits
	movl %gs:0x10(%rcx), %ebx
	.section .t3259,"ax",@progbits
	movl %fs:(%rax,%rsi,8), %ebx
	.section .t3260,"ax",@progbits
	movl (%rcx), %r9d
	.section .t3261,"ax",@progbits
	movl 0x10(%rcx), %r9d
	.section .t3262,"ax",@progbits
	movl -0x8(%rbp), %r9d
	.section .t3263,"ax",@progbits
	movl 0x12345(%rcx), %r9d
	.section .t3264,"ax",@progbits
	movl (%rax,%rsi,4), %r9d
	.section .t3265,"ax",@progbits
	movl 0x10(%rax,%rsi,8), %r9d
	.section .t3266,"ax",@progbits
	movl (,%rsi,2), %r9d
	.section .t3267,"ax",@progbits
	movl 0x40(%rip), %r9d
	.section .t3268,"ax",@progbits
	movl -0x100(%rip), %r9d
	.section .t3269,"ax",@progbits
	movl 0x1234, %r9d
	.section .t3270,"ax",@progbits
	movl (%r8), %r9d
	.section .t3271,"ax",@progbits
	movl (%r12), %r9d
	.section .t3272,"ax",@progbits
	movl 0x8(%r13), %r9d
	.section .t3273,"ax",@progbits
	movl (%r8,%r15,2), %r9d
	.section .t3274,"ax",@progbits
	movl (%rax,%r12,4), %r9d
	.section .t3275,"ax",@progbits
	movl 0x100(%rbp), %r9d
	.section .t3276,"ax",@progbits
	movl (%rsp), %r9d
	.section .t3277,"ax",@progbits
	movl 0x10(%rsp,%rsi,4), %r9d
	.section .t3278,"ax",@progbits
	movl %gs:0x10(%rcx), %r9d
	.section .t3279,"ax",@progbits
	movl %fs:(%rax,%rsi,8), %r9d
	.section .t3280,"ax",@progbits
	movq (%rcx), %rdx
	.section .t3281,"ax",@progbits
	movq 0x10(%rcx), %rdx
	.section .t3282,"ax",@progbits
	movq -0x8(%rbp), %rdx
	.section .t3283,"ax",@progbits
	movq 0x12345(%rcx), %rdx
	.section .t3284,"ax",@progbits
	movq (%rax,%rsi,4), %rdx
	.section .t3285,"ax",@progbits
	movq 0x10(%rax,%rsi,8), %rdx
	.section .t3286,"ax",@progbits
	movq (,%rsi,2), %rdx
	.section .t3287,"ax",@progbits
	movq 0x40(%rip), %rdx
	.section .t3288,"ax",@progbits
	movq -0x100(%rip), %rdx
	.section .t3289,"ax",@progbits
	movq 0x1234, %rdx
	.section .t3290,"ax",@progbits
	movq (%r8), %rdx
	.section .t3291,"ax",@progbits
	movq (%r12), %rdx
	.section .t3292,"ax",@progbits
	movq 0x8(%r13), %rdx
	.section .t3293,"ax",@progbits
	movq (%r8,%r15,2), %rdx
	.section .t3294,"ax",@progbits
	movq (%rax,%r12,4), %rdx
	.section .t3295,"ax",@progbits
	movq 0x100(%rbp), %rdx
	.section .t3296,"ax",@progbits
	movq (%rsp), %rdx
	.section .t3297,"ax",@progbits
	movq 0x10(%rsp,%rsi,4), %rdx
	.section .t3298,"ax",@progbits
	movq %gs:0x10(%rcx), %rdx
	.section .t3299,"ax",@progbits
	movq %fs:(%rax,%rsi,8), %rdx
	.section .t3300,"ax",@progbits
	movq (%rcx), %rbx
	.section .t3301,"ax",@progbits
	movq 0x10(%rcx), %rbx
	.section .t3302,"ax",@progbits
	movq -0x8(%rbp), %rbx
	.section .t3303,"ax",@progbits
	movq 0x12345(%rcx), %rbx
	.section .t3304,"ax",@progbits
	movq (%rax,%rsi,4), %rbx
	.section .t3305,"ax",@progbits
	movq 0x10(%rax,%rsi,8), %rbx
	.section .t3306,"ax",@progbits
	movq (,%rsi,2), %rbx
	.section .t3307,"ax",@progbits
	movq 0x40(%rip), %rbx
	.section .t3308,"ax",@progbits
	movq -0x100(%rip), %rbx
	.section .t3309,"ax",@progbits
	movq 0x1234, %rbx
	.section .t3310,"ax",@progbits
	movq (%r8), %rbx
	.section .t3311,"ax",@progbits
	movq (%r12), %rbx
	.section .t3312,"ax",@progbits
	movq 0x8(%r13), %rbx
	.section .t3313,"ax",@progbits
	movq (%r8,%r15,2), %rbx
	.section .t3314,"ax",@progbits
	movq (%rax,%r12,4), %rbx
	.section .t3315,"ax",@progbits
	movq 0x100(%rbp), %rbx
	.section .t3316,"ax",@progbits
	movq (%rsp), %rbx
	.section .t3317,"ax",@progbits
	movq 0x10(%rsp,%rsi,4), %rbx
	.section .t3318,"ax",@progbits
	movq %gs:0x10(%rcx), %rbx
	.section .t3319,"ax",@progbits
	movq %fs:(%rax,%rsi,8), %rbx
	.section .t3320,"ax",@progbits
	movq (%rcx), %r9
	.section .t3321,"ax",@progbits
	movq 0x10(%rcx), %r9
	.section .t3322,"ax",@progbits
	movq -0x8(%rbp), %r9
	.section .t3323,"ax",@progbits
	movq 0x12345(%rcx), %r9
	.section .t3324,"ax",@progbits
	movq (%rax,%rsi,4), %r9
	.section .t3325,"ax",@progbits
	movq 0x10(%rax,%rsi,8), %r9
	.section .t3326,"ax",@progbits
	movq (,%rsi,2), %r9
	.section .t3327,"ax",@progbits
	movq 0x40(%rip), %r9
	.section .t3328,"ax",@progbits
	movq -0x100(%rip), %r9
	.section .t3329,"ax",@progbits
	movq 0x1234, %r9
	.section .t3330,"ax",@progbits
	movq (%r8), %r9
	.section .t3331,"ax",@progbits
	movq (%r12), %r9
	.section .t3332,"ax",@progbits
	movq 0x8(%r13), %r9
	.section .t3333,"ax",@progbits
	movq (%r8,%r15,2), %r9
	.section .t3334,"ax",@progbits
	movq (%rax,%r12,4), %r9
	.section .t3335,"ax",@progbits
	movq 0x100(%rbp), %r9
	.section .t3336,"ax",@progbits
	movq (%rsp), %r9
	.section .t3337,"ax",@progbits
	movq 0x10(%rsp,%rsi,4), %r9
	.section .t3338,"ax",@progbits
	movq %gs:0x10(%rcx), %r9
	.section .t3339,"ax",@progbits
	movq %fs:(%rax,%rsi,8), %r9
	.section .t3340,"ax",@progbits
	movq (%rcx), %r12
	.section .t3341,"ax",@progbits
	movq 0x10(%rcx), %r12
	.section .t3342,"ax",@progbits
	movq -0x8(%rbp), %r12
	.section .t3343,"ax",@progbits
	movq 0x12345(%rcx), %r12
	.section .t3344,"ax",@progbits
	movq (%rax,%rsi,4), %r12
	.section .t3345,"ax",@progbits
	movq 0x10(%rax,%rsi,8), %r12
	.section .t3346,"ax",@progbits
	movq (,%rsi,2), %r12
	.section .t3347,"ax",@progbits
	movq 0x40(%rip), %r12
	.section .t3348,"ax",@progbits
	movq -0x100(%rip), %r12
	.section .t3349,"ax",@progbits
	movq 0x1234, %r12
	.section .t3350,"ax",@progbits
	movq (%r8), %r12
	.section .t3351,"ax",@progbits
	movq (%r12), %r12
	.section .t3352,"ax",@progbits
	movq 0x8(%r13), %r12
	.section .t3353,"ax",@progbits
	movq (%r8,%r15,2), %r12
	.section .t3354,"ax",@progbits
	movq (%rax,%r12,4), %r12
	.section .t3355,"ax",@progbits
	movq 0x100(%rbp), %r12
	.section .t3356,"ax",@progbits
	movq (%rsp), %r12
	.section .t3357,"ax",@progbits
	movq 0x10(%rsp,%rsi,4), %r12
	.section .t3358,"ax",@progbits
	movq %gs:0x10(%rcx), %r12
	.section .t3359,"ax",@progbits
	movq %fs:(%rax,%rsi,8), %r12
	.section .t3360,"ax",@progbits
	movzbl (%rcx), %edx
	.section .t3361,"ax",@progbits
	movzbl 0x10(%rcx), %edx
	.section .t3362,"ax",@progbits
	movzbl -0x8(%rbp), %edx
	.section .t3363,"ax",@progbits
	movzbl 0x12345(%rcx), %edx
	.section .t3364,"ax",@progbits
	movzbl (%rax,%rsi,4), %edx
	.section .t3365,"ax",@progbits
	movzbl 0x10(%rax,%rsi,8), %edx
	.section .t3366,"ax",@progbits
	movzbl (,%rsi,2), %edx
	.section .t3367,"ax",@progbits
	movzbl 0x40(%rip), %edx
	.section .t3368,"ax",@progbits
	movzbl -0x100(%rip), %edx
	.section .t3369,"ax",@progbits
	movzbl 0x1234, %edx
	.section .t3370,"ax",@progbits
	movzbl (%r8), %edx
	.section .t3371,"ax",@progbits
	movzbl (%r12), %edx
	.section .t3372,"ax",@progbits
	movzbl 0x8(%r13), %edx
	.section .t3373,"ax",@progbits
	movzbl (%r8,%r15,2), %edx
	.section .t3374,"ax",@progbits
	movzbl (%rax,%r12,4), %edx
	.section .t3375,"ax",@progbits
	movzbl 0x100(%rbp), %edx
	.section .t3376,"ax",@progbits
	movzbl (%rsp), %edx
	.section .t3377,"ax",@progbits
	movzbl 0x10(%rsp,%rsi,4), %edx
	.section .t3378,"ax",@progbits
	movzbl %gs:0x10(%rcx), %edx
	.section .t3379,"ax",@progbits
	movzbl %fs:(%rax,%rsi,8), %edx
	.section .t3380,"ax",@progbits
	movzwl (%rcx), %edx
	.section .t3381,"ax",@progbits
	movzwl 0x10(%rcx), %edx
	.section .t3382,"ax",@progbits
	movzwl -0x8(%rbp), %edx
	.section .t3383,"ax",@progbits
	movzwl 0x12345(%rcx), %edx
	.section .t3384,"ax",@progbits
	movzwl (%rax,%rsi,4), %edx
	.section .t3385,"ax",@progbits
	movzwl 0x10(%rax,%rsi,8), %edx
	.section .t3386,"ax",@progbits
	movzwl (,%rsi,2), %edx
	.section .t3387,"ax",@progbits
	movzwl 0x40(%rip), %edx
	.section .t3388,"ax",@progbits
	movzwl -0x100(%rip), %edx
	.section .t3389,"ax",@progbits
	movzwl 0x1234, %edx
	.section .t3390,"ax",@progbits
	movzwl (%r8), %edx
	.section .t3391,"ax",@progbits
	movzwl (%r12), %edx
	.section .t3392,"ax",@progbits
	movzwl 0x8(%r13), %edx
	.section .t3393,"ax",@progbits
	movzwl (%r8,%r15,2), %edx
	.section .t3394,"ax",@progbits
	movzwl (%rax,%r12,4), %edx
	.section .t3395,"ax",@progbits
	movzwl 0x100(%rbp), %edx
	.section .t3396,"ax",@progbits
	movzwl (%rsp), %edx
	.section .t3397,"ax",@progbits
	movzwl 0x10(%rsp,%rsi,4), %edx
	.section .t3398,"ax",@progbits
	movzwl %gs:0x10(%rcx), %edx
	.section .t3399,"ax",@progbits
	movzwl %fs:(%rax,%rsi,8), %edx
	.section .t3400,"ax",@progbits
	movsbl (%rcx), %edx
	.section .t3401,"ax",@progbits
	movsbl 0x10(%rcx), %edx
	.section .t3402,"ax",@progbits
	movsbl -0x8(%rbp), %edx
	.section .t3403,"ax",@progbits
	movsbl 0x12345(%rcx), %edx
	.section .t3404,"ax",@progbits
	movsbl (%rax,%rsi,4), %edx
	.section .t3405,"ax",@progbits
	movsbl 0x10(%rax,%rsi,8), %edx
	.section .t3406,"ax",@progbits
	movsbl (,%rsi,2), %edx
	.section .t3407,"ax",@progbits
	movsbl 0x40(%rip), %edx
	.section .t3408,"ax",@progbits
	movsbl -0x100(%rip), %edx
	.section .t3409,"ax",@progbits
	movsbl 0x1234, %edx
	.section .t3410,"ax",@progbits
	movsbl (%r8), %edx
	.section .t3411,"ax",@progbits
	movsbl (%r12), %edx
	.section .t3412,"ax",@progbits
	movsbl 0x8(%r13), %edx
	.section .t3413,"ax",@progbits
	movsbl (%r8,%r15,2), %edx
	.section .t3414,"ax",@progbits
	movsbl (%rax,%r12,4), %edx
	.section .t3415,"ax",@progbits
	movsbl 0x100(%rbp), %edx
	.section .t3416,"ax",@progbits
	movsbl (%rsp), %edx
	.section .t3417,"ax",@progbits
	movsbl 0x10(%rsp,%rsi,4), %edx
	.section .t3418,"ax",@progbits
	movsbl %gs:0x10(%rcx), %edx
	.section .t3419,"ax",@progbits
	movsbl %fs:(%rax,%rsi,8), %edx
	.section .t3420,"ax",@progbits
	movswl (%rcx), %edx
	.section .t3421,"ax",@progbits
	movswl 0x10(%rcx), %edx
	.section .t3422,"ax",@progbits
	movswl -0x8(%rbp), %edx
	.section .t3423,"ax",@progbits
	movswl 0x12345(%rcx), %edx
	.section .t3424,"ax",@progbits
	movswl (%rax,%rsi,4), %edx
	.section .t3425,"ax",@progbits
	movswl 0x10(%rax,%rsi,8), %edx
	.section .t3426,"ax",@progbits
	movswl (,%rsi,2), %edx
	.section .t3427,"ax",@progbits
	movswl 0x40(%rip), %edx
	.section .t3428,"ax",@progbits
	movswl -0x100(%rip), %edx
	.section .t3429,"ax",@progbits
	movswl 0x1234, %edx
	.section .t3430,"ax",@progbits
	movswl (%r8), %edx
	.section .t3431,"ax",@progbits
	movswl (%r12), %edx
	.section .t3432,"ax",@progbits
	movswl 0x8(%r13), %edx
	.section .t3433,"ax",@progbits
	movswl (%r8,%r15,2), %edx
	.section .t3434,"ax",@progbits
	movswl (%rax,%r12,4), %edx
	.section .t3435,"ax",@progbits
	movswl 0x100(%rbp), %edx
	.section .t3436,"ax",@progbits
	movswl (%rsp), %edx
	.section .t3437,"ax",@progbits
	movswl 0x10(%rsp,%rsi,4), %edx
	.section .t3438,"ax",@progbits
	movswl %gs:0x10(%rcx), %edx
	.section .t3439,"ax",@progbits
	movswl %fs:(%rax,%rsi,8), %edx
	.section .t3440,"ax",@progbits
	movzbq (%rcx), %r9
	.section .t3441,"ax",@progbits
	movzbq 0x10(%rcx), %r9
	.section .t3442,"ax",@progbits
	movzbq -0x8(%rbp), %r9
	.section .t3443,"ax",@progbits
	movzbq 0x12345(%rcx), %r9
	.section .t3444,"ax",@progbits
	movzbq (%rax,%rsi,4), %r9
	.section .t3445,"ax",@progbits
	movzbq 0x10(%rax,%rsi,8), %r9
	.section .t3446,"ax",@progbits
	movzbq (,%rsi,2), %r9
	.section .t3447,"ax",@progbits
	movzbq 0x40(%rip), %r9
	.section .t3448,"ax",@progbits
	movzbq -0x100(%rip), %r9
	.section .t3449,"ax",@progbits
	movzbq 0x1234, %r9
	.section .t3450,"ax",@progbits
	movzbq (%r8), %r9
	.section .t3451,"ax",@progbits
	movzbq (%r12), %r9
	.section .t3452,"ax",@progbits
	movzbq 0x8(%r13), %r9
	.section .t3453,"ax",@progbits
	movzbq (%r8,%r15,2), %r9
	.section .t3454,"ax",@progbits
	movzbq (%rax,%r12,4), %r9
	.section .t3455,"ax",@progbits
	movzbq 0x100(%rbp), %r9
	.section .t3456,"ax",@progbits
	movzbq (%rsp), %r9
	.section .t3457,"ax",@progbits
	movzbq 0x10(%rsp,%rsi,4), %r9
	.section .t3458,"ax",@progbits
	movzbq %gs:0x10(%rcx), %r9
	.section .t3459,"ax",@progbits
	movzbq %fs:(%rax,%rsi,8), %r9
	.section .t3460,"ax",@progbits
	movzwq (%rcx), %r9
	.section .t3461,"ax",@progbits
	movzwq 0x10(%rcx), %r9
	.section .t3462,"ax",@progbits
	movzwq -0x8(%rbp), %r9
	.section .t3463,"ax",@progbits
	movzwq 0x12345(%rcx), %r9
	.section .t3464,"ax",@progbits
	movzwq (%rax,%rsi,4), %r9
	.section .t3465,"ax",@progbits
	movzwq 0x10(%rax,%rsi,8), %r9
	.section .t3466,"ax",@progbits
	movzwq (,%rsi,2), %r9
	.section .t3467,"ax",@progbits
	movzwq 0x40(%rip), %r9
	.section .t3468,"ax",@progbits
	movzwq -0x100(%rip), %r9
	.section .t3469,"ax",@progbits
	movzwq 0x1234, %r9
	.section .t3470,"ax",@progbits
	movzwq (%r8), %r9
	.section .t3471,"ax",@progbits
	movzwq (%r12), %r9
	.section .t3472,"ax",@progbits
	movzwq 0x8(%r13), %r9
	.section .t3473,"ax",@progbits
	movzwq (%r8,%r15,2), %r9
	.section .t3474,"ax",@progbits
	movzwq (%rax,%r12,4), %r9
	.section .t3475,"ax",@progbits
	movzwq 0x100(%rbp), %r9
	.section .t3476,"ax",@progbits
	movzwq (%rsp), %r9
	.section .t3477,"ax",@progbits
	movzwq 0x10(%rsp,%rsi,4), %r9
	.section .t3478,"ax",@progbits
	movzwq %gs:0x10(%rcx), %r9
	.section .t3479,"ax",@progbits
	movzwq %fs:(%rax,%rsi,8), %r9
	.section .t3480,"ax",@progbits
	movsbq (%rcx), %r9
	.section .t3481,"ax",@progbits
	movsbq 0x10(%rcx), %r9
	.section .t3482,"ax",@progbits
	movsbq -0x8(%rbp), %r9
	.section .t3483,"ax",@progbits
	movsbq 0x12345(%rcx), %r9
	.section .t3484,"ax",@progbits
	movsbq (%rax,%rsi,4), %r9
	.section .t3485,"ax",@progbits
	movsbq 0x10(%rax,%rsi,8), %r9
	.section .t3486,"ax",@progbits
	movsbq (,%rsi,2), %r9
	.section .t3487,"ax",@progbits
	movsbq 0x40(%rip), %r9
	.section .t3488,"ax",@progbits
	movsbq -0x100(%rip), %r9
	.section .t3489,"ax",@progbits
	movsbq 0x1234, %r9
	.section .t3490,"ax",@progbits
	movsbq (%r8), %r9
	.section .t3491,"ax",@progbits
	movsbq (%r12), %r9
	.section .t3492,"ax",@progbits
	movsbq 0x8(%r13), %r9
	.section .t3493,"ax",@progbits
	movsbq (%r8,%r15,2), %r9
	.section .t3494,"ax",@progbits
	movsbq (%rax,%r12,4), %r9
	.section .t3495,"ax",@progbits
	movsbq 0x100(%rbp), %r9
	.section .t3496,"ax",@progbits
	movsbq (%rsp), %r9
	.section .t3497,"ax",@progbits
	movsbq 0x10(%rsp,%rsi,4), %r9
	.section .t3498,"ax",@progbits
	movsbq %gs:0x10(%rcx), %r9
	.section .t3499,"ax",@progbits
	movsbq %fs:(%rax,%rsi,8), %r9
	.section .t3500,"ax",@progbits
	movswq (%rcx), %r9
	.section .t3501,"ax",@progbits
	movswq 0x10(%rcx), %r9
	.section .t3502,"ax",@progbits
	movswq -0x8(%rbp), %r9
	.section .t3503,"ax",@progbits
	movswq 0x12345(%rcx), %r9
	.section .t3504,"ax",@progbits
	movswq (%rax,%rsi,4), %r9
	.section .t3505,"ax",@progbits
	movswq 0x10(%rax,%rsi,8), %r9
	.section .t3506,"ax",@progbits
	movswq (,%rsi,2), %r9
	.section .t3507,"ax",@progbits
	movswq 0x40(%rip), %r9
	.section .t3508,"ax",@progbits
	movswq -0x100(%rip), %r9
	.section .t3509,"ax",@progbits
	movswq 0x1234, %r9
	.section .t3510,"ax",@progbits
	movswq (%r8), %r9
	.section .t3511,"ax",@progbits
	movswq (%r12), %r9
	.section .t3512,"ax",@progbits
	movswq 0x8(%r13), %r9
	.section .t3513,"ax",@progbits
	movswq (%r8,%r15,2), %r9
	.section .t3514,"ax",@progbits
	movswq (%rax,%r12,4), %r9
	.section .t3515,"ax",@progbits
	movswq 0x100(%rbp), %r9
	.section .t3516,"ax",@progbits
	movswq (%rsp), %r9
	.section .t3517,"ax",@progbits
	movswq 0x10(%rsp,%rsi,4), %r9
	.section .t3518,"ax",@progbits
	movswq %gs:0x10(%rcx), %r9
	.section .t3519,"ax",@progbits
	movswq %fs:(%rax,%rsi,8), %r9
	.section .t3520,"ax",@progbits
	addb $0x7, (%rcx)
	.section .t3521,"ax",@progbits
	addb $0x7f, (%rcx)
	.section .t3522,"ax",@progbits
	addb $0x7, 0x10(%rcx)
	.section .t3523,"ax",@progbits
	addb $0x7f, 0x10(%rcx)
	.section .t3524,"ax",@progbits
	addb $0x7, -0x8(%rbp)
	.section .t3525,"ax",@progbits
	addb $0x7f, -0x8(%rbp)
	.section .t3526,"ax",@progbits
	addb $0x7, 0x12345(%rcx)
	.section .t3527,"ax",@progbits
	addb $0x7f, 0x12345(%rcx)
	.section .t3528,"ax",@progbits
	addb $0x7, (%rax,%rsi,4)
	.section .t3529,"ax",@progbits
	addb $0x7f, (%rax,%rsi,4)
	.section .t3530,"ax",@progbits
	addb $0x7, 0x10(%rax,%rsi,8)
	.section .t3531,"ax",@progbits
	addb $0x7f, 0x10(%rax,%rsi,8)
	.section .t3532,"ax",@progbits
	addb $0x7, (,%rsi,2)
	.section .t3533,"ax",@progbits
	addb $0x7f, (,%rsi,2)
	.section .t3534,"ax",@progbits
	addb $0x7, 0x40(%rip)
	.section .t3535,"ax",@progbits
	addb $0x7f, 0x40(%rip)
	.section .t3536,"ax",@progbits
	addb $0x7, -0x100(%rip)
	.section .t3537,"ax",@progbits
	addb $0x7f, -0x100(%rip)
	.section .t3538,"ax",@progbits
	addb $0x7, 0x1234
	.section .t3539,"ax",@progbits
	addb $0x7f, 0x1234
	.section .t3540,"ax",@progbits
	addb $0x7, (%r8)
	.section .t3541,"ax",@progbits
	addb $0x7f, (%r8)
	.section .t3542,"ax",@progbits
	addb $0x7, (%r12)
	.section .t3543,"ax",@progbits
	addb $0x7f, (%r12)
	.section .t3544,"ax",@progbits
	addb $0x7, 0x8(%r13)
	.section .t3545,"ax",@progbits
	addb $0x7f, 0x8(%r13)
	.section .t3546,"ax",@progbits
	addb $0x7, (%r8,%r15,2)
	.section .t3547,"ax",@progbits
	addb $0x7f, (%r8,%r15,2)
	.section .t3548,"ax",@progbits
	addb $0x7, (%rax,%r12,4)
	.section .t3549,"ax",@progbits
	addb $0x7f, (%rax,%r12,4)
	.section .t3550,"ax",@progbits
	addb $0x7, 0x100(%rbp)
	.section .t3551,"ax",@progbits
	addb $0x7f, 0x100(%rbp)
	.section .t3552,"ax",@progbits
	addb $0x7, (%rsp)
	.section .t3553,"ax",@progbits
	addb $0x7f, (%rsp)
	.section .t3554,"ax",@progbits
	addb $0x7, 0x10(%rsp,%rsi,4)
	.section .t3555,"ax",@progbits
	addb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t3556,"ax",@progbits
	addb $0x7, %gs:0x10(%rcx)
	.section .t3557,"ax",@progbits
	addb $0x7f, %gs:0x10(%rcx)
	.section .t3558,"ax",@progbits
	addb $0x7, %fs:(%rax,%rsi,8)
	.section .t3559,"ax",@progbits
	addb $0x7f, %fs:(%rax,%rsi,8)
	.section .t3560,"ax",@progbits
	addw $0x7, (%rcx)
	.section .t3561,"ax",@progbits
	addw $0x12345, (%rcx)
	.section .t3562,"ax",@progbits
	addw $0x7, 0x10(%rcx)
	.section .t3563,"ax",@progbits
	addw $0x12345, 0x10(%rcx)
	.section .t3564,"ax",@progbits
	addw $0x7, -0x8(%rbp)
	.section .t3565,"ax",@progbits
	addw $0x12345, -0x8(%rbp)
	.section .t3566,"ax",@progbits
	addw $0x7, 0x12345(%rcx)
	.section .t3567,"ax",@progbits
	addw $0x12345, 0x12345(%rcx)
	.section .t3568,"ax",@progbits
	addw $0x7, (%rax,%rsi,4)
	.section .t3569,"ax",@progbits
	addw $0x12345, (%rax,%rsi,4)
	.section .t3570,"ax",@progbits
	addw $0x7, 0x10(%rax,%rsi,8)
	.section .t3571,"ax",@progbits
	addw $0x12345, 0x10(%rax,%rsi,8)
	.section .t3572,"ax",@progbits
	addw $0x7, (,%rsi,2)
	.section .t3573,"ax",@progbits
	addw $0x12345, (,%rsi,2)
	.section .t3574,"ax",@progbits
	addw $0x7, 0x40(%rip)
	.section .t3575,"ax",@progbits
	addw $0x12345, 0x40(%rip)
	.section .t3576,"ax",@progbits
	addw $0x7, -0x100(%rip)
	.section .t3577,"ax",@progbits
	addw $0x12345, -0x100(%rip)
	.section .t3578,"ax",@progbits
	addw $0x7, 0x1234
	.section .t3579,"ax",@progbits
	addw $0x12345, 0x1234
	.section .t3580,"ax",@progbits
	addw $0x7, (%r8)
	.section .t3581,"ax",@progbits
	addw $0x12345, (%r8)
	.section .t3582,"ax",@progbits
	addw $0x7, (%r12)
	.section .t3583,"ax",@progbits
	addw $0x12345, (%r12)
	.section .t3584,"ax",@progbits
	addw $0x7, 0x8(%r13)
	.section .t3585,"ax",@progbits
	addw $0x12345, 0x8(%r13)
	.section .t3586,"ax",@progbits
	addw $0x7, (%r8,%r15,2)
	.section .t3587,"ax",@progbits
	addw $0x12345, (%r8,%r15,2)
	.section .t3588,"ax",@progbits
	addw $0x7, (%rax,%r12,4)
	.section .t3589,"ax",@progbits
	addw $0x12345, (%rax,%r12,4)
	.section .t3590,"ax",@progbits
	addw $0x7, 0x100(%rbp)
	.section .t3591,"ax",@progbits
	addw $0x12345, 0x100(%rbp)
	.section .t3592,"ax",@progbits
	addw $0x7, (%rsp)
	.section .t3593,"ax",@progbits
	addw $0x12345, (%rsp)
	.section .t3594,"ax",@progbits
	addw $0x7, 0x10(%rsp,%rsi,4)
	.section .t3595,"ax",@progbits
	addw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3596,"ax",@progbits
	addw $0x7, %gs:0x10(%rcx)
	.section .t3597,"ax",@progbits
	addw $0x12345, %gs:0x10(%rcx)
	.section .t3598,"ax",@progbits
	addw $0x7, %fs:(%rax,%rsi,8)
	.section .t3599,"ax",@progbits
	addw $0x12345, %fs:(%rax,%rsi,8)
	.section .t3600,"ax",@progbits
	addl $0x7, (%rcx)
	.section .t3601,"ax",@progbits
	addl $0x12345, (%rcx)
	.section .t3602,"ax",@progbits
	addl $0x7, 0x10(%rcx)
	.section .t3603,"ax",@progbits
	addl $0x12345, 0x10(%rcx)
	.section .t3604,"ax",@progbits
	addl $0x7, -0x8(%rbp)
	.section .t3605,"ax",@progbits
	addl $0x12345, -0x8(%rbp)
	.section .t3606,"ax",@progbits
	addl $0x7, 0x12345(%rcx)
	.section .t3607,"ax",@progbits
	addl $0x12345, 0x12345(%rcx)
	.section .t3608,"ax",@progbits
	addl $0x7, (%rax,%rsi,4)
	.section .t3609,"ax",@progbits
	addl $0x12345, (%rax,%rsi,4)
	.section .t3610,"ax",@progbits
	addl $0x7, 0x10(%rax,%rsi,8)
	.section .t3611,"ax",@progbits
	addl $0x12345, 0x10(%rax,%rsi,8)
	.section .t3612,"ax",@progbits
	addl $0x7, (,%rsi,2)
	.section .t3613,"ax",@progbits
	addl $0x12345, (,%rsi,2)
	.section .t3614,"ax",@progbits
	addl $0x7, 0x40(%rip)
	.section .t3615,"ax",@progbits
	addl $0x12345, 0x40(%rip)
	.section .t3616,"ax",@progbits
	addl $0x7, -0x100(%rip)
	.section .t3617,"ax",@progbits
	addl $0x12345, -0x100(%rip)
	.section .t3618,"ax",@progbits
	addl $0x7, 0x1234
	.section .t3619,"ax",@progbits
	addl $0x12345, 0x1234
	.section .t3620,"ax",@progbits
	addl $0x7, (%r8)
	.section .t3621,"ax",@progbits
	addl $0x12345, (%r8)
	.section .t3622,"ax",@progbits
	addl $0x7, (%r12)
	.section .t3623,"ax",@progbits
	addl $0x12345, (%r12)
	.section .t3624,"ax",@progbits
	addl $0x7, 0x8(%r13)
	.section .t3625,"ax",@progbits
	addl $0x12345, 0x8(%r13)
	.section .t3626,"ax",@progbits
	addl $0x7, (%r8,%r15,2)
	.section .t3627,"ax",@progbits
	addl $0x12345, (%r8,%r15,2)
	.section .t3628,"ax",@progbits
	addl $0x7, (%rax,%r12,4)
	.section .t3629,"ax",@progbits
	addl $0x12345, (%rax,%r12,4)
	.section .t3630,"ax",@progbits
	addl $0x7, 0x100(%rbp)
	.section .t3631,"ax",@progbits
	addl $0x12345, 0x100(%rbp)
	.section .t3632,"ax",@progbits
	addl $0x7, (%rsp)
	.section .t3633,"ax",@progbits
	addl $0x12345, (%rsp)
	.section .t3634,"ax",@progbits
	addl $0x7, 0x10(%rsp,%rsi,4)
	.section .t3635,"ax",@progbits
	addl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3636,"ax",@progbits
	addl $0x7, %gs:0x10(%rcx)
	.section .t3637,"ax",@progbits
	addl $0x12345, %gs:0x10(%rcx)
	.section .t3638,"ax",@progbits
	addl $0x7, %fs:(%rax,%rsi,8)
	.section .t3639,"ax",@progbits
	addl $0x12345, %fs:(%rax,%rsi,8)
	.section .t3640,"ax",@progbits
	addq $0x7, (%rcx)
	.section .t3641,"ax",@progbits
	addq $0x12345, (%rcx)
	.section .t3642,"ax",@progbits
	addq $0x7, 0x10(%rcx)
	.section .t3643,"ax",@progbits
	addq $0x12345, 0x10(%rcx)
	.section .t3644,"ax",@progbits
	addq $0x7, -0x8(%rbp)
	.section .t3645,"ax",@progbits
	addq $0x12345, -0x8(%rbp)
	.section .t3646,"ax",@progbits
	addq $0x7, 0x12345(%rcx)
	.section .t3647,"ax",@progbits
	addq $0x12345, 0x12345(%rcx)
	.section .t3648,"ax",@progbits
	addq $0x7, (%rax,%rsi,4)
	.section .t3649,"ax",@progbits
	addq $0x12345, (%rax,%rsi,4)
	.section .t3650,"ax",@progbits
	addq $0x7, 0x10(%rax,%rsi,8)
	.section .t3651,"ax",@progbits
	addq $0x12345, 0x10(%rax,%rsi,8)
	.section .t3652,"ax",@progbits
	addq $0x7, (,%rsi,2)
	.section .t3653,"ax",@progbits
	addq $0x12345, (,%rsi,2)
	.section .t3654,"ax",@progbits
	addq $0x7, 0x40(%rip)
	.section .t3655,"ax",@progbits
	addq $0x12345, 0x40(%rip)
	.section .t3656,"ax",@progbits
	addq $0x7, -0x100(%rip)
	.section .t3657,"ax",@progbits
	addq $0x12345, -0x100(%rip)
	.section .t3658,"ax",@progbits
	addq $0x7, 0x1234
	.section .t3659,"ax",@progbits
	addq $0x12345, 0x1234
	.section .t3660,"ax",@progbits
	addq $0x7, (%r8)
	.section .t3661,"ax",@progbits
	addq $0x12345, (%r8)
	.section .t3662,"ax",@progbits
	addq $0x7, (%r12)
	.section .t3663,"ax",@progbits
	addq $0x12345, (%r12)
	.section .t3664,"ax",@progbits
	addq $0x7, 0x8(%r13)
	.section .t3665,"ax",@progbits
	addq $0x12345, 0x8(%r13)
	.section .t3666,"ax",@progbits
	addq $0x7, (%r8,%r15,2)
	.section .t3667,"ax",@progbits
	addq $0x12345, (%r8,%r15,2)
	.section .t3668,"ax",@progbits
	addq $0x7, (%rax,%r12,4)
	.section .t3669,"ax",@progbits
	addq $0x12345, (%rax,%r12,4)
	.section .t3670,"ax",@progbits
	addq $0x7, 0x100(%rbp)
	.section .t3671,"ax",@progbits
	addq $0x12345, 0x100(%rbp)
	.section .t3672,"ax",@progbits
	addq $0x7, (%rsp)
	.section .t3673,"ax",@progbits
	addq $0x12345, (%rsp)
	.section .t3674,"ax",@progbits
	addq $0x7, 0x10(%rsp,%rsi,4)
	.section .t3675,"ax",@progbits
	addq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3676,"ax",@progbits
	addq $0x7, %gs:0x10(%rcx)
	.section .t3677,"ax",@progbits
	addq $0x12345, %gs:0x10(%rcx)
	.section .t3678,"ax",@progbits
	addq $0x7, %fs:(%rax,%rsi,8)
	.section .t3679,"ax",@progbits
	addq $0x12345, %fs:(%rax,%rsi,8)
	.section .t3680,"ax",@progbits
	orb $0x7, (%rcx)
	.section .t3681,"ax",@progbits
	orb $0x7f, (%rcx)
	.section .t3682,"ax",@progbits
	orb $0x7, 0x10(%rcx)
	.section .t3683,"ax",@progbits
	orb $0x7f, 0x10(%rcx)
	.section .t3684,"ax",@progbits
	orb $0x7, -0x8(%rbp)
	.section .t3685,"ax",@progbits
	orb $0x7f, -0x8(%rbp)
	.section .t3686,"ax",@progbits
	orb $0x7, 0x12345(%rcx)
	.section .t3687,"ax",@progbits
	orb $0x7f, 0x12345(%rcx)
	.section .t3688,"ax",@progbits
	orb $0x7, (%rax,%rsi,4)
	.section .t3689,"ax",@progbits
	orb $0x7f, (%rax,%rsi,4)
	.section .t3690,"ax",@progbits
	orb $0x7, 0x10(%rax,%rsi,8)
	.section .t3691,"ax",@progbits
	orb $0x7f, 0x10(%rax,%rsi,8)
	.section .t3692,"ax",@progbits
	orb $0x7, (,%rsi,2)
	.section .t3693,"ax",@progbits
	orb $0x7f, (,%rsi,2)
	.section .t3694,"ax",@progbits
	orb $0x7, 0x40(%rip)
	.section .t3695,"ax",@progbits
	orb $0x7f, 0x40(%rip)
	.section .t3696,"ax",@progbits
	orb $0x7, -0x100(%rip)
	.section .t3697,"ax",@progbits
	orb $0x7f, -0x100(%rip)
	.section .t3698,"ax",@progbits
	orb $0x7, 0x1234
	.section .t3699,"ax",@progbits
	orb $0x7f, 0x1234
	.section .t3700,"ax",@progbits
	orb $0x7, (%r8)
	.section .t3701,"ax",@progbits
	orb $0x7f, (%r8)
	.section .t3702,"ax",@progbits
	orb $0x7, (%r12)
	.section .t3703,"ax",@progbits
	orb $0x7f, (%r12)
	.section .t3704,"ax",@progbits
	orb $0x7, 0x8(%r13)
	.section .t3705,"ax",@progbits
	orb $0x7f, 0x8(%r13)
	.section .t3706,"ax",@progbits
	orb $0x7, (%r8,%r15,2)
	.section .t3707,"ax",@progbits
	orb $0x7f, (%r8,%r15,2)
	.section .t3708,"ax",@progbits
	orb $0x7, (%rax,%r12,4)
	.section .t3709,"ax",@progbits
	orb $0x7f, (%rax,%r12,4)
	.section .t3710,"ax",@progbits
	orb $0x7, 0x100(%rbp)
	.section .t3711,"ax",@progbits
	orb $0x7f, 0x100(%rbp)
	.section .t3712,"ax",@progbits
	orb $0x7, (%rsp)
	.section .t3713,"ax",@progbits
	orb $0x7f, (%rsp)
	.section .t3714,"ax",@progbits
	orb $0x7, 0x10(%rsp,%rsi,4)
	.section .t3715,"ax",@progbits
	orb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t3716,"ax",@progbits
	orb $0x7, %gs:0x10(%rcx)
	.section .t3717,"ax",@progbits
	orb $0x7f, %gs:0x10(%rcx)
	.section .t3718,"ax",@progbits
	orb $0x7, %fs:(%rax,%rsi,8)
	.section .t3719,"ax",@progbits
	orb $0x7f, %fs:(%rax,%rsi,8)
	.section .t3720,"ax",@progbits
	orw $0x7, (%rcx)
	.section .t3721,"ax",@progbits
	orw $0x12345, (%rcx)
	.section .t3722,"ax",@progbits
	orw $0x7, 0x10(%rcx)
	.section .t3723,"ax",@progbits
	orw $0x12345, 0x10(%rcx)
	.section .t3724,"ax",@progbits
	orw $0x7, -0x8(%rbp)
	.section .t3725,"ax",@progbits
	orw $0x12345, -0x8(%rbp)
	.section .t3726,"ax",@progbits
	orw $0x7, 0x12345(%rcx)
	.section .t3727,"ax",@progbits
	orw $0x12345, 0x12345(%rcx)
	.section .t3728,"ax",@progbits
	orw $0x7, (%rax,%rsi,4)
	.section .t3729,"ax",@progbits
	orw $0x12345, (%rax,%rsi,4)
	.section .t3730,"ax",@progbits
	orw $0x7, 0x10(%rax,%rsi,8)
	.section .t3731,"ax",@progbits
	orw $0x12345, 0x10(%rax,%rsi,8)
	.section .t3732,"ax",@progbits
	orw $0x7, (,%rsi,2)
	.section .t3733,"ax",@progbits
	orw $0x12345, (,%rsi,2)
	.section .t3734,"ax",@progbits
	orw $0x7, 0x40(%rip)
	.section .t3735,"ax",@progbits
	orw $0x12345, 0x40(%rip)
	.section .t3736,"ax",@progbits
	orw $0x7, -0x100(%rip)
	.section .t3737,"ax",@progbits
	orw $0x12345, -0x100(%rip)
	.section .t3738,"ax",@progbits
	orw $0x7, 0x1234
	.section .t3739,"ax",@progbits
	orw $0x12345, 0x1234
	.section .t3740,"ax",@progbits
	orw $0x7, (%r8)
	.section .t3741,"ax",@progbits
	orw $0x12345, (%r8)
	.section .t3742,"ax",@progbits
	orw $0x7, (%r12)
	.section .t3743,"ax",@progbits
	orw $0x12345, (%r12)
	.section .t3744,"ax",@progbits
	orw $0x7, 0x8(%r13)
	.section .t3745,"ax",@progbits
	orw $0x12345, 0x8(%r13)
	.section .t3746,"ax",@progbits
	orw $0x7, (%r8,%r15,2)
	.section .t3747,"ax",@progbits
	orw $0x12345, (%r8,%r15,2)
	.section .t3748,"ax",@progbits
	orw $0x7, (%rax,%r12,4)
	.section .t3749,"ax",@progbits
	orw $0x12345, (%rax,%r12,4)
	.section .t3750,"ax",@progbits
	orw $0x7, 0x100(%rbp)
	.section .t3751,"ax",@progbits
	orw $0x12345, 0x100(%rbp)
	.section .t3752,"ax",@progbits
	orw $0x7, (%rsp)
	.section .t3753,"ax",@progbits
	orw $0x12345, (%rsp)
	.section .t3754,"ax",@progbits
	orw $0x7, 0x10(%rsp,%rsi,4)
	.section .t3755,"ax",@progbits
	orw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3756,"ax",@progbits
	orw $0x7, %gs:0x10(%rcx)
	.section .t3757,"ax",@progbits
	orw $0x12345, %gs:0x10(%rcx)
	.section .t3758,"ax",@progbits
	orw $0x7, %fs:(%rax,%rsi,8)
	.section .t3759,"ax",@progbits
	orw $0x12345, %fs:(%rax,%rsi,8)
	.section .t3760,"ax",@progbits
	orl $0x7, (%rcx)
	.section .t3761,"ax",@progbits
	orl $0x12345, (%rcx)
	.section .t3762,"ax",@progbits
	orl $0x7, 0x10(%rcx)
	.section .t3763,"ax",@progbits
	orl $0x12345, 0x10(%rcx)
	.section .t3764,"ax",@progbits
	orl $0x7, -0x8(%rbp)
	.section .t3765,"ax",@progbits
	orl $0x12345, -0x8(%rbp)
	.section .t3766,"ax",@progbits
	orl $0x7, 0x12345(%rcx)
	.section .t3767,"ax",@progbits
	orl $0x12345, 0x12345(%rcx)
	.section .t3768,"ax",@progbits
	orl $0x7, (%rax,%rsi,4)
	.section .t3769,"ax",@progbits
	orl $0x12345, (%rax,%rsi,4)
	.section .t3770,"ax",@progbits
	orl $0x7, 0x10(%rax,%rsi,8)
	.section .t3771,"ax",@progbits
	orl $0x12345, 0x10(%rax,%rsi,8)
	.section .t3772,"ax",@progbits
	orl $0x7, (,%rsi,2)
	.section .t3773,"ax",@progbits
	orl $0x12345, (,%rsi,2)
	.section .t3774,"ax",@progbits
	orl $0x7, 0x40(%rip)
	.section .t3775,"ax",@progbits
	orl $0x12345, 0x40(%rip)
	.section .t3776,"ax",@progbits
	orl $0x7, -0x100(%rip)
	.section .t3777,"ax",@progbits
	orl $0x12345, -0x100(%rip)
	.section .t3778,"ax",@progbits
	orl $0x7, 0x1234
	.section .t3779,"ax",@progbits
	orl $0x12345, 0x1234
	.section .t3780,"ax",@progbits
	orl $0x7, (%r8)
	.section .t3781,"ax",@progbits
	orl $0x12345, (%r8)
	.section .t3782,"ax",@progbits
	orl $0x7, (%r12)
	.section .t3783,"ax",@progbits
	orl $0x12345, (%r12)
	.section .t3784,"ax",@progbits
	orl $0x7, 0x8(%r13)
	.section .t3785,"ax",@progbits
	orl $0x12345, 0x8(%r13)
	.section .t3786,"ax",@progbits
	orl $0x7, (%r8,%r15,2)
	.section .t3787,"ax",@progbits
	orl $0x12345, (%r8,%r15,2)
	.section .t3788,"ax",@progbits
	orl $0x7, (%rax,%r12,4)
	.section .t3789,"ax",@progbits
	orl $0x12345, (%rax,%r12,4)
	.section .t3790,"ax",@progbits
	orl $0x7, 0x100(%rbp)
	.section .t3791,"ax",@progbits
	orl $0x12345, 0x100(%rbp)
	.section .t3792,"ax",@progbits
	orl $0x7, (%rsp)
	.section .t3793,"ax",@progbits
	orl $0x12345, (%rsp)
	.section .t3794,"ax",@progbits
	orl $0x7, 0x10(%rsp,%rsi,4)
	.section .t3795,"ax",@progbits
	orl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3796,"ax",@progbits
	orl $0x7, %gs:0x10(%rcx)
	.section .t3797,"ax",@progbits
	orl $0x12345, %gs:0x10(%rcx)
	.section .t3798,"ax",@progbits
	orl $0x7, %fs:(%rax,%rsi,8)
	.section .t3799,"ax",@progbits
	orl $0x12345, %fs:(%rax,%rsi,8)
	.section .t3800,"ax",@progbits
	orq $0x7, (%rcx)
	.section .t3801,"ax",@progbits
	orq $0x12345, (%rcx)
	.section .t3802,"ax",@progbits
	orq $0x7, 0x10(%rcx)
	.section .t3803,"ax",@progbits
	orq $0x12345, 0x10(%rcx)
	.section .t3804,"ax",@progbits
	orq $0x7, -0x8(%rbp)
	.section .t3805,"ax",@progbits
	orq $0x12345, -0x8(%rbp)
	.section .t3806,"ax",@progbits
	orq $0x7, 0x12345(%rcx)
	.section .t3807,"ax",@progbits
	orq $0x12345, 0x12345(%rcx)
	.section .t3808,"ax",@progbits
	orq $0x7, (%rax,%rsi,4)
	.section .t3809,"ax",@progbits
	orq $0x12345, (%rax,%rsi,4)
	.section .t3810,"ax",@progbits
	orq $0x7, 0x10(%rax,%rsi,8)
	.section .t3811,"ax",@progbits
	orq $0x12345, 0x10(%rax,%rsi,8)
	.section .t3812,"ax",@progbits
	orq $0x7, (,%rsi,2)
	.section .t3813,"ax",@progbits
	orq $0x12345, (,%rsi,2)
	.section .t3814,"ax",@progbits
	orq $0x7, 0x40(%rip)
	.section .t3815,"ax",@progbits
	orq $0x12345, 0x40(%rip)
	.section .t3816,"ax",@progbits
	orq $0x7, -0x100(%rip)
	.section .t3817,"ax",@progbits
	orq $0x12345, -0x100(%rip)
	.section .t3818,"ax",@progbits
	orq $0x7, 0x1234
	.section .t3819,"ax",@progbits
	orq $0x12345, 0x1234
	.section .t3820,"ax",@progbits
	orq $0x7, (%r8)
	.section .t3821,"ax",@progbits
	orq $0x12345, (%r8)
	.section .t3822,"ax",@progbits
	orq $0x7, (%r12)
	.section .t3823,"ax",@progbits
	orq $0x12345, (%r12)
	.section .t3824,"ax",@progbits
	orq $0x7, 0x8(%r13)
	.section .t3825,"ax",@progbits
	orq $0x12345, 0x8(%r13)
	.section .t3826,"ax",@progbits
	orq $0x7, (%r8,%r15,2)
	.section .t3827,"ax",@progbits
	orq $0x12345, (%r8,%r15,2)
	.section .t3828,"ax",@progbits
	orq $0x7, (%rax,%r12,4)
	.section .t3829,"ax",@progbits
	orq $0x12345, (%rax,%r12,4)
	.section .t3830,"ax",@progbits
	orq $0x7, 0x100(%rbp)
	.section .t3831,"ax",@progbits
	orq $0x12345, 0x100(%rbp)
	.section .t3832,"ax",@progbits
	orq $0x7, (%rsp)
	.section .t3833,"ax",@progbits
	orq $0x12345, (%rsp)
	.section .t3834,"ax",@progbits
	orq $0x7, 0x10(%rsp,%rsi,4)
	.section .t3835,"ax",@progbits
	orq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3836,"ax",@progbits
	orq $0x7, %gs:0x10(%rcx)
	.section .t3837,"ax",@progbits
	orq $0x12345, %gs:0x10(%rcx)
	.section .t3838,"ax",@progbits
	orq $0x7, %fs:(%rax,%rsi,8)
	.section .t3839,"ax",@progbits
	orq $0x12345, %fs:(%rax,%rsi,8)
	.section .t3840,"ax",@progbits
	andb $0x7, (%rcx)
	.section .t3841,"ax",@progbits
	andb $0x7f, (%rcx)
	.section .t3842,"ax",@progbits
	andb $0x7, 0x10(%rcx)
	.section .t3843,"ax",@progbits
	andb $0x7f, 0x10(%rcx)
	.section .t3844,"ax",@progbits
	andb $0x7, -0x8(%rbp)
	.section .t3845,"ax",@progbits
	andb $0x7f, -0x8(%rbp)
	.section .t3846,"ax",@progbits
	andb $0x7, 0x12345(%rcx)
	.section .t3847,"ax",@progbits
	andb $0x7f, 0x12345(%rcx)
	.section .t3848,"ax",@progbits
	andb $0x7, (%rax,%rsi,4)
	.section .t3849,"ax",@progbits
	andb $0x7f, (%rax,%rsi,4)
	.section .t3850,"ax",@progbits
	andb $0x7, 0x10(%rax,%rsi,8)
	.section .t3851,"ax",@progbits
	andb $0x7f, 0x10(%rax,%rsi,8)
	.section .t3852,"ax",@progbits
	andb $0x7, (,%rsi,2)
	.section .t3853,"ax",@progbits
	andb $0x7f, (,%rsi,2)
	.section .t3854,"ax",@progbits
	andb $0x7, 0x40(%rip)
	.section .t3855,"ax",@progbits
	andb $0x7f, 0x40(%rip)
	.section .t3856,"ax",@progbits
	andb $0x7, -0x100(%rip)
	.section .t3857,"ax",@progbits
	andb $0x7f, -0x100(%rip)
	.section .t3858,"ax",@progbits
	andb $0x7, 0x1234
	.section .t3859,"ax",@progbits
	andb $0x7f, 0x1234
	.section .t3860,"ax",@progbits
	andb $0x7, (%r8)
	.section .t3861,"ax",@progbits
	andb $0x7f, (%r8)
	.section .t3862,"ax",@progbits
	andb $0x7, (%r12)
	.section .t3863,"ax",@progbits
	andb $0x7f, (%r12)
	.section .t3864,"ax",@progbits
	andb $0x7, 0x8(%r13)
	.section .t3865,"ax",@progbits
	andb $0x7f, 0x8(%r13)
	.section .t3866,"ax",@progbits
	andb $0x7, (%r8,%r15,2)
	.section .t3867,"ax",@progbits
	andb $0x7f, (%r8,%r15,2)
	.section .t3868,"ax",@progbits
	andb $0x7, (%rax,%r12,4)
	.section .t3869,"ax",@progbits
	andb $0x7f, (%rax,%r12,4)
	.section .t3870,"ax",@progbits
	andb $0x7, 0x100(%rbp)
	.section .t3871,"ax",@progbits
	andb $0x7f, 0x100(%rbp)
	.section .t3872,"ax",@progbits
	andb $0x7, (%rsp)
	.section .t3873,"ax",@progbits
	andb $0x7f, (%rsp)
	.section .t3874,"ax",@progbits
	andb $0x7, 0x10(%rsp,%rsi,4)
	.section .t3875,"ax",@progbits
	andb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t3876,"ax",@progbits
	andb $0x7, %gs:0x10(%rcx)
	.section .t3877,"ax",@progbits
	andb $0x7f, %gs:0x10(%rcx)
	.section .t3878,"ax",@progbits
	andb $0x7, %fs:(%rax,%rsi,8)
	.section .t3879,"ax",@progbits
	andb $0x7f, %fs:(%rax,%rsi,8)
	.section .t3880,"ax",@progbits
	andw $0x7, (%rcx)
	.section .t3881,"ax",@progbits
	andw $0x12345, (%rcx)
	.section .t3882,"ax",@progbits
	andw $0x7, 0x10(%rcx)
	.section .t3883,"ax",@progbits
	andw $0x12345, 0x10(%rcx)
	.section .t3884,"ax",@progbits
	andw $0x7, -0x8(%rbp)
	.section .t3885,"ax",@progbits
	andw $0x12345, -0x8(%rbp)
	.section .t3886,"ax",@progbits
	andw $0x7, 0x12345(%rcx)
	.section .t3887,"ax",@progbits
	andw $0x12345, 0x12345(%rcx)
	.section .t3888,"ax",@progbits
	andw $0x7, (%rax,%rsi,4)
	.section .t3889,"ax",@progbits
	andw $0x12345, (%rax,%rsi,4)
	.section .t3890,"ax",@progbits
	andw $0x7, 0x10(%rax,%rsi,8)
	.section .t3891,"ax",@progbits
	andw $0x12345, 0x10(%rax,%rsi,8)
	.section .t3892,"ax",@progbits
	andw $0x7, (,%rsi,2)
	.section .t3893,"ax",@progbits
	andw $0x12345, (,%rsi,2)
	.section .t3894,"ax",@progbits
	andw $0x7, 0x40(%rip)
	.section .t3895,"ax",@progbits
	andw $0x12345, 0x40(%rip)
	.section .t3896,"ax",@progbits
	andw $0x7, -0x100(%rip)
	.section .t3897,"ax",@progbits
	andw $0x12345, -0x100(%rip)
	.section .t3898,"ax",@progbits
	andw $0x7, 0x1234
	.section .t3899,"ax",@progbits
	andw $0x12345, 0x1234
	.section .t3900,"ax",@progbits
	andw $0x7, (%r8)
	.section .t3901,"ax",@progbits
	andw $0x12345, (%r8)
	.section .t3902,"ax",@progbits
	andw $0x7, (%r12)
	.section .t3903,"ax",@progbits
	andw $0x12345, (%r12)
	.section .t3904,"ax",@progbits
	andw $0x7, 0x8(%r13)
	.section .t3905,"ax",@progbits
	andw $0x12345, 0x8(%r13)
	.section .t3906,"ax",@progbits
	andw $0x7, (%r8,%r15,2)
	.section .t3907,"ax",@progbits
	andw $0x12345, (%r8,%r15,2)
	.section .t3908,"ax",@progbits
	andw $0x7, (%rax,%r12,4)
	.section .t3909,"ax",@progbits
	andw $0x12345, (%rax,%r12,4)
	.section .t3910,"ax",@progbits
	andw $0x7, 0x100(%rbp)
	.section .t3911,"ax",@progbits
	andw $0x12345, 0x100(%rbp)
	.section .t3912,"ax",@progbits
	andw $0x7, (%rsp)
	.section .t3913,"ax",@progbits
	andw $0x12345, (%rsp)
	.section .t3914,"ax",@progbits
	andw $0x7, 0x10(%rsp,%rsi,4)
	.section .t3915,"ax",@progbits
	andw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3916,"ax",@progbits
	andw $0x7, %gs:0x10(%rcx)
	.section .t3917,"ax",@progbits
	andw $0x12345, %gs:0x10(%rcx)
	.section .t3918,"ax",@progbits
	andw $0x7, %fs:(%rax,%rsi,8)
	.section .t3919,"ax",@progbits
	andw $0x12345, %fs:(%rax,%rsi,8)
	.section .t3920,"ax",@progbits
	andl $0x7, (%rcx)
	.section .t3921,"ax",@progbits
	andl $0x12345, (%rcx)
	.section .t3922,"ax",@progbits
	andl $0x7, 0x10(%rcx)
	.section .t3923,"ax",@progbits
	andl $0x12345, 0x10(%rcx)
	.section .t3924,"ax",@progbits
	andl $0x7, -0x8(%rbp)
	.section .t3925,"ax",@progbits
	andl $0x12345, -0x8(%rbp)
	.section .t3926,"ax",@progbits
	andl $0x7, 0x12345(%rcx)
	.section .t3927,"ax",@progbits
	andl $0x12345, 0x12345(%rcx)
	.section .t3928,"ax",@progbits
	andl $0x7, (%rax,%rsi,4)
	.section .t3929,"ax",@progbits
	andl $0x12345, (%rax,%rsi,4)
	.section .t3930,"ax",@progbits
	andl $0x7, 0x10(%rax,%rsi,8)
	.section .t3931,"ax",@progbits
	andl $0x12345, 0x10(%rax,%rsi,8)
	.section .t3932,"ax",@progbits
	andl $0x7, (,%rsi,2)
	.section .t3933,"ax",@progbits
	andl $0x12345, (,%rsi,2)
	.section .t3934,"ax",@progbits
	andl $0x7, 0x40(%rip)
	.section .t3935,"ax",@progbits
	andl $0x12345, 0x40(%rip)
	.section .t3936,"ax",@progbits
	andl $0x7, -0x100(%rip)
	.section .t3937,"ax",@progbits
	andl $0x12345, -0x100(%rip)
	.section .t3938,"ax",@progbits
	andl $0x7, 0x1234
	.section .t3939,"ax",@progbits
	andl $0x12345, 0x1234
	.section .t3940,"ax",@progbits
	andl $0x7, (%r8)
	.section .t3941,"ax",@progbits
	andl $0x12345, (%r8)
	.section .t3942,"ax",@progbits
	andl $0x7, (%r12)
	.section .t3943,"ax",@progbits
	andl $0x12345, (%r12)
	.section .t3944,"ax",@progbits
	andl $0x7, 0x8(%r13)
	.section .t3945,"ax",@progbits
	andl $0x12345, 0x8(%r13)
	.section .t3946,"ax",@progbits
	andl $0x7, (%r8,%r15,2)
	.section .t3947,"ax",@progbits
	andl $0x12345, (%r8,%r15,2)
	.section .t3948,"ax",@progbits
	andl $0x7, (%rax,%r12,4)
	.section .t3949,"ax",@progbits
	andl $0x12345, (%rax,%r12,4)
	.section .t3950,"ax",@progbits
	andl $0x7, 0x100(%rbp)
	.section .t3951,"ax",@progbits
	andl $0x12345, 0x100(%rbp)
	.section .t3952,"ax",@progbits
	andl $0x7, (%rsp)
	.section .t3953,"ax",@progbits
	andl $0x12345, (%rsp)
	.section .t3954,"ax",@progbits
	andl $0x7, 0x10(%rsp,%rsi,4)
	.section .t3955,"ax",@progbits
	andl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3956,"ax",@progbits
	andl $0x7, %gs:0x10(%rcx)
	.section .t3957,"ax",@progbits
	andl $0x12345, %gs:0x10(%rcx)
	.section .t3958,"ax",@progbits
	andl $0x7, %fs:(%rax,%rsi,8)
	.section .t3959,"ax",@progbits
	andl $0x12345, %fs:(%rax,%rsi,8)
	.section .t3960,"ax",@progbits
	andq $0x7, (%rcx)
	.section .t3961,"ax",@progbits
	andq $0x12345, (%rcx)
	.section .t3962,"ax",@progbits
	andq $0x7, 0x10(%rcx)
	.section .t3963,"ax",@progbits
	andq $0x12345, 0x10(%rcx)
	.section .t3964,"ax",@progbits
	andq $0x7, -0x8(%rbp)
	.section .t3965,"ax",@progbits
	andq $0x12345, -0x8(%rbp)
	.section .t3966,"ax",@progbits
	andq $0x7, 0x12345(%rcx)
	.section .t3967,"ax",@progbits
	andq $0x12345, 0x12345(%rcx)
	.section .t3968,"ax",@progbits
	andq $0x7, (%rax,%rsi,4)
	.section .t3969,"ax",@progbits
	andq $0x12345, (%rax,%rsi,4)
	.section .t3970,"ax",@progbits
	andq $0x7, 0x10(%rax,%rsi,8)
	.section .t3971,"ax",@progbits
	andq $0x12345, 0x10(%rax,%rsi,8)
	.section .t3972,"ax",@progbits
	andq $0x7, (,%rsi,2)
	.section .t3973,"ax",@progbits
	andq $0x12345, (,%rsi,2)
	.section .t3974,"ax",@progbits
	andq $0x7, 0x40(%rip)
	.section .t3975,"ax",@progbits
	andq $0x12345, 0x40(%rip)
	.section .t3976,"ax",@progbits
	andq $0x7, -0x100(%rip)
	.section .t3977,"ax",@progbits
	andq $0x12345, -0x100(%rip)
	.section .t3978,"ax",@progbits
	andq $0x7, 0x1234
	.section .t3979,"ax",@progbits
	andq $0x12345, 0x1234
	.section .t3980,"ax",@progbits
	andq $0x7, (%r8)
	.section .t3981,"ax",@progbits
	andq $0x12345, (%r8)
	.section .t3982,"ax",@progbits
	andq $0x7, (%r12)
	.section .t3983,"ax",@progbits
	andq $0x12345, (%r12)
	.section .t3984,"ax",@progbits
	andq $0x7, 0x8(%r13)
	.section .t3985,"ax",@progbits
	andq $0x12345, 0x8(%r13)
	.section .t3986,"ax",@progbits
	andq $0x7, (%r8,%r15,2)
	.section .t3987,"ax",@progbits
	andq $0x12345, (%r8,%r15,2)
	.section .t3988,"ax",@progbits
	andq $0x7, (%rax,%r12,4)
	.section .t3989,"ax",@progbits
	andq $0x12345, (%rax,%r12,4)
	.section .t3990,"ax",@progbits
	andq $0x7, 0x100(%rbp)
	.section .t3991,"ax",@progbits
	andq $0x12345, 0x100(%rbp)
	.section .t3992,"ax",@progbits
	andq $0x7, (%rsp)
	.section .t3993,"ax",@progbits
	andq $0x12345, (%rsp)
	.section .t3994,"ax",@progbits
	andq $0x7, 0x10(%rsp,%rsi,4)
	.section .t3995,"ax",@progbits
	andq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t3996,"ax",@progbits
	andq $0x7, %gs:0x10(%rcx)
	.section .t3997,"ax",@progbits
	andq $0x12345, %gs:0x10(%rcx)
	.section .t3998,"ax",@progbits
	andq $0x7, %fs:(%rax,%rsi,8)
	.section .t3999,"ax",@progbits
	andq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4000,"ax",@progbits
	subb $0x7, (%rcx)
	.section .t4001,"ax",@progbits
	subb $0x7f, (%rcx)
	.section .t4002,"ax",@progbits
	subb $0x7, 0x10(%rcx)
	.section .t4003,"ax",@progbits
	subb $0x7f, 0x10(%rcx)
	.section .t4004,"ax",@progbits
	subb $0x7, -0x8(%rbp)
	.section .t4005,"ax",@progbits
	subb $0x7f, -0x8(%rbp)
	.section .t4006,"ax",@progbits
	subb $0x7, 0x12345(%rcx)
	.section .t4007,"ax",@progbits
	subb $0x7f, 0x12345(%rcx)
	.section .t4008,"ax",@progbits
	subb $0x7, (%rax,%rsi,4)
	.section .t4009,"ax",@progbits
	subb $0x7f, (%rax,%rsi,4)
	.section .t4010,"ax",@progbits
	subb $0x7, 0x10(%rax,%rsi,8)
	.section .t4011,"ax",@progbits
	subb $0x7f, 0x10(%rax,%rsi,8)
	.section .t4012,"ax",@progbits
	subb $0x7, (,%rsi,2)
	.section .t4013,"ax",@progbits
	subb $0x7f, (,%rsi,2)
	.section .t4014,"ax",@progbits
	subb $0x7, 0x40(%rip)
	.section .t4015,"ax",@progbits
	subb $0x7f, 0x40(%rip)
	.section .t4016,"ax",@progbits
	subb $0x7, -0x100(%rip)
	.section .t4017,"ax",@progbits
	subb $0x7f, -0x100(%rip)
	.section .t4018,"ax",@progbits
	subb $0x7, 0x1234
	.section .t4019,"ax",@progbits
	subb $0x7f, 0x1234
	.section .t4020,"ax",@progbits
	subb $0x7, (%r8)
	.section .t4021,"ax",@progbits
	subb $0x7f, (%r8)
	.section .t4022,"ax",@progbits
	subb $0x7, (%r12)
	.section .t4023,"ax",@progbits
	subb $0x7f, (%r12)
	.section .t4024,"ax",@progbits
	subb $0x7, 0x8(%r13)
	.section .t4025,"ax",@progbits
	subb $0x7f, 0x8(%r13)
	.section .t4026,"ax",@progbits
	subb $0x7, (%r8,%r15,2)
	.section .t4027,"ax",@progbits
	subb $0x7f, (%r8,%r15,2)
	.section .t4028,"ax",@progbits
	subb $0x7, (%rax,%r12,4)
	.section .t4029,"ax",@progbits
	subb $0x7f, (%rax,%r12,4)
	.section .t4030,"ax",@progbits
	subb $0x7, 0x100(%rbp)
	.section .t4031,"ax",@progbits
	subb $0x7f, 0x100(%rbp)
	.section .t4032,"ax",@progbits
	subb $0x7, (%rsp)
	.section .t4033,"ax",@progbits
	subb $0x7f, (%rsp)
	.section .t4034,"ax",@progbits
	subb $0x7, 0x10(%rsp,%rsi,4)
	.section .t4035,"ax",@progbits
	subb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t4036,"ax",@progbits
	subb $0x7, %gs:0x10(%rcx)
	.section .t4037,"ax",@progbits
	subb $0x7f, %gs:0x10(%rcx)
	.section .t4038,"ax",@progbits
	subb $0x7, %fs:(%rax,%rsi,8)
	.section .t4039,"ax",@progbits
	subb $0x7f, %fs:(%rax,%rsi,8)
	.section .t4040,"ax",@progbits
	subw $0x7, (%rcx)
	.section .t4041,"ax",@progbits
	subw $0x12345, (%rcx)
	.section .t4042,"ax",@progbits
	subw $0x7, 0x10(%rcx)
	.section .t4043,"ax",@progbits
	subw $0x12345, 0x10(%rcx)
	.section .t4044,"ax",@progbits
	subw $0x7, -0x8(%rbp)
	.section .t4045,"ax",@progbits
	subw $0x12345, -0x8(%rbp)
	.section .t4046,"ax",@progbits
	subw $0x7, 0x12345(%rcx)
	.section .t4047,"ax",@progbits
	subw $0x12345, 0x12345(%rcx)
	.section .t4048,"ax",@progbits
	subw $0x7, (%rax,%rsi,4)
	.section .t4049,"ax",@progbits
	subw $0x12345, (%rax,%rsi,4)
	.section .t4050,"ax",@progbits
	subw $0x7, 0x10(%rax,%rsi,8)
	.section .t4051,"ax",@progbits
	subw $0x12345, 0x10(%rax,%rsi,8)
	.section .t4052,"ax",@progbits
	subw $0x7, (,%rsi,2)
	.section .t4053,"ax",@progbits
	subw $0x12345, (,%rsi,2)
	.section .t4054,"ax",@progbits
	subw $0x7, 0x40(%rip)
	.section .t4055,"ax",@progbits
	subw $0x12345, 0x40(%rip)
	.section .t4056,"ax",@progbits
	subw $0x7, -0x100(%rip)
	.section .t4057,"ax",@progbits
	subw $0x12345, -0x100(%rip)
	.section .t4058,"ax",@progbits
	subw $0x7, 0x1234
	.section .t4059,"ax",@progbits
	subw $0x12345, 0x1234
	.section .t4060,"ax",@progbits
	subw $0x7, (%r8)
	.section .t4061,"ax",@progbits
	subw $0x12345, (%r8)
	.section .t4062,"ax",@progbits
	subw $0x7, (%r12)
	.section .t4063,"ax",@progbits
	subw $0x12345, (%r12)
	.section .t4064,"ax",@progbits
	subw $0x7, 0x8(%r13)
	.section .t4065,"ax",@progbits
	subw $0x12345, 0x8(%r13)
	.section .t4066,"ax",@progbits
	subw $0x7, (%r8,%r15,2)
	.section .t4067,"ax",@progbits
	subw $0x12345, (%r8,%r15,2)
	.section .t4068,"ax",@progbits
	subw $0x7, (%rax,%r12,4)
	.section .t4069,"ax",@progbits
	subw $0x12345, (%rax,%r12,4)
	.section .t4070,"ax",@progbits
	subw $0x7, 0x100(%rbp)
	.section .t4071,"ax",@progbits
	subw $0x12345, 0x100(%rbp)
	.section .t4072,"ax",@progbits
	subw $0x7, (%rsp)
	.section .t4073,"ax",@progbits
	subw $0x12345, (%rsp)
	.section .t4074,"ax",@progbits
	subw $0x7, 0x10(%rsp,%rsi,4)
	.section .t4075,"ax",@progbits
	subw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4076,"ax",@progbits
	subw $0x7, %gs:0x10(%rcx)
	.section .t4077,"ax",@progbits
	subw $0x12345, %gs:0x10(%rcx)
	.section .t4078,"ax",@progbits
	subw $0x7, %fs:(%rax,%rsi,8)
	.section .t4079,"ax",@progbits
	subw $0x12345, %fs:(%rax,%rsi,8)
	.section .t4080,"ax",@progbits
	subl $0x7, (%rcx)
	.section .t4081,"ax",@progbits
	subl $0x12345, (%rcx)
	.section .t4082,"ax",@progbits
	subl $0x7, 0x10(%rcx)
	.section .t4083,"ax",@progbits
	subl $0x12345, 0x10(%rcx)
	.section .t4084,"ax",@progbits
	subl $0x7, -0x8(%rbp)
	.section .t4085,"ax",@progbits
	subl $0x12345, -0x8(%rbp)
	.section .t4086,"ax",@progbits
	subl $0x7, 0x12345(%rcx)
	.section .t4087,"ax",@progbits
	subl $0x12345, 0x12345(%rcx)
	.section .t4088,"ax",@progbits
	subl $0x7, (%rax,%rsi,4)
	.section .t4089,"ax",@progbits
	subl $0x12345, (%rax,%rsi,4)
	.section .t4090,"ax",@progbits
	subl $0x7, 0x10(%rax,%rsi,8)
	.section .t4091,"ax",@progbits
	subl $0x12345, 0x10(%rax,%rsi,8)
	.section .t4092,"ax",@progbits
	subl $0x7, (,%rsi,2)
	.section .t4093,"ax",@progbits
	subl $0x12345, (,%rsi,2)
	.section .t4094,"ax",@progbits
	subl $0x7, 0x40(%rip)
	.section .t4095,"ax",@progbits
	subl $0x12345, 0x40(%rip)
	.section .t4096,"ax",@progbits
	subl $0x7, -0x100(%rip)
	.section .t4097,"ax",@progbits
	subl $0x12345, -0x100(%rip)
	.section .t4098,"ax",@progbits
	subl $0x7, 0x1234
	.section .t4099,"ax",@progbits
	subl $0x12345, 0x1234
	.section .t4100,"ax",@progbits
	subl $0x7, (%r8)
	.section .t4101,"ax",@progbits
	subl $0x12345, (%r8)
	.section .t4102,"ax",@progbits
	subl $0x7, (%r12)
	.section .t4103,"ax",@progbits
	subl $0x12345, (%r12)
	.section .t4104,"ax",@progbits
	subl $0x7, 0x8(%r13)
	.section .t4105,"ax",@progbits
	subl $0x12345, 0x8(%r13)
	.section .t4106,"ax",@progbits
	subl $0x7, (%r8,%r15,2)
	.section .t4107,"ax",@progbits
	subl $0x12345, (%r8,%r15,2)
	.section .t4108,"ax",@progbits
	subl $0x7, (%rax,%r12,4)
	.section .t4109,"ax",@progbits
	subl $0x12345, (%rax,%r12,4)
	.section .t4110,"ax",@progbits
	subl $0x7, 0x100(%rbp)
	.section .t4111,"ax",@progbits
	subl $0x12345, 0x100(%rbp)
	.section .t4112,"ax",@progbits
	subl $0x7, (%rsp)
	.section .t4113,"ax",@progbits
	subl $0x12345, (%rsp)
	.section .t4114,"ax",@progbits
	subl $0x7, 0x10(%rsp,%rsi,4)
	.section .t4115,"ax",@progbits
	subl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4116,"ax",@progbits
	subl $0x7, %gs:0x10(%rcx)
	.section .t4117,"ax",@progbits
	subl $0x12345, %gs:0x10(%rcx)
	.section .t4118,"ax",@progbits
	subl $0x7, %fs:(%rax,%rsi,8)
	.section .t4119,"ax",@progbits
	subl $0x12345, %fs:(%rax,%rsi,8)
	.section .t4120,"ax",@progbits
	subq $0x7, (%rcx)
	.section .t4121,"ax",@progbits
	subq $0x12345, (%rcx)
	.section .t4122,"ax",@progbits
	subq $0x7, 0x10(%rcx)
	.section .t4123,"ax",@progbits
	subq $0x12345, 0x10(%rcx)
	.section .t4124,"ax",@progbits
	subq $0x7, -0x8(%rbp)
	.section .t4125,"ax",@progbits
	subq $0x12345, -0x8(%rbp)
	.section .t4126,"ax",@progbits
	subq $0x7, 0x12345(%rcx)
	.section .t4127,"ax",@progbits
	subq $0x12345, 0x12345(%rcx)
	.section .t4128,"ax",@progbits
	subq $0x7, (%rax,%rsi,4)
	.section .t4129,"ax",@progbits
	subq $0x12345, (%rax,%rsi,4)
	.section .t4130,"ax",@progbits
	subq $0x7, 0x10(%rax,%rsi,8)
	.section .t4131,"ax",@progbits
	subq $0x12345, 0x10(%rax,%rsi,8)
	.section .t4132,"ax",@progbits
	subq $0x7, (,%rsi,2)
	.section .t4133,"ax",@progbits
	subq $0x12345, (,%rsi,2)
	.section .t4134,"ax",@progbits
	subq $0x7, 0x40(%rip)
	.section .t4135,"ax",@progbits
	subq $0x12345, 0x40(%rip)
	.section .t4136,"ax",@progbits
	subq $0x7, -0x100(%rip)
	.section .t4137,"ax",@progbits
	subq $0x12345, -0x100(%rip)
	.section .t4138,"ax",@progbits
	subq $0x7, 0x1234
	.section .t4139,"ax",@progbits
	subq $0x12345, 0x1234
	.section .t4140,"ax",@progbits
	subq $0x7, (%r8)
	.section .t4141,"ax",@progbits
	subq $0x12345, (%r8)
	.section .t4142,"ax",@progbits
	subq $0x7, (%r12)
	.section .t4143,"ax",@progbits
	subq $0x12345, (%r12)
	.section .t4144,"ax",@progbits
	subq $0x7, 0x8(%r13)
	.section .t4145,"ax",@progbits
	subq $0x12345, 0x8(%r13)
	.section .t4146,"ax",@progbits
	subq $0x7, (%r8,%r15,2)
	.section .t4147,"ax",@progbits
	subq $0x12345, (%r8,%r15,2)
	.section .t4148,"ax",@progbits
	subq $0x7, (%rax,%r12,4)
	.section .t4149,"ax",@progbits
	subq $0x12345, (%rax,%r12,4)
	.section .t4150,"ax",@progbits
	subq $0x7, 0x100(%rbp)
	.section .t4151,"ax",@progbits
	subq $0x12345, 0x100(%rbp)
	.section .t4152,"ax",@progbits
	subq $0x7, (%rsp)
	.section .t4153,"ax",@progbits
	subq $0x12345, (%rsp)
	.section .t4154,"ax",@progbits
	subq $0x7, 0x10(%rsp,%rsi,4)
	.section .t4155,"ax",@progbits
	subq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4156,"ax",@progbits
	subq $0x7, %gs:0x10(%rcx)
	.section .t4157,"ax",@progbits
	subq $0x12345, %gs:0x10(%rcx)
	.section .t4158,"ax",@progbits
	subq $0x7, %fs:(%rax,%rsi,8)
	.section .t4159,"ax",@progbits
	subq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4160,"ax",@progbits
	xorb $0x7, (%rcx)
	.section .t4161,"ax",@progbits
	xorb $0x7f, (%rcx)
	.section .t4162,"ax",@progbits
	xorb $0x7, 0x10(%rcx)
	.section .t4163,"ax",@progbits
	xorb $0x7f, 0x10(%rcx)
	.section .t4164,"ax",@progbits
	xorb $0x7, -0x8(%rbp)
	.section .t4165,"ax",@progbits
	xorb $0x7f, -0x8(%rbp)
	.section .t4166,"ax",@progbits
	xorb $0x7, 0x12345(%rcx)
	.section .t4167,"ax",@progbits
	xorb $0x7f, 0x12345(%rcx)
	.section .t4168,"ax",@progbits
	xorb $0x7, (%rax,%rsi,4)
	.section .t4169,"ax",@progbits
	xorb $0x7f, (%rax,%rsi,4)
	.section .t4170,"ax",@progbits
	xorb $0x7, 0x10(%rax,%rsi,8)
	.section .t4171,"ax",@progbits
	xorb $0x7f, 0x10(%rax,%rsi,8)
	.section .t4172,"ax",@progbits
	xorb $0x7, (,%rsi,2)
	.section .t4173,"ax",@progbits
	xorb $0x7f, (,%rsi,2)
	.section .t4174,"ax",@progbits
	xorb $0x7, 0x40(%rip)
	.section .t4175,"ax",@progbits
	xorb $0x7f, 0x40(%rip)
	.section .t4176,"ax",@progbits
	xorb $0x7, -0x100(%rip)
	.section .t4177,"ax",@progbits
	xorb $0x7f, -0x100(%rip)
	.section .t4178,"ax",@progbits
	xorb $0x7, 0x1234
	.section .t4179,"ax",@progbits
	xorb $0x7f, 0x1234
	.section .t4180,"ax",@progbits
	xorb $0x7, (%r8)
	.section .t4181,"ax",@progbits
	xorb $0x7f, (%r8)
	.section .t4182,"ax",@progbits
	xorb $0x7, (%r12)
	.section .t4183,"ax",@progbits
	xorb $0x7f, (%r12)
	.section .t4184,"ax",@progbits
	xorb $0x7, 0x8(%r13)
	.section .t4185,"ax",@progbits
	xorb $0x7f, 0x8(%r13)
	.section .t4186,"ax",@progbits
	xorb $0x7, (%r8,%r15,2)
	.section .t4187,"ax",@progbits
	xorb $0x7f, (%r8,%r15,2)
	.section .t4188,"ax",@progbits
	xorb $0x7, (%rax,%r12,4)
	.section .t4189,"ax",@progbits
	xorb $0x7f, (%rax,%r12,4)
	.section .t4190,"ax",@progbits
	xorb $0x7, 0x100(%rbp)
	.section .t4191,"ax",@progbits
	xorb $0x7f, 0x100(%rbp)
	.section .t4192,"ax",@progbits
	xorb $0x7, (%rsp)
	.section .t4193,"ax",@progbits
	xorb $0x7f, (%rsp)
	.section .t4194,"ax",@progbits
	xorb $0x7, 0x10(%rsp,%rsi,4)
	.section .t4195,"ax",@progbits
	xorb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t4196,"ax",@progbits
	xorb $0x7, %gs:0x10(%rcx)
	.section .t4197,"ax",@progbits
	xorb $0x7f, %gs:0x10(%rcx)
	.section .t4198,"ax",@progbits
	xorb $0x7, %fs:(%rax,%rsi,8)
	.section .t4199,"ax",@progbits
	xorb $0x7f, %fs:(%rax,%rsi,8)
	.section .t4200,"ax",@progbits
	xorw $0x7, (%rcx)
	.section .t4201,"ax",@progbits
	xorw $0x12345, (%rcx)
	.section .t4202,"ax",@progbits
	xorw $0x7, 0x10(%rcx)
	.section .t4203,"ax",@progbits
	xorw $0x12345, 0x10(%rcx)
	.section .t4204,"ax",@progbits
	xorw $0x7, -0x8(%rbp)
	.section .t4205,"ax",@progbits
	xorw $0x12345, -0x8(%rbp)
	.section .t4206,"ax",@progbits
	xorw $0x7, 0x12345(%rcx)
	.section .t4207,"ax",@progbits
	xorw $0x12345, 0x12345(%rcx)
	.section .t4208,"ax",@progbits
	xorw $0x7, (%rax,%rsi,4)
	.section .t4209,"ax",@progbits
	xorw $0x12345, (%rax,%rsi,4)
	.section .t4210,"ax",@progbits
	xorw $0x7, 0x10(%rax,%rsi,8)
	.section .t4211,"ax",@progbits
	xorw $0x12345, 0x10(%rax,%rsi,8)
	.section .t4212,"ax",@progbits
	xorw $0x7, (,%rsi,2)
	.section .t4213,"ax",@progbits
	xorw $0x12345, (,%rsi,2)
	.section .t4214,"ax",@progbits
	xorw $0x7, 0x40(%rip)
	.section .t4215,"ax",@progbits
	xorw $0x12345, 0x40(%rip)
	.section .t4216,"ax",@progbits
	xorw $0x7, -0x100(%rip)
	.section .t4217,"ax",@progbits
	xorw $0x12345, -0x100(%rip)
	.section .t4218,"ax",@progbits
	xorw $0x7, 0x1234
	.section .t4219,"ax",@progbits
	xorw $0x12345, 0x1234
	.section .t4220,"ax",@progbits
	xorw $0x7, (%r8)
	.section .t4221,"ax",@progbits
	xorw $0x12345, (%r8)
	.section .t4222,"ax",@progbits
	xorw $0x7, (%r12)
	.section .t4223,"ax",@progbits
	xorw $0x12345, (%r12)
	.section .t4224,"ax",@progbits
	xorw $0x7, 0x8(%r13)
	.section .t4225,"ax",@progbits
	xorw $0x12345, 0x8(%r13)
	.section .t4226,"ax",@progbits
	xorw $0x7, (%r8,%r15,2)
	.section .t4227,"ax",@progbits
	xorw $0x12345, (%r8,%r15,2)
	.section .t4228,"ax",@progbits
	xorw $0x7, (%rax,%r12,4)
	.section .t4229,"ax",@progbits
	xorw $0x12345, (%rax,%r12,4)
	.section .t4230,"ax",@progbits
	xorw $0x7, 0x100(%rbp)
	.section .t4231,"ax",@progbits
	xorw $0x12345, 0x100(%rbp)
	.section .t4232,"ax",@progbits
	xorw $0x7, (%rsp)
	.section .t4233,"ax",@progbits
	xorw $0x12345, (%rsp)
	.section .t4234,"ax",@progbits
	xorw $0x7, 0x10(%rsp,%rsi,4)
	.section .t4235,"ax",@progbits
	xorw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4236,"ax",@progbits
	xorw $0x7, %gs:0x10(%rcx)
	.section .t4237,"ax",@progbits
	xorw $0x12345, %gs:0x10(%rcx)
	.section .t4238,"ax",@progbits
	xorw $0x7, %fs:(%rax,%rsi,8)
	.section .t4239,"ax",@progbits
	xorw $0x12345, %fs:(%rax,%rsi,8)
	.section .t4240,"ax",@progbits
	xorl $0x7, (%rcx)
	.section .t4241,"ax",@progbits
	xorl $0x12345, (%rcx)
	.section .t4242,"ax",@progbits
	xorl $0x7, 0x10(%rcx)
	.section .t4243,"ax",@progbits
	xorl $0x12345, 0x10(%rcx)
	.section .t4244,"ax",@progbits
	xorl $0x7, -0x8(%rbp)
	.section .t4245,"ax",@progbits
	xorl $0x12345, -0x8(%rbp)
	.section .t4246,"ax",@progbits
	xorl $0x7, 0x12345(%rcx)
	.section .t4247,"ax",@progbits
	xorl $0x12345, 0x12345(%rcx)
	.section .t4248,"ax",@progbits
	xorl $0x7, (%rax,%rsi,4)
	.section .t4249,"ax",@progbits
	xorl $0x12345, (%rax,%rsi,4)
	.section .t4250,"ax",@progbits
	xorl $0x7, 0x10(%rax,%rsi,8)
	.section .t4251,"ax",@progbits
	xorl $0x12345, 0x10(%rax,%rsi,8)
	.section .t4252,"ax",@progbits
	xorl $0x7, (,%rsi,2)
	.section .t4253,"ax",@progbits
	xorl $0x12345, (,%rsi,2)
	.section .t4254,"ax",@progbits
	xorl $0x7, 0x40(%rip)
	.section .t4255,"ax",@progbits
	xorl $0x12345, 0x40(%rip)
	.section .t4256,"ax",@progbits
	xorl $0x7, -0x100(%rip)
	.section .t4257,"ax",@progbits
	xorl $0x12345, -0x100(%rip)
	.section .t4258,"ax",@progbits
	xorl $0x7, 0x1234
	.section .t4259,"ax",@progbits
	xorl $0x12345, 0x1234
	.section .t4260,"ax",@progbits
	xorl $0x7, (%r8)
	.section .t4261,"ax",@progbits
	xorl $0x12345, (%r8)
	.section .t4262,"ax",@progbits
	xorl $0x7, (%r12)
	.section .t4263,"ax",@progbits
	xorl $0x12345, (%r12)
	.section .t4264,"ax",@progbits
	xorl $0x7, 0x8(%r13)
	.section .t4265,"ax",@progbits
	xorl $0x12345, 0x8(%r13)
	.section .t4266,"ax",@progbits
	xorl $0x7, (%r8,%r15,2)
	.section .t4267,"ax",@progbits
	xorl $0x12345, (%r8,%r15,2)
	.section .t4268,"ax",@progbits
	xorl $0x7, (%rax,%r12,4)
	.section .t4269,"ax",@progbits
	xorl $0x12345, (%rax,%r12,4)
	.section .t4270,"ax",@progbits
	xorl $0x7, 0x100(%rbp)
	.section .t4271,"ax",@progbits
	xorl $0x12345, 0x100(%rbp)
	.section .t4272,"ax",@progbits
	xorl $0x7, (%rsp)
	.section .t4273,"ax",@progbits
	xorl $0x12345, (%rsp)
	.section .t4274,"ax",@progbits
	xorl $0x7, 0x10(%rsp,%rsi,4)
	.section .t4275,"ax",@progbits
	xorl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4276,"ax",@progbits
	xorl $0x7, %gs:0x10(%rcx)
	.section .t4277,"ax",@progbits
	xorl $0x12345, %gs:0x10(%rcx)
	.section .t4278,"ax",@progbits
	xorl $0x7, %fs:(%rax,%rsi,8)
	.section .t4279,"ax",@progbits
	xorl $0x12345, %fs:(%rax,%rsi,8)
	.section .t4280,"ax",@progbits
	xorq $0x7, (%rcx)
	.section .t4281,"ax",@progbits
	xorq $0x12345, (%rcx)
	.section .t4282,"ax",@progbits
	xorq $0x7, 0x10(%rcx)
	.section .t4283,"ax",@progbits
	xorq $0x12345, 0x10(%rcx)
	.section .t4284,"ax",@progbits
	xorq $0x7, -0x8(%rbp)
	.section .t4285,"ax",@progbits
	xorq $0x12345, -0x8(%rbp)
	.section .t4286,"ax",@progbits
	xorq $0x7, 0x12345(%rcx)
	.section .t4287,"ax",@progbits
	xorq $0x12345, 0x12345(%rcx)
	.section .t4288,"ax",@progbits
	xorq $0x7, (%rax,%rsi,4)
	.section .t4289,"ax",@progbits
	xorq $0x12345, (%rax,%rsi,4)
	.section .t4290,"ax",@progbits
	xorq $0x7, 0x10(%rax,%rsi,8)
	.section .t4291,"ax",@progbits
	xorq $0x12345, 0x10(%rax,%rsi,8)
	.section .t4292,"ax",@progbits
	xorq $0x7, (,%rsi,2)
	.section .t4293,"ax",@progbits
	xorq $0x12345, (,%rsi,2)
	.section .t4294,"ax",@progbits
	xorq $0x7, 0x40(%rip)
	.section .t4295,"ax",@progbits
	xorq $0x12345, 0x40(%rip)
	.section .t4296,"ax",@progbits
	xorq $0x7, -0x100(%rip)
	.section .t4297,"ax",@progbits
	xorq $0x12345, -0x100(%rip)
	.section .t4298,"ax",@progbits
	xorq $0x7, 0x1234
	.section .t4299,"ax",@progbits
	xorq $0x12345, 0x1234
	.section .t4300,"ax",@progbits
	xorq $0x7, (%r8)
	.section .t4301,"ax",@progbits
	xorq $0x12345, (%r8)
	.section .t4302,"ax",@progbits
	xorq $0x7, (%r12)
	.section .t4303,"ax",@progbits
	xorq $0x12345, (%r12)
	.section .t4304,"ax",@progbits
	xorq $0x7, 0x8(%r13)
	.section .t4305,"ax",@progbits
	xorq $0x12345, 0x8(%r13)
	.section .t4306,"ax",@progbits
	xorq $0x7, (%r8,%r15,2)
	.section .t4307,"ax",@progbits
	xorq $0x12345, (%r8,%r15,2)
	.section .t4308,"ax",@progbits
	xorq $0x7, (%rax,%r12,4)
	.section .t4309,"ax",@progbits
	xorq $0x12345, (%rax,%r12,4)
	.section .t4310,"ax",@progbits
	xorq $0x7, 0x100(%rbp)
	.section .t4311,"ax",@progbits
	xorq $0x12345, 0x100(%rbp)
	.section .t4312,"ax",@progbits
	xorq $0x7, (%rsp)
	.section .t4313,"ax",@progbits
	xorq $0x12345, (%rsp)
	.section .t4314,"ax",@progbits
	xorq $0x7, 0x10(%rsp,%rsi,4)
	.section .t4315,"ax",@progbits
	xorq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4316,"ax",@progbits
	xorq $0x7, %gs:0x10(%rcx)
	.section .t4317,"ax",@progbits
	xorq $0x12345, %gs:0x10(%rcx)
	.section .t4318,"ax",@progbits
	xorq $0x7, %fs:(%rax,%rsi,8)
	.section .t4319,"ax",@progbits
	xorq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4320,"ax",@progbits
	cmpb $0x7, (%rcx)
	.section .t4321,"ax",@progbits
	cmpb $0x7f, (%rcx)
	.section .t4322,"ax",@progbits
	cmpb $0x7, 0x10(%rcx)
	.section .t4323,"ax",@progbits
	cmpb $0x7f, 0x10(%rcx)
	.section .t4324,"ax",@progbits
	cmpb $0x7, -0x8(%rbp)
	.section .t4325,"ax",@progbits
	cmpb $0x7f, -0x8(%rbp)
	.section .t4326,"ax",@progbits
	cmpb $0x7, 0x12345(%rcx)
	.section .t4327,"ax",@progbits
	cmpb $0x7f, 0x12345(%rcx)
	.section .t4328,"ax",@progbits
	cmpb $0x7, (%rax,%rsi,4)
	.section .t4329,"ax",@progbits
	cmpb $0x7f, (%rax,%rsi,4)
	.section .t4330,"ax",@progbits
	cmpb $0x7, 0x10(%rax,%rsi,8)
	.section .t4331,"ax",@progbits
	cmpb $0x7f, 0x10(%rax,%rsi,8)
	.section .t4332,"ax",@progbits
	cmpb $0x7, (,%rsi,2)
	.section .t4333,"ax",@progbits
	cmpb $0x7f, (,%rsi,2)
	.section .t4334,"ax",@progbits
	cmpb $0x7, 0x40(%rip)
	.section .t4335,"ax",@progbits
	cmpb $0x7f, 0x40(%rip)
	.section .t4336,"ax",@progbits
	cmpb $0x7, -0x100(%rip)
	.section .t4337,"ax",@progbits
	cmpb $0x7f, -0x100(%rip)
	.section .t4338,"ax",@progbits
	cmpb $0x7, 0x1234
	.section .t4339,"ax",@progbits
	cmpb $0x7f, 0x1234
	.section .t4340,"ax",@progbits
	cmpb $0x7, (%r8)
	.section .t4341,"ax",@progbits
	cmpb $0x7f, (%r8)
	.section .t4342,"ax",@progbits
	cmpb $0x7, (%r12)
	.section .t4343,"ax",@progbits
	cmpb $0x7f, (%r12)
	.section .t4344,"ax",@progbits
	cmpb $0x7, 0x8(%r13)
	.section .t4345,"ax",@progbits
	cmpb $0x7f, 0x8(%r13)
	.section .t4346,"ax",@progbits
	cmpb $0x7, (%r8,%r15,2)
	.section .t4347,"ax",@progbits
	cmpb $0x7f, (%r8,%r15,2)
	.section .t4348,"ax",@progbits
	cmpb $0x7, (%rax,%r12,4)
	.section .t4349,"ax",@progbits
	cmpb $0x7f, (%rax,%r12,4)
	.section .t4350,"ax",@progbits
	cmpb $0x7, 0x100(%rbp)
	.section .t4351,"ax",@progbits
	cmpb $0x7f, 0x100(%rbp)
	.section .t4352,"ax",@progbits
	cmpb $0x7, (%rsp)
	.section .t4353,"ax",@progbits
	cmpb $0x7f, (%rsp)
	.section .t4354,"ax",@progbits
	cmpb $0x7, 0x10(%rsp,%rsi,4)
	.section .t4355,"ax",@progbits
	cmpb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t4356,"ax",@progbits
	cmpb $0x7, %gs:0x10(%rcx)
	.section .t4357,"ax",@progbits
	cmpb $0x7f, %gs:0x10(%rcx)
	.section .t4358,"ax",@progbits
	cmpb $0x7, %fs:(%rax,%rsi,8)
	.section .t4359,"ax",@progbits
	cmpb $0x7f, %fs:(%rax,%rsi,8)
	.section .t4360,"ax",@progbits
	cmpw $0x7, (%rcx)
	.section .t4361,"ax",@progbits
	cmpw $0x12345, (%rcx)
	.section .t4362,"ax",@progbits
	cmpw $0x7, 0x10(%rcx)
	.section .t4363,"ax",@progbits
	cmpw $0x12345, 0x10(%rcx)
	.section .t4364,"ax",@progbits
	cmpw $0x7, -0x8(%rbp)
	.section .t4365,"ax",@progbits
	cmpw $0x12345, -0x8(%rbp)
	.section .t4366,"ax",@progbits
	cmpw $0x7, 0x12345(%rcx)
	.section .t4367,"ax",@progbits
	cmpw $0x12345, 0x12345(%rcx)
	.section .t4368,"ax",@progbits
	cmpw $0x7, (%rax,%rsi,4)
	.section .t4369,"ax",@progbits
	cmpw $0x12345, (%rax,%rsi,4)
	.section .t4370,"ax",@progbits
	cmpw $0x7, 0x10(%rax,%rsi,8)
	.section .t4371,"ax",@progbits
	cmpw $0x12345, 0x10(%rax,%rsi,8)
	.section .t4372,"ax",@progbits
	cmpw $0x7, (,%rsi,2)
	.section .t4373,"ax",@progbits
	cmpw $0x12345, (,%rsi,2)
	.section .t4374,"ax",@progbits
	cmpw $0x7, 0x40(%rip)
	.section .t4375,"ax",@progbits
	cmpw $0x12345, 0x40(%rip)
	.section .t4376,"ax",@progbits
	cmpw $0x7, -0x100(%rip)
	.section .t4377,"ax",@progbits
	cmpw $0x12345, -0x100(%rip)
	.section .t4378,"ax",@progbits
	cmpw $0x7, 0x1234
	.section .t4379,"ax",@progbits
	cmpw $0x12345, 0x1234
	.section .t4380,"ax",@progbits
	cmpw $0x7, (%r8)
	.section .t4381,"ax",@progbits
	cmpw $0x12345, (%r8)
	.section .t4382,"ax",@progbits
	cmpw $0x7, (%r12)
	.section .t4383,"ax",@progbits
	cmpw $0x12345, (%r12)
	.section .t4384,"ax",@progbits
	cmpw $0x7, 0x8(%r13)
	.section .t4385,"ax",@progbits
	cmpw $0x12345, 0x8(%r13)
	.section .t4386,"ax",@progbits
	cmpw $0x7, (%r8,%r15,2)
	.section .t4387,"ax",@progbits
	cmpw $0x12345, (%r8,%r15,2)
	.section .t4388,"ax",@progbits
	cmpw $0x7, (%rax,%r12,4)
	.section .t4389,"ax",@progbits
	cmpw $0x12345, (%rax,%r12,4)
	.section .t4390,"ax",@progbits
	cmpw $0x7, 0x100(%rbp)
	.section .t4391,"ax",@progbits
	cmpw $0x12345, 0x100(%rbp)
	.section .t4392,"ax",@progbits
	cmpw $0x7, (%rsp)
	.section .t4393,"ax",@progbits
	cmpw $0x12345, (%rsp)
	.section .t4394,"ax",@progbits
	cmpw $0x7, 0x10(%rsp,%rsi,4)
	.section .t4395,"ax",@progbits
	cmpw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4396,"ax",@progbits
	cmpw $0x7, %gs:0x10(%rcx)
	.section .t4397,"ax",@progbits
	cmpw $0x12345, %gs:0x10(%rcx)
	.section .t4398,"ax",@progbits
	cmpw $0x7, %fs:(%rax,%rsi,8)
	.section .t4399,"ax",@progbits
	cmpw $0x12345, %fs:(%rax,%rsi,8)
	.section .t4400,"ax",@progbits
	cmpl $0x7, (%rcx)
	.section .t4401,"ax",@progbits
	cmpl $0x12345, (%rcx)
	.section .t4402,"ax",@progbits
	cmpl $0x7, 0x10(%rcx)
	.section .t4403,"ax",@progbits
	cmpl $0x12345, 0x10(%rcx)
	.section .t4404,"ax",@progbits
	cmpl $0x7, -0x8(%rbp)
	.section .t4405,"ax",@progbits
	cmpl $0x12345, -0x8(%rbp)
	.section .t4406,"ax",@progbits
	cmpl $0x7, 0x12345(%rcx)
	.section .t4407,"ax",@progbits
	cmpl $0x12345, 0x12345(%rcx)
	.section .t4408,"ax",@progbits
	cmpl $0x7, (%rax,%rsi,4)
	.section .t4409,"ax",@progbits
	cmpl $0x12345, (%rax,%rsi,4)
	.section .t4410,"ax",@progbits
	cmpl $0x7, 0x10(%rax,%rsi,8)
	.section .t4411,"ax",@progbits
	cmpl $0x12345, 0x10(%rax,%rsi,8)
	.section .t4412,"ax",@progbits
	cmpl $0x7, (,%rsi,2)
	.section .t4413,"ax",@progbits
	cmpl $0x12345, (,%rsi,2)
	.section .t4414,"ax",@progbits
	cmpl $0x7, 0x40(%rip)
	.section .t4415,"ax",@progbits
	cmpl $0x12345, 0x40(%rip)
	.section .t4416,"ax",@progbits
	cmpl $0x7, -0x100(%rip)
	.section .t4417,"ax",@progbits
	cmpl $0x12345, -0x100(%rip)
	.section .t4418,"ax",@progbits
	cmpl $0x7, 0x1234
	.section .t4419,"ax",@progbits
	cmpl $0x12345, 0x1234
	.section .t4420,"ax",@progbits
	cmpl $0x7, (%r8)
	.section .t4421,"ax",@progbits
	cmpl $0x12345, (%r8)
	.section .t4422,"ax",@progbits
	cmpl $0x7, (%r12)
	.section .t4423,"ax",@progbits
	cmpl $0x12345, (%r12)
	.section .t4424,"ax",@progbits
	cmpl $0x7, 0x8(%r13)
	.section .t4425,"ax",@progbits
	cmpl $0x12345, 0x8(%r13)
	.section .t4426,"ax",@progbits
	cmpl $0x7, (%r8,%r15,2)
	.section .t4427,"ax",@progbits
	cmpl $0x12345, (%r8,%r15,2)
	.section .t4428,"ax",@progbits
	cmpl $0x7, (%rax,%r12,4)
	.section .t4429,"ax",@progbits
	cmpl $0x12345, (%rax,%r12,4)
	.section .t4430,"ax",@progbits
	cmpl $0x7, 0x100(%rbp)
	.section .t4431,"ax",@progbits
	cmpl $0x12345, 0x100(%rbp)
	.section .t4432,"ax",@progbits
	cmpl $0x7, (%rsp)
	.section .t4433,"ax",@progbits
	cmpl $0x12345, (%rsp)
	.section .t4434,"ax",@progbits
	cmpl $0x7, 0x10(%rsp,%rsi,4)
	.section .t4435,"ax",@progbits
	cmpl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4436,"ax",@progbits
	cmpl $0x7, %gs:0x10(%rcx)
	.section .t4437,"ax",@progbits
	cmpl $0x12345, %gs:0x10(%rcx)
	.section .t4438,"ax",@progbits
	cmpl $0x7, %fs:(%rax,%rsi,8)
	.section .t4439,"ax",@progbits
	cmpl $0x12345, %fs:(%rax,%rsi,8)
	.section .t4440,"ax",@progbits
	cmpq $0x7, (%rcx)
	.section .t4441,"ax",@progbits
	cmpq $0x12345, (%rcx)
	.section .t4442,"ax",@progbits
	cmpq $0x7, 0x10(%rcx)
	.section .t4443,"ax",@progbits
	cmpq $0x12345, 0x10(%rcx)
	.section .t4444,"ax",@progbits
	cmpq $0x7, -0x8(%rbp)
	.section .t4445,"ax",@progbits
	cmpq $0x12345, -0x8(%rbp)
	.section .t4446,"ax",@progbits
	cmpq $0x7, 0x12345(%rcx)
	.section .t4447,"ax",@progbits
	cmpq $0x12345, 0x12345(%rcx)
	.section .t4448,"ax",@progbits
	cmpq $0x7, (%rax,%rsi,4)
	.section .t4449,"ax",@progbits
	cmpq $0x12345, (%rax,%rsi,4)
	.section .t4450,"ax",@progbits
	cmpq $0x7, 0x10(%rax,%rsi,8)
	.section .t4451,"ax",@progbits
	cmpq $0x12345, 0x10(%rax,%rsi,8)
	.section .t4452,"ax",@progbits
	cmpq $0x7, (,%rsi,2)
	.section .t4453,"ax",@progbits
	cmpq $0x12345, (,%rsi,2)
	.section .t4454,"ax",@progbits
	cmpq $0x7, 0x40(%rip)
	.section .t4455,"ax",@progbits
	cmpq $0x12345, 0x40(%rip)
	.section .t4456,"ax",@progbits
	cmpq $0x7, -0x100(%rip)
	.section .t4457,"ax",@progbits
	cmpq $0x12345, -0x100(%rip)
	.section .t4458,"ax",@progbits
	cmpq $0x7, 0x1234
	.section .t4459,"ax",@progbits
	cmpq $0x12345, 0x1234
	.section .t4460,"ax",@progbits
	cmpq $0x7, (%r8)
	.section .t4461,"ax",@progbits
	cmpq $0x12345, (%r8)
	.section .t4462,"ax",@progbits
	cmpq $0x7, (%r12)
	.section .t4463,"ax",@progbits
	cmpq $0x12345, (%r12)
	.section .t4464,"ax",@progbits
	cmpq $0x7, 0x8(%r13)
	.section .t4465,"ax",@progbits
	cmpq $0x12345, 0x8(%r13)
	.section .t4466,"ax",@progbits
	cmpq $0x7, (%r8,%r15,2)
	.section .t4467,"ax",@progbits
	cmpq $0x12345, (%r8,%r15,2)
	.section .t4468,"ax",@progbits
	cmpq $0x7, (%rax,%r12,4)
	.section .t4469,"ax",@progbits
	cmpq $0x12345, (%rax,%r12,4)
	.section .t4470,"ax",@progbits
	cmpq $0x7, 0x100(%rbp)
	.section .t4471,"ax",@progbits
	cmpq $0x12345, 0x100(%rbp)
	.section .t4472,"ax",@progbits
	cmpq $0x7, (%rsp)
	.section .t4473,"ax",@progbits
	cmpq $0x12345, (%rsp)
	.section .t4474,"ax",@progbits
	cmpq $0x7, 0x10(%rsp,%rsi,4)
	.section .t4475,"ax",@progbits
	cmpq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4476,"ax",@progbits
	cmpq $0x7, %gs:0x10(%rcx)
	.section .t4477,"ax",@progbits
	cmpq $0x12345, %gs:0x10(%rcx)
	.section .t4478,"ax",@progbits
	cmpq $0x7, %fs:(%rax,%rsi,8)
	.section .t4479,"ax",@progbits
	cmpq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4480,"ax",@progbits
	testb $0x7, (%rcx)
	.section .t4481,"ax",@progbits
	testb $0x7f, (%rcx)
	.section .t4482,"ax",@progbits
	testb $0x7, 0x10(%rcx)
	.section .t4483,"ax",@progbits
	testb $0x7f, 0x10(%rcx)
	.section .t4484,"ax",@progbits
	testb $0x7, -0x8(%rbp)
	.section .t4485,"ax",@progbits
	testb $0x7f, -0x8(%rbp)
	.section .t4486,"ax",@progbits
	testb $0x7, 0x12345(%rcx)
	.section .t4487,"ax",@progbits
	testb $0x7f, 0x12345(%rcx)
	.section .t4488,"ax",@progbits
	testb $0x7, (%rax,%rsi,4)
	.section .t4489,"ax",@progbits
	testb $0x7f, (%rax,%rsi,4)
	.section .t4490,"ax",@progbits
	testb $0x7, 0x10(%rax,%rsi,8)
	.section .t4491,"ax",@progbits
	testb $0x7f, 0x10(%rax,%rsi,8)
	.section .t4492,"ax",@progbits
	testb $0x7, (,%rsi,2)
	.section .t4493,"ax",@progbits
	testb $0x7f, (,%rsi,2)
	.section .t4494,"ax",@progbits
	testb $0x7, 0x40(%rip)
	.section .t4495,"ax",@progbits
	testb $0x7f, 0x40(%rip)
	.section .t4496,"ax",@progbits
	testb $0x7, -0x100(%rip)
	.section .t4497,"ax",@progbits
	testb $0x7f, -0x100(%rip)
	.section .t4498,"ax",@progbits
	testb $0x7, 0x1234
	.section .t4499,"ax",@progbits
	testb $0x7f, 0x1234
	.section .t4500,"ax",@progbits
	testb $0x7, (%r8)
	.section .t4501,"ax",@progbits
	testb $0x7f, (%r8)
	.section .t4502,"ax",@progbits
	testb $0x7, (%r12)
	.section .t4503,"ax",@progbits
	testb $0x7f, (%r12)
	.section .t4504,"ax",@progbits
	testb $0x7, 0x8(%r13)
	.section .t4505,"ax",@progbits
	testb $0x7f, 0x8(%r13)
	.section .t4506,"ax",@progbits
	testb $0x7, (%r8,%r15,2)
	.section .t4507,"ax",@progbits
	testb $0x7f, (%r8,%r15,2)
	.section .t4508,"ax",@progbits
	testb $0x7, (%rax,%r12,4)
	.section .t4509,"ax",@progbits
	testb $0x7f, (%rax,%r12,4)
	.section .t4510,"ax",@progbits
	testb $0x7, 0x100(%rbp)
	.section .t4511,"ax",@progbits
	testb $0x7f, 0x100(%rbp)
	.section .t4512,"ax",@progbits
	testb $0x7, (%rsp)
	.section .t4513,"ax",@progbits
	testb $0x7f, (%rsp)
	.section .t4514,"ax",@progbits
	testb $0x7, 0x10(%rsp,%rsi,4)
	.section .t4515,"ax",@progbits
	testb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t4516,"ax",@progbits
	testb $0x7, %gs:0x10(%rcx)
	.section .t4517,"ax",@progbits
	testb $0x7f, %gs:0x10(%rcx)
	.section .t4518,"ax",@progbits
	testb $0x7, %fs:(%rax,%rsi,8)
	.section .t4519,"ax",@progbits
	testb $0x7f, %fs:(%rax,%rsi,8)
	.section .t4520,"ax",@progbits
	testw $0x7, (%rcx)
	.section .t4521,"ax",@progbits
	testw $0x12345, (%rcx)
	.section .t4522,"ax",@progbits
	testw $0x7, 0x10(%rcx)
	.section .t4523,"ax",@progbits
	testw $0x12345, 0x10(%rcx)
	.section .t4524,"ax",@progbits
	testw $0x7, -0x8(%rbp)
	.section .t4525,"ax",@progbits
	testw $0x12345, -0x8(%rbp)
	.section .t4526,"ax",@progbits
	testw $0x7, 0x12345(%rcx)
	.section .t4527,"ax",@progbits
	testw $0x12345, 0x12345(%rcx)
	.section .t4528,"ax",@progbits
	testw $0x7, (%rax,%rsi,4)
	.section .t4529,"ax",@progbits
	testw $0x12345, (%rax,%rsi,4)
	.section .t4530,"ax",@progbits
	testw $0x7, 0x10(%rax,%rsi,8)
	.section .t4531,"ax",@progbits
	testw $0x12345, 0x10(%rax,%rsi,8)
	.section .t4532,"ax",@progbits
	testw $0x7, (,%rsi,2)
	.section .t4533,"ax",@progbits
	testw $0x12345, (,%rsi,2)
	.section .t4534,"ax",@progbits
	testw $0x7, 0x40(%rip)
	.section .t4535,"ax",@progbits
	testw $0x12345, 0x40(%rip)
	.section .t4536,"ax",@progbits
	testw $0x7, -0x100(%rip)
	.section .t4537,"ax",@progbits
	testw $0x12345, -0x100(%rip)
	.section .t4538,"ax",@progbits
	testw $0x7, 0x1234
	.section .t4539,"ax",@progbits
	testw $0x12345, 0x1234
	.section .t4540,"ax",@progbits
	testw $0x7, (%r8)
	.section .t4541,"ax",@progbits
	testw $0x12345, (%r8)
	.section .t4542,"ax",@progbits
	testw $0x7, (%r12)
	.section .t4543,"ax",@progbits
	testw $0x12345, (%r12)
	.section .t4544,"ax",@progbits
	testw $0x7, 0x8(%r13)
	.section .t4545,"ax",@progbits
	testw $0x12345, 0x8(%r13)
	.section .t4546,"ax",@progbits
	testw $0x7, (%r8,%r15,2)
	.section .t4547,"ax",@progbits
	testw $0x12345, (%r8,%r15,2)
	.section .t4548,"ax",@progbits
	testw $0x7, (%rax,%r12,4)
	.section .t4549,"ax",@progbits
	testw $0x12345, (%rax,%r12,4)
	.section .t4550,"ax",@progbits
	testw $0x7, 0x100(%rbp)
	.section .t4551,"ax",@progbits
	testw $0x12345, 0x100(%rbp)
	.section .t4552,"ax",@progbits
	testw $0x7, (%rsp)
	.section .t4553,"ax",@progbits
	testw $0x12345, (%rsp)
	.section .t4554,"ax",@progbits
	testw $0x7, 0x10(%rsp,%rsi,4)
	.section .t4555,"ax",@progbits
	testw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4556,"ax",@progbits
	testw $0x7, %gs:0x10(%rcx)
	.section .t4557,"ax",@progbits
	testw $0x12345, %gs:0x10(%rcx)
	.section .t4558,"ax",@progbits
	testw $0x7, %fs:(%rax,%rsi,8)
	.section .t4559,"ax",@progbits
	testw $0x12345, %fs:(%rax,%rsi,8)
	.section .t4560,"ax",@progbits
	testl $0x7, (%rcx)
	.section .t4561,"ax",@progbits
	testl $0x12345, (%rcx)
	.section .t4562,"ax",@progbits
	testl $0x7, 0x10(%rcx)
	.section .t4563,"ax",@progbits
	testl $0x12345, 0x10(%rcx)
	.section .t4564,"ax",@progbits
	testl $0x7, -0x8(%rbp)
	.section .t4565,"ax",@progbits
	testl $0x12345, -0x8(%rbp)
	.section .t4566,"ax",@progbits
	testl $0x7, 0x12345(%rcx)
	.section .t4567,"ax",@progbits
	testl $0x12345, 0x12345(%rcx)
	.section .t4568,"ax",@progbits
	testl $0x7, (%rax,%rsi,4)
	.section .t4569,"ax",@progbits
	testl $0x12345, (%rax,%rsi,4)
	.section .t4570,"ax",@progbits
	testl $0x7, 0x10(%rax,%rsi,8)
	.section .t4571,"ax",@progbits
	testl $0x12345, 0x10(%rax,%rsi,8)
	.section .t4572,"ax",@progbits
	testl $0x7, (,%rsi,2)
	.section .t4573,"ax",@progbits
	testl $0x12345, (,%rsi,2)
	.section .t4574,"ax",@progbits
	testl $0x7, 0x40(%rip)
	.section .t4575,"ax",@progbits
	testl $0x12345, 0x40(%rip)
	.section .t4576,"ax",@progbits
	testl $0x7, -0x100(%rip)
	.section .t4577,"ax",@progbits
	testl $0x12345, -0x100(%rip)
	.section .t4578,"ax",@progbits
	testl $0x7, 0x1234
	.section .t4579,"ax",@progbits
	testl $0x12345, 0x1234
	.section .t4580,"ax",@progbits
	testl $0x7, (%r8)
	.section .t4581,"ax",@progbits
	testl $0x12345, (%r8)
	.section .t4582,"ax",@progbits
	testl $0x7, (%r12)
	.section .t4583,"ax",@progbits
	testl $0x12345, (%r12)
	.section .t4584,"ax",@progbits
	testl $0x7, 0x8(%r13)
	.section .t4585,"ax",@progbits
	testl $0x12345, 0x8(%r13)
	.section .t4586,"ax",@progbits
	testl $0x7, (%r8,%r15,2)
	.section .t4587,"ax",@progbits
	testl $0x12345, (%r8,%r15,2)
	.section .t4588,"ax",@progbits
	testl $0x7, (%rax,%r12,4)
	.section .t4589,"ax",@progbits
	testl $0x12345, (%rax,%r12,4)
	.section .t4590,"ax",@progbits
	testl $0x7, 0x100(%rbp)
	.section .t4591,"ax",@progbits
	testl $0x12345, 0x100(%rbp)
	.section .t4592,"ax",@progbits
	testl $0x7, (%rsp)
	.section .t4593,"ax",@progbits
	testl $0x12345, (%rsp)
	.section .t4594,"ax",@progbits
	testl $0x7, 0x10(%rsp,%rsi,4)
	.section .t4595,"ax",@progbits
	testl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4596,"ax",@progbits
	testl $0x7, %gs:0x10(%rcx)
	.section .t4597,"ax",@progbits
	testl $0x12345, %gs:0x10(%rcx)
	.section .t4598,"ax",@progbits
	testl $0x7, %fs:(%rax,%rsi,8)
	.section .t4599,"ax",@progbits
	testl $0x12345, %fs:(%rax,%rsi,8)
	.section .t4600,"ax",@progbits
	testq $0x7, (%rcx)
	.section .t4601,"ax",@progbits
	testq $0x12345, (%rcx)
	.section .t4602,"ax",@progbits
	testq $0x7, 0x10(%rcx)
	.section .t4603,"ax",@progbits
	testq $0x12345, 0x10(%rcx)
	.section .t4604,"ax",@progbits
	testq $0x7, -0x8(%rbp)
	.section .t4605,"ax",@progbits
	testq $0x12345, -0x8(%rbp)
	.section .t4606,"ax",@progbits
	testq $0x7, 0x12345(%rcx)
	.section .t4607,"ax",@progbits
	testq $0x12345, 0x12345(%rcx)
	.section .t4608,"ax",@progbits
	testq $0x7, (%rax,%rsi,4)
	.section .t4609,"ax",@progbits
	testq $0x12345, (%rax,%rsi,4)
	.section .t4610,"ax",@progbits
	testq $0x7, 0x10(%rax,%rsi,8)
	.section .t4611,"ax",@progbits
	testq $0x12345, 0x10(%rax,%rsi,8)
	.section .t4612,"ax",@progbits
	testq $0x7, (,%rsi,2)
	.section .t4613,"ax",@progbits
	testq $0x12345, (,%rsi,2)
	.section .t4614,"ax",@progbits
	testq $0x7, 0x40(%rip)
	.section .t4615,"ax",@progbits
	testq $0x12345, 0x40(%rip)
	.section .t4616,"ax",@progbits
	testq $0x7, -0x100(%rip)
	.section .t4617,"ax",@progbits
	testq $0x12345, -0x100(%rip)
	.section .t4618,"ax",@progbits
	testq $0x7, 0x1234
	.section .t4619,"ax",@progbits
	testq $0x12345, 0x1234
	.section .t4620,"ax",@progbits
	testq $0x7, (%r8)
	.section .t4621,"ax",@progbits
	testq $0x12345, (%r8)
	.section .t4622,"ax",@progbits
	testq $0x7, (%r12)
	.section .t4623,"ax",@progbits
	testq $0x12345, (%r12)
	.section .t4624,"ax",@progbits
	testq $0x7, 0x8(%r13)
	.section .t4625,"ax",@progbits
	testq $0x12345, 0x8(%r13)
	.section .t4626,"ax",@progbits
	testq $0x7, (%r8,%r15,2)
	.section .t4627,"ax",@progbits
	testq $0x12345, (%r8,%r15,2)
	.section .t4628,"ax",@progbits
	testq $0x7, (%rax,%r12,4)
	.section .t4629,"ax",@progbits
	testq $0x12345, (%rax,%r12,4)
	.section .t4630,"ax",@progbits
	testq $0x7, 0x100(%rbp)
	.section .t4631,"ax",@progbits
	testq $0x12345, 0x100(%rbp)
	.section .t4632,"ax",@progbits
	testq $0x7, (%rsp)
	.section .t4633,"ax",@progbits
	testq $0x12345, (%rsp)
	.section .t4634,"ax",@progbits
	testq $0x7, 0x10(%rsp,%rsi,4)
	.section .t4635,"ax",@progbits
	testq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4636,"ax",@progbits
	testq $0x7, %gs:0x10(%rcx)
	.section .t4637,"ax",@progbits
	testq $0x12345, %gs:0x10(%rcx)
	.section .t4638,"ax",@progbits
	testq $0x7, %fs:(%rax,%rsi,8)
	.section .t4639,"ax",@progbits
	testq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4640,"ax",@progbits
	movb $0x7, (%rcx)
	.section .t4641,"ax",@progbits
	movb $0x7f, (%rcx)
	.section .t4642,"ax",@progbits
	movb $0x7, 0x10(%rcx)
	.section .t4643,"ax",@progbits
	movb $0x7f, 0x10(%rcx)
	.section .t4644,"ax",@progbits
	movb $0x7, -0x8(%rbp)
	.section .t4645,"ax",@progbits
	movb $0x7f, -0x8(%rbp)
	.section .t4646,"ax",@progbits
	movb $0x7, 0x12345(%rcx)
	.section .t4647,"ax",@progbits
	movb $0x7f, 0x12345(%rcx)
	.section .t4648,"ax",@progbits
	movb $0x7, (%rax,%rsi,4)
	.section .t4649,"ax",@progbits
	movb $0x7f, (%rax,%rsi,4)
	.section .t4650,"ax",@progbits
	movb $0x7, 0x10(%rax,%rsi,8)
	.section .t4651,"ax",@progbits
	movb $0x7f, 0x10(%rax,%rsi,8)
	.section .t4652,"ax",@progbits
	movb $0x7, (,%rsi,2)
	.section .t4653,"ax",@progbits
	movb $0x7f, (,%rsi,2)
	.section .t4654,"ax",@progbits
	movb $0x7, 0x40(%rip)
	.section .t4655,"ax",@progbits
	movb $0x7f, 0x40(%rip)
	.section .t4656,"ax",@progbits
	movb $0x7, -0x100(%rip)
	.section .t4657,"ax",@progbits
	movb $0x7f, -0x100(%rip)
	.section .t4658,"ax",@progbits
	movb $0x7, 0x1234
	.section .t4659,"ax",@progbits
	movb $0x7f, 0x1234
	.section .t4660,"ax",@progbits
	movb $0x7, (%r8)
	.section .t4661,"ax",@progbits
	movb $0x7f, (%r8)
	.section .t4662,"ax",@progbits
	movb $0x7, (%r12)
	.section .t4663,"ax",@progbits
	movb $0x7f, (%r12)
	.section .t4664,"ax",@progbits
	movb $0x7, 0x8(%r13)
	.section .t4665,"ax",@progbits
	movb $0x7f, 0x8(%r13)
	.section .t4666,"ax",@progbits
	movb $0x7, (%r8,%r15,2)
	.section .t4667,"ax",@progbits
	movb $0x7f, (%r8,%r15,2)
	.section .t4668,"ax",@progbits
	movb $0x7, (%rax,%r12,4)
	.section .t4669,"ax",@progbits
	movb $0x7f, (%rax,%r12,4)
	.section .t4670,"ax",@progbits
	movb $0x7, 0x100(%rbp)
	.section .t4671,"ax",@progbits
	movb $0x7f, 0x100(%rbp)
	.section .t4672,"ax",@progbits
	movb $0x7, (%rsp)
	.section .t4673,"ax",@progbits
	movb $0x7f, (%rsp)
	.section .t4674,"ax",@progbits
	movb $0x7, 0x10(%rsp,%rsi,4)
	.section .t4675,"ax",@progbits
	movb $0x7f, 0x10(%rsp,%rsi,4)
	.section .t4676,"ax",@progbits
	movb $0x7, %gs:0x10(%rcx)
	.section .t4677,"ax",@progbits
	movb $0x7f, %gs:0x10(%rcx)
	.section .t4678,"ax",@progbits
	movb $0x7, %fs:(%rax,%rsi,8)
	.section .t4679,"ax",@progbits
	movb $0x7f, %fs:(%rax,%rsi,8)
	.section .t4680,"ax",@progbits
	movw $0x7, (%rcx)
	.section .t4681,"ax",@progbits
	movw $0x12345, (%rcx)
	.section .t4682,"ax",@progbits
	movw $0x7, 0x10(%rcx)
	.section .t4683,"ax",@progbits
	movw $0x12345, 0x10(%rcx)
	.section .t4684,"ax",@progbits
	movw $0x7, -0x8(%rbp)
	.section .t4685,"ax",@progbits
	movw $0x12345, -0x8(%rbp)
	.section .t4686,"ax",@progbits
	movw $0x7, 0x12345(%rcx)
	.section .t4687,"ax",@progbits
	movw $0x12345, 0x12345(%rcx)
	.section .t4688,"ax",@progbits
	movw $0x7, (%rax,%rsi,4)
	.section .t4689,"ax",@progbits
	movw $0x12345, (%rax,%rsi,4)
	.section .t4690,"ax",@progbits
	movw $0x7, 0x10(%rax,%rsi,8)
	.section .t4691,"ax",@progbits
	movw $0x12345, 0x10(%rax,%rsi,8)
	.section .t4692,"ax",@progbits
	movw $0x7, (,%rsi,2)
	.section .t4693,"ax",@progbits
	movw $0x12345, (,%rsi,2)
	.section .t4694,"ax",@progbits
	movw $0x7, 0x40(%rip)
	.section .t4695,"ax",@progbits
	movw $0x12345, 0x40(%rip)
	.section .t4696,"ax",@progbits
	movw $0x7, -0x100(%rip)
	.section .t4697,"ax",@progbits
	movw $0x12345, -0x100(%rip)
	.section .t4698,"ax",@progbits
	movw $0x7, 0x1234
	.section .t4699,"ax",@progbits
	movw $0x12345, 0x1234
	.section .t4700,"ax",@progbits
	movw $0x7, (%r8)
	.section .t4701,"ax",@progbits
	movw $0x12345, (%r8)
	.section .t4702,"ax",@progbits
	movw $0x7, (%r12)
	.section .t4703,"ax",@progbits
	movw $0x12345, (%r12)
	.section .t4704,"ax",@progbits
	movw $0x7, 0x8(%r13)
	.section .t4705,"ax",@progbits
	movw $0x12345, 0x8(%r13)
	.section .t4706,"ax",@progbits
	movw $0x7, (%r8,%r15,2)
	.section .t4707,"ax",@progbits
	movw $0x12345, (%r8,%r15,2)
	.section .t4708,"ax",@progbits
	movw $0x7, (%rax,%r12,4)
	.section .t4709,"ax",@progbits
	movw $0x12345, (%rax,%r12,4)
	.section .t4710,"ax",@progbits
	movw $0x7, 0x100(%rbp)
	.section .t4711,"ax",@progbits
	movw $0x12345, 0x100(%rbp)
	.section .t4712,"ax",@progbits
	movw $0x7, (%rsp)
	.section .t4713,"ax",@progbits
	movw $0x12345, (%rsp)
	.section .t4714,"ax",@progbits
	movw $0x7, 0x10(%rsp,%rsi,4)
	.section .t4715,"ax",@progbits
	movw $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4716,"ax",@progbits
	movw $0x7, %gs:0x10(%rcx)
	.section .t4717,"ax",@progbits
	movw $0x12345, %gs:0x10(%rcx)
	.section .t4718,"ax",@progbits
	movw $0x7, %fs:(%rax,%rsi,8)
	.section .t4719,"ax",@progbits
	movw $0x12345, %fs:(%rax,%rsi,8)
	.section .t4720,"ax",@progbits
	movl $0x7, (%rcx)
	.section .t4721,"ax",@progbits
	movl $0x12345, (%rcx)
	.section .t4722,"ax",@progbits
	movl $0x7, 0x10(%rcx)
	.section .t4723,"ax",@progbits
	movl $0x12345, 0x10(%rcx)
	.section .t4724,"ax",@progbits
	movl $0x7, -0x8(%rbp)
	.section .t4725,"ax",@progbits
	movl $0x12345, -0x8(%rbp)
	.section .t4726,"ax",@progbits
	movl $0x7, 0x12345(%rcx)
	.section .t4727,"ax",@progbits
	movl $0x12345, 0x12345(%rcx)
	.section .t4728,"ax",@progbits
	movl $0x7, (%rax,%rsi,4)
	.section .t4729,"ax",@progbits
	movl $0x12345, (%rax,%rsi,4)
	.section .t4730,"ax",@progbits
	movl $0x7, 0x10(%rax,%rsi,8)
	.section .t4731,"ax",@progbits
	movl $0x12345, 0x10(%rax,%rsi,8)
	.section .t4732,"ax",@progbits
	movl $0x7, (,%rsi,2)
	.section .t4733,"ax",@progbits
	movl $0x12345, (,%rsi,2)
	.section .t4734,"ax",@progbits
	movl $0x7, 0x40(%rip)
	.section .t4735,"ax",@progbits
	movl $0x12345, 0x40(%rip)
	.section .t4736,"ax",@progbits
	movl $0x7, -0x100(%rip)
	.section .t4737,"ax",@progbits
	movl $0x12345, -0x100(%rip)
	.section .t4738,"ax",@progbits
	movl $0x7, 0x1234
	.section .t4739,"ax",@progbits
	movl $0x12345, 0x1234
	.section .t4740,"ax",@progbits
	movl $0x7, (%r8)
	.section .t4741,"ax",@progbits
	movl $0x12345, (%r8)
	.section .t4742,"ax",@progbits
	movl $0x7, (%r12)
	.section .t4743,"ax",@progbits
	movl $0x12345, (%r12)
	.section .t4744,"ax",@progbits
	movl $0x7, 0x8(%r13)
	.section .t4745,"ax",@progbits
	movl $0x12345, 0x8(%r13)
	.section .t4746,"ax",@progbits
	movl $0x7, (%r8,%r15,2)
	.section .t4747,"ax",@progbits
	movl $0x12345, (%r8,%r15,2)
	.section .t4748,"ax",@progbits
	movl $0x7, (%rax,%r12,4)
	.section .t4749,"ax",@progbits
	movl $0x12345, (%rax,%r12,4)
	.section .t4750,"ax",@progbits
	movl $0x7, 0x100(%rbp)
	.section .t4751,"ax",@progbits
	movl $0x12345, 0x100(%rbp)
	.section .t4752,"ax",@progbits
	movl $0x7, (%rsp)
	.section .t4753,"ax",@progbits
	movl $0x12345, (%rsp)
	.section .t4754,"ax",@progbits
	movl $0x7, 0x10(%rsp,%rsi,4)
	.section .t4755,"ax",@progbits
	movl $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4756,"ax",@progbits
	movl $0x7, %gs:0x10(%rcx)
	.section .t4757,"ax",@progbits
	movl $0x12345, %gs:0x10(%rcx)
	.section .t4758,"ax",@progbits
	movl $0x7, %fs:(%rax,%rsi,8)
	.section .t4759,"ax",@progbits
	movl $0x12345, %fs:(%rax,%rsi,8)
	.section .t4760,"ax",@progbits
	movq $0x7, (%rcx)
	.section .t4761,"ax",@progbits
	movq $0x12345, (%rcx)
	.section .t4762,"ax",@progbits
	movq $0x7, 0x10(%rcx)
	.section .t4763,"ax",@progbits
	movq $0x12345, 0x10(%rcx)
	.section .t4764,"ax",@progbits
	movq $0x7, -0x8(%rbp)
	.section .t4765,"ax",@progbits
	movq $0x12345, -0x8(%rbp)
	.section .t4766,"ax",@progbits
	movq $0x7, 0x12345(%rcx)
	.section .t4767,"ax",@progbits
	movq $0x12345, 0x12345(%rcx)
	.section .t4768,"ax",@progbits
	movq $0x7, (%rax,%rsi,4)
	.section .t4769,"ax",@progbits
	movq $0x12345, (%rax,%rsi,4)
	.section .t4770,"ax",@progbits
	movq $0x7, 0x10(%rax,%rsi,8)
	.section .t4771,"ax",@progbits
	movq $0x12345, 0x10(%rax,%rsi,8)
	.section .t4772,"ax",@progbits
	movq $0x7, (,%rsi,2)
	.section .t4773,"ax",@progbits
	movq $0x12345, (,%rsi,2)
	.section .t4774,"ax",@progbits
	movq $0x7, 0x40(%rip)
	.section .t4775,"ax",@progbits
	movq $0x12345, 0x40(%rip)
	.section .t4776,"ax",@progbits
	movq $0x7, -0x100(%rip)
	.section .t4777,"ax",@progbits
	movq $0x12345, -0x100(%rip)
	.section .t4778,"ax",@progbits
	movq $0x7, 0x1234
	.section .t4779,"ax",@progbits
	movq $0x12345, 0x1234
	.section .t4780,"ax",@progbits
	movq $0x7, (%r8)
	.section .t4781,"ax",@progbits
	movq $0x12345, (%r8)
	.section .t4782,"ax",@progbits
	movq $0x7, (%r12)
	.section .t4783,"ax",@progbits
	movq $0x12345, (%r12)
	.section .t4784,"ax",@progbits
	movq $0x7, 0x8(%r13)
	.section .t4785,"ax",@progbits
	movq $0x12345, 0x8(%r13)
	.section .t4786,"ax",@progbits
	movq $0x7, (%r8,%r15,2)
	.section .t4787,"ax",@progbits
	movq $0x12345, (%r8,%r15,2)
	.section .t4788,"ax",@progbits
	movq $0x7, (%rax,%r12,4)
	.section .t4789,"ax",@progbits
	movq $0x12345, (%rax,%r12,4)
	.section .t4790,"ax",@progbits
	movq $0x7, 0x100(%rbp)
	.section .t4791,"ax",@progbits
	movq $0x12345, 0x100(%rbp)
	.section .t4792,"ax",@progbits
	movq $0x7, (%rsp)
	.section .t4793,"ax",@progbits
	movq $0x12345, (%rsp)
	.section .t4794,"ax",@progbits
	movq $0x7, 0x10(%rsp,%rsi,4)
	.section .t4795,"ax",@progbits
	movq $0x12345, 0x10(%rsp,%rsi,4)
	.section .t4796,"ax",@progbits
	movq $0x7, %gs:0x10(%rcx)
	.section .t4797,"ax",@progbits
	movq $0x12345, %gs:0x10(%rcx)
	.section .t4798,"ax",@progbits
	movq $0x7, %fs:(%rax,%rsi,8)
	.section .t4799,"ax",@progbits
	movq $0x12345, %fs:(%rax,%rsi,8)
	.section .t4800,"ax",@progbits
	btw $3, (%rcx)
	.section .t4801,"ax",@progbits
	btl $30, (%rcx)
	.section .t4802,"ax",@progbits
	btq $63, (%rcx)
	.section .t4803,"ax",@progbits
	btw $3, 0x10(%rcx)
	.section .t4804,"ax",@progbits
	btl $30, 0x10(%rcx)
	.section .t4805,"ax",@progbits
	btq $63, 0x10(%rcx)
	.section .t4806,"ax",@progbits
	btw $3, -0x8(%rbp)
	.section .t4807,"ax",@progbits
	btl $30, -0x8(%rbp)
	.section .t4808,"ax",@progbits
	btq $63, -0x8(%rbp)
	.section .t4809,"ax",@progbits
	btw $3, 0x12345(%rcx)
	.section .t4810,"ax",@progbits
	btl $30, 0x12345(%rcx)
	.section .t4811,"ax",@progbits
	btq $63, 0x12345(%rcx)
	.section .t4812,"ax",@progbits
	btw $3, (%rax,%rsi,4)
	.section .t4813,"ax",@progbits
	btl $30, (%rax,%rsi,4)
	.section .t4814,"ax",@progbits
	btq $63, (%rax,%rsi,4)
	.section .t4815,"ax",@progbits
	btw $3, 0x10(%rax,%rsi,8)
	.section .t4816,"ax",@progbits
	btl $30, 0x10(%rax,%rsi,8)
	.section .t4817,"ax",@progbits
	btq $63, 0x10(%rax,%rsi,8)
	.section .t4818,"ax",@progbits
	btw $3, (,%rsi,2)
	.section .t4819,"ax",@progbits
	btl $30, (,%rsi,2)
	.section .t4820,"ax",@progbits
	btq $63, (,%rsi,2)
	.section .t4821,"ax",@progbits
	btw $3, 0x40(%rip)
	.section .t4822,"ax",@progbits
	btl $30, 0x40(%rip)
	.section .t4823,"ax",@progbits
	btq $63, 0x40(%rip)
	.section .t4824,"ax",@progbits
	btw $3, -0x100(%rip)
	.section .t4825,"ax",@progbits
	btl $30, -0x100(%rip)
	.section .t4826,"ax",@progbits
	btq $63, -0x100(%rip)
	.section .t4827,"ax",@progbits
	btw $3, 0x1234
	.section .t4828,"ax",@progbits
	btl $30, 0x1234
	.section .t4829,"ax",@progbits
	btq $63, 0x1234
	.section .t4830,"ax",@progbits
	btw $3, (%r8)
	.section .t4831,"ax",@progbits
	btl $30, (%r8)
	.section .t4832,"ax",@progbits
	btq $63, (%r8)
	.section .t4833,"ax",@progbits
	btw $3, (%r12)
	.section .t4834,"ax",@progbits
	btl $30, (%r12)
	.section .t4835,"ax",@progbits
	btq $63, (%r12)
	.section .t4836,"ax",@progbits
	btw $3, 0x8(%r13)
	.section .t4837,"ax",@progbits
	btl $30, 0x8(%r13)
	.section .t4838,"ax",@progbits
	btq $63, 0x8(%r13)
	.section .t4839,"ax",@progbits
	btw $3, (%r8,%r15,2)
	.section .t4840,"ax",@progbits
	btl $30, (%r8,%r15,2)
	.section .t4841,"ax",@progbits
	btq $63, (%r8,%r15,2)
	.section .t4842,"ax",@progbits
	btw $3, (%rax,%r12,4)
	.section .t4843,"ax",@progbits
	btl $30, (%rax,%r12,4)
	.section .t4844,"ax",@progbits
	btq $63, (%rax,%r12,4)
	.section .t4845,"ax",@progbits
	btw $3, 0x100(%rbp)
	.section .t4846,"ax",@progbits
	btl $30, 0x100(%rbp)
	.section .t4847,"ax",@progbits
	btq $63, 0x100(%rbp)
	.section .t4848,"ax",@progbits
	btw $3, (%rsp)
	.section .t4849,"ax",@progbits
	btl $30, (%rsp)
	.section .t4850,"ax",@progbits
	btq $63, (%rsp)
	.section .t4851,"ax",@progbits
	btw $3, 0x10(%rsp,%rsi,4)
	.section .t4852,"ax",@progbits
	btl $30, 0x10(%rsp,%rsi,4)
	.section .t4853,"ax",@progbits
	btq $63, 0x10(%rsp,%rsi,4)
	.section .t4854,"ax",@progbits
	btw $3, %gs:0x10(%rcx)
	.section .t4855,"ax",@progbits
	btl $30, %gs:0x10(%rcx)
	.section .t4856,"ax",@progbits
	btq $63, %gs:0x10(%rcx)
	.section .t4857,"ax",@progbits
	btw $3, %fs:(%rax,%rsi,8)
	.section .t4858,"ax",@progbits
	btl $30, %fs:(%rax,%rsi,8)
	.section .t4859,"ax",@progbits
	btq $63, %fs:(%rax,%rsi,8)
	.section .t4860,"ax",@progbits
	btsw $3, (%rcx)
	.section .t4861,"ax",@progbits
	btsl $30, (%rcx)
	.section .t4862,"ax",@progbits
	btsq $63, (%rcx)
	.section .t4863,"ax",@progbits
	btsw $3, 0x10(%rcx)
	.section .t4864,"ax",@progbits
	btsl $30, 0x10(%rcx)
	.section .t4865,"ax",@progbits
	btsq $63, 0x10(%rcx)
	.section .t4866,"ax",@progbits
	btsw $3, -0x8(%rbp)
	.section .t4867,"ax",@progbits
	btsl $30, -0x8(%rbp)
	.section .t4868,"ax",@progbits
	btsq $63, -0x8(%rbp)
	.section .t4869,"ax",@progbits
	btsw $3, 0x12345(%rcx)
	.section .t4870,"ax",@progbits
	btsl $30, 0x12345(%rcx)
	.section .t4871,"ax",@progbits
	btsq $63, 0x12345(%rcx)
	.section .t4872,"ax",@progbits
	btsw $3, (%rax,%rsi,4)
	.section .t4873,"ax",@progbits
	btsl $30, (%rax,%rsi,4)
	.section .t4874,"ax",@progbits
	btsq $63, (%rax,%rsi,4)
	.section .t4875,"ax",@progbits
	btsw $3, 0x10(%rax,%rsi,8)
	.section .t4876,"ax",@progbits
	btsl $30, 0x10(%rax,%rsi,8)
	.section .t4877,"ax",@progbits
	btsq $63, 0x10(%rax,%rsi,8)
	.section .t4878,"ax",@progbits
	btsw $3, (,%rsi,2)
	.section .t4879,"ax",@progbits
	btsl $30, (,%rsi,2)
	.section .t4880,"ax",@progbits
	btsq $63, (,%rsi,2)
	.section .t4881,"ax",@progbits
	btsw $3, 0x40(%rip)
	.section .t4882,"ax",@progbits
	btsl $30, 0x40(%rip)
	.section .t4883,"ax",@progbits
	btsq $63, 0x40(%rip)
	.section .t4884,"ax",@progbits
	btsw $3, -0x100(%rip)
	.section .t4885,"ax",@progbits
	btsl $30, -0x100(%rip)
	.section .t4886,"ax",@progbits
	btsq $63, -0x100(%rip)
	.section .t4887,"ax",@progbits
	btsw $3, 0x1234
	.section .t4888,"ax",@progbits
	btsl $30, 0x1234
	.section .t4889,"ax",@progbits
	btsq $63, 0x1234
	.section .t4890,"ax",@progbits
	btsw $3, (%r8)
	.section .t4891,"ax",@progbits
	btsl $30, (%r8)
	.section .t4892,"ax",@progbits
	btsq $63, (%r8)
	.section .t4893,"ax",@progbits
	btsw $3, (%r12)
	.section .t4894,"ax",@progbits
	btsl $30, (%r12)
	.section .t4895,"ax",@progbits
	btsq $63, (%r12)
	.section .t4896,"ax",@progbits
	btsw $3, 0x8(%r13)
	.section .t4897,"ax",@progbits
	btsl $30, 0x8(%r13)
	.section .t4898,"ax",@progbits
	btsq $63, 0x8(%r13)
	.section .t4899,"ax",@progbits
	btsw $3, (%r8,%r15,2)
	.section .t4900,"ax",@progbits
	btsl $30, (%r8,%r15,2)
	.section .t4901,"ax",@progbits
	btsq $63, (%r8,%r15,2)
	.section .t4902,"ax",@progbits
	btsw $3, (%rax,%r12,4)
	.section .t4903,"ax",@progbits
	btsl $30, (%rax,%r12,4)
	.section .t4904,"ax",@progbits
	btsq $63, (%rax,%r12,4)
	.section .t4905,"ax",@progbits
	btsw $3, 0x100(%rbp)
	.section .t4906,"ax",@progbits
	btsl $30, 0x100(%rbp)
	.section .t4907,"ax",@progbits
	btsq $63, 0x100(%rbp)
	.section .t4908,"ax",@progbits
	btsw $3, (%rsp)
	.section .t4909,"ax",@progbits
	btsl $30, (%rsp)
	.section .t4910,"ax",@progbits
	btsq $63, (%rsp)
	.section .t4911,"ax",@progbits
	btsw $3, 0x10(%rsp,%rsi,4)
	.section .t4912,"ax",@progbits
	btsl $30, 0x10(%rsp,%rsi,4)
	.section .t4913,"ax",@progbits
	btsq $63, 0x10(%rsp,%rsi,4)
	.section .t4914,"ax",@progbits
	btsw $3, %gs:0x10(%rcx)
	.section .t4915,"ax",@progbits
	btsl $30, %gs:0x10(%rcx)
	.section .t4916,"ax",@progbits
	btsq $63, %gs:0x10(%rcx)
	.section .t4917,"ax",@progbits
	btsw $3, %fs:(%rax,%rsi,8)
	.section .t4918,"ax",@progbits
	btsl $30, %fs:(%rax,%rsi,8)
	.section .t4919,"ax",@progbits
	btsq $63, %fs:(%rax,%rsi,8)
	.section .t4920,"ax",@progbits
	btrw $3, (%rcx)
	.section .t4921,"ax",@progbits
	btrl $30, (%rcx)
	.section .t4922,"ax",@progbits
	btrq $63, (%rcx)
	.section .t4923,"ax",@progbits
	btrw $3, 0x10(%rcx)
	.section .t4924,"ax",@progbits
	btrl $30, 0x10(%rcx)
	.section .t4925,"ax",@progbits
	btrq $63, 0x10(%rcx)
	.section .t4926,"ax",@progbits
	btrw $3, -0x8(%rbp)
	.section .t4927,"ax",@progbits
	btrl $30, -0x8(%rbp)
	.section .t4928,"ax",@progbits
	btrq $63, -0x8(%rbp)
	.section .t4929,"ax",@progbits
	btrw $3, 0x12345(%rcx)
	.section .t4930,"ax",@progbits
	btrl $30, 0x12345(%rcx)
	.section .t4931,"ax",@progbits
	btrq $63, 0x12345(%rcx)
	.section .t4932,"ax",@progbits
	btrw $3, (%rax,%rsi,4)
	.section .t4933,"ax",@progbits
	btrl $30, (%rax,%rsi,4)
	.section .t4934,"ax",@progbits
	btrq $63, (%rax,%rsi,4)
	.section .t4935,"ax",@progbits
	btrw $3, 0x10(%rax,%rsi,8)
	.section .t4936,"ax",@progbits
	btrl $30, 0x10(%rax,%rsi,8)
	.section .t4937,"ax",@progbits
	btrq $63, 0x10(%rax,%rsi,8)
	.section .t4938,"ax",@progbits
	btrw $3, (,%rsi,2)
	.section .t4939,"ax",@progbits
	btrl $30, (,%rsi,2)
	.section .t4940,"ax",@progbits
	btrq $63, (,%rsi,2)
	.section .t4941,"ax",@progbits
	btrw $3, 0x40(%rip)
	.section .t4942,"ax",@progbits
	btrl $30, 0x40(%rip)
	.section .t4943,"ax",@progbits
	btrq $63, 0x40(%rip)
	.section .t4944,"ax",@progbits
	btrw $3, -0x100(%rip)
	.section .t4945,"ax",@progbits
	btrl $30, -0x100(%rip)
	.section .t4946,"ax",@progbits
	btrq $63, -0x100(%rip)
	.section .t4947,"ax",@progbits
	btrw $3, 0x1234
	.section .t4948,"ax",@progbits
	btrl $30, 0x1234
	.section .t4949,"ax",@progbits
	btrq $63, 0x1234
	.section .t4950,"ax",@progbits
	btrw $3, (%r8)
	.section .t4951,"ax",@progbits
	btrl $30, (%r8)
	.section .t4952,"ax",@progbits
	btrq $63, (%r8)
	.section .t4953,"ax",@progbits
	btrw $3, (%r12)
	.section .t4954,"ax",@progbits
	btrl $30, (%r12)
	.section .t4955,"ax",@progbits
	btrq $63, (%r12)
	.section .t4956,"ax",@progbits
	btrw $3, 0x8(%r13)
	.section .t4957,"ax",@progbits
	btrl $30, 0x8(%r13)
	.section .t4958,"ax",@progbits
	btrq $63, 0x8(%r13)
	.section .t4959,"ax",@progbits
	btrw $3, (%r8,%r15,2)
	.section .t4960,"ax",@progbits
	btrl $30, (%r8,%r15,2)
	.section .t4961,"ax",@progbits
	btrq $63, (%r8,%r15,2)
	.section .t4962,"ax",@progbits
	btrw $3, (%rax,%r12,4)
	.section .t4963,"ax",@progbits
	btrl $30, (%rax,%r12,4)
	.section .t4964,"ax",@progbits
	btrq $63, (%rax,%r12,4)
	.section .t4965,"ax",@progbits
	btrw $3, 0x100(%rbp)
	.section .t4966,"ax",@progbits
	btrl $30, 0x100(%rbp)
	.section .t4967,"ax",@progbits
	btrq $63, 0x100(%rbp)
	.section .t4968,"ax",@progbits
	btrw $3, (%rsp)
	.section .t4969,"ax",@progbits
	btrl $30, (%rsp)
	.section .t4970,"ax",@progbits
	btrq $63, (%rsp)
	.section .t4971,"ax",@progbits
	btrw $3, 0x10(%rsp,%rsi,4)
	.section .t4972,"ax",@progbits
	btrl $30, 0x10(%rsp,%rsi,4)
	.section .t4973,"ax",@progbits
	btrq $63, 0x10(%rsp,%rsi,4)
	.section .t4974,"ax",@progbits
	btrw $3, %gs:0x10(%rcx)
	.section .t4975,"ax",@progbits
	btrl $30, %gs:0x10(%rcx)
	.section .t4976,"ax",@progbits
	btrq $63, %gs:0x10(%rcx)
	.section .t4977,"ax",@progbits
	btrw $3, %fs:(%rax,%rsi,8)
	.section .t4978,"ax",@progbits
	btrl $30, %fs:(%rax,%rsi,8)
	.section .t4979,"ax",@progbits
	btrq $63, %fs:(%rax,%rsi,8)
	.section .t4980,"ax",@progbits
	btcw $3, (%rcx)
	.section .t4981,"ax",@progbits
	btcl $30, (%rcx)
	.section .t4982,"ax",@progbits
	btcq $63, (%rcx)
	.section .t4983,"ax",@progbits
	btcw $3, 0x10(%rcx)
	.section .t4984,"ax",@progbits
	btcl $30, 0x10(%rcx)
	.section .t4985,"ax",@progbits
	btcq $63, 0x10(%rcx)
	.section .t4986,"ax",@progbits
	btcw $3, -0x8(%rbp)
	.section .t4987,"ax",@progbits
	btcl $30, -0x8(%rbp)
	.section .t4988,"ax",@progbits
	btcq $63, -0x8(%rbp)
	.section .t4989,"ax",@progbits
	btcw $3, 0x12345(%rcx)
	.section .t4990,"ax",@progbits
	btcl $30, 0x12345(%rcx)
	.section .t4991,"ax",@progbits
	btcq $63, 0x12345(%rcx)
	.section .t4992,"ax",@progbits
	btcw $3, (%rax,%rsi,4)
	.section .t4993,"ax",@progbits
	btcl $30, (%rax,%rsi,4)
	.section .t4994,"ax",@progbits
	btcq $63, (%rax,%rsi,4)
	.section .t4995,"ax",@progbits
	btcw $3, 0x10(%rax,%rsi,8)
	.section .t4996,"ax",@progbits
	btcl $30, 0x10(%rax,%rsi,8)
	.section .t4997,"ax",@progbits
	btcq $63, 0x10(%rax,%rsi,8)
	.section .t4998,"ax",@progbits
	btcw $3, (,%rsi,2)
	.section .t4999,"ax",@progbits
	btcl $30, (,%rsi,2)
	.section .t5000,"ax",@progbits
	btcq $63, (,%rsi,2)
	.section .t5001,"ax",@progbits
	btcw $3, 0x40(%rip)
	.section .t5002,"ax",@progbits
	btcl $30, 0x40(%rip)
	.section .t5003,"ax",@progbits
	btcq $63, 0x40(%rip)
	.section .t5004,"ax",@progbits
	btcw $3, -0x100(%rip)
	.section .t5005,"ax",@progbits
	btcl $30, -0x100(%rip)
	.section .t5006,"ax",@progbits
	btcq $63, -0x100(%rip)
	.section .t5007,"ax",@progbits
	btcw $3, 0x1234
	.section .t5008,"ax",@progbits
	btcl $30, 0x1234
	.section .t5009,"ax",@progbits
	btcq $63, 0x1234
	.section .t5010,"ax",@progbits
	btcw $3, (%r8)
	.section .t5011,"ax",@progbits
	btcl $30, (%r8)
	.section .t5012,"ax",@progbits
	btcq $63, (%r8)
	.section .t5013,"ax",@progbits
	btcw $3, (%r12)
	.section .t5014,"ax",@progbits
	btcl $30, (%r12)
	.section .t5015,"ax",@progbits
	btcq $63, (%r12)
	.section .t5016,"ax",@progbits
	btcw $3, 0x8(%r13)
	.section .t5017,"ax",@progbits
	btcl $30, 0x8(%r13)
	.section .t5018,"ax",@progbits
	btcq $63, 0x8(%r13)
	.section .t5019,"ax",@progbits
	btcw $3, (%r8,%r15,2)
	.section .t5020,"ax",@progbits
	btcl $30, (%r8,%r15,2)
	.section .t5021,"ax",@progbits
	btcq $63, (%r8,%r15,2)
	.section .t5022,"ax",@progbits
	btcw $3, (%rax,%r12,4)
	.section .t5023,"ax",@progbits
	btcl $30, (%rax,%r12,4)
	.section .t5024,"ax",@progbits
	btcq $63, (%rax,%r12,4)
	.section .t5025,"ax",@progbits
	btcw $3, 0x100(%rbp)
	.section .t5026,"ax",@progbits
	btcl $30, 0x100(%rbp)
	.section .t5027,"ax",@progbits
	btcq $63, 0x100(%rbp)
	.section .t5028,"ax",@progbits
	btcw $3, (%rsp)
	.section .t5029,"ax",@progbits
	btcl $30, (%rsp)
	.section .t5030,"ax",@progbits
	btcq $63, (%rsp)
	.section .t5031,"ax",@progbits
	btcw $3, 0x10(%rsp,%rsi,4)
	.section .t5032,"ax",@progbits
	btcl $30, 0x10(%rsp,%rsi,4)
	.section .t5033,"ax",@progbits
	btcq $63, 0x10(%rsp,%rsi,4)
	.section .t5034,"ax",@progbits
	btcw $3, %gs:0x10(%rcx)
	.section .t5035,"ax",@progbits
	btcl $30, %gs:0x10(%rcx)
	.section .t5036,"ax",@progbits
	btcq $63, %gs:0x10(%rcx)
	.section .t5037,"ax",@progbits
	btcw $3, %fs:(%rax,%rsi,8)
	.section .t5038,"ax",@progbits
	btcl $30, %fs:(%rax,%rsi,8)
	.section .t5039,"ax",@progbits
	btcq $63, %fs:(%rax,%rsi,8)
	.section .t5040,"ax",@progbits
	incb (%rcx)
	.section .t5041,"ax",@progbits
	decb (%rcx)
	.section .t5042,"ax",@progbits
	incb 0x10(%rcx)
	.section .t5043,"ax",@progbits
	decb 0x10(%rcx)
	.section .t5044,"ax",@progbits
	incb -0x8(%rbp)
	.section .t5045,"ax",@progbits
	decb -0x8(%rbp)
	.section .t5046,"ax",@progbits
	incb 0x12345(%rcx)
	.section .t5047,"ax",@progbits
	decb 0x12345(%rcx)
	.section .t5048,"ax",@progbits
	incb (%rax,%rsi,4)
	.section .t5049,"ax",@progbits
	decb (%rax,%rsi,4)
	.section .t5050,"ax",@progbits
	incb 0x10(%rax,%rsi,8)
	.section .t5051,"ax",@progbits
	decb 0x10(%rax,%rsi,8)
	.section .t5052,"ax",@progbits
	incb (,%rsi,2)
	.section .t5053,"ax",@progbits
	decb (,%rsi,2)
	.section .t5054,"ax",@progbits
	incb 0x40(%rip)
	.section .t5055,"ax",@progbits
	decb 0x40(%rip)
	.section .t5056,"ax",@progbits
	incb -0x100(%rip)
	.section .t5057,"ax",@progbits
	decb -0x100(%rip)
	.section .t5058,"ax",@progbits
	incb 0x1234
	.section .t5059,"ax",@progbits
	decb 0x1234
	.section .t5060,"ax",@progbits
	incb (%r8)
	.section .t5061,"ax",@progbits
	decb (%r8)
	.section .t5062,"ax",@progbits
	incb (%r12)
	.section .t5063,"ax",@progbits
	decb (%r12)
	.section .t5064,"ax",@progbits
	incb 0x8(%r13)
	.section .t5065,"ax",@progbits
	decb 0x8(%r13)
	.section .t5066,"ax",@progbits
	incb (%r8,%r15,2)
	.section .t5067,"ax",@progbits
	decb (%r8,%r15,2)
	.section .t5068,"ax",@progbits
	incb (%rax,%r12,4)
	.section .t5069,"ax",@progbits
	decb (%rax,%r12,4)
	.section .t5070,"ax",@progbits
	incb 0x100(%rbp)
	.section .t5071,"ax",@progbits
	decb 0x100(%rbp)
	.section .t5072,"ax",@progbits
	incb (%rsp)
	.section .t5073,"ax",@progbits
	decb (%rsp)
	.section .t5074,"ax",@progbits
	incb 0x10(%rsp,%rsi,4)
	.section .t5075,"ax",@progbits
	decb 0x10(%rsp,%rsi,4)
	.section .t5076,"ax",@progbits
	incb %gs:0x10(%rcx)
	.section .t5077,"ax",@progbits
	decb %gs:0x10(%rcx)
	.section .t5078,"ax",@progbits
	incb %fs:(%rax,%rsi,8)
	.section .t5079,"ax",@progbits
	decb %fs:(%rax,%rsi,8)
	.section .t5080,"ax",@progbits
	incw (%rcx)
	.section .t5081,"ax",@progbits
	decw (%rcx)
	.section .t5082,"ax",@progbits
	incw 0x10(%rcx)
	.section .t5083,"ax",@progbits
	decw 0x10(%rcx)
	.section .t5084,"ax",@progbits
	incw -0x8(%rbp)
	.section .t5085,"ax",@progbits
	decw -0x8(%rbp)
	.section .t5086,"ax",@progbits
	incw 0x12345(%rcx)
	.section .t5087,"ax",@progbits
	decw 0x12345(%rcx)
	.section .t5088,"ax",@progbits
	incw (%rax,%rsi,4)
	.section .t5089,"ax",@progbits
	decw (%rax,%rsi,4)
	.section .t5090,"ax",@progbits
	incw 0x10(%rax,%rsi,8)
	.section .t5091,"ax",@progbits
	decw 0x10(%rax,%rsi,8)
	.section .t5092,"ax",@progbits
	incw (,%rsi,2)
	.section .t5093,"ax",@progbits
	decw (,%rsi,2)
	.section .t5094,"ax",@progbits
	incw 0x40(%rip)
	.section .t5095,"ax",@progbits
	decw 0x40(%rip)
	.section .t5096,"ax",@progbits
	incw -0x100(%rip)
	.section .t5097,"ax",@progbits
	decw -0x100(%rip)
	.section .t5098,"ax",@progbits
	incw 0x1234
	.section .t5099,"ax",@progbits
	decw 0x1234
	.section .t5100,"ax",@progbits
	incw (%r8)
	.section .t5101,"ax",@progbits
	decw (%r8)
	.section .t5102,"ax",@progbits
	incw (%r12)
	.section .t5103,"ax",@progbits
	decw (%r12)
	.section .t5104,"ax",@progbits
	incw 0x8(%r13)
	.section .t5105,"ax",@progbits
	decw 0x8(%r13)
	.section .t5106,"ax",@progbits
	incw (%r8,%r15,2)
	.section .t5107,"ax",@progbits
	decw (%r8,%r15,2)
	.section .t5108,"ax",@progbits
	incw (%rax,%r12,4)
	.section .t5109,"ax",@progbits
	decw (%rax,%r12,4)
	.section .t5110,"ax",@progbits
	incw 0x100(%rbp)
	.section .t5111,"ax",@progbits
	decw 0x100(%rbp)
	.section .t5112,"ax",@progbits
	incw (%rsp)
	.section .t5113,"ax",@progbits
	decw (%rsp)
	.section .t5114,"ax",@progbits
	incw 0x10(%rsp,%rsi,4)
	.section .t5115,"ax",@progbits
	decw 0x10(%rsp,%rsi,4)
	.section .t5116,"ax",@progbits
	incw %gs:0x10(%rcx)
	.section .t5117,"ax",@progbits
	decw %gs:0x10(%rcx)
	.section .t5118,"ax",@progbits
	incw %fs:(%rax,%rsi,8)
	.section .t5119,"ax",@progbits
	decw %fs:(%rax,%rsi,8)
	.section .t5120,"ax",@progbits
	incl (%rcx)
	.section .t5121,"ax",@progbits
	decl (%rcx)
	.section .t5122,"ax",@progbits
	incl 0x10(%rcx)
	.section .t5123,"ax",@progbits
	decl 0x10(%rcx)
	.section .t5124,"ax",@progbits
	incl -0x8(%rbp)
	.section .t5125,"ax",@progbits
	decl -0x8(%rbp)
	.section .t5126,"ax",@progbits
	incl 0x12345(%rcx)
	.section .t5127,"ax",@progbits
	decl 0x12345(%rcx)
	.section .t5128,"ax",@progbits
	incl (%rax,%rsi,4)
	.section .t5129,"ax",@progbits
	decl (%rax,%rsi,4)
	.section .t5130,"ax",@progbits
	incl 0x10(%rax,%rsi,8)
	.section .t5131,"ax",@progbits
	decl 0x10(%rax,%rsi,8)
	.section .t5132,"ax",@progbits
	incl (,%rsi,2)
	.section .t5133,"ax",@progbits
	decl (,%rsi,2)
	.section .t5134,"ax",@progbits
	incl 0x40(%rip)
	.section .t5135,"ax",@progbits
	decl 0x40(%rip)
	.section .t5136,"ax",@progbits
	incl -0x100(%rip)
	.section .t5137,"ax",@progbits
	decl -0x100(%rip)
	.section .t5138,"ax",@progbits
	incl 0x1234
	.section .t5139,"ax",@progbits
	decl 0x1234
	.section .t5140,"ax",@progbits
	incl (%r8)
	.section .t5141,"ax",@progbits
	decl (%r8)
	.section .t5142,"ax",@progbits
	incl (%r12)
	.section .t5143,"ax",@progbits
	decl (%r12)
	.section .t5144,"ax",@progbits
	incl 0x8(%r13)
	.section .t5145,"ax",@progbits
	decl 0x8(%r13)
	.section .t5146,"ax",@progbits
	incl (%r8,%r15,2)
	.section .t5147,"ax",@progbits
	decl (%r8,%r15,2)
	.section .t5148,"ax",@progbits
	incl (%rax,%r12,4)
	.section .t5149,"ax",@progbits
	decl (%rax,%r12,4)
	.section .t5150,"ax",@progbits
	incl 0x100(%rbp)
	.section .t5151,"ax",@progbits
	decl 0x100(%rbp)
	.section .t5152,"ax",@progbits
	incl (%rsp)
	.section .t5153,"ax",@progbits
	decl (%rsp)
	.section .t5154,"ax",@progbits
	incl 0x10(%rsp,%rsi,4)
	.section .t5155,"ax",@progbits
	decl 0x10(%rsp,%rsi,4)
	.section .t5156,"ax",@progbits
	incl %gs:0x10(%rcx)
	.section .t5157,"ax",@progbits
	decl %gs:0x10(%rcx)
	.section .t5158,"ax",@progbits
	incl %fs:(%rax,%rsi,8)
	.section .t5159,"ax",@progbits
	decl %fs:(%rax,%rsi,8)
	.section .t5160,"ax",@progbits
	incq (%rcx)
	.section .t5161,"ax",@progbits
	decq (%rcx)
	.section .t5162,"ax",@progbits
	incq 0x10(%rcx)
	.section .t5163,"ax",@progbits
	decq 0x10(%rcx)
	.section .t5164,"ax",@progbits
	incq -0x8(%rbp)
	.section .t5165,"ax",@progbits
	decq -0x8(%rbp)
	.section .t5166,"ax",@progbits
	incq 0x12345(%rcx)
	.section .t5167,"ax",@progbits
	decq 0x12345(%rcx)
	.section .t5168,"ax",@progbits
	incq (%rax,%rsi,4)
	.section .t5169,"ax",@progbits
	decq (%rax,%rsi,4)
	.section .t5170,"ax",@progbits
	incq 0x10(%rax,%rsi,8)
	.section .t5171,"ax",@progbits
	decq 0x10(%rax,%rsi,8)
	.section .t5172,"ax",@progbits
	incq (,%rsi,2)
	.section .t5173,"ax",@progbits
	decq (,%rsi,2)
	.section .t5174,"ax",@progbits
	incq 0x40(%rip)
	.section .t5175,"ax",@progbits
	decq 0x40(%rip)
	.section .t5176,"ax",@progbits
	incq -0x100(%rip)
	.section .t5177,"ax",@progbits
	decq -0x100(%rip)
	.section .t5178,"ax",@progbits
	incq 0x1234
	.section .t5179,"ax",@progbits
	decq 0x1234
	.section .t5180,"ax",@progbits
	incq (%r8)
	.section .t5181,"ax",@progbits
	decq (%r8)
	.section .t5182,"ax",@progbits
	incq (%r12)
	.section .t5183,"ax",@progbits
	decq (%r12)
	.section .t5184,"ax",@progbits
	incq 0x8(%r13)
	.section .t5185,"ax",@progbits
	decq 0x8(%r13)
	.section .t5186,"ax",@progbits
	incq (%r8,%r15,2)
	.section .t5187,"ax",@progbits
	decq (%r8,%r15,2)
	.section .t5188,"ax",@progbits
	incq (%rax,%r12,4)
	.section .t5189,"ax",@progbits
	decq (%rax,%r12,4)
	.section .t5190,"ax",@progbits
	incq 0x100(%rbp)
	.section .t5191,"ax",@progbits
	decq 0x100(%rbp)
	.section .t5192,"ax",@progbits
	incq (%rsp)
	.section .t5193,"ax",@progbits
	decq (%rsp)
	.section .t5194,"ax",@progbits
	incq 0x10(%rsp,%rsi,4)
	.section .t5195,"ax",@progbits
	decq 0x10(%rsp,%rsi,4)
	.section .t5196,"ax",@progbits
	incq %gs:0x10(%rcx)
	.section .t5197,"ax",@progbits
	decq %gs:0x10(%rcx)
	.section .t5198,"ax",@progbits
	incq %fs:(%rax,%rsi,8)
	.section .t5199,"ax",@progbits
	decq %fs:(%rax,%rsi,8)
	.section .t5200,"ax",@progbits
	btw %r14w, (%rcx)
	.section .t5201,"ax",@progbits
	btl %r13d, (%rcx)
	.section .t5202,"ax",@progbits
	btl %r14d, (%rcx)
	.section .t5203,"ax",@progbits
	btq %r15, (%rcx)
	.section .t5204,"ax",@progbits
	btw %r14w, 0x10(%rcx)
	.section .t5205,"ax",@progbits
	btl %r13d, 0x10(%rcx)
	.section .t5206,"ax",@progbits
	btl %r14d, 0x10(%rcx)
	.section .t5207,"ax",@progbits
	btq %r15, 0x10(%rcx)
	.section .t5208,"ax",@progbits
	btw %r14w, -0x8(%rbp)
	.section .t5209,"ax",@progbits
	btl %r13d, -0x8(%rbp)
	.section .t5210,"ax",@progbits
	btl %r14d, -0x8(%rbp)
	.section .t5211,"ax",@progbits
	btq %r15, -0x8(%rbp)
	.section .t5212,"ax",@progbits
	btw %r14w, 0x12345(%rcx)
	.section .t5213,"ax",@progbits
	btl %r13d, 0x12345(%rcx)
	.section .t5214,"ax",@progbits
	btl %r14d, 0x12345(%rcx)
	.section .t5215,"ax",@progbits
	btq %r15, 0x12345(%rcx)
	.section .t5216,"ax",@progbits
	btw %r14w, (%rax,%rsi,4)
	.section .t5217,"ax",@progbits
	btl %r13d, (%rax,%rsi,4)
	.section .t5218,"ax",@progbits
	btl %r14d, (%rax,%rsi,4)
	.section .t5219,"ax",@progbits
	btq %r15, (%rax,%rsi,4)
	.section .t5220,"ax",@progbits
	btw %r14w, 0x10(%rax,%rsi,8)
	.section .t5221,"ax",@progbits
	btl %r13d, 0x10(%rax,%rsi,8)
	.section .t5222,"ax",@progbits
	btl %r14d, 0x10(%rax,%rsi,8)
	.section .t5223,"ax",@progbits
	btq %r15, 0x10(%rax,%rsi,8)
	.section .t5224,"ax",@progbits
	btw %r14w, (,%rsi,2)
	.section .t5225,"ax",@progbits
	btl %r13d, (,%rsi,2)
	.section .t5226,"ax",@progbits
	btl %r14d, (,%rsi,2)
	.section .t5227,"ax",@progbits
	btq %r15, (,%rsi,2)
	.section .t5228,"ax",@progbits
	btw %r14w, 0x40(%rip)
	.section .t5229,"ax",@progbits
	btl %r13d, 0x40(%rip)
	.section .t5230,"ax",@progbits
	btl %r14d, 0x40(%rip)
	.section .t5231,"ax",@progbits
	btq %r15, 0x40(%rip)
	.section .t5232,"ax",@progbits
	btw %r14w, -0x100(%rip)
	.section .t5233,"ax",@progbits
	btl %r13d, -0x100(%rip)
	.section .t5234,"ax",@progbits
	btl %r14d, -0x100(%rip)
	.section .t5235,"ax",@progbits
	btq %r15, -0x100(%rip)
	.section .t5236,"ax",@progbits
	btw %r14w, 0x1234
	.section .t5237,"ax",@progbits
	btl %r13d, 0x1234
	.section .t5238,"ax",@progbits
	btl %r14d, 0x1234
	.section .t5239,"ax",@progbits
	btq %r15, 0x1234
	.section .t5240,"ax",@progbits
	btw %r14w, (%r8)
	.section .t5241,"ax",@progbits
	btl %r13d, (%r8)
	.section .t5242,"ax",@progbits
	btl %r14d, (%r8)
	.section .t5243,"ax",@progbits
	btq %r15, (%r8)
	.section .t5244,"ax",@progbits
	btw %r14w, (%r12)
	.section .t5245,"ax",@progbits
	btl %r13d, (%r12)
	.section .t5246,"ax",@progbits
	btl %r14d, (%r12)
	.section .t5247,"ax",@progbits
	btq %r15, (%r12)
	.section .t5248,"ax",@progbits
	btw %r14w, 0x8(%r13)
	.section .t5249,"ax",@progbits
	btl %r13d, 0x8(%r13)
	.section .t5250,"ax",@progbits
	btl %r14d, 0x8(%r13)
	.section .t5251,"ax",@progbits
	btq %r15, 0x8(%r13)
	.section .t5252,"ax",@progbits
	btw %r14w, (%r8,%r15,2)
	.section .t5253,"ax",@progbits
	btl %r13d, (%r8,%r15,2)
	.section .t5254,"ax",@progbits
	btl %r14d, (%r8,%r15,2)
	.section .t5255,"ax",@progbits
	btq %r15, (%r8,%r15,2)
	.section .t5256,"ax",@progbits
	btw %r14w, (%rax,%r12,4)
	.section .t5257,"ax",@progbits
	btl %r13d, (%rax,%r12,4)
	.section .t5258,"ax",@progbits
	btl %r14d, (%rax,%r12,4)
	.section .t5259,"ax",@progbits
	btq %r15, (%rax,%r12,4)
	.section .t5260,"ax",@progbits
	btw %r14w, 0x100(%rbp)
	.section .t5261,"ax",@progbits
	btl %r13d, 0x100(%rbp)
	.section .t5262,"ax",@progbits
	btl %r14d, 0x100(%rbp)
	.section .t5263,"ax",@progbits
	btq %r15, 0x100(%rbp)
	.section .t5264,"ax",@progbits
	btw %r14w, (%rsp)
	.section .t5265,"ax",@progbits
	btl %r13d, (%rsp)
	.section .t5266,"ax",@progbits
	btl %r14d, (%rsp)
	.section .t5267,"ax",@progbits
	btq %r15, (%rsp)
	.section .t5268,"ax",@progbits
	btw %r14w, 0x10(%rsp,%rsi,4)
	.section .t5269,"ax",@progbits
	btl %r13d, 0x10(%rsp,%rsi,4)
	.section .t5270,"ax",@progbits
	btl %r14d, 0x10(%rsp,%rsi,4)
	.section .t5271,"ax",@progbits
	btq %r15, 0x10(%rsp,%rsi,4)
	.section .t5272,"ax",@progbits
	btw %r14w, %gs:0x10(%rcx)
	.section .t5273,"ax",@progbits
	btl %r13d, %gs:0x10(%rcx)
	.section .t5274,"ax",@progbits
	btl %r14d, %gs:0x10(%rcx)
	.section .t5275,"ax",@progbits
	btq %r15, %gs:0x10(%rcx)
	.section .t5276,"ax",@progbits
	btw %r14w, %fs:(%rax,%rsi,8)
	.section .t5277,"ax",@progbits
	btl %r13d, %fs:(%rax,%rsi,8)
	.section .t5278,"ax",@progbits
	btl %r14d, %fs:(%rax,%rsi,8)
	.section .t5279,"ax",@progbits
	btq %r15, %fs:(%rax,%rsi,8)
	.section .t5280,"ax",@progbits
	btsw %r14w, (%rcx)
	.section .t5281,"ax",@progbits
	btsl %r13d, (%rcx)
	.section .t5282,"ax",@progbits
	btsl %r14d, (%rcx)
	.section .t5283,"ax",@progbits
	btsq %r15, (%rcx)
	.section .t5284,"ax",@progbits
	btsw %r14w, 0x10(%rcx)
	.section .t5285,"ax",@progbits
	btsl %r13d, 0x10(%rcx)
	.section .t5286,"ax",@progbits
	btsl %r14d, 0x10(%rcx)
	.section .t5287,"ax",@progbits
	btsq %r15, 0x10(%rcx)
	.section .t5288,"ax",@progbits
	btsw %r14w, -0x8(%rbp)
	.section .t5289,"ax",@progbits
	btsl %r13d, -0x8(%rbp)
	.section .t5290,"ax",@progbits
	btsl %r14d, -0x8(%rbp)
	.section .t5291,"ax",@progbits
	btsq %r15, -0x8(%rbp)
	.section .t5292,"ax",@progbits
	btsw %r14w, 0x12345(%rcx)
	.section .t5293,"ax",@progbits
	btsl %r13d, 0x12345(%rcx)
	.section .t5294,"ax",@progbits
	btsl %r14d, 0x12345(%rcx)
	.section .t5295,"ax",@progbits
	btsq %r15, 0x12345(%rcx)
	.section .t5296,"ax",@progbits
	btsw %r14w, (%rax,%rsi,4)
	.section .t5297,"ax",@progbits
	btsl %r13d, (%rax,%rsi,4)
	.section .t5298,"ax",@progbits
	btsl %r14d, (%rax,%rsi,4)
	.section .t5299,"ax",@progbits
	btsq %r15, (%rax,%rsi,4)
	.section .t5300,"ax",@progbits
	btsw %r14w, 0x10(%rax,%rsi,8)
	.section .t5301,"ax",@progbits
	btsl %r13d, 0x10(%rax,%rsi,8)
	.section .t5302,"ax",@progbits
	btsl %r14d, 0x10(%rax,%rsi,8)
	.section .t5303,"ax",@progbits
	btsq %r15, 0x10(%rax,%rsi,8)
	.section .t5304,"ax",@progbits
	btsw %r14w, (,%rsi,2)
	.section .t5305,"ax",@progbits
	btsl %r13d, (,%rsi,2)
	.section .t5306,"ax",@progbits
	btsl %r14d, (,%rsi,2)
	.section .t5307,"ax",@progbits
	btsq %r15, (,%rsi,2)
	.section .t5308,"ax",@progbits
	btsw %r14w, 0x40(%rip)
	.section .t5309,"ax",@progbits
	btsl %r13d, 0x40(%rip)
	.section .t5310,"ax",@progbits
	btsl %r14d, 0x40(%rip)
	.section .t5311,"ax",@progbits
	btsq %r15, 0x40(%rip)
	.section .t5312,"ax",@progbits
	btsw %r14w, -0x100(%rip)
	.section .t5313,"ax",@progbits
	btsl %r13d, -0x100(%rip)
	.section .t5314,"ax",@progbits
	btsl %r14d, -0x100(%rip)
	.section .t5315,"ax",@progbits
	btsq %r15, -0x100(%rip)
	.section .t5316,"ax",@progbits
	btsw %r14w, 0x1234
	.section .t5317,"ax",@progbits
	btsl %r13d, 0x1234
	.section .t5318,"ax",@progbits
	btsl %r14d, 0x1234
	.section .t5319,"ax",@progbits
	btsq %r15, 0x1234
	.section .t5320,"ax",@progbits
	btsw %r14w, (%r8)
	.section .t5321,"ax",@progbits
	btsl %r13d, (%r8)
	.section .t5322,"ax",@progbits
	btsl %r14d, (%r8)
	.section .t5323,"ax",@progbits
	btsq %r15, (%r8)
	.section .t5324,"ax",@progbits
	btsw %r14w, (%r12)
	.section .t5325,"ax",@progbits
	btsl %r13d, (%r12)
	.section .t5326,"ax",@progbits
	btsl %r14d, (%r12)
	.section .t5327,"ax",@progbits
	btsq %r15, (%r12)
	.section .t5328,"ax",@progbits
	btsw %r14w, 0x8(%r13)
	.section .t5329,"ax",@progbits
	btsl %r13d, 0x8(%r13)
	.section .t5330,"ax",@progbits
	btsl %r14d, 0x8(%r13)
	.section .t5331,"ax",@progbits
	btsq %r15, 0x8(%r13)
	.section .t5332,"ax",@progbits
	btsw %r14w, (%r8,%r15,2)
	.section .t5333,"ax",@progbits
	btsl %r13d, (%r8,%r15,2)
	.section .t5334,"ax",@progbits
	btsl %r14d, (%r8,%r15,2)
	.section .t5335,"ax",@progbits
	btsq %r15, (%r8,%r15,2)
	.section .t5336,"ax",@progbits
	btsw %r14w, (%rax,%r12,4)
	.section .t5337,"ax",@progbits
	btsl %r13d, (%rax,%r12,4)
	.section .t5338,"ax",@progbits
	btsl %r14d, (%rax,%r12,4)
	.section .t5339,"ax",@progbits
	btsq %r15, (%rax,%r12,4)
	.section .t5340,"ax",@progbits
	btsw %r14w, 0x100(%rbp)
	.section .t5341,"ax",@progbits
	btsl %r13d, 0x100(%rbp)
	.section .t5342,"ax",@progbits
	btsl %r14d, 0x100(%rbp)
	.section .t5343,"ax",@progbits
	btsq %r15, 0x100(%rbp)
	.section .t5344,"ax",@progbits
	btsw %r14w, (%rsp)
	.section .t5345,"ax",@progbits
	btsl %r13d, (%rsp)
	.section .t5346,"ax",@progbits
	btsl %r14d, (%rsp)
	.section .t5347,"ax",@progbits
	btsq %r15, (%rsp)
	.section .t5348,"ax",@progbits
	btsw %r14w, 0x10(%rsp,%rsi,4)
	.section .t5349,"ax",@progbits
	btsl %r13d, 0x10(%rsp,%rsi,4)
	.section .t5350,"ax",@progbits
	btsl %r14d, 0x10(%rsp,%rsi,4)
	.section .t5351,"ax",@progbits
	btsq %r15, 0x10(%rsp,%rsi,4)
	.section .t5352,"ax",@progbits
	btsw %r14w, %gs:0x10(%rcx)
	.section .t5353,"ax",@progbits
	btsl %r13d, %gs:0x10(%rcx)
	.section .t5354,"ax",@progbits
	btsl %r14d, %gs:0x10(%rcx)
	.section .t5355,"ax",@progbits
	btsq %r15, %gs:0x10(%rcx)
	.section .t5356,"ax",@progbits
	btsw %r14w, %fs:(%rax,%rsi,8)
	.section .t5357,"ax",@progbits
	btsl %r13d, %fs:(%rax,%rsi,8)
	.section .t5358,"ax",@progbits
	btsl %r14d, %fs:(%rax,%rsi,8)
	.section .t5359,"ax",@progbits
	btsq %r15, %fs:(%rax,%rsi,8)
	.section .t5360,"ax",@progbits
	btrw %r14w, (%rcx)
	.section .t5361,"ax",@progbits
	btrl %r13d, (%rcx)
	.section .t5362,"ax",@progbits
	btrl %r14d, (%rcx)
	.section .t5363,"ax",@progbits
	btrq %r15, (%rcx)
	.section .t5364,"ax",@progbits
	btrw %r14w, 0x10(%rcx)
	.section .t5365,"ax",@progbits
	btrl %r13d, 0x10(%rcx)
	.section .t5366,"ax",@progbits
	btrl %r14d, 0x10(%rcx)
	.section .t5367,"ax",@progbits
	btrq %r15, 0x10(%rcx)
	.section .t5368,"ax",@progbits
	btrw %r14w, -0x8(%rbp)
	.section .t5369,"ax",@progbits
	btrl %r13d, -0x8(%rbp)
	.section .t5370,"ax",@progbits
	btrl %r14d, -0x8(%rbp)
	.section .t5371,"ax",@progbits
	btrq %r15, -0x8(%rbp)
	.section .t5372,"ax",@progbits
	btrw %r14w, 0x12345(%rcx)
	.section .t5373,"ax",@progbits
	btrl %r13d, 0x12345(%rcx)
	.section .t5374,"ax",@progbits
	btrl %r14d, 0x12345(%rcx)
	.section .t5375,"ax",@progbits
	btrq %r15, 0x12345(%rcx)
	.section .t5376,"ax",@progbits
	btrw %r14w, (%rax,%rsi,4)
	.section .t5377,"ax",@progbits
	btrl %r13d, (%rax,%rsi,4)
	.section .t5378,"ax",@progbits
	btrl %r14d, (%rax,%rsi,4)
	.section .t5379,"ax",@progbits
	btrq %r15, (%rax,%rsi,4)
	.section .t5380,"ax",@progbits
	btrw %r14w, 0x10(%rax,%rsi,8)
	.section .t5381,"ax",@progbits
	btrl %r13d, 0x10(%rax,%rsi,8)
	.section .t5382,"ax",@progbits
	btrl %r14d, 0x10(%rax,%rsi,8)
	.section .t5383,"ax",@progbits
	btrq %r15, 0x10(%rax,%rsi,8)
	.section .t5384,"ax",@progbits
	btrw %r14w, (,%rsi,2)
	.section .t5385,"ax",@progbits
	btrl %r13d, (,%rsi,2)
	.section .t5386,"ax",@progbits
	btrl %r14d, (,%rsi,2)
	.section .t5387,"ax",@progbits
	btrq %r15, (,%rsi,2)
	.section .t5388,"ax",@progbits
	btrw %r14w, 0x40(%rip)
	.section .t5389,"ax",@progbits
	btrl %r13d, 0x40(%rip)
	.section .t5390,"ax",@progbits
	btrl %r14d, 0x40(%rip)
	.section .t5391,"ax",@progbits
	btrq %r15, 0x40(%rip)
	.section .t5392,"ax",@progbits
	btrw %r14w, -0x100(%rip)
	.section .t5393,"ax",@progbits
	btrl %r13d, -0x100(%rip)
	.section .t5394,"ax",@progbits
	btrl %r14d, -0x100(%rip)
	.section .t5395,"ax",@progbits
	btrq %r15, -0x100(%rip)
	.section .t5396,"ax",@progbits
	btrw %r14w, 0x1234
	.section .t5397,"ax",@progbits
	btrl %r13d, 0x1234
	.section .t5398,"ax",@progbits
	btrl %r14d, 0x1234
	.section .t5399,"ax",@progbits
	btrq %r15, 0x1234
	.section .t5400,"ax",@progbits
	btrw %r14w, (%r8)
	.section .t5401,"ax",@progbits
	btrl %r13d, (%r8)
	.section .t5402,"ax",@progbits
	btrl %r14d, (%r8)
	.section .t5403,"ax",@progbits
	btrq %r15, (%r8)
	.section .t5404,"ax",@progbits
	btrw %r14w, (%r12)
	.section .t5405,"ax",@progbits
	btrl %r13d, (%r12)
	.section .t5406,"ax",@progbits
	btrl %r14d, (%r12)
	.section .t5407,"ax",@progbits
	btrq %r15, (%r12)
	.section .t5408,"ax",@progbits
	btrw %r14w, 0x8(%r13)
	.section .t5409,"ax",@progbits
	btrl %r13d, 0x8(%r13)
	.section .t5410,"ax",@progbits
	btrl %r14d, 0x8(%r13)
	.section .t5411,"ax",@progbits
	btrq %r15, 0x8(%r13)
	.section .t5412,"ax",@progbits
	btrw %r14w, (%r8,%r15,2)
	.section .t5413,"ax",@progbits
	btrl %r13d, (%r8,%r15,2)
	.section .t5414,"ax",@progbits
	btrl %r14d, (%r8,%r15,2)
	.section .t5415,"ax",@progbits
	btrq %r15, (%r8,%r15,2)
	.section .t5416,"ax",@progbits
	btrw %r14w, (%rax,%r12,4)
	.section .t5417,"ax",@progbits
	btrl %r13d, (%rax,%r12,4)
	.section .t5418,"ax",@progbits
	btrl %r14d, (%rax,%r12,4)
	.section .t5419,"ax",@progbits
	btrq %r15, (%rax,%r12,4)
	.section .t5420,"ax",@progbits
	btrw %r14w, 0x100(%rbp)
	.section .t5421,"ax",@progbits
	btrl %r13d, 0x100(%rbp)
	.section .t5422,"ax",@progbits
	btrl %r14d, 0x100(%rbp)
	.section .t5423,"ax",@progbits
	btrq %r15, 0x100(%rbp)
	.section .t5424,"ax",@progbits
	btrw %r14w, (%rsp)
	.section .t5425,"ax",@progbits
	btrl %r13d, (%rsp)
	.section .t5426,"ax",@progbits
	btrl %r14d, (%rsp)
	.section .t5427,"ax",@progbits
	btrq %r15, (%rsp)
	.section .t5428,"ax",@progbits
	btrw %r14w, 0x10(%rsp,%rsi,4)
	.section .t5429,"ax",@progbits
	btrl %r13d, 0x10(%rsp,%rsi,4)
	.section .t5430,"ax",@progbits
	btrl %r14d, 0x10(%rsp,%rsi,4)
	.section .t5431,"ax",@progbits
	btrq %r15, 0x10(%rsp,%rsi,4)
	.section .t5432,"ax",@progbits
	btrw %r14w, %gs:0x10(%rcx)
	.section .t5433,"ax",@progbits
	btrl %r13d, %gs:0x10(%rcx)
	.section .t5434,"ax",@progbits
	btrl %r14d, %gs:0x10(%rcx)
	.section .t5435,"ax",@progbits
	btrq %r15, %gs:0x10(%rcx)
	.section .t5436,"ax",@progbits
	btrw %r14w, %fs:(%rax,%rsi,8)
	.section .t5437,"ax",@progbits
	btrl %r13d, %fs:(%rax,%rsi,8)
	.section .t5438,"ax",@progbits
	btrl %r14d, %fs:(%rax,%rsi,8)
	.section .t5439,"ax",@progbits
	btrq %r15, %fs:(%rax,%rsi,8)
	.section .t5440,"ax",@progbits
	btcw %r14w, (%rcx)
	.section .t5441,"ax",@progbits
	btcl %r13d, (%rcx)
	.section .t5442,"ax",@progbits
	btcl %r14d, (%rcx)
	.section .t5443,"ax",@progbits
	btcq %r15, (%rcx)
	.section .t5444,"ax",@progbits
	btcw %r14w, 0x10(%rcx)
	.section .t5445,"ax",@progbits
	btcl %r13d, 0x10(%rcx)
	.section .t5446,"ax",@progbits
	btcl %r14d, 0x10(%rcx)
	.section .t5447,"ax",@progbits
	btcq %r15, 0x10(%rcx)
	.section .t5448,"ax",@progbits
	btcw %r14w, -0x8(%rbp)
	.section .t5449,"ax",@progbits
	btcl %r13d, -0x8(%rbp)
	.section .t5450,"ax",@progbits
	btcl %r14d, -0x8(%rbp)
	.section .t5451,"ax",@progbits
	btcq %r15, -0x8(%rbp)
	.section .t5452,"ax",@progbits
	btcw %r14w, 0x12345(%rcx)
	.section .t5453,"ax",@progbits
	btcl %r13d, 0x12345(%rcx)
	.section .t5454,"ax",@progbits
	btcl %r14d, 0x12345(%rcx)
	.section .t5455,"ax",@progbits
	btcq %r15, 0x12345(%rcx)
	.section .t5456,"ax",@progbits
	btcw %r14w, (%rax,%rsi,4)
	.section .t5457,"ax",@progbits
	btcl %r13d, (%rax,%rsi,4)
	.section .t5458,"ax",@progbits
	btcl %r14d, (%rax,%rsi,4)
	.section .t5459,"ax",@progbits
	btcq %r15, (%rax,%rsi,4)
	.section .t5460,"ax",@progbits
	btcw %r14w, 0x10(%rax,%rsi,8)
	.section .t5461,"ax",@progbits
	btcl %r13d, 0x10(%rax,%rsi,8)
	.section .t5462,"ax",@progbits
	btcl %r14d, 0x10(%rax,%rsi,8)
	.section .t5463,"ax",@progbits
	btcq %r15, 0x10(%rax,%rsi,8)
	.section .t5464,"ax",@progbits
	btcw %r14w, (,%rsi,2)
	.section .t5465,"ax",@progbits
	btcl %r13d, (,%rsi,2)
	.section .t5466,"ax",@progbits
	btcl %r14d, (,%rsi,2)
	.section .t5467,"ax",@progbits
	btcq %r15, (,%rsi,2)
	.section .t5468,"ax",@progbits
	btcw %r14w, 0x40(%rip)
	.section .t5469,"ax",@progbits
	btcl %r13d, 0x40(%rip)
	.section .t5470,"ax",@progbits
	btcl %r14d, 0x40(%rip)
	.section .t5471,"ax",@progbits
	btcq %r15, 0x40(%rip)
	.section .t5472,"ax",@progbits
	btcw %r14w, -0x100(%rip)
	.section .t5473,"ax",@progbits
	btcl %r13d, -0x100(%rip)
	.section .t5474,"ax",@progbits
	btcl %r14d, -0x100(%rip)
	.section .t5475,"ax",@progbits
	btcq %r15, -0x100(%rip)
	.section .t5476,"ax",@progbits
	btcw %r14w, 0x1234
	.section .t5477,"ax",@progbits
	btcl %r13d, 0x1234
	.section .t5478,"ax",@progbits
	btcl %r14d, 0x1234
	.section .t5479,"ax",@progbits
	btcq %r15, 0x1234
	.section .t5480,"ax",@progbits
	btcw %r14w, (%r8)
	.section .t5481,"ax",@progbits
	btcl %r13d, (%r8)
	.section .t5482,"ax",@progbits
	btcl %r14d, (%r8)
	.section .t5483,"ax",@progbits
	btcq %r15, (%r8)
	.section .t5484,"ax",@progbits
	btcw %r14w, (%r12)
	.section .t5485,"ax",@progbits
	btcl %r13d, (%r12)
	.section .t5486,"ax",@progbits
	btcl %r14d, (%r12)
	.section .t5487,"ax",@progbits
	btcq %r15, (%r12)
	.section .t5488,"ax",@progbits
	btcw %r14w, 0x8(%r13)
	.section .t5489,"ax",@progbits
	btcl %r13d, 0x8(%r13)
	.section .t5490,"ax",@progbits
	btcl %r14d, 0x8(%r13)
	.section .t5491,"ax",@progbits
	btcq %r15, 0x8(%r13)
	.section .t5492,"ax",@progbits
	btcw %r14w, (%r8,%r15,2)
	.section .t5493,"ax",@progbits
	btcl %r13d, (%r8,%r15,2)
	.section .t5494,"ax",@progbits
	btcl %r14d, (%r8,%r15,2)
	.section .t5495,"ax",@progbits
	btcq %r15, (%r8,%r15,2)
	.section .t5496,"ax",@progbits
	btcw %r14w, (%rax,%r12,4)
	.section .t5497,"ax",@progbits
	btcl %r13d, (%rax,%r12,4)
	.section .t5498,"ax",@progbits
	btcl %r14d, (%rax,%r12,4)
	.section .t5499,"ax",@progbits
	btcq %r15, (%rax,%r12,4)
	.section .t5500,"ax",@progbits
	btcw %r14w, 0x100(%rbp)
	.section .t5501,"ax",@progbits
	btcl %r13d, 0x100(%rbp)
	.section .t5502,"ax",@progbits
	btcl %r14d, 0x100(%rbp)
	.section .t5503,"ax",@progbits
	btcq %r15, 0x100(%rbp)
	.section .t5504,"ax",@progbits
	btcw %r14w, (%rsp)
	.section .t5505,"ax",@progbits
	btcl %r13d, (%rsp)
	.section .t5506,"ax",@progbits
	btcl %r14d, (%rsp)
	.section .t5507,"ax",@progbits
	btcq %r15, (%rsp)
	.section .t5508,"ax",@progbits
	btcw %r14w, 0x10(%rsp,%rsi,4)
	.section .t5509,"ax",@progbits
	btcl %r13d, 0x10(%rsp,%rsi,4)
	.section .t5510,"ax",@progbits
	btcl %r14d, 0x10(%rsp,%rsi,4)
	.section .t5511,"ax",@progbits
	btcq %r15, 0x10(%rsp,%rsi,4)
	.section .t5512,"ax",@progbits
	btcw %r14w, %gs:0x10(%rcx)
	.section .t5513,"ax",@progbits
	btcl %r13d, %gs:0x10(%rcx)
	.section .t5514,"ax",@progbits
	btcl %r14d, %gs:0x10(%rcx)
	.section .t5515,"ax",@progbits
	btcq %r15, %gs:0x10(%rcx)
	.section .t5516,"ax",@progbits
	btcw %r14w, %fs:(%rax,%rsi,8)
	.section .t5517,"ax",@progbits
	btcl %r13d, %fs:(%rax,%rsi,8)
	.section .t5518,"ax",@progbits
	btcl %r14d, %fs:(%rax,%rsi,8)
	.section .t5519,"ax",@progbits
	btcq %r15, %fs:(%rax,%rsi,8)
	.section .t5520,"ax",@progbits
	lock addl %edx, (%rcx)
	.section .t5521,"ax",@progbits
	lock andl $0x7, (%rcx)
	.section .t5522,"ax",@progbits
	lock addl %edx, 0x10(%rcx)
	.section .t5523,"ax",@progbits
	lock andl $0x7, 0x10(%rcx)
	.section .t5524,"ax",@progbits
	lock addl %edx, -0x8(%rbp)
	.section .t5525,"ax",@progbits
	lock andl $0x7, -0x8(%rbp)
	.section .t5526,"ax",@progbits
	lock addl %edx, 0x12345(%rcx)
	.section .t5527,"ax",@progbits
	lock andl $0x7, 0x12345(%rcx)
	.section .t5528,"ax",@progbits
	lock addl %edx, (%rax,%rsi,4)
	.section .t5529,"ax",@progbits
	lock andl $0x7, (%rax,%rsi,4)
	.section .t5530,"ax",@progbits
	lock addl %edx, 0x10(%rax,%rsi,8)
	.section .t5531,"ax",@progbits
	lock andl $0x7, 0x10(%rax,%rsi,8)
	.section .t5532,"ax",@progbits
	lock addl %edx, (,%rsi,2)
	.section .t5533,"ax",@progbits
	lock andl $0x7, (,%rsi,2)
	.section .t5534,"ax",@progbits
	lock addl %edx, 0x40(%rip)
	.section .t5535,"ax",@progbits
	lock andl $0x7, 0x40(%rip)
	.section .t5536,"ax",@progbits
	lock addl %edx, -0x100(%rip)
	.section .t5537,"ax",@progbits
	lock andl $0x7, -0x100(%rip)
	.section .t5538,"ax",@progbits
	lock addl %edx, 0x1234
	.section .t5539,"ax",@progbits
	lock andl $0x7, 0x1234
	.section .t5540,"ax",@progbits
	lock addl %edx, (%r8)
	.section .t5541,"ax",@progbits
	lock andl $0x7, (%r8)
	.section .t5542,"ax",@progbits
	lock addl %edx, (%r12)
	.section .t5543,"ax",@progbits
	lock andl $0x7, (%r12)
	.section .t5544,"ax",@progbits
	lock addl %edx, 0x8(%r13)
	.section .t5545,"ax",@progbits
	lock andl $0x7, 0x8(%r13)
	.section .t5546,"ax",@progbits
	lock addl %edx, (%r8,%r15,2)
	.section .t5547,"ax",@progbits
	lock andl $0x7, (%r8,%r15,2)
	.section .t5548,"ax",@progbits
	lock addl %edx, (%rax,%r12,4)
	.section .t5549,"ax",@progbits
	lock andl $0x7, (%rax,%r12,4)
	.section .t5550,"ax",@progbits
	lock addl %edx, 0x100(%rbp)
	.section .t5551,"ax",@progbits
	lock andl $0x7, 0x100(%rbp)
	.section .t5552,"ax",@progbits
	lock addl %edx, (%rsp)
	.section .t5553,"ax",@progbits
	lock andl $0x7, (%rsp)
	.section .t5554,"ax",@progbits
	lock addl %edx, 0x10(%rsp,%rsi,4)
	.section .t5555,"ax",@progbits
	lock andl $0x7, 0x10(%rsp,%rsi,4)
	.section .t5556,"ax",@progbits
	lock addl %edx, %gs:0x10(%rcx)
	.section .t5557,"ax",@progbits
	lock andl $0x7, %gs:0x10(%rcx)
	.section .t5558,"ax",@progbits
	lock addl %edx, %fs:(%rax,%rsi,8)
	.section .t5559,"ax",@progbits
	lock andl $0x7, %fs:(%rax,%rsi,8)
	.section .t7336,"ax",@progbits
	movl %edx, %ecx
	.section .t7337,"ax",@progbits
	addl %edx, %ecx
	.section .t7338,"ax",@progbits
	btsl $3, %ecx
	.section .t7339,"ax",@progbits
	movb %ah, (%rcx)
	.section .t7340,"ax",@progbits
	addb %ch, (%rcx)
	.section .t7341,"ax",@progbits
	testb %dh, (%rcx)
	.section .t7342,"ax",@progbits
	xchgb %bh, (%rcx)
	.section .t7343,"ax",@progbits
	movq %rsp, (%rcx)
	.section .t7344,"ax",@progbits
	addq %rsp, (%rcx)
	.section .t7345,"ax",@progbits
	testq %rsp, (%rcx)
	.section .t7346,"ax",@progbits
	xchgq %rsp, (%rcx)
	.section .t7347,"ax",@progbits
	movq (%rcx), %rsp
	.section .t7348,"ax",@progbits
	movzbq (%rcx), %rsp
	.section .t7349,"ax",@progbits
	callq *(%rax)
	.section .t7350,"ax",@progbits
	jmpq *(%rax)
	.section .t7351,"ax",@progbits
	pushq (%rax)
	.section .t7352,"ax",@progbits
	notl (%rcx)
	.section .t7353,"ax",@progbits
	negl (%rcx)
	.section .t7354,"ax",@progbits
	mull (%rcx)
	.section .t7355,"ax",@progbits
	divl (%rcx)
	.section .t7356,"ax",@progbits
	adcl %edx, (%rcx)
	.section .t7357,"ax",@progbits
	sbbl %edx, (%rcx)
	.section .t7358,"ax",@progbits
	adcl $0x7, (%rcx)
	.section .t7359,"ax",@progbits
	addl (%rcx), %edx
	.section .t7360,"ax",@progbits
	andl (%rcx), %edx
	.section .t7361,"ax",@progbits
	cmpl (%rcx), %edx
	.section .t7362,"ax",@progbits
	btsl $40, (%rcx)
	.section .t7363,"ax",@progbits
	btl $32, (%rcx)
	.section .t7364,"ax",@progbits
	btrw $20, (%rcx)
	.section .t7365,"ax",@progbits
	btcq $64, (%rcx)
	.section .t7366,"ax",@progbits
	btsl $200, (%rcx)
	.section .t7368,"ax",@progbits
	btl %edi, (%rcx)
	.section .t7369,"ax",@progbits
	btsq %rdi, (%rcx)
	.section .t7370,"ax",@progbits
	btrl %r11d, (%rcx)
	.section .t7371,"ax",@progbits
	btcw %r13w, (%rcx)
	.section .t7372,"ax",@progbits
	btsq %r11, (%rcx)
	.section .t7373,"ax",@progbits
	lcallq *(%rax)
	.section .t7374,"ax",@progbits
	ljmpq *(%rax)
	.section .t7375,"ax",@progbits
	xaddq %rsp, (%rcx)
	.section .t7376,"ax",@progbits
	xaddb %ah, (%rcx)
	.section .t7377,"ax",@progbits
	cmpxchgq %rsp, (%rcx)
	.section .t7378,"ax",@progbits
	cmpxchgb %ah, (%rcx)
	.section .t7379,"ax",@progbits
	btsl %esp, (%rcx)
	.section .t7380,"ax",@progbits
	btq %rsp, (%rcx)
	.section .t7382,"ax",@progbits
	movl %edx, (%rcx)
	.section .t7383,"ax",@progbits
	movq %rdx, (%rcx)
	.section .t7384,"ax",@progbits
	movb %dl, (%rcx)
	.section .t7385,"ax",@progbits
	movl $0x12345678, (%rcx)
	.section .t7386,"ax",@progbits
	movq $-1, (%rcx)
	.section .t7387,"ax",@progbits
	movl (%rcx), %edx
	.section .t7388,"ax",@progbits
	movb (%rcx), %dl
	.section .t7389,"ax",@progbits
	movzbl (%rcx), %edx
	.section .t7390,"ax",@progbits
	movsbq (%rcx), %rdx
	.section .t7391,"ax",@progbits
	movswl (%rcx), %edx
	.section .t7392,"ax",@progbits
	movzwq (%rcx), %rdx
	.section .t7393,"ax",@progbits
	xchgl %edx, (%rcx)
	.section .t7394,"ax",@progbits
	andl %edx, (%rcx)
	.section .t7395,"ax",@progbits
	orq %rdx, (%rcx)
	.section .t7396,"ax",@progbits
	xorw %dx, (%rcx)
	.section .t7397,"ax",@progbits
	addl %edx, (%rcx)
	.section .t7398,"ax",@progbits
	subb %dl, (%rcx)
	.section .t7399,"ax",@progbits
	addq $-1, (%rcx)
	.section .t7400,"ax",@progbits
	andl $0x0f0f0f0f, (%rcx)
	.section .t7401,"ax",@progbits
	cmpl $0x10, (%rcx)
	.section .t7402,"ax",@progbits
	cmpq $-1, (%rcx)
	.section .t7403,"ax",@progbits
	cmpb %dl, (%rcx)
	.section .t7404,"ax",@progbits
	cmpl %edx, (%rcx)
	.section .t7405,"ax",@progbits
	cmpq %rdx, (%rcx)
	.section .t7406,"ax",@progbits
	cmpw %r9w, (%rcx)
	.section .t7407,"ax",@progbits
	testl %edx, (%rcx)
	.section .t7408,"ax",@progbits
	testq $-1, (%rcx)
	.section .t7409,"ax",@progbits
	btl $3, (%rcx)
	.section .t7410,"ax",@progbits
	btsl $31, (%rcx)
	.section .t7411,"ax",@progbits
	btrq $63, (%rcx)
	.section .t7412,"ax",@progbits
	btcw $9, (%rcx)
	.section .t7413,"ax",@progbits
	btl %r14d, (%rcx)
	.section .t7414,"ax",@progbits
	btsl %r13d, (%rcx)
	.section .t7415,"ax",@progbits
	btrq %r15, (%rcx)
	.section .t7416,"ax",@progbits
	btcw %r14w, (%rcx)
	.section .t7417,"ax",@progbits
	btq %r13, (%rcx)
	.section .t7418,"ax",@progbits
	incb (%rcx)
	.section .t7419,"ax",@progbits
	incw (%rcx)
	.section .t7420,"ax",@progbits
	incl (%rcx)
	.section .t7421,"ax",@progbits
	incq (%rcx)
	.section .t7422,"ax",@progbits
	decb (%rcx)
	.section .t7423,"ax",@progbits
	decl (%rcx)
	.section .t7424,"ax",@progbits
	decq (%rcx)
	.section .t7425,"ax",@progbits
	xaddb %dl, (%rcx)
	.section .t7426,"ax",@progbits
	xaddw %r9w, (%rcx)
	.section .t7427,"ax",@progbits
	xaddl %edx, (%rcx)
	.section .t7428,"ax",@progbits
	xaddq %rdx, (%rcx)
	.section .t7429,"ax",@progbits
	cmpxchgb %dl, (%rcx)
	.section .t7430,"ax",@progbits
	cmpxchgw %r9w, (%rcx)
	.section .t7431,"ax",@progbits
	cmpxchgl %edx, (%rcx)
	.section .t7432,"ax",@progbits
	cmpxchgq %rdx, (%rcx)
	.section .t7433,"ax",@progbits
	cmpxchgl %r13d, (%rcx)
