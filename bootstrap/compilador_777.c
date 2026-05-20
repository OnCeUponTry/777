/* compilador_777.c: compilador del lenguaje 777, escrito en C subset M2-Planet.
 *
 * Lee source .777m por stdin, emite ELF ejecutable a stdout.
 * Equivalente a compile_777m_to_bytecode + build_vm_elf del original Rust.
 *
 * Bootstrap: stage0-posix (357 bytes hex0 -> M2-Planet -> este .c -> ELF).
 * Self-replicating: este ELF compila V3_elf_source.777m -> ELF identico.
 *
 * En M2-Planet/amd64: int = 64-bit signed. Usado como i64/u64 indistintamente.
 */

/* === Forward declarations de M2libc === */
int read(int fd, char* buf, int count);
int write(int fd, char* buf, int count);
void exit(int code);

/* === Constantes opcodes VM === */
#define OP_HALT       0
#define OP_LOAD_IMM32 1
#define OP_ADD        2
#define OP_SUB        3
#define OP_MUL        4
#define OP_ADD_R12    5
#define OP_LOAD_VAR   6
#define OP_STORE_VAR  7
#define OP_JMP        8
#define OP_JZ         9
#define OP_EQ        10
#define OP_LT        11
#define OP_FD_READ   12
#define OP_FD_WRITE  13
#define OP_ALLOC     14
#define OP_LOAD_BYTE 15
#define OP_STORE_BYTE 16
#define OP_LOAD_I64  17
#define OP_STORE_I64 18
#define OP_EXIT_TOP  19
#define OP_SHR       20
#define OP_BITAND    21
#define OP_LOAD_IMM64 22
#define OP_DIV       23
#define OP_MOD       24
#define OP_BITOR     25
#define OP_SHL       26
#define OP_GT        27
#define OP_BITXOR    28
#define OP_CALL      29
#define OP_RET       30
#define OP_OPEN      31
#define OP_CLOSE     32
#define OP_IOCTL     33
#define OP_NANOSLEEP 34
#define OP_POLL      35
#define OP_FORK      36
#define OP_EXECVE    37
#define OP_WAIT4     38
#define OP_PIPE      39
#define OP_DUP2      40
/* Lote 1 sockets (network base) */
#define OP_SOCKET    41
#define OP_CONNECT   42
#define OP_BIND      43
#define OP_LISTEN    44
#define OP_ACCEPT    45
/* Lote 2 memoria avanzada (framebuffer + zero-copy) */
#define OP_MMAP_FD   46
#define OP_MUNMAP    47
/* Lote 3 tiempo + senales (timestamps + SIGWINCH resize TUI) */
#define OP_GETTIMEOFDAY 48
#define OP_RT_SIGACTION 49
/* Lote 4 (2026-05-19): slot 16-bit para habilitar self-host real con >256 slots */
#define OP_LOAD_VAR16  50
#define OP_STORE_VAR16 51

#define NUM_OPCODES  52
/* 2-byte slot index (Lote 4 2026-05-19): self-host real demando > 800 slots.
 * Expandido a 4096 slots con Lote 4b (2026-05-19). */
#define DROP_SLOT     4095  /* Slot dummy para pop sin store. */
#define RET_ADDR_BASE 3000  /* ret_addr slot de F slot S = (3000 + S). Max S=127 -> slot 3127. */
#define USER_SLOTS_MAX 3000 /* slots 0..2999 user. 3000..4095 reservados. */
#define N_VARS       4096
#define VARS_AREA_SIZE 32768          /* 4096 * 8 */
#define HEADER_TOTAL 120
/* BASE_VADDR removido: usar arch_base_vaddr() en su lugar (definido en arch_<X>.c). */

/* === Buffers globales === */
/* MAX_SOURCE expandido Lote 4 (2026-05-19): self-host .777m son ~110KB, antes 64KB lo truncaba. */
#define MAX_SOURCE   262144   /* 256 KB source max */
#define MAX_BYTECODE 131072   /* 128 KB bytecode max (era 32K, self-host emite ~17K, margen 8x) */
#define MAX_ELF      262144   /* 256 KB ELF max */

char g_source[MAX_SOURCE];
int g_source_len;

char g_bytecode[MAX_BYTECODE];
int g_bytecode_len;

char g_elf[MAX_ELF];
int g_elf_len;

/* Debug buffer global (M2-Planet falla con char arrays locales). */
char g_dbuf[32];

/* === String literals + interning Alt E.6.1 ===
 * Strings literales de `D ... Text "..."` se acumulan en str_data.
 * Cada string es lookup lineal (intern dedup automatico).
 * Fixups: posiciones en bytecode (LOAD_IMM64) que deben patchearse con abs addr.
 * Al final del compile: append str_data al bytecode + patch fixups con
 *   abs_addr = arch_base_vaddr() + HEADER_TOTAL + scaffold_size + bc_data_start + data_off.
 */
#define MAX_STR_DATA  8192        /* buffer total strings */
#define MAX_STRINGS   256         /* max strings distintos */
#define MAX_FIXUPS    256         /* max referencias a strings desde bytecode */

char str_data[MAX_STR_DATA];      /* bytes de strings concatenados */
char str_tmp[MAX_STR_DATA];       /* buffer temp para procesar escapes antes de intern */
int str_data_len;                 /* tamano usado actual */
int str_offsets[MAX_STRINGS];     /* offsets de cada string distinto en str_data */
int str_lens[MAX_STRINGS];        /* len de cada string distinto */
int str_count;                    /* cuantos strings distintos */
int fixup_bc[MAX_FIXUPS];         /* posicion del LOAD_IMM64 a patchear */
int fixup_data[MAX_FIXUPS];       /* offset en str_data del string referenciado */
int fixup_count;

/* === Functions table (Item 2: sintaxis FUNCTION/RET friendly) ===
 * Lookup name -> offset bytecode. Resuelve Call [name] a rel16 automatic.
 * Forward references soportadas via fixups patcheados post-parse.
 * Sintaxis: F <row> <col> <name> [params] registra function_offsets[slot]=bc_pos.
 *           Call [name] busca slot; si offset >= 0 emite rel inmediato;
 *           si offset == -1 emite placeholder 0x0000 y agrega fixup.
 */
#define MAX_FUNCTIONS 128   /* Lote 4 2026-05-19: 64 -> 128 para self-host (compilador en .777m tiene 74 F) */
#define MAX_FUNCTION_FIXUPS 512
#define MAX_FUNC_NAME_LEN 32   /* nombres funcion hasta 31 chars + null. M2-Planet no acepta expr en array size (bug 17) */
#define FUNC_NAMES_BUF_SIZE 4096   /* MAX_FUNCTIONS * MAX_FUNC_NAME_LEN = 128*32 */
#define MAX_PARAMS_PER_FN 8
#define FUNC_PARAMS_BUF_SIZE 1024  /* MAX_FUNCTIONS * MAX_PARAMS_PER_FN = 128*8 */
char function_names[FUNC_NAMES_BUF_SIZE];
int function_offsets[MAX_FUNCTIONS];
int function_count;
int function_fixup_bc[MAX_FUNCTION_FIXUPS];
int function_fixup_target[MAX_FUNCTION_FIXUPS];
int function_fixup_count;
/* function_params[fn_slot * MAX_PARAMS_PER_FN + i] = slot global del param i de fn_slot */
int function_params[FUNC_PARAMS_BUF_SIZE];
int function_param_count[MAX_FUNCTIONS];

/* === Labels table (Sub-decision E: Labels nombrados elimina rel16 manual) ===
 * Paralelo a functions: tabla nombre -> offset bytecode + fixups para forward refs.
 * Sintaxis: R 0 0 Label [name] registra label_offsets[slot] = g_bytecode_len.
 *           R 0 0 Jmp [name] o R 0 0 Jz [cond, name] resuelve auto via tabla.
 *           Si target NO definido aun (forward): emit placeholder + fixup.
 */
#define MAX_LABELS 512   /* Lote 4 2026-05-19: 64 -> 512 para self-host (compilador en .777m tiene ~422 labels unicos) */
#define MAX_LABEL_FIXUPS 1024
#define MAX_LABEL_NAME_LEN 32
#define LABEL_NAMES_BUF_SIZE 16384  /* 512 * 32 */
char label_names[LABEL_NAMES_BUF_SIZE];
int label_offsets[MAX_LABELS];
int label_count;
int label_fixup_bc[MAX_LABEL_FIXUPS];
int label_fixup_target[MAX_LABEL_FIXUPS];
int label_fixup_count;

/* pending_fn_jmp_pos: posicion del rel16 del JMP emitido al ver F que debe
 * patchearse al ver el siguiente Ret. -1 si no hay F abierta. */
int pending_fn_jmp_pos;
/* pending_fn_slot: slot de la F actual (para saber su param_count en Ret). -1 si no hay. */
int pending_fn_slot;
/* Sub-C reciclaje slots (2026-05-18): snapshot de sym_count al entrar F.
 * Al cerrar F (EndF / siguiente F / EOF) -> sym_count = fn_slot_alloc_start
 * libera todos los slots locales creados dentro F para reuso. */
int fn_slot_alloc_start;

/* Calcula scaffold_size = setup + dispatch + handlers (con jmps) + tail + JT + vars.
 * Replica logica de build_vm_elf para conocer donde empezara bytecode dentro del ELF.
 * Necesario para patch fixups con abs addr. */
int calc_scaffold_size()
{
    int cur;
    int i;
    int hbytes_len;
    int sz;
    cur = arch_setup_size() + arch_dispatch_size();
    i = 0;
    while(i < NUM_OPCODES)
    {
        hbytes_len = arch_handler_size(i);
        if(arch_handler_is_halt(i) != 0)
        {
            sz = arch_jmp_back_size();
        }
        else
        {
            if(arch_handler_is_exit(i) != 0)
            {
                sz = hbytes_len;
            }
            else
            {
                sz = hbytes_len + arch_jmp_back_size();
            }
        }
        cur = cur + sz;
        i = i + 1;
    }
    cur = cur + arch_tail_size();
    cur = cur + NUM_OPCODES * arch_jt_entry_bytes();
    cur = cur + VARS_AREA_SIZE;
    return cur;
}

/* Reset string state al inicio de compile. */
void str_init()
{
    int i;
    str_data_len = 0;
    str_count = 0;
    fixup_count = 0;
    i = 0;
    while(i < MAX_STR_DATA) { str_data[i] = 0; i = i + 1; }
}

/* Compara bytes en str_data[off..off+len] con bytes content[0..content_len]. */
int str_eq(int off, int len, char* content, int content_len)
{
    int i;
    if(len != content_len) return 0;
    i = 0;
    while(i < len)
    {
        if((str_data[off + i] & 0xFF) != (content[i] & 0xFF)) return 0;
        i = i + 1;
    }
    return 1;
}

/* Intern: busca content en str_data. Si existe retorna su offset.
 * Si no, append y retorna nuevo offset. Retorna -1 si no cabe. */
int str_intern(char* content, int content_len)
{
    int i;
    int off;
    /* Lookup */
    i = 0;
    while(i < str_count)
    {
        if(str_eq(str_offsets[i], str_lens[i], content, content_len) != 0)
        {
            return str_offsets[i];
        }
        i = i + 1;
    }
    /* No existe, agregar */
    if(str_count >= MAX_STRINGS) return 0 - 1;
    if(str_data_len + content_len > MAX_STR_DATA) return 0 - 1;
    off = str_data_len;
    i = 0;
    while(i < content_len)
    {
        str_data[off + i] = content[i] & 0xFF;
        i = i + 1;
    }
    str_data_len = str_data_len + content_len;
    str_offsets[str_count] = off;
    str_lens[str_count] = content_len;
    str_count = str_count + 1;
    return off;
}

/* Agrega fixup para patch posterior. Retorna 0 si OK, 1 si full. */
int str_add_fixup(int bc_pos, int data_off)
{
    if(fixup_count >= MAX_FIXUPS) return 1;
    fixup_bc[fixup_count] = bc_pos;
    fixup_data[fixup_count] = data_off;
    fixup_count = fixup_count + 1;
    return 0;
}

/* === Functions table helpers (Item 2) === */

void func_init()
{
    int i;
    i = 0;
    while(i < FUNC_NAMES_BUF_SIZE)
    {
        function_names[i] = 0;
        i = i + 1;
    }
    i = 0;
    while(i < MAX_FUNCTIONS)
    {
        function_offsets[i] = 0 - 1;
        function_param_count[i] = 0;
        i = i + 1;
    }
    i = 0;
    while(i < FUNC_PARAMS_BUF_SIZE)
    {
        function_params[i] = 0;
        i = i + 1;
    }
    function_count = 0;
    function_fixup_count = 0;
    pending_fn_jmp_pos = 0 - 1;
    pending_fn_slot = 0 - 1;
}

/* Compara name (len bytes) con function_names[slot]. Retorna 1 si igual. */
int func_name_eq(int slot, char* name, int len)
{
    int base;
    int i;
    base = slot * MAX_FUNC_NAME_LEN;
    i = 0;
    while(i < len)
    {
        if((function_names[base + i] & 0xFF) != (name[i] & 0xFF)) return 0;
        i = i + 1;
    }
    if((function_names[base + len] & 0xFF) != 0) return 0;
    return 1;
}

/* Busca o crea entrada para name. Retorna slot o -1 si tabla llena. */
int func_lookup_or_create(char* name, int len)
{
    int slot;
    int base;
    int i;
    if(len <= 0) return 0 - 1;
    if(len > MAX_FUNC_NAME_LEN - 1) return 0 - 1;
    slot = 0;
    while(slot < function_count)
    {
        if(func_name_eq(slot, name, len) != 0) return slot;
        slot = slot + 1;
    }
    if(function_count >= MAX_FUNCTIONS) return 0 - 1;
    slot = function_count;
    base = slot * MAX_FUNC_NAME_LEN;
    i = 0;
    while(i < len)
    {
        function_names[base + i] = name[i] & 0xFF;
        i = i + 1;
    }
    function_names[base + len] = 0;
    function_offsets[slot] = 0 - 1;
    function_count = function_count + 1;
    return slot;
}

/* Agrega fixup para Call forward reference. Retorna 0 OK, 1 si full. */
int func_add_fixup(int bc_pos, int target_slot)
{
    if(function_fixup_count >= MAX_FUNCTION_FIXUPS) return 1;
    function_fixup_bc[function_fixup_count] = bc_pos;
    function_fixup_target[function_fixup_count] = target_slot;
    function_fixup_count = function_fixup_count + 1;
    return 0;
}

/* Post-parse: patchea todos los fixups con rel16 calculado.
 * Si function_offsets[slot] == -1 (nunca definido): deja 0x0000 placeholder
 * (programa rompera en runtime, pero parser no aborta). */
void func_resolve_fixups()
{
    int i;
    int slot;
    int target_off;
    int call_post_off;
    int rel;
    int fixup_bc_pos;
    i = 0;
    while(i < function_fixup_count)
    {
        slot = function_fixup_target[i];
        fixup_bc_pos = function_fixup_bc[i];
        target_off = function_offsets[slot];
        if(target_off >= 0)
        {
            call_post_off = fixup_bc_pos + 2;
            rel = target_off - call_post_off;
            g_bytecode[fixup_bc_pos] = rel & 0xFF;
            g_bytecode[fixup_bc_pos + 1] = (rel >> 8) & 0xFF;
        }
        i = i + 1;
    }
}

/* === Labels table helpers (Sub-decision E) === */

void label_init()
{
    int i;
    i = 0;
    while(i < LABEL_NAMES_BUF_SIZE)
    {
        label_names[i] = 0;
        i = i + 1;
    }
    i = 0;
    while(i < MAX_LABELS)
    {
        label_offsets[i] = 0 - 1;
        i = i + 1;
    }
    label_count = 0;
    label_fixup_count = 0;
}

int label_name_eq(int slot, char* name, int len)
{
    int base;
    int i;
    base = slot * MAX_LABEL_NAME_LEN;
    i = 0;
    while(i < len)
    {
        if((label_names[base + i] & 0xFF) != (name[i] & 0xFF)) return 0;
        i = i + 1;
    }
    if((label_names[base + len] & 0xFF) != 0) return 0;
    return 1;
}

/* Sub-C aplicado a labels (2026-05-19): labels dentro F prefijadas con bytes
 * invisibles [HI,LO,'|',name] del fn_slot. Sin prefijo: globales.
 *
 * BUG resuelto B7/B2: 422 labels distintos en source, sin prefix colapsaban
 * en mismo slot. F1 'Label [loop]' y F2 'Label [loop]' compartian slot ->
 * Jmp [loop] de F1 resolvia a posicion bytecode de F2 -> ejecucion corrupta. */
char label_prefixed_buf[40];

int label_lookup_or_create_inner(char* name, int len)
{
    int slot;
    int base;
    int i;
    if(len <= 0) return 0 - 1;
    if(len > MAX_LABEL_NAME_LEN - 1) return 0 - 1;
    slot = 0;
    while(slot < label_count)
    {
        if(label_name_eq(slot, name, len) != 0) return slot;
        slot = slot + 1;
    }
    if(label_count >= MAX_LABELS) return 0 - 1;
    slot = label_count;
    base = slot * MAX_LABEL_NAME_LEN;
    i = 0;
    while(i < len)
    {
        label_names[base + i] = name[i] & 0xFF;
        i = i + 1;
    }
    label_names[base + len] = 0;
    label_offsets[slot] = 0 - 1;
    label_count = label_count + 1;
    return slot;
}

int label_lookup_or_create(char* name, int len)
{
    int plen;
    int i;
    if(len <= 0) return 0 - 1;
    if(len > MAX_LABEL_NAME_LEN - 1) return 0 - 1;
    if(pending_fn_slot >= 0)
    {
        /* `__name` accede global directamente (skip __). */
        if(len > 2)
        {
            if((name[0] & 0xFF) == 95)
            {
                if((name[1] & 0xFF) == 95)
                {
                    return label_lookup_or_create_inner(name + 2, len - 2);
                }
            }
        }
        /* Local: prefijar con fn_slot bytes invisibles. */
        label_prefixed_buf[0] = pending_fn_slot & 0xFF;
        label_prefixed_buf[1] = (pending_fn_slot >> 8) & 0xFF;
        label_prefixed_buf[2] = 124;   /* '|' = 0x7C */
        i = 0;
        while(i < len)
        {
            label_prefixed_buf[3 + i] = name[i] & 0xFF;
            i = i + 1;
        }
        plen = 3 + len;
        if(plen > MAX_LABEL_NAME_LEN - 1) return 0 - 1;
        return label_lookup_or_create_inner(label_prefixed_buf, plen);
    }
    return label_lookup_or_create_inner(name, len);
}

int label_add_fixup(int bc_pos, int target_slot)
{
    if(label_fixup_count >= MAX_LABEL_FIXUPS) return 1;
    label_fixup_bc[label_fixup_count] = bc_pos;
    label_fixup_target[label_fixup_count] = target_slot;
    label_fixup_count = label_fixup_count + 1;
    return 0;
}

/* Post-parse: patchea fixups con rel16 = label_offsets[slot] - post_payload. */
void label_resolve_fixups()
{
    int i;
    int slot;
    int target_off;
    int post_off;
    int rel;
    int fixup_bc_pos;
    i = 0;
    while(i < label_fixup_count)
    {
        slot = label_fixup_target[i];
        fixup_bc_pos = label_fixup_bc[i];
        target_off = label_offsets[slot];
        if(target_off >= 0)
        {
            post_off = fixup_bc_pos + 2;   /* fixup_bc = pos low byte rel16, +2 = post payload */
            rel = target_off - post_off;
            g_bytecode[fixup_bc_pos] = rel & 0xFF;
            g_bytecode[fixup_bc_pos + 1] = (rel >> 8) & 0xFF;
        }
        i = i + 1;
    }
}

/* === Symbol Table (Fase C: multi-char vars) ===
 * Slots 0-25 reservados para A-Z (compat con V3_elf existente).
 * Slots 26-189 asignados dinamicamente para multi-char identifiers.
 * Slots 190-255 reservados system (RET_ADDR_BASE, DROP_SLOT).
 * Layout: sym_names[slot * MAX_NAME_LEN] = string del nombre (terminado en 0).
 * Lookup lineal (sin hash). Suficiente para programas hasta ~190 vars.
 *
 * Sub-decision C (2026-05-18): nombres "locales a F" se prefijan internamente
 * con bytes [HI, LO, '|', ...name] donde HI:LO es el fn_slot. Esto garantiza
 * que dos F con mismo nombre de variable obtienen slots distintos. Acceso a
 * global desde dentro F usa prefix `__` en source (parser lo intercepta). */
#define MAX_NAME_LEN 32     /* 32 chars max nombre. Permite prefix [HI,LO,'|',name<=28]. */
#define SYM_BUF_SIZE 131072 /* 4096 slots * 32 chars (Lote 4b 2026-05-19) */
char sym_names[SYM_BUF_SIZE];
int sym_count;              /* Cantidad de slots asignados (inicialmente 26 para A-Z) */
int sym_high_water;         /* Sub-C reciclaje: max sym_count alcanzado durante parse.
                             * Nuevos slots siempre >= sym_high_water para no colisionar
                             * con slots locales reciclados que el bytecode F sigue usando. */

/* Init symbol table: prellena slots 0-25 con nombres A-Z. */
void sym_init()
{
    int i;
    int base;
    /* Cero todo el buffer primero */
    i = 0;
    while(i < SYM_BUF_SIZE)
    {
        sym_names[i] = 0;
        i = i + 1;
    }
    /* Slots 0-25: nombres 1-char A-Z */
    i = 0;
    while(i < 26)
    {
        base = i * MAX_NAME_LEN;
        sym_names[base] = 65 + i;   /* 'A' + i */
        sym_names[base + 1] = 0;
        i = i + 1;
    }
    sym_count = 26;
    sym_high_water = 26;
}

/* Compara name (len bytes) con sym_names[slot * 16]. Retorna 1 si igual. */
int sym_eq(int slot, char* name, int len)
{
    int base;
    int i;
    base = slot * MAX_NAME_LEN;
    i = 0;
    while(i < len)
    {
        if((sym_names[base + i] & 0xFF) != (name[i] & 0xFF)) return 0;
        i = i + 1;
    }
    /* Verifica que sym_names[slot][len] sea 0 (fin de string en sym) */
    if((sym_names[base + len] & 0xFF) != 0) return 0;
    return 1;
}

/* Busca name en symtab. Retorna slot o -1 si no existe. NO crea. */
int sym_lookup_global(char* name, int len)
{
    int slot;
    if(len <= 0) return 0 - 1;
    if(len > MAX_NAME_LEN - 1) return 0 - 1;
    slot = 0;
    while(slot < sym_count)
    {
        if(sym_eq(slot, name, len) != 0) return slot;
        slot = slot + 1;
    }
    return 0 - 1;
}

/* Busca name en symtab. Si existe, retorna slot. Si no, agrega y retorna nuevo slot.
 * Retorna -1 si tabla llena. Operacion sobre tabla global directamente
 * (sin scope local). Usado internamente por sym_intern.
 *
 * Sub-C reciclaje (2026-05-18): el slot creado usa max(sym_count, sym_high_water)
 * para evitar colisiones con slots locales reciclados que el bytecode F sigue
 * referenciando. sym_count puede haber retrocedido por reciclaje. */
int sym_intern_global(char* name, int len)
{
    int slot;
    int base;
    int i;
    int new_slot;
    if(len <= 0) return 0 - 1;
    if(len > MAX_NAME_LEN - 1) return 0 - 1;
    slot = 0;
    while(slot < sym_count)
    {
        if(sym_eq(slot, name, len) != 0) return slot;
        slot = slot + 1;
    }
    /* Crear nuevo: max(sym_count, sym_high_water) protege slots reciclados. */
    if(sym_count >= sym_high_water)
    {
        new_slot = sym_count;
    }
    else
    {
        new_slot = sym_high_water;
    }
    if(new_slot >= USER_SLOTS_MAX) return 0 - 1;
    base = new_slot * MAX_NAME_LEN;
    i = 0;
    while(i < len)
    {
        sym_names[base + i] = name[i] & 0xFF;
        i = i + 1;
    }
    sym_names[base + len] = 0;
    sym_count = new_slot + 1;
    if(sym_count > sym_high_water) sym_high_water = sym_count;
    return new_slot;
}

/* Buffer temp para construir nombres prefijados local-scope (Sub-C).
 * Layout: [fn_slot_HI, fn_slot_LO, '|', ...original_name].
 * Bytes 0-1 + '|' = 3 bytes prefijo. Name max 28 bytes -> total max 31. */
char sym_prefixed_buf[40];

/* sym_intern context-aware (Sub-decision C 2026-05-18):
 *   Si pending_fn_slot >= 0 (dentro F):
 *     - name "__xxx" (>=2 chars con __ prefix): acceso global, intern xxx en global.
 *     - Sino: lookup global existente (top-level vars). Si existe, retorna global slot.
 *     - Sino: intern como local con prefijo [HI,LO,'|',name] en global table.
 *   Si fuera F: intern global normal.
 *
 * Esto garantiza:
 *   - Vars top-level (declaradas fuera F) son globales accesibles desde dentro F.
 *   - Vars NUEVAS dentro F son locales (prefijo invisible al programador).
 *   - Acceso explicito a global desde F via `__name`. */
int sym_intern(char* name, int len)
{
    int slot;
    int plen;
    int i;
    if(len <= 0) return 0 - 1;
    if(len > MAX_NAME_LEN - 1) return 0 - 1;
    if(pending_fn_slot >= 0)
    {
        /* Acceso global explicito con __ prefix. */
        if(len > 2)
        {
            if((name[0] & 0xFF) == 95)
            {
                if((name[1] & 0xFF) == 95)
                {
                    return sym_intern_global(name + 2, len - 2);
                }
            }
        }
        /* Lookup global existente (top-level decl antes de F): si existe, usar. */
        slot = sym_lookup_global(name, len);
        if(slot >= 0) return slot;
        /* No existe como global. Crear local con prefijo bytes invisibles. */
        sym_prefixed_buf[0] = pending_fn_slot & 0xFF;
        sym_prefixed_buf[1] = (pending_fn_slot >> 8) & 0xFF;
        sym_prefixed_buf[2] = 124;   /* '|' = 0x7C */
        i = 0;
        while(i < len)
        {
            sym_prefixed_buf[3 + i] = name[i] & 0xFF;
            i = i + 1;
        }
        plen = 3 + len;
        if(plen > MAX_NAME_LEN - 1) return 0 - 1;
        return sym_intern_global(sym_prefixed_buf, plen);
    }
    return sym_intern_global(name, len);
}

/* Valida identifier: [A-Za-z_][A-Za-z0-9_]{0,15}. */
int is_valid_ident(char* name, int len)
{
    int i;
    int c;
    if(len <= 0) return 0;
    if(len > MAX_NAME_LEN - 1) return 0;
    c = name[0] & 0xFF;
    /* Primer char: A-Z (65-90), a-z (97-122), _ (95) */
    if(c == 95) goto first_ok;
    if(c >= 65) { if(c <= 90) goto first_ok; }
    if(c >= 97) { if(c <= 122) goto first_ok; }
    return 0;
first_ok:
    i = 1;
    while(i < len)
    {
        c = name[i] & 0xFF;
        if(c == 95) goto next;
        if(c >= 48) { if(c <= 57) goto next; }   /* 0-9 */
        if(c >= 65) { if(c <= 90) goto next; }
        if(c >= 97) { if(c <= 122) goto next; }
        return 0;
next:
        i = i + 1;
    }
    return 1;
}

/* === Backend arch (provee arch_x86.c, arch_arm.c, etc) ===
 * Las funciones arch_*() encapsulan TODO lo arch-specific (handler bytes,
 * ELF e_machine, setup/dispatch/tail bytes). compilador_777.c es arch-agnostic.
 * Para portar a otra arquitectura: crear arch_<X>.c con misma interface y
 * pasar -f arch_<X>.c antes de -f compilador_777.c en build.
 *
 * Funciones esperadas (M2-Planet no tiene forward decl: definir antes de usar):
 *   arch_e_machine() -> int
 *   arch_base_vaddr() -> int
 *   arch_jt_entry_bytes() -> int
 *   arch_handler_size(idx) -> int
 *   arch_handler_ptr(idx) -> int*
 *   arch_handler_is_halt(idx) -> int (1 si si)
 *   arch_handler_is_exit(idx) -> int (1 si si)
 *   arch_setup_size() -> int
 *   arch_dispatch_size() -> int
 *   arch_tail_size() -> int
 *   arch_jmp_back_size() -> int
 *   arch_emit_setup(char* buf, int bc_disp, int vars_disp)
 *   arch_emit_dispatch(char* buf, int jt_disp)
 *   arch_emit_tail(char* buf)
 *   arch_emit_jmp_back(char* buf, int disp)
 *   arch_emit_halt_jmp(char* buf, int disp)
 */

/* (handler bytes + sizes/ptr movidos a arch_x86.c. Acceso via arch_handler_*.) */

/* === Resto del compilador (parser, ELF builder generico, main) === */

/* handler_size + handler_ptr ahora son arch_handler_size + arch_handler_ptr (en arch_x86.c). */

/* === Helpers basicos === */

/* Imprime "label<num>\n" a stderr usando g_dbuf. */
void debug_int(int v, char* label)
{
    int idx;
    int label_len;
    int q;
    label_len = 0;
    while(label[label_len] != 0) label_len = label_len + 1;
    write(2, label, label_len);
    idx = 0;
    if(v == 0)
    {
        g_dbuf[0] = 48;
        idx = 1;
    }
    while(v > 0)
    {
        q = v / 10;
        g_dbuf[idx] = 48 + (v - q * 10);
        v = q;
        idx = idx + 1;
    }
    while(idx > 0)
    {
        idx = idx - 1;
        write(2, g_dbuf + idx, 1);
    }
    write(2, "\n", 1);
}

/* M2-Planet quirk: `char arr[]; c = arr[i]` retorna byte con basura en bytes altos.
 * Helper enmascarado para leer byte real de g_source. */
int sb(int p)
{
    return g_source[p] & 0xFF;
}

/* var_slot: si c en A-Z, retorna c-'A'. Sino -1. */
int var_slot(int c)
{
    if(c >= 65)
    {
        if(c <= 90) return c - 65;
    }
    return 0 - 1;
}

/* parse_i64: parsea decimal (con opcional '-' prefix) o u64 unsigned grande.
 * En M2-Planet/amd64 int es 64-bit, mul/add wraps modular. */
int parse_i64(char* s, int len, int* err_out)
{
    int neg;
    int acc;
    int i;
    int b;

    *err_out = 0;
    if(len == 0) { *err_out = 1; return 0; }

    neg = 0;
    i = 0;
    if(s[0] == 45) /* '-' */
    {
        neg = 1;
        i = 1;
        if(len == 1) { *err_out = 1; return 0; }
    }

    acc = 0;
    while(i < len)
    {
        b = s[i];
        if(b < 48) { *err_out = 1; return 0; }
        if(b > 57) { *err_out = 1; return 0; }
        acc = acc * 10 + (b - 48);
        i = i + 1;
    }
    if(neg) return 0 - acc;
    return acc;
}

/* fits_in_i32: val cabe en i32 [-2^31, 2^31)? */
int fits_in_i32(int v)
{
    if(v < 0)
    {
        if(v >= 0 - 2147483648) return 1;
        return 0;
    }
    if(v <= 2147483647) return 1;
    return 0;
}

/* === Emit primitives a g_bytecode === */
void bc_emit(int b)
{
    g_bytecode[g_bytecode_len] = b;
    g_bytecode_len = g_bytecode_len + 1;
}

/* Emite slot tras un opcode LOAD_VAR/STORE_VAR ya emitido.
 * IMPORTANTE: el opcode debe haber sido OP_LOAD_VAR (6) o OP_STORE_VAR (7).
 * Si slot < 256: emite 1 byte slot (compat 1-byte slot opcode).
 * Si slot >= 256: backpatch el opcode anterior a su variante OP_LOAD_VAR16 (50)
 *                 o OP_STORE_VAR16 (51) y emite 2 bytes slot.
 * Esto preserva bytecode size para tests con slot<256 que usan rel manual
 * y habilita slots grandes (hasta 65535) para self-host real. */
void bc_emit_slot(int s)
{
    int prev;
    if(s < 256)
    {
        bc_emit(s & 0xFF);
    }
    else
    {
        /* Backpatch opcode anterior. */
        prev = g_bytecode[g_bytecode_len - 1] & 0xFF;
        if(prev == OP_LOAD_VAR)
        {
            g_bytecode[g_bytecode_len - 1] = OP_LOAD_VAR16;
        }
        else
        {
            if(prev == OP_STORE_VAR)
            {
                g_bytecode[g_bytecode_len - 1] = OP_STORE_VAR16;
            }
        }
        bc_emit(s & 0xFF);
        bc_emit((s >> 8) & 0xFF);
    }
}

void bc_emit_imm32(int v)
{
    bc_emit(v);
    bc_emit(v >> 8);
    bc_emit(v >> 16);
    bc_emit(v >> 24);
}

void bc_emit_imm64(int v)
{
    bc_emit(v);
    bc_emit(v >> 8);
    bc_emit(v >> 16);
    bc_emit(v >> 24);
    bc_emit(v >> 32);
    bc_emit(v >> 40);
    bc_emit(v >> 48);
    bc_emit(v >> 56);
}

/* === Emit primitives a g_elf === */
void elf_emit_at(int off, int b)
{
    g_elf[off] = b;
}

void elf_emit_u16_at(int off, int v)
{
    g_elf[off] = v;
    g_elf[off + 1] = v >> 8;
}

void elf_emit_u32_at(int off, int v)
{
    g_elf[off] = v;
    g_elf[off + 1] = v >> 8;
    g_elf[off + 2] = v >> 16;
    g_elf[off + 3] = v >> 24;
}

void elf_emit_u64_at(int off, int v)
{
    g_elf[off] = v;
    g_elf[off + 1] = v >> 8;
    g_elf[off + 2] = v >> 16;
    g_elf[off + 3] = v >> 24;
    g_elf[off + 4] = v >> 32;
    g_elf[off + 5] = v >> 40;
    g_elf[off + 6] = v >> 48;
    g_elf[off + 7] = v >> 56;
}

/* === build_vm_elf: arma ELF en g_elf desde g_bytecode === */
void build_vm_elf()
{
    int setup_size;
    int dloop_size;
    int handler_offsets[64];  /* margen amplio para futuras adiciones (NUM_OPCODES actual + futuro) */
    int cur;
    int i;
    int tail_offset;
    int tail_size;
    int vm_code_size;
    int jt_offset;
    int jt_size;
    int vars_offset;
    int bc_offset;
    int total_file_size;
    int entry;
    int vm_base;
    int lea_r15_post;
    int bc_abs;
    int disp_bc;
    int lea_rbp_post;
    int vars_abs;
    int disp_vars;
    int dloop_abs;
    int lea_rcx_post;
    int jt_abs;
    int disp_jt;
    int tail_abs;
    int h_abs;
    int jmp_pos;
    int jmp_post;
    int disp;
    int h_post;
    int abs_addr;
    int sz;
    int j;
    int hbytes_len;
    int* hbytes;

    setup_size = arch_setup_size();
    dloop_size = arch_dispatch_size();

    /* Compute handler offsets (relativo al inicio de VM code). */
    cur = setup_size + dloop_size;
    i = 0;
    while(i < NUM_OPCODES)
    {
        handler_offsets[i] = cur;
        hbytes_len = arch_handler_size(i);
        if(arch_handler_is_halt(i) != 0)
        {
            sz = arch_jmp_back_size();    /* halt: jmp near to tail */
        }
        else
        {
            if(arch_handler_is_exit(i) != 0)
            {
                sz = hbytes_len;          /* syscall, no jmp */
            }
            else
            {
                sz = hbytes_len + arch_jmp_back_size();    /* + jmp near to dispatch */
            }
        }
        cur = cur + sz;
        i = i + 1;
    }

    tail_offset = cur;
    tail_size = arch_tail_size();
    vm_code_size = tail_offset + tail_size;
    jt_offset = vm_code_size;
    jt_size = NUM_OPCODES * arch_jt_entry_bytes();
    vars_offset = jt_offset + jt_size;
    bc_offset = vars_offset + VARS_AREA_SIZE;
    total_file_size = HEADER_TOTAL + bc_offset + g_bytecode_len;

    g_elf_len = total_file_size;

    /* Zero entire buffer up to total_file_size. */
    i = 0;
    while(i < total_file_size)
    {
        g_elf[i] = 0;
        i = i + 1;
    }

    entry = arch_base_vaddr() + HEADER_TOTAL;

    /* ELF header */
    g_elf[0] = 0x7f;
    g_elf[1] = 0x45;  /* E */
    g_elf[2] = 0x4c;  /* L */
    g_elf[3] = 0x46;  /* F */
    g_elf[4] = 2;     /* 64-bit */
    g_elf[5] = 1;     /* little-endian */
    g_elf[6] = 1;     /* version */
    /* 7..15 padding zeros */
    elf_emit_u16_at(16, 2);    /* e_type EXEC */
    elf_emit_u16_at(18, arch_e_machine());   /* e_machine: provee arch backend */
    elf_emit_u32_at(20, 1);    /* e_version */
    elf_emit_u64_at(24, entry);     /* e_entry */
    elf_emit_u64_at(32, 64);   /* e_phoff */
    elf_emit_u64_at(40, 0);    /* e_shoff */
    elf_emit_u32_at(48, 0);    /* e_flags */
    elf_emit_u16_at(52, 64);   /* e_ehsize */
    elf_emit_u16_at(54, 56);   /* e_phentsize */
    elf_emit_u16_at(56, 1);    /* e_phnum */

    /* Program header (offset 64..120) */
    elf_emit_u32_at(64, 1);              /* p_type PT_LOAD */
    elf_emit_u32_at(68, 7);              /* p_flags RWX */
    elf_emit_u64_at(72, 0);              /* p_offset */
    elf_emit_u64_at(80, arch_base_vaddr());     /* p_vaddr */
    elf_emit_u64_at(88, arch_base_vaddr());     /* p_paddr */
    elf_emit_u64_at(96, total_file_size); /* p_filesz */
    elf_emit_u64_at(104, total_file_size); /* p_memsz */
    elf_emit_u64_at(112, 0x1000);        /* p_align */

    vm_base = HEADER_TOTAL;

    /* Setup (prologo): arch_emit_setup encapsula los bytes especificos.
     * Pasamos las displacements rip-relative; arch backend conoce el tamano
     * y posicion exacta de las instrucciones lea. */
    lea_r15_post = vm_base + 10;       /* RIP post lea_r15 (x86) */
    bc_abs = vm_base + bc_offset;
    disp_bc = bc_abs - lea_r15_post;
    lea_rbp_post = vm_base + 17;       /* RIP post lea_rbp (x86) */
    vars_abs = vm_base + vars_offset;
    disp_vars = vars_abs - lea_rbp_post;
    arch_emit_setup(g_elf + vm_base, disp_bc, disp_vars);

    /* Dispatch loop: arch_emit_dispatch encapsula bytes especificos.
     * disp es relativo al RIP post lea (caller computa offset exacto). */
    dloop_abs = vm_base + setup_size;
    lea_rcx_post = dloop_abs + 14;     /* RIP post lea_rcx (x86) */
    jt_abs = vm_base + jt_offset;
    disp_jt = jt_abs - lea_rcx_post;
    arch_emit_dispatch(g_elf + dloop_abs, disp_jt);

    /* Handlers */
    tail_abs = vm_base + tail_offset;
    i = 0;
    while(i < NUM_OPCODES)
    {
        h_abs = vm_base + handler_offsets[i];
        hbytes = arch_handler_ptr(i);
        hbytes_len = arch_handler_size(i);
        if(arch_handler_is_halt(i) != 0)
        {
            /* HALT: arch_emit_halt_jmp escribe jmp near to tail */
            h_post = h_abs + arch_jmp_back_size();
            disp = tail_abs - h_post;
            arch_emit_halt_jmp(g_elf + h_abs, disp);
        }
        else
        {
            if(arch_handler_is_exit(i) != 0)
            {
                /* Copy bytes, no jmp back */
                j = 0;
                while(j < hbytes_len)
                {
                    g_elf[h_abs + j] = hbytes[j];
                    j = j + 1;
                }
            }
            else
            {
                /* Copy + jmp near to dispatch */
                j = 0;
                while(j < hbytes_len)
                {
                    g_elf[h_abs + j] = hbytes[j];
                    j = j + 1;
                }
                jmp_pos = h_abs + hbytes_len;
                jmp_post = jmp_pos + arch_jmp_back_size();
                disp = dloop_abs - jmp_post;
                arch_emit_jmp_back(g_elf + jmp_pos, disp);
            }
        }
        i = i + 1;
    }

    /* Tail: arch_emit_tail escribe el epilogo arch-specific. */
    arch_emit_tail(g_elf + tail_abs);

    /* Jump table: abs addrs to handlers. Arch decide tamano de cada entry. */
    i = 0;
    while(i < NUM_OPCODES)
    {
        abs_addr = arch_base_vaddr() + vm_base + handler_offsets[i];
        elf_emit_u64_at(vm_base + jt_offset + i * arch_jt_entry_bytes(), abs_addr);
        i = i + 1;
    }

    /* Vars area: ya esta zero por el zero-fill inicial. */

    /* Bytecode */
    i = 0;
    while(i < g_bytecode_len)
    {
        g_elf[vm_base + bc_offset + i] = g_bytecode[i];
        i = i + 1;
    }
}

/* === Parser: compile_777m_to_bytecode === */
/* Parsea g_source[0..g_source_len], emite a g_bytecode.
 * Sintaxis subset:
 *   D <id> <id> <name1char> Integer <val>
 *   R <id> <id> <kind> [<arg1>, ...] [<output>]
 *   F/C/lineas que empiezan con '7': skip.
 * Kinds: Add Sub Mul Eq Lt Shr BitAnd LoadByte LoadI64 StoreByte StoreI64
 *        Alloc FdRead FdWrite Jmp Jz ExitTop.
 */

/* parse_token: avanza pos en *p hasta proximo espacio/tab/EOL o EOF.
 * Retorna ptr al inicio del token y len por out param. */
char* next_token(int* p, int end, int* out_len)
{
    int start;
    int c;
    /* Skip leading whitespace (space, tab). */
    while(*p < end)
    {
        c = sb(*p);
        if(c != 32) { if(c != 9) break; }
        *p = *p + 1;
    }
    start = *p;
    /* Quoted string: consume desde " hasta " cerrando, procesando escapes mentalmente. */
    if(*p < end)
    {
        if(sb(*p) == 34)  /* '"' */
        {
            *p = *p + 1;  /* skip opening " */
            while(*p < end && sb(*p) != 34)
            {
                if(sb(*p) == 92 && (*p + 1) < end)   /* '\\' + escape char */
                {
                    *p = *p + 2;
                }
                else
                {
                    *p = *p + 1;
                }
            }
            if(*p < end) { *p = *p + 1; }  /* skip closing " */
            *out_len = *p - start;
            return g_source + start;
        }
    }
    /* Read until whitespace or EOL */
    while(*p < end)
    {
        c = sb(*p);
        if(c == 32) break;
        if(c == 9) break;
        if(c == 10) break;
        if(c == 13) break;
        *p = *p + 1;
    }
    *out_len = *p - start;
    return g_source + start;
}

/* skip_to_eol: avanza pos hasta despues del proximo '\n' o EOF. */
void skip_to_eol(int* p, int end)
{
    while(*p < end)
    {
        if(sb(*p) == 10) { *p = *p + 1; return; }
        *p = *p + 1;
    }
}

/* eq_str: compara n bytes de a con C-string b (terminado en 0). Retorna 1 si match exacto y len iguales. */
int eq_str(char* a, int alen, char* b)
{
    int i;
    i = 0;
    while(i < alen)
    {
        if(b[i] == 0) return 0;
        if(a[i] != b[i]) return 0;
        i = i + 1;
    }
    if(b[i] != 0) return 0;
    return 1;
}

/* emit_load_arg: emite LOAD_VAR si arg es identifier valido (1-char A-Z compat
 * o multi-char via symtab). Sino LOAD_IMM32/64. */
int emit_load_arg(char* arg, int alen)
{
    int slot;
    int val;
    int err;
    if(alen == 0) return 1;
    /* Identifier valido: prueba sym_intern (cubre A-Z + multi-char). */
    if(is_valid_ident(arg, alen) != 0)
    {
        slot = sym_intern(arg, alen);
        if(slot < 0) return 1;
        bc_emit(OP_LOAD_VAR);
        bc_emit_slot(slot);
        return 0;
    }
    val = parse_i64(arg, alen, &err);
    if(err != 0) return 1;
    if(fits_in_i32(val))
    {
        bc_emit(OP_LOAD_IMM32);
        bc_emit_imm32(val);
    }
    else
    {
        bc_emit(OP_LOAD_IMM64);
        bc_emit_imm64(val);
    }
    return 0;
}

/* opcode_from_kind: mapea kind name -> opcode. Retorna -1 si no soportado. */
int opcode_from_kind(char* kind, int klen)
{
    if(eq_str(kind, klen, "Add")) return OP_ADD;
    if(eq_str(kind, klen, "Sub")) return OP_SUB;
    if(eq_str(kind, klen, "Mul")) return OP_MUL;
    if(eq_str(kind, klen, "Eq")) return OP_EQ;
    if(eq_str(kind, klen, "Lt")) return OP_LT;
    if(eq_str(kind, klen, "Alloc")) return OP_ALLOC;
    if(eq_str(kind, klen, "FdRead")) return OP_FD_READ;
    if(eq_str(kind, klen, "FdWrite")) return OP_FD_WRITE;
    if(eq_str(kind, klen, "LoadByte")) return OP_LOAD_BYTE;
    if(eq_str(kind, klen, "StoreByte")) return OP_STORE_BYTE;
    if(eq_str(kind, klen, "LoadI64")) return OP_LOAD_I64;
    if(eq_str(kind, klen, "StoreI64")) return OP_STORE_I64;
    if(eq_str(kind, klen, "Shr")) return OP_SHR;
    if(eq_str(kind, klen, "BitAnd")) return OP_BITAND;
    if(eq_str(kind, klen, "Div")) return OP_DIV;
    if(eq_str(kind, klen, "Mod")) return OP_MOD;
    if(eq_str(kind, klen, "BitOr")) return OP_BITOR;
    if(eq_str(kind, klen, "Shl")) return OP_SHL;
    if(eq_str(kind, klen, "Gt")) return OP_GT;
    if(eq_str(kind, klen, "BitXor")) return OP_BITXOR;
    if(eq_str(kind, klen, "Open")) return OP_OPEN;
    if(eq_str(kind, klen, "Close")) return OP_CLOSE;
    if(eq_str(kind, klen, "Ioctl")) return OP_IOCTL;
    if(eq_str(kind, klen, "Nanosleep")) return OP_NANOSLEEP;
    if(eq_str(kind, klen, "Poll")) return OP_POLL;
    if(eq_str(kind, klen, "Fork")) return OP_FORK;
    if(eq_str(kind, klen, "Execve")) return OP_EXECVE;
    if(eq_str(kind, klen, "Wait4")) return OP_WAIT4;
    if(eq_str(kind, klen, "Pipe")) return OP_PIPE;
    if(eq_str(kind, klen, "Dup2")) return OP_DUP2;
    if(eq_str(kind, klen, "Socket")) return OP_SOCKET;
    if(eq_str(kind, klen, "Connect")) return OP_CONNECT;
    if(eq_str(kind, klen, "Bind")) return OP_BIND;
    if(eq_str(kind, klen, "Listen")) return OP_LISTEN;
    if(eq_str(kind, klen, "Accept")) return OP_ACCEPT;
    if(eq_str(kind, klen, "MmapFd")) return OP_MMAP_FD;
    if(eq_str(kind, klen, "Munmap")) return OP_MUNMAP;
    if(eq_str(kind, klen, "GetTimeOfDay")) return OP_GETTIMEOFDAY;
    if(eq_str(kind, klen, "RtSigAction")) return OP_RT_SIGACTION;
    return 0 - 1;
}

int is_void_kind(int op)
{
    if(op == OP_STORE_BYTE) return 1;
    if(op == OP_STORE_I64) return 1;
    return 0;
}

/* compile_line_R: procesa "R <id> <id> <kind> [args] [output]\n". */
/* Asume p apunta justo despues de "R ". end es fin del source.
 * Avanza p hasta despues del '\n'. */
int compile_line_R(int* p, int end)
{
    char* tok;
    int tlen;
    char* kind;
    int klen;
    int op;
    int line_end;
    int q;
    int bracket_start;
    int bracket_end;
    int args_start;
    char* arg;
    int alen;
    int rel;
    int err;
    int slot;
    char* output;
    int olen;
    int has_output;
    int c;

    /* Encontrar fin de linea */
    line_end = *p;
    while(line_end < end)
    {
        if(sb(line_end) == 10) break;
        line_end = line_end + 1;
    }

    /* token: id1 (skip) */
    tok = next_token(p, line_end, &tlen);
    if(tlen == 0) goto skip_line;
    /* token: id2 (skip) */
    tok = next_token(p, line_end, &tlen);
    if(tlen == 0) goto skip_line;
    /* token: kind */
    kind = next_token(p, line_end, &klen);
    if(klen == 0) goto skip_line;

    /* Special control flow kinds: Jmp/Jz/ExitTop */
    if(eq_str(kind, klen, "Label"))
    {
        /* R 0 0 Label [name]. Sub-decision E: registra offset, no emite bytecode.
         * Jmp/Jz que referencien name resuelven a este offset via fixup. */
        int lbl_slot;
        int lbl_start;
        int lbl_len;
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) != 91) goto skip_line;
        *p = *p + 1;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
        lbl_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 93) break;
            if(c == 32) break;
            if(c == 9) break;
            *p = *p + 1;
        }
        lbl_len = *p - lbl_start;
        if(lbl_len <= 0) goto skip_line;
        lbl_slot = label_lookup_or_create(g_source + lbl_start, lbl_len);
        if(lbl_slot < 0) goto skip_line;
        label_offsets[lbl_slot] = g_bytecode_len;
        goto end_line;
    }

    if(eq_str(kind, klen, "Jmp"))
    {
        /* R 0 0 Jmp [rel16 | label_name].
         * Sub-decision E: si arg es identifier, resolver via label table. */
        int jmp_label_slot;
        int jmp_post_off;
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) != 91) goto skip_line;
        *p = *p + 1;
        args_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 93) break;
            if(c == 32) break;
            *p = *p + 1;
        }
        alen = *p - args_start;
        rel = parse_i64(g_source + args_start, alen, &err);
        bc_emit(OP_JMP);
        if(err == 0)
        {
            bc_emit(rel & 0xFF);
            bc_emit((rel >> 8) & 0xFF);
        }
        else
        {
            jmp_label_slot = label_lookup_or_create(g_source + args_start, alen);
            if(jmp_label_slot < 0) goto skip_line;
            if(label_offsets[jmp_label_slot] >= 0)
            {
                jmp_post_off = g_bytecode_len + 2;
                rel = label_offsets[jmp_label_slot] - jmp_post_off;
                bc_emit(rel & 0xFF);
                bc_emit((rel >> 8) & 0xFF);
            }
            else
            {
                label_add_fixup(g_bytecode_len, jmp_label_slot);
                bc_emit(0);
                bc_emit(0);
            }
        }
        goto end_line;
    }

    if(eq_str(kind, klen, "Jz"))
    {
        /* R 0 0 Jz [cond, rel]. */
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) != 91) goto skip_line;
        *p = *p + 1;
        /* arg1: cond */
        while(*p < line_end)
        {
            if(sb(*p) != 32) break;
            *p = *p + 1;
        }
        args_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 44) break;  /* ',' */
            if(c == 32) break;
            *p = *p + 1;
        }
        alen = *p - args_start;
        if(emit_load_arg(g_source + args_start, alen) != 0) goto skip_line;
        /* Skip ws + ',' */
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 44) { *p = *p + 1; break; }
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
        while(*p < line_end)
        {
            if(sb(*p) != 32) break;
            *p = *p + 1;
        }
        /* arg2: rel literal o label name (Sub-decision E) */
        {
            int jz_label_slot;
            int jz_post_off;
            args_start = *p;
            while(*p < line_end)
            {
                c = sb(*p);
                if(c == 93) break;
                if(c == 32) break;
                *p = *p + 1;
            }
            alen = *p - args_start;
            rel = parse_i64(g_source + args_start, alen, &err);
            bc_emit(OP_JZ);
            if(err == 0)
            {
                bc_emit(rel & 0xFF);
                bc_emit((rel >> 8) & 0xFF);
            }
            else
            {
                jz_label_slot = label_lookup_or_create(g_source + args_start, alen);
                if(jz_label_slot < 0) goto skip_line;
                if(label_offsets[jz_label_slot] >= 0)
                {
                    jz_post_off = g_bytecode_len + 2;
                    rel = label_offsets[jz_label_slot] - jz_post_off;
                    bc_emit(rel & 0xFF);
                    bc_emit((rel >> 8) & 0xFF);
                }
                else
                {
                    label_add_fixup(g_bytecode_len, jz_label_slot);
                    bc_emit(0);
                    bc_emit(0);
                }
            }
        }
        goto end_line;
    }

    if(eq_str(kind, klen, "ExitTop"))
    {
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) != 91) goto skip_line;
        *p = *p + 1;
        while(*p < line_end)
        {
            if(sb(*p) != 32) break;
            *p = *p + 1;
        }
        args_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 93) break;
            if(c == 32) break;
            *p = *p + 1;
        }
        alen = *p - args_start;
        if(emit_load_arg(g_source + args_start, alen) != 0) goto skip_line;
        bc_emit(OP_EXIT_TOP);
        goto end_line;
    }

    if(eq_str(kind, klen, "Call"))
    {
        /* R 0 0 Call [target, arg1, arg2, ...]
         * target: numero rel16 (compat back) o identifier (auto-resolve via fixup).
         * args opcionales: identifiers (LOAD_VAR slot) o numeros (LOAD_IMM32).
         *
         * Orden emit: pushes args PRIMERO, OP_CALL + rel DESPUES.
         * F body inicia con auto-pop (STORE_VAR pN, ..., p1) que recoge args. */
        int call_slot;
        int call_post_off;
        int target_start;
        int target_len;
        int arg_start;
        int arg_len;
        int arg_val;
        int arg_err;
        int arg_slot;
        /* skip ws + '[' */
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) != 91) goto skip_line;
        *p = *p + 1;
        /* skip ws */
        while(*p < line_end)
        {
            c = sb(*p);
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
        /* Parsear target: hasta ',' o ']' o ws */
        target_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 93) break;
            if(c == 32) break;
            if(c == 9) break;
            if(c == 44) break;
            *p = *p + 1;
        }
        target_len = *p - target_start;
        if(target_len <= 0) goto skip_line;

        /* Parsear args adicionales + emit pushes. Bucle hasta ']'. */
        while(1)
        {
            /* skip ws/coma */
            while(*p < line_end)
            {
                c = sb(*p);
                if(c == 93) break;
                if(c == 32) { *p = *p + 1; }
                else
                {
                    if(c == 9) { *p = *p + 1; }
                    else
                    {
                        if(c == 44) { *p = *p + 1; }
                        else break;
                    }
                }
            }
            if(*p >= line_end) break;
            if(sb(*p) == 93) { *p = *p + 1; break; }
            /* parse 1 arg token */
            arg_start = *p;
            while(*p < line_end)
            {
                c = sb(*p);
                if(c == 32) break;
                if(c == 9) break;
                if(c == 44) break;
                if(c == 93) break;
                *p = *p + 1;
            }
            arg_len = *p - arg_start;
            if(arg_len <= 0) continue;
            /* Try parse as i64 */
            arg_val = parse_i64(g_source + arg_start, arg_len, &arg_err);
            if(arg_err == 0)
            {
                /* es numero literal: LOAD_IMM32 */
                bc_emit(OP_LOAD_IMM32);
                bc_emit(arg_val & 0xFF);
                bc_emit((arg_val >> 8) & 0xFF);
                bc_emit((arg_val >> 16) & 0xFF);
                bc_emit((arg_val >> 24) & 0xFF);
            }
            else
            {
                /* es identifier: LOAD_VAR slot */
                arg_slot = sym_intern(g_source + arg_start, arg_len);
                if(arg_slot < 0) goto skip_line;
                bc_emit(OP_LOAD_VAR);
                bc_emit_slot(arg_slot);
            }
        }

        /* Emit OP_CALL + rel para target. */
        rel = parse_i64(g_source + target_start, target_len, &err);
        bc_emit(OP_CALL);
        if(err == 0)
        {
            /* Target es numero: rel16 literal (compat back). NO args ni result post.
             * Tests previos test_op_call_ret usan este path. */
            bc_emit(rel & 0xFF);
            bc_emit((rel >> 8) & 0xFF);
            goto end_line;
        }
        /* Target es identifier de funcion. */
        call_slot = func_lookup_or_create(g_source + target_start, target_len);
        if(call_slot < 0) goto skip_line;
        if(function_offsets[call_slot] >= 0)
        {
            call_post_off = g_bytecode_len + 2;
            rel = function_offsets[call_slot] - call_post_off;
            bc_emit(rel & 0xFF);
            bc_emit((rel >> 8) & 0xFF);
        }
        else
        {
            func_add_fixup(g_bytecode_len, call_slot);
            bc_emit(0);
            bc_emit(0);
        }

        /* Sub-decision B: F siempre push val pre-RET. Caller debe consumir.
         * Parse opcional result slot tras ']'. Si presente: STORE_VAR result.
         * Si no presente: OP_DROP (discard val). */
        {
            int result_start;
            int result_len;
            int result_slot;
            /* skip ws */
            while(*p < line_end)
            {
                c = sb(*p);
                if(c != 32) { if(c != 9) break; }
                *p = *p + 1;
            }
            result_start = *p;
            while(*p < line_end)
            {
                c = sb(*p);
                if(c == 32) break;
                if(c == 9) break;
                if(c == 10) break;
                if(c == 13) break;
                *p = *p + 1;
            }
            result_len = *p - result_start;
            if(result_len > 0)
            {
                /* result slot: emit STORE_VAR result (pop val al slot). */
                result_slot = sym_intern(g_source + result_start, result_len);
                if(result_slot < 0) goto skip_line;
                bc_emit(OP_STORE_VAR);
                bc_emit_slot(result_slot);
            }
            else
            {
                /* sin result: emit STORE_VAR DROP_SLOT (discard val pushed por F).
                 * Reusa OP_STORE_VAR existente con slot dummy 4095 (Lote 4b). */
                bc_emit(OP_STORE_VAR);
                bc_emit_slot(DROP_SLOT);
            }
        }
        goto end_line;
    }

    if(eq_str(kind, klen, "Ret"))
    {
        /* Sub-decision B + Sub-G (2026-05-18):
         * Dentro F (pending_fn_slot >= 0):
         *   R 0 0 Ret []      -> emit LOAD_IMM32 0 + LOAD RET_ADDR_SLOT + OP_RET (push 0 dummy)
         *   R 0 0 Ret [val]   -> emit LOAD val + LOAD RET_ADDR_SLOT + OP_RET (push val)
         *   Stack post-RET: [..., val]. Caller hace STORE_VAR result o OP_DROP.
         *
         * Fuera F (pending_fn_slot < 0): solo OP_RET (compat back con Call rel16 manual).
         *
         * Sub-G: NO patchea pending_fn_jmp_pos. Eso ocurre en EndF / siguiente F / EOF.
         * Eso permite multi-Ret dentro F sin que el JMP del header salte solo
         * sobre el primer Ret y deje el resto del body como top-level. */
        int ret_val;
        int ret_err;
        int ret_arg_start;
        int ret_arg_len;
        int ret_arg_slot;
        if(pending_fn_slot >= 0)
        {
            /* Parse opcional [val] - skip ws + '[' + arg + ']'. */
            while(*p < line_end)
            {
                c = sb(*p);
                if(c != 32) { if(c != 9) break; }
                *p = *p + 1;
            }
            ret_val = 0;
            ret_arg_len = 0;
            if(*p < line_end && sb(*p) == 91)
            {
                *p = *p + 1;
                while(*p < line_end)
                {
                    c = sb(*p);
                    if(c != 32) { if(c != 9) break; }
                    *p = *p + 1;
                }
                ret_arg_start = *p;
                while(*p < line_end)
                {
                    c = sb(*p);
                    if(c == 93) break;
                    if(c == 32) break;
                    if(c == 9) break;
                    *p = *p + 1;
                }
                ret_arg_len = *p - ret_arg_start;
            }
            if(ret_arg_len > 0)
            {
                /* Has val: try parse num, else identifier */
                ret_val = parse_i64(g_source + ret_arg_start, ret_arg_len, &ret_err);
                if(ret_err == 0)
                {
                    bc_emit(OP_LOAD_IMM32);
                    bc_emit(ret_val & 0xFF);
                    bc_emit((ret_val >> 8) & 0xFF);
                    bc_emit((ret_val >> 16) & 0xFF);
                    bc_emit((ret_val >> 24) & 0xFF);
                }
                else
                {
                    ret_arg_slot = sym_intern(g_source + ret_arg_start, ret_arg_len);
                    if(ret_arg_slot < 0) goto skip_line;
                    bc_emit(OP_LOAD_VAR);
                    bc_emit_slot(ret_arg_slot);
                }
            }
            else
            {
                /* Ret [] o sin args: push 0 dummy para uniformidad. */
                bc_emit(OP_LOAD_IMM32);
                bc_emit(0);
                bc_emit(0);
                bc_emit(0);
                bc_emit(0);
            }
            bc_emit(OP_LOAD_VAR);
            bc_emit_slot(RET_ADDR_BASE + pending_fn_slot);
        }
        bc_emit(OP_RET);
        goto end_line;
    }

    if(eq_str(kind, klen, "EndF"))
    {
        /* Sub-G (2026-05-18): cierre explicito de F body.
         * Patchea el JMP placeholder del header F al bytecode_len actual
         * (justo despues del ultimo Ret/codigo del body). Reset pending_fn_*. */
        int endf_jmp_post;
        int endf_jmp_rel;
        if(pending_fn_jmp_pos >= 0)
        {
            endf_jmp_post = pending_fn_jmp_pos + 2;
            endf_jmp_rel = g_bytecode_len - endf_jmp_post;
            g_bytecode[pending_fn_jmp_pos] = endf_jmp_rel & 0xFF;
            g_bytecode[pending_fn_jmp_pos + 1] = (endf_jmp_rel >> 8) & 0xFF;
            pending_fn_jmp_pos = 0 - 1;
        }
        pending_fn_slot = 0 - 1;
        /* Sub-C reciclaje: liberar slots locales creados dentro F. */
        if(fn_slot_alloc_start > 0)
        {
            sym_count = fn_slot_alloc_start;
            fn_slot_alloc_start = 0;
        }
        /* EndF acepta opcionalmente '[]' para uniformidad de sintaxis. Skip si presente. */
        while(*p < line_end)
        {
            c = sb(*p);
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
        if(*p < line_end && sb(*p) == 91)
        {
            while(*p < line_end)
            {
                if(sb(*p) == 93) { *p = *p + 1; break; }
                *p = *p + 1;
            }
        }
        goto end_line;
    }

    op = opcode_from_kind(kind, klen);
    if(op < 0) goto skip_line;

    /* Parse [args] - lista separada por ','. */
    /* Skip ws hasta '[' */
    while(*p < line_end)
    {
        if(sb(*p) != 32) { if(sb(*p) != 9) break; }
        *p = *p + 1;
    }
    if(*p >= line_end) goto skip_line;
    if(sb(*p) != 91) goto skip_line;
    *p = *p + 1;

    /* Loop args */
    while(1)
    {
        while(*p < line_end)
        {
            if(sb(*p) != 32) { if(sb(*p) != 9) break; }
            *p = *p + 1;
        }
        if(*p >= line_end) goto skip_line;
        if(sb(*p) == 93) { *p = *p + 1; break; }  /* ']' */
        args_start = *p;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 44) break;
            if(c == 93) break;
            if(c == 32) break;
            *p = *p + 1;
        }
        alen = *p - args_start;
        if(emit_load_arg(g_source + args_start, alen) != 0) goto skip_line;
        while(*p < line_end)
        {
            c = sb(*p);
            if(c == 44) { *p = *p + 1; break; }
            if(c == 93) break;
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
    }

    /* Emit opcode */
    bc_emit(op);

    /* Check output_var (post-]) o sufijo. Soporta 1-char A-Z y multi-char idents. */
    has_output = 0;
    if(is_void_kind(op) == 0)
    {
        output = next_token(p, line_end, &olen);
        if(is_valid_ident(output, olen) != 0)
        {
            slot = sym_intern(output, olen);
            if(slot >= 0)
            {
                bc_emit(OP_STORE_VAR);
                bc_emit_slot(slot);
                has_output = 1;
            }
        }
        if(has_output == 0)
        {
            bc_emit(OP_ADD_R12);
        }
    }

end_line:
    /* Skip rest of line including '\n'. */
    skip_to_eol(p, end);
    return 0;
skip_line:
    skip_to_eol(p, end);
    return 0;
}

/* compile_line_D: "D <id> <id> <name1char> Integer <val>". */
int compile_line_D(int* p, int end)
{
    char* tok;
    char* type_tok;
    int type_len;
    int tlen;
    int line_end;
    int slot;
    int val;
    int err;
    int data_off;
    int fixup_pos;
    int i;
    int j;
    char b;
    char esc;
    int content_len;
    char* raw;

    line_end = *p;
    while(line_end < end)
    {
        if(sb(line_end) == 10) break;
        line_end = line_end + 1;
    }

    /* id1, id2 skip */
    tok = next_token(p, line_end, &tlen);
    if(tlen == 0) goto skip_d;
    tok = next_token(p, line_end, &tlen);
    if(tlen == 0) goto skip_d;
    /* name: 1-char A-Z compat o multi-char ident via symtab. */
    tok = next_token(p, line_end, &tlen);
    if(is_valid_ident(tok, tlen) == 0) goto skip_d;
    slot = sym_intern(tok, tlen);
    if(slot < 0) goto skip_d;
    /* type: "Integer" o "Text" */
    type_tok = next_token(p, line_end, &type_len);
    if(type_len == 0) goto skip_d;

    /* Branch por type */
    if(eq_str(type_tok, type_len, "Text"))
    {
        /* Siguiente token debe ser quoted string "..." (con escapes). */
        tok = next_token(p, line_end, &tlen);
        if(tlen < 2) goto skip_d;
        if((tok[0] & 0xFF) != 34) goto skip_d;                   /* opening " */
        if((tok[tlen - 1] & 0xFF) != 34) goto skip_d;            /* closing " */
        /* Procesa escapes del contenido raw[1..tlen-1] en buffer global str_data temp.
         * Aprovecho que str_intern hace lookup por len/content. Necesito construir
         * content procesado primero. Uso g_dbuf como buffer (max 32 bytes) si cabe,
         * sino reuso final de str_data antes del intern. Para evitar limites,
         * append directo a str_data y lookup despues NO funciona porque cambia state.
         * Solucion: usar parte trasera de g_source (que ya tiene el source en memoria) como
         * temp buffer? No. Mejor: agregar buffer global str_tmp. */
        raw = tok + 1;
        content_len = tlen - 2;
        /* Procesar escapes in-place a str_tmp (otro buffer global). */
        i = 0;
        j = 0;
        while(i < content_len)
        {
            b = raw[i] & 0xFF;
            if(b == 92 && (i + 1) < content_len)
            {
                esc = raw[i + 1] & 0xFF;
                if(esc == 110) { str_tmp[j] = 0x0A; }            /* \n */
                else if(esc == 116) { str_tmp[j] = 0x09; }       /* \t */
                else if(esc == 92) { str_tmp[j] = 92; }          /* \\ */
                else if(esc == 34) { str_tmp[j] = 34; }          /* \" */
                else if(esc == 48) { str_tmp[j] = 0; }           /* \0 */
                else if(esc == 114) { str_tmp[j] = 0x0D; }       /* \r */
                else goto skip_d;                                /* escape no soportado */
                j = j + 1;
                i = i + 2;
            }
            else
            {
                str_tmp[j] = b;
                j = j + 1;
                i = i + 1;
            }
        }
        data_off = str_intern(str_tmp, j);
        if(data_off < 0) goto skip_d;
        /* Emit LOAD_IMM64 + placeholder 8 bytes + STORE_VAR slot. */
        bc_emit(OP_LOAD_IMM64);
        fixup_pos = g_bytecode_len;
        bc_emit_imm64(0);   /* placeholder, patched al final */
        if(str_add_fixup(fixup_pos, data_off) != 0) goto skip_d;
        bc_emit(OP_STORE_VAR);
        bc_emit_slot(slot);
        goto skip_d;        /* skip resto de la linea */
    }

    /* Path Integer (legacy). type_tok = "Integer", val es next token. */
    tok = next_token(p, line_end, &tlen);
    if(tlen == 0) goto skip_d;
    val = parse_i64(tok, tlen, &err);
    if(err != 0) goto skip_d;

    if(fits_in_i32(val))
    {
        bc_emit(OP_LOAD_IMM32);
        bc_emit_imm32(val);
    }
    else
    {
        bc_emit(OP_LOAD_IMM64);
        bc_emit_imm64(val);
    }
    bc_emit(OP_STORE_VAR);
    bc_emit_slot(slot);

skip_d:
    skip_to_eol(p, end);
    return 0;
}

/* compile_line_F: procesa "F <row> <col> <name> [params]\n".
 * Registra function_offsets[slot] = g_bytecode_len actual.
 * Params no se procesan en MVP (programmer maneja args via value stack manual).
 * Caller ya consumio 'F'. */
int compile_line_F(int* p, int end)
{
    int line_end;
    int name_start;
    int nlen;
    int slot;
    int c;
    int fb_jmp_post;
    int fb_jmp_rel;

    /* Sub-G (2026-05-18): fallback patch de F previa si no se cerro con EndF.
     * Patchea el JMP placeholder al bytecode_len actual (justo antes de la
     * nueva F). Esto permite multi-Ret sin EndF cuando F siguiente es F sin
     * top-level R intermedio. */
    if(pending_fn_jmp_pos >= 0)
    {
        fb_jmp_post = pending_fn_jmp_pos + 2;
        fb_jmp_rel = g_bytecode_len - fb_jmp_post;
        g_bytecode[pending_fn_jmp_pos] = fb_jmp_rel & 0xFF;
        g_bytecode[pending_fn_jmp_pos + 1] = (fb_jmp_rel >> 8) & 0xFF;
        pending_fn_jmp_pos = 0 - 1;
    }
    pending_fn_slot = 0 - 1;
    /* Sub-C reciclaje: liberar slots locales de F previa al abrir nueva. */
    if(fn_slot_alloc_start > 0)
    {
        sym_count = fn_slot_alloc_start;
        fn_slot_alloc_start = 0;
    }

    line_end = *p;
    while(line_end < end)
    {
        if(sb(line_end) == 10) break;
        line_end = line_end + 1;
    }

    /* Skip ws */
    while(*p < line_end)
    {
        c = sb(*p);
        if(c != 32) { if(c != 9) break; }
        *p = *p + 1;
    }
    /* Parse row (skip digits) */
    while(*p < line_end)
    {
        c = sb(*p);
        if(c < 48) break;
        if(c > 57) break;
        *p = *p + 1;
    }
    /* Skip ws */
    while(*p < line_end)
    {
        c = sb(*p);
        if(c != 32) { if(c != 9) break; }
        *p = *p + 1;
    }
    /* Parse col (skip digits) */
    while(*p < line_end)
    {
        c = sb(*p);
        if(c < 48) break;
        if(c > 57) break;
        *p = *p + 1;
    }
    /* Skip ws */
    while(*p < line_end)
    {
        c = sb(*p);
        if(c != 32) { if(c != 9) break; }
        *p = *p + 1;
    }
    /* Parse name (hasta ws o '[') */
    name_start = *p;
    while(*p < line_end)
    {
        c = sb(*p);
        if(c == 32) break;
        if(c == 9) break;
        if(c == 91) break;
        *p = *p + 1;
    }
    nlen = *p - name_start;

    if(nlen <= 0) { skip_to_eol(p, end); return 0; }

    slot = func_lookup_or_create(g_source + name_start, nlen);
    if(slot < 0) { skip_to_eol(p, end); return 0; }

    /* Sub-decision C (2026-05-18): activar scope local ANTES de parse params
     * para que sym_intern de cada param use prefijo local. Sin esto, params
     * de F1 y F2 con mismo nombre colisionaban en mismo slot global. */
    pending_fn_slot = slot;
    /* Snapshot sym_count para reciclaje slots locales al cerrar F. */
    fn_slot_alloc_start = sym_count;

    /* Sub-A: Parse params [p1, p2, ..., pN]. Cada param se intern al
     * symbol table global, se guarda su slot en function_params[].
     * Si no hay '[' tras name: param_count = 0 (F sin args). */
    {
        int param_count;
        int param_start;
        int plen;
        int param_sym;
        int param_base;
        int i;
        int j;
        param_count = 0;
        param_base = slot * MAX_PARAMS_PER_FN;
        /* skip ws hasta '[' o EOL */
        while(*p < line_end)
        {
            c = sb(*p);
            if(c != 32) { if(c != 9) break; }
            *p = *p + 1;
        }
        if(*p < line_end && sb(*p) == 91)   /* '[' */
        {
            *p = *p + 1;
            while(1)
            {
                /* skip ws */
                while(*p < line_end)
                {
                    c = sb(*p);
                    if(c != 32) { if(c != 9) break; }
                    *p = *p + 1;
                }
                if(*p >= line_end) break;
                if(sb(*p) == 93) { *p = *p + 1; break; }   /* ']' */
                /* parse name del param: hasta ws/coma/bracket */
                param_start = *p;
                while(*p < line_end)
                {
                    c = sb(*p);
                    if(c == 32) break;
                    if(c == 9) break;
                    if(c == 44) break;
                    if(c == 93) break;
                    *p = *p + 1;
                }
                plen = *p - param_start;
                if(plen > 0)
                {
                    if(param_count < MAX_PARAMS_PER_FN)
                    {
                        param_sym = sym_intern(g_source + param_start, plen);
                        if(param_sym >= 0)
                        {
                            function_params[param_base + param_count] = param_sym;
                            param_count = param_count + 1;
                        }
                    }
                }
                /* skip ',' o ws hasta ']' */
                while(*p < line_end)
                {
                    c = sb(*p);
                    if(c == 93) break;
                    if(c == 44) { *p = *p + 1; break; }
                    if(c == 32) { *p = *p + 1; }
                    else
                    {
                        if(c == 9) { *p = *p + 1; }
                        else break;
                    }
                }
            }
        }
        function_param_count[slot] = param_count;

        /* Emit JMP placeholder que salta el body. Ret lo patcheara. */
        bc_emit(OP_JMP);
        pending_fn_jmp_pos = g_bytecode_len;
        pending_fn_slot = slot;
        bc_emit(0);
        bc_emit(0);
        function_offsets[slot] = g_bytecode_len;

        /* Auto-pop (Sub-decision B): F body inicia con STORE_VAR a slot UNICO
         * para esta F: RET_ADDR_BASE + slot. Esto permite call chain (F llama otra F)
         * sin sobreescribir ret_addr del caller.
         * Stack tras OP_CALL: [arg1, ..., argN, ret_addr]. Top = ret_addr.
         * Si F con params: STORE_VAR (RET_ADDR_BASE+slot) + STORE_VAR pN..p1.
         * Ret [val] o Ret [] luego LOAD val + LOAD (RET_ADDR_BASE+slot) + OP_RET. */
        bc_emit(OP_STORE_VAR);
        bc_emit_slot(RET_ADDR_BASE + slot);
        if(param_count > 0)
        {
            i = param_count - 1;
            while(i >= 0)
            {
                j = function_params[param_base + i];
                bc_emit(OP_STORE_VAR);
                bc_emit_slot(j);
                i = i - 1;
            }
        }
    }

    skip_to_eol(p, end);
    return 0;
}

void compile_777m_to_bytecode()
{
    int p;
    int c;

    g_bytecode_len = 0;
    p = 0;

    while(p < g_source_len)
    {
        /* Skip leading ws on line. */
        while(p < g_source_len)
        {
            c = sb(p);
            if(c == 32) { p = p + 1; }
            else
            {
                if(c == 9) { p = p + 1; }
                else
                {
                    if(c == 10) { p = p + 1; }
                    else
                    {
                        if(c == 13) { p = p + 1; }
                        else break;
                    }
                }
            }
        }
        if(p >= g_source_len) break;
        c = sb(p);
        if(c == 55)  /* '7' header line */
        {
            skip_to_eol(&p, g_source_len);
        }
        else
        {
            if(c == 70)  /* 'F' */
            {
                p = p + 1;  /* skip 'F' */
                compile_line_F(&p, g_source_len);
            }
            else
            {
                if(c == 67)  /* 'C' */
                {
                    skip_to_eol(&p, g_source_len);
                }
                else
                {
                    if(c == 68)  /* 'D' */
                    {
                        p = p + 1;  /* skip 'D' */
                        compile_line_D(&p, g_source_len);
                    }
                    else
                    {
                        if(c == 82)  /* 'R' */
                        {
                            p = p + 1;  /* skip 'R' */
                            compile_line_R(&p, g_source_len);
                        }
                        else
                        {
                            skip_to_eol(&p, g_source_len);
                        }
                    }
                }
            }
        }
    }
    /* Sub-G fallback EOF: si pending_fn_jmp_pos sigue abierto al EOF
     * (F sin EndF al final del source), patchear al bytecode_len actual.
     * Compat con tests que tienen F + Ret + EOF sin top-level R intermedio. */
    {
        int eof_jmp_post;
        int eof_jmp_rel;
        if(pending_fn_jmp_pos >= 0)
        {
            eof_jmp_post = pending_fn_jmp_pos + 2;
            eof_jmp_rel = g_bytecode_len - eof_jmp_post;
            g_bytecode[pending_fn_jmp_pos] = eof_jmp_rel & 0xFF;
            g_bytecode[pending_fn_jmp_pos + 1] = (eof_jmp_rel >> 8) & 0xFF;
            pending_fn_jmp_pos = 0 - 1;
        }
        pending_fn_slot = 0 - 1;
        /* Sub-C reciclaje slots locales al EOF. */
        if(fn_slot_alloc_start > 0)
        {
            sym_count = fn_slot_alloc_start;
            fn_slot_alloc_start = 0;
        }
    }
    /* Resolve function Call fixups (Item 2): patchear placeholders 0x0000
     * en bytecode emitido con rel16 = function_offsets[slot] - call_post_off. */
    func_resolve_fixups();
    /* Resolve label fixups (Sub-decision E): patchear Jmp/Jz placeholders. */
    label_resolve_fixups();
    bc_emit(OP_HALT);

    /* Post-process strings: append str_data al bytecode + patch fixups.
     * abs_addr = arch_base_vaddr() + HEADER_TOTAL + scaffold_size + bc_data_start + data_off. */
    {
        int bc_data_start;
        int scaffold;
        int bc_section_abs;
        int i;
        int abs_addr;
        int fixup_pos;
        int j;
        if(str_data_len > 0)
        {
            bc_data_start = g_bytecode_len;
            scaffold = calc_scaffold_size();
            bc_section_abs = arch_base_vaddr() + HEADER_TOTAL + scaffold;
            /* Patch cada fixup. */
            i = 0;
            while(i < fixup_count)
            {
                abs_addr = bc_section_abs + bc_data_start + fixup_data[i];
                fixup_pos = fixup_bc[i];
                /* Sobreescribe 8 bytes del placeholder con abs_addr LE. */
                g_bytecode[fixup_pos]     = abs_addr & 0xFF;
                g_bytecode[fixup_pos + 1] = (abs_addr >> 8) & 0xFF;
                g_bytecode[fixup_pos + 2] = (abs_addr >> 16) & 0xFF;
                g_bytecode[fixup_pos + 3] = (abs_addr >> 24) & 0xFF;
                g_bytecode[fixup_pos + 4] = (abs_addr >> 32) & 0xFF;
                g_bytecode[fixup_pos + 5] = (abs_addr >> 40) & 0xFF;
                g_bytecode[fixup_pos + 6] = (abs_addr >> 48) & 0xFF;
                g_bytecode[fixup_pos + 7] = (abs_addr >> 56) & 0xFF;
                i = i + 1;
            }
            /* Append str_data al bytecode. */
            j = 0;
            while(j < str_data_len)
            {
                g_bytecode[g_bytecode_len] = str_data[j] & 0xFF;
                g_bytecode_len = g_bytecode_len + 1;
                j = j + 1;
            }
        }
    }
}

/* === main === */
int main()
{
    int n;

    /* Init symbol table (pre-puebla slots 0-25 con A-Z). */
    sym_init();
    /* Init string intern + fixups. */
    str_init();
    /* Init functions table (Item 2: F/Call resolucion automatica). */
    func_init();
    /* Init labels table (Sub-decision E: Labels nombrados). */
    label_init();

    /* Lee stdin a g_source. */
    g_source_len = 0;
    while(1)
    {
        n = read(0, g_source + g_source_len, MAX_SOURCE - g_source_len);
        if(n <= 0) break;
        g_source_len = g_source_len + n;
        if(g_source_len >= MAX_SOURCE) break;
    }

    compile_777m_to_bytecode();
    build_vm_elf();

    /* Escribe ELF a stdout. */
    write(1, g_elf, g_elf_len);

    return 0;
}
