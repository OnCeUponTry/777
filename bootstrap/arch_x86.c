/* arch_x86.c: backend AMD64 para compilador_777.c.
 * Provee TODO lo arch-specific (bytes maquina, ELF machine type, layout).
 * compilador_777.c es arch-agnostic y llama estas funciones via interface.
 *
 * Para portar a otra arquitectura (ARM, RISC-V): crear arch_arm.c / arch_riscv.c
 * con misma interface. compilador_777.c NO requiere cambios.
 *
 * Interface arch_*() expuesta:
 *   arch_e_machine()             -> ELF e_machine value (62 para AMD64)
 *   arch_base_vaddr()            -> base virtual addr para PT_LOAD
 *   arch_handler_size(idx)       -> tamano del handler idx en bytes
 *   arch_handler_ptr(idx)        -> ptr a array de bytes del handler
 *   arch_handler_is_halt(idx)    -> 1 si handler es HALT (necesita patch jmp-to-tail)
 *   arch_handler_is_exit(idx)    -> 1 si handler termina con syscall exit (sin jmp back)
 *   arch_setup_size()            -> bytes del prologo (xor r12 + setup r15/rbp)
 *   arch_dispatch_size()         -> bytes del dispatch loop
 *   arch_tail_size()             -> bytes del epilogo (syscall exit con r12)
 *   arch_jmp_back_size()         -> bytes del jmp post-handler a dispatch (5 en x86 = E9+disp32)
 *   arch_emit_setup(buf, bc_disp, vars_disp)
 *   arch_emit_dispatch(buf, jt_disp)
 *   arch_emit_tail(buf)
 *   arch_emit_jmp_back(buf, disp)   - jmp near to dispatch from end of handler
 *   arch_emit_halt_jmp(buf, disp)   - patch HALT marker con jmp near to tail
 *   arch_jt_entry_bytes()           -> bytes por entry de jump table (8 en amd64)
 */

/* === Handler bytes (instructions AMD64) === */

/* HALT: marker 2 bytes, reemplazado por arch_emit_halt_jmp(buf, disp) -> 5 bytes. */
int h_halt[] = {0xEB, 0x00};

/* LOAD_IMM32: movsxd rax,[r15]; add r15,4; push rax. */
int h_load_imm32[] = {0x49, 0x63, 0x07, 0x49, 0x83, 0xC7, 0x04, 0x50};

/* ADD: pop rax; pop rcx; add rax,rcx; push rax. */
int h_add[] = {0x58, 0x59, 0x48, 0x01, 0xC8, 0x50};

/* SUB: pop rcx; pop rax; sub rax,rcx; push. */
int h_sub[] = {0x59, 0x58, 0x48, 0x29, 0xC8, 0x50};

/* MUL: pop 2; imul rax,rcx; push. */
int h_mul[] = {0x58, 0x59, 0x48, 0x0F, 0xAF, 0xC1, 0x50};

/* ADD_R12: pop rax; add r12,rax. */
int h_add_r12[] = {0x58, 0x49, 0x01, 0xC4};

/* LOAD_VAR: movzx rax,byte[r15]; inc r15; push qword[rbp+rax*8]. (1-byte slot, compat) */
int h_load_var[] = {0x49, 0x0F, 0xB6, 0x07, 0x49, 0xFF, 0xC7, 0xFF, 0x74, 0xC5, 0x00};

/* STORE_VAR: pop qword[rbp+rax*8]. (1-byte slot, compat) */
int h_store_var[] = {0x49, 0x0F, 0xB6, 0x07, 0x49, 0xFF, 0xC7, 0x8F, 0x44, 0xC5, 0x00};

/* JMP rel16: movsx rax,word[r15]; add r15,2; add r15,rax. */
int h_jmp[] = {0x49, 0x0F, 0xBF, 0x07, 0x49, 0x83, 0xC7, 0x02, 0x49, 0x01, 0xC7};

/* JZ rel16 */
int h_jz[] = {0x59, 0x49, 0x0F, 0xBF, 0x07, 0x49, 0x83, 0xC7, 0x02, 0x48, 0x85, 0xC9, 0x75, 0x03, 0x49, 0x01, 0xC7};

/* EQ */
int h_eq[] = {0x59, 0x58, 0x48, 0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x48, 0x0F, 0xB6, 0xC0, 0x50};

/* LT */
int h_lt[] = {0x59, 0x58, 0x48, 0x39, 0xC8, 0x0F, 0x9C, 0xC0, 0x48, 0x0F, 0xB6, 0xC0, 0x50};

/* FD_READ */
int h_fd_read[] = {0x5A, 0x5E, 0x5F, 0x48, 0x31, 0xC0, 0x0F, 0x05, 0x50};

/* FD_WRITE */
int h_fd_write[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* ALLOC */
int h_alloc[] = {0x5F, 0x48, 0xC7, 0xC0, 0x09, 0x00, 0x00, 0x00,
                 0x48, 0x89, 0xFE, 0x48, 0x31, 0xFF,
                 0x48, 0xC7, 0xC2, 0x03, 0x00, 0x00, 0x00,
                 0x49, 0xC7, 0xC2, 0x22, 0x00, 0x00, 0x00,
                 0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
                 0x4D, 0x31, 0xC9, 0x0F, 0x05, 0x50};

/* LOAD_BYTE: pop off; pop ptr; movzx rax,byte[rcx+rax]; push. */
int h_load_byte[] = {0x58, 0x59, 0x48, 0x0F, 0xB6, 0x04, 0x01, 0x50};

/* STORE_BYTE */
int h_store_byte[] = {0x58, 0x5A, 0x59, 0x88, 0x04, 0x11};

/* LOAD_I64 */
int h_load_i64[] = {0x58, 0x59, 0x48, 0x8B, 0x04, 0x01, 0x50};

/* STORE_I64 */
int h_store_i64[] = {0x58, 0x5A, 0x59, 0x48, 0x89, 0x04, 0x11};

/* EXIT_TOP: pop rdi; mov rax,60; syscall. */
int h_exit_top[] = {0x5F, 0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00, 0x0F, 0x05};

/* SHR */
int h_shr[] = {0x59, 0x58, 0x48, 0xD3, 0xE8, 0x50};

/* BITAND */
int h_bitand[] = {0x59, 0x58, 0x48, 0x21, 0xC8, 0x50};

/* LOAD_IMM64 */
int h_load_imm64[] = {0x49, 0x8B, 0x07, 0x49, 0x83, 0xC7, 0x08, 0x50};

/* DIV */
int h_div[] = {0x59, 0x58, 0x48, 0x99, 0x48, 0xF7, 0xF9, 0x50};

/* MOD */
int h_mod[] = {0x59, 0x58, 0x48, 0x99, 0x48, 0xF7, 0xF9, 0x52};

/* BITOR */
int h_bitor[] = {0x59, 0x58, 0x48, 0x09, 0xC8, 0x50};

/* SHL */
int h_shl[] = {0x59, 0x58, 0x48, 0xD3, 0xE0, 0x50};

/* GT */
int h_gt[] = {0x59, 0x58, 0x48, 0x39, 0xC8, 0x0F, 0x9F, 0xC0, 0x48, 0x0F, 0xB6, 0xC0, 0x50};

/* BITXOR */
int h_bitxor[] = {0x59, 0x58, 0x48, 0x31, 0xC8, 0x50};

/* CALL rel16: movsx rax,word[r15]; add r15,2; push r15; add r15,rax. */
int h_call[] = {0x49, 0x0F, 0xBF, 0x07, 0x49, 0x83, 0xC7, 0x02, 0x41, 0x57, 0x49, 0x01, 0xC7};

/* RET: pop r15. */
int h_ret[] = {0x41, 0x5F};

/* OPEN: pop rsi (flags); pop rdi (pathname); rax=2; xor rdx,rdx; syscall; push rax. */
int h_open[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00,
                0x48, 0x31, 0xD2, 0x0F, 0x05, 0x50};

/* CLOSE: pop rdi (fd); rax=3; syscall; push rax. */
int h_close[] = {0x5F, 0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* IOCTL: pop rdx (argp); pop rsi (request); pop rdi (fd); rax=16; syscall; push rax. */
int h_ioctl[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x10, 0x00, 0x00, 0x00,
                 0x0F, 0x05, 0x50};

/* NANOSLEEP: pop rsi (rem_ptr); pop rdi (req_ptr); rax=35; syscall; push rax. */
int h_nanosleep[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x23, 0x00, 0x00, 0x00,
                     0x0F, 0x05, 0x50};

/* POLL: pop rdx (timeout); pop rsi (nfds); pop rdi (fds_ptr); rax=7; syscall; push rax. */
int h_poll[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x07, 0x00, 0x00, 0x00,
                0x0F, 0x05, 0x50};

/* FORK: rax=57; syscall; push rax. */
int h_fork[] = {0x48, 0xC7, 0xC0, 0x39, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* EXECVE: pop rdx (envp); pop rsi (argv); pop rdi (filename); rax=59; syscall; push rax. */
int h_execve[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x3B, 0x00, 0x00, 0x00,
                  0x0F, 0x05, 0x50};

/* WAIT4: pop r10 (rusage); pop rdx (options); pop rsi (status); pop rdi (pid); rax=61; syscall; push. */
int h_wait4[] = {0x41, 0x5A, 0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x3D, 0x00, 0x00, 0x00,
                 0x0F, 0x05, 0x50};

/* PIPE: pop rdi (fildes_ptr); rax=22; syscall; push rax. */
int h_pipe[] = {0x5F, 0x48, 0xC7, 0xC0, 0x16, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* DUP2: pop rsi (newfd); pop rdi (oldfd); rax=33; syscall; push rax. */
int h_dup2[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x21, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* === Lote 1 Sockets (Sub-decision F: network base) === */

/* SOCKET: pop rdx (protocol); pop rsi (type); pop rdi (domain); rax=41; syscall; push rax (fd o -errno). */
int h_socket[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x29, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* CONNECT: pop rdx (addrlen); pop rsi (addr_ptr); pop rdi (fd); rax=42; syscall; push rax. */
int h_connect[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* BIND: pop rdx (addrlen); pop rsi (addr_ptr); pop rdi (fd); rax=49; syscall; push rax. */
int h_bind[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x31, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* LISTEN: pop rsi (backlog); pop rdi (fd); rax=50; syscall; push rax. */
int h_listen[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x32, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* ACCEPT: pop rdx (addrlen_ptr); pop rsi (addr_ptr); pop rdi (fd); rax=43; syscall; push rax (new_fd o -errno). */
int h_accept[] = {0x5A, 0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x2B, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* === Lote 2 Memoria avanzada (Sub-decision F: framebuffer + zero-copy) === */

/* MMAP_FD: pop r9 (offset); pop r8 (fd); pop r10 (flags); pop rdx (prot); pop rsi (len);
 * xor rdi (addr=NULL kernel elige); rax=9; syscall; push rax (addr o -errno). */
int h_mmap_fd[] = {0x41, 0x59, 0x41, 0x58, 0x41, 0x5A, 0x5A, 0x5E,
                   0x48, 0x31, 0xFF,
                   0x48, 0xC7, 0xC0, 0x09, 0x00, 0x00, 0x00,
                   0x0F, 0x05, 0x50};

/* MUNMAP: pop rsi (len); pop rdi (addr); rax=11; syscall; push rax. */
int h_munmap[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x0B, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* === Lote 3 Tiempo + Senales (timestamps + SIGWINCH resize TUI) === */

/* GETTIMEOFDAY: pop rsi (tz_ptr); pop rdi (tv_ptr); rax=96; syscall; push rax. */
int h_gettimeofday[] = {0x5E, 0x5F, 0x48, 0xC7, 0xC0, 0x60, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x50};

/* RT_SIGACTION: pop r10 (sigsetsize); pop rdx (oldact_ptr); pop rsi (act_ptr);
 * pop rdi (signum); rax=13; syscall; push rax. */
int h_rt_sigaction[] = {0x41, 0x5A, 0x5A, 0x5E, 0x5F,
                        0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,
                        0x0F, 0x05, 0x50};

/* === Lote 4 Slot 16-bit (2026-05-19): habilita slots >255 para self-host real === */

/* LOAD_VAR16: movzx rax,word[r15]; add r15,2; push qword[rbp+rax*8]. */
int h_load_var16[] = {0x49, 0x0F, 0xB7, 0x07, 0x49, 0x83, 0xC7, 0x02, 0xFF, 0x74, 0xC5, 0x00};

/* STORE_VAR16: movzx rax,word[r15]; add r15,2; pop qword[rbp+rax*8]. */
int h_store_var16[] = {0x49, 0x0F, 0xB7, 0x07, 0x49, 0x83, 0xC7, 0x02, 0x8F, 0x44, 0xC5, 0x00};

/* === Interface arch_*() === */

int arch_e_machine() { return 62; }       /* EM_X86_64 */
int arch_base_vaddr() { return 0x400000; } /* tipico Linux user space x86 */
int arch_jt_entry_bytes() { return 8; }    /* abs addr 8 bytes */

/* Bytes del prologo: xor r12,r12 (3) + lea r15,[rip+bc_disp] (7) + lea rbp,[rip+vars_disp] (7) = 17B */
int arch_setup_size() { return 17; }

/* Bytes del dispatch loop: movzx rax,byte[r15] (4) + inc r15 (3) + lea rcx (7) + jmp[rcx+rax*8] (3) = 17B */
int arch_dispatch_size() { return 17; }

/* Bytes del epilogo: mov rdi,r12 (3) + mov rax,60 (7) + syscall (2) = 12B */
int arch_tail_size() { return 12; }

/* Bytes del jmp near post-handler a dispatch: E9 + disp32 = 5B */
int arch_jmp_back_size() { return 5; }

/* idx == 0 es HALT marker (necesita patch a jmp-near-to-tail). */
int arch_handler_is_halt(int idx) { if(idx == 0) return 1; return 0; }

/* OP_EXIT_TOP (0x13 = 19): termina con syscall exit, no jmp back. */
int arch_handler_is_exit(int idx) { if(idx == 19) return 1; return 0; }

/* Sizes paralelos: M2-Planet no soporta sizeof() en arrays. Hardcoded por idx. */
int arch_handler_size(int idx)
{
    if(idx == 0) return 2;       /* halt marker (sera 5B post-patch) */
    if(idx == 1) return 8;       /* load_imm32 */
    if(idx == 2) return 6;       /* add */
    if(idx == 3) return 6;       /* sub */
    if(idx == 4) return 7;       /* mul */
    if(idx == 5) return 4;       /* add_r12 */
    if(idx == 6) return 11;      /* load_var (1-byte slot, compat) */
    if(idx == 7) return 11;      /* store_var (1-byte slot, compat) */
    if(idx == 8) return 11;      /* jmp */
    if(idx == 9) return 17;      /* jz */
    if(idx == 10) return 13;     /* eq */
    if(idx == 11) return 13;     /* lt */
    if(idx == 12) return 9;      /* fd_read */
    if(idx == 13) return 13;     /* fd_write */
    if(idx == 14) return 41;     /* alloc */
    if(idx == 15) return 8;      /* load_byte */
    if(idx == 16) return 6;      /* store_byte */
    if(idx == 17) return 7;      /* load_i64 */
    if(idx == 18) return 7;      /* store_i64 */
    if(idx == 19) return 10;     /* exit_top */
    if(idx == 20) return 6;      /* shr */
    if(idx == 21) return 6;      /* bitand */
    if(idx == 22) return 8;      /* load_imm64 */
    if(idx == 23) return 8;      /* div */
    if(idx == 24) return 8;      /* mod */
    if(idx == 25) return 6;      /* bitor */
    if(idx == 26) return 6;      /* shl */
    if(idx == 27) return 13;     /* gt */
    if(idx == 28) return 6;      /* bitxor */
    if(idx == 29) return 13;     /* call */
    if(idx == 30) return 2;      /* ret */
    if(idx == 31) return 15;     /* open */
    if(idx == 32) return 11;     /* close */
    if(idx == 33) return 13;     /* ioctl */
    if(idx == 34) return 12;     /* nanosleep */
    if(idx == 35) return 13;     /* poll */
    if(idx == 36) return 10;     /* fork */
    if(idx == 37) return 13;     /* execve */
    if(idx == 38) return 15;     /* wait4 */
    if(idx == 39) return 11;     /* pipe */
    if(idx == 40) return 12;     /* dup2 */
    if(idx == 41) return 13;     /* socket */
    if(idx == 42) return 13;     /* connect */
    if(idx == 43) return 13;     /* bind */
    if(idx == 44) return 12;     /* listen */
    if(idx == 45) return 13;     /* accept */
    if(idx == 46) return 21;     /* mmap_fd */
    if(idx == 47) return 12;     /* munmap */
    if(idx == 48) return 12;     /* gettimeofday */
    if(idx == 49) return 15;     /* rt_sigaction */
    if(idx == 50) return 12;     /* load_var16 (2-byte slot) */
    if(idx == 51) return 12;     /* store_var16 (2-byte slot) */
    return 0;
}

int* arch_handler_ptr(int idx)
{
    if(idx == 0) return h_halt;
    if(idx == 1) return h_load_imm32;
    if(idx == 2) return h_add;
    if(idx == 3) return h_sub;
    if(idx == 4) return h_mul;
    if(idx == 5) return h_add_r12;
    if(idx == 6) return h_load_var;
    if(idx == 7) return h_store_var;
    if(idx == 8) return h_jmp;
    if(idx == 9) return h_jz;
    if(idx == 10) return h_eq;
    if(idx == 11) return h_lt;
    if(idx == 12) return h_fd_read;
    if(idx == 13) return h_fd_write;
    if(idx == 14) return h_alloc;
    if(idx == 15) return h_load_byte;
    if(idx == 16) return h_store_byte;
    if(idx == 17) return h_load_i64;
    if(idx == 18) return h_store_i64;
    if(idx == 19) return h_exit_top;
    if(idx == 20) return h_shr;
    if(idx == 21) return h_bitand;
    if(idx == 22) return h_load_imm64;
    if(idx == 23) return h_div;
    if(idx == 24) return h_mod;
    if(idx == 25) return h_bitor;
    if(idx == 26) return h_shl;
    if(idx == 27) return h_gt;
    if(idx == 28) return h_bitxor;
    if(idx == 29) return h_call;
    if(idx == 30) return h_ret;
    if(idx == 31) return h_open;
    if(idx == 32) return h_close;
    if(idx == 33) return h_ioctl;
    if(idx == 34) return h_nanosleep;
    if(idx == 35) return h_poll;
    if(idx == 36) return h_fork;
    if(idx == 37) return h_execve;
    if(idx == 38) return h_wait4;
    if(idx == 39) return h_pipe;
    if(idx == 40) return h_dup2;
    if(idx == 41) return h_socket;
    if(idx == 42) return h_connect;
    if(idx == 43) return h_bind;
    if(idx == 44) return h_listen;
    if(idx == 45) return h_accept;
    if(idx == 46) return h_mmap_fd;
    if(idx == 47) return h_munmap;
    if(idx == 48) return h_gettimeofday;
    if(idx == 49) return h_rt_sigaction;
    if(idx == 50) return h_load_var16;
    if(idx == 51) return h_store_var16;
    return h_halt;
}

/* Emite bytes del prologo en buf:
 *   xor r12,r12                 (3 bytes: 4D 31 E4)
 *   lea r15,[rip + bc_disp]    (7 bytes: 4C 8D 3D + disp32)
 *   lea rbp,[rip + vars_disp]  (7 bytes: 48 8D 2D + disp32)
 * Total: 17 bytes.
 *
 * Los disps son relativos al post-instruction RIP.
 * Caller calcula bc_disp = bc_abs - (vm_base + 10),  vars_disp = vars_abs - (vm_base + 17).
 */
void arch_emit_setup(char* buf, int bc_disp, int vars_disp)
{
    buf[0] = 0x4D;
    buf[1] = 0x31;
    buf[2] = 0xE4;
    buf[3] = 0x4C;
    buf[4] = 0x8D;
    buf[5] = 0x3D;
    buf[6] = bc_disp;
    buf[7] = bc_disp >> 8;
    buf[8] = bc_disp >> 16;
    buf[9] = bc_disp >> 24;
    buf[10] = 0x48;
    buf[11] = 0x8D;
    buf[12] = 0x2D;
    buf[13] = vars_disp;
    buf[14] = vars_disp >> 8;
    buf[15] = vars_disp >> 16;
    buf[16] = vars_disp >> 24;
}

/* Emite dispatch loop en buf:
 *   movzx rax, byte[r15]    (4 bytes: 49 0F B6 07)
 *   inc r15                  (3 bytes: 49 FF C7)
 *   lea rcx, [rip + jt_disp] (7 bytes: 48 8D 0D + disp32)
 *   jmp [rcx + rax*8]        (3 bytes: FF 24 C1)
 * Total: 17 bytes.
 *
 * jt_disp es relativo al post-lea RIP (i.e., disp32 = jt_abs - (dloop_abs + 14)).
 */
void arch_emit_dispatch(char* buf, int jt_disp)
{
    buf[0] = 0x49;
    buf[1] = 0x0F;
    buf[2] = 0xB6;
    buf[3] = 0x07;
    buf[4] = 0x49;
    buf[5] = 0xFF;
    buf[6] = 0xC7;
    buf[7] = 0x48;
    buf[8] = 0x8D;
    buf[9] = 0x0D;
    buf[10] = jt_disp;
    buf[11] = jt_disp >> 8;
    buf[12] = jt_disp >> 16;
    buf[13] = jt_disp >> 24;
    buf[14] = 0xFF;
    buf[15] = 0x24;
    buf[16] = 0xC1;
}

/* Emite epilogo en buf:
 *   mov rdi, r12            (3 bytes: 4C 89 E7)
 *   mov rax, 60             (7 bytes: 48 C7 C0 3C 00 00 00)
 *   syscall                 (2 bytes: 0F 05)
 * Total: 12 bytes.
 */
void arch_emit_tail(char* buf)
{
    buf[0] = 0x4C;
    buf[1] = 0x89;
    buf[2] = 0xE7;
    buf[3] = 0x48;
    buf[4] = 0xC7;
    buf[5] = 0xC0;
    buf[6] = 0x3C;
    buf[7] = 0x00;
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x0F;
    buf[11] = 0x05;
}

/* Emite jmp near + disp32 (5 bytes: E9 + disp32). */
void arch_emit_jmp_back(char* buf, int disp)
{
    buf[0] = 0xE9;
    buf[1] = disp;
    buf[2] = disp >> 8;
    buf[3] = disp >> 16;
    buf[4] = disp >> 24;
}

/* Patch HALT marker (2 bytes) con jmp near to tail (5 bytes). Sobreescribe los 2 originales. */
void arch_emit_halt_jmp(char* buf, int disp)
{
    arch_emit_jmp_back(buf, disp);
}
