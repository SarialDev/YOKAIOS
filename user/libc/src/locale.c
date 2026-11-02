/* Doar localul "C" */
#include <locale.h>
#include <string.h>

static struct lconv c_lconv = {(char *)".", (char *)"", (char *)""};

char *setlocale(int category, const char *locale){
    (void)category;
    if(!locale || !*locale || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0){
        return (char *)"C";
    }
    return NULL;
}

struct lconv *localeconv(void){ return &c_lconv; }
