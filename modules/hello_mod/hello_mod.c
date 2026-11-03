/* Modul de kernel de proba (T8.5): ELF relocabil semnat, incarcat de core/kmod.cpp. Compilat cu -fPIC -fno-plt, deci
 * simbolurile kernelului se ating prin GOT (GOTPCRELX), iar tabelele de pointeri cer R_X86_64_64. Folosit de test=sign. */
typedef unsigned long size_t;

extern int kprintf(const char *fmt, ...);
extern void *kmalloc(size_t n);
extern void kfree(void *p);
extern void *memset(void *d, int c, size_t n);

const char yokai_module_name[] = "hello_mod";

static int add(int a, int b){
    return a + b;
}

static int mul(int a, int b){
    return a * b;
}

static int (*const ops[])(int, int) = {add, mul};              /* .data.rel.ro: R_X86_64_64 */
static const char *const names[] = {"alfa", "beta", "gama"};   /* idem, spre .rodata.str */
static int counter = 5;                                         /* .data */
static int zeros[64];                                           /* .bss */

int yokai_module_init(void){
    char *p = kmalloc(64);
    if(!p){
        return -12;
    }
    memset(p, 'x', 64);
    int ok = p[0] == 'x' && p[63] == 'x';
    kfree(p);
    for(int i = 0; i < 64; i++){
        if(zeros[i]){
            ok = 0;
        }
    }
    counter += 2;
    if(!ok || ops[0](counter, 3) != 10 || ops[1](counter, 3) != 21 || names[2][0] != 'g'){
        return -22;
    }
    kprintf("[OK] hello_mod: init (%s, counter=%d)\n", names[1], counter);
    return 0;
}

void yokai_module_exit(void){
    kprintf("[OK] hello_mod: exit\n");
}
